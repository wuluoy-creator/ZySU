#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <set>
#include "core/profile.hpp"
#include "userspace/common/utf8.hpp"

namespace sumhp::profiles {
namespace {
bool scopes(const std::string& text, std::vector<std::string>& out) {
    static const std::set<std::string> allowed{"settings", "modules", "user_hide", "maps"};
    if (text == "all") {
        out.assign(allowed.begin(), allowed.end());
        return true;
    }
    std::set<std::string> selected;
    std::size_t start = 0;
    for (;;) {
        const auto end = text.find(',', start);
        const auto value = text.substr(start, end == std::string::npos ? end : end - start);
        if (!allowed.count(value) || !selected.insert(value).second)
            return false;
        out.push_back(value);
        if (end == std::string::npos)
            return true;
        start = end + 1;
    }
}
bool input_path_valid(const std::string& path) {
    return path == "-" || (!path.empty() && path.front() == '/' && path.size() < 4096 &&
                           path.find('\0') == std::string::npos && ksud::text::valid_utf8(path));
}
}  // namespace
bool parse_arguments(const std::vector<std::string>& args, Request& request, std::string& error) {
    Request parsed;
    const auto fail = [&] {
        error = "invalid_profile_arguments";
        return false;
    };
    if (args.size() < 2 || args[0] != "profile")
        return fail();
    parsed.action = args[1];
    if (parsed.action == "list") {
        if (args.size() != 2)
            return fail();
    } else if (parsed.action == "show" || parsed.action == "export") {
        if (args.size() != 3 || !valid_profile_id(args[2]))
            return fail();
        parsed.id = args[2];
    } else if (parsed.action == "preview") {
        if (args.size() != 5 || !valid_profile_id(args[2]) || args[3] != "--resources" ||
            !scopes(args[4], parsed.scopes))
            return fail();
        parsed.id = args[2];
    } else if (parsed.action == "import-preview") {
        if (args.size() != 6 || args[2] != "--input" || !input_path_valid(args[3]) ||
            args[4] != "--resources" || !scopes(args[5], parsed.scopes))
            return fail();
        parsed.input_path = args[3];
    } else if (parsed.action == "capture" || parsed.action == "import" ||
               parsed.action == "clone" || parsed.action == "rename" || parsed.action == "remove") {
        if (args.size() != 4 || args[2] != "--input" || !input_path_valid(args[3]))
            return fail();
        parsed.input_path = args[3];
    } else
        return fail();
    request = std::move(parsed);
    error.clear();
    return true;
}
bool read_profile_input(const std::string& path, std::string& bytes, std::string& error) {
    if (!input_path_valid(path)) {
        error = "invalid_input_path";
        return false;
    }
    const bool standard_input = path == "-";
    class Fd {
        int value;
        int old_flags = -1;

    public:
        explicit Fd(int descriptor) : value(descriptor) {}
        Fd(const Fd&) = delete;
        Fd& operator=(const Fd&) = delete;
        Fd(Fd&&) = delete;
        Fd& operator=(Fd&&) = delete;
        [[nodiscard]] int get() const { return value; }
        bool make_nonblocking() {
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
        ~Fd() {
            if (value < 0)
                return;
            if (old_flags >= 0)
                (void)::fcntl(value, F_SETFL, old_flags);
            ::close(value);
        }
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
    if (static_cast<std::uint64_t>(metadata.st_size) > PROFILE_INPUT_MAX_BYTES)
        return fail("input_capacity");
    if (standard_input) {
        if (!fd.make_nonblocking())
            return fail("input_nonblocking_failed");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::string value;
    char buffer[4096];
    while (value.size() <= PROFILE_INPUT_MAX_BYTES) {
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
            fd.get(), buffer, std::min(sizeof(buffer), PROFILE_INPUT_MAX_BYTES + 1 - value.size()));
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count < 0)
            return fail("input_read_failed");
        if (!count) {
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
}  // namespace sumhp::profiles
