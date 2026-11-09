#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "mount/backend.hpp"

namespace sumhp::mount::sumh {

// Mount orchestration honors kernel-build boot scheduling. Explicit runtime
// config apply passes apply_immediately=true. The kernel engine is always on.
// Mount transactions defer Maps to the shared helper so its failures can be
// reported without rolling back successfully installed module mappings.
bool apply_feature_config(const Config& config, std::string& error, bool apply_immediately = false,
                          bool apply_maps = true);
// Resolve every path in init's mount namespace before atomically replacing the
// managed maps set. Resolution or transport errors preserve the previous set.
bool restore_managed_maps_rules(const Config& config, std::string& error);
// Shared lifecycle for explicit apply, boot restoration and feature config.
// Before module mounts are ready, restore_rules=false applies only the switch.
bool apply_maps_config(const Config& config, std::string& error, bool restore_rules = true);
// Snapshot source stat values in init's mount namespace, then replace the
// entire managed set with one ioctl. An invalid path leaves live rules intact.
bool restore_managed_kstat_rules(const Config& config, std::string& error);

// Reset runtime features without discarding path rules. The SUMH engine
// remains enabled while this cleanup is performed.
bool reset_feature_state(std::string& error);

// Restore user HIDE rules in init's mount namespace after boot completion,
// when SUMH is available. Failures must not roll back module mappings.
bool restore_persisted_hide_rules(std::string& error);
bool has_pending_hide_rules();
// Called by the daemon's event loop; reloads current rules before retrying.
void retry_pending_hide_rules();

// Reset runtime features and clear path rules without requiring an active mirror.
bool reset_runtime_state(std::string& error);

// An ephemeral batch compiled from the existing module trees. Preparation is
// read-only; keep this batch in the same init-namespace callback as reset/apply.
struct ModuleMountPlan {
    struct Mapping {
        std::string path, source;
        bool merge;
    };
    std::vector<Mapping> mappings;
    std::set<std::string> hide;
};

bool prepare_modules(const std::vector<ModuleEntry>& modules, const Config& config,
                     const ModuleRuleMap& rules, ModuleMountPlan& plan, std::string& error);
bool apply_modules(const ModuleMountPlan& batch);

// Compiles each enabled module's /data/adb/modules tree into ADD/MERGE/HIDE
// SUMH rules that redirect straight to the source inode (no mirror; the vnode
// clones the source SELinux SID). Must run in PID 1's mount namespace so the
// rules resolve against the init-namespace view.
bool mount_modules(const std::vector<ModuleEntry>& modules, const Config& config,
                   const ModuleRuleMap& rules);

// Removes SUMHP-owned SUMH rules. SUMH holds no mirror storage of its own.
// Must run in the init mount namespace.
bool unmount_all(const Config& config);

bool is_active();
void invalidate_active_state();
bool has_replayable_mappings();
std::vector<std::string> replayable_module_ids();
bool record_replayable_mappings(const std::vector<ModuleEntry>& modules);
void clear_replayable_mappings();

}  // namespace sumhp::mount::sumh
