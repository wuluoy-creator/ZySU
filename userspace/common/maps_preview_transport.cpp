#include "maps_preview_transport.hpp"
#include "../ksud/sumhp/src/core/operation_manager_input.hpp"
#include "../ksud/sumhp/src/core/user_hide_input.hpp"
#include "json.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>

namespace sumhp::preview_transport {
namespace {
constexpr std::uint32_t magic = 0x53504d31;  // Private Maps preview protocol v1.
constexpr std::uint32_t ready_kind = 1, data_kind = 2, done_kind = 3, ack_kind = 4;
constexpr std::size_t chunk_limit = 4096, response_limit = std::size_t{512} * 1024;
std::size_t raw_limit(Service service) {
    if (service == Service::DiagnosticPackages)
        return diagnostic_package_manager::response_bytes;
    if (service == Service::Diagnostics)
        return diagnostics::response_bytes;
    if (service == Service::TemporaryMaps)
        return temporary_maps::response_bytes;
    return service == Service::ModulePreview ? module_preview::response_bytes : response_limit;
}
bool module_response_safe(const std::string& output) {
    // Compact production JSON has no raw controls. This makes the legacy
    // stdout-string envelope expansion at most 2x plus fixed punctuation.
    return std::none_of(output.begin(), output.end(), [](unsigned char c) { return c < 0x20; });
}

constexpr char diagnostics_guard_name[] = "sumh.doctor.sample.v1";
constexpr char package_guard_name[] = "sumh.doctor.package.v1";
class OwnedDescriptor {
public:
    explicit OwnedDescriptor(int value = -1) : fd_(value) {}
    ~OwnedDescriptor() {
        const int error = errno;
        if (fd_ >= 0)
            ::close(fd_);
        errno = error;
    }
    OwnedDescriptor(const OwnedDescriptor&) = delete;
    OwnedDescriptor& operator=(const OwnedDescriptor&) = delete;
    OwnedDescriptor(OwnedDescriptor&&) = delete;
    OwnedDescriptor& operator=(OwnedDescriptor&&) = delete;
    [[nodiscard]] int get() const { return fd_; }

private:
    int fd_;
};

int acquire_named_guard(const char* name, std::size_t name_size) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path + 1, name, name_size - 1);
    const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + name_size);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), length)) {
        const int error = errno == EADDRINUSE ? EBUSY : errno;
        ::close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

int acquire_diagnostics_guard() {
    return acquire_named_guard(diagnostics_guard_name, sizeof(diagnostics_guard_name));
}
int acquire_package_guard() {
    return acquire_named_guard(package_guard_name, sizeof(package_guard_name));
}
int acquire_service_guard(Service service) {
    if (service == Service::Diagnostics)
        return acquire_diagnostics_guard();
    if (service == Service::DiagnosticPackages)
        return acquire_package_guard();
    return -1;
}

bool matches_guard(int fd, const char* name, std::size_t name_size) {
    int type = 0;
    socklen_t type_length = sizeof(type);
    sockaddr_un address{};
    socklen_t length = sizeof(address);
    return ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_length) == 0 &&
           type_length == sizeof(type) && type == SOCK_STREAM &&
           ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0 &&
           length == static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + name_size) &&
           address.sun_family == AF_UNIX && address.sun_path[0] == '\0' &&
           std::memcmp(address.sun_path + 1, name, name_size - 1) == 0;
}
bool valid_diagnostics_guard(int fd) {
    return matches_guard(fd, diagnostics_guard_name, sizeof(diagnostics_guard_name)) &&
           ::fcntl(fd, F_SETFD, FD_CLOEXEC) >= 0;
}
// Set only inside an authenticated package server (or its diagnostic child).
// Explicit spawn file actions carry it across the nested doctor exec; forked
// sampling workers then inherit it even after their package parent is killed.
thread_local int package_guard_for_sampling = -1;

bool diagnostics_response_safe(const std::string& output) {
    json::Value parsed;
    std::string error;
    return module_response_safe(output) && ksud::text::valid_utf8(output) &&
           json::Parser(output, true).parse(parsed, error) && parsed.is_object();
}
constexpr std::int64_t tick_ns = 10'000'000;
struct Header {
    std::uint32_t magic_value, kind, size, error;
};
struct Frame {
    Header header{};
    std::array<char, chunk_limit> body{};
};
struct Pending {
    bool active = false;
    int socket = -1;
    int pidfd = -1;
};
std::mutex request_mutex;
Pending pending;

