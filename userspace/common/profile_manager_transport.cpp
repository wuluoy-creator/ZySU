#include "profile_manager_transport.hpp"
#include "json.hpp"
#include "utf8.hpp"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>

namespace sumhp::profile_transport {
namespace {
constexpr std::uint32_t magic = 0x53504631;
constexpr std::uint32_t ready_kind = 1, ack_kind = 2, input_kind = 3, input_done_kind = 4,
                        output_kind = 5, output_done_kind = 6;
constexpr std::size_t chunk_limit = 4096, input_limit = (8UL * 1024 * 1024) + 4096,
                      output_limit = 24UL * 1024 * 1024;
constexpr char guard_name[] = "sumh.profile.manager.v1";
struct Header {
    std::uint32_t magic_value, kind, size, error;
};
struct Frame {
    Header header{};
    std::array<char, chunk_limit> body{};
};
struct Pending {
    bool active = false;
    int socket = -1, pidfd = -1;
};
std::mutex request_mutex;
Pending pending;

class OwnedFd {
public:
    explicit OwnedFd(int fd = -1) : fd_(fd) {}
    ~OwnedFd() {
        const int error = errno;
        if (fd_ >= 0)
            ::close(fd_);
        errno = error;
    }
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    OwnedFd(OwnedFd&&) = delete;
    OwnedFd& operator=(OwnedFd&&) = delete;
    [[nodiscard]] int get() const { return fd_; }

private:
    int fd_;
};

bool now_ns(std::int64_t& value) {
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now))
        return false;
    if (now.tv_sec < 0 || now.tv_sec > (INT64_MAX - 999999999LL) / 1000000000LL ||
        now.tv_nsec < 0 || now.tv_nsec >= 1000000000) {
        errno = EOVERFLOW;
        return false;
    }
    value = static_cast<std::int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
    return true;
}
bool deadline_after(unsigned milliseconds, std::int64_t& deadline) {
    if (!now_ns(deadline))
        return false;
    const auto budget = static_cast<std::int64_t>(milliseconds) * 1000000;
    if (deadline > INT64_MAX - budget) {
        errno = EOVERFLOW;
        return false;
    }
    deadline += budget;
    return true;
}
bool within_deadline(std::int64_t deadline, std::int64_t& previous) {
    std::int64_t now;
    if (!now_ns(now))
        return false;
    if (now < previous) {
        errno = EIO;
        return false;
    }
    previous = now;
    if (now >= deadline) {
        errno = ETIMEDOUT;
        return false;
    }
    return true;
}
// Sleep only for socket/pidfd readiness, never after successfully drained data.
bool wait_ready(int fd, short events, std::int64_t deadline) {
    std::int64_t previous = 0;
    for (;;) {
        if (!within_deadline(deadline, previous))
            return false;
        const auto remaining = (deadline - previous + 999999) / 1000000;
        pollfd item{fd, events, 0};
        const auto count =
            ::poll(&item, 1, static_cast<int>(std::min<std::int64_t>(remaining, 8000)));
        if (count > 0) {
            if (item.revents & POLLNVAL) {
                errno = EBADF;
                return false;
            }
            if (item.revents & (events | POLLHUP | POLLERR))
                return true;
        } else if (count < 0 && errno != EINTR)
            return false;
    }
}
int acquire_guard() {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path + 1, guard_name, sizeof(guard_name) - 1);
    const auto length =
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + sizeof(guard_name));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), length)) {
        const int error = errno == EADDRINUSE ? EBUSY : errno;
        ::close(fd);
        errno = error;
        return -1;
    }
    return fd;
}
bool valid_guard(int fd) {
    int type = 0;
    socklen_t type_size = sizeof(type);
    sockaddr_un address{};
    socklen_t length = sizeof(address);
    return ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_size) == 0 &&
           type_size == sizeof(type) && type == SOCK_STREAM &&
           ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0 &&
           length == static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + sizeof(guard_name)) &&
           address.sun_family == AF_UNIX && address.sun_path[0] == '\0' &&
           std::memcmp(address.sun_path + 1, guard_name, sizeof(guard_name) - 1) == 0 &&
           ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

