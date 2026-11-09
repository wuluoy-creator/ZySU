#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace json {
struct Value;
}

namespace sumhp {

// Keep the serialized array, including Android JSON request-string escaping,
// within the Manager JNI and daemon budgets with room for other settings.
inline constexpr std::size_t MAPS_RULES_MAX_ENCODED_BYTES = 56 * 1024;

struct MapsPathRule {
    std::string target_path;
    std::string replacement_path;
};

struct KstatPathRule {
    std::string target_path;
    std::string source_path;
    std::string mode;
    // Flat internal keys such as "mtime.sec"; JSON uses nested time objects.
    std::map<std::string, std::string> custom_fields = {};
};

struct KstatCustomValues {
    std::uint32_t field_mask = 0;
    std::uint32_t dev = 0;
    std::uint64_t ino = 0;
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

struct Config {
    std::string mount_source = "KSU";
    std::string fs_type = "auto";
    std::string work_dir = "/dev/sumhp";
    std::string mirror_dir;
    int mirror_img_size_mb = 2048;
    bool overlay_writable = false;
    std::vector<std::string> partitions;
    bool debug = false;
    bool verbose = false;
    bool enable_kernel_debug = false;
    bool enable_stealth = true;
    bool enable_overlay_xattr_hide = false;
    bool enable_mount_hide = false;
    std::string mount_hide_mode = "normal";
    bool enable_maps_spoof = false;
    std::vector<MapsPathRule> maps_rules;
    std::vector<KstatPathRule> kstat_rules;
    bool enable_statfs_spoof = false;
    bool enable_kernel_build_spoof = false;
    std::string kernel_build_release;
    std::string kernel_build_version;
    std::string kernel_build_apply_stage = "post-fs-data";
    bool overlayfs_enabled = true;
    bool magic_mount_enabled = true;
    std::string mount_backend = "auto";
};

std::vector<std::string> load_user_hide_rules();
// Missing files represent an empty rule set; read/parse failures leave rules
// unchanged so synchronization cannot mistake damaged persistence for deletion.
bool load_user_hide_rules(std::vector<std::string>& rules, std::string& error);
bool save_user_hide_rules(const std::vector<std::string>& rules);
std::string default_config_json();
void prune_config_fields(json::Value& config);
bool validate_config_patch(const json::Value& patch, std::string& error);
json::Value maps_path_rules_json(const std::vector<MapsPathRule>& rules);
bool validate_maps_path_rules(const std::vector<MapsPathRule>& rules, std::string& error);
json::Value kstat_path_rules_json(const std::vector<KstatPathRule>& rules);
bool validate_kstat_path_rules(const std::vector<KstatPathRule>& rules, std::string& error);
bool parse_kstat_custom_fields(const std::map<std::string, std::string>& fields,
                               KstatCustomValues& values, std::string& error);
bool write_default_config(std::string& error);
bool parse_config_json(const std::string& json, Config& config, std::string& error);
bool read_config_file(Config& config, std::string& error);
bool merge_config_json(const std::string& updates, std::string& error);
// Read, edit, persist, and apply under one config file lock so CLI operations
// cannot commit an older snapshot after a newer edit. A null replacement removes
// target_path; otherwise it upserts the replacement.
struct KstatRuleEditResult {
    bool saved = false;
    bool applied = false;
    int apply_errno = 0;
};
struct KstatRuleApplyResult {
    bool loaded = false;
    bool applied = false;
    int apply_errno = 0;
};
KstatRuleEditResult edit_kstat_path_rule(
    const std::string& target_path, const KstatPathRule* replacement, Config& config,
    std::string& error, const std::function<bool(const Config&, std::string&)>& apply);
KstatRuleApplyResult apply_saved_kstat_rules(
    Config& config, std::string& error,
    const std::function<bool(const Config&, std::string&)>& apply);
// Repair only invalid kernel-build fields while disabling the saved feature.
bool reset_kernel_build_config(std::string& error);

}  // namespace sumhp