bool now_ns(std::int64_t& value) {
    struct timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now))
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

bool deadline_after(unsigned milliseconds, std::int64_t& deadline) {
    std::int64_t value;
    if (!now_ns(value))
        return false;
    const auto budget = static_cast<std::int64_t>(milliseconds) * 1'000'000;
    if (value > INT64_MAX - budget) {
        errno = EOVERFLOW;
        return false;
    }
    deadline = value + budget;
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

bool pause_once(std::int64_t deadline, std::int64_t now) {
    const struct timespec delay{0, static_cast<long>(std::min(tick_ns, deadline - now))};
    return ::nanosleep(&delay, nullptr) == 0 || errno == EINTR;
}

void release_pending() {
    if (pending.socket >= 0)
        ::close(pending.socket);
    if (pending.pidfd >= 0)
        ::close(pending.pidfd);
    pending = {};
}

void cancel(int fd) {
#ifdef SYS_pidfd_send_signal
    if (fd >= 0)
        (void)::syscall(SYS_pidfd_send_signal, fd, SIGKILL, nullptr, 0);
#else
    (void)fd;
#endif
}

int reap(siginfo_t& status) {
    if (pending.pidfd < 0)
        return 0;
    // Linux's P_PIDFD ABI value, including builds with older libc headers.
    if (::waitid(static_cast<idtype_t>(3), static_cast<id_t>(pending.pidfd), &status,
                 WEXITED | WNOHANG))
        return -1;
    return status.si_pid ? 1 : 0;
}

// 1 frame; 0 would-block/EINTR; -1 invalid/transport; -2 peer closed.
int receive(int socket, Frame& frame, int& descriptor) {
    struct iovec data{&frame, sizeof(frame)};
    alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int) * 2)]{};
    struct msghdr message{};
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
    // Linux may echo MSG_CMSG_CLOEXEC even for EOF; it is not a payload flag.
    if (!count && descriptor < 0 && message.msg_controllen == 0 &&
        (message.msg_flags & ~MSG_CMSG_CLOEXEC) == 0)
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
    Frame frame;
    frame.header = {magic, kind, static_cast<std::uint32_t>(size),
                    static_cast<std::uint32_t>(error)};
    if (size)
        std::memcpy(frame.body.data(), bytes, size);
    struct iovec data{&frame, sizeof(Header) + size};
    alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    struct msghdr message{};
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
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return false;
        if (!pause_once(deadline, previous))
            return false;
    }
}

void abandon() {
    cancel(pending.pidfd);
    if (pending.pidfd < 0)
        return;  // Retain socket for late handle, never ACK it.
    if (pending.socket >= 0)
        ::close(pending.socket);
    pending.socket = -1;
    siginfo_t status{};
    const auto waited = reap(status);
    if (waited == 1 || (waited < 0 && errno == ECHILD))
        release_pending();
}

