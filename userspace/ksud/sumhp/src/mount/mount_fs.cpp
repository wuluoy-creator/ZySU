#include "mount/mount_fs.hpp"
#include <linux/stat.h>
#include <sys/syscall.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include "core/runtime.hpp"

#include "core/log.hpp"
#include "sumhp/embedded.hpp"
#include "utils.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <algorithm>
#include <climits>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace sumhp::mount::fsutil {

constexpr const char* kSelinuxXattr = "security.selinux";

const std::vector<std::string>& managed_partitions() {
    static const std::vector<std::string> partitions = {
        "system", "vendor", "product", "system_ext", "odm", "oem",
    };
    return partitions;
}

std::vector<std::string> managed_partitions(const std::vector<std::string>& extra) {
    auto partitions = managed_partitions();
    for (const auto& partition : extra) {
        if (!partition.empty() &&
            std::find(partitions.begin(), partitions.end(), partition) == partitions.end()) {
            partitions.push_back(partition);
        }
    }
    return partitions;
}

void mlog(const std::string& msg, logging::Level level) {
    logging::write(level, "mount", msg);
}

bool get_context(const std::string& path, std::string& out) {
    char buf[256];
    const ssize_t n = lgetxattr(path.c_str(), kSelinuxXattr, buf, sizeof(buf) - 1);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';
    out.assign(buf);
    return true;
}

bool set_context(const std::string& path, const std::string& ctx) {
    return lsetxattr(path.c_str(), kSelinuxXattr, ctx.c_str(), ctx.size() + 1, 0) == 0;
}

void clone_attr(const std::string& src, const std::string& dst) {
    struct stat st{};
    if (lstat(src.c_str(), &st) != 0) {
        return;
    }
    if (!S_ISLNK(st.st_mode)) {
        chmod(dst.c_str(), st.st_mode & 07777);
    }
    chown(dst.c_str(), st.st_uid, st.st_gid);
    std::string ctx;
    if (get_context(src, ctx)) {
        set_context(dst, ctx);
    }
}

bool bind_mount(const std::string& src, const std::string& dst) {
    return ::mount(src.c_str(), dst.c_str(), nullptr, MS_BIND, nullptr) == 0;
}

bool mirror_entry(const std::string& src, const std::string& dst) {
    struct stat st{};
    if (lstat(src.c_str(), &st) != 0) {
        mlog("lstat " + src + " failed: " + std::strerror(errno), logging::Level::Error);
        return false;
    }

    if (S_ISREG(st.st_mode)) {
        const int fd = open(dst.c_str(), O_CREAT | O_WRONLY | O_TRUNC, st.st_mode & 07777);
        if (fd < 0) {
            mlog("create placeholder " + dst + " failed: " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
        close(fd);
        if (!bind_mount(src, dst)) {
            mlog("bind " + src + " -> " + dst + " failed: " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
        return true;
    }

    if (S_ISLNK(st.st_mode)) {
        char tgt[PATH_MAX];
        const ssize_t len = readlink(src.c_str(), tgt, sizeof(tgt) - 1);
        if (len < 0) {
            mlog("readlink " + src + " failed: " + std::strerror(errno), logging::Level::Error);
            return false;
        }
        tgt[len] = '\0';
        if (symlink(tgt, dst.c_str()) != 0 && errno != EEXIST) {
            mlog("symlink " + dst + " failed: " + std::strerror(errno), logging::Level::Error);
            return false;
        }
        std::string ctx;
        if (get_context(src, ctx)) {
            set_context(dst, ctx);
        }
        return true;
    }

    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst.c_str(), st.st_mode & 07777) != 0 && errno != EEXIST) {
            mlog("mkdir " + dst + " failed: " + std::strerror(errno), logging::Level::Error);
            return false;
        }
        clone_attr(src, dst);
        DIR* d = opendir(src.c_str());
        if (!d) {
            mlog("opendir " + src + " failed: " + std::strerror(errno), logging::Level::Error);
            return false;
        }
        bool ok = true;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
                continue;
            }
            if (!mirror_entry(src + "/" + e->d_name, dst + "/" + e->d_name)) {
                ok = false;
                break;
            }
        }
        closedir(d);
        return ok;
    }

    return true;  // skip device/socket/fifo nodes
}

