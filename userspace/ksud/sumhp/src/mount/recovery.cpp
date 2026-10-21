#include "mount/backend.hpp"

#include "core/json.hpp"
#include "core/runtime.hpp"
#include "utils.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <sstream>

namespace sumhp::mount {
namespace {
namespace fs = std::filesystem;
constexpr int kMaxBootAttempts = 3;
constexpr size_t kMaxRecoveryRecord = 4096;

fs::path recovery_path(const char* name) {
    return runtime_data_dir() / "run" / name;
}

bool storage_error(const std::string& operation, const fs::path& path, std::string& error) {
    error = operation + " " + path.string() + ": " + std::strerror(errno);
    return false;
}

class RecoveryLock {
public:
    RecoveryLock() = default;
    RecoveryLock(const RecoveryLock&) = delete;
    RecoveryLock& operator=(const RecoveryLock&) = delete;
    RecoveryLock(RecoveryLock&&) = delete;
    RecoveryLock& operator=(RecoveryLock&&) = delete;
    ~RecoveryLock() {
        if (fd_ >= 0)
            close(fd_);
    }
    bool acquire(bool mutate, std::string& error) {
        const auto path = recovery_path("recovery.lock");
        if (mutate && !prepare_private_directory(path.parent_path(), error))
            return false;
        fd_ = open(path.c_str(),
                   O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (mutate ? O_RDWR | O_CREAT : O_RDONLY),
                   0600);
        if (fd_ < 0) {
            if (!mutate && errno == ENOENT)
                return true;
            return storage_error("open recovery lock", path, error);
        }
        struct stat st{};
        if (fstat(fd_, &st) != 0)
            return storage_error("stat recovery lock", path, error);
        if (!S_ISREG(st.st_mode) || st.st_nlink != 1) {
            error = "not a private recovery lock: " + path.string();
            return false;
        }
        int result;
        do {
            result = flock(fd_, mutate ? LOCK_EX : LOCK_SH);
        } while (result != 0 && errno == EINTR);
        return result == 0 || storage_error("lock recovery state", path, error);
    }

private:
    int fd_ = -1;
};

enum class ReadState : std::uint8_t { Missing, Valid, Corrupt, Unreadable };

const char* state_name(ReadState state) {
    switch (state) {
    case ReadState::Missing:
        return "missing";
    case ReadState::Valid:
        return "valid";
    case ReadState::Corrupt:
        return "corrupt";
    case ReadState::Unreadable:
        return "unreadable";
    }
    return "unreadable";
}

ReadState read_record(const fs::path& path, std::string& content, std::string& error) {
    content.clear();
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT)
            return ReadState::Missing;
        storage_error("read recovery state", path, error);
        return ReadState::Unreadable;
    }
    struct stat st{};
    if (fstat(fd, &st) != 0) {
        const int saved = errno;
        close(fd);
        errno = saved;
        storage_error("stat recovery state", path, error);
        return ReadState::Unreadable;
    }
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size < 0 ||
        static_cast<uint64_t>(st.st_size) > kMaxRecoveryRecord) {
        close(fd);
        error = "invalid recovery record type or size: " + path.string();
        return ReadState::Corrupt;
    }
    char buffer[kMaxRecoveryRecord + 1];
    size_t used = 0;
    while (used < sizeof(buffer)) {
        const ssize_t count = read(fd, buffer + used, sizeof(buffer) - used);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            const int saved = errno;
            close(fd);
            errno = saved;
            storage_error("read recovery state", path, error);
            return ReadState::Unreadable;
        }
        if (count == 0)
            break;
        used += static_cast<size_t>(count);
    }
    if (close(fd) != 0) {
        storage_error("close recovery state", path, error);
        return ReadState::Unreadable;
    }
    if (used > kMaxRecoveryRecord) {
        error = "recovery record exceeds size limit: " + path.string();
        return ReadState::Corrupt;
    }
    content.assign(buffer, used);
    return ReadState::Valid;
}