bool collect_pending() {
    if (pending.pidfd < 0) {
        Frame frame;
        int fd = -1;
        const int received = pending.socket >= 0 ? receive(pending.socket, frame, fd) : -1;
        if (received == 1 && frame.header.kind == ready_kind && !frame.header.size &&
            !frame.header.error && fd >= 0) {
            pending.pidfd = fd;
            cancel(fd);  // A previously timed-out request must never start work.
        } else {
            if (fd >= 0)
                ::close(fd);
            if (received != 0 && pending.socket >= 0) {
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
    const auto waited = reap(status);
    if (waited == 1 || (waited < 0 && errno == ECHILD)) {
        release_pending();
        return true;
    }
    errno = EBUSY;
    return false;
}

bool spawn_preview(const std::string& executable, Service service, const std::string* revision,
                   const std::string* partitions_json, const std::vector<std::string>* hide_args,
                   int diagnostics_reservation = -1) {
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets))
        return false;
    // Keep sources above every fixed child target, even for closed stdio or an
    // original reservation at fd3. Never clear CLOEXEC on a parent descriptor.
    const bool diagnostics = service == Service::Diagnostics;
    const bool guarded = diagnostics || service == Service::DiagnosticPackages;
    const bool inherit_package = diagnostics && package_guard_for_sampling >= 0;
    int minimum_source = 3;
    if (inherit_package)
        minimum_source = 5;
    else if (guarded)
        minimum_source = 4;
    const int channel = ::fcntl(sockets[1], F_DUPFD_CLOEXEC, minimum_source);
    const int duplicate_error = errno;
    ::close(sockets[1]);
    if (channel < 0) {
        ::close(sockets[0]);
        errno = duplicate_error;
        return false;
    }
    const OwnedDescriptor guard_source(
        guarded ? ::fcntl(diagnostics_reservation, F_DUPFD_CLOEXEC, minimum_source) : -1);
    if (guarded && guard_source.get() < 0) {
        const int error = errno;
        ::close(channel);
        ::close(sockets[0]);
        errno = error;
        return false;
    }
    const OwnedDescriptor inherited_source(
        inherit_package ? ::fcntl(package_guard_for_sampling, F_DUPFD_CLOEXEC, minimum_source)
                        : -1);
    if (inherit_package && inherited_source.get() < 0) {
        const int error = errno;
        ::close(channel);
        ::close(sockets[0]);
        errno = error;
        return false;
    }
    posix_spawn_file_actions_t actions;
    int error = ::posix_spawn_file_actions_init(&actions);
    if (!error) {
        error = ::posix_spawn_file_actions_adddup2(&actions, channel, STDIN_FILENO);
        if (!error)
            error = ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                                       O_WRONLY, 0);
        if (!error)
            error = ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                                       O_WRONLY, 0);
        if (!error && guarded)
            error = ::posix_spawn_file_actions_adddup2(&actions, guard_source.get(), 3);
        if (!error && inherit_package)
            error = ::posix_spawn_file_actions_adddup2(&actions, inherited_source.get(), 4);
        if (!error)
            error = ::posix_spawn_file_actions_addclose(&actions, channel);
        if (!error && guarded)
            error = ::posix_spawn_file_actions_addclose(&actions, guard_source.get());
        if (!error && inherit_package)
            error = ::posix_spawn_file_actions_addclose(&actions, inherited_source.get());
        std::vector<std::string> args{diagnostics ? "ksud" : executable, "sumhp"};
        if (hide_args)
            args.insert(args.end(), hide_args->begin(), hide_args->end());
        else if (revision && partitions_json) {
            args.insert(args.end(), {"config", "set-partitions", *revision, *partitions_json});
        } else if (service == Service::PartitionDiscovery) {
            args.insert(args.end(), {"config", "discover-partitions"});
        } else if (service == Service::Capabilities) {
            args.insert(args.end(), {"api", "capabilities"});
        } else
            args.insert(args.end(), {"sumh", "maps", "preview"});
        args.push_back("--channel");
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& arg : args)
            argv.push_back(arg.data());
        argv.push_back(nullptr);
        pid_t unused_pid = -1;
        if (!error)
            error = ::posix_spawn(&unused_pid, executable.c_str(), &actions, nullptr, argv.data(),
                                  environ);
        ::posix_spawn_file_actions_destroy(&actions);
    }
    ::close(channel);
    if (error) {
        ::close(sockets[0]);
        errno = error;
        return false;
    }
    pending = {true, sockets[0], -1};
    return true;
}

std::string envelope(const std::string& output, int error) {
    auto object = json::Value::object();
    object["ok"] = error == 0;
    object["exit_code"] = error ? 1 : 0;
    object["errno"] = error;
    object["stdout"] = output;
    object["stderr"] =
        error ? "Preview transport failed (errno " + std::to_string(error) + ")" : "";
    return json::dump(object);
}
}  // namespace

std::string failure_envelope(int error) {
    return envelope({}, error > 0 && error <= 4095 ? error : EIO);
}

