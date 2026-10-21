// Linux-only actual exec/fork lifetime smoke. Includes the unchanged transport
// body so the reservation probe uses its real address and bind implementation.
// Pipe events establish every death/cancellation window; no guessed sleeps.
#include <poll.h>
#include <sys/prctl.h>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include "userspace/common/maps_preview_transport.cpp"

namespace {
namespace transport = sumhp::preview_transport;
constexpr const char* request = R"({"schema_version":1,"scenario":"boot-protection"})";
struct Event {
    char kind;
    pid_t pid;
    int result;
};
int event_fd = -1, release_doctor = -1, release_sampler = -1;
std::string mode;

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
    char byte;
    assert(read(fd, &byte, 1) == 1);
}
Event await_event(int fd, char kind) {
    pollfd p{fd, POLLIN, 0};
    assert(poll(&p, 1, 10000) == 1);
    Event event{};
    assert(read(fd, &event, sizeof(event)) == static_cast<ssize_t>(sizeof(event)));
    assert(event.kind == kind);
    return event;
}
void reap_child(pid_t child, bool killed = false) {
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(killed ? (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)
                  : (WIFEXITED(status) && WEXITSTATUS(status) == 0));
}
void make_pipe(int (&fds)[2]) {
    int original[2];
    assert(pipe(original) == 0);
    // Keep test synchronization descriptors away from production stdio/fd3.
    fds[0] = fcntl(original[0], F_DUPFD, 10);
    fds[1] = fcntl(original[1], F_DUPFD, 10);
    assert(fds[0] >= 10 && fds[1] >= 10);
    close(original[0]);
    close(original[1]);
}
void expect_busy_without_exec(int events) {
    const auto report = json::parse(transport::request_diagnostics(request, 100));
    assert(!report.at("ok").b && report.at("errno").n == EBUSY && report.at("stdout").s.empty());
    pollfd p{events, POLLIN, 0};
    assert(poll(&p, 1, 0) == 0);  // No second doctor/collector chain was spawned.
}
void expect_free() {
    const int guard = transport::acquire_diagnostics_guard();
    assert(guard >= 0);
    close(guard);
}
int doctor_child(int argc, char** argv) {
    assert(argc == 6 && std::string(argv[0]) == "ksud");
    sumhp::diagnostics::Request parsed;
    std::string reason;
    assert(sumhp::diagnostics::parse_channel_arguments(
        std::vector<std::string>(argv + 2, argv + argc), parsed, reason));
    mode = getenv("SUMH_DOCTOR_TEST_MODE") ? getenv("SUMH_DOCTOR_TEST_MODE") : "probe";
    if (mode != "probe") {
        event_fd = std::stoi(getenv("SUMH_DOCTOR_TEST_EVENT"));
        release_doctor = std::stoi(getenv("SUMH_DOCTOR_TEST_DOCTOR"));
        release_sampler = std::stoi(getenv("SUMH_DOCTOR_TEST_SAMPLER"));
        emit('D');
        // The exec itself has already inherited FD3, before serve/READY.
        assert(transport::valid_diagnostics_guard(3));
        if (mode == "pre-ready-death")
            await_release(release_doctor);
    }
    bool called = false;
    const int result = transport::serve(
        [&] {
            called = true;
            assert(fcntl(3, F_GETFD) & FD_CLOEXEC);
            if (mode == "probe")
                return std::string("{\"sample\":true}");
            assert(mode != "pre-ready-death");  // Parent died before READY/ACK.
            int started[2];
            assert(pipe(started) == 0);
            const auto sampler = fork();
            assert(sampler >= 0);
            if (!sampler) {
                close(started[0]);
                emit('Q');
                release(started[1]);
                close(started[1]);
                await_release(release_sampler);
                // No special close/shutdown of FD3: exit releases this process's
                // inherited reference, just like an existing namespace sampler.
                _exit(0);
            }
            close(started[1]);
            await_release(started[0]);
            close(started[0]);
            if (mode != "completed-descendant")
                await_release(release_doctor);
            return std::string("{\"sample\":true}");
        },
        transport::Service::Diagnostics);
    if (mode == "probe")
        return result;
    if (mode == "pre-ready-death")
        assert(!called && result != 0);
    if (mode == "ack-death")
        assert(called && result != 0);
    if (mode == "completed-descendant")
        assert(called && result == 0);
    return 0;
}

