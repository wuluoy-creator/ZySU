#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#ifndef ESTALE
#define ESTALE 116
#endif

namespace query_fixture {
using pid_t = int;
using idtype_t = int;
using id_t = unsigned int;
struct siginfo_t {
    int si_pid = 0, si_code = 0, si_status = 0;
};
constexpr int CLD_EXITED = 1;
#ifndef WEXITED
constexpr int WEXITED = 4;
#endif
struct iovec {
    void* iov_base;
    std::size_t iov_len;
};
struct cmsghdr {
    std::size_t cmsg_len;
    int cmsg_level;
    int cmsg_type;
};
struct msghdr {
    iovec* msg_iov = nullptr;
    std::size_t msg_iovlen = 0;
    void* msg_control = nullptr;
    std::size_t msg_controllen = 0;
    int msg_flags = 0;
};
constexpr int AF_UNIX = 1, SOCK_SEQPACKET = 2, SOCK_CLOEXEC = 4;
constexpr int SOL_SOCKET = 5, SCM_RIGHTS = 6;
constexpr int MSG_NOSIGNAL = 8, MSG_DONTWAIT = 16, MSG_CMSG_CLOEXEC = 32;
constexpr int MSG_TRUNC = 64, MSG_CTRUNC = 128;
#ifndef WNOHANG
constexpr int WNOHANG = 1;
#endif
#ifndef SIGKILL
constexpr int SIGKILL = 9;
#endif
#ifndef CLOCK_MONOTONIC
constexpr int CLOCK_MONOTONIC = 1;
#endif
#define SYS_pidfd_open 1001
#define SYS_pidfd_send_signal 1002
#define CMSG_SPACE(n) (sizeof(cmsghdr) + (n))
#define CMSG_LEN(n) (sizeof(cmsghdr) + (n))
#define CMSG_FIRSTHDR(message)                                                                    \
    ((message)->msg_controllen >= sizeof(cmsghdr) ? static_cast<cmsghdr*>((message)->msg_control) \
                                                  : nullptr)
#define CMSG_NXTHDR(message, current) nullptr
#define CMSG_DATA(header) (reinterpret_cast<unsigned char*>(header) + sizeof(cmsghdr))
#ifdef WIFEXITED
#undef WIFEXITED
#undef WEXITSTATUS
#endif
#define WIFEXITED(status) (((status) & 255) == 0)
#define WEXITSTATUS(status) ((status) >> 8)

namespace query_model {
struct Wire {
    int kind, error, fd = -1, flags = 0, fd2 = -1;
};
struct ChildExit {
    int code;
};
inline std::deque<Wire> packets;
inline std::vector<Wire> sent;
inline std::vector<int> closed;
inline std::vector<int> killed;
inline std::int64_t now_ns;
inline int calls, forks, waits, sleeps, clock_calls, ack_count;
inline int fork_result, socket_error, fork_error, clock_error_at, sleep_error;
inline int wait_error, wait_interrupts, recv_interrupts, recv_error, send_error;
inline int kill_error, pidfd_error, namespace_error, child_ack, child_send_error;
inline bool blocked, dead, auto_done, initial_ready, reaped_early, unrelated_pid_reused;
inline int done_error, child_status;
inline std::function<void()> sleep_hook, wait_hook;

void reset() {
    packets.clear();
    sent.clear();
    closed.clear();
    killed.clear();
    now_ns = 0;
    calls = forks = waits = sleeps = clock_calls = ack_count = 0;
    fork_result = 123;
    socket_error = fork_error = clock_error_at = sleep_error = wait_error = wait_interrupts = 0;
    recv_interrupts = recv_error = send_error = kill_error = pidfd_error = namespace_error =
        child_send_error = 0;
    child_ack = 1;
    blocked = dead = reaped_early = unrelated_pid_reused = false;
    auto_done = initial_ready = true;
    done_error = child_status = 0;
    sleep_hook = {};
    wait_hook = {};
}
int getpid() {
    return 321;
}
long syscall(long number, ...) {
    va_list args;
    va_start(args, number);
    const int id = va_arg(args, int);
    va_end(args);
    if (number == SYS_pidfd_open) {
        assert(id == 321);  // The child never opens a fork-returned numeric PID.
        if (pidfd_error) {
            errno = pidfd_error;
            return -1;
        }
        return 81;
    }
    assert(number == SYS_pidfd_send_signal && id == 81);
    killed.push_back(id);
    if (kill_error) {
        errno = kill_error;
        return -1;
    }
    return 0;
}
int close(int fd) {
    assert(fd >= 0);
    closed.push_back(fd);
    errno = EBADF;
    return 0;
}
int socketpair(int, int, int, int* fds) {
    if (socket_error) {
        errno = socket_error;
        return -1;
    }
    fds[0] = 10;
    fds[1] = 11;
    return 0;
}
int fork() {
    ++forks;
    if (fork_error) {
        errno = fork_error;
        return -1;
    }
    if (fork_result > 0 && initial_ready)
        packets.push_back({1, 0, 81});
    return fork_result;
}
int clock_gettime(int, struct timespec* value) {
    ++clock_calls;
    if (clock_calls == clock_error_at) {
        errno = EIO;
        return -1;
    }
    now_ns += 1'000'000;
    value->tv_sec = static_cast<time_t>(now_ns / 1'000'000'000);
    value->tv_nsec = static_cast<long>(now_ns % 1'000'000'000);
    return 0;
}
int nanosleep(const struct timespec* delay, struct timespec*) {
    ++sleeps;
    assert(delay->tv_sec == 0 && delay->tv_nsec <= 10'000'000);
    now_ns += delay->tv_nsec;
    if (sleep_hook) {
        auto fn = std::move(sleep_hook);
        sleep_hook = {};
        fn();
    }
    if (sleep_error) {
        errno = sleep_error;
        return -1;
    }
    return 0;
}
int send(int fd, const void* data, std::size_t count, int flags) {
    assert(fd == 10 && count == 1 && *static_cast<const char*>(data) == 1);
    assert(flags & MSG_DONTWAIT);
    ++ack_count;
    if (send_error) {
        errno = send_error;
        return -1;
    }
    if (auto_done) {
        packets.push_back({2, done_error});
        dead = !blocked;
    }
    return 1;
}
int recvmsg(int, msghdr* message, int flags) {
    assert((flags & (MSG_DONTWAIT | MSG_CMSG_CLOEXEC)) == (MSG_DONTWAIT | MSG_CMSG_CLOEXEC));
    if (recv_interrupts) {
        --recv_interrupts;
        errno = EINTR;
        return -1;
    }
    if (recv_error) {
        errno = recv_error;
        return -1;
    }
    if (packets.empty()) {
        errno = EAGAIN;
        return -1;
    }
    const auto next = packets.front();
    packets.pop_front();
    const int bytes[2] = {next.kind, next.error};
    std::memcpy(message->msg_iov->iov_base, bytes, sizeof(bytes));
    message->msg_flags = next.flags;
    if (next.fd >= 0) {
        auto* rights = CMSG_FIRSTHDR(message);
        rights->cmsg_level = SOL_SOCKET;
        rights->cmsg_type = SCM_RIGHTS;
        rights->cmsg_len = CMSG_LEN(sizeof(int) * (next.fd2 >= 0 ? 2 : 1));
        std::memcpy(CMSG_DATA(rights), &next.fd, sizeof(int));
        if (next.fd2 >= 0)
            std::memcpy(CMSG_DATA(rights) + sizeof(int), &next.fd2, sizeof(int));
        message->msg_controllen = CMSG_SPACE(sizeof(int) * (next.fd2 >= 0 ? 2 : 1));
    } else
        message->msg_controllen = 0;
    return sizeof(bytes);
}
int waitid(idtype_t type, id_t fd, siginfo_t* status, int options) {
    assert(type == 3 && fd == 81 && options == (WEXITED | WNOHANG));
    ++waits;
    if (wait_hook) {
        auto fn = std::move(wait_hook);
        wait_hook = {};
        fn();
    }
    // The original child's handle must yield ECHILD, never the unrelated
    // replacement process's successful status under the same numeric PID.
    if (unrelated_pid_reused) {
        errno = ECHILD;
        return -1;
    }
    if (wait_interrupts) {
        --wait_interrupts;
        errno = EINTR;
        return -1;
    }
    if (wait_error) {
        errno = wait_error;
        return -1;
    }
    if (dead || reaped_early) {
        status->si_pid = 123;
        status->si_code = (child_status & 255) ? 2 : CLD_EXITED;
        status->si_status = child_status >> 8;
    }
    return 0;
}
int sendmsg(int, const msghdr* message, int) {
    if (child_send_error) {
        errno = child_send_error;
        return -1;
    }
    int bytes[2];
    std::memcpy(bytes, message->msg_iov->iov_base, sizeof(bytes));
    Wire packet{bytes[0], bytes[1]};
    if (message->msg_controllen)
        std::memcpy(&packet.fd, CMSG_DATA(static_cast<cmsghdr*>(message->msg_control)),
                    sizeof(int));
    sent.push_back(packet);
    return sizeof(bytes);
}
int recv(int, void* data, std::size_t, int) {
    *static_cast<char*>(data) = static_cast<char>(child_ack);
    return child_ack == -1 ? 0 : 1;
}
[[noreturn]] void _exit(int code) {
    throw ChildExit{code};
}
}  // namespace query_model
namespace ksud {
bool switch_mnt_ns(int pid) {
    assert(pid == 1);
    if (query_model::namespace_error) {
        errno = query_model::namespace_error;
        return false;
    }
    return true;
}
}  // namespace ksud

#include "query_worker_under_test.inc"
}  // namespace query_fixture

int main() {
    using namespace query_fixture;
    using namespace query_model;
    using namespace sumhp::mount::fsutil;
    int tests = 0;
    const auto fresh = [&] {
        release_worker();
        reset();
        ++tests;
    };
    const auto run = [&] { return run_query_in_init_mount_ns([] { return true; }); };
    fresh();
    assert(run() && errno == 0 && forks == 1 && ack_count == 1 && killed.empty());
    fresh();
    done_error = EACCES;
    child_status = 256;
    assert(!run() && errno == EACCES);
    fresh();
    child_status = SIGKILL;
    assert(!run() && errno == EIO);
    fresh();
    socket_error = EMFILE;
    assert(!run() && errno == EMFILE && forks == 0);
    fresh();
    fork_error = EAGAIN;
    assert(!run() && errno == EAGAIN && closed.size() == 2);
    fresh();
    clock_error_at = 1;
    assert(!run() && errno == EIO && forks == 0);
    fresh();
    now_ns = INT64_MAX - 2'000'000;
    assert(!run() && errno == EOVERFLOW && forks == 0);
    fresh();
    clock_error_at = 2;
    assert(!run() && errno == EIO && killed.empty());
    fresh();
    recv_interrupts = 3;
    assert(run() && sleeps >= 1);
    fresh();
    wait_interrupts = 3;
    assert(run() && waits == 4);
    fresh();
    blocked = true;
    auto_done = false;
    assert(!run() && errno == ETIMEDOUT && killed.size() == 1 && waits < 600);
    assert(pending_query.pid == 123 && pending_query.socket == -1);
    const int spawned = forks;
    assert(!run() && errno == EBUSY && forks == spawned);
    wait_interrupts = 1;
    assert(!run() && errno == EBUSY && forks == spawned);
    dead = true;
    blocked = false;
    auto_done = true;
    assert(run() && forks == spawned + 1);
    fresh();
    blocked = true;
    auto_done = false;
    sleep_error = EINTR;
    assert(!run() && errno == ETIMEDOUT && waits < 600);
    fresh();
    blocked = true;
    auto_done = false;
    wait_interrupts = 10000;
    assert(!run() && errno == ETIMEDOUT && waits < 600);
    fresh();
    blocked = true;
    auto_done = false;
    kill_error = EPERM;
    assert(!run() && errno == ETIMEDOUT && pending_query.pid == 123);
    assert(!run() && errno == EBUSY && forks == 1);
    fresh();
    initial_ready = false;
    blocked = true;
    auto_done = false;
    assert(!run() && errno == ETIMEDOUT && killed.empty() && ack_count == 0);
    assert(waits == 0);  // No numeric fallback before the stable handle arrives.
    packets.push_back({1, 0, 81});
    dead = true;
    blocked = false;
    initial_ready = auto_done = true;
    assert(run() && killed.size() == 1 && forks == 2 && ack_count == 1);
    fresh();
    wait_error = ECHILD;
    assert(!run() && errno == ECHILD && pending_query.pid == -1 && killed.empty());
    fresh();
    unrelated_pid_reused = true;
    assert(!run() && errno == ECHILD && pending_query.pid == -1 && killed.empty() && forks == 1);
    fresh();
    wait_error = EINVAL;
    assert(!run() && errno == EINVAL && killed.size() == 1 && pending_query.pid == 123);
    fresh();
    auto_done = false;
    blocked = true;
    sleep_error = EINVAL;
    assert(!run() && errno == EINVAL && killed.size() == 1);
    fresh();
    recv_error = EBADF;
    assert(!run() && errno == EBADF && ack_count == 0 && killed.empty());
    fresh();
    send_error = EPIPE;
    assert(!run() && errno == EPIPE && killed.size() == 1);
    fresh();
    initial_ready = false;
    packets.push_back({2, ENOSYS});
    dead = true;
    child_status = 256;
    assert(!run() && errno == ENOSYS && killed.empty());
    assert(!run() && errno == EBUSY && forks == 1 && waits == 0);
    fresh();
    initial_ready = false;
    packets.push_back({2, 0});
    assert(!run() && errno == EPROTO && killed.empty());
    fresh();
    initial_ready = false;
    packets.push_back({1, 0, 81, MSG_CTRUNC});
    assert(!run() && errno == EPROTO && ack_count == 0);
    fresh();
    initial_ready = false;
    packets.push_back({1, 0});
    assert(!run() && errno == EPROTO);
    fresh();
    initial_ready = false;
    packets.push_back({1, 0, 81, 0, 82});
    assert(!run() && errno == EPROTO && std::count(closed.begin(), closed.end(), 81) == 1 &&
           std::count(closed.begin(), closed.end(), 82) == 1 && killed.empty());
    fresh();
    initial_ready = false;
    packets.push_back({99, 0, 81});
    assert(!run() && errno == EPROTO && std::count(closed.begin(), closed.end(), 81) == 1 &&
           killed.empty());
    fresh();
    initial_ready = false;
    packets.push_back({1, 0, 81});
    packets.push_back({1, 0, 82});
    assert(!run() && errno == EPROTO && std::count(closed.begin(), closed.end(), 82) == 1 &&
           killed.size() == 1);
    fresh();
    auto_done = false;
    wait_hook = [] {
        packets.push_back({2, 0});
        dead = true;
    };
    assert(run());  // Done packet arrived between the read and successful waitid.
    fresh();
    auto_done = false;
    reaped_early = true;
    assert(!run() && errno == EIO && killed.empty());  // Exit 0 alone is insufficient.
    fresh();
    auto_done = false;
    blocked = true;
    sleep_hook = [&] {
        bool nested = true;
        int failure = 0;
        std::thread contender([&] {
            nested = run();
            failure = errno;
        });
        contender.join();
        assert(!nested && failure == EBUSY && forks == 1);
        packets.push_back({2, 0});
        dead = true;
    };
    assert(run());
    // Execute the actual production child branch with an _exit substitute.
    const auto child = [&](const std::function<bool()>& fn) {
        fork_result = 0;
        try {
            (void)run_query_in_init_mount_ns(fn);
            assert(false);
        } catch (const ChildExit& result) {
            return result.code;
        }
        return -1;
    };
    fresh();
    assert(child([] { return true; }) == 0 && sent.size() == 2 && sent[0].fd == 81 &&
           sent[1].error == 0);
    fresh();
    namespace_error = EPERM;
    bool called = false;
    assert(child([&] {
               called = true;
               return true;
           }) == 1 &&
           !called && sent.back().error == EPERM);
    fresh();
    assert(child([] {
               errno = ESTALE;
               return false;
           }) == 1 &&
           sent.back().error == ESTALE);
    fresh();
    assert(child([] { return false; }) == 1 && sent.back().error == EIO);
    fresh();
    assert(child([]() -> bool { throw std::runtime_error("callback"); }) == 1 &&
           sent.back().error == EIO);
    fresh();
    pidfd_error = ENOSYS;
    called = false;
    assert(child([&] {
               called = true;
               return true;
           }) == 1 &&
           !called && sent.size() == 1 && sent[0].error == ENOSYS);
    fresh();
    child_ack = -1;
    called = false;
    assert(child([&] {
               called = true;
               return true;
           }) == 1 &&
           !called && sent.size() == 1);
    fresh();
    child_send_error = EINTR;
    called = false;
    assert(child([&] {
               called = true;
               return true;
           }) == 1 &&
           !called);
    release_worker();
    std::cout << tests << " production query-worker syscall-model scenarios passed\n";
}
