// Execute the production mount orchestration with independent failures in
// module installation, feature restoration and OverlayFS attribute handling.
#include "mount/backend.hpp"
#include "mount/module_rebuild.hpp"
#include "mount/sumh.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
fs::path fixture_dir;
bool available = true;
bool configuration_ok = true;
bool discovery_ok = true;
bool backend_metadata_ok = true;
bool compile_ok = true;
bool prepare_ok = true;
bool mount_ok = true;
bool features_ok = true;
bool maps_ok = true;
bool hide_restore_ok = true;
bool xattr_ok = true;
bool record_ok = true;
bool recovery_ok = true;
bool recovery_disabled = false;
bool mappings_active = false;
bool rebuild_allowed = true;
bool active_marker_ok = true;
int reset_calls = 0;
int compile_calls = 0;
int namespace_call = 0;
int current_namespace_call = 0;
int mount_calls = 0;
int feature_calls = 0;
int maps_calls = 0;
int hide_restore_calls = 0;
int xattr_calls = 0;
int attempts = 0;
std::vector<std::string> replayable;
std::vector<sumhp::mount::ModuleEntry> modules{{"owned", "/modules/owned"},
                                               {"fallback", "/modules/fallback"}};
std::vector<std::string> installed_modules;
sumhp::mount::ModuleModeMap configured_modes;
sumhp::mount::ModuleRuleMap configured_rules;

void reset() {
    available = prepare_ok = mount_ok = features_ok = maps_ok = xattr_ok = record_ok = true;
    configuration_ok = discovery_ok = backend_metadata_ok = compile_ok = hide_restore_ok = true;
    recovery_ok = true;
    recovery_disabled = false;
    mappings_active = false;
    rebuild_allowed = active_marker_ok = true;
    reset_calls = mount_calls = feature_calls = maps_calls = hide_restore_calls = xattr_calls =
        attempts = 0;
    compile_calls = namespace_call = current_namespace_call = 0;
    replayable = {"owned"};
    installed_modules.clear();
    configured_modes.clear();
    configured_rules.clear();
}
}  // namespace

namespace sumhp {
namespace logging {
enum class Level { Debug, Info, Warning, Error };
}
fs::path runtime_config_file() {
    return fixture_dir / "config.json";
}
}  // namespace sumhp

namespace sumhp::sumh {
bool is_available() {
    return available;
}
}  // namespace sumhp::sumh

namespace sumhp::ksud {
std::string getprop(const std::string&) {
    return "1";
}
}  // namespace sumhp::ksud

