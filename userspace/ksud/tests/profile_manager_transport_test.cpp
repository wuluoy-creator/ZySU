#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "userspace/common/json.hpp"
#include "userspace/common/utf8.hpp"

// Portable syscall fault model. Production body is extracted by the runner;
// Linux ABI names here are isolated from platform SDK definitions.
namespace fixture {
using pid_t = int;
using idtype_t = int;
using id_t = unsigned;
using socklen_t = unsigned;
using off_t = std::int64_t;
struct siginfo_t {
    int si_pid = 0, si_code = 0, si_status = 0;
};
struct iovec {
    void* iov_base;
    std::size_t iov_len;
};
struct cmsghdr {
    std::size_t cmsg_len;
    int cmsg_level, cmsg_type;
};
struct msghdr {
    iovec* msg_iov = nullptr;
    std::size_t msg_iovlen = 0;
    void* msg_control = nullptr;
    std::size_t msg_controllen = 0;
    int msg_flags = 0;
};
struct sockaddr {};
struct sockaddr_un {
    int sun_family;
    char sun_path[108];
};
struct posix_spawn_file_actions_t {};
struct stat {
    unsigned st_mode = 0100600, st_nlink = 1, st_uid = 123, st_dev = 1, st_ino = 1;
    off_t st_size = 0;
    timespec st_mtim{}, st_ctim{};
};
struct pollfd {
    int fd;
    short events, revents;
};
constexpr int AF_UNIX = 1, SOCK_SEQPACKET = 2, SOCK_CLOEXEC = 4, SOCK_STREAM = 8;
constexpr int SOL_SOCKET = 5, SCM_RIGHTS = 6, SO_TYPE = 7;
constexpr int MSG_NOSIGNAL = 8, MSG_DONTWAIT = 16, MSG_CMSG_CLOEXEC = 32, MSG_TRUNC = 64,
              MSG_CTRUNC = 128;
#ifndef STDIN_FILENO
constexpr int STDIN_FILENO = 0, STDOUT_FILENO = 1, STDERR_FILENO = 2;
#endif
constexpr int F_DUPFD_CLOEXEC = 91, F_SETFD = 92, FD_CLOEXEC = 93, F_GETFL = 94;
constexpr int O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2, O_ACCMODE = 3, O_APPEND = 4;
constexpr int POLLIN = 1, POLLOUT = 2, POLLHUP = 4, POLLERR = 8, POLLNVAL = 16, SHUT_WR = 1;
constexpr int CLD_EXITED = 1;
#ifndef CLOCK_MONOTONIC
constexpr int CLOCK_MONOTONIC = 1;
#endif
#ifndef WEXITED
constexpr int WEXITED = 4;
#endif
#ifndef WNOHANG
constexpr int WNOHANG = 1;
#endif
#ifndef ESTALE
constexpr int ESTALE = 116;
#endif
#define SYS_pidfd_open 1001
#define S_ISREG(mode) (((mode) & 0170000) == 0100000)
#define CMSG_SPACE(n) (sizeof(cmsghdr) + (n))
#define CMSG_LEN(n) (sizeof(cmsghdr) + (n))
#define CMSG_FIRSTHDR(m) \
    ((m)->msg_controllen >= sizeof(cmsghdr) ? static_cast<cmsghdr*>((m)->msg_control) : nullptr)
#define CMSG_NXTHDR(m, c) nullptr
#define CMSG_DATA(h) (reinterpret_cast<unsigned char*>(h) + sizeof(cmsghdr))
#ifdef environ
#undef environ
#endif
char** environ = nullptr;

namespace transport_model {
int assertions = 0, scenarios = 0;
void check_impl(bool ok, int line) {
    ++assertions;
    if (!ok) {
        std::cerr << "failure line " << line << '\n';
        std::abort();
    }
}
#define check(value) check_impl((value), __LINE__)
struct Wire {
    unsigned kind = 0;
    std::string body;
    unsigned error = 0;
    int fd = -1, second_fd = -1;
    int flags = 0;
    bool short_frame = false;
    unsigned magic = 0x53504631;
};
struct Descriptor {
    int tag = 0;
    bool cloexec = true;
};
struct Action {
    char kind;
    int from, to;
};
std::deque<Wire> incoming;
std::vector<Wire> replies, sent;
std::vector<int> closed;
std::map<int, Descriptor> descriptors;
std::vector<Action> actions;
std::map<int, struct stat> stats;
std::map<int, std::string> files;
std::map<int, int> modes;
std::int64_t time_ns = 0;
int spawn_count, ack_count, poll_count, fstat_calls, syscalls, waits;
int spawn_error, action_fail, action_calls, duplicate_fail, duplicate_calls, recv_error, send_error,
    send_blocks;
int wait_error, pidfd_error, fsync_error, write_error, child_status, socket_error, bind_error;
int partial_read, partial_write, clock_error, clock_calls, stat_error;
bool child_mode, ready, no_reply, live, dead, child_guard, guard_bound, auto_eof, snapshot_change,
    invalid_guard;
std::string received_input;
std::function<void()> poll_hook;
int allocate(Descriptor descriptor, int minimum = 0) {
    while (descriptors.count(minimum))
        ++minimum;
    descriptors[minimum] = descriptor;
    return minimum;
}
Wire done(unsigned kind, std::size_t size, unsigned error = 0) {
    std::uint64_t value = size;
    return {kind, std::string(reinterpret_cast<const char*>(&value), sizeof(value)), error};
}
void response(const std::string& bytes) {
    replies.clear();
    for (std::size_t i = 0; i < bytes.size(); i += 4096)
        replies.push_back({5, bytes.substr(i, 4096)});
    replies.push_back(done(6, bytes.size()));
}
void reset() {
    ++scenarios;
    incoming.clear();
    replies.clear();
    sent.clear();
    closed.clear();
    descriptors.clear();
    actions.clear();
    stats.clear();
    files.clear();
    modes.clear();
    received_input.clear();
    time_ns = 0;
    poll_hook = {};
    spawn_count = ack_count = poll_count = fstat_calls = syscalls = waits = 0;
    spawn_error = action_fail = action_calls = duplicate_fail = duplicate_calls = recv_error =
        send_error = send_blocks = 0;
    wait_error = pidfd_error = fsync_error = write_error = child_status = socket_error =
        bind_error = 0;
    partial_read = partial_write = clock_error = clock_calls = stat_error = 0;
    child_mode = no_reply = live = dead = child_guard = guard_bound = snapshot_change =
        invalid_guard = false;
    ready = auto_eof = true;
    for (int i : {0, 1, 2})
        descriptors[i] = {0, true};
    descriptors[50] = {5, true};
    descriptors[51] = {6, true};
    files[50] = "{\"schema_version\":1,\"action\":\"list\"}";
    files[51] = "";
    stats[50] = {};
    stats[50].st_size = static_cast<off_t>(files[50].size());
    stats[50].st_ino = 50;
    stats[51] = {};
    stats[51].st_ino = 51;
    modes[50] = O_RDONLY;
    modes[51] = O_RDWR;
    response("{\"schema_version\":1,\"kind\":\"sumh.profile.result\",\"unicode\":\"方案\"}");
}
int clock_gettime(int clock, timespec* out) {
    check(clock == CLOCK_MONOTONIC);
    if (++clock_calls == clock_error) {
        errno = EIO;
        return -1;
    }
    time_ns += 1000;
    out->tv_sec = static_cast<time_t>(time_ns / 1000000000);
    out->tv_nsec = static_cast<long>(time_ns % 1000000000);
    return 0;
}
int poll(pollfd* items, unsigned count, int timeout) {
    ++poll_count;
    check(count == 1 && timeout > 0 && timeout <= 8000);
    if (poll_hook) {
        auto hook = std::move(poll_hook);
        poll_hook = {};
        hook();
    }
    if ((!incoming.empty() || (!no_reply && auto_eof)) || items[0].events == POLLOUT || dead) {
        time_ns += 1000000;
        items[0].revents = items[0].events;
        return 1;
    }
    time_ns += static_cast<std::int64_t>(timeout) * 1000000;
    return 0;
}
int close(int fd) {
    check(fd >= 0);
    closed.push_back(fd);
    descriptors.erase(fd);
    bool owns = child_guard;
    for (const auto& entry : descriptors)
        owns |= entry.second.tag == 3;
    guard_bound = owns;
    return 0;
}
int socket(int domain, int type, int protocol) {
    check(domain == AF_UNIX && type == (SOCK_STREAM | SOCK_CLOEXEC) && protocol == 0);
    if (socket_error) {
        errno = socket_error;
        return -1;
    }
    return allocate({3, true});
}
int bind(int fd, const sockaddr* value, socklen_t length) {
    check(descriptors.at(fd).tag == 3);
    const auto* address = reinterpret_cast<const sockaddr_un*>(value);
    check(address->sun_family == AF_UNIX && address->sun_path[0] == 0 &&
          std::string(address->sun_path + 1) == "sumh.profile.manager.v1");
    check(length == offsetof(sockaddr_un, sun_path) + sizeof("sumh.profile.manager.v1"));
    if (bind_error || guard_bound) {
        errno = bind_error ? bind_error : EADDRINUSE;
        return -1;
    }
    guard_bound = true;
    return 0;
}
int socketpair(int domain, int type, int protocol, int* pair) {
    check(domain == AF_UNIX && type == (SOCK_SEQPACKET | SOCK_CLOEXEC) && protocol == 0);
    if (socket_error) {
        errno = socket_error;
        return -1;
    }
    pair[0] = allocate({1, true});
    pair[1] = allocate({2, true});
    return 0;
}
int fcntl(int fd, int op, int arg = 0) {
    if (op == F_GETFL)
        return modes.at(fd);
    if (op == F_SETFD) {
        check(child_mode && (fd == 0 || fd == 3) && arg == FD_CLOEXEC);
        return 0;
    }
    check(op == F_DUPFD_CLOEXEC && arg == 4 && descriptors.count(fd));
    if (++duplicate_calls == duplicate_fail) {
        errno = EMFILE;
        return -1;
    }
    return allocate({descriptors.at(fd).tag, true}, arg);
}
int action_result() {
    return ++action_calls == action_fail ? ENOMEM : 0;
}
int posix_spawn_file_actions_init(posix_spawn_file_actions_t*) {
    actions.clear();
    return action_result();
}
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t*, int from, int to) {
    check(from >= 4 && (to == 0 || to == 3));
    actions.push_back({'d', from, to});
    return action_result();
}
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t*, int target, const char* path,
                                     int mode, int) {
    check((target == 1 || target == 2) && std::string(path) == "/dev/null" && mode == O_WRONLY);
    actions.push_back({'o', -1, target});
    return action_result();
}
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t*, int fd) {
    check(fd >= 4);
    actions.push_back({'c', fd, -1});
    return action_result();
}
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t*) {
    return 0;
}
int posix_spawn(pid_t* pid, const char* path, posix_spawn_file_actions_t*, void*, char* const* argv,
                char**) {
    ++spawn_count;
    check(std::string(path) == "/ksud");
    const std::vector<std::string> expected{"/ksud", "sumhp", "profile", "manager", "--channel"};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        check(argv[i] && expected[i] == argv[i]);
    }
    check(argv[5] == nullptr);
    auto child = descriptors;
    for (const auto& action : actions) {
        if (action.kind == 'd')
            child[action.to] = {child.at(action.from).tag, false};
        else if (action.kind == 'o')
            child[action.to] = {0, false};
        else
            child.erase(action.from);
    }
    for (auto it = child.begin(); it != child.end();) {
        if (it->second.cloexec)
            it = child.erase(it);
        else
            ++it;
    }
    check(child.size() == 4 && child.at(0).tag == 2 && child.at(3).tag == 3 &&
          child.at(1).tag == 0 && child.at(2).tag == 0);
    if (spawn_error)
        return spawn_error;
    *pid = 444;
    child_guard = true;
    dead = false;
    if (ready)
        incoming.push_back({1, "", 0, 90});
    return 0;
}
std::int64_t recvmsg(int, msghdr* message, int flags) {
    check(flags == (MSG_DONTWAIT | MSG_CMSG_CLOEXEC));
    if (recv_error) {
        errno = recv_error;
        return -1;
    }
    if (incoming.empty()) {
        message->msg_controllen = 0;
        message->msg_flags = 0;
        if (auto_eof && !no_reply) {
            if (!child_mode && !live) {
                dead = true;
                child_guard = false;
            }
            return 0;
        }
        errno = EAGAIN;
        return -1;
    }
    const auto wire = incoming.front();
    incoming.pop_front();
    unsigned header[4] = {wire.magic, wire.kind, static_cast<unsigned>(wire.body.size()),
                          wire.error};
    check(message->msg_iovlen == 1 &&
          sizeof(header) + wire.body.size() <= message->msg_iov[0].iov_len);
    std::memcpy(message->msg_iov[0].iov_base, header, sizeof(header));
    std::memcpy(static_cast<char*>(message->msg_iov[0].iov_base) + sizeof(header), wire.body.data(),
                wire.body.size());
    message->msg_flags = wire.flags;
    if (wire.fd >= 0) {
        const int fds[] = {wire.fd, wire.second_fd};
        const std::size_t count = wire.second_fd >= 0 ? 2 : 1;
        auto* rights = static_cast<cmsghdr*>(message->msg_control);
        rights->cmsg_level = SOL_SOCKET;
        rights->cmsg_type = SCM_RIGHTS;
        rights->cmsg_len = CMSG_LEN(count * sizeof(int));
        std::memcpy(CMSG_DATA(rights), fds, count * sizeof(int));
        message->msg_controllen = CMSG_SPACE(count * sizeof(int));
    } else
        message->msg_controllen = 0;
    return static_cast<std::int64_t>(wire.short_frame ? 3 : sizeof(header) + wire.body.size());
}
std::int64_t sendmsg(int, const msghdr* message, int flags) {
    check(flags == (MSG_DONTWAIT | MSG_NOSIGNAL));
    if (send_error || send_blocks) {
        errno = send_error ? send_error : EAGAIN;
        if (send_blocks)
            --send_blocks;
        return -1;
    }
    const auto* header = static_cast<const unsigned*>(message->msg_iov[0].iov_base);
    check(header[0] == 0x53504631 && header[2] <= 4096 &&
          message->msg_iov[0].iov_len == 16 + header[2]);
    Wire wire;
    wire.kind = header[1];
    wire.error = header[3];
    wire.body.assign(static_cast<const char*>(message->msg_iov[0].iov_base) + 16, header[2]);
    if (message->msg_controllen)
        std::memcpy(&wire.fd, CMSG_DATA(static_cast<cmsghdr*>(message->msg_control)), sizeof(int));
    sent.push_back(wire);
    if (wire.kind == 2)
        ++ack_count;
    if (wire.kind == 3)
        received_input += wire.body;
    return static_cast<std::int64_t>(message->msg_iov[0].iov_len);
}
int shutdown(int, int how) {
    check(how == SHUT_WR && !child_mode && ack_count > 0);
    if (!no_reply)
        for (const auto& reply : replies)
            incoming.push_back(reply);
    return 0;
}
int waitid(idtype_t kind, id_t fd, siginfo_t* status, int options) {
    ++waits;
    check(kind == 3 && fd == 90 && options == (WEXITED | WNOHANG));
    if (wait_error) {
        errno = wait_error;
        return -1;
    }
    if (dead) {
        *status = {444, CLD_EXITED, child_status};
        child_guard = false;
    }
    return 0;
}
int fstat(int fd, struct stat* out) {
    ++fstat_calls;
    if (stat_error) {
        errno = EIO;
        return -1;
    }
    *out = stats.at(fd);
    out->st_size = static_cast<off_t>(files.at(fd).size());
    if (snapshot_change && fd == 50 && fstat_calls > 2)
        ++out->st_mtim.tv_nsec;
    return 0;
}
std::int64_t pread(int fd, void* buffer, std::size_t count, off_t offset) {
    check(fd == 50 && offset >= 0 && count <= 65536);
    count = std::min(count, files.at(fd).size() - static_cast<std::size_t>(offset));
    if (partial_read)
        count = std::min(count, static_cast<std::size_t>(partial_read));
    std::memcpy(buffer, files.at(fd).data() + offset, count);
    return static_cast<std::int64_t>(count);
}
std::int64_t pwrite(int fd, const void* buffer, std::size_t count, off_t offset) {
    check(fd == 51 && offset >= 0);
    if (write_error) {
        errno = write_error;
        return -1;
    }
    if (partial_write)
        count = std::min(count, static_cast<std::size_t>(partial_write));
    auto& bytes = files[fd];
    const auto end = static_cast<std::size_t>(offset) + count;
    if (end > bytes.size())
        bytes.resize(end);
    bytes.replace(static_cast<std::size_t>(offset), count, static_cast<const char*>(buffer), count);
    return static_cast<std::int64_t>(count);
}
int ftruncate(int fd, off_t size) {
    check(fd == 51 && size == 0);
    files[fd].clear();
    return 0;
}
int fsync(int fd) {
    check(fd == 51);
    if (fsync_error) {
        errno = fsync_error;
        return -1;
    }
    return 0;
}
int getsockopt(int fd, int level, int option, void* out, socklen_t* length) {
    check(child_mode && (fd == 0 || fd == 3) && level == SOL_SOCKET && option == SO_TYPE &&
          *length == sizeof(int));
    *static_cast<int*>(out) = fd == 0 ? SOCK_SEQPACKET : SOCK_STREAM;
    return 0;
}
int getsockname(int fd, sockaddr* out, socklen_t* length) {
    check(fd == 3 && child_mode);
    auto* address = reinterpret_cast<sockaddr_un*>(out);
    *address = {};
    address->sun_family = AF_UNIX;
    std::memcpy(address->sun_path + 1, "sumh.profile.manager.v1",
                sizeof("sumh.profile.manager.v1") - 1);
    *length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                     sizeof("sumh.profile.manager.v1")) +
              (invalid_guard ? 1 : 0);
    return 0;
}
int getpid() {
    return 444;
}
long syscall(int op, int pid, int flags) {
    ++syscalls;
    check(op == SYS_pidfd_open && pid == 444 && flags == 0);
    if (pidfd_error) {
        errno = pidfd_error;
        return -1;
    }
    return 90;
}
}  // namespace transport_model
#include "profile_manager_transport_under_test.inc"
}  // namespace fixture

