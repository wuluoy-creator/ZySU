#include "core/log.hpp"
#include "core/log_repeat.hpp"
#include "core/runtime.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <optional>
#include <sstream>

namespace sumhp::logging {
namespace {
std::atomic<bool> debug_enabled{false};
constexpr std::time_t first_wall_clock = 1577836800;
class Fd {
public:
    explicit Fd(int value = -1) noexcept : value_(value) {}
    ~Fd() {
        if (value_ >= 0)
            close(value_);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&&) = delete;
    Fd& operator=(Fd&&) = delete;
    [[nodiscard]] int get() const noexcept { return value_; }
    bool close_checked() {
        const int value = value_;
        value_ = -1;
        return value < 0 || close(value) == 0;
    }

private:
    int value_;
};
class ErrnoGuard {
public:
    ErrnoGuard() = default;
    ErrnoGuard(const ErrnoGuard&) = delete;
    ErrnoGuard& operator=(const ErrnoGuard&) = delete;
    ErrnoGuard(ErrnoGuard&&) = delete;
    ErrnoGuard& operator=(ErrnoGuard&&) = delete;
    ~ErrnoGuard() { errno = saved_; }

private:
    int saved_ = errno;
};
struct Rotation {
    std::string boot;
    char phase = 'R';
    unsigned long long device = 0, inode = 0;
};

bool write_all(int fd, const std::string& text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const auto count = ::write(fd, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            if (count == 0)
                errno = EIO;
            return false;
        }
        offset += static_cast<size_t>(count);
    }
    return true;
}
bool regular(int fd, struct stat* result = nullptr) {
    struct stat st{};
    if (fstat(fd, &st) != 0)
        return false;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size < 0) {
        errno = EINVAL;
        return false;
    }
    if (result)
        *result = st;
    return true;
}
bool sync_fd(int fd) {
    int result;
    do {
        result = fsync(fd);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}
bool inspect_file(const std::string& path, struct stat& st, bool& exists) {
    exists = lstat(path.c_str(), &st) == 0;
    if (!exists)
        return errno == ENOENT;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size < 0) {
        errno = EINVAL;
        return false;
    }
    const Fd file{open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW)};
    return file.get() >= 0 && regular(file.get()) && fchmod(file.get(), 0600) == 0;
}
bool discard_stage(const std::string& stage) {
    struct stat st{};
    bool exists;
    if (!inspect_file(stage, st, exists))
        return false;
    return !exists || unlink(stage.c_str()) == 0;
}

// One fixed, private stage is shared by log compaction and the boot marker.
// The caller holds the common log flock. Pre-rename failure preserves the old
// destination; post-rename failure stops this append without pretending rollback.
bool replace_locked(const std::string& path, const std::string& content, const std::string& stage) {
    if (content.size() > file_limit) {
        errno = EFBIG;
        return false;
    }
    const auto parent = std::filesystem::path(path).parent_path();
    Fd directory{open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (directory.get() < 0 || !discard_stage(stage))
        return false;
    Fd file{open(stage.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW,
                 0600)};
    if (file.get() < 0)
        return false;
    bool ok = regular(file.get()) && fchmod(file.get(), 0600) == 0 &&
              write_all(file.get(), content) && sync_fd(file.get());
    int saved = errno;
    if (!file.close_checked() && ok) {
        ok = false;
        saved = errno;
    }
    if (ok && rename(stage.c_str(), path.c_str()) != 0) {
        ok = false;
        saved = errno;
    }
    if (!ok) {
        (void)unlink(stage.c_str());
        errno = saved;
        return false;
    }
    if (!sync_fd(directory.get()))
        return false;
    return directory.close_checked();
}
bool save_rotation(const std::string& path, const Rotation& state) {
    const std::string record = state.boot + " " + state.phase + " " + std::to_string(state.device) +
                               " " + std::to_string(state.inode) + "\n";
    return replace_locked(path + ".boot", record, path + ".stage");
}
bool read_exact_at(int fd, char* data, size_t size, off_t start) {
    size_t offset = 0;
    while (offset < size) {
        const auto count =
            pread(fd, data + offset, size - offset, start + static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            if (count == 0)
                errno = EAGAIN;
            return false;
        }
        offset += static_cast<size_t>(count);
    }
    return true;
}

bool compact_locked(const std::string& path, const std::string& stage) {
    const Fd file{open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW)};
    struct stat before{};
    if (file.get() < 0 || !regular(file.get(), &before))
        return false;
    const off_t start = std::max<off_t>(0, before.st_size - static_cast<off_t>(tail_limit));
    std::string tail(static_cast<size_t>(before.st_size - start), '\0');
    if (!read_exact_at(file.get(), tail.data(), tail.size(), start))
        return false;
    if (start > 0) {
        char previous;
        if (!read_exact_at(file.get(), &previous, 1, start - 1))
            return false;
        if (previous != '\n') {
            const auto newline = tail.find('\n');
            tail.erase(0, newline == std::string::npos ? tail.size() : newline + 1);
        }
    }
    const auto last = tail.rfind('\n');
    tail.resize(last == std::string::npos ? 0 : last + 1);
    struct stat after{};
    if (!regular(file.get(), &after))
        return false;
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec) {
        errno = EAGAIN;
        return false;
    }
    const std::string retained =
        timestamp() +
        " [WARN] [logging] older or incomplete records discarded to enforce log storage limit\n" +
        tail;
    return replace_locked(path, retained, stage);
}
bool maintain_limit(const std::string& path, const std::string& stage) {
    struct stat st{};
    bool exists;
    if (!inspect_file(path, st, exists))
        return false;
    return !exists || static_cast<unsigned long long>(st.st_size) <= file_limit ||
           compact_locked(path, stage);
}

