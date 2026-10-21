#include "core/runtime.hpp"
#include "core/log.hpp"
#include "core/migration.hpp"
#include "defs.hpp"
#include "utils.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#if defined(__ANDROID__)
#include <sys/xattr.h>
#endif
#include "sumhp/embedded_paths.hpp"

namespace sumhp {

namespace fs = std::filesystem;

fs::path runtime_data_dir() {
    return embedded_data_dir;
}

fs::path runtime_modules_dir() {
    return fs::path(ksud::MODULE_DIR).lexically_normal();
}

fs::path runtime_config_file() {
    return runtime_data_dir() / "config.json";
}

fs::path runtime_log_file() {
    return embedded_log_file;
}

fs::path runtime_socket_file() {
    return runtime_data_dir() / "sumhpd.sock";
}
fs::path runtime_pid_file() {
    return runtime_data_dir() / "sumhpd.pid";
}
fs::path runtime_daemon_lock_file() {
    return runtime_data_dir() / "sumhpd.lock";
}
std::string runtime_boot_id() {
    std::ifstream input("/proc/sys/kernel/random/boot_id");
    std::string id;
    std::getline(input, id);
    return id;
}

bool marker_matches_current_boot(const fs::path& path) {
    const std::string boot_id = runtime_boot_id();
    std::ifstream input(path);
    std::string recorded;
    return !boot_id.empty() && std::getline(input, recorded) && recorded == boot_id;
}

bool write_boot_marker(const fs::path& path, const std::string& detail) {
    const std::string boot_id = runtime_boot_id();
    if (boot_id.empty()) {
        errno = EIO;
        return false;
    }
    std::string error;
    if (ksud::write_file_durable(path, boot_id + "\n" + (detail.empty() ? "" : detail + "\n"),
                                 error))
        return true;
    logging::write(logging::Level::Error, "runtime", "write boot marker: " + error);
    return false;
}

namespace {
class RuntimeFileLock {
public:
    RuntimeFileLock() = default;
    RuntimeFileLock(const RuntimeFileLock&) = delete;
    RuntimeFileLock& operator=(const RuntimeFileLock&) = delete;
    RuntimeFileLock(RuntimeFileLock&&) = delete;
    RuntimeFileLock& operator=(RuntimeFileLock&&) = delete;
    ~RuntimeFileLock() {
        if (fd_ >= 0)
            close(fd_);
    }

    bool acquire(const fs::path& path, bool create, std::string& error) {
        fd_ = open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT : 0), 0600);
        if (fd_ < 0) {
            if (!create && errno == ENOENT)
                return true;
            error = "open migration lock " + path.string() + ": " + std::strerror(errno);
            return false;
        }
        struct stat st{};
        if (fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
            error = "not a private migration lock: " + path.string();
            return false;
        }
        if (flock(fd_, LOCK_EX | (create ? 0 : LOCK_NB)) != 0) {
            error = "controller is busy or inaccessible; restart before migrating " +
                    path.string() + ": " + std::strerror(errno);
            return false;
        }
        return true;
    }

private:
    int fd_ = -1;
};

bool metadata(const fs::path& path, mode_t mode, std::string& error) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) {
        error = "lstat " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        error = "refusing metadata symlink: " + path.string();
        return false;
    }
    if (((st.st_mode & 07777) != mode && chmod(path.c_str(), mode) != 0) ||
        (geteuid() == 0 && (st.st_uid != 0 || st.st_gid != 0) && chown(path.c_str(), 0, 0) != 0)) {
        error = "secure " + path.string() + ": " + std::strerror(errno);
        return false;
    }
#if defined(__ANDROID__)
    constexpr char context[] = "u:object_r:adb_data_file:s0";
    char current[256]{};
    const ssize_t length =
        lgetxattr(path.c_str(), "security.selinux", current, sizeof(current) - 1);
    if (length < 0 || std::strcmp(current, context) != 0) {
        if (lsetxattr(path.c_str(), "security.selinux", context, sizeof(context), 0) != 0) {
            error = "label " + path.string() + ": " + std::strerror(errno);
            return false;
        }
    }
#endif
    return true;
}
}  // namespace

bool prepare_private_directory(const fs::path& path, std::string& error) {
    error.clear();
    const auto normalized = path.lexically_normal();
    if (normalized.empty() || !normalized.is_absolute() || normalized == normalized.root_path() ||
        normalized == "/data" || normalized == "/data/adb" || normalized == "/data/adb/ksu" ||
        normalized == "/data/adb/modules" || normalized == "/tmp") {
        error = "refusing a shared runtime directory: " + path.string();
        return false;
    }
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) {
        error = "mkdir " + path.string() + ": " + ec.message();
        return false;
    }
    return metadata(path, 0700, error);
}

bool prepare_private_file(const fs::path& path, std::string& error) {
    error.clear();
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT)
            return true;
        error = "lstat " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    if ((!S_ISREG(st.st_mode) && !S_ISSOCK(st.st_mode)) || st.st_nlink != 1) {
        error = "not a private controller file: " + path.string();
        return false;
    }
    return metadata(path, 0600, error);
}

bool sync_private_directory(const fs::path& path, std::string& error) {
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        error = "open migration directory " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    int result;
    do {
        result = fsync(fd);
    } while (result < 0 && errno == EINTR);
    const int sync_error = errno;
    close(fd);
    if (result != 0) {
        errno = sync_error;
        error = "sync migration directory " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

bool prepare_runtime(std::string& error) {
    error.clear();
    if (!prepare_private_directory(runtime_data_dir(), error) ||
        !prepare_private_directory(runtime_data_dir() / "run", error) ||
        !prepare_private_directory(runtime_log_file().parent_path(), error))
        return false;
    // Use the same lock as config writers, so startup cannot race a manager
    // update while upgrading persisted backend names.
    RuntimeFileLock config_lock;
    if (!config_lock.acquire(runtime_config_file().string() + ".lock", true, error))
        return false;
    const auto legacy_dir = runtime_data_dir().parent_path() / "kagami";
    std::error_code ec;
    const auto legacy_status = fs::symlink_status(legacy_dir, ec);
    if ((ec && ec != std::errc::no_such_file_or_directory) ||
        (fs::exists(legacy_status) && !fs::is_directory(legacy_status))) {
        error = "not an accessible legacy state directory: " + legacy_dir.string();
        return false;
    }
    // An old daemon must release ownership before its saved records are moved.
    RuntimeFileLock legacy_lock;
    if (!legacy_lock.acquire(legacy_dir / "kagamid.lock", false, error) ||
        !migrate_legacy_state(legacy_dir, runtime_data_dir(), error))
        return false;
    // These are controller records, not mounted module payloads or LKM assets.
    for (const auto* name : {"config.json", "config.json.lock", "module_mode.json",
                             "module_rules.json", "user_hide_rules.json", "sumhpd.pid",
                             "sumhpd.sock", "sumhpd.lock", "mirror.img", "mirror.erofs"}) {
        if (!prepare_private_file(runtime_data_dir() / name, error))
            return false;
    }
    return true;
}

}  // namespace sumhp