void lifetime_case(const char* scenario, unsigned closed_stdio) {
    int events[2], doctor[2], sampler[2];
    make_pipe(events);
    make_pipe(doctor);
    make_pipe(sampler);
    const auto parent = fork();
    assert(parent >= 0);
    if (!parent) {
        event_fd = events[1];
        setenv("SUMH_DOCTOR_TEST_MODE", scenario, 1);
        setenv("SUMH_DOCTOR_TEST_EVENT", std::to_string(events[1]).c_str(), 1);
        setenv("SUMH_DOCTOR_TEST_DOCTOR", std::to_string(doctor[0]).c_str(), 1);
        setenv("SUMH_DOCTOR_TEST_SAMPLER", std::to_string(sampler[0]).c_str(), 1);
        for (int fd = 0; fd < 3; ++fd)
            if (closed_stdio & (1U << fd))
                close(fd);
        const auto value = json::parse(transport::request_diagnostics(
            request, std::string(scenario) == "cancel-descendant" ? 1000 : 8000));
        emit('P', static_cast<int>(value.at("errno").n));
        _exit(0);
    }
    const auto d = await_event(events[0], 'D');
    if (std::string(scenario) == "pre-ready-death") {
        assert(kill(parent, SIGKILL) == 0);
        reap_child(parent, true);
        expect_busy_without_exec(events[0]);
        release(doctor[1]);
        reap_child(d.pid);
        expect_free();
    } else {
        const auto q = await_event(events[0], 'Q');  // Confirms ACK admitted callback.
        if (std::string(scenario) == "ack-death") {
            assert(kill(parent, SIGKILL) == 0);
            reap_child(parent, true);
            expect_busy_without_exec(events[0]);
            release(doctor[1]);
            reap_child(d.pid);
        } else {
            const auto p = await_event(events[0], 'P');
            assert(p.pid == parent &&
                   p.result == (std::string(scenario) == "cancel-descendant" ? ETIMEDOUT : 0));
            reap_child(parent);
            if (std::string(scenario) == "cancel-descendant") {
                // abandon() may already have reaped D before P exits. Otherwise
                // the subreaper adopts it; either route must leave no live D.
                int status = 0;
                const auto waited = waitpid(d.pid, &status, 0);
                assert((waited == d.pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) ||
                       (waited < 0 && errno == ECHILD));
            }
            // Successful D was already reaped by P using its pidfd.
        }
        expect_busy_without_exec(events[0]);
        release(sampler[1]);
        reap_child(q.pid);
        expect_free();
    }
    for (const auto& pair : {events, doctor, sampler}) {
        close(pair[0]);
        close(pair[1]);
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2)
        return doctor_child(argc, argv);
    assert(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    const auto supported = json::parse(transport::request_diagnostics(request));
    const auto error = static_cast<int>(supported.at("errno").n);
    if (error == ENOSYS || error == EINVAL || error == EPERM) {
        std::cout << "SKIP: host policy/kernel lacks pidfd transport support\n";
        return 0;
    }
    assert(supported.at("ok").b);
    lifetime_case("pre-ready-death", 0);
    lifetime_case("ack-death", 3);
    lifetime_case("cancel-descendant", 7);
    lifetime_case("completed-descendant", 1);
    std::cout << "POSIX Diagnostics exec/fork: pre-READY death, ACK death, pidfd cancellation, "
                 "completed parent with live descendant and slot reuse passed\n";
}
