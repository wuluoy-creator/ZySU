#include "mount/query_worker.hpp"
#include "utils.hpp"

#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>

namespace sumhp::mount::fsutil {
namespace {
constexpr std::int64_t query_budget_ns = 5'000'000'000LL;
constexpr std::int64_t poll_interval_ns = 10'000'000;
constexpr int ready_packet = 1;
constexpr int done_packet = 2;
struct Packet {
    int kind;
    int error;
};
struct Worker {
    pid_t pid = -1;
    int pidfd = -1;
    int socket = -1;
};
std::mutex query_mutex;
Worker pending_query;

int self_pidfd() {
#ifdef SYS_pidfd_open
    return static_cast<int>(::syscall(SYS_pidfd_open, ::getpid(), 0));
#else
    errno = ENOSYS;
    return -1;
#endif
}

void cancel_by_handle(int fd) {
#ifdef SYS_pidfd_send_signal
    if (fd >= 0)
        (void)::syscall(SYS_pidfd_send_signal, fd, SIGKILL, nullptr, 0);
#else
    (void)fd;
#endif
    // No numeric kill fallback: even a waitpid/kill pair races PID reuse when
    // another waiter or inherited SIGCHLD disposition reaps an exited child.
}

void release_worker() {
    if (pending_query.pidfd >= 0)
        ::close(pending_query.pidfd);
    if (pending_query.socket >= 0)
        ::close(pending_query.socket);
    pending_query = Worker{};
}

// Linux waitid's P_PIDFD selector is 3 (available since Linux 5.4). Keep the
// numeric ABI value for host libcs whose headers predate the named constant.
int reap_by_handle(siginfo_t& status) {
    if (pending_query.pidfd < 0)
        return 0;
    if (::waitid(static_cast<idtype_t>(3), static_cast<id_t>(pending_query.pidfd), &status,
                 WEXITED | WNOHANG) != 0)
        return -1;
    return status.si_pid ? 1 : 0;
}

bool monotonic_now(std::int64_t& value) {
    struct timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return false;
    if (now.tv_sec < 0 || now.tv_sec > INT64_MAX / 1'000'000'000LL || now.tv_nsec < 0 ||
        now.tv_nsec >= 1'000'000'000) {
        errno = EOVERFLOW;
        return false;
    }
    value = static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000LL;
    if (now.tv_nsec > INT64_MAX - value) {
        errno = EOVERFLOW;
        return false;
    }
    value += now.tv_nsec;
    return true;
}

bool send_packet(int socket, const Packet& packet, int fd) {
    struct iovec data{const_cast<Packet*>(&packet), sizeof(packet)};
    alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    struct msghdr message{};
    message.msg_iov = &data;
    message.msg_iovlen = 1;
    if (fd >= 0) {
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        auto* rights = CMSG_FIRSTHDR(&message);
        rights->cmsg_level = SOL_SOCKET;
        rights->cmsg_type = SCM_RIGHTS;
        rights->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(rights), &fd, sizeof(fd));
    }
    return ::sendmsg(socket, &message, MSG_NOSIGNAL) == sizeof(packet);
}

[[noreturn]] void query_child(int socket, const std::function<bool()>& fn) {
    // Open our OWN pidfd, then transfer it. Opening the fork-returned numeric
    // PID in the parent could already address a recycled, unrelated process.
    const int pidfd = self_pidfd();
    if (pidfd < 0) {
        const int failure = errno ? errno : EIO;
        (void)send_packet(socket, {done_packet, failure}, -1);
        ::_exit(1);
    }
    const bool handed_off = send_packet(socket, {ready_packet, 0}, pidfd);
    ::close(pidfd);
    if (!handed_off)
        ::_exit(1);
    // Do not start a callback before the parent owns the cancellation handle.
    // Closing the parent's socket on early timeout releases this wait with EOF.
    char ack = 0;
    if (::recv(socket, &ack, sizeof(ack), 0) != 1 || ack != 1)
        ::_exit(1);
    int failure = 0;
    try {
        errno = 0;
        if (!ksud::switch_mnt_ns(1))
            failure = errno ? errno : EIO;
        else {
            errno = 0;
            if (!fn())
                failure = errno ? errno : EIO;
        }
    } catch (...) {
        failure = EIO;
    }
    const bool sent = send_packet(socket, {done_packet, failure}, -1);
    ::close(socket);
    ::_exit(sent && !failure ? 0 : 1);
}

// Returns 1 for a packet, 0 for no data/EINTR, -1 for protocol/transport error.
// The child only sends two packets, so callers never drain an unbounded stream.
int receive_packet(Packet& packet, int& descriptor) {
    struct iovec data{&packet, sizeof(packet)};
    alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int) * 2)]{};
    struct msghdr message{};
    message.msg_iov = &data;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    descriptor = -1;
    const auto count = ::recvmsg(pending_query.socket, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (count < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    bool invalid = count != sizeof(packet) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
    for (auto* rights = CMSG_FIRSTHDR(&message); rights; rights = CMSG_NXTHDR(&message, rights)) {
        if (rights->cmsg_level != SOL_SOCKET || rights->cmsg_type != SCM_RIGHTS ||
            rights->cmsg_len < CMSG_LEN(0)) {
            invalid = true;
            continue;
        }
        const auto bytes = rights->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int))
            invalid = true;
        for (std::size_t at = 0; at + sizeof(int) <= bytes; at += sizeof(int)) {
            int fd;
            std::memcpy(&fd, CMSG_DATA(rights) + at, sizeof(fd));
            if (descriptor >= 0) {
                ::close(fd);
                invalid = true;
            } else
                descriptor = fd;
        }
    }
    if (invalid) {
        if (descriptor >= 0)
            ::close(descriptor);
        descriptor = -1;
        errno = EPROTO;
        return -1;
    }
    return 1;
}