bool directory_is_opaque(const std::string& path, bool& opaque) {
    opaque = false;
    struct stat marker{};
    if (lstat((path + "/.replace").c_str(), &marker) == 0) {
        opaque = true;
        return true;
    }
    if (errno != ENOENT)
        return false;
    char value[8]{};
    const ssize_t count = lgetxattr(path.c_str(), "trusted.overlay.opaque", value, sizeof(value));
    if (count < 0) {
        if (errno == ENODATA || errno == EOPNOTSUPP)
            return true;
        mlog("read opaque attribute " + path + ": " + std::strerror(errno), logging::Level::Error);
        return false;
    }
    opaque = count == 1 && value[0] == 'y';
    return true;
}

bool stat_is_whiteout(const std::string& path, const struct stat& st, bool& whiteout) {
    whiteout = S_ISCHR(st.st_mode) && st.st_rdev == makedev(0, 0);
    if (S_ISREG(st.st_mode) && st.st_size == 0) {
        const ssize_t count = lgetxattr(path.c_str(), "trusted.overlay.whiteout", nullptr, 0);
        if (count >= 0)
            whiteout = true;
        else if (errno != ENODATA && errno != EOPNOTSUPP) {
            mlog("read whiteout attribute " + path + ": " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
    }
    return true;
}

bool path_is_whiteout(const std::string& path, bool& whiteout) {
    whiteout = false;
    struct stat st{};
    return lstat(path.c_str(), &st) == 0 && stat_is_whiteout(path, st, whiteout);
}

bool copy_tree(const std::string& src, const std::string& dst) {
    struct stat st{};
    if (lstat(src.c_str(), &st) != 0) {
        mlog("copy_tree lstat " + src + " failed: " + std::strerror(errno), logging::Level::Error);
        return false;
    }

    bool whiteout = false;
    if (!stat_is_whiteout(src, st, whiteout))
        return false;
    if (whiteout) {
        if (mknod(dst.c_str(), S_IFCHR | (st.st_mode & 07777), makedev(0, 0)) != 0) {
            mlog("create whiteout " + dst + ": " + std::strerror(errno), logging::Level::Error);
            return false;
        }
        clone_attr(src, dst);
        return true;
    }

    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst.c_str(), st.st_mode & 07777) != 0 && errno != EEXIST) {
            mlog("copy_tree mkdir " + dst + " failed: " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
        clone_attr(src, dst);
        bool opaque = false;
        if (!directory_is_opaque(src, opaque))
            return false;
        if (opaque && lsetxattr(dst.c_str(), "trusted.overlay.opaque", "y", 1, 0) != 0) {
            mlog("set opaque attribute " + dst + ": " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
        DIR* d = opendir(src.c_str());
        if (!d) {
            return false;
        }
        bool ok = true;
        struct dirent* e;
        for (;;) {
            errno = 0;
            e = readdir(d);
            if (!e) {
                ok = ok && errno == 0;
                break;
            }
            if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
                continue;
            }
            if (std::strcmp(e->d_name, ".replace") == 0)
                continue;
            if (!copy_tree(src + "/" + e->d_name, dst + "/" + e->d_name)) {
                ok = false;
            }
        }
        if (closedir(d) != 0)
            ok = false;
        return ok;
    }

    if (S_ISLNK(st.st_mode)) {
        char tgt[PATH_MAX];
        const ssize_t len = readlink(src.c_str(), tgt, sizeof(tgt) - 1);
        if (len < 0) {
            return false;
        }
        tgt[len] = '\0';
        unlink(dst.c_str());
        if (symlink(tgt, dst.c_str()) != 0 && errno != EEXIST) {
            return false;
        }
        std::string ctx;
        if (get_context(src, ctx)) {
            set_context(dst, ctx);
        }
        return true;
    }

    if (S_ISREG(st.st_mode)) {
        const bool ok = ksud::copy_file_data(src, dst, st.st_mode & 07777);
        if (!ok)
            mlog("copy_tree " + src + " -> " + dst + " failed: " + std::strerror(errno),
                 logging::Level::Error);
        clone_attr(src, dst);
        return ok;
    }

    return true;  // skip device/socket/fifo nodes
}

bool capture_mount_identity(const std::string& path, MountRecord& record) {
    struct statx st{};
    constexpr unsigned mask = STATX_INO | STATX_MNT_ID;
    if (syscall(__NR_statx, AT_FDCWD, path.c_str(), AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, mask,
                &st) != 0 ||
        (st.stx_mask & mask) != mask ||
        ((st.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT) &&
         !(st.stx_attributes & STATX_ATTR_MOUNT_ROOT)))
        return false;
    record = {path, st.stx_mnt_id,
              (static_cast<uint64_t>(st.stx_dev_major) << 32) | st.stx_dev_minor, st.stx_ino};
    return true;
}

bool mount_matches(const MountRecord& record, bool include_init) {
    MountRecord live;
    if (capture_mount_identity(record.path, live) && live.mount_id == record.mount_id &&
        live.device == record.device && live.inode == record.inode)
        return true;
    if (!include_init)
        return false;
    MountRecord init = record;
    init.path = "/proc/1/root" + record.path;
    return mount_matches(init, false);
}

bool read_mount_journal(const std::string& journal, std::vector<MountRecord>& records,
                        bool& legacy) {
    legacy = false;
    records.clear();
    std::ifstream input(journal);
    if (!input)
        return errno == ENOENT;
    std::string line;
    if (!std::getline(input, line))
        return input.eof();
    constexpr const char* prefix = "SUMHP_MOUNTS_V2 ";
    if (line.rfind(prefix, 0) != 0) {
        legacy = true;
        do {
            if (!line.empty()) {
                if (line.front() != '/' || line.find('\0') != std::string::npos)
                    return false;
                records.push_back({line});
            }
        } while (std::getline(input, line));
        return input.eof();
    }
    const auto boot = runtime_boot_id();
    if (boot.empty())
        return false;
    if (line.substr(std::strlen(prefix)) != boot)
        return true;
    while (std::getline(input, line)) {
        MountRecord record;
        std::istringstream row(line);
        if (!(row >> record.mount_id >> record.device >> record.inode >>
              std::quoted(record.path)) ||
            !(row >> std::ws).eof() || record.path.empty() || record.path.front() != '/' ||
            record.path.find_first_of("\r\n") != std::string::npos ||
            record.path.find('\0') != std::string::npos)
            return false;
        records.push_back(std::move(record));
    }
    return input.eof();
}

bool write_mount_journal(const std::string& journal, const std::vector<MountRecord>& records) {
    const auto boot = runtime_boot_id();
    if (boot.empty())
        return false;
    std::ostringstream data;
    data << "SUMHP_MOUNTS_V2 " << boot << "\n";
    for (const auto& record : records) {
        if (record.path.empty() || record.path.front() != '/' || !record.mount_id ||
            record.path.find_first_of("\r\n") != std::string::npos ||
            record.path.find('\0') != std::string::npos)
            return false;
        data << record.mount_id << " " << record.device << " " << record.inode << " "
             << std::quoted(record.path) << "\n";
    }
    std::string error;
    if (ksud::write_file_durable(journal, data.str(), error))
        return true;
    mlog("write mount journal: " + error, logging::Level::Error);
    return false;
}

bool write_mount_journal(const std::string& journal, const std::vector<std::string>& mounts) {
    std::vector<MountRecord> records;
    records.reserve(mounts.size());
    for (const auto& path : mounts) {
        MountRecord record;
        if (!capture_mount_identity(path, record))
            return false;
        records.push_back(std::move(record));
    }
    return write_mount_journal(journal, records);
}

std::string decode_mount_path(const std::string& input) {
    std::string out;
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\\' && i + 3 < input.size() && input[i + 1] >= '0' &&
            input[i + 1] <= '3' && input[i + 2] >= '0' && input[i + 2] <= '7' &&
            input[i + 3] >= '0' && input[i + 3] <= '7') {
            out += static_cast<char>(((input[i + 1] - '0') * 64) + ((input[i + 2] - '0') * 8) +
                                     input[i + 3] - '0');
            i += 3;
        } else {
            out += input[i];
        }
    }
    return out;
}