// 1 frame, 0 would-block, -1 malformed/IO, -2 orderly EOF. Every received FD
// is either returned as the unique descriptor or closed, including truncation.
int receive(int socket, Frame& frame, int& descriptor) {
    iovec data{&frame, sizeof(frame)};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 2)]{};
    msghdr message{};
    message.msg_iov = &data;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    descriptor = -1;
    const auto count = ::recvmsg(socket, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (count < 0)
        return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    bool invalid = static_cast<std::size_t>(count) < sizeof(Header) ||
                   (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
    for (auto* rights = CMSG_FIRSTHDR(&message); rights; rights = CMSG_NXTHDR(&message, rights)) {
        if (rights->cmsg_level != SOL_SOCKET || rights->cmsg_type != SCM_RIGHTS ||
            rights->cmsg_len < CMSG_LEN(0)) {
            invalid = true;
            continue;
        }
        const auto bytes = rights->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int))
            invalid = true;
        for (std::size_t i = 0; i + sizeof(int) <= bytes; i += sizeof(int)) {
            int fd;
            std::memcpy(&fd, CMSG_DATA(rights) + i, sizeof(fd));
            if (descriptor >= 0) {
                ::close(fd);
                invalid = true;
            } else
                descriptor = fd;
        }
    }
    if (count == 0 && descriptor < 0 && message.msg_flags == 0)
        return -2;
    invalid = invalid || frame.header.magic_value != magic || frame.header.size > chunk_limit ||
              frame.header.error > 4095 ||
              static_cast<std::size_t>(count) != sizeof(Header) + frame.header.size;
    if (invalid) {
        if (descriptor >= 0)
            ::close(descriptor);
        descriptor = -1;
        errno = EPROTO;
        return -1;
    }
    return 1;
}
bool send_frame(int socket, std::uint32_t kind, const char* bytes, std::size_t size, int descriptor,
                int error, std::int64_t deadline) {
    if (size > chunk_limit || (size && !bytes)) {
        errno = EINVAL;
        return false;
    }
    Frame frame;
    frame.header = {magic, kind, static_cast<std::uint32_t>(size),
                    static_cast<std::uint32_t>(error)};
    if (size)
        std::memcpy(frame.body.data(), bytes, size);
    iovec data{&frame, sizeof(Header) + size};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr message{};
    message.msg_iov = &data;
    message.msg_iovlen = 1;
    if (descriptor >= 0) {
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        auto* rights = CMSG_FIRSTHDR(&message);
        rights->cmsg_level = SOL_SOCKET;
        rights->cmsg_type = SCM_RIGHTS;
        rights->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(rights), &descriptor, sizeof(descriptor));
    }
    std::int64_t previous = 0;
    for (;;) {
        if (!within_deadline(deadline, previous))
            return false;
        const auto count = ::sendmsg(socket, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (count >= 0 && static_cast<std::size_t>(count) == data.iov_len)
            return true;
        if (count >= 0) {
            errno = EPROTO;
            return false;
        }
        if (errno == EINTR)
            continue;
        if ((errno != EAGAIN && errno != EWOULDBLOCK) || !wait_ready(socket, POLLOUT, deadline))
            return false;
    }
}
int next_frame(int socket, Frame& frame, int& descriptor, std::int64_t deadline) {
    std::int64_t previous = 0;
    for (;;) {
        if (!within_deadline(deadline, previous))
            return -1;
        const int got = receive(socket, frame, descriptor);
        if (got != 0)
            return got;
        if (!wait_ready(socket, POLLIN, deadline))
            return -1;
    }
}
bool done_matches(const Frame& frame, std::size_t count) {
    std::uint64_t stated = 0;
    if (frame.header.size != sizeof(stated))
        return false;
    std::memcpy(&stated, frame.body.data(), sizeof(stated));
    return stated == count;
}
bool send_done(int socket, std::uint32_t kind, std::size_t count, int error,
               std::int64_t deadline) {
    const auto size = static_cast<std::uint64_t>(count);
    return send_frame(socket, kind, reinterpret_cast<const char*>(&size), sizeof(size), -1, error,
                      deadline);
}
void release_pending() {
    if (pending.socket >= 0)
        ::close(pending.socket);
    if (pending.pidfd >= 0)
        ::close(pending.pidfd);
    pending = {};
}
int reap(siginfo_t& status) {
    if (pending.pidfd < 0)
        return 0;
    if (::waitid(static_cast<idtype_t>(3), static_cast<id_t>(pending.pidfd), &status,
                 WEXITED | WNOHANG))
        return -1;
    return status.si_pid ? 1 : 0;
}
void abandon() {
    // Deliberately no signal: the complete input may already be committing.
    // Before READY retain the socket solely to collect a late self pidfd. It is
    // never ACKed. An unidentified child keeps this process slot quarantined.
    if (pending.pidfd < 0)
        return;
    if (pending.socket >= 0)
        ::close(pending.socket);
    pending.socket = -1;
    siginfo_t status{};
    const int waited = reap(status);
    if (waited == 1 || (waited < 0 && errno == ECHILD))
        release_pending();
}
bool collect_pending() {
    if (pending.pidfd < 0) {
        Frame frame;
        int fd = -1;
        const int got = pending.socket >= 0 ? receive(pending.socket, frame, fd) : -1;
        if (got == 1 && frame.header.kind == ready_kind && !frame.header.size &&
            !frame.header.error && fd >= 0)
            pending.pidfd = fd;
        else {
            if (fd >= 0)
                ::close(fd);
            if (got != 0 && pending.socket >= 0) {
                ::close(pending.socket);
                pending.socket = -1;
            }
            errno = EBUSY;
            return false;
        }
        ::close(pending.socket);
        pending.socket = -1;
    }
    siginfo_t status{};
    const int waited = reap(status);
    if (waited == 1 || (waited < 0 && errno == ECHILD)) {
        release_pending();
        return true;
    }
    errno = EBUSY;
    return false;
}
bool spawn(const std::string& executable, int guard) {
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets))
        return false;
    const OwnedFd original(sockets[1]);
    const OwnedFd channel(::fcntl(original.get(), F_DUPFD_CLOEXEC, 4));
    const OwnedFd guard_source(::fcntl(guard, F_DUPFD_CLOEXEC, 4));
    if (channel.get() < 0 || guard_source.get() < 0) {
        const int error = errno;
        ::close(sockets[0]);
        errno = error;
        return false;
    }
    posix_spawn_file_actions_t actions;
    int error = ::posix_spawn_file_actions_init(&actions);
    if (!error) {
        error = ::posix_spawn_file_actions_adddup2(&actions, channel.get(), STDIN_FILENO);
        if (!error)
            error = ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                                       O_WRONLY, 0);
        if (!error)
            error = ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                                       O_WRONLY, 0);
        if (!error)
            error = ::posix_spawn_file_actions_adddup2(&actions, guard_source.get(), 3);
        if (!error)
            error = ::posix_spawn_file_actions_addclose(&actions, channel.get());
        if (!error)
            error = ::posix_spawn_file_actions_addclose(&actions, guard_source.get());
        std::array<std::string, 5> args{executable, "sumhp", "profile", "manager", "--channel"};
        std::array<char*, 6> argv{};
        for (std::size_t i = 0; i < args.size(); ++i)
            argv[i] = args[i].data();
        pid_t unused_pid = -1;
        if (!error)
            error = ::posix_spawn(&unused_pid, executable.c_str(), &actions, nullptr, argv.data(),
                                  environ);
        ::posix_spawn_file_actions_destroy(&actions);
    }
    if (error) {
        ::close(sockets[0]);
        errno = error;
        return false;
    }
    pending = {true, sockets[0], -1};
    return true;
}
bool private_regular(const struct stat& value) {
    return S_ISREG(value.st_mode) && value.st_nlink == 1 && (value.st_mode & 0077) == 0;
}
bool snapshot(int input, int output, std::string& bytes, std::int64_t deadline) {
    struct stat before{};
    struct stat destination{};
    if (::fstat(input, &before) || ::fstat(output, &destination))
        return false;
    const int in_flags = ::fcntl(input, F_GETFL);
    const int out_flags = ::fcntl(output, F_GETFL);
    if (in_flags < 0 || out_flags < 0)
        return false;
    if (!private_regular(before) || !private_regular(destination) ||
        before.st_uid != destination.st_uid ||
        (before.st_dev == destination.st_dev && before.st_ino == destination.st_ino) ||
        before.st_size <= 0 || static_cast<std::uint64_t>(before.st_size) > input_limit ||
        destination.st_size != 0 || (in_flags & O_ACCMODE) == O_WRONLY ||
        (out_flags & O_ACCMODE) == O_RDONLY || (out_flags & O_APPEND)) {
        errno = EINVAL;
        return false;
    }
    bytes.resize(static_cast<std::size_t>(before.st_size));
    std::int64_t previous = 0;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (!within_deadline(deadline, previous))
            return false;
        const auto got = ::pread(input, bytes.data() + offset,
                                 std::min<std::size_t>(65536, bytes.size() - offset),
                                 static_cast<off_t>(offset));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            if (!got)
                errno = EIO;
            return false;
        }
        offset += static_cast<std::size_t>(got);
    }
    struct stat after{};
    if (::fstat(input, &after))
        return false;
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size || before.st_mode != after.st_mode ||
        before.st_uid != after.st_uid || before.st_nlink != after.st_nlink ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
        errno = ESTALE;
        return false;
    }
    return true;
}
bool write_chunk(int output, const char* bytes, std::size_t count, std::size_t offset,
                 std::int64_t deadline) {
    std::int64_t previous = 0;
    while (count) {
        if (!within_deadline(deadline, previous))
            return false;
        const auto wrote = ::pwrite(output, bytes, count, static_cast<off_t>(offset));
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0) {
            if (!wrote)
                errno = EIO;
            return false;
        }
        bytes += wrote;
        count -= static_cast<std::size_t>(wrote);
        offset += static_cast<std::size_t>(wrote);
    }
    return true;
}
std::string receipt(int error, std::size_t bytes = 0) {
    auto out = json::Value::object();
    out["schema_version"] = 1;
    out["kind"] = "sumh.profile.transfer";
    out["complete"] = error == 0;
    out["errno"] = error;
    out["bytes"] = static_cast<int>(bytes);
    return json::dump(out);
}
}  // namespace