struct RecoveryRecord {
    int attempts = 0;
    std::string boot_id;
    std::string phase;
    std::string reason;
};

bool parse_record(const std::string& content, bool disabled, RecoveryRecord& record) {
    // Old versions stored an integer counter and an empty/newline disable marker.
    if (disabled && (content.empty() || content == "\n"))
        return true;
    const auto newline = content.find('\n');
    const auto first = content.substr(0, newline);
    const auto parsed = std::from_chars(first.data(), first.data() + first.size(), record.attempts);
    if (parsed.ec != std::errc{} || parsed.ptr != first.data() + first.size() ||
        record.attempts < 0)
        return false;
    if (newline == std::string::npos || newline + 1 == content.size())
        return !disabled;
    std::istringstream lines(content.substr(newline + 1));
    std::string version;
    std::string trailing;
    return std::getline(lines, version) && version == "sumhp-recovery-v1" &&
           std::getline(lines, record.boot_id) && !record.boot_id.empty() &&
           std::getline(lines, record.phase) && !record.phase.empty() &&
           std::getline(lines, record.reason) && !record.reason.empty() &&
           !std::getline(lines, trailing);
}

ReadState load_record(const char* name, bool disabled, RecoveryRecord& record, std::string& error) {
    std::string content;
    auto state = read_record(recovery_path(name), content, error);
    if (state == ReadState::Valid && !parse_record(content, disabled, record)) {
        error = "corrupt recovery record: " + recovery_path(name).string();
        state = ReadState::Corrupt;
    }
    return state;
}

bool healthy(ReadState state) {
    return state == ReadState::Valid || state == ReadState::Missing;
}

bool current_boot(std::string& boot_id, std::string& error) {
    boot_id = runtime_boot_id();
    if (boot_id.empty() || boot_id.size() > 128 ||
        boot_id.find_first_of("\r\n\0", 0, 3) != std::string::npos) {
        error = "cannot read a valid current boot ID; automatic mounting remains disabled";
        return false;
    }
    return true;
}

bool commit_record(const char* name, int attempts, const std::string& boot_id, const char* phase,
                   const char* reason, std::string& error) {
    const auto path = recovery_path(name);
    // The first line remains compatible with the old integer-counter reader.
    const std::string content = std::to_string(attempts) + "\nsumhp-recovery-v1\n" + boot_id +
                                "\n" + phase + "\n" + reason + "\n";
    if (!ksud::write_file_durable(path, content, error, 0600)) {
        error = "commit recovery state: " + error;
        return false;
    }
    std::string actual;
    if (read_record(path, actual, error) != ReadState::Valid || actual != content) {
        if (error.empty())
            error = "recovery state readback mismatch: " + path.string();
        return false;
    }
    return true;
}
}  // namespace

bool recovery_begin_mount(bool& disabled, std::string& error) {
    error.clear();
    disabled = true;  // Any storage failure must stop automatic mounting.
    RecoveryLock lock;
    if (!lock.acquire(true, error))
        return false;
    RecoveryRecord attempts;
    RecoveryRecord marker;
    if (!healthy(load_record("boot_attempts", false, attempts, error)))
        return false;
    const auto marker_state = load_record("mount_disabled", true, marker, error);
    if (!healthy(marker_state))
        return false;
    if (marker_state == ReadState::Valid) {
        error = "mounting disabled by bootloop protection; run 'ksud sumhp recovery reset'";
        return true;
    }
    std::string boot_id;
    if (!current_boot(boot_id, error))
        return false;
    if (attempts.attempts >= kMaxBootAttempts) {
        if (!commit_record("mount_disabled", attempts.attempts, boot_id, "blocked",
                           "unconfirmed-boot-limit", error))
            return false;
        error = "bootloop protection tripped after " + std::to_string(attempts.attempts) +
                " unconfirmed boots; run 'ksud sumhp recovery reset'";
        return true;
    }
    if (!commit_record("boot_attempts", attempts.attempts + 1, boot_id, "mount-attempt",
                       "awaiting-boot-completed", error))
        return false;
    disabled = false;
    return true;
}

