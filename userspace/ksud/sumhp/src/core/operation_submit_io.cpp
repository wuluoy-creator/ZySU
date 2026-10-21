#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include "core/operation_submit.hpp"

namespace sumhp::state {
bool read_operation_input(const std::string& path, std::string& bytes, std::string& error) {
    const bool standard_input = path == "-";
    class Fd {
    public:
        explicit Fd(int descriptor) : value(descriptor) {}
        Fd(const Fd&) = delete;
        Fd& operator=(const Fd&) = delete;
        Fd(Fd&&) = delete;
        Fd& operator=(Fd&&) = delete;
        ~Fd() {
            if (value < 0)
                return;
            if (old_flags >= 0)
                (void)::fcntl(value, F_SETFL, old_flags);
            ::close(value);
        }
        [[nodiscard]] int get() const { return value; }
        bool nonblocking() {
            old_flags = ::fcntl(value, F_GETFL);
            return old_flags >= 0 && ::fcntl(value, F_SETFL, old_flags | O_NONBLOCK) == 0;
        }
        bool restore_flags() {
            if (old_flags < 0)
                return true;
            if (::fcntl(value, F_SETFL, old_flags) != 0)
                return false;
            old_flags = -1;
            return true;
        }

    private:
        int value;
        int old_flags = -1;
    } fd{standard_input ? ::fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3)
                        : ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    const auto fail = [&](const char* reason) {
        error = reason;
        return false;
    };
    if (fd.get() < 0)
        return fail("input_open_failed");
    struct stat metadata{};
    if (::fstat(fd.get(), &metadata) != 0)
        return fail("input_stat_failed");
    if (metadata.st_size < 0 ||
        (!standard_input && (!S_ISREG(metadata.st_mode) || metadata.st_nlink != 1)) ||
        (standard_input && !S_ISREG(metadata.st_mode) && !S_ISFIFO(metadata.st_mode) &&
         !S_ISSOCK(metadata.st_mode)))
        return fail("input_type_or_links_invalid");
    if (static_cast<std::uint64_t>(metadata.st_size) > OPERATION_INPUT_BYTES)
        return fail("input_capacity");
    if (standard_input && !fd.nonblocking())
        return fail("input_nonblocking_failed");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::string value;
    char buffer[4096];
    while (value.size() <= OPERATION_INPUT_BYTES) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0)
            return fail("input_timeout");
        pollfd descriptor{fd.get(), POLLIN, 0};
        const auto ready = ::poll(&descriptor, 1, static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready == 0)
            return fail("input_timeout");
        if (ready < 0 || (descriptor.revents & POLLNVAL))
            return fail("input_poll_failed");
        const auto count = ::read(
            fd.get(), buffer, std::min(sizeof(buffer), OPERATION_INPUT_BYTES + 1 - value.size()));
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count < 0)
            return fail("input_read_failed");
        if (!count) {
            // stdin's duplicate shares its open-file flags with the caller.
            // Do not silently report success if restoring them failed.
            if (!fd.restore_flags())
                return fail("input_flags_restore_failed");
            bytes = std::move(value);
            error.clear();
            return true;
        }
        value.append(buffer, static_cast<std::size_t>(count));
    }
    return fail("input_capacity");
}
}  // namespace sumhp::state
