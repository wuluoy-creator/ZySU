#include "mount/sumh.hpp"

#include "core/json.hpp"
#include "core/runtime.hpp"
#include "mount/mount_fs.hpp"
#include "mount/partition_alias.hpp"
#include "sumhp/sumh_client.hpp"
#include "uapi/sumh.h"
#include "utils.hpp"

#include <dirent.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace sumhp::mount::sumh {

namespace fs = std::filesystem;
using fsutil::mlog;

namespace {

bool user_hide_restore_pending = false;

fs::path active_file() {
    return runtime_data_dir() / "run" / "sumh_active";
}
fs::path mapping_plan_file() {
    return runtime_data_dir() / "run" / "sumh_mapping_plan";
}

bool path_matches_rule(const std::string& path, const std::string& prefix) {
    if (path == prefix) {
        return true;
    }
    if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    return prefix.back() == '/' || path[prefix.size()] == '/';
}

std::string effective_mode(const std::string& path, const std::vector<ModuleRule>& rules) {
    std::string mode = "sumh";
    std::size_t longest = 0;
    for (const auto& rule : rules) {
        if (rule.path.empty() || rule.path.front() != '/') {
            continue;
        }
        if (path_matches_rule(path, rule.path) && rule.path.size() >= longest) {
            longest = rule.path.size();
            mode = rule.mode;
        }
    }
    return mode;
}

bool has_nested_rule(const std::string& path, const std::vector<ModuleRule>& rules) {
    return std::any_of(rules.begin(), rules.end(), [&](const ModuleRule& rule) {
        return rule.path.size() > path.size() && path_matches_rule(rule.path, path);
    });
}

// Resolve symlinked partition paths (e.g. /system/vendor -> /vendor) without
// discarding a non-existent leaf that a module adds.
bool resolve_virtual_path(const std::string& value, std::string& resolved, std::string& error,
                          bool directory = false) {
    const fs::path path(value);
    const auto missing = [](const std::error_code& ec) {
        return !ec || ec == std::errc::no_such_file_or_directory ||
               ec == std::errc::not_a_directory;
    };
    const auto fail = [&](const std::error_code& ec) {
        error = "cannot resolve virtual path " + value + ": " + ec.message();
        return false;
    };
    std::error_code ec;
    if (directory) {
        const bool is_directory = fs::is_directory(path, ec);
        if (!missing(ec))
            return fail(ec);
        if (is_directory) {
            const auto canonical = fs::canonical(path, ec);
            if (ec)
                return fail(ec);
            resolved = canonical.generic_string();
            return true;
        }
    }
    if (!path.has_parent_path()) {
        resolved = value;
        return true;
    }
    fs::path current = path.parent_path();
    std::vector<fs::path> suffix;
    while (!current.empty()) {
        ec.clear();
        if (fs::exists(current, ec)) {
            current = fs::canonical(current, ec);
            if (ec)
                return fail(ec);
            break;
        }
        if (!missing(ec))
            return fail(ec);
        if (current == "/")
            break;
        suffix.push_back(current.filename());
        current = current.parent_path();
    }
    for (auto it = suffix.rbegin(); it != suffix.rend(); ++it)
        current /= *it;
    current /= path.filename();
    resolved = current.generic_string();
    return true;
}

bool resolve_module_rules(const std::vector<ModuleRule>& input, std::vector<ModuleRule>& output,
                          std::string& error) {
    // Legacy lists accepted duplicate literal paths; their last entry wins.
    // Resolve aliases after that reduction so upgrades preserve that contract.
    std::map<std::string, std::string> literal;
    for (const auto& rule : input)
        literal[rule.path] = rule.mode;
    std::map<std::string, std::string> resolved;
    for (const auto& [literal_path, mode] : literal) {
        // A legacy trailing slash only matches descendants, not the directory
        // itself. Preserve that boundary while resolving its directory alias.
        const bool descendants_only = literal_path.size() > 1 && literal_path.back() == '/';
        std::string path;
        if (!resolve_virtual_path(literal_path, path, error, true))
            return false;
        if (descendants_only && path.back() != '/')
            path += '/';
        const auto [it, inserted] = resolved.emplace(path, mode);
        if (!inserted && it->second != mode) {
            error = "conflicting module rule aliases resolve to " + path;
            return false;
        }
    }
    output.clear();
    for (const auto& [path, mode] : resolved)
        output.push_back({path, mode});
    return true;
}

// Bulk MERGE materialization cannot process whiteouts or apply per-entry
// partition aliases. Fall back to the same compiler's ADD/HIDE walk for those
// trees. Unsupported replacement metadata still fails before any ioctl.
bool merge_tree_supported(const fs::path& root, const std::string& virtual_root, bool& merge,
                          std::string& error) {
    merge = true;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
        const auto source = it->path();
        const auto status = it->symlink_status(ec);
        if (ec)
            break;
        bool opaque = false;
        if (source.filename() == ".replace" ||
            (fs::is_directory(status) &&
             (!fsutil::directory_is_opaque(source.string(), opaque) || opaque))) {
            error = "unsupported replacement metadata: " + source.string();
            return false;
        }
        bool whiteout = false;
        if (!fsutil::path_is_whiteout(source.string(), whiteout)) {
            error = "cannot read module entry metadata: " + source.string();
            return false;
        }
        const auto virtual_path =
            (fs::path(virtual_root) / source.lexically_relative(root)).generic_string();
        std::string resolved;
        if (!resolve_virtual_path(virtual_path, resolved, error, fs::is_directory(status)))
            return false;
        if (whiteout || it.depth() >= 8 || fs::is_symlink(status) || resolved != virtual_path)
            merge = false;
    }
    if (ec)
        error = "scan failed for " + root.string() + ": " + ec.message();
    return !ec;
}

