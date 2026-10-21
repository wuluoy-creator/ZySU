#include "mount/magic_mount.hpp"

#include "core/runtime.hpp"
#include "mount/mount_fs.hpp"
#include "mount/partition_alias.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <climits>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef MS_PRIVATE
#define MS_PRIVATE (1 << 18)
#endif

// Lazy-tmpfs magic mount: build a merged module tree and walk from "/". A dir
// becomes a tmpfs skeleton only when a direct child needs it; otherwise it is
// recursed into and left as-is, so partition roots and untouched dirs stay put.

namespace sumhp::mount::magic {

namespace fs = std::filesystem;
using fsutil::mlog;

enum class NType : std::uint8_t { Regular, Directory, Symlink, Whiteout };

struct Node {
    std::string name;
    NType type = NType::Directory;
    std::map<std::string, Node> children;
    std::string module_path;  // backing module file; "" if mirror-only / root
    bool replace = false;     // opaque dir (.replace / trusted.overlay.opaque)
};

struct Walk {
    std::vector<std::string> committed;  // mount points to record for teardown
    int files = 0;
    int tmpfs_dirs = 0;
    int symlinks = 0;
};

namespace {
std::string state_file() {
    return (runtime_data_dir() / "run" / "magic_mounts.list").string();
}

// Report every committed mount. The per-app policy decides whether modules
// remain visible; skipping library paths makes that policy incomplete.
bool ksu_umount_add(const std::string& path) {
    return fsutil::register_umount(path);
}

bool lexists(const std::string& p) {
    struct stat st{};
    return lstat(p.c_str(), &st) == 0;
}

std::string join(const std::string& base, const std::string& name) {
    std::string path = base;
    if (path != "/")
        path += '/';
    path += name;
    return path;
}

[[noreturn]] void scan_error(const std::string& message) {
    mlog("magic: " + message, logging::Level::Error);
    throw std::runtime_error(message);
}

NType type_from_lstat(const std::string& path, const struct stat& st) {
    bool whiteout = false;
    if (!fsutil::stat_is_whiteout(path, st, whiteout))
        scan_error("cannot read whiteout metadata: " + path);
    if (whiteout)
        return NType::Whiteout;
    if (S_ISDIR(st.st_mode))
        return NType::Directory;
    if (S_ISLNK(st.st_mode))
        return NType::Symlink;
    if (S_ISREG(st.st_mode))
        return NType::Regular;
    scan_error("unsupported module entry: " + path);
}

bool dir_is_replace(const std::string& path) {
    bool opaque = false;
    if (!fsutil::directory_is_opaque(path, opaque))
        scan_error("cannot read replacement metadata: " + path);
    return opaque;
}

bool clone_symlink(const std::string& src, const std::string& dst) {
    char tgt[PATH_MAX];
    const ssize_t n = readlink(src.c_str(), tgt, sizeof(tgt) - 1);
    if (n < 0) {
        return false;
    }
    tgt[n] = '\0';
    if (symlink(tgt, dst.c_str()) != 0 && errno != EEXIST) {
        return false;
    }
    std::string ctx;
    if (fsutil::get_context(src, ctx)) {
        fsutil::set_context(dst, ctx);
    }
    return true;
}

// Higher-priority entries win; opaque directories stop lower-layer merging.
bool collect_into(Node& parent, const std::string& dir,
                  const std::vector<std::string>* partitions = nullptr) {
    const std::unique_ptr<DIR, decltype(&closedir)> directory(opendir(dir.c_str()), closedir);
    if (!directory)
        scan_error("cannot scan " + dir + ": " + std::strerror(errno));
    bool has = false;
    for (;;) {
        errno = 0;
        const auto* entry = readdir(directory.get());
        if (!entry) {
            if (errno)
                scan_error("cannot read " + dir + ": " + std::strerror(errno));
            break;
        }
        const std::string name = entry->d_name;
        if (name == "." || name == ".." || name == ".replace")
            continue;
        std::string path = join(dir, name);
        struct stat st{};
        if (lstat(path.c_str(), &st) != 0)
            scan_error("cannot stat " + path + ": " + std::strerror(errno));
        if (S_ISLNK(st.st_mode) && partitions &&
            std::find(partitions->begin(), partitions->end(), name) != partitions->end()) {
            if (const auto target = fsutil::module_partition_alias(path)) {
                path = target->string();
                if (lstat(path.c_str(), &st) != 0)
                    scan_error("cannot stat " + path + ": " + std::strerror(errno));
            }
        }
        const NType type = type_from_lstat(path, st);
        auto existing = parent.children.find(name);
        if (existing != parent.children.end()) {
            auto& node = existing->second;
            if (node.type != NType::Directory || node.replace) {
                has = true;
                continue;
            }
            if (type != NType::Directory) {
                node.replace = true;
                has = true;
                continue;
            }
            node.replace = dir_is_replace(path);
            has = collect_into(node, path) || node.replace || has;
        } else {
            Node node;
            node.name = name;
            node.type = type;
            node.module_path = path;
            if (type == NType::Directory) {
                node.replace = dir_is_replace(path);
                collect_into(node, path);
            }
            has = true;
            parent.children.emplace(name, std::move(node));
        }
    }
    return has;
}

// Build the merged root tree: module system/ trees, with vendor/product/... moved
// out to root level when the device exposes them as /system/<p> symlinks.
std::optional<Node> collect_module_files(const std::vector<ModuleEntry>& modules,
                                         const std::vector<std::string>& extra) {
    Node root;
    root.type = NType::Directory;
    Node system;
    system.name = "system";
    system.type = NType::Directory;

    const auto partitions = fsutil::managed_partitions(extra);

    bool has = false;
    for (const auto& m : modules) {
        const std::string ms = m.path.string() + "/system";
        struct stat st{};
        if (lstat(ms.c_str(), &st) != 0) {
            if (errno == ENOENT)
                continue;
            scan_error("cannot stat " + ms + ": " + std::strerror(errno));
        }
        if (!S_ISDIR(st.st_mode))
            scan_error("module system entry is not a directory: " + ms);
        if (dir_is_replace(ms))
            scan_error("partition-root replacement is unsupported: " + ms);
        has |= collect_into(system, ms, &partitions);
    }
    if (!has) {
        return std::nullopt;
    }

    const auto move_partition = [&](const std::string& part, bool require_symlink) {
        std::error_code ec;
        const bool ok = fs::is_directory("/" + part, ec) &&
                        (!require_symlink || fs::is_symlink("/system/" + part, ec));
        if (!ok) {
            return;
        }
        auto it = system.children.find(part);
        if (it != system.children.end()) {
            if (it->second.replace || it->second.type != NType::Directory)
                scan_error("partition-root replacement is unsupported: /" + part);
            Node moved = std::move(it->second);
            system.children.erase(it);
            // Partition roots are not replaced wholesale; existing files can be bound individually.
            moved.module_path.clear();
            moved.replace = false;
            root.children.emplace(part, std::move(moved));
        }
    };

    const std::pair<const char*, bool> builtin[] = {
        {"vendor", true}, {"system_ext", true}, {"product", true}, {"odm", false}};
    for (const auto& [part, req] : builtin) {
        move_partition(part, req);
    }
    for (const auto& part : extra) {
        if (part == "system") {
            continue;
        }
        bool is_builtin = false;
        for (const auto& bp : builtin) {
            if (part == bp.first) {
                is_builtin = true;
            }
        }
        if (!is_builtin) {
            move_partition(part, false);
        }
    }

    root.children.emplace("system", std::move(system));
    return root;
}

// Create the tmpfs skeleton dir at `work`, cloning mode/owner/SELinux context
// from the real dir (or the module dir when the real one does not exist).
bool tmpfs_skeleton(const std::string& real, const std::string& work, const Node& node) {
    std::error_code ec;
    fs::create_directories(work, ec);
    if (ec) {
        mlog("skeleton mkdir " + work + " failed: " + ec.message(), logging::Level::Error);
        return false;
    }
    struct stat st{};
    const std::string src =
        lstat(real.c_str(), &st) == 0 && S_ISDIR(st.st_mode) ? real : node.module_path;
    if (src.empty()) {
        return false;
    }
    fsutil::clone_attr(src, work);
    return true;
}

// Mirror one unmodified real entry into the skeleton: dirs recursed entry by
// entry, files bind-mounted, symlinks recreated.
bool mount_mirror(const std::string& real, const std::string& work, const std::string& name) {
    const std::string r = join(real, name);
    const std::string w = join(work, name);
    struct stat st{};
    if (lstat(r.c_str(), &st) != 0) {
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(w.c_str(), 0755) != 0 && errno != EEXIST) {
            return false;
        }
        fsutil::clone_attr(r, w);
        // Mirror each child individually; binding the whole subtree could carry
        // nested mounts and break the moved skeleton.
        DIR* d = opendir(r.c_str());
        if (!d) {
            mlog("mirror opendir " + r + " failed: " + std::strerror(errno), logging::Level::Error);
            return false;
        }
        bool ok = true;
        struct dirent* e;
        for (;;) {
            errno = 0;
            e = readdir(d);
            if (!e) {
                ok = errno == 0;
                break;
            }
            if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
                continue;
            }
            if (!mount_mirror(r, w, e->d_name)) {
                ok = false;
                break;
            }
        }
        closedir(d);
        return ok;
    }
    if (S_ISLNK(st.st_mode)) {
        return clone_symlink(r, w);
    }
    if (S_ISREG(st.st_mode)) {
        const int fd = open(w.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            close(fd);
        }
        if (!fsutil::bind_mount(r, w)) {
            mlog("mirror bind " + r + " failed: " + std::strerror(errno), logging::Level::Error);
            return false;
        }
        return true;
    }
    return true;  // skip device/socket/fifo
}

