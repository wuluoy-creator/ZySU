#include <sys/wait.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <string>
#include "userspace/common/json.hpp"
#include "userspace/common/maps_preview_transport.hpp"

int main(int argc, char** argv) {
    namespace transport = sumhp::preview_transport;
    if (argc == 6) {
        assert(std::string(argv[1]) == "sumhp" && std::string(argv[2]) == "sumh" &&
               std::string(argv[3]) == "maps" && std::string(argv[4]) == "preview" &&
               std::string(argv[5]) == "--channel");
        return transport::serve([] {
            if (getenv("SUMH_TEST_TIMEOUT"))
                sleep(2);
            return std::string("{\"schema_version\":1,\"unicode\":\"预览\"}");
        });
    }
    char path[4096];
    const auto count = readlink("/proc/self/exe", path, sizeof(path) - 1);
    assert(count > 0);
    path[count] = 0;
    const auto observed = json::parse(transport::request(path));
    if (observed.at("errno").n == ENOSYS || observed.at("errno").n == EINVAL ||
        observed.at("errno").n == EPERM) {
        std::cout << "SKIP: kernel/security policy lacks pidfd transport support\n";
        return 0;
    }
    if (!observed.at("ok").b)
        std::cerr << "Initial POSIX preview response: " << json::dump(observed) << '\n';
    assert(observed.at("ok").b);
    assert(json::parse(observed.at("stdout").s).at("unicode").s == "预览");
    setenv("SUMH_TEST_TIMEOUT", "1", 1);
    assert(json::parse(transport::request(path, 50)).at("errno").n == ETIMEDOUT);
    unsetenv("SUMH_TEST_TIMEOUT");
    usleep(100000);
    assert(json::parse(transport::request(path)).at("ok").b);
    const int guard = transport::acquire_preview_guard();
    assert(guard >= 0);
    int sync[2];
    assert(pipe(sync) == 0);
    const auto child = fork();
    assert(child >= 0);
    if (!child) {
        close(sync[1]);
        char byte;
        assert(read(sync[0], &byte, 1) == 1);
        close(guard);
        _exit(0);
    }
    close(sync[0]);
    close(guard);
    // A forked sampler keeps reservation alive after the preview parent closes.
    assert(transport::acquire_preview_guard() < 0 && errno == EBUSY);
    assert(write(sync[1], "x", 1) == 1);
    close(sync[1]);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
    const int again = transport::acquire_preview_guard();
    assert(again >= 0);
    close(again);
    std::cout << "POSIX fixed exec, pidfd timeout/reap and inherited abstract guard smoke passed\n";
}
