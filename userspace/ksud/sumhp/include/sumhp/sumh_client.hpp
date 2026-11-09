#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sumhp::sumh {

enum class Status {
    Available = 0,
    NotPresent = 1,
    KernelTooOld = 2,
    ClientTooOld = 3,
};

struct VersionInfo {
    int expected_protocol = 0;
    int kernel_protocol = 0;
    int last_errno = 0;
    int process_uid = -1;
    int process_euid = -1;
    Status status = Status::NotPresent;
};

struct FeatureCapabilities {
    bool ok = false;
    int last_errno = 0;
    int bitmask = 0;
};

struct KernelBuild {
    bool enabled = false;
    std::string release;
    std::string version;
};

struct MapsRule {
    unsigned long target_ino = 0;
    unsigned long target_dev = 0;
    unsigned long spoofed_ino = 0;
    unsigned long spoofed_dev = 0;
    std::string spoofed_path;
};

struct KstatRule {
    std::string target_path;
    std::uint32_t field_mask = 0;
    unsigned long spoofed_ino = 0;
    unsigned long spoofed_dev = 0;
    unsigned int spoofed_nlink = 0;
    std::int64_t spoofed_size = 0;
    std::int64_t spoofed_atime_sec = 0;
    long spoofed_atime_nsec = 0;
    std::int64_t spoofed_mtime_sec = 0;
    long spoofed_mtime_nsec = 0;
    std::int64_t spoofed_ctime_sec = 0;
    long spoofed_ctime_nsec = 0;
    unsigned long spoofed_blksize = 0;
    std::uint64_t spoofed_blocks = 0;
};

struct RawKstat {
    std::uint64_t ino = 0;
    std::uint32_t dev = 0;
    std::uint32_t nlink = 0;
    std::int64_t size = 0;
    std::int64_t atime_sec = 0;
    std::int64_t atime_nsec = 0;
    std::int64_t mtime_sec = 0;
    std::int64_t mtime_nsec = 0;
    std::int64_t ctime_sec = 0;
    std::int64_t ctime_nsec = 0;
    std::uint32_t blksize = 0;
    std::uint64_t blocks = 0;
};

struct ManagedKstatStatus {
    std::uint32_t active_count = 0;
    std::uint64_t generation = 0;
};

struct UserHideRule {
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    std::uint32_t management = 0;
    std::uint32_t binding = 0;
    int error = 0;
    std::string path;
};

bool user_hide_rules(std::vector<UserHideRule>& rules);
bool upsert_user_hide(const std::string& path, UserHideRule* result = nullptr);
bool delete_user_hide(const std::string& path, std::uint64_t id = 0);
bool retry_user_hide(const std::string& path);
// A retained managed definition prevents switching to legacy HIDE after an error.
bool managed_hide_mode(bool& managed);

VersionInfo version_info();
bool is_available();
std::string active_rules();
std::string hooks();
FeatureCapabilities feature_capabilities();
int features();
std::vector<std::string> feature_names(int bitmask);
std::vector<std::string> active_modules_from_rules(const std::string& rules);

bool set_debug(bool enable);
bool fix_mounts();
bool hide_overlay_xattrs(const std::string& path);
bool clear_overlay_xattr_hiding();
bool set_maps_spoof(bool enable);
// Empty enabled fields retain ORIGINAL values; at least one custom field is
// required. Disabling ignores both supplied strings.
bool set_kernel_build(bool enable, const std::string& release = {},
                      const std::string& version = {});
bool kernel_build(KernelBuild& state);
bool original_kernel_build(KernelBuild& state);
bool clear_rules();
bool add_rule(const std::string& target, const std::string& source, int type);
bool add_merge_rule(const std::string& target, const std::string& source);
bool hide_path(const std::string& path);
bool delete_rule(const std::string& path);
bool add_maps_rule(unsigned long target_ino, unsigned long target_dev, unsigned long spoofed_ino,
                   unsigned long spoofed_dev, const std::string& spoofed_path);
bool clear_maps_rules();
// Atomically replaces only the managed set. Legacy numeric rules are retained.
bool replace_managed_maps_rules(const std::vector<MapsRule>& rules);
// Atomically replaces only managed kstat rules; legacy explicit rules survive.
bool replace_managed_kstat_rules(const std::vector<KstatRule>& rules);
bool raw_kstat(const std::string& source_path, RawKstat& result);
bool managed_kstat_status(ManagedKstatStatus& result);
int enabled_state();
int process_uid();
int process_euid();

}  // namespace sumhp::sumh
