#pragma once

#include <cstddef>
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
// Legacy readers see enabled paths only. Reads never migrate the source file.
bool load_user_hide_rules(std::vector<std::string>& rules, std::string& error);
// Compatibility writers migrate legacy arrays under the definition lock.
// Structured membership changes require the definition-aware transaction API
// to preserve IDs, disabled entries, metadata and explicit removal intent.
bool save_user_hide_rules(const std::vector<std::string>& rules);
bool save_user_hide_rules(const std::vector<std::string>& rules, std::string& error);
std::string default_config_json();
bool validate_config_patch(const json::Value& patch, std::string& error);
json::Value maps_path_rules_json(const std::vector<MapsPathRule>& rules);
bool validate_maps_path_rules(const std::vector<MapsPathRule>& rules, std::string& error);
bool write_default_config(std::string& error);
bool parse_config_json(const std::string& json, Config& config, std::string& error);
bool read_config_file(Config& config, std::string& error);
bool merge_config_json(const std::string& updates, std::string& error);
// Repair only invalid kernel-build fields while disabling the saved feature.
bool reset_kernel_build_config(std::string& error);

}  // namespace sumhp