std::string failure_receipt(int error) {
    return receipt(error > 0 && error <= 4095 ? error : EIO);
}

std::string request(const std::string& executable, int input_fd, int output_fd,
                    unsigned timeout_ms) {
    const std::unique_lock<std::mutex> lock(request_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return failure_receipt(EBUSY);
    if (executable.empty() || executable.front() != '/' || executable.size() > 4096 ||
        executable.find('\0') != std::string::npos || !ksud::text::valid_utf8(executable) ||
        !timeout_ms || timeout_ms > 8000 || input_fd < 0 || output_fd < 0)
        return failure_receipt(EINVAL);
    if (pending.active && !collect_pending())
        return failure_receipt(EBUSY);
    std::int64_t deadline;
    if (!deadline_after(timeout_ms, deadline))
        return failure_receipt(errno);
    std::string input;
    if (!snapshot(input_fd, output_fd, input, deadline))
        return failure_receipt(errno);
    const OwnedFd guard(acquire_guard());
    if (guard.get() < 0)
        return failure_receipt(errno);
    if (!spawn(executable, guard.get()))
        return failure_receipt(errno);
    auto fail = [&](int error) {
        abandon();
        (void)::ftruncate(output_fd, 0);
        return failure_receipt(error);
    };
    Frame frame;
    int fd = -1;
    int got = next_frame(pending.socket, frame, fd, deadline);
    if (got != 1 || frame.header.kind != ready_kind || frame.header.size || frame.header.error ||
        fd < 0) {
        if (fd >= 0)
            ::close(fd);
        return fail(got == -1 ? errno : EPROTO);
    }
    pending.pidfd = fd;
    siginfo_t status{};
    // Prove waitid(P_PIDFD) support before allowing even a read-only callback.
    const int initial_wait = reap(status);
    if (initial_wait != 0)
        return fail(initial_wait < 0 ? errno : ECHILD);
    if (!send_frame(pending.socket, ack_kind, nullptr, 0, -1, 0, deadline))
        return fail(errno);
    for (std::size_t offset = 0; offset < input.size(); offset += chunk_limit) {
        if (!send_frame(pending.socket, input_kind, input.data() + offset,
                        std::min(chunk_limit, input.size() - offset), -1, 0, deadline))
            return fail(errno);
    }
    if (!send_done(pending.socket, input_done_kind, input.size(), 0, deadline) ||
        ::shutdown(pending.socket, SHUT_WR))
        return fail(errno);
    std::size_t written = 0;
    bool done = false;
    for (;;) {
        fd = -1;
        got = next_frame(pending.socket, frame, fd, deadline);
        if (fd >= 0) {
            ::close(fd);
            return fail(EPROTO);
        }
        if (got == -2) {
            if (!done)
                return fail(EPROTO);
            break;
        }
        if (got < 0)
            return fail(errno);
        if (done)
            return fail(EPROTO);
        if (frame.header.kind == output_kind && frame.header.size && !frame.header.error) {
            if (frame.header.size > output_limit - written)
                return fail(EOVERFLOW);
            if (!write_chunk(output_fd, frame.body.data(), frame.header.size, written, deadline))
                return fail(errno);
            written += frame.header.size;
        } else if (frame.header.kind == output_done_kind && done_matches(frame, written)) {
            if (frame.header.error)
                return fail(static_cast<int>(frame.header.error));
            if (!written)
                return fail(EPROTO);
            done = true;
        } else
            return fail(EPROTO);
    }
    for (;;) {
        status = {};
        const int waited = reap(status);
        if (waited < 0)
            return fail(errno);
        if (waited == 1)
            break;
        if (!wait_ready(pending.pidfd, POLLIN, deadline))
            return fail(errno);
    }
    // siginfo_t exposes the POSIX exit status through a platform union.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
    const bool successful_exit = status.si_code == CLD_EXITED && status.si_status == 0;
    release_pending();
    if (!successful_exit)
        return fail(EIO);
    struct stat completed{};
    if (::fsync(output_fd) || ::fstat(output_fd, &completed))
        return fail(errno);
    if (!private_regular(completed) || completed.st_size != static_cast<off_t>(written))
        return fail(EPROTO);
    return receipt(0, written);
}

int serve(const std::function<std::string(const std::string&)>& callback) {
    // Parent and child hold the same reservation. Parent loss never releases
    // the cross-process guard while a writer is still inside the callback.
    const OwnedFd guard(3);
    if (!valid_guard(guard.get()))
        return 1;
    constexpr int socket = STDIN_FILENO;
    int type = 0;
    socklen_t length = sizeof(type);
    if (::getsockopt(socket, SOL_SOCKET, SO_TYPE, &type, &length) || type != SOCK_SEQPACKET ||
        length != sizeof(type) || ::fcntl(socket, F_SETFD, FD_CLOEXEC) < 0)
        return 1;
    std::int64_t deadline;
    if (!deadline_after(8000, deadline))
        return 1;
    int self = -1;
#ifdef SYS_pidfd_open
    self = static_cast<int>(::syscall(SYS_pidfd_open, ::getpid(), 0));
#else
    errno = ENOSYS;
#endif
    if (self < 0)
        return 1;
    const OwnedFd pidfd(self);
    if (!send_frame(socket, ready_kind, nullptr, 0, pidfd.get(), 0, deadline))
        return 1;
    Frame frame;
    int fd = -1;
    int got = next_frame(socket, frame, fd, deadline);
    if (fd >= 0)
        ::close(fd);
    if (got != 1 || fd >= 0 || frame.header.kind != ack_kind || frame.header.size ||
        frame.header.error)
        return 1;
    std::string input;
    bool done = false;
    for (;;) {
        fd = -1;
        got = next_frame(socket, frame, fd, deadline);
        if (fd >= 0) {
            ::close(fd);
            return 1;
        }
        if (got == -2) {
            if (!done)
                return 1;
            break;
        }
        if (got < 0 || done || frame.header.error)
            return 1;
        if (frame.header.kind == input_kind && frame.header.size) {
            if (frame.header.size > input_limit - input.size())
                return 1;
            input.append(frame.body.data(), frame.header.size);
        } else if (frame.header.kind == input_done_kind && !input.empty() &&
                   done_matches(frame, input.size()))
            done = true;
        else
            return 1;
    }
    // No timeout cancellation once dispatched. Lost output is unknown; the
    // Manager's durable checkpoint prevents replay and enables observation.
    std::string output;
    int error = 0;
    try {
        output = callback(input);
        json::Value value;
        std::string reason;
        if (output.empty() || output.size() > output_limit)
            error = EOVERFLOW;
        else if (!ksud::text::valid_utf8(output) ||
                 !json::Parser(output, true).parse(value, reason) || !value.is_object())
            error = EPROTO;
    } catch (...) {
        error = EIO;
    }
    if (!deadline_after(8000, deadline))
        return 1;
    std::size_t sent = 0;
    if (!error) {
        while (sent < output.size()) {
            const auto count = std::min(chunk_limit, output.size() - sent);
            if (!send_frame(socket, output_kind, output.data() + sent, count, -1, 0, deadline))
                return 1;
            sent += count;
        }
    }
    return send_done(socket, output_done_kind, sent, error, deadline) && !error ? 0 : 1;
}
}  // namespace sumhp::profile_transport
