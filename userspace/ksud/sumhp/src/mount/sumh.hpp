#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/restore_policy.hpp"
#include "mount/backend.hpp"
#include "mount/feature_result.hpp"

namespace sumhp::mount::sumh {

// Mount orchestration honors kernel-build boot scheduling. Explicit runtime
// config apply passes apply_immediately=true. The kernel engine is always on.
// Mount transactions defer Maps to the shared helper so its failures can be
// reported without rolling back successfully installed module mappings.
bool apply_feature_config(const Config& config, std::string& error, bool apply_immediately = false,
                          bool apply_maps = true, FeatureApplyReport* output = nullptr);
// Resolve every path in init's mount namespace before atomically replacing the
// managed maps set. Confirmed validation/replacement rejection preserves it;
// missing child replies leave the commit outcome unknown and must not be replayed.
bool restore_managed_maps_rules(const Config& config, std::string& error,
                                ManagedMapsRestoreState* state = nullptr,
                                RestoreTrigger trigger = RestoreTrigger::Automatic);
// Shared lifecycle for explicit apply, boot restoration and feature config.
// Before module mounts are ready, restore_rules=false applies only the switch.
bool apply_maps_config(const Config& config, std::string& error, bool restore_rules = true,
                       FeatureApplyOutcome* outcome = nullptr,
                       RestoreTrigger trigger = RestoreTrigger::Automatic);

// Reset runtime features without discarding path rules. The SUMH engine
// remains enabled while this cleanup is performed.
bool reset_feature_state(std::string& error);

// Restore user HIDE rules in init's mount namespace after boot completion,
// when SUMH is available. Failures must not roll back module mappings.
bool restore_persisted_hide_rules(std::string& error,
                                  RestoreTrigger trigger = RestoreTrigger::Automatic);
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

// Contributions produced by the real compiler, not a claim about a running
// kernel's final view. MERGE overlap and other backends require separate checks.
struct ModulePlanEvent {
    std::string module_id;
    std::string original_path, resolved_path, source;
    std::string action, reason;
    std::string matched_rule_path, requested_mode, effective_mode;
    std::string hidden_parent;
    std::string previous_module_id, previous_source;
    bool alias = false;
    std::size_t mapping_index = 0;
    bool has_previous_direct = false;
};

struct ModuleSourceObservation {
    std::string module_id, path, metadata;
};

struct ModulePlanDiagnostics {
    static constexpr std::size_t max_events = 2048;
    static constexpr std::size_t max_observations = 4096;
    static constexpr std::size_t max_actions = 4096;
    static constexpr std::size_t max_bytes = std::size_t{512} * 1024;
    std::vector<ModulePlanEvent> events;
    std::vector<ModuleSourceObservation> observations;
    std::size_t bytes = 0;
    bool events_truncated = false;
    bool observations_truncated = false;
    bool complete = false;
};

bool prepare_modules(const std::vector<ModuleEntry>& modules, const Config& config,
                     const ModuleRuleMap& rules, ModuleMountPlan& plan, std::string& error);
// Shared path resolution for explain filtering; caller validates rule grammar.
bool resolve_module_plan_path(const std::string& value, std::string& resolved, std::string& error);
// Same compiler as prepare_modules, with bounded optional diagnostics. Source
// observations are metadata samples, not pinned contents or an ABA-free view.
// Diagnostic capacity exhaustion stops preparation and preserves the old plan.
// Production prepare_modules retains its existing capacity/ordering behavior.
bool prepare_modules_diagnostic(const std::vector<ModuleEntry>& modules, const Config& config,
                                const ModuleRuleMap& rules, ModuleMountPlan& plan,
                                ModulePlanDiagnostics& diagnostics, std::string& error);
bool apply_modules(const ModuleMountPlan& batch);

// Compile and execute the recorded legacy rebuild in the SAME init namespace
// child. Lost child replies always yield PartialOrUnknown, never Unchanged.
bool rebuild_modules_in_init_ns(const std::vector<ModuleEntry>& modules, const Config& config,
                                const ModuleRuleMap& rules, ModuleRebuildOutcome& outcome,
                                std::string& error);

// Compiles each enabled module's /data/adb/modules tree into ADD/MERGE/HIDE
// SUMH rules that redirect straight to the source inode (no mirror; the vnode
// clones the source SELinux SID). Must run in PID 1's mount namespace so the
// rules resolve against the init-namespace view.
bool mount_modules(const std::vector<ModuleEntry>& modules, const Config& config,
                   const ModuleRuleMap& rules);

// Legacy global reset, not owner-scoped removal. SUMH has no mirror storage.
// Must run in the init mount namespace.
bool unmount_all(const Config& config);

bool is_active();
void invalidate_active_state();
bool has_replayable_mappings();
std::vector<std::string> replayable_module_ids();
bool record_replayable_mappings(const std::vector<ModuleEntry>& modules);
void clear_replayable_mappings();

}  // namespace sumhp::mount::sumh