bool recovery_boot_completed(std::string& error) {
    error.clear();
    RecoveryLock lock;
    if (!lock.acquire(true, error))
        return false;
    std::string boot_id;
    std::string orchestrator;
    if (!current_boot(boot_id, error))
        return false;
    const auto state = read_record(recovery_path("mount_orchestrator_boot"), orchestrator, error);
    if (!healthy(state))
        return false;
    if (state == ReadState::Missing || orchestrator.substr(0, orchestrator.find('\n')) != boot_id)
        return true;
    RecoveryRecord attempts;
    if (!healthy(load_record("boot_attempts", false, attempts, error)))
        return false;
    // Confirmation clears only the counter. A tripped disable marker always
    // requires an explicit recovery reset, including after a successful boot.
    return commit_record("boot_attempts", 0, boot_id, "boot-completed", "boot-confirmed", error);
}

bool recovery_reset(std::string& error) {
    error.clear();
    RecoveryLock lock;
    std::string boot_id;
    if (!lock.acquire(true, error) || !current_boot(boot_id, error))
        return false;
    // Persist zero before removing the disable marker. An interrupted reset
    // therefore remains disabled, or is fully safe to retry.
    if (!commit_record("boot_attempts", 0, boot_id, "reset", "user-request", error))
        return false;
    const auto path = recovery_path("mount_disabled");
    if (unlink(path.c_str()) != 0 && errno != ENOENT)
        return storage_error("remove recovery disable marker", path, error);
    if (!sync_private_directory(path.parent_path(), error)) {
        error = "recovery marker removed but durability is unconfirmed: " + error;
        return false;
    }
    std::string content;
    if (read_record(path, content, error) != ReadState::Missing) {
        if (error.empty())
            error = "recovery disable marker still present after reset";
        return false;
    }
    return true;
}

std::string recovery_status_json(bool* ok) {
    std::string error;
    RecoveryLock lock;
    const bool locked = lock.acquire(false, error);
    RecoveryRecord attempts;
    RecoveryRecord marker;
    const auto attempts_state =
        locked ? load_record("boot_attempts", false, attempts, error) : ReadState::Unreadable;
    std::string marker_error;
    const auto marker_state =
        locked ? load_record("mount_disabled", true, marker, marker_error) : ReadState::Unreadable;
    if (!marker_error.empty())
        error += (error.empty() ? "" : "; ") + marker_error;
    const bool valid = locked && healthy(attempts_state) && healthy(marker_state);
    if (ok)
        *ok = valid;
    const auto& record = marker_state == ReadState::Valid ? marker : attempts;
    std::ostringstream out;
    out << "{\"ok\":" << (valid ? "true" : "false") << ",\"boot_attempts\":";
    if (healthy(attempts_state))
        out << attempts.attempts;
    else
        out << "null";
    out << ",\"max_attempts\":" << kMaxBootAttempts << ",\"mounting_disabled\":";
    if (marker_state == ReadState::Unreadable)
        out << "null";
    else
        out << (marker_state == ReadState::Missing ? "false" : "true");
    out << ",\"automatic_mount_allowed\":"
        << (valid && marker_state == ReadState::Missing && attempts.attempts < kMaxBootAttempts
                ? "true"
                : "false")
        << ",\"counter_state\":" << json_quote(state_name(attempts_state))
        << ",\"disable_marker_state\":" << json_quote(state_name(marker_state))
        << ",\"boot_id\":" << json_quote(record.boot_id)
        << ",\"phase\":" << json_quote(record.phase) << ",\"reason\":" << json_quote(record.reason)
        << ",\"error\":" << json_quote(error) << "}";
    return out.str();
}
}  // namespace sumhp::mount