bool abandon_query(int failure) {
    cancel_by_handle(pending_query.pidfd);
    // If the handle has not arrived, retain the socket so a later query can
    // receive it and cancel safely. No acknowledgement means no callback starts.
    if (pending_query.pidfd >= 0) {
        if (pending_query.socket >= 0)
            ::close(pending_query.socket);
        pending_query.socket = -1;
        siginfo_t status{};
        const auto waited = reap_by_handle(status);
        if (waited == 1 || (waited < 0 && errno == ECHILD))
            release_worker();
    }
    errno = failure;
    return false;
}

bool collect_pending() {
    if (pending_query.pidfd < 0) {
        if (pending_query.socket < 0) {
            errno = EBUSY;
            return false;
        }
        Packet packet{};
        int fd = -1;
        const int received = receive_packet(packet, fd);
        if (received == 1 && packet.kind == ready_packet && fd >= 0 && !packet.error) {
            pending_query.pidfd = fd;
            cancel_by_handle(fd);  // Never acknowledge a previously timed-out sample.
        } else {
            if (fd >= 0)
                ::close(fd);
            if (received != 0) {
                ::close(pending_query.socket);
                pending_query.socket = -1;
            }
            // No stable process handle: never wait on a potentially reused PID.
            // Quarantine this one slot until daemon exit if no handle can arrive.
            errno = EBUSY;
            return false;
        }
        ::close(pending_query.socket);
        pending_query.socket = -1;
    }
    siginfo_t status{};
    const int waited = reap_by_handle(status);
    if (waited == 1 || (waited < 0 && errno == ECHILD)) {
        release_worker();
        return true;
    }
    if (waited >= 0 || errno == EINTR)
        errno = EBUSY;
    return false;
}
}  // namespace

bool run_query_in_init_mount_ns(const std::function<bool()>& fn) {
    const std::unique_lock<std::mutex> lock(query_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        errno = EBUSY;
        return false;
    }
    if (pending_query.pid >= 0 && !collect_pending())
        return false;
    std::int64_t now = 0;
    if (!monotonic_now(now))
        return false;
    if (now > INT64_MAX - query_budget_ns) {
        errno = EOVERFLOW;
        return false;
    }
    const auto deadline = now + query_budget_ns;
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0)
        return false;
    const auto pid = ::fork();
    if (pid == 0) {
        ::close(sockets[0]);
        query_child(sockets[1], fn);
    }
    const int fork_error = errno;
    ::close(sockets[1]);
    if (pid < 0) {
        ::close(sockets[0]);
        errno = fork_error;
        return false;
    }
    pending_query = {pid, -1, sockets[0]};
    bool ready = false;
    bool done = false;
    int child_error = 0;
    for (;;) {
        const auto previous = now;
        if (!monotonic_now(now))
            return abandon_query(errno ? errno : EIO);
        if (now < previous)
            return abandon_query(EIO);
        if (now >= deadline)
            return abandon_query(ETIMEDOUT);
        for (int messages = 0; messages < 2 && !done; ++messages) {
            Packet packet{};
            int fd = -1;
            const int received = receive_packet(packet, fd);
            if (received < 0)
                return abandon_query(errno ? errno : EIO);
            if (!received)
                break;
            if (packet.kind == ready_packet && !ready && fd >= 0 && !packet.error) {
                pending_query.pidfd = fd;
                ready = true;
                const char ack = 1;
                if (::send(pending_query.socket, &ack, sizeof(ack), MSG_DONTWAIT | MSG_NOSIGNAL) !=
                    1)
                    return abandon_query(errno ? errno : EIO);
            } else if (packet.kind == done_packet && fd < 0 && packet.error >= 0 &&
                       packet.error <= 4095 && (ready || packet.error)) {
                done = true;
                child_error = packet.error;
                if (!ready)
                    return abandon_query(child_error);
            } else {
                if (fd >= 0)
                    ::close(fd);
                return abandon_query(EPROTO);
            }
        }
        siginfo_t status{};
        const auto waited = reap_by_handle(status);
        if (waited == 1) {
            // Data may have arrived between receive_packet and waitid. Drain
            // once before deciding, but never wait again for
            // a PID whose child has already been reaped.
            if (!done) {
                Packet packet{};
                int fd = -1;
                const int received = receive_packet(packet, fd);
                if (received == 1 && packet.kind == done_packet && fd < 0 && packet.error >= 0 &&
                    packet.error <= 4095 && (ready || packet.error)) {
                    done = true;
                    child_error = packet.error;
                }
                if (fd >= 0)
                    ::close(fd);
            }
            // siginfo_t is a POSIX ABI union; read the child status only for CLD_EXITED.
            // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
            const bool exited_cleanly = status.si_code == CLD_EXITED && status.si_status == 0;
            // NOLINTEND(cppcoreguidelines-pro-type-union-access)
            const bool success = done && ready && !child_error && exited_cleanly;
            release_worker();
            if (success)
                errno = 0;
            else
                errno = child_error ? child_error : EIO;
            return success;
        }
        if (waited < 0 && errno != EINTR) {
            const int failure = errno ? errno : EIO;
            if (failure == ECHILD) {
                release_worker();
                errno = failure;
                return false;
            }
            return abandon_query(failure);
        }
        const auto remaining = deadline - now;
        const struct timespec delay{
            0, static_cast<long>(remaining < poll_interval_ns ? remaining : poll_interval_ns)};
        if (::nanosleep(&delay, nullptr) != 0 && errno != EINTR)
            return abandon_query(errno ? errno : EIO);
    }
}
}  // namespace sumhp::mount::fsutil