bool compile_tree(const fs::path& source_root, const std::string& virtual_root,
                  const std::vector<ModuleRule>& rules, ModuleMountPlan& batch,
                  std::string& error) {
    std::error_code ec;
    const auto root_status = fs::symlink_status(source_root, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(root_status)))
        return true;
    if (ec) {
        error = "cannot inspect module partition " + source_root.string() + ": " + ec.message();
        return false;
    }
    if (!fs::is_directory(root_status)) {
        error = "module partition must be a real directory: " + source_root.string();
        return false;
    }
    std::string resolved_root;
    if (!resolve_virtual_path(virtual_root, resolved_root, error, true))
        return false;
    const auto root_mode = effective_mode(resolved_root, rules);
    if (root_mode == "hide" || (root_mode == "none" && !has_nested_rule(resolved_root, rules)))
        return true;
    bool root_opaque;
    if (!fsutil::directory_is_opaque(source_root.string(), root_opaque) || root_opaque) {
        error = "unsupported partition replacement: " + source_root.string();
        return false;
    }
    auto it = fs::recursive_directory_iterator(source_root, ec);
    const auto end = fs::recursive_directory_iterator();
    for (; it != end && !ec; it.increment(ec)) {
        const fs::path source = it->path();
        const fs::path relative = source.lexically_relative(source_root);
        if (ec) {
            break;
        }
        const auto source_status = it->symlink_status(ec);
        if (ec)
            break;
        if (fs::is_symlink(source_status) && fsutil::module_partition_alias(source))
            continue;  // The installer alias is compiled through the top-level partition.
        std::string virtual_path;
        if (!resolve_virtual_path((fs::path(virtual_root) / relative).generic_string(),
                                  virtual_path, error, fs::is_directory(source_status)))
            return false;
        std::string mode = effective_mode(virtual_path, rules);
        if (mode == "hide") {
            batch.hide.insert(virtual_path);
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (mode == "none") {
            if (it->is_directory(ec) && !has_nested_rule(virtual_path, rules)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (mode != "sumh" && mode != "auto") {
            // A module selected for SUMH cannot safely create an OverlayFS or
            // Magic subtree after boot. Keep the path visible through SUMH
            // rather than silently dropping it; mixed-backend planning remains
            // a boot-time concern for a later planner pass.
            mode = "sumh";
        }

        if (source.filename() == ".replace") {
            error = "unsupported replacement marker: " + source.string();
            return false;
        }
        struct stat st = {};
        if (lstat(source.c_str(), &st) != 0) {
            error = "cannot inspect module entry: " + source.string();
            return false;
        }
        bool whiteout = false;
        if (!fsutil::path_is_whiteout(source.string(), whiteout)) {
            error = "cannot read module entry metadata: " + source.string();
            return false;
        }
        if (whiteout) {
            batch.hide.insert(virtual_path);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            bool opaque = false;
            if (!fsutil::directory_is_opaque(source.string(), opaque) || opaque) {
                error = "unsupported replacement metadata: " + source.string();
                return false;
            }
            std::error_code target_ec;
            if (fs::is_directory(virtual_path, target_ec) && !target_ec &&
                !has_nested_rule(virtual_path, rules)) {
                bool merge = false;
                if (!merge_tree_supported(source, virtual_path, merge, error))
                    return false;
                if (merge) {
                    batch.mappings.push_back({virtual_path, source.string(), true});
                    it.disable_recursion_pending();
                }
            }
            continue;
        }
        if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
            if (S_ISLNK(st.st_mode)) {
                std::error_code target_ec;
                const auto target_status = fs::symlink_status(virtual_path, target_ec);
                if (target_ec && target_ec != std::errc::no_such_file_or_directory &&
                    target_ec != std::errc::not_a_directory) {
                    error = "cannot inspect virtual symlink " + virtual_path + ": " +
                            target_ec.message();
                    return false;
                }
                if (!target_ec && fs::is_symlink(target_status)) {
                    std::error_code link_ec;
                    const fs::path module_link = fs::read_symlink(source, link_ec);
                    if (!link_ec) {
                        const fs::path system_link = fs::read_symlink(virtual_path, link_ec);
                        if (!link_ec) {
                            const fs::path parent = fs::path(virtual_path).parent_path();
                            const auto destination = [&](const fs::path& link) {
                                return (link.is_absolute() ? link : parent / link)
                                    .lexically_normal();
                            };
                            if (destination(module_link) == destination(system_link)) {
                                mlog("sumh: preserving matching symlink alias " + virtual_path +
                                     " from " + source.string());
                                continue;
                            }
                        }
                    }
                }
                target_ec.clear();
                if (fs::is_directory(virtual_path, target_ec) && !target_ec) {
                    error = "refusing to replace directory with symlink " + virtual_path +
                            " from " + source.string();
                    return false;
                }
                if (target_ec && target_ec != std::errc::no_such_file_or_directory &&
                    target_ec != std::errc::not_a_directory) {
                    error = "cannot resolve virtual symlink " + virtual_path + ": " +
                            target_ec.message();
                    return false;
                }
            }
            batch.mappings.push_back({virtual_path, source.string(), false});
        }
    }
    if (ec) {
        error = "scan failed for " + source_root.string() + ": " + ec.message();
    }
    return !ec;
}

bool disable_kernel_features(std::string* error = nullptr) {
    bool ok = true;
    const auto disable = [&](bool result, const char* name) {
        if (!result) {
            ok = false;
            if (error && error->empty()) {
                *error = std::string("failed to disable SUMH ") + name;
            }
        }
    };
    disable(::sumhp::sumh::set_debug(false), "kernel debug");
    disable(::sumhp::sumh::set_maps_spoof(false), "maps spoof");
    disable(::sumhp::sumh::set_kernel_build(false), "kernel build spoof");
    return ok;
}

}  // namespace

