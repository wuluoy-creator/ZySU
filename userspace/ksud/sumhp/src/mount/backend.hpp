#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "sumhp/config.hpp"

namespace sumhp::mount {

enum class BackendKind {
    SUMH,
    Overlayfs,
    MagicMount,
};

struct BackendStatus {
    BackendKind kind;
    std::string name;
    bool available;
    bool preferred;
    std::string detail;
};

std::vector<BackendStatus> backend_statuses();
std::string backend_kind_name(BackendKind kind);

// A regular module eligible for mounting: /data/adb/modules/<id>.
struct ModuleEntry {
    std::string id;
    std::filesystem::path path;
};

// A path-specific backend override persisted in module_rules.json. The longest
// matching absolute path wins, matching the ZySU hymo control-plane contract.
struct ModuleRule {
    std::string path;
    std::string mode;
};

using ModuleModeMap = std::map<std::string, std::string>;
using ModuleRuleMap = std::map<std::string, std::vector<ModuleRule>>;

struct MountReport {
    bool ok = false;
    int modules = 0;  // enabled modules processed
    int mounts = 0;   // committed partition mount points
    std::string backend;
    std::string detail;
};

// Enabled, mountable modules under runtime_modules_dir(): skips entries with
// no module.prop or with a disable/remove/skip_mount marker.
std::vector<ModuleEntry> enumerate_mountable_modules();
// Read-only discovery; failure leaves modules unchanged. Missing roots and
// unreadable/changed entries are errors, not a complete empty set. The budget
// counts every directory entry, including entries excluded by existing markers.
bool enumerate_mountable_modules_checked(std::vector<ModuleEntry>& modules, std::string& error,
                                         std::size_t max_entries = 16384);

bool load_module_configuration(ModuleModeMap& modes, ModuleRuleMap& rules, std::string& error);
// Pure counterpart of the locked production loader. The caller supplies "{}"
// for a document known to be absent and owns read budgets/change detection.
// Canonical object entries override legacy modes, including explicit auto.
bool parse_module_configuration_documents(const std::string& modes_json,
                                          const std::string& rules_json, ModuleModeMap& modes,
                                          ModuleRuleMap& rules, std::string& error);
bool valid_module_id(const std::string& id);
bool valid_module_rule_path(const std::string& path);
bool parse_module_rules(const std::string& json, std::vector<ModuleRule>& rules,
                        std::string& error);
// Mode and rules share a single atomic commit in module_rules.json. Legacy
// module_mode.json/array entries are imported on the first successful mutation.
bool configure_module(const std::string& id, const std::string& mode,
                      const std::vector<ModuleRule>& rules, std::string& error);
bool set_module_mode(const std::string& id, const std::string& mode, std::string& error);
bool upsert_module_rule(const std::string& id, const ModuleRule& rule, std::string& error);
bool remove_module_rule(const std::string& id, const std::string& path, std::string& error);

// Resolve a module's effective backend ("overlay"|"magic"|"sumh"|"none") from
// its configured mode (modes[id], the global override config.mount_backend, or
// the auto fallback). Used by the orchestrator and to report a module's actual
// mount method. `modes` contains the saved per-module modes. Set
// allow_sumh=false when reporting a module without SUMH boot ownership.
struct BackendProbe {
    bool checked = false;
    bool known = false;
    bool available = false;
    int error_number = 0;
    std::string detail;
};

struct ModuleBackendDecision {
    std::string configured_mode = "auto";
    std::string global_mode;
    std::string requested = "auto";
    std::string source = "default";
    std::string selected = "none";
    std::string reason;
    bool used_fallback = false;
    bool complete = true;
    bool has_content = false;
    bool allow_sumh = true;
    bool requires_magic = false;
    std::string observation_error;
    BackendProbe sumh;
    BackendProbe overlay;
};

// Shared production decision, without controller logging. Probes are lazy;
// unchecked and unknown are distinct from known unavailable. A selected value
// with complete=false is the legacy fallback suggestion, not verified readiness.
// Lower-level ioctl logging must be scoped off by a pure diagnostic gateway.
ModuleBackendDecision resolve_module_backend_decision(const ModuleEntry& module,
                                                      const Config& config,
                                                      const ModuleModeMap& modes,
                                                      bool allow_sumh = true);

// Shared production guard: SUMH hide/none exclusions must not silently disappear
// when an explicit SUMH request falls back to an unrelated backend.
bool validate_module_backend_rules(const std::string& requested, const std::string& selected,
                                   const std::vector<ModuleRule>& rules, std::string& error);

std::string resolve_module_backend(const ModuleEntry& module, const Config& config,
                                   const ModuleModeMap& modes, bool allow_sumh = true);

namespace sumh {
enum class ModuleRebuildOutcome;
}
// Legacy whole-SUMH rebuild, admitted only for this boot's module list. It is
// not an owner-scoped transaction: global reset still affects other resources.
// A partial/unknown attempt is never rolled back or retried automatically.
bool refresh_sumh_modules(const Config& config, bool* mappings_applied = nullptr,
                          sumh::ModuleRebuildOutcome* outcome = nullptr);

// Apply the boot-only module mount plan inside the init mount namespace.
MountReport mount_all_enabled(const Config& config);

// Tear down everything SUMHP mounted (enters the init mount ns).
bool unmount_all(const Config& config);

// Bootloop protection: mount_all_enabled() bumps an unconfirmed-boot counter and
// refuses once it exceeds the limit. recovery_boot_completed() clears it once a
// boot reaches boot_completed; recovery_reset() re-enables after a trip.
// Storage failure is distinct from a successfully persisted protection trip.
// Both prevent mounting, but only the latter is a successful protected boot.
bool recovery_begin_mount(bool& disabled, std::string& error);
bool recovery_boot_completed(std::string& error);
bool recovery_reset(std::string& error);
std::string recovery_status_json(bool* ok = nullptr);

}  // namespace sumhp::mount
