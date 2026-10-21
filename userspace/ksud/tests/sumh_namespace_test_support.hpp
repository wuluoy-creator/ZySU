#pragma once

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#include "mount/namespace_result.hpp"

#if !defined(_WIN32) && !defined(SUMH_NAMESPACE_MODELED_CHILD)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace namespace_fixture {
using sumhp::mount::fsutil::NamespaceOperationState;
inline bool switch_ok = true;
inline bool pipe_ok = true;
namespace ksud {
bool switch_mnt_ns(int) {
    return switch_ok;
}
}  // namespace ksud
namespace logging {
enum class Level { Error };
}
void mlog(const std::string&, logging::Level) {}

#if defined(_WIN32) || defined(SUMH_NAMESPACE_MODELED_CHILD)
using pid_t = int;
#ifndef PIPE_BUF
#define PIPE_BUF 4096
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
inline std::vector<char> pipe_bytes;
inline std::size_t read_cursor = 0;
inline bool interrupted_write = false;
inline bool interrupted_read = false;
inline bool drop_reply = false;
inline bool truncate_reply = false;
inline bool failed_read = false;
inline bool lost_child_completion = false;
struct ChildExit {
    int status;
};
int pipe2(int* fds, int) {
    if (!pipe_ok) {
        errno = EMFILE;
        return -1;
    }
    pipe_bytes.clear();
    read_cursor = 0;
    fds[0] = 5;
    fds[1] = 6;
    return 0;
}
int close(int) {
    return 0;
}
int fork() {
    return 0;
}
int waitpid(int, int*, int) {
    return -1;
}
#ifndef WIFEXITED
#define WIFEXITED(value) ((value) == 0)
#define WEXITSTATUS(value) (value)
#endif
#define _exit maps_test_exit
[[noreturn]] void maps_test_exit(int status) {
    throw ChildExit{status};
}
int write(int, const void* data, std::size_t count) {
    if (interrupted_write) {
        interrupted_write = false;
        errno = EINTR;
        return -1;
    }
    if (drop_reply || (truncate_reply && !pipe_bytes.empty())) {
        errno = EPIPE;
        return -1;
    }
    const auto* bytes = static_cast<const char*>(data);
    const auto written = std::min(count, std::size_t{19});
    pipe_bytes.insert(pipe_bytes.end(), bytes, bytes + written);
    return static_cast<int>(written);
}
int read(int, void* data, std::size_t count) {
    if (interrupted_read) {
        interrupted_read = false;
        errno = EINTR;
        return -1;
    }
    if (failed_read) {
        errno = EIO;
        return -1;
    }
    const auto received = std::min({count, pipe_bytes.size() - read_cursor, std::size_t{23}});
    if (received)
        std::memcpy(data, pipe_bytes.data() + read_cursor, received);
    read_cursor += received;
    return static_cast<int>(received);
}
// Exercise the production fork/namespace/exit branch while keeping the callback
// in this process so tests can inspect a committed set after a lost response.
#define run_in_init_mount_ns run_in_init_mount_ns_child
#include "sumh_namespace_child_under_test.inc"
#undef run_in_init_mount_ns
#undef _exit
bool run_in_init_mount_ns(const std::function<bool()>& fn) {
    try {
        return run_in_init_mount_ns_child(fn);
    } catch (const ChildExit& exit) {
        if (lost_child_completion) {
            errno = ECHILD;
            return false;
        }
        return exit.status == 0;
    }
}
#include "sumh_namespace_pipe_under_test.inc"
#else
#include "sumh_namespace_result_under_test.inc"
#endif
}  // namespace namespace_fixture