bool valid_boot(const std::string& boot) {
    return !boot.empty() && boot.size() <= 128 &&
           boot.find_first_of(" \r\n\t") == std::string::npos &&
           boot.find('\0') == std::string::npos;
}
bool prepare_locked(const std::string& path, const std::string& boot, std::string& error) {
    if (!valid_boot(boot)) {
        error = "boot identity unavailable";
        errno = EINVAL;
        return false;
    }
    if (!discard_stage(path + ".stage"))
        return false;
    Rotation state;
    const Fd previous{
        open((path + ".boot").c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW)};
    if (previous.get() >= 0) {
        struct stat st{};
        if (!regular(previous.get(), &st) || fchmod(previous.get(), 0600) != 0)
            return false;
        if (st.st_size <= 0 || st.st_size >= 256) {
            errno = EINVAL;
            return false;
        }
        std::string record(static_cast<size_t>(st.st_size), '\0');
        if (!read_exact_at(previous.get(), record.data(), record.size(), 0))
            return false;
        std::istringstream input(record);
        std::string extra;
        if (!(input >> state.boot >> state.phase >> state.device >> state.inode) ||
            !valid_boot(state.boot) || (state.phase != 'R' && state.phase != 'P') ||
            (input >> extra)) {
            errno = EINVAL;
            return false;
        }
    } else if (errno != ENOENT)
        return false;
    struct stat st{};
    struct stat history{};
    bool exists;
    bool history_exists;
    if (!inspect_file(path, st, exists) || !inspect_file(path + ".old", history, history_exists))
        return false;
    if (state.boot != boot) {
        state = {boot, 'P', exists ? static_cast<unsigned long long>(st.st_dev) : 0,
                 exists ? static_cast<unsigned long long>(st.st_ino) : 0};
        if (!save_rotation(path, state))
            return false;
    }
    // Resolve pending boot rotation before size maintenance can replace its inode.
    if (state.phase == 'P' && exists &&
        state.device == static_cast<unsigned long long>(st.st_dev) &&
        state.inode == static_cast<unsigned long long>(st.st_ino)) {
        if (rename(path.c_str(), (path + ".old").c_str()) != 0)
            return false;
    }
    const Fd file{open(path.c_str(),
                       O_CREAT | O_WRONLY | O_APPEND | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (file.get() < 0 || !regular(file.get()) || fchmod(file.get(), 0600) != 0)
        return false;
    if (state.phase == 'P') {
        state.phase = 'R';
        if (!save_rotation(path, state))
            return false;
    }
    return maintain_limit(path, path + ".stage") && maintain_limit(path + ".old", path + ".stage");
}
const char* name(Level level) {
    switch (level) {
    case Level::Debug:
        return "DEBUG";
    case Level::Info:
        return "INFO";
    case Level::Warning:
        return "WARN";
    case Level::Error:
        return "ERROR";
    }
    return "ERROR";
}

// Bound encoded bytes, preserving complete UTF-8 sequences and escaping record
// separators. Invalid byte sequences become '?' rather than creating bad UTF-8.
std::string clean(const std::string& text, size_t limit) {
    constexpr char suffix[] = "...[truncated]";
    std::string output;
    for (size_t offset = 0; offset < text.size();) {
        const auto c = static_cast<unsigned char>(text[offset]);
        size_t length = 1;
        std::string token;
        if (c == '\n')
            token = "\\n";
        else if (c == '\r')
            token = "\\r";
        else if (c < 0x20 && c != '\t')
            token = "?";
        else if (c < 0x80)
            token.assign(1, static_cast<char>(c));
        else {
            if (c >= 0xc2 && c <= 0xdf)
                length = 2;
            else if (c >= 0xe0 && c <= 0xef)
                length = 3;
            else if (c >= 0xf0 && c <= 0xf4)
                length = 4;
            else
                length = 0;
            bool valid = length && offset + length <= text.size();
            for (size_t i = 1; valid && i < length; ++i)
                valid = (static_cast<unsigned char>(text[offset + i]) & 0xc0) == 0x80;
            if (valid && length >= 3) {
                const auto second = static_cast<unsigned char>(text[offset + 1]);
                valid = (c != 0xe0 || second >= 0xa0) && (c != 0xed || second < 0xa0) &&
                        (c != 0xf0 || second >= 0x90) && (c != 0xf4 || second < 0x90);
            }
            if (valid)
                token = text.substr(offset, length);
            else {
                length = 1;
                token = "?";
            }
        }
        if (output.size() + token.size() > limit) {
            size_t end = std::min(output.size(), limit - (sizeof(suffix) - 1));
            while (end > 0 && end < output.size() &&
                   (static_cast<unsigned char>(output[end]) & 0xc0) == 0x80)
                --end;
            output.resize(end);
            output += suffix;
            break;
        }
        output += token;
        offset += length;
    }
    return output;
}
bool append_locked(const std::string& path, Level level, const std::string& component,
                   const std::string& message) {
    std::string line = timestamp() + " [" + name(level) + "] [" +
                       clean(component, component_limit) + "] " + clean(message, message_limit) +
                       "\n";
    Fd file{open(path.c_str(), O_RDWR | O_APPEND | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW)};
    struct stat st{};
    if (file.get() < 0 || !regular(file.get(), &st))
        return false;
    if (st.st_size > 0) {
        char last;
        if (!read_exact_at(file.get(), &last, 1, st.st_size - 1))
            return false;
        if (last != '\n')
            line.insert(0, 1, '\n');
    }
    if (static_cast<unsigned long long>(st.st_size) + line.size() > file_limit) {
        if (!file.close_checked() || !compact_locked(path, path + ".stage"))
            return false;
        const Fd compacted{
            open(path.c_str(), O_WRONLY | O_APPEND | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW)};
        if (compacted.get() < 0 || !regular(compacted.get(), &st) ||
            static_cast<unsigned long long>(st.st_size) + line.size() > file_limit) {
            errno = EFBIG;
            return false;
        }
        return write_all(compacted.get(), line);
    }
    return write_all(file.get(), line);
}
std::optional<std::uint64_t> monotonic_ms() {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 || now.tv_nsec < 0 ||
        now.tv_nsec >= 1000000000L)
        return std::nullopt;
    return (static_cast<std::uint64_t>(now.tv_sec) * 1000) +
           (static_cast<std::uint64_t>(now.tv_nsec) / 1000000);
}
struct RepeatContext {
    RepeatFilter filter;
    std::string boot;
    pid_t process = 0;
    struct stat observed{};
    bool known = false;
};
RepeatContext repeat;
void observe(const std::string& path, const std::string& boot, bool reset_changed) {
    struct stat st{};
    const bool known = lstat(path.c_str(), &st) == 0;
    if (reset_changed &&
        (!known || !repeat.known || repeat.boot != boot || repeat.process != getpid() ||
         st.st_dev != repeat.observed.st_dev || st.st_ino != repeat.observed.st_ino ||
         st.st_size != repeat.observed.st_size ||
         st.st_mtim.tv_sec != repeat.observed.st_mtim.tv_sec ||
         st.st_mtim.tv_nsec != repeat.observed.st_mtim.tv_nsec))
        repeat.filter.reset();
    repeat.boot = boot;
    repeat.process = getpid();
    repeat.observed = st;
    repeat.known = known;
}
bool open_lock_and_prepare(const std::string& path, int fd, const std::string& boot,
                           std::string& error) {
    return fd >= 0 && regular(fd) && fchmod(fd, 0600) == 0 && flock(fd, LOCK_EX) == 0 &&
           prepare_locked(path, boot, error);
}
}  // namespace

