#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include "userspace/common/diagnostic_package_request.hpp"
#include "userspace/common/diagnostics_request.hpp"
#include "userspace/common/json.hpp"
#include "userspace/common/module_preview_request.hpp"
#include "userspace/common/temporary_maps_request.hpp"
#include "userspace/ksud/sumhp/src/core/operation_manager_input.hpp"
#include "userspace/ksud/sumhp/src/core/user_hide_input.hpp"

namespace fixture {
namespace sumhp {
namespace module_preview = ::sumhp::module_preview;
namespace temporary_maps = ::sumhp::temporary_maps;
namespace diagnostics = ::sumhp::diagnostics;
namespace diagnostic_package_manager = ::sumhp::diagnostic_package_manager;
namespace preview_transport {
enum class Service {
    Maps,
    PartitionDiscovery,
    UserHide,
    Operation,
    ModulePreview,
    Capabilities,
    TemporaryMaps,
    Diagnostics,
    DiagnosticPackages
};
int serve(const std::function<std::string()>&, Service service = Service::Maps);
}  // namespace preview_transport
}  // namespace sumhp
using pid_t = int;
using idtype_t = int;
using id_t = unsigned;
using socklen_t = unsigned;
struct siginfo_t {
    int si_pid = 0, si_code = 0, si_status = 0;
};
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
struct sockaddr {};
struct sockaddr_un {
    int sun_family;
    char sun_path[108];
};
struct posix_spawn_file_actions_t {};
constexpr int AF_UNIX = 1, SOCK_SEQPACKET = 2, SOCK_CLOEXEC = 4, SOCK_STREAM = 8;
constexpr int SOL_SOCKET = 5, SCM_RIGHTS = 6, SO_TYPE = 7;
constexpr int MSG_NOSIGNAL = 8, MSG_DONTWAIT = 16, MSG_CMSG_CLOEXEC = 32;
constexpr int MSG_TRUNC = 64, MSG_CTRUNC = 128;
#ifndef STDIN_FILENO
constexpr int STDIN_FILENO = 0, STDOUT_FILENO = 1, STDERR_FILENO = 2;
#endif
constexpr int F_DUPFD_CLOEXEC = 91, F_SETFD = 92, FD_CLOEXEC = 93, O_WRONLY = 94;
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
#ifndef SIGKILL
constexpr int SIGKILL = 9;
#endif
#define SYS_pidfd_open 1001
#define SYS_pidfd_send_signal 1002
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
constexpr unsigned magic = 0x53504d31;
struct Wire {
    unsigned kind = 0;
    std::string body{};
    unsigned error = 0;
    int fd = -1;
    int flags = 0, second_fd = -1;
    bool short_frame = false;
    unsigned magic_value = magic;
};
std::deque<Wire> incoming;
std::vector<Wire> replies, sent;
std::vector<int> closed, killed;
std::int64_t time_ns;
int spawn_count, ack_count, wait_count, clock_count, receives;
int spawn_error, socket_error, duplicate_error, action_error, recv_error, send_error;
int wait_error, pidfd_error, child_status, clock_error_at, sleep_error;
int recv_interrupts, send_interrupts, wait_interrupts, socket_type, bind_error;
bool initial_ready, dead, blocked, no_reply, guard_bound, automatic_ack, child_mode, extra_on_exit;
bool discovery;
bool partition_save;
bool diagnostic_mode, child_guard;
bool package_mode, nested_package;
struct Descriptor {
    int tag;
    bool cloexec;
};
std::map<int, Descriptor> descriptors;
struct Action {
    char kind;
    int from;
    int to;
};
std::vector<Action> actions;
int duplicate_calls, duplicate_fail_at, action_calls, action_fail_at;
int guard_socket_error, guard_name_error, guard_type, guard_family, guard_length_delta,
    guard_fcntl_error;
std::string guard_name;
int diagnostic_parent_socket;
int allocate(Descriptor value, int minimum = 0) {
    while (descriptors.count(minimum))
        ++minimum;
    descriptors[minimum] = value;
    return minimum;
}
std::string expected_revision, expected_partitions;
std::vector<std::string> expected_hide;
std::function<void()> tick_hook;
void reset() {
    incoming.clear();
    replies.clear();
    sent.clear();
    closed.clear();
    killed.clear();
    time_ns = 0;
    spawn_count = ack_count = wait_count = clock_count = receives = 0;
    spawn_error = socket_error = duplicate_error = action_error = recv_error = send_error = 0;
    wait_error = pidfd_error = child_status = clock_error_at = sleep_error = 0;
    recv_interrupts = send_interrupts = wait_interrupts = bind_error = 0;
    initial_ready = automatic_ack = true;
    dead = blocked = no_reply = guard_bound = child_mode = extra_on_exit = false;
    discovery = false;
    partition_save = false;
    expected_revision.clear();
    expected_partitions.clear();
    expected_hide.clear();
    socket_type = SOCK_SEQPACKET;
    diagnostic_mode = child_guard = false;
    descriptors.clear();
    actions.clear();
    package_mode = nested_package = false;
    duplicate_calls = duplicate_fail_at = action_calls = action_fail_at = 0;
    guard_socket_error = guard_name_error = guard_length_delta = guard_fcntl_error = 0;
    guard_type = SOCK_STREAM;
    guard_family = AF_UNIX;
    guard_name = "sumh.doctor.sample.v1";
    diagnostic_parent_socket = 10;
    tick_hook = {};
}
int clock_gettime(int clock, struct timespec* value) {
    assert(clock == CLOCK_MONOTONIC);
    if (++clock_count == clock_error_at) {
        errno = EIO;
        return -1;
    }
    time_ns += 1'000'000;
    value->tv_sec = static_cast<time_t>(time_ns / 1'000'000'000);
    value->tv_nsec = static_cast<long>(time_ns % 1'000'000'000);
    return 0;
}
int nanosleep(const struct timespec* value, struct timespec*) {
    assert(value->tv_sec == 0 && value->tv_nsec > 0 && value->tv_nsec <= 10'000'000);
    time_ns += value->tv_nsec;
    if (tick_hook) {
        auto fn = std::move(tick_hook);
        tick_hook = {};
        fn();
    }
    if (sleep_error) {
        errno = sleep_error;
        return -1;
    }
    return 0;
}
int close(int fd) {
    assert(fd >= 0);
    closed.push_back(fd);
    if (diagnostic_mode) {
        descriptors.erase(fd);
        bool own = false;
        for (const auto& item : descriptors)
            own |= item.second.tag == 3;
        guard_bound = own || child_guard;
    } else if (fd == 91)
        guard_bound = false;
    return 0;
}
int socketpair(int domain, int type, int, int* fds) {
    assert(domain == AF_UNIX && type == (SOCK_SEQPACKET | SOCK_CLOEXEC));
    if (socket_error) {
        errno = socket_error;
        return -1;
    }
    if (diagnostic_mode) {
        fds[0] = allocate({1, true});
        fds[1] = allocate({2, true});
        diagnostic_parent_socket = fds[0];
    } else {
        fds[0] = 10;
        fds[1] = 11;
    }
    return 0;
}
int fcntl(int fd, int op, int arg) {
    if (diagnostic_mode) {
        if (op == F_SETFD) {
            assert((fd == 0 || fd == 3 || fd == 4) && arg == FD_CLOEXEC && child_mode);
            if (fd == 3 && guard_fcntl_error) {
                errno = EIO;
                return -1;
            }
            descriptors[fd].cloexec = true;
            return 0;
        }
        assert(op == F_DUPFD_CLOEXEC && arg == (nested_package ? 5 : 4) && descriptors.count(fd));
        if (++duplicate_calls == duplicate_fail_at) {
            errno = EMFILE;
            return -1;
        }
        return allocate({descriptors.at(fd).tag, true}, arg);
    }
    if (op == F_SETFD) {
        assert(fd == 0 && arg == FD_CLOEXEC);
        return 0;
    }
    assert(op == F_DUPFD_CLOEXEC && fd == 11 && arg == 3);
    if (duplicate_error) {
        errno = duplicate_error;
        return -1;
    }
    return 12;
}
int action_result() {
    return ++action_calls == action_fail_at ? ENOMEM : action_error;
}
int posix_spawn_file_actions_init(posix_spawn_file_actions_t*) {
    actions.clear();
    return action_result();
}
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t*, int from, int to) {
    if (diagnostic_mode) {
        assert(from >= (nested_package ? 5 : 4) &&
               (to == 0 || to == 3 || (nested_package && to == 4)));
        actions.push_back({'d', from, to});
        return action_result();
    }
    assert(from == 12 && to == 0);
    return action_error;
}
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t*, int fd, const char* path,
                                     int mode, int) {
    assert((fd == 1 || fd == 2) && std::string(path) == "/dev/null" && mode == O_WRONLY);
    if (diagnostic_mode)
        actions.push_back({'o', -1, fd});
    return action_result();
}
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t*, int fd) {
    if (diagnostic_mode) {
        assert(fd >= 4);
        actions.push_back({'c', fd, -1});
        return action_result();
    }
    assert(fd == 12);
    return action_error;
}
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t*) {
    return 0;
}
int posix_spawn(pid_t* pid, const char* path, posix_spawn_file_actions_t*, void*, char* const* argv,
                char**) {
    ++spawn_count;
    assert(std::string(path) == (diagnostic_mode && !package_mode ? "/proc/self/exe" : "/ksud"));
    if (diagnostic_mode) {
        auto child = descriptors;
        for (const auto& action : actions) {
            if (action.kind == 'd') {
                assert(child.count(action.from));
                child[action.to] = {child.at(action.from).tag, false};
            } else if (action.kind == 'o')
                child[action.to] = {0, false};
            else {
                assert(child.count(action.from));
                child.erase(action.from);
            }
        }
        for (const auto& item : descriptors)
            if (item.second.tag)
                assert(item.second.cloexec);
        for (auto it = child.begin(); it != child.end();) {
            if (it->second.cloexec)
                it = child.erase(it);
            else
                ++it;
        }
        assert(child.at(0).tag == 2 && child.at(3).tag == 3 && child.at(1).tag == 0 &&
               child.at(2).tag == 0);
        assert(actions.size() == (nested_package ? 8U : 6U) && actions[0].to == 0 &&
               actions[1].to == 1 && actions[2].to == 2 && actions[3].to == 3);
        assert(actions[0].from != actions[3].from);
        if (nested_package) {
            assert(child.at(4).tag == 4 && actions[4].to == 4);
            assert(actions[4].from != actions[0].from && actions[4].from != actions[3].from);
        }
    }
    std::vector<std::string> expected =
        partition_save
            ? std::vector<std::string>{"/ksud",          "sumhp",           "config",
                                       "set-partitions", expected_revision, expected_partitions,
                                       "--channel"}
        : discovery
            ? std::vector<std::string>{"/ksud", "sumhp", "config", "discover-partitions",
                                       "--channel"}
            : std::vector<std::string>{"/ksud", "sumhp", "sumh", "maps", "preview", "--channel"};
    if (!expected_hide.empty()) {
        expected = {diagnostic_mode && !package_mode ? "ksud" : "/ksud", "sumhp"};
        expected.insert(expected.end(), expected_hide.begin(), expected_hide.end());
        expected.push_back("--channel");
    }
    for (std::size_t i = 0; i < expected.size(); ++i)
        assert(argv[i] && argv[i] == expected[i]);
    assert(argv[expected.size()] == nullptr);
    if (spawn_error)
        return spawn_error;
    if (diagnostic_mode)
        child_guard = true;
    *pid = 123;
    if (initial_ready)
        incoming.push_back({1, {}, 0, 81});
    return 0;
}
int getpid() {
    return 321;
}
long syscall(long number, ...) {
    va_list args;
    va_start(args, number);
    const int fd = va_arg(args, int);
    va_end(args);
    if (number == SYS_pidfd_open) {
        assert(fd == 321);  // Own process only; never the numeric spawn result.
        if (pidfd_error) {
            errno = pidfd_error;
            return -1;
        }
        return 81;
    }
    assert(number == SYS_pidfd_send_signal && fd == 81);
    killed.push_back(fd);
    if (!blocked) {
        dead = true;
        child_guard = false;
    }
    return 0;
}
int waitid(idtype_t type, id_t id, siginfo_t* info, int flags) {
    assert(type == 3 && id == 81 && flags == (WEXITED | WNOHANG));
    ++wait_count;
    if (wait_interrupts) {
        --wait_interrupts;
        errno = EINTR;
        return -1;
    }
    if (wait_error) {
        errno = wait_error;
        return -1;
    }
    if (dead) {
        info->si_pid = 123;
        info->si_code = CLD_EXITED;
        info->si_status = child_status;
        if (extra_on_exit) {
            incoming.push_back({3});
            extra_on_exit = false;
        }
    }
    return 0;
}
int sendmsg(int socket, const msghdr* message, int flags) {
    assert((flags & (MSG_DONTWAIT | MSG_NOSIGNAL)) == (MSG_DONTWAIT | MSG_NOSIGNAL));
    if (send_interrupts) {
        --send_interrupts;
        errno = EINTR;
        return -1;
    }
    if (send_error) {
        errno = send_error;
        return -1;
    }
    const auto* header = static_cast<unsigned*>(message->msg_iov->iov_base);
    assert(header[0] == magic);
    Wire wire{header[1], std::string(reinterpret_cast<const char*>(header + 4), header[2]),
              header[3]};
    if (message->msg_control)
        std::memcpy(&wire.fd, CMSG_DATA(CMSG_FIRSTHDR(message)), sizeof(int));
    sent.push_back(wire);
    if (child_mode) {
        assert(socket == 0);
        if (wire.kind == 1 && automatic_ack)
            incoming.push_back({4});
    } else {
        assert(socket == (diagnostic_mode ? diagnostic_parent_socket : 10) && wire.kind == 4);
        ++ack_count;
        if (!no_reply) {
            if (replies.empty()) {
                incoming.push_back({2, "{\"sample\":true}"});
                incoming.push_back({3});
            } else
                for (const auto& reply : replies)
                    incoming.push_back(reply);
            dead = !blocked;
            if (dead)
                child_guard = false;
        }
    }
    return static_cast<int>(message->msg_iov->iov_len);
}
int recvmsg(int, msghdr* message, int flags) {
    assert(flags == (MSG_DONTWAIT | MSG_CMSG_CLOEXEC));
    ++receives;
    if (recv_interrupts) {
        --recv_interrupts;
        errno = EINTR;
        return -1;
    }
    if (recv_error) {
        errno = recv_error;
        return -1;
    }
    if (incoming.empty()) {
        message->msg_controllen = 0;
        if (dead)
            return 0;
        errno = EAGAIN;
        return -1;
    }
    auto wire = incoming.front();
    incoming.pop_front();
    const unsigned header[]{wire.magic_value, wire.kind, static_cast<unsigned>(wire.body.size()),
                            wire.error};
    const auto total = sizeof(header) + wire.body.size();
    std::memcpy(message->msg_iov->iov_base, header, sizeof(header));
    std::memcpy(static_cast<char*>(message->msg_iov->iov_base) + sizeof(header), wire.body.data(),
                std::min(wire.body.size(), message->msg_iov->iov_len - sizeof(header)));
    message->msg_flags = wire.flags;
    if (wire.fd >= 0) {
        auto* control = static_cast<cmsghdr*>(message->msg_control);
        control->cmsg_len = CMSG_LEN(sizeof(int) * (wire.second_fd >= 0 ? 2 : 1));
        control->cmsg_level = SOL_SOCKET;
        control->cmsg_type = SCM_RIGHTS;
        std::memcpy(CMSG_DATA(control), &wire.fd, sizeof(int));
        if (wire.second_fd >= 0)
            std::memcpy(CMSG_DATA(control) + sizeof(int), &wire.second_fd, sizeof(int));
        message->msg_controllen = control->cmsg_len;
    } else
        message->msg_controllen = 0;
    return wire.short_frame ? 3 : static_cast<int>(total);
}
int getsockopt(int socket, int, int option, void* value, socklen_t*) {
    if (diagnostic_mode && socket == 4) {
        if (!descriptors.count(4) || descriptors.at(4).tag != 4) {
            errno = EBADF;
            return -1;
        }
        *static_cast<int*>(value) = SOCK_STREAM;
        return 0;
    }
    if (diagnostic_mode && socket == 3) {
        if (guard_socket_error) {
            errno = EBADF;
            return -1;
        }
        *static_cast<int*>(value) = guard_type;
        return 0;
    }
    assert(socket == 0 && option == SO_TYPE);
    *static_cast<int*>(value) = socket_type;
    return 0;
}
int getsockname(int fd, sockaddr* value, socklen_t* length) {
    assert(diagnostic_mode && (fd == 3 || fd == 4));
    if (guard_name_error) {
        errno = EBADF;
        return -1;
    }
    auto* address = reinterpret_cast<sockaddr_un*>(value);
    address->sun_family = guard_family;
    address->sun_path[0] = 0;
    const std::string name = fd == 4 ? "sumh.doctor.package.v1" : guard_name;
    std::memcpy(address->sun_path + 1, name.data(), name.size());
    *length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + name.size() + 1 +
                                     guard_length_delta);
    return 0;
}
int socket(int domain, int type, int) {
    assert(domain == AF_UNIX && type == (SOCK_STREAM | SOCK_CLOEXEC));
    if (diagnostic_mode) {
        if (guard_socket_error) {
            errno = EMFILE;
            return -1;
        }
        return allocate({3, true});
    }
    return 91;
}
int bind(int fd, const sockaddr* value, socklen_t length) {
    assert(diagnostic_mode ? descriptors.at(fd).tag == 3 : fd == 91);
    auto* address = reinterpret_cast<const sockaddr_un*>(value);
    assert(address->sun_family == AF_UNIX && address->sun_path[0] == 0);
    const std::string expected = package_mode      ? "sumh.doctor.package.v1"
                                 : diagnostic_mode ? "sumh.doctor.sample.v1"
                                                   : "sumh.maps.preview.sample.v1";
    assert(length == offsetof(sockaddr_un, sun_path) + expected.size() + 1);
    assert(std::string(address->sun_path + 1, expected.size()) == expected);
    if (bind_error || guard_bound) {
        errno = bind_error ? bind_error : EADDRINUSE;
        return -1;
    }
    guard_bound = true;
    return 0;
}
}  // namespace transport_model