bool restore_managed_maps_rules(const Config& config, std::string& error) {
    if (!validate_maps_path_rules(config.maps_rules, error))
        return false;
    // Probe before resolving paths so an old kernel is reported accurately.
    const auto caps = ::sumhp::sumh::feature_capabilities();
    if (!caps.ok) {
        errno = caps.last_errno;
        error = "failed to query managed maps support: " + std::string(std::strerror(errno));
        return false;
    }
    if (!(caps.bitmask & SUMH_FEATURE_MANAGED_MAPS)) {
        if (config.maps_rules.empty())
            return true;
        errno = EOPNOTSUPP;
        error = "this kernel does not support managed maps rules";
        return false;
    }
    const bool ok = fsutil::run_in_init_mount_ns(
        [&]() {
            std::vector<::sumhp::sumh::MapsRule> resolved;
            std::set<std::pair<unsigned long, unsigned long>> targets;
            for (const auto& rule : config.maps_rules) {
                struct stat target = {};
                struct stat replacement = {};
                const auto read_identity = [&](const std::string& path, struct stat& info,
                                               unsigned long& ino, unsigned long& dev) {
                    if (::stat(path.c_str(), &info) != 0) {
                        error = "cannot resolve maps path " + path + ": " + std::strerror(errno);
                        return false;
                    }
                    if (!S_ISREG(info.st_mode)) {
                        errno = EINVAL;
                        error = "maps path must be a regular file: " + path;
                        return false;
                    }
                    const auto device_major = static_cast<std::uint64_t>(major(info.st_dev));
                    const auto device_minor = static_cast<std::uint64_t>(minor(info.st_dev));
                    // Kernel dev_t uses MKDEV's 12:20 layout, unlike libc st_dev.
                    const auto kernel_device = (device_major << 20) | device_minor;
                    if (!info.st_ino || static_cast<std::uint64_t>(info.st_ino) > ULONG_MAX ||
                        device_major > 0xfff || device_minor > 0xfffff || !kernel_device) {
                        errno = EOVERFLOW;
                        error = "maps file identity is not representable: " + path;
                        return false;
                    }
                    ino = static_cast<unsigned long>(info.st_ino);
                    dev = static_cast<unsigned long>(kernel_device);
                    return true;
                };
                ::sumhp::sumh::MapsRule item;
                if (!read_identity(rule.target_path, target, item.target_ino, item.target_dev) ||
                    !read_identity(rule.replacement_path, replacement, item.spoofed_ino,
                                   item.spoofed_dev))
                    return false;
                if (!targets.emplace(item.target_ino, item.target_dev).second) {
                    errno = EEXIST;
                    error = "maps target paths resolve to the same file: " + rule.target_path;
                    return false;
                }
                item.spoofed_path = rule.replacement_path;
                resolved.push_back(std::move(item));
            }
            if (!::sumhp::sumh::replace_managed_maps_rules(resolved)) {
                error =
                    "failed to replace managed maps rules: " + std::string(std::strerror(errno));
                return false;
            }
            return true;
        },
        error);
    if (!ok && error.empty())
        error = "failed to enter init mount namespace for managed maps rules";
    return ok;
}

