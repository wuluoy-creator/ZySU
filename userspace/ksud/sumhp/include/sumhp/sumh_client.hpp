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

enum class RuntimeFeature : std::uint32_t {
    Debug = 1,
    Stealth = 2,
    MountHide = 3,
    MapsSpoof = 4,
    StatfsSpoof = 5,
    KernelBuild = 6,
};

struct RuntimeFeatureState {
    RuntimeFeature id = RuntimeFeature::Debug;
    bool supported = false;
    bool configured = false;
    bool active = false;
    bool degraded = false;
    int last_error =
        0;  // Negative errno from the most recent kernel setter, not an observer result.
};

struct RuntimeState {
    std::uint64_t generation = 0;
    std::uint64_t instance_id = 0;
    std::uint32_t flags = 0;
    std::uint32_t mount_hide_mode = 0;
    std::vector<RuntimeFeatureState> entries;

    const RuntimeFeatureState* find(RuntimeFeature id) const {
        for (const auto& entry : entries)
            if (entry.id == id)
                return &entry;
        return nullptr;
    }
};

// Caller negotiates the individual capability first. Failures leave output unchanged.
bool runtime_state(RuntimeState& state);

// Registered definitions only. Ordinals are scoped to category + instance +
// generation, never stable mutation handles. Strings retain kernel path bytes.
struct InventoryRequest {
    std::uint32_t category = 0;
    std::uint64_t instance_id = 0;
    std::uint64_t generation = 0;
    std::uint64_t cursor = 0;
};
struct InventoryEntry {
    std::uint32_t category = 0, owner = 0, id_kind = 0, type = 0, flags = 0;
    std::uint64_t id = 0, definition_generation = 0;
    std::uint64_t target_ino = 0, target_dev = 0, spoofed_ino = 0, spoofed_dev = 0;
    std::string path1, path2;
};
struct InventoryPage {
    std::uint32_t category = 0, supported_categories = 0;
    std::uint64_t instance_id = 0, generation = 0, cursor = 0, next_cursor = 0;
    bool at_end = false;
    std::vector<InventoryEntry> entries;  // At most one entry per page.
};
// Caller negotiates rule_inventory. One ioctl, no retry; failure leaves page
// unchanged. Reuse both tokens across every page/category of a collection.
bool rule_inventory(const InventoryRequest& request, InventoryPage& page);
inline const char* runtime_feature_name(RuntimeFeature id) {
    switch (id) {
    case RuntimeFeature::Debug:
        return "kernel_debug";
    case RuntimeFeature::Stealth:
        return "stealth";
    case RuntimeFeature::MountHide:
        return "mount_hide";
    case RuntimeFeature::MapsSpoof:
        return "maps_spoof";
    case RuntimeFeature::StatfsSpoof:
        return "statfs_spoof";
    case RuntimeFeature::KernelBuild:
        return "kernel_build_spoof";
    }
    return "unknown";
}
// Conditional single-flag mutation. A stale lifecycle/generation fails without changing settings.
bool set_runtime_feature(const RuntimeState& expected, RuntimeFeature id, bool enable,
                         std::uint32_t mount_hide_mode = 0);

struct KernelBuildSnapshot {
    RuntimeState runtime;
    KernelBuild current;
    KernelBuild original;
    bool has_original = false;
};

// Brackets values with an unchanged SUMH control token; fails rather than retrying.
// The sequence cannot detect third-party kernel writes to the initial UTS namespace.
bool kernel_build_snapshot(KernelBuildSnapshot& snapshot, bool include_original);
// Enabled fields must already be resolved. Disabling emits canonical empty strings.
bool set_kernel_build_conditional(const RuntimeState& expected, const KernelBuild& desired);

struct MapsRule {
    unsigned long target_ino = 0;
    unsigned long target_dev = 0;
    unsigned long spoofed_ino = 0;
    unsigned long spoofed_dev = 0;
    std::string spoofed_path;
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

enum class MountHideMode : std::uint32_t {
    Normal = 0,
    Aggressive = 1,
};

VersionInfo version_info();
bool is_available();
std::string active_rules();
std::string hooks();
FeatureCapabilities feature_capabilities();
int features();
std::vector<std::string> feature_names(int bitmask);
std::vector<std::string> active_modules_from_rules(const std::string& rules);

bool set_debug(bool enable);
bool set_stealth(bool enable);
bool fix_mounts();
bool hide_overlay_xattrs(const std::string& path);
bool clear_overlay_xattr_hiding();
bool set_mount_hide(bool enable, MountHideMode mode = MountHideMode::Normal);
bool set_maps_spoof(bool enable);
bool set_statfs_spoof(bool enable);
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
// Exact temporary key only, conditional on inventory instance/generation.
// Negotiates individual capabilities; never retries or falls back to clear.
// attempted is true only after invoking the mutation ioctl, not an acknowledgement.
bool delete_temporary_maps_rule(std::uint64_t target_ino, std::uint64_t target_dev,
                                std::uint64_t instance_id, std::uint64_t generation,
                                bool& attempted);
// Atomically replaces only the managed set. Legacy numeric rules are retained.
bool replace_managed_maps_rules(const std::vector<MapsRule>& rules);
// Distinguish the legacy empty-set compatibility no-op from an actual ioctl
// acknowledgement. Diagnostic submission receipts must use this variant.
bool replace_managed_maps_rules_checked(const std::vector<MapsRule>& rules, bool& submitted);
int enabled_state();
int process_uid();
int process_euid();

}  // namespace sumhp::sumh