int main() {
    using namespace fixture;
    using namespace transport_model;
    namespace transport = sumhp::profile_transport;
    auto clean = [&] {
        transport::release_pending();
        reset();
    };
    auto call = [&](unsigned timeout = 8000) {
        return json::parse(transport::request("/ksud", 50, 51, timeout));
    };
    auto failed = [&](const json::Value& value) {
        check(!value.at("complete").b && value.at("bytes").n == 0 && value.at("errno").n != 0);
    };
    clean();
    auto out = call();
    check(out.at("complete").b && out.at("bytes").n == files[51].size());
    check(received_input == files[50] && ack_count == 1 && spawn_count == 1 && poll_count == 0 &&
          !transport::pending.active);
    clean();
    files[50] = std::string(8 * 1024 * 1024 + 4096, 'i');
    const auto large = "{\"data\":\"" + std::string(24 * 1024 * 1024 - 11, 'x') + "\"}";
    check(large.size() == 24 * 1024 * 1024);
    response(large);
    out = call();
    check(out.at("complete").b && files[51] == large && received_input == files[50] &&
          poll_count == 0);
    clean();
    partial_read = 3;
    partial_write = 5;
    send_blocks = 2;
    out = call();
    check(out.at("complete").b && poll_count == 2);
    for (int bad = 0; bad < 11; ++bad) {
        clean();
        if (bad == 0)
            stats[50].st_nlink = 2;
        if (bad == 1)
            stats[51].st_mode |= 0040;
        if (bad == 2)
            stats[51].st_uid = 0;
        if (bad == 3)
            stats[51].st_ino = 50;
        if (bad == 4)
            files[50].clear();
        if (bad == 5)
            files[50] = std::string(8 * 1024 * 1024 + 4097, 'x');
        if (bad == 6)
            files[51] = "prior";
        if (bad == 7)
            modes[50] = O_WRONLY;
        if (bad == 8)
            modes[51] = O_RDONLY;
        if (bad == 9)
            modes[51] |= O_APPEND;
        if (bad == 10)
            snapshot_change = true;
        failed(call());
        check(spawn_count == 0);
    }
    for (int failure = 0; failure < 7; ++failure) {
        clean();
        action_fail = failure + 1;
        failed(call());
        check(spawn_count == 0 && !transport::pending.active);
    }
    for (int duplicate = 1; duplicate <= 2; ++duplicate) {
        clean();
        duplicate_fail = duplicate;
        failed(call());
        check(spawn_count == 0);
    }
    clean();
    descriptors.erase(0);
    descriptors.erase(1);
    descriptors.erase(2);
    out = call();
    check(out.at("complete").b);
    clean();
    spawn_error = ENOENT;
    failed(call());
    check(!transport::pending.active);
    clean();
    wait_error = EINVAL;
    failed(call());
    check(ack_count == 0 && transport::pending.active);
    clean();
    bind_error = EADDRINUSE;
    failed(call());
    check(spawn_count == 0);
    for (int invalid = 0; invalid < 8; ++invalid) {
        clean();
        if (invalid == 0)
            replies[0].fd = 91;
        if (invalid == 1)
            replies[0].second_fd = 92, replies[0].fd = 91;
        if (invalid == 2)
            replies[0].flags = MSG_CTRUNC;
        if (invalid == 3)
            replies[0].short_frame = true;
        if (invalid == 4)
            replies[0].magic = 1;
        if (invalid == 5)
            replies.back() = done(6, 999);
        if (invalid == 6)
            replies.push_back({5, "trailing"});
        if (invalid == 7)
            replies.back().error = EIO;
        failed(call());
        check(files[51].empty() && transport::pending.active);
    }
    clean();
    response(std::string(24 * 1024 * 1024 + 1, 'x'));
    failed(call());
    check(files[51].empty());
    clean();
    child_status = 3;
    failed(call());
    check(files[51].empty() && !transport::pending.active);
    clean();
    fsync_error = EIO;
    failed(call());
    check(files[51].empty());
    clean();
    write_error = ENOSPC;
    failed(call());
    check(files[51].empty());
    clean();
    ready = false;
    no_reply = true;
    failed(call(10));
    check(transport::pending.active && transport::pending.pidfd < 0);
    incoming.push_back({1, "", 0, 90});
    out = call();
    check(out.at("errno").n == EBUSY && ack_count == 0);
    dead = true;
    child_guard = false;
    no_reply = false;
    ready = true;
    out = call();  // New request succeeds after identified orphan exit.
    check(out.at("complete").b && spawn_count == 2);
    clean();
    no_reply = true;
    live = true;
    out = call(10);
    check(out.at("errno").n == ETIMEDOUT && received_input == files[50]);
    check(transport::pending.active && transport::pending.pidfd == 90 &&
          transport::pending.socket < 0 && child_guard && syscalls == 0);
    const auto spawned = spawn_count;
    out = call();
    check(out.at("errno").n == EBUSY && spawn_count == spawned);
    dead = true;
    live = false;
    child_guard = false;
    no_reply = false;
    out = call();
    check(out.at("complete").b);
    clean();
    send_blocks = 1;
    poll_hook = [&] {
        const auto nested = call();
        check(nested.at("errno").n == EBUSY && spawn_count == 1);
    };
    out = call();
    check(out.at("complete").b);
    clean();
    no_reply = true;
    live = true;
    failed(call(10));
    transport::release_pending();
    // A restarted client no longer has the local slot, but the still-running
    // callback holds the inherited abstract guard and prevents another spawn.
    out = call();
    check(out.at("errno").n == EBUSY && spawn_count == 1);
    // Child protocol: no callback before ACK, complete input and terminal EOF.
    auto child_input = [&] {
        child_mode = true;
        descriptors[3] = {3, true};
        incoming.push_back({2, ""});
        incoming.push_back({3, "document"});
        incoming.push_back(done(4, 8));
    };
    int callbacks = 0;
    for (int invalid = 0; invalid < 10; ++invalid) {
        clean();
        child_input();
        if (invalid == 0)
            incoming.front().kind = 3;
        if (invalid == 1)
            incoming[1].fd = 91;
        if (invalid == 2)
            incoming.back() = done(4, 7);
        if (invalid == 3)
            incoming.pop_back();
        if (invalid == 4)
            incoming.push_back({3, "late"});
        if (invalid == 5)
            incoming[1].flags = MSG_TRUNC;
        if (invalid == 6)
            incoming[1].short_frame = true;
        if (invalid == 7)
            incoming.front().fd = 91;
        if (invalid == 8)
            invalid_guard = true;
        if (invalid == 9)
            pidfd_error = ENOSYS;
        callbacks = 0;
        check(transport::serve([&](const auto&) {
                  ++callbacks;
                  return std::string("{}");
              }) == 1 &&
              callbacks == 0);
    }
    clean();
    child_input();
    callbacks = 0;
    check(transport::serve([&](const auto& bytes) {
              ++callbacks;
              check(bytes == "document");
              return std::string("{\"ok\":true}");
          }) == 0 &&
          callbacks == 1);
    check(sent.front().kind == 1 && sent.front().fd == 90 && sent.back().kind == 6 &&
          sent.back().error == 0);
    for (const auto& bad : {std::string{}, std::string("[]"), std::string("{\"a\":1,\"a\":2}"),
                            std::string("{\"a\":\"\xff\"}")}) {
        clean();
        child_input();
        check(transport::serve([&](const auto&) { return bad; }) == 1 && sent.back().error != 0);
    }
    clean();
    child_input();
    check(transport::serve([](const auto&) -> std::string { throw 3; }) == 1 &&
          sent.back().error == EIO);
    clean();
    child_input();
    incoming.clear();
    incoming.push_back({2, ""});
    for (std::size_t size = 0; size < 8 * 1024 * 1024 + 4096; size += 4096)
        incoming.push_back({3, std::string(4096, 'x')});
    incoming.push_back({3, "over"});
    incoming.push_back(done(4, 8 * 1024 * 1024 + 4100));
    callbacks = 0;
    check(transport::serve([&](const auto&) {
              ++callbacks;
              return std::string("{}");
          }) == 1 &&
          callbacks == 0);
    clean();
    child_input();
    check(transport::serve([](const auto&) { return std::string(24 * 1024 * 1024 + 1, 'x'); }) ==
              1 &&
          sent.back().error == EOVERFLOW);
    clean();
    child_input();
    callbacks = 0;
    check(transport::serve([&](const auto&) {
              ++callbacks;
              time_ns += 9000000000LL;
              return std::string("{}");
          }) == 0 &&
          callbacks == 1);
    // Callback is allowed to finish beyond transfer deadline; no cancel/replay.
    clean();
    child_input();
    callbacks = 0;
    check(transport::serve([&](const auto&) {
              ++callbacks;
              send_error = EPIPE;
              return std::string("{}");
          }) == 1 &&
          callbacks == 1);
    transport::release_pending();
    std::cout << scenarios << " profile transport scenarios / " << assertions
              << " assertions passed (syscall model)\n";
}
