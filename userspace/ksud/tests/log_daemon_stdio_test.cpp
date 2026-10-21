#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

// Deterministic descriptor fixture for the production detach function.
enum { O_RDWR = 2, O_CLOEXEC = 4, O_NOFOLLOW = 8, F_DUPFD_CLOEXEC = 10 };
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif
namespace fixture {
std::map<int, std::string> descriptors;
int fail_open = 0, fail_duplicate = 0, fail_target = -1, calls = 0, duplicate_calls = 0;
void reset() {
    descriptors = {{0, "stdin"}, {1, "stdout"}, {2, "stderr"}};
    fail_open = fail_duplicate = duplicate_calls = 0;
    fail_target = -1;
}
int allocate(const std::string& name) {
    int fd = 0;
    while (descriptors.count(fd))
        ++fd;
    descriptors[fd] = name;
    return fd;
}
int open(const char* path, int flags) {
    ++calls;
    if (std::string(path) != "/dev/null" ||
        (flags != O_RDWR && flags != (O_RDWR | O_CLOEXEC | O_NOFOLLOW)))
        std::abort();
    if (fail_open) {
        errno = EACCES;
        return -1;
    }
    return allocate("/dev/null");
}
int fcntl(int fd, int command, int low) {
    if (command != F_DUPFD_CLOEXEC || low != 3)
        std::abort();
    if (++duplicate_calls == fail_duplicate) {
        errno = EMFILE;
        return -1;
    }
    while (descriptors.count(low))
        ++low;
    descriptors[low] = descriptors.at(fd);
    return low;
}
int close(int fd) {
    descriptors.erase(fd);
    return 0;
}
int dup2(int from, int to) {
    if (to == fail_target) {
        errno = EBADF;
        return -1;
    }
    descriptors[to] = descriptors.at(from);
    return to;
}
}  // namespace fixture
#include "log_daemon_stdio_under_test.inc"
#define LOGE(...) ((void)0)
#include "log_daemon_reset_under_test.inc"
#undef LOGE
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
int main() {
    fixture::reset();
    require(detach_background_stdio(), "detach succeeds");
    require(fixture::descriptors.size() == 3, "no extra descriptors");
    for (int fd = 0; fd < 3; ++fd)
        require(fixture::descriptors.at(fd) == "/dev/null", "all standard descriptors detached");
    for (int missing = 0; missing < 3; ++missing) {
        fixture::reset();
        fixture::descriptors.erase(missing);
        require(detach_background_stdio(), "detach tolerates closed standard descriptor");
        require(fixture::descriptors.size() == 3, "reused descriptor remains installed");
        for (int fd = 0; fd < 3; ++fd)
            require(fixture::descriptors.at(fd) == "/dev/null",
                    "closed standard descriptor restored");
    }
    fixture::reset();
    fixture::fail_open = 1;
    require(!detach_background_stdio() && errno == EACCES, "open failure propagated");
    require(fixture::descriptors.size() == 3, "open failure leak-free");
    fixture::reset();
    fixture::fail_duplicate = 1;
    require(!detach_background_stdio() && errno == EMFILE, "stable duplication failure");
    require(fixture::descriptors.size() == 3, "duplication failure leak-free");
    for (int target = 0; target < 3; ++target) {
        fixture::reset();
        fixture::fail_target = target;
        require(!detach_background_stdio() && errno == EBADF, "partial dup2 failure propagated");
        require(fixture::descriptors.size() == 3, "partial dup2 failure leak-free");
    }
    for (int mask = 0; mask < 8; ++mask) {
        fixture::reset();
        for (int fd = 0; fd < 3; ++fd)
            if (mask & (1 << fd))
                fixture::descriptors.erase(fd);
        int pipe[2] = {fixture::allocate("ready-reader"), fixture::allocate("ready-writer")};
        require(promote_background_pipe(pipe), "closed standard stream combinations promote");
        require(pipe[0] >= 3 && pipe[1] >= 3, "both endpoints promoted before daemonization");
        // Execute the actual helper called inside daemonize_process, then the
        // SUMHP-specific checked detach. Either may replace descriptors 0..2.
        require(reset_stdio_to_devnull(), "daemonization stdio reset succeeds");
        require(detach_background_stdio(), "SUMHP checked stdio detach succeeds");
        require(fixture::descriptors.size() == 5, "closed combinations no leak");
        require(fixture::descriptors.at(pipe[0]) == "ready-reader", "read endpoint preserved");
        require(fixture::descriptors.at(pipe[1]) == "ready-writer", "write endpoint preserved");
        for (int fd = 0; fd < 3; ++fd)
            require(fixture::descriptors.at(fd) == "/dev/null",
                    "all streams detached after promotion");
    }
    for (int failure = 1; failure <= 2; ++failure) {
        fixture::reset();
        fixture::descriptors.clear();
        int pipe[2] = {fixture::allocate("ready-reader"), fixture::allocate("ready-writer")};
        fixture::fail_duplicate = failure;
        require(!promote_background_pipe(pipe) && errno == EMFILE, "promotion failure propagated");
        require(fixture::descriptors.at(pipe[0]) == "ready-reader",
                "reader retained for cleanup after failure");
        require(fixture::descriptors.at(pipe[1]) == "ready-writer",
                "writer retained for cleanup after failure");
        fixture::close(pipe[0]);
        fixture::close(pipe[1]);
        require(fixture::descriptors.empty(),
                "partial promotion caller cleanup leaves no descriptors");
    }
    std::cout << "daemon stdio checks: " << checks << '\n';
}