bool do_mount(Node& node, const std::string& real, const std::string& work, bool has_tmpfs,
              Walk& w);

bool do_directory(Node& node, const std::string& real, const std::string& work, bool has_tmpfs,
                  Walk& w) {
    struct stat real_stat{};
    const int result = lstat(real.c_str(), &real_stat);
    if (result != 0 && errno != ENOENT && errno != ENOTDIR) {
        mlog("magic: cannot stat directory " + real + ": " + std::strerror(errno),
             logging::Level::Error);
        return false;
    }
    const bool real_directory = result == 0 && S_ISDIR(real_stat.st_mode);
    bool tmpfs = !has_tmpfs && node.replace && !node.module_path.empty();

    if (!has_tmpfs && !tmpfs) {
        for (auto& [name, child] : node.children) {
            const std::string real_child = join(real, name);
            bool need;
            if (child.type == NType::Symlink) {
                need = true;
            } else if (child.type == NType::Regular) {
                struct stat st{};
                need = lstat(real_child.c_str(), &st) != 0 || !S_ISREG(st.st_mode);
            } else if (child.type == NType::Whiteout) {
                need = lexists(real_child);
            } else {
                // A dir needs a skeleton when a child's real counterpart is not a
                // directory (a file/symlink) or is missing.
                struct stat st{};
                if (lstat(real_child.c_str(), &st) == 0) {
                    need = !S_ISDIR(st.st_mode);
                } else {
                    need = true;
                }
            }
            if (need) {
                if (logging::enabled(logging::Level::Debug)) {
                    mlog(std::string("  need-skeleton at ")
                             .append(real)
                             .append(" due to child '")
                             .append(name)
                             .append("' (real=")
                             .append(real_child)
                             .append(")"));
                }
                if (node.module_path.empty()) {
                    mlog(
                        "magic: cannot add, delete or change the type of a partition-root entry: " +
                            real_child,
                        logging::Level::Error);
                    return false;
                }
                tmpfs = true;
                break;
            }
        }
    }

    const bool now_tmpfs = tmpfs || has_tmpfs;
    if (now_tmpfs && !tmpfs_skeleton(real, work, node)) {
        return false;
    }
    if (tmpfs && !fsutil::bind_mount(work, work)) {  // make the skeleton movable
        mlog("self-bind " + work + " failed: " + std::strerror(errno), logging::Level::Error);
        return false;
    }

    // Only a skeleton needs untouched stock entries. Direct binds walk the
    // module children below without enumerating potentially large system dirs.
    if (now_tmpfs && real_directory && !node.replace) {
        DIR* d = opendir(real.c_str());
        if (!d) {
            mlog("magic: cannot read directory " + real + ": " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
        {
            struct dirent* e;
            for (;;) {
                errno = 0;
                e = readdir(d);
                if (!e) {
                    const int error = errno;
                    closedir(d);
                    if (error)
                        return false;
                    break;
                }
                if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
                    continue;
                }
                const std::string name = e->d_name;
                auto it = node.children.find(name);
                if (it != node.children.end()) {
                    Node child = std::move(it->second);
                    node.children.erase(it);
                    if (!do_mount(child, join(real, name), join(work, name), now_tmpfs, w)) {
                        closedir(d);
                        return false;
                    }
                } else {
                    if (!mount_mirror(real, work, name)) {
                        closedir(d);
                        return false;
                    }
                }
            }
        }
    }

    // Without a skeleton these are all module children; otherwise only entries
    // not encountered in the stock directory remain (or all for an opaque dir).
    for (auto& [name, child] : node.children) {
        if (!do_mount(child, join(real, name), join(work, name), now_tmpfs, w)) {
            return false;
        }
    }

    if (tmpfs) {
        if (::mount(nullptr, work.c_str(), nullptr, MS_REMOUNT | MS_BIND | MS_RDONLY, nullptr) != 0)
            return false;
        if (::mount(work.c_str(), real.c_str(), nullptr, MS_MOVE, nullptr) != 0) {
            mlog("move " + work + " -> " + real + " failed: " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
        w.committed.push_back(real);
        if (::mount("none", real.c_str(), nullptr, MS_PRIVATE | MS_REC, nullptr) != 0)
            return false;
        ++w.tmpfs_dirs;
        mlog("magic: skeletoned " + real);
    }
    return true;
}

bool do_mount(Node& node, const std::string& real, const std::string& work, bool has_tmpfs,
              Walk& w) {
    switch (node.type) {
    case NType::Symlink:
        if (node.module_path.empty()) {
            mlog("symlink node without module backing: " + real);
            return false;
        }
        if (!clone_symlink(node.module_path, work)) {
            return false;
        }
        ++w.symlinks;
        return true;
    case NType::Regular: {
        if (node.module_path.empty())
            return false;
        const std::string& target = has_tmpfs ? work : real;
        if (has_tmpfs) {
            const int fd = open(target.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
            if (fd < 0)
                return false;
            if (close(fd) != 0)
                return false;
        } else {
            struct stat st{};
            if (lstat(target.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                mlog("magic: direct bind requires an existing regular file: " + target,
                     logging::Level::Error);
                return false;
            }
        }
        if (!fsutil::bind_mount(node.module_path, target)) {
            mlog("magic: bind " + node.module_path + " -> " + target + ": " + std::strerror(errno),
                 logging::Level::Error);
            return false;
        }
        if (!has_tmpfs)
            w.committed.push_back(target);
        if (::mount(nullptr, target.c_str(), nullptr, MS_REMOUNT | MS_BIND | MS_RDONLY, nullptr) !=
            0) {
            mlog("magic: readonly remount failed: " + target, logging::Level::Error);
            return false;
        }
        if (!has_tmpfs && ::mount("none", target.c_str(), nullptr, MS_PRIVATE, nullptr) != 0) {
            mlog("magic: private propagation failed: " + target, logging::Level::Error);
            return false;
        }
        ++w.files;
        return true;
    }
    case NType::Whiteout:
        return true;  // absence in the skeleton hides the original
    case NType::Directory:
        return do_directory(node, real, work, has_tmpfs, w);
    }
    return false;
}

// Mount points whose mount source equals `source`. Our skeletons and work tmpfs
// use config.mount_source; real partitions are block-backed, so teardown can match
// ours without ever touching a real partition.
std::set<std::string> mounts_with_source(const std::string& source) {
    std::set<std::string> out;
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    while (std::getline(in, line)) {
        const auto sep = line.find(" - ");
        if (sep == std::string::npos) {
            continue;
        }
        std::istringstream pre(line.substr(0, sep));
        std::vector<std::string> f;
        std::string t;
        while (pre >> t) {
            f.push_back(t);
        }
        if (f.size() < 5) {
            continue;
        }
        std::istringstream post(line.substr(sep + 3));
        std::string fstype;
        std::string src;
        post >> fstype >> src;
        if (fsutil::decode_mount_path(src) == source) {
            out.insert(fsutil::decode_mount_path(f[4]));
        }
    }
    return out;
}
}  // namespace

using fsutil::MountRecord;

namespace {
bool read_mounts(std::vector<MountRecord>& records, bool& legacy) {
    return fsutil::read_mount_journal(state_file(), records, legacy);
}

bool rollback(const std::vector<std::string>& paths) {
    const std::set<std::string, std::greater<>> ordered(paths.begin(), paths.end());
    std::vector<std::string> remaining;
    bool ok = true;
    for (const auto& path : ordered) {
        if (umount2(path.c_str(), MNT_DETACH) != 0) {
            mlog("magic: rollback failed for " + path + ": " + std::strerror(errno),
                 logging::Level::Error);
            remaining.push_back(path);
            ok = false;
        } else {
            ok = fsutil::unregister_umount(path) && ok;
        }
    }
    // A failed final journal write may have left only the scratch mount in the
    // old journal. Retain any mounts that could not be detached for a retry.
    if (!remaining.empty() && !fsutil::write_mount_journal(state_file(), remaining)) {
        mlog("magic: cannot journal mounts left after rollback", logging::Level::Error);
    } else if (ok) {
        std::error_code ec;
        fs::remove(state_file(), ec);
        ok = !ec;
    }
    return ok;
}
}  // namespace

bool unmount_all(const Config& config) {
    std::vector<MountRecord> records;
    bool legacy;
    if (!read_mounts(records, legacy)) {
        mlog("magic: invalid or unreadable mount journal", logging::Level::Error);
        return false;
    }
    std::map<std::string, MountRecord, std::greater<>> ordered;
    for (const auto& record : records)
        ordered.emplace(record.path, record);
    const auto old_mounts =
        legacy ? mounts_with_source(config.mount_source) : std::set<std::string>{};
    bool ok = true;
    for (const auto& [path, record] : ordered) {
        if (legacy ? !old_mounts.count(path) : !fsutil::mount_matches(record)) {
            if (!legacy)
                ok = fsutil::unregister_umount(path) && ok;
            continue;
        }
        if (umount2(path.c_str(), MNT_DETACH) != 0) {
            mlog("magic: detach failed for " + path + ": " + std::strerror(errno),
                 logging::Level::Error);
            ok = false;
        } else {
            ok = fsutil::unregister_umount(path) && ok;
        }
    }
    if (ok) {
        std::error_code ec;
        fs::remove(state_file(), ec);
        ok = !ec;
    }
    return ok;
}

std::vector<std::string> active_mounts(const Config& config) {
    std::vector<MountRecord> records;
    bool legacy;
    if (!read_mounts(records, legacy))
        return {};
    const auto old_mounts =
        legacy ? mounts_with_source(config.mount_source) : std::set<std::string>{};
    std::vector<std::string> paths;
    for (const auto& record : records) {
        if (legacy ? old_mounts.count(record.path) != 0 : fsutil::mount_matches(record, true))
            paths.push_back(record.path);
    }
    return paths;
}

namespace {
bool save_mounts(const std::vector<std::string>& mounts) {
    return fsutil::write_mount_journal(state_file(), mounts);
}
}  // namespace

bool mount_modules(const std::vector<ModuleEntry>& modules, const Config& config) {
    if (!unmount_all(config))
        return false;

    std::error_code ec;
    fs::create_directories(runtime_data_dir() / "run", ec);
    if (ec) {
        mlog("magic: cannot create journal directory: " + ec.message(), logging::Level::Error);
        return false;
    }

    auto root = collect_module_files(modules, config.partitions);
    if (!root) {
        mlog("magic: no module files to mount");
        return true;
    }

    const std::string work = config.work_dir;
    if (!fsutil::prepare_empty_mountpoint(work))
        return false;
    if (::mount(config.mount_source.c_str(), work.c_str(), "tmpfs", 0, nullptr) != 0) {
        mlog("magic: work tmpfs failed: " + std::string(std::strerror(errno)),
             logging::Level::Error);
        return false;
    }
    if (::mount("none", work.c_str(), nullptr, MS_PRIVATE | MS_REC, nullptr) != 0 ||
        !save_mounts({work})) {
        (void)rollback({work});
        return false;
    }

    Walk w;
    w.committed.push_back(work);
    bool ok = false;
    try {
        ok = do_mount(*root, "/", work, false, w);
        if (ok) {
            // Moved skeletons and their child binds retain their own references
            // to the tmpfs. Release the staging mount before apps inherit it.
            if (umount2(work.c_str(), MNT_DETACH) != 0) {
                mlog("magic: scratch detach failed: " + std::string(std::strerror(errno)),
                     logging::Level::Error);
                ok = false;
            } else {
                w.committed.erase(w.committed.begin());
            }
        }
    } catch (const std::exception& error) {
        ok = false;
        mlog("magic: mount traversal failed: " + std::string(error.what()), logging::Level::Error);
    } catch (...) {
        ok = false;
        mlog("magic: mount traversal failed", logging::Level::Error);
    }

    if (!save_mounts(w.committed)) {
        (void)rollback(w.committed);
        return false;
    }
    if (ok) {
        for (const auto& m : w.committed)
            ok = ksu_umount_add(m) && ok;
    }
    if (!ok)
        (void)unmount_all(config);
    mlog("magic: files=" + std::to_string(w.files) + " skeletons=" + std::to_string(w.tmpfs_dirs) +
         " symlinks=" + std::to_string(w.symlinks));
    return ok;
}

bool is_active(const Config& config) {
    return !active_mounts(config).empty();
}

namespace {
const std::vector<std::string>& normalization_partitions() {
    static const std::vector<std::string> names{"vendor", "system_ext", "product", "odm", "oem"};
    return names;
}

bool normalization_error(std::string& error, const std::string& phase, const std::string& path,
                         int code = 0) {
    const int failure = code ? code : (errno ? errno : EIO);
    errno = failure;
    error = phase + " " + path + ": " + std::strerror(failure);
    return false;
}

NormalizationObservation normalization_metadata(const std::string& path, const struct stat& st) {
    NormalizationObservation value;
    value.path = path;
    value.kind = S_ISDIR(st.st_mode)   ? "directory"
                 : S_ISREG(st.st_mode) ? "regular"
                 : S_ISLNK(st.st_mode) ? "symlink"
                                       : "other";
    value.device = st.st_dev;
    value.inode = st.st_ino;
    value.mode = st.st_mode;
    value.size = st.st_size;
    value.mtime_seconds = st.st_mtim.tv_sec;
    value.mtime_nanoseconds = st.st_mtim.tv_nsec;
    value.ctime_seconds = st.st_ctim.tv_sec;
    value.ctime_nanoseconds = st.st_ctim.tv_nsec;
    return value;
}

bool normalization_probe(const std::string& path, NormalizationObservation& result,
                         std::string& error) {
    struct stat metadata{};
    if (lstat(path.c_str(), &metadata) != 0) {
        if (errno != ENOENT)
            return normalization_error(error, "inspect", path);
        result = {};
        result.path = path;
        result.kind = "missing";
        return true;
    }
    result = normalization_metadata(path, metadata);
    if (result.kind == "symlink") {
        char bytes[PATH_MAX];
        const auto size = ::readlink(path.c_str(), bytes, sizeof(bytes));
        if (size < 0)
            return normalization_error(error, "readlink", path);
        if (static_cast<std::size_t>(size) == sizeof(bytes))
            return normalization_error(error, "link target exceeds budget", path, EOVERFLOW);
        result.link_target.assign(bytes, static_cast<std::size_t>(size));
    }
    return true;
}

bool normalization_same(const NormalizationObservation& a, const NormalizationObservation& b) {
    return a.path == b.path && a.kind == b.kind && a.device == b.device && a.inode == b.inode &&
           a.mode == b.mode && a.size == b.size && a.mtime_seconds == b.mtime_seconds &&
           a.mtime_nanoseconds == b.mtime_nanoseconds && a.ctime_seconds == b.ctime_seconds &&
           a.ctime_nanoseconds == b.ctime_nanoseconds && a.link_target == b.link_target;
}

bool normalization_same_identity(const NormalizationObservation& a,
                                 const NormalizationObservation& b) {
    return a.kind == b.kind && a.device == b.device && a.inode == b.inode && a.mode == b.mode;
}

bool normalization_same_plan(const NormalizationPlan& a, const NormalizationPlan& b) {
    if (!a.complete || !b.complete || a.requested_root != b.requested_root ||
        a.resolved_root != b.resolved_root || a.observations.size() != b.observations.size() ||
        a.actions.size() != b.actions.size() || a.bytes != b.bytes)
        return false;
    for (std::size_t i = 0; i < a.observations.size(); ++i)
        if (!normalization_same(a.observations[i], b.observations[i]))
            return false;
    for (std::size_t i = 0; i < a.actions.size(); ++i) {
        const auto& x = a.actions[i];
        const auto& y = b.actions[i];
        if (x.kind != y.kind || x.source != y.source || x.target != y.target ||
            x.link_target != y.link_target || x.reason != y.reason)
            return false;
    }
    return true;
}

bool normalization_room(NormalizationPlan& plan, std::size_t bytes, std::string& error) {
    if (bytes > NormalizationPlan::max_bytes - plan.bytes)
        return normalization_error(error, "normalization capacity exceeded", plan.resolved_root,
                                   EOVERFLOW);
    plan.bytes += bytes;
    return true;
}

bool normalization_observe(NormalizationPlan& plan, const std::string& path,
                           NormalizationObservation& value, std::string& error) {
    if (plan.observations.size() >= NormalizationPlan::max_observations)
        return normalization_error(error, "normalization observation capacity exceeded", path,
                                   EOVERFLOW);
    if (!normalization_probe(path, value, error) ||
        !normalization_room(
            plan, sizeof(value) + value.path.size() + value.kind.size() + value.link_target.size(),
            error))
        return false;
    plan.observations.push_back(value);
    return true;
}

bool normalization_action(NormalizationPlan& plan, NormalizationAction action, std::string& error) {
    if (plan.actions.size() >= NormalizationPlan::max_actions ||
        !normalization_room(plan,
                            sizeof(action) + action.kind.size() + action.source.size() +
                                action.target.size() + action.link_target.size() +
                                action.reason.size(),
                            error))
        return normalization_error(error, "normalization action capacity exceeded", action.target,
                                   EOVERFLOW);
    plan.actions.push_back(std::move(action));
    return true;
}

bool normalization_directory(const std::string& path, std::string& canonical, std::string& error) {
    std::error_code ec;
    const bool directory = fs::is_directory(path, ec);
    if (ec)
        return normalization_error(error, "inspect directory", path, ec.value());
    if (!directory)
        return normalization_error(error, "not a directory", path, ENOTDIR);
    const auto resolved = fs::canonical(path, ec);
    if (ec)
        return normalization_error(error, "resolve directory", path, ec.value());
    canonical = resolved.generic_string();
    return true;
}

bool normalization_safe_root(const std::string& path, std::string& resolved, std::string& error) {
    static const std::set<std::string> unsafe{"/",
                                              "/data",
                                              "/data/adb",
                                              "/data/adb/modules",
                                              "/data/adb/modules_update",
                                              "/data/adb/ksu",
                                              "/system",
                                              "/vendor",
                                              "/product",
                                              "/odm",
                                              "/oem",
                                              "/system_ext",
                                              "/tmp",
                                              "/dev",
                                              "/proc",
                                              "/sys"};
    if (!valid_module_rule_path(path) || unsafe.count(path))
        return normalization_error(error, "unsafe module root", path, EINVAL);
    if (!normalization_directory(path, resolved, error))
        return false;
    // Equality also excludes symlink ancestors, dot segments and alias roots.
    if (resolved != path)
        return normalization_error(error, "module root must be canonical without symlinks", path,
                                   ELOOP);
    return true;
}

class NormalizationFd {
    int value = -1;

public:
    NormalizationFd() = default;
    explicit NormalizationFd(int descriptor) : value(descriptor) {}
    NormalizationFd(const NormalizationFd&) = delete;
    NormalizationFd& operator=(const NormalizationFd&) = delete;
    NormalizationFd(NormalizationFd&&) = delete;
    NormalizationFd& operator=(NormalizationFd&&) = delete;
    [[nodiscard]] int get() const { return value; }
    void reset(int descriptor) {
        if (value >= 0)
            ::close(value);
        value = descriptor;
    }
    ~NormalizationFd() {
        if (value >= 0)
            ::close(value);
    }
};

bool normalization_fd_matches(int fd, const NormalizationObservation& expected,
                              std::string& error) {
    struct stat metadata{};
    if (::fstat(fd, &metadata) != 0)
        return normalization_error(error, "inspect directory descriptor", expected.path);
    if (!normalization_same_identity(normalization_metadata(expected.path, metadata), expected))
        return normalization_error(error, "directory descriptor changed", expected.path, ESTALE);
    return true;
}
}  // namespace

bool inspect_module_root(const std::string& module_path, std::string& resolved,
                         std::string& error) {
    error.clear();
    std::string canonical;
    if (!normalization_safe_root(module_path, canonical, error))
        return false;
    NormalizationObservation root;
    NormalizationObservation property;
    if (!normalization_probe(canonical, root, error) ||
        !normalization_probe(canonical + "/module.prop", property, error))
        return false;
    if (root.kind != "directory" || property.kind != "regular")
        return normalization_error(error, "expected real module root and module.prop", canonical,
                                   EINVAL);
    resolved = std::move(canonical);
    return true;
}

bool prepare_normalization(const std::string& module_path, NormalizationPlan& plan,
                           std::string& error) {
    error.clear();
    NormalizationPlan candidate;
    candidate.requested_root = module_path;
    if (!normalization_safe_root(module_path, candidate.resolved_root, error))
        return false;
    if (!normalization_room(candidate, module_path.size() + candidate.resolved_root.size(), error))
        return false;
    NormalizationObservation root;
    NormalizationObservation property;
    NormalizationObservation system;
    const auto system_path = module_path + "/system";
    if (!normalization_observe(candidate, module_path, root, error) ||
        !normalization_observe(candidate, module_path + "/module.prop", property, error) ||
        !normalization_observe(candidate, system_path, system, error))
        return false;
    if (root.kind != "directory" || property.kind != "regular" ||
        (system.kind != "missing" && system.kind != "directory"))
        return normalization_error(error,
                                   "expected real module root, module.prop and system directory",
                                   module_path, EINVAL);
    bool create_system = system.kind == "missing";
    for (const auto& part : normalization_partitions()) {
        const auto target_path = "/" + part;
        const auto alias_path = "/system/" + part;
        auto top = module_path;
        top += "/";
        top += part;
        auto nested = system_path;
        nested += "/";
        nested += part;
        NormalizationObservation target;
        NormalizationObservation alias;
        if (!normalization_observe(candidate, target_path, target, error) ||
            !normalization_observe(candidate, alias_path, alias, error))
            return false;
        if (target.kind == "missing" || alias.kind != "symlink") {
            if (!normalization_action(candidate,
                                      {"skip", top, nested, "",
                                       target.kind == "missing" ? "platform_partition_missing"
                                                                : "platform_alias_absent"},
                                      error))
                return false;
            continue;
        }
        std::string target_real;
        std::string alias_real;
        if (!normalization_directory(target_path, target_real, error) ||
            !normalization_directory(alias_path, alias_real, error))
            return false;
        if (target_real != alias_real)
            return normalization_error(error, "platform alias does not target partition",
                                       alias_path, EINVAL);
        if (target_real != target_path) {
            NormalizationObservation real;
            if (!normalization_observe(candidate, target_real, real, error))
                return false;
        }
        NormalizationObservation top_entry;
        NormalizationObservation nested_entry;
        if (!normalization_observe(candidate, top, top_entry, error) ||
            !normalization_observe(candidate, nested, nested_entry, error))
            return false;
        if (top_entry.kind == "symlink") {
            // Accept equivalent in-root aliases, never dangling or escaping links.
            const auto link = fs::path(top_entry.link_target);
            const auto destination = (link.is_absolute() ? link : fs::path(module_path) / link)
                                         .lexically_normal()
                                         .generic_string();
            if (destination != nested || nested_entry.kind != "directory")
                return normalization_error(error, "conflicting or escaping partition alias", top,
                                           EINVAL);
            if (!normalization_action(
                    candidate,
                    {"unchanged", top, nested, top_entry.link_target, "matching_partition_alias"},
                    error))
                return false;
            continue;
        }
        if ((top_entry.kind != "missing" && top_entry.kind != "directory") ||
            (nested_entry.kind != "missing" && nested_entry.kind != "directory") ||
            (top_entry.kind != "missing" && nested_entry.kind != "missing")) {
            auto conflict = top;
            conflict += " and ";
            conflict += nested;
            return normalization_error(error, "conflicting partition entries", conflict, EEXIST);
        }
        if (top_entry.kind == "missing" && nested_entry.kind == "missing") {
            if (!normalization_action(candidate,
                                      {"skip", top, nested, "", "module_partition_missing"}, error))
                return false;
            continue;
        }
        if (create_system) {
            if (!normalization_action(
                    candidate, {"mkdir", "", system_path, "", "partition_parent_required"}, error))
                return false;
            create_system = false;
        }
        if (top_entry.kind == "directory" &&
            !normalization_action(candidate,
                                  {"move", top, nested, "", "fold_partition_into_system"}, error))
            return false;
        if (!normalization_action(
                candidate,
                {"symlink", "", top, "./system/" + part, "retain_top_level_partition_alias"},
                error))
            return false;
    }
    candidate.complete = true;
    plan = std::move(candidate);
    error.clear();
    return true;
}

bool execute_normalization(const NormalizationPlan& plan, NormalizationResult& result,
                           std::string& error) {
    result = {};
    result.status = "rejected";
    result.recovery_hint = "Inspect a fresh normalization preview before another explicit attempt; "
                           "do not overwrite or remove surviving directories.";
    NormalizationPlan fresh;
    if (!prepare_normalization(plan.requested_root, fresh, error) ||
        !normalization_same_plan(plan, fresh)) {
        if (error.empty())
            normalization_error(error, "normalization plan changed", plan.requested_root, ESTALE);
        return false;
    }
    for (std::size_t i = 0; i < plan.actions.size(); ++i)
        result.actions.push_back({i, "not_run", 0, ""});
    auto expected = plan.observations;
    const auto find = [&](const std::string& path) -> NormalizationObservation* {
        const auto found = std::find_if(expected.begin(), expected.end(),
                                        [&](const auto& item) { return item.path == path; });
        return found == expected.end() ? nullptr : &*found;
    };
    const NormalizationFd root_fd{
        ::open(plan.resolved_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (root_fd.get() < 0)
        return normalization_error(error, "open module root", plan.resolved_root);
    NormalizationFd system_fd;
    const auto system_path = plan.resolved_root + "/system";
    const auto check_all = [&]() {
        std::string resolved;
        if (!normalization_safe_root(plan.resolved_root, resolved, error))
            return false;
        for (const auto& before : expected) {
            NormalizationObservation now;
            if (!normalization_probe(before.path, now, error))
                return false;
            if (!normalization_same(before, now))
                return normalization_error(error, "normalization input changed", before.path,
                                           ESTALE);
        }
        if (!normalization_fd_matches(root_fd.get(), *find(plan.resolved_root), error))
            return false;
        return system_fd.get() < 0 ||
               normalization_fd_matches(system_fd.get(), *find(system_path), error);
    };
    const auto refresh_parent = [&](const std::string& path) {
        auto* before = find(path);
        NormalizationObservation after;
        if (!before || !normalization_probe(path, after, error))
            return false;
        if (!normalization_same_identity(*before, after))
            return normalization_error(error, "parent changed after normalization action", path,
                                       ESTALE);
        *before = std::move(after);
        return true;
    };
    const auto sync_parent = [&](int fd, const std::string& path) {
        if (::fsync(fd) != 0)
            return normalization_error(error, "sync normalization parent", path);
        return true;
    };
    for (std::size_t index = 0; index < plan.actions.size(); ++index) {
        const auto& action = plan.actions[index];
        auto& report = result.actions[index];
        const auto fail = [&]() {
            report.status = "failed";
            report.error_number = errno ? errno : EIO;
            report.detail = error;
            result.status = result.changed ? "partial" : "failed";
            return false;
        };
        if (!check_all())
            return fail();
        if (action.kind == "skip" || action.kind == "unchanged") {
            report.status = action.kind == "skip" ? "skipped" : "unchanged";
            continue;
        }
        if (action.kind == "mkdir") {
            if (::mkdirat(root_fd.get(), "system", 0755) != 0) {
                normalization_error(error, "create normalization parent", system_path);
                return fail();
            }
            result.changed = true;
            report.effect_observed = true;
            NormalizationObservation created;
            if (!normalization_probe(system_path, created, error) || created.kind != "directory") {
                if (error.empty())
                    normalization_error(error, "created parent changed", system_path, ESTALE);
                return fail();
            }
            *find(system_path) = std::move(created);
            if (!refresh_parent(plan.resolved_root) ||
                !sync_parent(root_fd.get(), plan.resolved_root))
                return fail();
        } else {
            if (system_fd.get() < 0) {
                system_fd.reset(::openat(root_fd.get(), "system",
                                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
                if (system_fd.get() < 0 ||
                    !normalization_fd_matches(system_fd.get(), *find(system_path), error)) {
                    if (error.empty())
                        normalization_error(error, "open normalization parent", system_path);
                    return fail();
                }
            }
            const auto part =
                fs::path(action.kind == "move" ? action.source : action.target).filename().string();
            if (action.kind == "move") {
                const auto before = *find(action.source);
                // RENAME_NOREPLACE is required: a prior check cannot authorize
                // replacing a concurrently created destination, even if empty.
                if (::syscall(SYS_renameat2, root_fd.get(), part.c_str(), system_fd.get(),
                              part.c_str(), 1U) != 0) {
                    normalization_error(error, "move partition without replacement", action.target);
                    return fail();
                }
                result.changed = true;
                report.effect_observed = true;
                NormalizationObservation source;
                NormalizationObservation target;
                if (!normalization_probe(action.source, source, error) ||
                    !normalization_probe(action.target, target, error) ||
                    source.kind != "missing" || !normalization_same_identity(before, target)) {
                    if (error.empty())
                        normalization_error(error, "moved partition changed", action.target,
                                            ESTALE);
                    return fail();
                }
                *find(action.source) = std::move(source);
                *find(action.target) = std::move(target);
                if (!refresh_parent(plan.resolved_root) || !refresh_parent(system_path) ||
                    !sync_parent(root_fd.get(), plan.resolved_root) ||
                    !sync_parent(system_fd.get(), system_path))
                    return fail();
            } else if (action.kind == "symlink") {
                if (::symlinkat(action.link_target.c_str(), root_fd.get(), part.c_str()) != 0) {
                    normalization_error(error, "create partition alias", action.target);
                    return fail();
                }
                result.changed = true;
                report.effect_observed = true;
                NormalizationObservation link;
                if (!normalization_probe(action.target, link, error) || link.kind != "symlink" ||
                    link.link_target != action.link_target) {
                    if (error.empty())
                        normalization_error(error, "partition alias changed", action.target,
                                            ESTALE);
                    return fail();
                }
                *find(action.target) = std::move(link);
                if (!refresh_parent(plan.resolved_root) ||
                    !sync_parent(root_fd.get(), plan.resolved_root))
                    return fail();
            }
        }
        report.status = "completed";
        report.durability_confirmed = true;
    }
    if (!check_all()) {
        result.status = result.changed ? "partial" : "failed";
        return false;
    }
    result.complete = true;
    result.status = "completed";
    result.recovery_hint =
        "Normalization completed; no mounts or saved configuration were applied.";
    error.clear();
    return true;
}

bool normalize_module(const std::string& module_path) {
    NormalizationPlan plan;
    NormalizationResult result;
    std::string error;
    if (prepare_normalization(module_path, plan, error) &&
        execute_normalization(plan, result, error))
        return true;
    mlog("normalize: " + error +
             (result.changed ? "; partial layout remains; inspect before retry" : ""),
         logging::Level::Error);
    return false;
}

}  // namespace sumhp::mount::magic