bool restore_managed_kstat_rules(const Config& config, std::string& error) {
    if (!validate_kstat_path_rules(config.kstat_rules, error))
        return false;
    const auto caps = ::sumhp::sumh::feature_capabilities();
    if (!caps.ok) {
        errno = caps.last_errno;
        error = "failed to query managed kstat support: " + std::string(std::strerror(errno));
        return false;
    }
    if (!(caps.bitmask & SUMH_FEATURE_MANAGED_KSTAT)) {
        if (config.kstat_rules.empty())
            return true;
        errno = EOPNOTSUPP;
        error = "this kernel does not support managed kstat rules";
        return false;
    }
    const bool ok = fsutil::run_in_init_mount_ns(
        [&]() {
            std::vector<::sumhp::sumh::KstatRule> resolved;
            for (const auto& rule : config.kstat_rules) {
                ::sumhp::sumh::KstatRule item;
                item.target_path = rule.target_path;
                if (rule.mode == "custom") {
                    KstatCustomValues fields;
                    if (!parse_kstat_custom_fields(rule.custom_fields, fields, error))
                        return false;
                    if (fields.ino > ULONG_MAX) {
                        errno = EOVERFLOW;
                        error = "custom kstat inode is not representable";
                        return false;
                    }
                    item.field_mask = fields.field_mask;
                    item.spoofed_dev = fields.dev;
                    item.spoofed_ino = static_cast<unsigned long>(fields.ino);
                    item.spoofed_nlink = fields.nlink;
                    item.spoofed_size = fields.size;
                    item.spoofed_atime_sec = fields.atime_sec;
                    item.spoofed_atime_nsec = fields.atime_nsec;
                    item.spoofed_mtime_sec = fields.mtime_sec;
                    item.spoofed_mtime_nsec = fields.mtime_nsec;
                    item.spoofed_ctime_sec = fields.ctime_sec;
                    item.spoofed_ctime_nsec = fields.ctime_nsec;
                    item.spoofed_blksize = fields.blksize;
                    item.spoofed_blocks = fields.blocks;
                } else {
                    ::sumhp::sumh::RawKstat source;
                    if (!::sumhp::sumh::raw_kstat(rule.source_path, source)) {
                        error = "cannot query unprojected kstat source " + rule.source_path + ": " +
                                std::strerror(errno);
                        return false;
                    }
                    if (source.ino > ULONG_MAX) {
                        errno = EOVERFLOW;
                        error = "kstat source inode is not representable: " + rule.source_path;
                        return false;
                    }
                    item.spoofed_ino = static_cast<unsigned long>(source.ino);
                    item.spoofed_dev = source.dev;
                    item.field_mask = SUMH_KSTAT_FIELD_DEV | SUMH_KSTAT_FIELD_INO;
                    if (rule.mode == "full") {
                        if (source.size < 0) {
                            errno = EOVERFLOW;
                            error = "kstat source size is not representable: " + rule.source_path;
                            return false;
                        }
                        item.field_mask = SUMH_KSTAT_FIELD_ALL;
                        item.spoofed_nlink = source.nlink;
                        item.spoofed_size = source.size;
                        item.spoofed_atime_sec = source.atime_sec;
                        item.spoofed_atime_nsec = source.atime_nsec;
                        item.spoofed_mtime_sec = source.mtime_sec;
                        item.spoofed_mtime_nsec = source.mtime_nsec;
                        item.spoofed_ctime_sec = source.ctime_sec;
                        item.spoofed_ctime_nsec = source.ctime_nsec;
                        item.spoofed_blksize = source.blksize;
                        item.spoofed_blocks = source.blocks;
                    }
                }
                resolved.push_back(std::move(item));
            }
            if (!::sumhp::sumh::replace_managed_kstat_rules(resolved)) {
                error =
                    "failed to replace managed kstat rules: " + std::string(std::strerror(errno));
                return false;
            }
            return true;
        },
        error);
    if (!ok && error.empty())
        error = "failed to enter init mount namespace for managed kstat rules";
    return ok;
}

