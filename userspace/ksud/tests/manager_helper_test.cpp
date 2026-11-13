#include "manager_helper.hpp"
#include "core/ksucalls.hpp"
#include "userspace/common/manager_protocol.hpp"

#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cassert>
#include <iostream>

namespace {
bool authorized = true, compatible = true, permit_root = true;
bool exercise_watchdog = false;
int grants = 0;
uid_t caller = 10123;
using namespace zysu::manager;
Deadline deadline() {
    return std::chrono::steady_clock::now() + std::chrono::seconds(3);
}
template <class F>
void rejects(F action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    assert(rejected);
}
Frame run_request(uint32_t operation, const std::string& payload, int expected_grants,
                  bool truncate = false) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    const auto pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(sockets[0]);
        assert(dup2(sockets[1], STDIN_FILENO) == STDIN_FILENO);
        assert(dup2(sockets[1], STDOUT_FILENO) == STDOUT_FILENO);
        close(sockets[1]);
        grants = 0;
        const int status = ksud::manager_helper_main();
        _exit(status == 0 && grants == expected_grants ? 0 : 99);
    }
    close(sockets[1]);
    if (authorized && compatible && caller != 0) {
        if (truncate) {
            std::string header;
            append_u32(header, magic);
            append_u32(header, version);
            append_u32(header, operation);
            append_u32(header, 100);
            send_bytes(sockets[0], header, deadline());
        } else
            send_frame(sockets[0], operation, payload, deadline());
    }
    shutdown(sockets[0], SHUT_WR);
    const auto response = receive_frame(sockets[0], deadline());
    close(sockets[0]);
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return response;
}
}  // namespace

// Test only the real dispatch/framing/authentication sequence. No kernel root
// grant, filesystem installation, daemon start or privileged ioctl is performed.
extern "C" uid_t __wrap_getuid() {
    return caller;
}
extern "C" uid_t __wrap_geteuid() {
    return grants && permit_root ? 0 : caller;
}
extern "C" int ksu_sumh_ioctl(unsigned long, void*) {
    errno = EPERM;
    return -1;
}
namespace ksud {
void setup_sigsys_handler() {}
uint32_t get_flags() {
    return authorized ? KSU_GET_INFO_FLAG_MANAGER : 0;
}
bool ensure_uapi_version_matched(std::string*) {
    return compatible;
}
int grant_root() {
    ++grants;
    return permit_root ? 0 : -1;
}
std::string manager_integrity(zysu::manager::Operation operation, const std::string&) {
    assert(operation == zysu::manager::Operation::RepairLinks);
    assert(grants == 1);
    if (exercise_watchdog) {
        const auto descendant = fork();
        assert(descendant >= 0);
        if (!descendant)
            for (;;)
                pause();
        raise(SIGALRM);
        for (;;)
            pause();
    }
    return "1";
}
}  // namespace ksud

int main() {
    const std::vector<std::string> fields{"", "path with spaces", std::string("a\0b", 3)};
    assert(decode_fields(encode_fields(fields)) == fields);
    rejects([] { decode_fields(""); });
    rejects([] { decode_fields(encode_fields({"ok"}) + "extra"); });
    rejects([] { encode_fields(std::vector<std::string>(max_fields + 1)); });
    rejects([] { encode_fields({std::string(max_payload, 'x')}); });
    auto truncated = encode_fields({"abcd"});
    truncated.pop_back();
    rejects([&] { decode_fields(truncated); });
    std::string too_many;
    append_u32(too_many, max_fields + 1);
    rejects([&] { decode_fields(too_many); });

    const auto repair = static_cast<uint32_t>(Operation::RepairLinks);
    const auto empty = encode_fields({});
    assert(run_request(repair, empty, 1).payload == "1");
    authorized = false;
    assert(run_request(repair, empty, 0).code != 0);
    authorized = true;
    compatible = false;
    assert(run_request(repair, empty, 0).code != 0);
    compatible = true;
    caller = 0;
    assert(run_request(repair, empty, 0).code != 0);
    caller = 10123;
    permit_root = false;
    assert(run_request(repair, empty, 1).code != 0);
    permit_root = true;
    assert(run_request(999, empty, 0).code != 0);
    assert(run_request(repair, "malformed", 0).code != 0);
    assert(run_request(repair, empty, 0, true).code != 0);
    assert(run_request(repair, encode_fields({"unexpected"}), 1).code != 0);

    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    rejects([&] { receive_frame(sockets[0], std::chrono::steady_clock::now()); });
    std::string header;
    append_u32(header, magic);
    append_u32(header, version);
    append_u32(header, 0);
    append_u32(header, max_payload + 1);
    send_bytes(sockets[1], header, deadline());
    rejects([&] { receive_frame(sockets[0], deadline()); });
    close(sockets[1]);
    rejects([&] { send_frame(sockets[0], 0, "closed peer", deadline()); });
    close(sockets[0]);

    // The privileged side must restore SIGALRM and clean up its whole group,
    // even when the launching app ignored and blocked that signal.
    assert(prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    const auto watchdog = fork();
    assert(watchdog >= 0);
    if (!watchdog) {
        close(sockets[0]);
        assert(dup2(sockets[1], 0) == 0 && dup2(sockets[1], 1) == 1);
        close(sockets[1]);
        signal(SIGALRM, SIG_IGN);
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGALRM);
        assert(sigprocmask(SIG_BLOCK, &blocked, nullptr) == 0);
        exercise_watchdog = true;
        _exit(ksud::manager_helper_main());
    }
    close(sockets[1]);
    send_frame(sockets[0], repair, empty, deadline());
    shutdown(sockets[0], SHUT_WR);
    rejects([&] { receive_frame(sockets[0], deadline()); });
    close(sockets[0]);
    int status = 0;
    assert(waitpid(watchdog, &status, 0) == watchdog);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    assert(waitpid(-1, &status, 0) > 0);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    std::cout << "Manager helper framing and authorization tests passed\n";
}