namespace {
std::string request_impl(const std::string& executable, unsigned timeout_ms, Service service,
                         const std::string* revision, const std::string* partitions_json,
                         const std::vector<std::string>* hide_args) {
    const std::unique_lock<std::mutex> lock(request_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return failure_envelope(EBUSY);
    if (executable.empty() || executable.front() != '/' || executable.size() > 4096 ||
        executable.find('\0') != std::string::npos || !timeout_ms || timeout_ms > 8000 ||
        (service != Service::Maps && service != Service::PartitionDiscovery &&
         service != Service::UserHide && service != Service::Operation &&
         service != Service::ModulePreview && service != Service::Capabilities &&
         service != Service::TemporaryMaps && service != Service::Diagnostics &&
         service != Service::DiagnosticPackages) ||
        ((service == Service::UserHide || service == Service::Operation ||
          service == Service::ModulePreview || service == Service::TemporaryMaps ||
          service == Service::Diagnostics || service == Service::DiagnosticPackages) !=
         static_cast<bool>(hide_args)))
        return failure_envelope(EINVAL);
    if (pending.active && !collect_pending())
        return failure_envelope(EBUSY);
    std::int64_t deadline;
    std::int64_t previous = 0;
    if (!deadline_after(timeout_ms, deadline))
        return failure_envelope(errno);
    const bool guarded = service == Service::Diagnostics || service == Service::DiagnosticPackages;
    const OwnedDescriptor reservation(acquire_service_guard(service));
    if (guarded && reservation.get() < 0)
        return failure_envelope(errno);
    if (!spawn_preview(executable, service, revision, partitions_json, hide_args,
                       reservation.get()))
        return failure_envelope(errno);
    bool ready = false;
    bool done = false;
    bool exited = false;
    siginfo_t status{};
    std::string output;
    const auto fail = [&](int error) {
        abandon();
        return failure_envelope(error);
    };
    for (;;) {
        if (!within_deadline(deadline, previous))
            return fail(errno);
        // A bounded batch allows continuous EINTR/data to obey the same deadline.
        bool drained = false;
        for (unsigned frames = 0; frames < 8; ++frames) {
            Frame frame;
            int fd = -1;
            const int got = receive(pending.socket, frame, fd);
            if (got == 0 || got == -2) {
                drained = true;
                break;
            }
            if (got < 0)
                return fail(errno);
            if (frame.header.kind == ready_kind && !ready && !done && !frame.header.size &&
                !frame.header.error && fd >= 0) {
                pending.pidfd = fd;
                ready = true;
                if (!send_frame(pending.socket, ack_kind, nullptr, 0, -1, 0, deadline))
                    return fail(errno);
            } else if (frame.header.kind == data_kind && ready && !done && fd < 0 &&
                       frame.header.size && !frame.header.error) {
                if (frame.header.size > raw_limit(service) - output.size())
                    return fail(EOVERFLOW);
                output.append(frame.body.data(), frame.header.size);
            } else if (frame.header.kind == done_kind && !done && fd < 0 && !frame.header.size &&
                       (ready || frame.header.error)) {
                if (frame.header.error)
                    return fail(static_cast<int>(frame.header.error));
                done = true;
            } else {
                if (fd >= 0)
                    ::close(fd);
                return fail(EPROTO);
            }
        }
        if (exited && drained) {
            // POSIX waitid exposes child status through siginfo_t's tagged ABI union.
            // Read si_status only after the CLD_EXITED discriminator matches.
            const bool success =
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
                ready && done && status.si_code == CLD_EXITED && status.si_status == 0;
            release_pending();
            if (!success)
                return failure_envelope(EIO);
            json::Value parsed;
            std::string error;
            if (((service == Service::Diagnostics || service == Service::DiagnosticPackages) &&
                 !diagnostics_response_safe(output)) ||
                ((service == Service::ModulePreview || service == Service::TemporaryMaps) &&
                 !module_response_safe(output)) ||
                (service == Service::TemporaryMaps && !ksud::text::valid_utf8(output)) ||
                !json::Parser(output, service == Service::TemporaryMaps).parse(parsed, error) ||
                !parsed.is_object())
                return failure_envelope(EPROTO);
            auto result = envelope(output, 0);
            if (service == Service::TemporaryMaps && result.size() > temporary_maps::envelope_bytes)
                return failure_envelope(EOVERFLOW);
            if (service == Service::Diagnostics && result.size() > diagnostics::envelope_bytes)
                return failure_envelope(EOVERFLOW);
            if (service == Service::DiagnosticPackages &&
                result.size() > diagnostic_package_manager::envelope_bytes)
                return failure_envelope(EOVERFLOW);
            return result;
        }
        if (!exited) {
            const auto waited = reap(status);
            if (waited == 1)
                exited = true;
            else if (waited < 0 && errno != EINTR) {
                const int error = errno;
                if (error == ECHILD) {
                    release_pending();
                    return failure_envelope(error);
                }
                return fail(error);
            }
        }
        // Drain queued frames immediately after confirmed exit; never wait again
        // on this process. Forked samplers may still hold an inherited socket.
        if (exited && !drained)
            continue;
        if (!pause_once(deadline, previous))
            return fail(errno);
    }
}
}  // namespace

std::string request(const std::string& executable, unsigned timeout_ms, Service service) {
    return request_impl(executable, timeout_ms, service, nullptr, nullptr, nullptr);
}

std::string request_partitions_save(const std::string& executable, const std::string& revision,
                                    const std::string& partitions_json, unsigned timeout_ms) {
    if (revision.size() != 74 || revision.compare(0, 10, "sha256-v1:") != 0 ||
        !std::all_of(revision.begin() + 10, revision.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
        partitions_json.empty() || partitions_json.size() > 40UL * 1024 ||
        partitions_json.find('\0') != std::string::npos)
        return failure_envelope(EINVAL);
    return request_impl(executable, timeout_ms, Service::PartitionDiscovery, &revision,
                        &partitions_json, nullptr);
}

std::string request_user_hide(const std::string& executable, const std::string& arguments_json,
                              unsigned timeout_ms) {
    std::vector<std::string> args;
    ::sumhp::hide::Request request;
    std::string error;
    if (!::sumhp::hide::parse_manager_input(arguments_json, args, request, error))
        return failure_envelope(EINVAL);
    return request_impl(executable, timeout_ms, Service::UserHide, nullptr, nullptr, &args);
}
std::string request_operation(const std::string& executable, const std::string& request_json,
                              unsigned timeout_ms) {
    ::sumhp::state::OperationManagerRequest request;
    std::string error;
    if (!::sumhp::state::parse_operation_manager_input(request_json, request, error))
        return failure_envelope(EINVAL);
    const std::vector<std::string> args{"operation", "manager", request_json};
    return request_impl(executable, timeout_ms, Service::Operation, nullptr, nullptr, &args);
}

std::string request_module_preview(const std::string& executable,
                                   const module_preview::Input& input, unsigned timeout_ms) {
    if (!module_preview::valid(input))
        return failure_envelope(EINVAL);
    const auto args = module_preview::arguments(input);
    return request_impl(executable, timeout_ms, Service::ModulePreview, nullptr, nullptr, &args);
}

std::string request_temporary_maps(const std::string& executable, const std::string& request_json,
                                   unsigned timeout_ms) {
    temporary_maps::Request request;
    if (!temporary_maps::parse(request_json, request))
        return failure_envelope(EINVAL);
    const std::vector<std::string> args{"sumh", "maps", "manager", request_json};
    return request_impl(executable, timeout_ms, Service::TemporaryMaps, nullptr, nullptr, &args);
}

std::string request_diagnostics(const std::string& request_json, unsigned timeout_ms) {
    diagnostics::Request request;
    std::string reason;
    if (!diagnostics::parse(request_json, request, reason))
        return failure_envelope(EINVAL);
    const std::vector<std::string> args{"doctor", "manager", request_json};
    return request_impl("/proc/self/exe", timeout_ms, Service::Diagnostics, nullptr, nullptr,
                        &args);
}

std::string request_diagnostic_packages(const std::string& executable,
                                        const std::string& request_json, unsigned timeout_ms) {
    diagnostic_package_manager::Request request;
    if (!diagnostic_package_manager::parse(request_json, request))
        return failure_envelope(EINVAL);
    const std::vector<std::string> args{"doctor", "package", "manager", request_json};
    return request_impl(executable, timeout_ms, Service::DiagnosticPackages, nullptr, nullptr,
                        &args);
}

int serve(const std::function<std::string()>& preview, Service service) {
    const OwnedDescriptor reservation(
        service == Service::Diagnostics || service == Service::DiagnosticPackages ? 3 : -1);
    if (service == Service::Diagnostics && !valid_diagnostics_guard(reservation.get()))
        return 1;
    if (service == Service::DiagnosticPackages &&
        (!matches_guard(reservation.get(), package_guard_name, sizeof(package_guard_name)) ||
         ::fcntl(reservation.get(), F_SETFD, FD_CLOEXEC) < 0))
        return 1;
    // An unrelated fd4 on an ordinary doctor invocation is neither closed nor
    // changed. Only the exact named inherited package reservation is adopted.
    const OwnedDescriptor inherited_package(
        service == Service::Diagnostics &&
                matches_guard(4, package_guard_name, sizeof(package_guard_name))
            ? 4
            : -1);
    if (inherited_package.get() >= 0 && ::fcntl(inherited_package.get(), F_SETFD, FD_CLOEXEC) < 0)
        return 1;
    class PackageGuardScope {
    public:
        explicit PackageGuardScope(int fd) : previous_(package_guard_for_sampling) {
            package_guard_for_sampling = fd;
        }
        ~PackageGuardScope() { package_guard_for_sampling = previous_; }
        PackageGuardScope(const PackageGuardScope&) = delete;
        PackageGuardScope& operator=(const PackageGuardScope&) = delete;
        PackageGuardScope(PackageGuardScope&&) = delete;
        PackageGuardScope& operator=(PackageGuardScope&&) = delete;

    private:
        int previous_;
    };
    const PackageGuardScope package_scope(
        service == Service::DiagnosticPackages ? reservation.get() : inherited_package.get());
    constexpr int socket = STDIN_FILENO;
    int type = 0;
    socklen_t size = sizeof(type);
    if (::getsockopt(socket, SOL_SOCKET, SO_TYPE, &type, &size) || type != SOCK_SEQPACKET ||
        ::fcntl(socket, F_SETFD, FD_CLOEXEC) < 0)
        return 1;
    std::int64_t deadline;
    std::int64_t previous = 0;
    if (!deadline_after(8000, deadline))
        return 1;
    int pidfd = -1;
#ifdef SYS_pidfd_open
    pidfd = static_cast<int>(::syscall(SYS_pidfd_open, ::getpid(), 0));
#else
    errno = ENOSYS;
#endif
    if (pidfd < 0) {
        (void)send_frame(socket, done_kind, nullptr, 0, -1, errno ? errno : EIO, deadline);
        return 1;
    }
    const bool sent = send_frame(socket, ready_kind, nullptr, 0, pidfd, 0, deadline);
    ::close(pidfd);
    if (!sent)
        return 1;
    for (;;) {
        if (!within_deadline(deadline, previous))
            return 1;
        Frame ack;
        int fd = -1;
        const int got = receive(socket, ack, fd);
        if (fd >= 0)
            ::close(fd);
        if (got == 1) {
            if (fd >= 0 || ack.header.kind != ack_kind || ack.header.size || ack.header.error)
                return 1;
            break;
        }
        if (got < 0 || !pause_once(deadline, previous))
            return 1;
    }
    std::string output;
    int error = 0;
    try {
        output = preview();
    } catch (...) {
        error = EIO;
    }
    if (!error && (output.empty() || output.size() > raw_limit(service)))
        error = EOVERFLOW;
    if (!error && (service == Service::ModulePreview || service == Service::TemporaryMaps) &&
        !module_response_safe(output))
        error = EPROTO;
    if (!error && (service == Service::Diagnostics || service == Service::DiagnosticPackages) &&
        !diagnostics_response_safe(output))
        error = EPROTO;
    if (!error && service == Service::Diagnostics &&
        envelope(output, 0).size() > diagnostics::envelope_bytes)
        error = EOVERFLOW;
    if (!error && service == Service::DiagnosticPackages &&
        envelope(output, 0).size() > diagnostic_package_manager::envelope_bytes)
        error = EOVERFLOW;
    if (!error) {
        for (std::size_t offset = 0; offset < output.size(); offset += chunk_limit) {
            const auto count = std::min(chunk_limit, output.size() - offset);
            if (!send_frame(socket, data_kind, output.data() + offset, count, -1, 0, deadline))
                return 1;
        }
    }
    return send_frame(socket, done_kind, nullptr, 0, -1, error, deadline) && !error ? 0 : 1;
}

int acquire_preview_guard() {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un address{};
    address.sun_family = AF_UNIX;
    constexpr char name[] = "sumh.maps.preview.sample.v1";
    std::memcpy(address.sun_path + 1, name, sizeof(name) - 1);
    const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + sizeof(name));
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&address), length)) {
        const int error = errno == EADDRINUSE ? EBUSY : errno;
        ::close(fd);
        errno = error;
        return -1;
    }
    return fd;
}
}  // namespace sumhp::preview_transport
