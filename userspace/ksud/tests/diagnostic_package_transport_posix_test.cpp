// Linux-only package -> doctor -> forked sampler lifetime smoke using the
// production transport. Pipes establish lifecycle windows without sleeps.
#include <poll.h>
#include <sys/prctl.h>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include "userspace/common/maps_preview_transport.cpp"

namespace {
namespace transport = sumhp::preview_transport;
constexpr const char* package_request = R"({"schema_version":1,"action":"list"})";
constexpr const char* doctor_request = R"({"schema_version":1,"scenario":"boot-protection"})";
struct Event {
    char kind;
    pid_t pid;
    int result;
};
int event_fd = -1, package_gate = -1, doctor_gate = -1, sampler_gate = -1;
std::string mode = "probe", executable;

void emit(char kind, int result = 0) {
    const Event event{kind, getpid(), result};
    assert(write(event_fd, &event, sizeof(event)) == static_cast<ssize_t>(sizeof(event)));
}
void release(int fd) {
    assert(write(fd, "x", 1) == 1);
}
void await_release(int fd) {
    pollfd p{fd, POLLIN, 0};
    assert(poll(&p, 1, 10000) == 1);
    char value;
    assert(read(fd, &value, 1) == 1);
}
Event await_event(int fd, char kind) {
    pollfd p{fd, POLLIN, 0};
    assert(poll(&p, 1, 10000) == 1);
    Event event{};
    assert(read(fd, &event, sizeof(event)) == static_cast<ssize_t>(sizeof(event)));
    assert(event.kind == kind);
    return event;
}
void reap_child(pid_t pid, bool killed = false) {
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(killed ? WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL
                  : WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
void make_pipe(int (&fds)[2]) {
    int source[2];
    assert(pipe(source) == 0);
    fds[0] = fcntl(source[0], F_DUPFD, 10);
    fds[1] = fcntl(source[1], F_DUPFD, 10);
    assert(fds[0] >= 10 && fds[1] >= 10);
    close(source[0]);
    close(source[1]);
}
void configure_child() {
    if (const auto* value = getenv("SUMH_PACKAGE_TEST_MODE"))
        mode = value;
    if (mode == "probe")
        return;
    event_fd = std::stoi(getenv("SUMH_PACKAGE_TEST_EVENTS"));
    package_gate = std::stoi(getenv("SUMH_PACKAGE_TEST_PACKAGE"));
    doctor_gate = std::stoi(getenv("SUMH_PACKAGE_TEST_DOCTOR"));
    sampler_gate = std::stoi(getenv("SUMH_PACKAGE_TEST_SAMPLER"));
}
void expect_busy(int events, bool doctor_too) {
    auto value =
        json::parse(transport::request_diagnostic_packages(executable, package_request, 100));
    assert(!value.at("ok").b && value.at("errno").n == EBUSY && value.at("stdout").s.empty());
    if (doctor_too) {
        value = json::parse(transport::request_diagnostics(doctor_request, 100));
        assert(!value.at("ok").b && value.at("errno").n == EBUSY);
    }
    pollfd p{events, POLLIN, 0};
    assert(poll(&p, 1, 0) == 0);
}
void expect_free() {
    const int package = transport::acquire_package_guard();
    assert(package >= 0);
    close(package);
    const int doctor = transport::acquire_diagnostics_guard();
    assert(doctor >= 0);
    close(doctor);
}
int package_child(int argc, char** argv) {
    assert(argc == 7);
    sumhp::diagnostic_package_manager::Request parsed;
    assert(sumhp::diagnostic_package_manager::parse_channel_arguments(
        std::vector<std::string>(argv + 2, argv + argc), parsed));
    assert(transport::matches_guard(3, transport::package_guard_name,
                                    sizeof(transport::package_guard_name)));
    if (mode != "probe") {
        emit('K');
        if (mode == "pre-ready-death")
            await_release(package_gate);
    }
    bool called = false;
    const auto result = transport::serve(
        [&] {
            called = true;
            assert(mode != "pre-ready-death");
            assert(transport::package_guard_for_sampling == 3 && (fcntl(3, F_GETFD) & FD_CLOEXEC));
            // The nested request intentionally uses the standalone ksud's own exe.
            return transport::request_diagnostics(doctor_request, 8000);
        },
        transport::Service::DiagnosticPackages);
    if (mode == "probe")
        return result;
    if (mode == "pre-ready-death")
        assert(!called && result != 0);
    if (mode == "completed-descendant")
        assert(called && result == 0);
    return 0;
}
int doctor_child(int argc, char** argv) {
    assert(argc == 6 && std::string(argv[0]) == "ksud");
    sumhp::diagnostics::Request parsed;
    std::string reason;
    assert(sumhp::diagnostics::parse_channel_arguments(
        std::vector<std::string>(argv + 2, argv + argc), parsed, reason));
    // The package guard already survived exec, before doctor READY or ACK.
    assert(transport::valid_diagnostics_guard(3));
    assert(transport::matches_guard(4, transport::package_guard_name,
                                    sizeof(transport::package_guard_name)));
    if (mode != "probe")
        emit('D');
    const auto result = transport::serve(
        [&] {
            assert(transport::package_guard_for_sampling == 4);
            assert((fcntl(3, F_GETFD) & FD_CLOEXEC) && (fcntl(4, F_GETFD) & FD_CLOEXEC));
            if (mode == "probe")
                return std::string("{\"sample\":true}");
            int started[2];
            assert(pipe(started) == 0);
            const auto child = fork();
            assert(child >= 0);
            if (!child) {
                close(started[0]);
                emit('Q');
                release(started[1]);
                close(started[1]);
                assert(transport::valid_diagnostics_guard(3));
                assert(transport::matches_guard(4, transport::package_guard_name,
                                                sizeof(transport::package_guard_name)));
                await_release(sampler_gate);
                _exit(0);
            }
            close(started[1]);
            await_release(started[0]);
            close(started[0]);
            if (mode != "completed-descendant")
                await_release(doctor_gate);
            return std::string("{\"sample\":true}");
        },
        transport::Service::Diagnostics);
    if (mode == "probe")
        return result;
    assert(mode == "completed-descendant" ? result == 0 : result != 0);
    return 0;
}
void lifetime_case(const char* scenario, unsigned closed_stdio) {
    int events[2], package[2], doctor[2], sampler[2];
    make_pipe(events);
    make_pipe(package);
    make_pipe(doctor);
    make_pipe(sampler);
    const auto parent = fork();
    assert(parent >= 0);
    if (!parent) {
        event_fd = events[1];
        setenv("SUMH_PACKAGE_TEST_MODE", scenario, 1);
        setenv("SUMH_PACKAGE_TEST_EVENTS", std::to_string(events[1]).c_str(), 1);
        setenv("SUMH_PACKAGE_TEST_PACKAGE", std::to_string(package[0]).c_str(), 1);
        setenv("SUMH_PACKAGE_TEST_DOCTOR", std::to_string(doctor[0]).c_str(), 1);
        setenv("SUMH_PACKAGE_TEST_SAMPLER", std::to_string(sampler[0]).c_str(), 1);
        for (int fd = 0; fd < 3; ++fd)
            if (closed_stdio & (1U << fd))
                close(fd);
        auto value = json::parse(transport::request_diagnostic_packages(
            executable, package_request,
            std::string(scenario) == "cancel-descendant" ? 1500 : 8000));
        emit('P', static_cast<int>(value.at("errno").n));
        _exit(0);
    }
    const auto k = await_event(events[0], 'K');
    if (std::string(scenario) == "pre-ready-death") {
        assert(kill(parent, SIGKILL) == 0);
        reap_child(parent, true);
        expect_busy(events[0], false);
        release(package[1]);
        reap_child(k.pid);
        expect_free();
    } else {
        const auto d = await_event(events[0], 'D');
        const auto q = await_event(events[0], 'Q');
        if (std::string(scenario) == "ack-death") {
            assert(kill(parent, SIGKILL) == 0);
            reap_child(parent, true);
            assert(kill(k.pid, SIGKILL) == 0);
            reap_child(k.pid, true);
        } else {
            const auto p = await_event(events[0], 'P');
            assert(p.pid == parent &&
                   p.result == (std::string(scenario) == "cancel-descendant" ? ETIMEDOUT : 0));
            reap_child(parent);
            if (std::string(scenario) == "cancel-descendant") {
                int status = 0;
                const auto waited = waitpid(k.pid, &status, 0);
                assert((waited == k.pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) ||
                       (waited < 0 && errno == ECHILD));
            }
        }
        expect_busy(events[0], true);
        if (std::string(scenario) != "completed-descendant") {
            release(doctor[1]);
            reap_child(d.pid);
            expect_busy(events[0], true);
        }
        release(sampler[1]);
        reap_child(q.pid);
        expect_free();
    }
    for (const auto& pair : {events, package, doctor, sampler}) {
        close(pair[0]);
        close(pair[1]);
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2) {
        configure_child();
        return argc == 7 ? package_child(argc, argv) : doctor_child(argc, argv);
    }
    char path[4096];
    const auto size = readlink("/proc/self/exe", path, sizeof(path));
    assert(size > 0 && size < static_cast<ssize_t>(sizeof(path)));
    executable.assign(path, static_cast<std::size_t>(size));
    assert(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    const auto supported =
        json::parse(transport::request_diagnostic_packages(executable, package_request));
    const auto error = static_cast<int>(supported.at("errno").n);
    if (error == ENOSYS || error == EINVAL || error == EPERM) {
        std::cout << "SKIP: host policy/kernel lacks pidfd package transport support\n";
        return 0;
    }
    assert(supported.at("ok").b && json::parse(supported.at("stdout").s).at("ok").b);
    lifetime_case("pre-ready-death", 0);
    lifetime_case("ack-death", 3);
    lifetime_case("cancel-descendant", 7);
    lifetime_case("completed-descendant", 1);
    std::cout << "POSIX package/doctor/sampler guards: pre-READY death, ACK parent/worker death, "
                 "pidfd cancellation and live descendant after successful completion passed\n";
}