bool apply_maps_config(const Config& config, std::string& error, bool restore_rules) {
    error.clear();
    if (restore_rules && config.enable_maps_spoof && !restore_managed_maps_rules(config, error))
        return false;
    if (!::sumhp::sumh::set_maps_spoof(config.enable_maps_spoof)) {
        error = std::string("failed to ") + (config.enable_maps_spoof ? "enable" : "disable") +
                " SUMH maps spoof: " + std::strerror(errno);
        return false;
    }
    // Disabling must work even if a saved path has disappeared. Discard stale
    // managed identities while retaining saved paths and unmanaged numeric rules.
    if (restore_rules && !config.enable_maps_spoof &&
        !::sumhp::sumh::replace_managed_maps_rules({})) {
        error = "failed to clear managed maps rules: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

bool apply_feature_config(const Config& config, std::string& error, bool apply_immediately,
                          bool apply_maps) {
    // Built-in protections are intrinsic to the kernel and have no user-space switches.
    mlog("apply optional features: maps=" + std::to_string(config.enable_maps_spoof) +
         " kernel_build=" + std::to_string(config.enable_kernel_build_spoof));
    bool ok = true;
    const auto apply = [&](bool result, const char* name) {
        if (!result) {
            if (error.empty()) {
                error = std::string("failed to set SUMH ") + name;
            }
            ok = false;
        }
    };

    apply(::sumhp::sumh::set_debug(config.enable_kernel_debug), "kernel debug");
    if (apply_immediately || !config.enable_kernel_build_spoof ||
        config.kernel_build_apply_stage == "post-fs-data" ||
        ksud::getprop("sys.boot_completed") == "1") {
        apply(::sumhp::sumh::set_kernel_build(config.enable_kernel_build_spoof,
                                              config.kernel_build_release,
                                              config.kernel_build_version),
              "kernel build spoof");
    }
    if (!ok) {
        (void)disable_kernel_features();
        return false;
    }
    if (!apply_maps)
        return true;
    return apply_maps_config(config, error,
                             apply_immediately || ksud::getprop("sys.boot_completed") == "1");
}

bool reset_feature_state(std::string& error) {
    user_hide_restore_pending = false;
    return disable_kernel_features(&error);
}

namespace {
bool restore_user_hide_rules(std::string& error, bool retry) {
    if (ksud::getprop("sys.boot_completed") != "1" || !::sumhp::sumh::is_available()) {
        user_hide_restore_pending = false;
        return true;
    }
    std::vector<std::string> paths;
    if (!load_user_hide_rules(paths, error)) {
        user_hide_restore_pending = true;
        return false;
    }
    bool managed = false;
    if (!::sumhp::sumh::managed_hide_mode(managed)) {
        user_hide_restore_pending = true;
        error = "failed to query user hide ownership";
        return false;
    }
    if (managed) {
        // The kernel retries binding registered definitions. Failed queries or
        // mutations still need userspace to retry synchronizing those definitions.
        user_hide_restore_pending = true;
        std::vector<::sumhp::sumh::UserHideRule> registered;
        if (!::sumhp::sumh::user_hide_rules(registered)) {
            error = "failed to query managed user hide rules";
            return false;
        }
        const std::set<std::string> desired_paths(paths.begin(), paths.end());
        std::set<std::string> registered_paths;
        bool ok = true;
        for (const auto& rule : registered) {
            registered_paths.insert(rule.path);
            if (desired_paths.count(rule.path) == 0)
                ok = ::sumhp::sumh::delete_user_hide(rule.path, rule.id) && ok;
        }
        for (const auto& path : paths) {
            if (registered_paths.insert(path).second)
                ok = ::sumhp::sumh::upsert_user_hide(path) && ok;
        }
        user_hide_restore_pending = !ok;
        if (!ok)
            error = "failed to synchronize managed user hide rules; module rules remain active";
        return ok;
    }
    const int enabled = ::sumhp::sumh::enabled_state();
    if (enabled == 0) {
        user_hide_restore_pending = false;
        return true;
    }
    if (enabled < 0) {
        user_hide_restore_pending = true;
        error = "failed to query SUMH state before restoring user hide rules";
        if (!retry)
            mlog("sumh: " + error, logging::Level::Error);
        return false;
    }
    const bool ok = fsutil::run_in_init_mount_ns([retry, &paths]() {
        bool restored = true;
        for (const auto& path : paths) {
            struct stat parent = {};
            const bool parent_ready =
                !retry || (stat(fs::path(path).parent_path().c_str(), &parent) == 0 &&
                           S_ISDIR(parent.st_mode));
            const bool hidden = parent_ready && ::sumhp::sumh::hide_path(path);
            if (!retry)
                mlog("user hide path=" + path +
                         (hidden ? " ok" : " failed errno=" + std::to_string(errno)),
                     hidden ? logging::Level::Debug : logging::Level::Error);
            restored = hidden && restored;
        }
        return restored;
    });
    user_hide_restore_pending = !ok;
    if (!ok) {
        error = "user hide rules pending retry; module rules remain active";
        if (!retry)
            mlog("sumh: " + error, logging::Level::Error);
    } else if (retry) {
        mlog("sumh: pending user hide rules restored");
    }
    return ok;
}
}  // namespace

bool restore_persisted_hide_rules(std::string& error) {
    return restore_user_hide_rules(error, false);
}

bool has_pending_hide_rules() {
    return user_hide_restore_pending;
}

void retry_pending_hide_rules() {
    if (!user_hide_restore_pending)
        return;
    Config config;
    std::string error;
    if (!read_config_file(config, error))
        return;
    (void)restore_user_hide_rules(error, true);
}

bool reset_runtime_state(std::string& error) {
    bool ok = reset_feature_state(error);
    if (!::sumhp::sumh::clear_rules()) {
        if (error.empty()) {
            error = "failed to clear SUMH rules";
        }
        ok = false;
    }
    invalidate_active_state();
    return ok;
}

bool prepare_modules(const std::vector<ModuleEntry>& modules, const Config& config,
                     const ModuleRuleMap& rules, ModuleMountPlan& plan, std::string& error) {
    if (!::sumhp::sumh::is_available()) {
        error = "SUMH backend requested but protocol is unavailable";
        return false;
    }
    const auto partitions = fsutil::managed_partitions(config.partitions);
    ModuleMountPlan batch;
    ModuleRuleMap resolved_rules;
    for (const auto& module : modules) {
        const auto rule_it = rules.find(module.id);
        if (rule_it == rules.end())
            continue;
        auto& module_rules = resolved_rules[module.id];
        if (!resolve_module_rules(rule_it->second, module_rules, error)) {
            error.insert(0, ": ");
            error.insert(0, module.id);
            return false;
        }
        for (const auto& rule : module_rules) {
            if (rule.mode == "hide")
                batch.hide.insert(rule.path);
        }
    }
    // Kernel rules use last-write-wins semantics. Enumerated modules are sorted
    // by id, so emit them backwards to retain the existing module priority.
    for (auto it = modules.rbegin(); it != modules.rend(); ++it) {
        const auto& module_rules = resolved_rules[it->id];
        for (const auto& partition : partitions) {
            if (!compile_tree(it->path / partition, "/" + partition, module_rules, batch, error)) {
                error.insert(0, ": ");
                error.insert(0, it->id);
                return false;
            }
        }
    }
    plan = std::move(batch);
    error.clear();
    return true;
}

bool apply_modules(const ModuleMountPlan& batch) {
    bool ok = true;
    size_t add_count = 0;
    size_t merge_count = 0;
    for (const auto& rule : batch.mappings) {
        const bool added = rule.merge ? ::sumhp::sumh::add_merge_rule(rule.path, rule.source)
                                      : ::sumhp::sumh::add_rule(rule.path, rule.source, 0);
        rule.merge ? ++merge_count : ++add_count;
        mlog(std::string(rule.merge ? "merge" : "add") + " rule target=" + rule.path + " source=" +
                 rule.source + (added ? " ok" : " failed errno=" + std::to_string(errno)),
             added ? logging::Level::Debug : logging::Level::Error);
        ok = added && ok;
    }
    for (const auto& path : batch.hide) {
        const bool hidden = ::sumhp::sumh::hide_path(path);
        mlog("hide path=" + path + (hidden ? " ok" : " failed errno=" + std::to_string(errno)),
             hidden ? logging::Level::Debug : logging::Level::Error);
        ok = hidden && ok;
    }
    std::error_code ec;
    fs::create_directories(active_file().parent_path(), ec);
    if (ok && !write_boot_marker(active_file())) {
        ok = false;
        mlog("sumh: failed to record restored runtime state", logging::Level::Error);
    }
    if (ok) {
        mlog("sumh: installed add=" + std::to_string(add_count) + " merge=" +
             std::to_string(merge_count) + " hide=" + std::to_string(batch.hide.size()));
    } else {
        fs::remove(active_file(), ec);
        mlog("sumh: one or more rule operations failed", logging::Level::Error);
    }
    return ok;
}

bool mount_modules(const std::vector<ModuleEntry>& modules, const Config& config,
                   const ModuleRuleMap& rules) {
    ModuleMountPlan plan;
    std::string error;
    if (!prepare_modules(modules, config, rules, plan, error)) {
        mlog("sumh: " + error, logging::Level::Error);
        return false;
    }
    return apply_modules(plan);
}

bool unmount_all(const Config& config) {
    (void)config;  // shared storage is released centrally after OverlayFS too.
    bool ok = true;
    if (::sumhp::sumh::is_available()) {
        std::string error;
        ok = reset_runtime_state(error);
        if (!ok) {
            mlog("sumh: " + error);
        }
    }
    std::error_code ec;
    fs::remove(active_file(), ec);
    clear_replayable_mappings();
    return ok;
}

bool is_active() {
    return marker_matches_current_boot(active_file());
}

void invalidate_active_state() {
    user_hide_restore_pending = false;
    std::error_code ec;
    fs::remove(active_file(), ec);
}

std::vector<std::string> replayable_module_ids() {
    std::vector<std::string> ids;
    if (!marker_matches_current_boot(mapping_plan_file())) {
        return ids;
    }
    std::ifstream in(mapping_plan_file());
    std::string line;
    std::getline(in, line);  // boot id
    while (std::getline(in, line)) {
        if (!line.empty()) {
            ids.push_back(line);
        }
    }
    return ids;
}

bool has_replayable_mappings() {
    return !replayable_module_ids().empty();
}

bool record_replayable_mappings(const std::vector<ModuleEntry>& modules) {
    if (modules.empty()) {
        clear_replayable_mappings();
        return true;
    }
    std::ostringstream ids;
    for (const auto& module : modules) {
        ids << module.id << "\n";
    }
    if (write_boot_marker(mapping_plan_file(), ids.str())) {
        return true;
    }
    clear_replayable_mappings();
    return false;
}

void clear_replayable_mappings() {
    std::error_code ec;
    fs::remove(mapping_plan_file(), ec);
}

}  // namespace sumhp::mount::sumh
