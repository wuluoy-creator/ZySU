#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <iostream>
#include "../../common/utf8.hpp"
#include "core/profile.hpp"

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
constexpr int F_DUPFD_CLOEXEC = 1, F_GETFL = 2, F_SETFL = 3;
constexpr int O_RDONLY = 0, O_CLOEXEC = 4, O_NOFOLLOW = 8, O_NONBLOCK = 16;
constexpr int POLLIN = 1, POLLHUP = 2, POLLNVAL = 4;
constexpr int REGULAR = 1, FIFO = 2, SOCKET = 3, DIRECTORY = 4;
#define S_ISREG(m) ((m) == REGULAR)
#define S_ISFIFO(m) ((m) == FIFO)
#define S_ISSOCK(m) ((m) == SOCKET)
struct InputStat {
    long long st_size;
    int st_mode;
    int st_nlink;
};
struct pollfd {
    int fd;
    short events;
    short revents;
};
namespace io_model {
struct Step {
    int count;
    int error = 0;
};
std::deque<Step> poll_steps, read_steps;
std::string input;
std::size_t consumed;
InputStat metadata;
int close_count, opens, dup_count, set_count, flags, fail_set, error_open, error_stat, error_get;
short events;
std::int64_t tick, poll_delay;
int assertions = 0;
struct Clock {
    using duration = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<Clock, duration>;
    static time_point now() { return time_point(duration(tick)); }
};
void check(bool value) {
    ++assertions;
    if (!value) {
        std::cerr << "IO check " << assertions << " failed\n";
        std::abort();
    }
}
int open(const char*, int options) {
    ++opens;
    assert(options == (O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    return error_open ? -1 : 7;
}
int close(int fd) {
    assert(fd == 7);
    ++close_count;
    return 0;
}
int fstat(int fd, struct InputStat* out) {
    assert(fd == 7);
    *out = metadata;
    return error_stat ? -1 : 0;
}
int fcntl(int fd, int command, int value = 0) {
    if (command == F_DUPFD_CLOEXEC) {
        assert(fd == 0 && value == 3);
        ++dup_count;
        return error_open ? -1 : 7;
    }
    assert(fd == 7);
    if (command == F_GETFL)
        return error_get ? -1 : flags;
    assert(command == F_SETFL);
    ++set_count;
    if (set_count == fail_set)
        return -1;
    flags = value;
    return 0;
}
int poll(pollfd* fd, unsigned count, int timeout) {
    assert(count == 1 && fd->fd == 7 && timeout > 0 && timeout <= 1000);
    tick += poll_delay;
    fd->revents = events;
    if (poll_steps.empty())
        return 1;
    const auto step = poll_steps.front();
    poll_steps.pop_front();
    errno = step.error;
    return step.count;
}
std::ptrdiff_t read(int fd, char* buffer, std::size_t capacity) {
    assert(fd == 7 && capacity > 0 && capacity <= 4096);
    auto count = std::min(capacity, input.size() - consumed);
    if (!read_steps.empty()) {
        const auto step = read_steps.front();
        read_steps.pop_front();
        errno = step.error;
        if (step.count < 0)
            return step.count;
        count = std::min(count, static_cast<std::size_t>(step.count));
    }
    std::memcpy(buffer, input.data() + consumed, count);
    consumed += count;
    return count;
}
void reset(const std::string& bytes = "hello") {
    input = bytes;
    consumed = 0;
    metadata = {static_cast<long long>(bytes.size()), REGULAR, 1};
    close_count = opens = dup_count = set_count = error_open = error_stat = error_get = fail_set =
        flags = 0;
    tick = 0;
    poll_delay = 0;
    events = POLLIN;
    poll_steps.clear();
    read_steps.clear();
}
}  // namespace io_model
#include "profile_input_io_under_test.inc"
int main() {
    using namespace io_model;
    using sumhp::profiles::PROFILE_INPUT_MAX_BYTES;
    using sumhp::profiles::read_profile_input;
    std::string bytes, error;
    for (const auto& path : {std::string(), std::string("relative"), std::string("/bad\0path", 9),
                             std::string("/\xc0\xaf", 3), std::string(4096, '/')}) {
        reset();
        bytes = "unchanged";
        check(!read_profile_input(path, bytes, error) && error == "invalid_input_path" &&
              bytes == "unchanged");
        check(opens == 0 && dup_count == 0 && close_count == 0);
    }
    reset();
    check(read_profile_input("/test", bytes, error) && bytes == input && error.empty());
    check(close_count == 1 && opens == 1 && dup_count == 0);
    reset();
    check(read_profile_input("-", bytes, error) && bytes == input && flags == 0 && set_count == 2 &&
          close_count == 1);
    reset();
    flags = O_NONBLOCK;
    check(read_profile_input("-", bytes, error) && flags == O_NONBLOCK);
    for (int type : {FIFO, SOCKET}) {
        reset();
        metadata = {0, type, 2};
        events = POLLIN | POLLHUP;
        read_steps = {{2}, {-1, EINTR}, {-1, EAGAIN}, {3}};
        poll_steps = {{-1, EINTR}, {1}};
        check(read_profile_input("-", bytes, error) && bytes == input && flags == 0);
    }
    for (const auto* path : {"/test", "-"}) {
        for (int failure = 0; failure < 9; ++failure) {
            reset();
            bytes = "unchanged";
            if (failure == 0)
                error_open = 1;
            if (failure == 1)
                error_stat = 1;
            if (failure == 2)
                metadata.st_mode = DIRECTORY;
            if (failure == 3)
                metadata.st_size = -1;
            if (failure == 4)
                metadata.st_size = PROFILE_INPUT_MAX_BYTES + 1;
            if (failure == 5)
                poll_steps = {{0}};
            if (failure == 6)
                poll_steps = {{-1, EIO}};
            if (failure == 7)
                events = POLLNVAL;
            if (failure == 8)
                read_steps = {{-1, EIO}};
            check(!read_profile_input(path, bytes, error) && bytes == "unchanged" &&
                  !error.empty());
            check(close_count == (failure == 0 ? 0 : 1) && flags == 0);
        }
    }
    for (int type : {FIFO, SOCKET}) {
        reset();
        metadata.st_mode = type;
        check(!read_profile_input("/test", bytes, error));
    }
    reset();
    metadata.st_nlink = 2;
    check(!read_profile_input("/test", bytes, error) && error == "input_type_or_links_invalid");
    reset();
    error_get = 1;
    check(!read_profile_input("-", bytes, error) && error == "input_nonblocking_failed");
    reset();
    fail_set = 1;
    check(!read_profile_input("-", bytes, error) && flags == 0);
    reset();
    fail_set = 2;
    bytes = "unchanged";
    check(!read_profile_input("-", bytes, error) && error == "input_flags_restore_failed" &&
          bytes == "unchanged" && flags == 0);
    reset(std::string(PROFILE_INPUT_MAX_BYTES, 'x'));
    check(read_profile_input("-", bytes, error) && bytes.size() == PROFILE_INPUT_MAX_BYTES);
    reset(std::string(PROFILE_INPUT_MAX_BYTES + 1, 'x'));
    metadata = {0, FIFO, 1};
    bytes = "unchanged";
    check(!read_profile_input("-", bytes, error) && error == "input_capacity" &&
          bytes == "unchanged" && flags == 0);
    reset();
    metadata = {0, FIFO, 1};
    poll_delay = 400;
    read_steps = {{1}, {-1, EAGAIN}, {-1, EINTR}};
    check(!read_profile_input("-", bytes, error) && error == "input_timeout" && consumed == 1 &&
          flags == 0);
    reset("");
    check(read_profile_input("/test", bytes, error) && bytes.empty());
    std::cout << assertions << " profile input IO assertions passed\n";
}