namespace sumhp::mount {
namespace fs = std::filesystem;
namespace fsutil {
void mlog(const std::string&, logging::Level = logging::Level::Info) {}
template <typename Callback>
bool run_in_init_mount_ns(Callback callback) {
    current_namespace_call = ++namespace_call;
    const bool ok = callback();
    current_namespace_call = 0;
    return ok;
}
template <typename Callback>
bool run_in_init_mount_ns(Callback callback, std::string&) {
    return run_in_init_mount_ns(callback);
}
}  // namespace fsutil

bool enumerate_mountable_modules_checked(std::vector<ModuleEntry>& found, std::string& error,
                                         std::size_t) {
    if (!discovery_ok) {
        error = "injected module discovery error";
        return false;
    }
    found = modules;
    return true;
}
bool load_module_configuration(ModuleModeMap& modes, ModuleRuleMap& rules, std::string& error) {
    modes = configured_modes;
    rules = configured_rules;
    if (!configuration_ok)
        error = "injected malformed module configuration";
    return configuration_ok;
}
ModuleBackendDecision resolve_module_backend_decision(const ModuleEntry& module,
                                                      const Config& config,
                                                      const ModuleModeMap& modes, bool) {
    ModuleBackendDecision decision;
    decision.selected = module.id == "owned" && available ? "sumh" : "magic";
    const auto configured = modes.find(module.id);
    decision.requested = configured == modes.end() ? "auto" : configured->second;
    if (!config.mount_backend.empty() && config.mount_backend != "auto")
        decision.requested = config.mount_backend;
    if (!backend_metadata_ok)
        decision.observation_error = "injected backend metadata error";
    return decision;
}

namespace magic {
bool mount_modules(const std::vector<ModuleEntry>&, const Config&) {
    return true;
}
std::vector<std::string> active_mounts(const Config&) {
    return {"/system"};
}
}  // namespace magic
namespace overlay {
std::vector<std::string> active_mounts(const Config&) {
    return {"/vendor/lib"};
}
bool mount_modules(const std::vector<ModuleEntry>&, const Config&) {
    return true;
}
bool restore_xattr_hiding(const Config&) {
    ++xattr_calls;
    return xattr_ok;
}
}  // namespace overlay

namespace sumh {
bool module_rebuild_allowed(std::string& error) {
    if (!rebuild_allowed)
        error = "injected unresolved rebuild guard";
    return rebuild_allowed;
}
void invalidate_active_state() {
    mappings_active = false;
}
void clear_replayable_mappings() {
    replayable.clear();
}
std::vector<std::string> replayable_module_ids() {
    return replayable;
}
bool record_replayable_mappings(const std::vector<ModuleEntry>& selected) {
    if (!record_ok)
        return false;
    replayable.clear();
    for (const auto& module : selected)
        replayable.push_back(module.id);
    return true;
}
bool reset_runtime_state(std::string& error) {
    ++reset_calls;
    mappings_active = false;
    installed_modules.clear();
    if (!prepare_ok)
        error = "injected runtime reset failure";
    return prepare_ok;
}
bool prepare_modules(const std::vector<ModuleEntry>& selected, const Config&, const ModuleRuleMap&,
                     ModuleMountPlan& plan, std::string& error) {
    ++compile_calls;
    assert(current_namespace_call != 0);
    if (!compile_ok) {
        error = "injected invalid module tree";
        return false;
    }
    for (const auto& module : selected)
        plan.mappings.push_back({module.id, std::to_string(current_namespace_call), false});
    return true;
}
bool apply_modules(const ModuleMountPlan& plan) {
    ++mount_calls;
    // A backend may install some rules before detecting a failed rule.
    mappings_active = true;
    for (const auto& mapping : plan.mappings) {
        // A plan compiled in an earlier namespace child cannot be carried in
        // ordinary parent memory: rebuild and apply must share this callback.
        assert(mapping.source == std::to_string(current_namespace_call));
        installed_modules.push_back(mapping.path);
    }
    return mount_ok;
}
bool rebuild_modules_in_init_ns(const std::vector<ModuleEntry>& selected, const Config& config,
                                const ModuleRuleMap& rules, ModuleRebuildOutcome& outcome,
                                std::string& error) {
    outcome = ModuleRebuildOutcome::Unchanged;
    if (!module_rebuild_allowed(error))
        return false;
    return fsutil::run_in_init_mount_ns([&] {
        ModuleMountPlan plan;
        if (!prepare_modules(selected, config, rules, plan, error))
            return false;
        outcome = ModuleRebuildOutcome::PartialOrUnknown;
        if (!reset_runtime_state(error) || !apply_modules(plan))
            return false;
        outcome = ModuleRebuildOutcome::Applied;
        return active_marker_ok;
    });
}
bool apply_feature_config(const Config&, std::string& error, bool, bool, FeatureApplyReport*) {
    ++feature_calls;
    if (!features_ok)
        error = "injected base feature failure";
    return features_ok;
}
bool apply_maps_config(const Config&, std::string& error, bool restore, FeatureApplyOutcome*,
                       RestoreTrigger) {
    ++maps_calls;
    if (!restore || maps_ok)
        return true;
    error = "injected missing managed maps path";
    return false;
}
bool restore_persisted_hide_rules(std::string& error, RestoreTrigger trigger) {
    assert(trigger == RestoreTrigger::UserRequested);
    ++hide_restore_calls;
    if (!hide_restore_ok)
        error = "injected hide restore failure";
    return hide_restore_ok;
}
}  // namespace sumh

bool recovery_begin_mount(bool& disabled, std::string& error) {
    disabled = recovery_disabled;
    if (!recovery_ok) {
        error = "injected recovery storage failure";
        return false;
    }
    if (!disabled)
        ++attempts;
    return true;
}
namespace {
bool claim_mount_orchestrator() {
    return true;
}
bool sumh_usable() {
    return available;
}
#include "sumh_backend_features_under_test.inc"
}  // namespace
#include "sumh_backend_lifecycle_under_test.inc"
#include "sumh_backend_rule_guard_under_test.inc"
}  // namespace sumhp::mount