#include "maps_preview_transport_under_test.inc"
}  // namespace fixture

int main() {
    using namespace fixture;
    namespace api = fixture::sumhp::preview_transport;
    namespace model = transport_model;
    int tests = 0;
    const auto reset = [&] {
        api::release_pending();
        api::package_guard_for_sampling = -1;
        model::reset();
        ++tests;
    };
    const auto result = [](unsigned ms = 80) {
        return json::parse(api::request("/ksud", ms, api::Service::Maps));
    };
    const auto expect = [&](int error, unsigned ms = 80) {
        auto value = result(ms);
        assert(value.at("errno").n == error && value.at("ok").b == (error == 0));
        if (error)
            assert(value.at("stdout").s.empty());
    };
    reset();
    expect(0);
    assert(model::spawn_count == 1 && model::ack_count == 1 && !api::pending.active);
    reset();
    model::discovery = true;
    assert(json::parse(api::request("/ksud", 80, api::Service::PartitionDiscovery)).at("ok").b);
    assert(model::spawn_count == 1 && model::ack_count == 1 && !api::pending.active);
    reset();
    model::expected_hide = {"api", "capabilities"};
    assert(json::parse(api::request("/ksud", 80, api::Service::Capabilities)).at("ok").b);
    assert(model::spawn_count == 1 && model::ack_count == 1 && !api::pending.active);
    const std::string revision = "sha256-v1:" + std::string(64, 'a');
    const std::string partitions = "[\"oem\",\"vendor_dlkm\"]";
    reset();
    model::partition_save = true;
    model::expected_revision = revision;
    model::expected_partitions = partitions;
    assert(json::parse(api::request_partitions_save("/ksud", revision, partitions, 80)).at("ok").b);
    assert(model::spawn_count == 1 && model::ack_count == 1);
    reset();
    assert(json::parse(api::request_partitions_save("/ksud", revision + "a", partitions, 80))
               .at("errno")
               .n == EINVAL);
    assert(!model::spawn_count);
    reset();
    assert(json::parse(api::request_partitions_save("/ksud", "sha256-v1:" + std::string(64, 'A'),
                                                    partitions, 80))
               .at("errno")
               .n == EINVAL);
    assert(!model::spawn_count);
    reset();
    assert(json::parse(api::request_partitions_save("/ksud", revision, std::string(40961, ' '), 80))
               .at("errno")
               .n == EINVAL);
    assert(!model::spawn_count);
    reset();
    assert(json::parse(api::request_partitions_save("/ksud", revision, std::string("[]\0", 3), 80))
               .at("errno")
               .n == EINVAL);
    assert(!model::spawn_count);
    reset();
    model::partition_save = true;
    model::expected_revision = revision;
    model::expected_partitions = partitions;
    model::no_reply = true;
    model::blocked = true;
    assert(json::parse(api::request_partitions_save("/ksud", revision, partitions, 80))
               .at("errno")
               .n == ETIMEDOUT);
    assert(json::parse(api::request("/ksud", 80, api::Service::PartitionDiscovery)).at("errno").n ==
           EBUSY);
    assert(model::spawn_count == 1);  // No replay or separate slot after unknown save outcome.
    const auto hide_json = [](const std::vector<std::string>& args) {
        auto value = json::Value::array();
        for (const auto& arg : args)
            value.a.emplace_back(arg);
        return json::dump(value);
    };
    const std::string hide_id(32, 'b');
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"hide", "status"},
             {"hide", "status", revision, "16"},
             {"hide", "definitions"},
             {"hide", "definitions", revision, "64"},
             {"api", "meta", "--control-only"},
             {"hide", "import-id", hide_id, revision,
              "{\"path\":\"/import\\n "
              "\",\"enabled\":false,\"name\":\"\",\"note\":\"\",\"group\":\"--help\",\"expect\":"
              "\"absent\"}"},
             {"hide", "create", revision,
              "{\"path\":\"/路径 \",\"enabled\":false,\"note\":\"$() `;\\n\"}"},
             {"hide", "annotate", hide_id, revision, "{\"group\":\"组\\n \",\"name\":\"$() ;`\"}"},
             {"hide", "enable", hide_id, revision},
             {"hide", "disable", hide_id, revision},
             {"hide", "enable-group", "--channel", revision},
             {"hide", "disable-group", "", revision},
             {"hide", "retry", hide_id, revision},
             {"hide", "remove-id", hide_id, revision}}) {
        reset();
        model::expected_hide = args;
        assert(json::parse(api::request_user_hide("/ksud", hide_json(args), 80)).at("ok").b);
        assert(model::spawn_count == 1 && model::ack_count == 1 && !api::pending.active);
    }
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"hide", "add", "/x"},
             {"hide", "remove", "/x"},
             {"hide", "retry-path", "/x"},
             {"sumh", "clear"},
             {"hide", "export"},
             {"hide", "definitions", revision, "01"},
             {"hide", "import-id", hide_id, revision, "{}"},
             {"api", "meta"},
             {"api", "meta", "--control-only", "extra"},
             {"api", "sumh"},
             {"api", "meta", "--channel"},
             {"hide", "status", "--channel"},
             {"hide", "status", revision, "01"},
             {"hide", "status", revision, "4097"},
             {"hide", "disable", hide_id, "bad"},
             {"hide", "enable", std::string(32, 'B'), revision},
             {"hide", "create", revision, "{\"path\":\"/a\",\"path\":\"/b\"}"},
             {"hide", "annotate", hide_id, revision, "{\"enabled\":false}"},
             {"hide", "enable-group", std::string("x\0y", 3), revision},
             {"hide", "enable-group", std::string(1, char(0xff)), revision}}) {
        reset();
        assert(json::parse(api::request_user_hide("/ksud", hide_json(args), 80)).at("errno").n ==
               EINVAL);
        assert(model::spawn_count == 0 && model::ack_count == 0);
    }
    for (const auto& input : std::vector<std::string>{
             "{}", "[\"hide\",7]", "[\"hide\",\"status\"] trailing",
             std::string(48 * 1024 + 1, ' '), std::string("[]\0", 3), std::string(1, char(0xff))}) {
        reset();
        assert(json::parse(api::request_user_hide("/ksud", input, 80)).at("errno").n == EINVAL);
        assert(!model::spawn_count);
    }
    reset();
    assert(json::parse(api::request("/ksud", 80, api::Service::UserHide)).at("errno").n == EINVAL);
    assert(!model::spawn_count);
    reset();
    model::expected_hide = {"hide", "status"};
    auto padded = hide_json(model::expected_hide);
    padded.append(48 * 1024 - padded.size(), ' ');
    assert(json::parse(api::request_user_hide("/ksud", padded, 80)).at("ok").b);
    reset();
    model::expected_hide = {"hide", "disable", hide_id, revision};
    model::blocked = true;
    model::no_reply = true;
    assert(json::parse(api::request_user_hide("/ksud", hide_json(model::expected_hide), 80))
               .at("errno")
               .n == ETIMEDOUT);
    assert(json::parse(api::request_user_hide("/ksud", hide_json(model::expected_hide), 80))
               .at("errno")
               .n == EBUSY);
    assert(json::parse(api::request("/ksud", 80, api::Service::Maps)).at("errno").n == EBUSY &&
           model::spawn_count == 1);
    auto operation = json::Value::object();
    operation.o["schema_version"] = 1;
    operation.o["action"] = "show";
    operation.o["operation_id"] = hide_id;
    const auto operation_json = json::dump(operation);
    reset();
    model::expected_hide = {"operation", "manager", operation_json};
    assert(json::parse(api::request_operation("/ksud", operation_json, 80)).at("ok").b &&
           model::spawn_count == 1 && model::ack_count == 1);
    reset();
    assert(json::parse(api::request("/ksud", 80, api::Service::Operation)).at("errno").n ==
               EINVAL &&
           !model::spawn_count);
    for (const auto& invalid : std::vector<std::string>{"{}", "[]", operation_json + " {}",
                                                        std::string(48 * 1024 + 1, ' ')}) {
        reset();
        assert(json::parse(api::request_operation("/ksud", invalid, 80)).at("errno").n == EINVAL &&
               !model::spawn_count);
    }
    reset();
    model::expected_hide = {"operation", "manager", operation_json};
    model::blocked = true;
    model::no_reply = true;
    assert(json::parse(api::request_operation("/ksud", operation_json, 80)).at("errno").n ==
           ETIMEDOUT);
    assert(json::parse(api::request_operation("/ksud", operation_json, 80)).at("errno").n ==
               EBUSY &&
           model::spawn_count == 1);
    // Maps tracking is opt-in schema 2; old schema 1 remains hide-only. These
    // requests travel through the existing fixed Operation executable channel.
    auto tracked = json::Value::object();
    tracked.o["schema_version"] = 2;
    tracked.o["resource"] = "temporary_maps";
    tracked.o["action"] = "prepare";
    auto tracked_input = json::Value::object();
    tracked_input.o["schema_version"] = 1;
    tracked_input.o["args"] = json::parse(
        hide_json({"sumh", "maps", "remove", "18446744073709551615", "4294967295", "--instance",
                   "18446744073709551615", "--generation", "9007199254740993"}));
    tracked.o["input"] = tracked_input;
    for (const auto* action : {"prepare", "execute", "show", "wait", "result"}) {
        auto request = tracked;
        request.o["action"] = action;
        if (std::string(action) != "prepare")
            request.o["operation_id"] = hide_id;
        if (std::string(action) != "prepare" && std::string(action) != "execute")
            request.o.erase("input");
        if (std::string(action) == "wait")
            request.o["timeout_ms"] = 5000;
        const auto text = json::dump(request);
        reset();
        model::expected_hide = {"operation", "manager", text};
        assert(json::parse(api::request_operation("/ksud", text, 80)).at("ok").b);
        assert(model::spawn_count == 1 && model::ack_count == 1);
    }
    for (const auto& change : std::vector<std::function<void(json::Value&)>>{
             [](auto& v) { v.o["schema_version"] = 1; }, [](auto& v) { v.o.erase("resource"); },
             [](auto& v) { v.o["resource"] = "user_hide"; },
             [](auto& v) { v.o["action"] = "clear"; },
             [](auto& v) { v.o["input"].o["args"].a[3] = "01"; },
             [](auto& v) { v.o["input"].o["args"] = json::parse("[\"config\",\"apply\"]"); }}) {
        auto invalid = tracked;
        change(invalid);
        reset();
        assert(
            json::parse(api::request_operation("/ksud", json::dump(invalid), 80)).at("errno").n ==
            EINVAL);
        assert(model::spawn_count == 0 && model::ack_count == 0);
    }
    tracked.o["action"] = "execute";
    tracked.o["operation_id"] = hide_id;
    const auto tracked_json = json::dump(tracked);
    reset();
    model::expected_hide = {"operation", "manager", tracked_json};
    model::blocked = true;
    model::no_reply = true;
    assert(json::parse(api::request_operation("/ksud", tracked_json, 80)).at("errno").n ==
           ETIMEDOUT);
    assert(json::parse(api::request_operation("/ksud", tracked_json, 80)).at("errno").n == EBUSY);
    assert(model::spawn_count == 1 &&
           model::ack_count == 1);  // no transport retry after acknowledgement
    reset();
    assert(json::parse(api::request("/ksud", 80, static_cast<api::Service>(999))).at("errno").n ==
           EINVAL);
    assert(!model::spawn_count);
    reset();
    assert(json::parse(api::request("relative", 80, api::Service::Maps)).at("errno").n == EINVAL);
    assert(!model::spawn_count);
    reset();
    assert(
        json::parse(api::request(std::string("/a\0b", 4), 80, api::Service::Maps)).at("errno").n ==
        EINVAL);
    reset();
    expect(EINVAL, 0);
    assert(!model::spawn_count);
    reset();
    expect(EINVAL, 8001);
    reset();
    model::spawn_error = ENOENT;
    expect(ENOENT);
    assert(!api::pending.active);
    reset();
    model::socket_error = EMFILE;
    expect(EMFILE);
    assert(!model::spawn_count);
    reset();
    model::duplicate_error = EMFILE;
    expect(EMFILE);
    reset();
    model::action_error = ENOMEM;
    expect(ENOMEM);
    reset();
    model::initial_ready = false;
    expect(ETIMEDOUT);
    assert(api::pending.active && model::wait_count == 0);
    expect(EBUSY);
    assert(model::spawn_count == 1);
    assert(json::parse(api::request("/ksud", 80, api::Service::PartitionDiscovery)).at("errno").n ==
           EBUSY);
    assert(model::spawn_count == 1);  // Services share the unreaped-child quarantine.
    reset();
    model::initial_ready = false;
    expect(ETIMEDOUT);
    model::incoming.push_back({1, {}, 0, 81});
    model::initial_ready = true;
    expect(0);
    assert(model::spawn_count == 2 && model::ack_count == 1 && !model::killed.empty());
    reset();
    model::initial_ready = false;
    model::incoming.push_back({3, {}, ENOSYS});
    expect(ENOSYS);
    expect(EBUSY);
    reset();
    model::no_reply = true;
    model::blocked = true;
    expect(ETIMEDOUT);
    expect(EBUSY);
    assert(model::spawn_count == 1);
    model::blocked = false;
    model::dead = true;
    model::no_reply = false;
    expect(0);
    assert(model::spawn_count == 2);
    reset();
    model::send_error = EPIPE;
    expect(EPIPE);
    assert(model::killed.size() == 1);
    reset();
    model::recv_error = EIO;
    expect(EIO);
    assert(api::pending.active && model::wait_count == 0);
    reset();
    model::recv_interrupts = 1000;
    expect(ETIMEDOUT);
    assert(model::time_ns < 200'000'000);
    reset();
    model::send_interrupts = 1000;
    expect(ETIMEDOUT);
    reset();
    model::wait_interrupts = 1000;
    expect(ETIMEDOUT);
    reset();
    model::wait_error = EINVAL;
    expect(EINVAL);
    expect(EBUSY);
    reset();
    model::wait_error = ECHILD;
    expect(ECHILD);
    assert(!api::pending.active);
    reset();
    model::child_status = 1;
    expect(EIO);
    reset();
    model::clock_error_at = 1;
    expect(EIO);
    assert(!model::spawn_count);
    reset();
    model::clock_error_at = 3;
    expect(EIO);
    reset();
    model::sleep_error = EIO;
    model::no_reply = true;
    expect(EIO);
    reset();
    model::replies = {{2, "{}"}};
    expect(EIO);
    reset();
    model::replies = {{3}, {2, "{}"}};
    expect(EPROTO);
    reset();
    model::replies = {{2, "{}"}, {3}, {3}};
    expect(EPROTO);
    reset();
    model::extra_on_exit = true;
    expect(EPROTO);
    reset();
    model::replies = {{1, {}, 0, 82}};
    expect(EPROTO);
    assert(model::closed.end() != std::find(model::closed.begin(), model::closed.end(), 82));
    reset();
    model::replies = {{2, "{}", 0, 82}};
    expect(EPROTO);
    reset();
    model::replies = {{2, "{}", 0, -1, MSG_TRUNC}};
    expect(EPROTO);
    reset();
    model::initial_ready = false;
    model::incoming.push_back({1, {}, 0, 81, 0, 82});
    expect(EPROTO);
    reset();
    model::initial_ready = false;
    model::incoming.push_back({1, {}, 0, 81, MSG_CTRUNC});
    expect(EPROTO);
    reset();
    model::replies = {{2, "{}", 0, -1, 0, -1, true}};
    expect(EPROTO);
    reset();
    model::replies = {{2, "{}", 0, -1, 0, -1, false, 1}};
    expect(EPROTO);
    reset();
    model::replies = {{2, "{"}, {3}};
    expect(EPROTO);
    reset();
    model::replies = {{2, "[]"}, {3}};
    expect(EPROTO);
    reset();
    model::replies = {{2, "{} {}"}, {3}};
    expect(EPROTO);
    reset();
    model::replies = {{3, {}, EOVERFLOW}};
    expect(EOVERFLOW);
    reset();
    for (int i = 0; i < 129; ++i)
        model::replies.push_back({2, std::string(4096, ' ')});
    model::replies.push_back({3});
    expect(EOVERFLOW, 8000);
    reset();
    std::string large = "{\"value\":\"" + std::string(512 * 1024 - 12, 'x') + "\"}";
    assert(large.size() == 512 * 1024);
    for (std::size_t at = 0; at < large.size(); at += 4096)
        model::replies.push_back({2, large.substr(at, 4096)});
    model::replies.push_back({3});
    expect(0, 8000);
    reset();
    model::child_mode = true;
    int callbacks = 0;
    assert(api::serve([&] {
               ++callbacks;
               return "{}";
           }) == 0 &&
           callbacks == 1);
    assert(model::sent.size() == 3 && model::sent[0].fd == 81);
    reset();
    model::child_mode = true;
    model::pidfd_error = ENOSYS;
    callbacks = 0;
    assert(api::serve([&] {
               ++callbacks;
               return "{}";
           }) == 1 &&
           callbacks == 0);
    reset();
    model::child_mode = true;
    model::automatic_ack = false;
    callbacks = 0;
    assert(api::serve([&] {
               ++callbacks;
               return "{}";
           }) == 1 &&
           callbacks == 0);
    reset();
    model::child_mode = true;
    model::socket_type = SOCK_STREAM;
    callbacks = 0;
    assert(api::serve([&] {
               ++callbacks;
               return "{}";
           }) == 1 &&
           callbacks == 0);
    reset();
    model::child_mode = true;
    model::automatic_ack = false;
    model::incoming.push_back({4, {}, 0, 82});
    callbacks = 0;
    assert(api::serve([&] {
               ++callbacks;
               return "{}";
           }) == 1 &&
           callbacks == 0);
    reset();
    model::child_mode = true;
    assert(api::serve([]() -> std::string { throw std::runtime_error("failed"); }) == 1);
    assert(model::sent.back().kind == 3 && model::sent.back().error == EIO);
    reset();
    model::child_mode = true;
    assert(api::serve([] { return std::string(512 * 1024 + 1, 'x'); }) == 1);
    assert(model::sent.back().error == EOVERFLOW);
    const std::string candidate =
        R"({"schema_version":1,"kind":"sumh.module.plan.input","modules":[]})";
    namespace module = ::sumhp::module_preview;
    for (const auto& input :
         std::vector<module::Input>{{std::nullopt, ""}, {candidate, "/vendor/中文 $()`; \t "}}) {
        reset();
        model::expected_hide = module::arguments(input);
        assert(json::parse(api::request_module_preview("/ksud", input, 80)).at("ok").b);
        assert(model::spawn_count == 1 && model::ack_count == 1);
        auto argv = model::expected_hide;
        argv.push_back("--channel");
        module::Input parsed;
        assert(module::parse_arguments(argv, parsed) && parsed.candidate == input.candidate &&
               parsed.explain_path == input.explain_path);
    }
    std::string exact_candidate = candidate;
    exact_candidate.resize(module::input_bytes, ' ');
    reset();
    model::expected_hide = module::arguments({exact_candidate, "/" + std::string(4094, 'a')});
    assert(json::parse(api::request_module_preview(
                           "/ksud", {exact_candidate, "/" + std::string(4094, 'a')}, 80))
               .at("ok")
               .b);
    for (const auto& input : std::vector<module::Input>{
             {"", ""},
             {"{}", ""},
             {candidate + " {}", ""},
             {exact_candidate + " ", ""},
             {std::string("{}\0", 3), ""},
             {std::string(1, char(0xff)), ""},
             {R"({"schema_version":1,"schema_version":1,"kind":"sumh.module.plan.input"})", ""},
             {R"({"schema_version":1,"kind":"sumh.module.plan.input","unknown":true})", ""},
             {std::nullopt, "relative"},
             {std::nullopt, "/a//b"},
             {std::nullopt, "/a/../b"},
             {std::nullopt, "/a/"},
             {std::nullopt, "/a\n"},
             {std::nullopt, "/a\r"},
             {std::nullopt, std::string("/a\0", 3)},
             {std::nullopt, "/" + std::string(4095, 'a')},
             {std::nullopt, "/" + std::string(1, char(0xff))}}) {
        reset();
        assert(json::parse(api::request_module_preview("/ksud", input, 80)).at("errno").n ==
               EINVAL);
        assert(!model::spawn_count);
    }
    for (const auto& argv : std::vector<std::vector<std::string>>{
             {"module", "plan", "--channel"},
             {"module", "manager-preview", "saved", ""},
             {"module", "manager-preview", "saved", "", "extra", "--channel"},
             {"module", "manager-preview", "candidate", "", "", "--channel"},
             {"module", "manager-preview", "candidate", candidate, "", "--channel", "extra"}}) {
        reset();
        module::Input input;
        assert(!module::parse_arguments(argv, input));
    }
    reset();
    assert(json::parse(api::request("/ksud", 80, api::Service::ModulePreview)).at("errno").n ==
               EINVAL &&
           !model::spawn_count);
    const auto module_response = [&](const std::string& body) {
        model::expected_hide = module::arguments({std::nullopt, ""});
        for (std::size_t at = 0; at < body.size(); at += 4096)
            model::replies.push_back({2, body.substr(at, 4096)});
        model::replies.push_back({3});
        return api::request_module_preview("/ksud", {std::nullopt, ""}, 8000);
    };
    auto quoted = json::Value::object();
    quoted.o["value"] = std::string((module::response_bytes - 12) / 2, '\\');
    const auto heavy = json::dump(quoted);
    assert(heavy.size() == module::response_bytes);
    reset();
    const auto heavy_envelope = module_response(heavy);
    assert(heavy_envelope.size() > 4UL * 1024 * 1024 &&
           heavy_envelope.size() <= module::envelope_bytes);
    assert(json::parse(heavy_envelope).at("stdout").s == heavy);
    reset();
    assert(json::parse(module_response(heavy + " ")).at("errno").n == EOVERFLOW);
    reset();
    assert(json::parse(module_response("{\n\"valid\":true}")).at("errno").n == EPROTO);
    reset();
    assert(json::parse(module_response("{}")).at("ok").b);
    reset();
    model::child_mode = true;
    assert(api::serve([&] { return heavy; }, api::Service::ModulePreview) == 0);
    assert(model::sent.size() == 2 + module::response_bytes / 4096 &&
           model::sent.back().error == 0);
    reset();
    model::child_mode = true;
    assert(api::serve([&] { return heavy + " "; }, api::Service::ModulePreview) == 1);
    assert(model::sent.back().error == EOVERFLOW);
    reset();
    model::child_mode = true;
    assert(api::serve([] { return "{\n}"; }, api::Service::ModulePreview) == 1);
    assert(model::sent.back().error == EPROTO);
    reset();
    model::child_mode = true;
    model::automatic_ack = false;
    callbacks = 0;
    assert(api::serve(
               [&] {
                   ++callbacks;
                   return "{}";
               },
               api::Service::ModulePreview) == 1 &&
           callbacks == 0);
    reset();
    model::expected_hide = module::arguments({candidate, ""});
    model::no_reply = true;
    model::blocked = true;
    assert(json::parse(api::request_module_preview("/ksud", {candidate, ""}, 80)).at("errno").n ==
           ETIMEDOUT);
    assert(json::parse(api::request_module_preview("/ksud", {candidate, ""}, 80)).at("errno").n ==
               EBUSY &&
           model::spawn_count == 1);
    const std::string temporary_request =
        R"({"schema_version":1,"action":"remove","instance_id":"1","generation":"2","target_ino":"0","target_dev":"0"})";
    const auto temporary_response = [&](const std::string& body) {
        model::expected_hide = {"sumh", "maps", "manager", temporary_request};
        for (std::size_t at = 0; at < body.size(); at += 4096)
            model::replies.push_back({2, body.substr(at, 4096)});
        model::replies.push_back({3});
        return api::request_temporary_maps("/ksud", temporary_request, 8000);
    };
    const int temporary_start = tests;
    reset();
    assert(json::parse(temporary_response("{}")).at("ok").b && model::spawn_count == 1 &&
           model::ack_count == 1);
    reset();
    assert(json::parse(api::request("/ksud", 80, api::Service::TemporaryMaps)).at("errno").n ==
               EINVAL &&
           !model::spawn_count);
    for (const auto& invalid :
         std::vector<std::string>{"{}", "[]", temporary_request + " {}", std::string(1025, ' '),
                                  R"({"schema_version":1,"action":"clear"})",
                                  R"({"schema_version":1,"action":"list","action":"list"})"}) {
        reset();
        assert(json::parse(api::request_temporary_maps("/ksud", invalid, 80)).at("errno").n ==
                   EINVAL &&
               !model::spawn_count);
    }
    for (const auto& invalid :
         std::vector<std::string>{"{", "[]", "{} {}", "{\n}", "{\"a\":0,\"a\":0}",
                                  std::string("{\"a\":\"") + char(0xff) + "\"}"}) {
        reset();
        const auto response = json::parse(temporary_response(invalid));
        assert(response.at("errno").n == EPROTO && response.at("stdout").s.empty());
    }
    auto temporary_heavy = json::Value::object();
    temporary_heavy["value"] =
        std::string((::sumhp::temporary_maps::response_bytes - 12) / 2, '\\');
    const auto temporary_raw = json::dump(temporary_heavy);
    assert(temporary_raw.size() == ::sumhp::temporary_maps::response_bytes);
    reset();
    const auto temporary_envelope = temporary_response(temporary_raw);
    assert(temporary_envelope.size() <= ::sumhp::temporary_maps::envelope_bytes &&
           json::parse(temporary_envelope).at("stdout").s == temporary_raw);
    reset();
    assert(json::parse(temporary_response(temporary_raw + " ")).at("errno").n == EOVERFLOW);
    reset();
    model::child_mode = true;
    callbacks = 0;
    assert(api::serve(
               [&] {
                   ++callbacks;
                   return temporary_raw;
               },
               api::Service::TemporaryMaps) == 0 &&
           callbacks == 1);
    reset();
    model::child_mode = true;
    model::automatic_ack = false;
    callbacks = 0;
    assert(api::serve(
               [&] {
                   ++callbacks;
                   return "{}";
               },
               api::Service::TemporaryMaps) == 1 &&
           callbacks == 0);
    reset();
    model::child_mode = true;
    assert(api::serve([&] { return temporary_raw + " "; }, api::Service::TemporaryMaps) == 1 &&
           model::sent.back().error == EOVERFLOW);
    reset();
    model::expected_hide = {"sumh", "maps", "manager", temporary_request};
    model::no_reply = true;
    model::blocked = true;
    const auto unknown = json::parse(api::request_temporary_maps("/ksud", temporary_request, 80));
    assert(unknown.at("errno").n == ETIMEDOUT && unknown.at("stdout").s.empty() &&
           model::ack_count == 1);
    assert(json::parse(api::request_temporary_maps("/ksud", temporary_request, 80)).at("errno").n ==
           EBUSY);
    assert(json::parse(api::request("/ksud", 80, api::Service::Maps)).at("errno").n == EBUSY &&
           model::spawn_count == 1);
    reset();
    model::expected_hide = {"sumh", "maps", "manager", temporary_request};
    model::replies = {{2, "{\"result\":"}};
    const auto incomplete =
        json::parse(api::request_temporary_maps("/ksud", temporary_request, 80));
    assert(!incomplete.at("ok").b && incomplete.at("stdout").s.empty() && model::spawn_count == 1);
    std::cout << (tests - temporary_start) << " temporary Maps private transport checks passed\n";
    const int diagnostics_start = tests;
    const std::string diagnostic_request = R"({"schema_version":1,"scenario":"boot-protection"})";
    const auto diagnostic_setup = [&](unsigned closed_stdio = 0, bool occupied_three = false) {
        reset();
        model::diagnostic_mode = true;
        for (int fd = 0; fd < 3; ++fd)
            if (!(closed_stdio & (1U << fd)))
                model::descriptors[fd] = {0, false};
        if (occupied_three)
            model::descriptors[3] = {9, true};
        model::expected_hide = {"doctor", "manager", diagnostic_request};
    };
    const auto diagnostic_result = [&](unsigned ms = 80) {
        return json::parse(api::request_diagnostics(diagnostic_request, ms));
    };
    const auto diagnostic_expect = [&](int error, unsigned ms = 80) {
        const auto value = diagnostic_result(ms);
        assert(value.at("errno").n == error && value.at("ok").b == (error == 0));
        if (error)
            assert(value.at("stdout").s.empty());
    };
    const auto no_owned_fds = [] {
        for (const auto& item : model::descriptors)
            assert(item.second.tag != 1 && item.second.tag != 2 && item.second.tag != 3);
    };
    for (unsigned closed = 0; closed < 8; ++closed)
        for (bool occupied : {false, true}) {
            diagnostic_setup(closed, occupied);
            const auto before = model::descriptors;
            diagnostic_expect(0);
            assert(model::spawn_count == 1 && model::ack_count == 1 && !model::guard_bound);
            assert(model::descriptors.size() == before.size());
            for (const auto& fd : before)
                assert(model::descriptors.at(fd.first).tag == fd.second.tag &&
                       model::descriptors.at(fd.first).cloexec == fd.second.cloexec);
            no_owned_fds();
        }
    for (int stage = 1; stage <= 2; ++stage) {
        diagnostic_setup();
        model::duplicate_fail_at = stage;
        diagnostic_expect(EMFILE);
        assert(!model::spawn_count && !model::guard_bound);
        no_owned_fds();
    }
    for (int stage = 1; stage <= 7; ++stage) {
        diagnostic_setup();
        model::action_fail_at = stage;
        diagnostic_expect(ENOMEM);
        assert(!model::spawn_count && !model::guard_bound);
        no_owned_fds();
    }
    diagnostic_setup();
    model::spawn_error = EAGAIN;
    diagnostic_expect(EAGAIN);
    no_owned_fds();
    assert(!model::guard_bound);
    diagnostic_setup();
    model::socket_error = EMFILE;
    diagnostic_expect(EMFILE);
    no_owned_fds();
    diagnostic_setup();
    model::guard_socket_error = EMFILE;
    diagnostic_expect(EMFILE);
    no_owned_fds();
    diagnostic_setup();
    model::bind_error = EADDRINUSE;
    diagnostic_expect(EBUSY);
    assert(!model::spawn_count);
    no_owned_fds();
    diagnostic_setup();
    model::bind_error = EACCES;
    diagnostic_expect(EACCES);
    assert(!model::spawn_count);
    no_owned_fds();
    for (const auto& invalid : std::vector<std::string>{
             "{}", "[]", diagnostic_request + " {}", std::string(4097, ' '),
             R"({"schema_version":1,"scenario":"boot-protection","scenario":"boot-protection"})",
             R"({"schema_version":1,"scenario":"maps-failure","path":"/x"})",
             R"({"schema_version":2,"scenario":"boot-protection"})", std::string(1, char(0xff)),
             std::string("{}\0", 3)}) {
        diagnostic_setup();
        assert(json::parse(api::request_diagnostics(invalid, 80)).at("errno").n == EINVAL);
        assert(!model::spawn_count && !model::guard_bound);
        no_owned_fds();
    }
    diagnostic_setup();
    diagnostic_expect(EINVAL, 0);
    assert(!model::spawn_count);
    no_owned_fds();
    diagnostic_setup();
    diagnostic_expect(EINVAL, 8001);
    assert(!model::spawn_count);
    no_owned_fds();
    diagnostic_setup();
    auto exact_request = diagnostic_request;
    exact_request.resize(4096, ' ');
    model::expected_hide = {"doctor", "manager", exact_request};
    assert(json::parse(api::request_diagnostics(exact_request, 80)).at("ok").b);
    diagnostic_setup();
    assert(json::parse(api::request("/ksud", 80, api::Service::Diagnostics)).at("errno").n ==
           EINVAL);
    assert(!model::spawn_count);
    const auto diagnostic_response = [&](const std::string& body) {
        for (std::size_t at = 0; at < body.size(); at += 4096)
            model::replies.push_back({2, body.substr(at, 4096)});
        model::replies.push_back({3});
        return api::request_diagnostics(diagnostic_request, 8000);
    };
    for (const auto& invalid :
         std::vector<std::string>{"{", "[]", "{} {}", "{\n}", "{\"a\":0,\"a\":0}",
                                  std::string("{\"a\":\"") + char(0xff) + "\"}"}) {
        diagnostic_setup();
        assert(json::parse(diagnostic_response(invalid)).at("errno").n == EPROTO);
    }
    auto diagnostic_heavy = json::Value::object();
    diagnostic_heavy["value"] = std::string((::sumhp::diagnostics::response_bytes - 12) / 2, '\\');
    const auto diagnostic_raw = json::dump(diagnostic_heavy);
    assert(diagnostic_raw.size() == 65536);
    diagnostic_setup();
    const auto diagnostic_envelope = diagnostic_response(diagnostic_raw);
    assert(diagnostic_envelope.size() <= 131328 &&
           json::parse(diagnostic_envelope).at("stdout").s == diagnostic_raw);
    diagnostic_setup();
    assert(json::parse(diagnostic_response(diagnostic_raw + " ")).at("errno").n == EOVERFLOW);
    diagnostic_setup();
    model::replies = {{2, "{}"}};
    diagnostic_expect(EIO);
    diagnostic_setup();
    model::child_status = 1;
    diagnostic_expect(EIO);
    diagnostic_setup();
    model::replies = {{2, "{\"partial\":"}, {3}};
    diagnostic_expect(EPROTO);
    diagnostic_setup();
    model::initial_ready = false;
    diagnostic_expect(ETIMEDOUT);
    assert(model::child_guard && api::pending.active && !model::ack_count);
    diagnostic_expect(EBUSY);
    assert(model::spawn_count == 1);
    model::incoming.push_back({1, {}, 0, 81});
    model::initial_ready = true;
    diagnostic_expect(0);
    assert(model::spawn_count == 2 && model::ack_count == 1 && !model::killed.empty());
    diagnostic_setup();
    model::no_reply = model::blocked = true;
    diagnostic_expect(ETIMEDOUT);
    assert(model::child_guard && model::ack_count == 1 && model::killed.size() == 1);
    diagnostic_expect(EBUSY);
    // A new CLI has no in-process quarantine, but an inherited descendant guard
    // still refuses admission before another spawn.
    api::release_pending();
    diagnostic_expect(EBUSY);
    assert(model::spawn_count == 1);
    model::child_guard = model::guard_bound = false;
    model::blocked = model::no_reply = false;
    diagnostic_expect(0);
    diagnostic_setup();
    model::send_error = EPIPE;
    diagnostic_expect(EPIPE);
    assert(model::killed.size() == 1);
    diagnostic_setup();
    model::recv_error = EIO;
    diagnostic_expect(EIO);
    assert(!model::ack_count);
    diagnostic_setup();
    model::wait_error = ECHILD;
    diagnostic_expect(ECHILD);
    assert(!api::pending.active);
    diagnostic_setup();
    model::tick_hook = [&] { diagnostic_expect(EBUSY); };
    model::no_reply = true;
    diagnostic_expect(ETIMEDOUT);
    const auto diagnostic_child = [&] {
        diagnostic_setup();
        model::child_mode = true;
        model::descriptors[0] = {2, false};
        model::descriptors[3] = {3, false};
    };
    for (int fault = 0; fault < 8; ++fault) {
        diagnostic_child();
        callbacks = 0;
        if (fault == 0)
            model::guard_socket_error = EBADF;
        if (fault == 1)
            model::guard_type = SOCK_SEQPACKET;
        if (fault == 2)
            model::guard_name_error = EBADF;
        if (fault == 3)
            model::guard_family = 0;
        if (fault == 4)
            model::guard_name = "sumh.maps.preview.sample.v1";
        if (fault == 5)
            model::guard_length_delta = 1;
        if (fault == 6)
            model::guard_length_delta = -1;
        if (fault == 7)
            model::guard_fcntl_error = EIO;
        assert(api::serve(
                   [&] {
                       ++callbacks;
                       return "{}";
                   },
                   api::Service::Diagnostics) == 1 &&
               !callbacks && model::sent.empty());
        assert(!model::descriptors.count(3));
    }
    diagnostic_child();
    model::automatic_ack = false;
    callbacks = 0;
    assert(api::serve(
               [&] {
                   ++callbacks;
                   return "{}";
               },
               api::Service::Diagnostics) == 1 &&
           !callbacks);
    diagnostic_child();
    model::dead = true;
    model::automatic_ack = false;
    callbacks = 0;
    assert(api::serve(
               [&] {
                   ++callbacks;
                   return "{}";
               },
               api::Service::Diagnostics) == 1 &&
           !callbacks);
    diagnostic_child();
    model::pidfd_error = ENOSYS;
    callbacks = 0;
    assert(api::serve(
               [&] {
                   ++callbacks;
                   return "{}";
               },
               api::Service::Diagnostics) == 1 &&
           !callbacks);
    diagnostic_child();
    model::automatic_ack = false;
    model::incoming.push_back({4, {}, 0, 82});
    callbacks = 0;
    assert(api::serve(
               [&] {
                   ++callbacks;
                   return "{}";
               },
               api::Service::Diagnostics) == 1 &&
           !callbacks);
    diagnostic_child();
    model::send_error = EPIPE;
    callbacks = 0;
    assert(api::serve(
               [&] {
                   ++callbacks;
                   return "{}";
               },
               api::Service::Diagnostics) == 1 &&
           !callbacks);
    diagnostic_child();
    callbacks = 0;
    assert(api::serve(
               [&] {
                   ++callbacks;
                   assert(model::descriptors.at(3).cloexec && model::incoming.empty() &&
                          model::sent.size() == 1 && model::sent[0].kind == 1);
                   return diagnostic_raw;
               },
               api::Service::Diagnostics) == 0 &&
           callbacks == 1);
    assert(!model::descriptors.count(3) && model::sent.front().kind == 1 &&
           model::sent.back().kind == 3);
    for (const auto& invalid : std::vector<std::string>{
             "{", "[]", "{} {}", "{\n}", "{\"a\":0,\"a\":0}",
             std::string("{\"a\":\"") + char(0xff) + "\"}", diagnostic_raw + " "}) {
        diagnostic_child();
        assert(api::serve([&] { return invalid; }, api::Service::Diagnostics) == 1);
        assert(model::sent.size() == 2 && model::sent.back().error && model::sent.back().kind == 3);
    }
    std::cout << (tests - diagnostics_start)
              << " Diagnostics transport and inherited-FD fault checks passed\n";
    const int packages_start = tests;
    const std::string package_request = R"({"schema_version":1,"action":"list"})";
    const auto package_setup = [&](unsigned closed_stdio = 0, bool occupied_three = false) {
        diagnostic_setup(closed_stdio, occupied_three);
        model::package_mode = true;
        model::guard_name = "sumh.doctor.package.v1";
        model::expected_hide = {"doctor", "package", "manager", package_request};
    };
    const auto package_expect = [&](int error, unsigned ms = 80) {
        const auto value =
            json::parse(api::request_diagnostic_packages("/ksud", package_request, ms));
        assert(value.at("errno").n == error && value.at("ok").b == (error == 0));
        if (error)
            assert(value.at("stdout").s.empty());
    };
    for (unsigned closed = 0; closed < 8; ++closed)
        for (bool occupied : {false, true}) {
            package_setup(closed, occupied);
            const auto before = model::descriptors;
            package_expect(0);
            assert(model::spawn_count == 1 && model::ack_count == 1 && !model::guard_bound);
            assert(model::descriptors.size() == before.size());
            for (const auto& fd : before)
                assert(model::descriptors.at(fd.first).tag == fd.second.tag &&
                       model::descriptors.at(fd.first).cloexec == fd.second.cloexec);
        }
    for (int stage = 1; stage <= 2; ++stage) {
        package_setup();
        model::duplicate_fail_at = stage;
        package_expect(EMFILE);
        assert(!model::spawn_count && !model::guard_bound);
        no_owned_fds();
    }
    for (int stage = 1; stage <= 7; ++stage) {
        package_setup();
        model::action_fail_at = stage;
        package_expect(ENOMEM);
        assert(!model::spawn_count && !model::guard_bound);
        no_owned_fds();
    }
    package_setup();
    model::spawn_error = EAGAIN;
    package_expect(EAGAIN);
    no_owned_fds();
    package_setup();
    model::socket_error = EMFILE;
    package_expect(EMFILE);
    no_owned_fds();
    package_setup();
    model::guard_socket_error = EMFILE;
    package_expect(EMFILE);
    no_owned_fds();
    package_setup();
    model::bind_error = EADDRINUSE;
    package_expect(EBUSY);
    assert(!model::spawn_count);
    no_owned_fds();
    for (const auto& invalid :
         std::vector<std::string>{"{}", "[]", package_request + " {}", std::string(8193, ' '),
                                  R"({"schema_version":1,"action":"list","action":"list"})",
                                  R"({"schema_version":1,"action":"list","path":"/secret"})",
                                  R"({"schema_version":2,"action":"list"})",
                                  std::string(1, char(0xff)), std::string("{}\0", 3)}) {
        package_setup();
        assert(json::parse(api::request_diagnostic_packages("/ksud", invalid, 80)).at("errno").n ==
               EINVAL);
        assert(!model::spawn_count && !model::guard_bound);
        no_owned_fds();
    }
    package_setup();
    package_expect(EINVAL, 0);
    assert(!model::spawn_count);
    package_setup();
    package_expect(EINVAL, 8001);
    assert(!model::spawn_count);
    package_setup();
    assert(json::parse(api::request("/ksud", 80, api::Service::DiagnosticPackages)).at("errno").n ==
           EINVAL);
    assert(!model::spawn_count);
    const auto package_response = [&](const std::string& body) {
        for (std::size_t at = 0; at < body.size(); at += 4096)
            model::replies.push_back({2, body.substr(at, 4096)});
        model::replies.push_back({3});
        return api::request_diagnostic_packages("/ksud", package_request, 8000);
    };
    auto package_heavy = json::Value::object();
    package_heavy["value"] =
        std::string((::sumhp::diagnostic_package_manager::response_bytes - 12) / 2, '\\');
    const auto package_raw = json::dump(package_heavy);
    assert(package_raw.size() == 208U * 1024);
    package_setup();
    const auto package_envelope = package_response(package_raw);
    assert(package_envelope.size() <= 432U * 1024 &&
           json::parse(package_envelope).at("stdout").s == package_raw);
    package_setup();
    assert(json::parse(package_response(package_raw + " ")).at("errno").n == EOVERFLOW);
    for (const auto& invalid :
         std::vector<std::string>{"{", "[]", "{} {}", "{\n}", "{\"a\":0,\"a\":0}",
                                  std::string("{\"a\":\"") + char(0xff) + "\"}"}) {
        package_setup();
        assert(json::parse(package_response(invalid)).at("errno").n == EPROTO);
    }
    package_setup();
    model::replies = {{2, "{\"partial\":"}};
    package_expect(EIO);
    package_setup();
    model::initial_ready = false;
    package_expect(ETIMEDOUT);
    package_expect(EBUSY);
    assert(model::child_guard && model::spawn_count == 1 && !model::ack_count);
    package_setup();
    model::no_reply = model::blocked = true;
    package_expect(ETIMEDOUT);
    package_expect(EBUSY);
    assert(model::child_guard && model::ack_count == 1 && model::spawn_count == 1 &&
           model::killed.size() == 1);
    api::release_pending();
    package_expect(EBUSY);
    assert(model::spawn_count == 1);
    model::child_guard = model::guard_bound = model::blocked = model::no_reply = false;
    package_expect(0);
    const auto package_child = [&] {
        package_setup();
        model::child_mode = true;
        model::descriptors[0] = {2, false};
        model::descriptors[3] = {3, false};
    };
    package_child();
    callbacks = 0;
    assert(api::serve(
               [&] {
                   ++callbacks;
                   assert(api::package_guard_for_sampling == 3 &&
                          model::descriptors.at(3).cloexec && model::sent.size() == 1);
                   return package_raw;
               },
               api::Service::DiagnosticPackages) == 0 &&
           callbacks == 1 && api::package_guard_for_sampling == -1);
    assert(!model::descriptors.count(3));
    package_child();
    model::guard_name = "sumh.doctor.sample.v1";
    callbacks = 0;
    assert(api::serve(
               [&] {
                   ++callbacks;
                   return "{}";
               },
               api::Service::DiagnosticPackages) == 1 &&
           !callbacks && model::sent.empty());
    package_child();
    model::automatic_ack = false;
    callbacks = 0;
    assert(api::serve(
               [&] {
                   ++callbacks;
                   return "{}";
               },
               api::Service::DiagnosticPackages) == 1 &&
           !callbacks);
    package_child();
    assert(api::serve([&] { return package_raw + " "; }, api::Service::DiagnosticPackages) == 1 &&
           model::sent.back().error == EOVERFLOW);
    // A nested doctor must inherit BOTH reservations through its exec, with
    // every source promoted above fd4. Parent descriptors remain CLOEXEC.
    const auto nested_setup = [&](unsigned closed_stdio = 0) {
        diagnostic_setup(closed_stdio);
        model::nested_package = true;
        model::descriptors[3] = {4, true};
        api::package_guard_for_sampling = 3;
    };
    for (unsigned closed = 0; closed < 8; ++closed) {
        nested_setup(closed);
        diagnostic_expect(0);
        assert(model::descriptors.at(3).tag == 4 && model::descriptors.at(3).cloexec &&
               api::package_guard_for_sampling == 3);
    }
    for (int stage = 1; stage <= 3; ++stage) {
        nested_setup(7);
        model::duplicate_fail_at = stage;
        diagnostic_expect(EMFILE);
        assert(!model::spawn_count && model::descriptors.size() == 1 &&
               model::descriptors.at(3).tag == 4);
    }
    for (int stage = 1; stage <= 9; ++stage) {
        nested_setup(7);
        model::action_fail_at = stage;
        diagnostic_expect(ENOMEM);
        assert(!model::spawn_count && model::descriptors.size() == 1 &&
               model::descriptors.at(3).tag == 4);
    }
    diagnostic_child();
    model::descriptors[4] = {4, false};
    callbacks = 0;
    assert(api::serve(
               [&] {
                   ++callbacks;
                   assert(api::package_guard_for_sampling == 4 && model::descriptors.at(4).cloexec);
                   return "{}";
               },
               api::Service::Diagnostics) == 0 &&
           callbacks == 1 && api::package_guard_for_sampling == -1);
    assert(!model::descriptors.count(3) && !model::descriptors.count(4));
    diagnostic_child();
    model::descriptors[4] = {9, false};
    assert(api::serve(
               [&] {
                   assert(api::package_guard_for_sampling == -1);
                   return "{}";
               },
               api::Service::Diagnostics) == 0);
    assert(model::descriptors.at(4).tag == 9 &&
           !model::descriptors.at(4).cloexec);  // Unrelated fd4 untouched.
    std::cout << (tests - packages_start)
              << " diagnostic package transport/nested-reservation fault checks passed\n";
    reset();
    const auto fd = api::acquire_preview_guard();
    assert(fd == 91);
    assert(api::acquire_preview_guard() == -1 && errno == EBUSY);
    reset();
    model::bind_error = EACCES;
    assert(api::acquire_preview_guard() == -1 && errno == EACCES);
    api::release_pending();
    std::cout << tests << " production preview transport fault-model checks passed\n";
}