bool register_umount(const std::string& path) {
    return embedded_register_umount(path);
}

bool unregister_umount(const std::string& path) {
    return embedded_unregister_umount(path);
}

bool prepare_empty_mountpoint(const std::string& path) {
    namespace fs = std::filesystem;
    const fs::path value(path);
    std::error_code ec;
    if (path.find('\0') != std::string::npos || !value.is_absolute() ||
        value.lexically_normal() == value.root_path() ||
        fs::weakly_canonical(value, ec) != value.lexically_normal() || ec) {
        mlog("refusing unsafe mountpoint " + path, logging::Level::Error);
        return false;
    }
    fs::create_directories(value, ec);
    if (ec || !fs::is_empty(value, ec) || ec) {
        mlog("mountpoint must be an empty directory: " + path, logging::Level::Error);
        return false;
    }
    return true;
}

bool run_in_init_mount_ns(const std::function<bool()>& fn) {
    const pid_t pid = fork();
    if (pid < 0) {
        mlog("fork failed: " + std::string(std::strerror(errno)), logging::Level::Error);
        return false;
    }
    if (pid == 0) {
        if (!ksud::switch_mnt_ns(1)) {
            mlog("enter init mount namespace failed", logging::Level::Error);
            _exit(2);
        }
        bool ok = false;
        try {
            ok = fn();
        } catch (...) {
            ok = false;
        }
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool run_in_init_mount_ns(const std::function<bool()>& fn, std::string& error,
                          NamespaceOperationState* state) {
    // Keep the reply below PIPE_BUF: the existing helper waits for its child
    // before this parent reads, so the complete reply must fit without blocking.
    struct Reply {
        int error;
        char message[1024];
    } reply = {};
    static_assert(sizeof(Reply) <= PIPE_BUF);
    int channel[2];
    if (state)
        *state = NamespaceOperationState::NotStarted;
    if (pipe2(channel, O_CLOEXEC) != 0) {
        error = "create namespace result pipe: " + std::string(std::strerror(errno));
        return false;
    }
    error.clear();
    if (state)
        *state = NamespaceOperationState::Unknown;
    errno = 0;
    const bool ok = run_in_init_mount_ns([&]() {
        close(channel[0]);
        errno = 0;
        const bool child_ok = fn();
        if (!child_ok)
            reply.error = errno ? errno : EIO;
        const auto length = std::min(error.size(), sizeof(reply.message) - 1);
        std::memcpy(reply.message, error.data(), length);
        const char* cursor = reinterpret_cast<const char*>(&reply);
        std::size_t remaining = sizeof(reply);
        while (remaining) {
            const auto written = write(channel[1], cursor, remaining);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                break;
            cursor += written;
            remaining -= static_cast<std::size_t>(written);
        }
        close(channel[1]);
        return child_ok;
    });
    const int namespace_error = errno;
    close(channel[1]);
    char* cursor = reinterpret_cast<char*>(&reply);
    std::size_t received = 0;
    while (received < sizeof(reply)) {
        const auto count = read(channel[0], cursor + received, sizeof(reply) - received);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        received += static_cast<std::size_t>(count);
    }
    close(channel[0]);
    if (ok && received == sizeof(reply) && reply.error == 0) {
        if (state)
            *state = NamespaceOperationState::Completed;
        return true;
    }
    int failure = namespace_error ? namespace_error : EIO;
    if (received == sizeof(reply) && reply.error) {
        if (state)
            *state = NamespaceOperationState::CallbackFailed;
        failure = reply.error;
    }
    errno = failure;
    error = received == sizeof(reply) && reply.message[0]
                ? std::string(reply.message)
                : "failed to run managed operation in init mount namespace";
    return false;
}

std::string partition_mount_point(const std::string& partition) {
    if (partition == "system") {
        return "/system";
    }
    return "/" + partition;
}

std::string resolve_real_mount_target(const std::string& mount_point) {
    char buf[PATH_MAX];
    if (realpath(mount_point.c_str(), buf) == nullptr) {
        return "";
    }
    return {buf};
}

}  // namespace sumhp::mount::fsutil