int main() {
    using sumhp::mount::mount_all_enabled;
    using sumhp::mount::refresh_sumh_modules;
    const sumhp::Config config;
    fixture_dir = fs::temp_directory_path() /
                  ("sumh-backend-" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    reset();
    recovery_ok = false;
    auto report = mount_all_enabled(config);
    assert(!report.ok && report.detail.find("recovery storage failure") != std::string::npos);
    assert(mount_calls == 0 && feature_calls == 0 && attempts == 0 && reset_calls == 1);

    reset();
    recovery_disabled = true;
    assert(mount_all_enabled(config).ok);
    assert(mount_calls == 0 && feature_calls == 0 && attempts == 0 && reset_calls == 1);

    // Invalid saved configuration must fail before touching live mappings or
    // marking this boot as mounted; replay cannot silently discard path rules.
    reset();
    configuration_ok = false;
    mappings_active = true;
    assert(!mount_all_enabled(config).ok);
    assert(mappings_active && reset_calls == 0 && mount_calls == 0 && attempts == 0);
    assert(replayable == std::vector<std::string>{"owned"});
    assert(!refresh_sumh_modules(config));
    assert(mappings_active && reset_calls == 0 && mount_calls == 0);

    reset();
    discovery_ok = false;
    mappings_active = true;
    report = mount_all_enabled(config);
    assert(!report.ok && report.detail.find("discovery error") != std::string::npos);
    assert(mappings_active && reset_calls == 0 && mount_calls == 0 && attempts == 0);
    assert(replayable == std::vector<std::string>{"owned"});
    bool discovery_applied = true;
    assert(!refresh_sumh_modules(config, &discovery_applied));
    assert(!discovery_applied && mappings_active && reset_calls == 0 && mount_calls == 0);
    assert(hide_restore_calls == 0);

    reset();
    backend_metadata_ok = false;
    mappings_active = true;
    report = mount_all_enabled(config);
    assert(!report.ok && report.detail.find("metadata error") != std::string::npos);
    assert(mappings_active && reset_calls == 0 && mount_calls == 0 && attempts == 0);
    assert(replayable == std::vector<std::string>{"owned"});

    // A well-formed configuration can still contain an unreadable or invalid
    // module tree; compilation failure must also preserve the live plan.
    reset();
    compile_ok = false;
    mappings_active = true;
    bool mappings_applied = true;
    assert(!mount_all_enabled(config).ok);
    assert(mappings_active && reset_calls == 0 && mount_calls == 0 && attempts == 0);
    assert(replayable == std::vector<std::string>{"owned"});
    assert(!refresh_sumh_modules(config, &mappings_applied));
    assert(!mappings_applied);
    assert(mappings_active && reset_calls == 0 && mount_calls == 0);
    assert(hide_restore_calls == 0);

    for (const auto* mode : {"hide", "none"}) {
        reset();
        available = false;
        configured_modes["owned"] = "sumh";
        configured_rules["owned"] = {{"/system/app/Hidden", mode}};
        assert(!mount_all_enabled(config).ok);
        assert(attempts == 0 && reset_calls == 0 && mount_calls == 0);
    }

    reset();
    maps_ok = false;
    assert(!mount_all_enabled(config).ok);
    assert(reset_calls == 1 && mount_calls == 1 && feature_calls == 1 && maps_calls == 1);
    assert(mappings_active && installed_modules == std::vector<std::string>{"owned"});
    assert(replayable == std::vector<std::string>{"owned"});

    // A post-boot missing path reports failure without undoing the rebuilt
    // module plan or preventing independent persisted hide-rule restoration.
    reset();
    maps_ok = false;
    assert(!refresh_sumh_modules(config, &mappings_applied));
    assert(mappings_applied);
    assert(reset_calls == 1 && mount_calls == 1 && feature_calls == 1 && maps_calls == 1);
    assert(mappings_active && installed_modules == std::vector<std::string>{"owned"});
    assert(hide_restore_calls == 1 && replayable == std::vector<std::string>{"owned"});
    maps_ok = true;
    assert(refresh_sumh_modules(config, &mappings_applied));
    assert(mappings_applied);
    assert(mappings_active && hide_restore_calls == 2 && feature_calls == 2 && maps_calls == 2);

    reset();
    hide_restore_ok = false;
    assert(!refresh_sumh_modules(config, &mappings_applied));
    assert(mappings_applied && mappings_active && reset_calls == 1);
    assert(hide_restore_calls == 1 && installed_modules == std::vector<std::string>{"owned"});

    // Every independent feature failure retains the committed module set and
    // does not prevent Maps or persisted hide restoration from being attempted.
    reset();
    features_ok = false;
    assert(!refresh_sumh_modules(config, &mappings_applied));
    assert(mappings_applied);
    assert(reset_calls == 1 && mappings_active && !installed_modules.empty());
    assert(feature_calls == 1 && maps_calls == 1 && hide_restore_calls == 1);
    reset();
    features_ok = false;
    assert(!mount_all_enabled(config).ok);
    assert(reset_calls == 1 && mappings_active && maps_calls == 1);

    reset();
    xattr_ok = false;
    assert(!refresh_sumh_modules(config, &mappings_applied));
    assert(mappings_applied && mappings_active && reset_calls == 1);
    assert(feature_calls == 1 && maps_calls == 1 && hide_restore_calls == 1);
    reset();
    xattr_ok = false;
    assert(!mount_all_enabled(config).ok);
    assert(reset_calls == 1 && mappings_active && feature_calls == 1 && maps_calls == 1);
    assert(replayable == std::vector<std::string>{"owned"});

    // A failed/partial rebuild stops without a second destructive reset.
    // Partial mappings are evidence, not a reason to erase the runtime again.
    for (int failure = 0; failure < 2; ++failure) {
        reset();
        prepare_ok = failure != 0;
        mount_ok = failure != 1;
        sumhp::mount::sumh::ModuleRebuildOutcome outcome;
        assert(!refresh_sumh_modules(config, nullptr, &outcome));
        assert(outcome == sumhp::mount::sumh::ModuleRebuildOutcome::PartialOrUnknown);
        assert(reset_calls == 1 && mappings_active == (failure == 1));
        assert(mount_calls == (failure == 1 ? 1 : 0));
        assert(feature_calls == 0 && hide_restore_calls == 0);
    }

    for (int failure = 0; failure < 3; ++failure) {
        reset();
        prepare_ok = failure != 0;
        mount_ok = failure != 1;
        record_ok = failure != 2;
        assert(!mount_all_enabled(config).ok);
        assert(reset_calls == 1 && mappings_active == (failure != 0));
        assert(mount_calls == (failure != 0 ? 1 : 0));
        assert(feature_calls == 0 && replayable.empty());
    }

    reset();
    rebuild_allowed = false;
    mappings_active = true;
    assert(!mount_all_enabled(config).ok);
    assert(mappings_active && reset_calls == 0 && mount_calls == 0 && attempts == 0);
    assert(replayable == std::vector<std::string>{"owned"});
    sumhp::mount::sumh::ModuleRebuildOutcome refused;
    assert(!refresh_sumh_modules(config, nullptr, &refused));
    assert(refused == sumhp::mount::sumh::ModuleRebuildOutcome::Unchanged);
    assert(mappings_active && reset_calls == 0 && mount_calls == 0);

    reset();
    active_marker_ok = false;
    assert(!refresh_sumh_modules(config, &mappings_applied, &refused));
    assert(mappings_applied && refused == sumhp::mount::sumh::ModuleRebuildOutcome::Applied);
    assert(reset_calls == 1 && mappings_active && feature_calls == 0);

    reset();
    const auto successful = mount_all_enabled(config);
    assert(successful.ok && successful.mounts == 2);
    assert(reset_calls == 1 && mappings_active && feature_calls == 1);
    assert(xattr_calls == 1);
    reset();
    replayable.clear();
    assert(!refresh_sumh_modules(config));
    assert(reset_calls == 0 && mount_calls == 0 && feature_calls == 0);
    reset();
    available = false;
    assert(!refresh_sumh_modules(config));
    assert(reset_calls == 0 && mount_calls == 0 && feature_calls == 0);

    fs::remove_all(fixture_dir);
    std::cout << "sumh_backend_lifecycle_test: all checks passed\n";
}