void set_debug_enabled(bool value) noexcept {
    debug_enabled.store(value);
}
bool enabled(Level level) noexcept {
    return level != Level::Debug || debug_enabled.load();
}

std::string timestamp() {
    const auto now = std::time(nullptr);
    char buffer[64]{};
    if (now >= first_wall_clock) {
        tm local{};
        if (localtime_r(&now, &local) &&
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local) != 0)
            return buffer;
    }
#if defined(CLOCK_BOOTTIME)
    timespec boot{};
    if (clock_gettime(CLOCK_BOOTTIME, &boot) == 0) {
        const int written =
            std::snprintf(buffer, sizeof(buffer), "early-boot +%lld.%03ld",
                          static_cast<long long>(boot.tv_sec), boot.tv_nsec / 1000000L);
        if (written > 0 && static_cast<size_t>(written) < sizeof(buffer))
            return buffer;
    }
#endif
    return "early-boot (clock unset)";
}
bool prepare_boot_log(std::string& error, const std::string& boot_id) {
    const ErrnoGuard saved;
    error.clear();
    const auto path = runtime_log_file();
    if (!prepare_private_directory(path.parent_path(), error))
        return false;
    const Fd lock{open((path.string() + ".lock").c_str(),
                       O_CREAT | O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (!open_lock_and_prepare(path.string(), lock.get(),
                               boot_id.empty() ? runtime_boot_id() : boot_id, error)) {
        if (error.empty())
            error = "prepare " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    return true;
}
void write(Level level, const std::string& component, const std::string& message) noexcept {
    const ErrnoGuard saved;
    if (!enabled(level))
        return;
    try {
        const auto path = runtime_log_file();
        const auto boot = runtime_boot_id();
        std::string error;
        if (!prepare_private_directory(path.parent_path(), error)) {
            (void)std::fprintf(stderr, "[ERROR] [logging] %s\n", error.c_str());
            return;
        }
        const Fd lock{open((path.string() + ".lock").c_str(),
                           O_CREAT | O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW, 0600)};
        if (!open_lock_and_prepare(path.string(), lock.get(), boot, error)) {
            (void)std::fprintf(stderr, "[ERROR] [logging] %s: %s\n", path.c_str(),
                               std::strerror(errno));
            return;
        }
        // Separate flock opens serialize both threads and cooperating processes.
        // The small repeat table is process-local; foreign writes/clear reset it.
        observe(path.string(), boot, true);
        const bool ok = repeat.filter.write(
            level, clean(component, component_limit), clean(message, message_limit), monotonic_ms(),
            [&](Level severity, const std::string& tag, const std::string& text) {
                return append_locked(path.string(), severity, tag, text);
            });
        const int error_number = errno;
        observe(path.string(), boot, false);
        if (!ok)
            (void)std::fprintf(stderr, "[ERROR] [logging] append %s: %s\n", path.c_str(),
                               std::strerror(error_number));
    } catch (...) {
        (void)std::fputs("[ERROR] [logging] log write failed\n", stderr);
    }
}
void flush() noexcept {
    const ErrnoGuard saved;
    try {
        const auto path = runtime_log_file();
        const auto boot = runtime_boot_id();
        std::string error;
        if (!prepare_private_directory(path.parent_path(), error))
            return;
        const Fd lock{open((path.string() + ".lock").c_str(),
                           O_CREAT | O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW, 0600)};
        if (!open_lock_and_prepare(path.string(), lock.get(), boot, error))
            return;
        observe(path.string(), boot, true);
        (void)repeat.filter.flush(
            [&](Level level, const std::string& component, const std::string& message) {
                return append_locked(path.string(), level, component, message);
            });
        observe(path.string(), boot, false);
    } catch (...) {
        // Shutdown summaries are best effort, never recursive.
        return;
    }
}
void append(const std::string& component, const std::string& message) {
    write(Level::Info, component, message);
}
}  // namespace sumhp::logging
