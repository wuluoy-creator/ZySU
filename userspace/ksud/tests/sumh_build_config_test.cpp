// Exercise the production config parser/merge and KMI source selection without
// filesystem writes, root privileges, or a running SUMH kernel.
#include "cli_args.hpp"
#include "core/config_document.hpp"
#include "core/json.hpp"
#include "core/restore_policy.hpp"
#include "core/user_hide_input.hpp"
#include "mount/feature_result.hpp"
#include "mount/kernel_build_apply.hpp"
#include "mount/maps_temporary.hpp"
#include "mount/runtime_apply.hpp"
#include "sumhp/config.hpp"
#include "sumhp/embedded.hpp"
#include "sumhp/kernel_build.hpp"
#include "sumhp/sumh_client.hpp"
#include "uapi/sumh.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
std::optional<std::string> saved_config;
std::optional<std::string> schema_backup;
bool backup_ok = true;
bool debug_logging = false;
bool saved = false;
bool save_ok = true;
bool build_supported = true;
int supported_features = SUMH_FEATURE_MOUNT_HIDE | SUMH_FEATURE_MOUNT_HIDE_AGGRESSIVE |
                         SUMH_FEATURE_MAPS_SPOOF | SUMH_FEATURE_STATFS_SPOOF |
                         SUMH_FEATURE_MANAGED_MAPS;
int capabilities_error = 0;
bool original_query_ok = true;
int original_query_error = EIO;
const std::string original_version = "#2 SMP PREEMPT_RT Fri Aug 23 03:08:10 UTC 2024";
std::string system_build_date = "Sun Oct 4 12:30:00 UTC 2026";
bool system_build_date_present = true;
bool boot_completed = false;
int projected_reads = 0;
bool apply_ok = true;
bool replace_saved_during_build = false;
bool kernel_available = true;
bool safe_mode = false;
bool runtime_preparation_ok = true;
int runtime_preparation_calls = 0;
std::string mount_owner;
int build_apply_calls = 0;
int hide_apply_calls = 0;
bool hide_restore_ok = true;
int maps_boot_calls = 0;
int maps_restore_calls = 0;
int maps_clear_calls = 0;
bool maps_restore_ok = true;
int maps_switch_error = 0;
int maps_clear_error = 0;
int feature_reset_calls = 0;
bool replay_pending = false;
bool replay_ok = true;
int replay_calls = 0;
int xattr_restore_calls = 0;
bool xattr_restore_ok = true;
int snapshot_calls = 0;
std::vector<std::string> maps_operations;
std::vector<sumhp::MapsPathRule> restored_maps;
std::vector<bool> maps_enable_calls;
int boot_errors = 0;
int aggressive_mount_error = 0;
int normal_mount_error = 0;
std::vector<std::pair<bool, sumhp::sumh::MountHideMode>> mount_hide_calls;
sumhp::sumh::KernelBuild live_build;
bool live_debug = false;
bool live_stealth = false;
bool live_mount_hide = false;
bool live_statfs = false;
bool live_maps = false;
std::uint64_t runtime_generation = 1;
int runtime_reads = 0;
std::vector<sumhp::sumh::RuntimeFeature> runtime_writes;
int runtime_write_error = 0;
int build_query_calls = 0;
std::set<int> failed_build_queries;
std::set<int> mismatched_build_queries;
std::set<int> mismatched_build_version_queries;
std::map<std::string, std::set<int>> failed_mutations;
std::map<std::string, int> mutation_counts;
std::vector<std::string> mutations;
bool record_mutation(const std::string& name) {
    mutations.push_back(name);
    if (failed_mutations[name].count(++mutation_counts[name])) {
        errno = EIO;
        return false;
    }
    return true;
}
std::string cli_output;
}  // namespace

namespace ksud {
std::optional<std::string> getprop(const std::string& key) {
    if (key == "ro.build.date") {
        if (!system_build_date_present)
            return std::nullopt;
        return system_build_date;
    }
    assert(key == "sys.boot_completed");
    return boot_completed ? "1" : "0";
}
bool is_safe_mode() {
    return safe_mode;
}
std::optional<std::string> read_file(const std::string&) {
    errno = saved_config ? 0 : ENOENT;
    return saved_config;
}
bool write_file_durable(const std::filesystem::path& path, const std::string& content,
                        std::string& error) {
    assert(path.string() == "/mock/config.json.pre-schema-1.bak");
    if (!backup_ok) {
        error = "injected backup failure";
        return false;
    }
    schema_backup = content;
    return true;
}
}  // namespace ksud

namespace sumhp {
bool prepare_runtime(std::string& error) {
    ++runtime_preparation_calls;
    if (!runtime_preparation_ok)
        error = "injected migration failure";
    return runtime_preparation_ok;
}
namespace logging {
void set_debug_enabled(bool enabled) {
    debug_logging = enabled;
}
enum class Level { Info, Error };
void write(Level level, const std::string&, const std::string&) {
    if (level == Level::Error)
        ++boot_errors;
}
}  // namespace logging

struct ConfigFileLock {
    bool acquire(std::string&) { return true; }
};
std::filesystem::path runtime_config_file() {
    return "/mock/config.json";
}
bool save_config(const std::string& data, std::string& error) {
    if (!save_ok) {
        errno = EIO;
        error = "injected persistence failure";
        return false;
    }
    saved_config = data;
    saved = true;
    return true;
}
#include "sumh_build_config_under_test.inc"
}  // namespace sumhp

namespace sumhp::sumh {
bool runtime_state(RuntimeState& state) {
    assert(supported_features & SUMH_FEATURE_RUNTIME_STATE);
    ++runtime_reads;
    state = {};
    state.instance_id = 9;
    state.generation = runtime_generation;
    const bool flags[] = {live_debug, live_stealth, live_mount_hide, live_maps, live_statfs};
    for (std::uint32_t i = 0; i < 5; ++i)
        state.entries.push_back(
            {static_cast<RuntimeFeature>(i + 1), true, flags[i], flags[i], false, 0});
    return true;
}
bool set_runtime_feature(const RuntimeState& expected, RuntimeFeature id, bool enable,
                         std::uint32_t mode) {
    assert(supported_features & SUMH_FEATURE_RUNTIME_SET);
    assert(expected.generation == runtime_generation && expected.instance_id == 9 && mode == 0);
    runtime_writes.push_back(id);
    if (runtime_write_error) {
        errno = runtime_write_error;
        return false;
    }
    ++runtime_generation;
    switch (id) {
    case RuntimeFeature::Debug:
        live_debug = enable;
        break;
    case RuntimeFeature::Stealth:
        live_stealth = enable;
        break;
    case RuntimeFeature::MountHide:
        live_mount_hide = enable;
        break;
    case RuntimeFeature::MapsSpoof:
        live_maps = enable;
        break;
    case RuntimeFeature::StatfsSpoof:
        live_statfs = enable;
        break;
    default:
        assert(false);
    }
    return true;
}
bool kernel_build_snapshot(KernelBuildSnapshot& state, bool include_original) {
    state = {};
    if (!runtime_state(state.runtime) || !kernel_build(state.current) ||
        (include_original && !original_kernel_build(state.original)))
        return false;
    state.has_original = include_original;
    return true;
}
bool set_kernel_build_conditional(const RuntimeState& expected, const KernelBuild& desired) {
    assert(supported_features & SUMH_FEATURE_KERNEL_BUILD_CAS);
    runtime_writes.push_back(RuntimeFeature::KernelBuild);
    if (expected.generation != runtime_generation || expected.instance_id != 9) {
        errno = ESTALE;
        return false;
    }
    ++runtime_generation;
    return set_kernel_build(desired.enabled, desired.release, desired.version);
}
FeatureCapabilities feature_capabilities() {
    if (capabilities_error)
        return {false, capabilities_error, 0};
    return {true, 0, supported_features | (build_supported ? SUMH_FEATURE_KERNEL_BUILD_SPOOF : 0)};
}
bool original_kernel_build(KernelBuild& state) {
    if (!original_query_ok) {
        errno = original_query_error;
        return false;
    }
    state = {true, "6.6.1-android15-original", original_version};
    return true;
}
bool kernel_build(KernelBuild& state) {
    if (failed_build_queries.count(++build_query_calls)) {
        errno = EIO;
        return false;
    }
    state = live_build;
    if (mismatched_build_queries.count(build_query_calls))
        state.release = "unexpected-release";
    if (mismatched_build_version_queries.count(build_query_calls))
        state.version = "unexpected-version";
    return true;
}
bool set_kernel_build(bool enable, const std::string& release, const std::string& version) {
    ++build_apply_calls;
    if (replace_saved_during_build)
        saved_config =
            R"({"enable_kernel_build_spoof":true,"kernel_build_release":"external","kernel_build_version":"#9 external"})";
    KernelBuild resolved{enable, release, version};
    if (enable && (release.empty() || version.empty())) {
        KernelBuild original;
        if (!original_kernel_build(original))
            return false;
        if (release.empty())
            resolved.release = original.release;
        if (version.empty())
            resolved.version = original.version;
    }
    if (!record_mutation("kernel_build_spoof")) {
        // Model a transport failure after the kernel has committed the value.
        live_build = resolved;
        return false;
    }
    if (capabilities_error) {
        errno = capabilities_error;
        return false;
    }
    if (enable && !build_supported) {
        errno = EOPNOTSUPP;
        return false;
    }
    if (!apply_ok) {
        errno = EIO;
        return false;
    }
    live_build = resolved;
    return true;
}
bool is_available() {
    return kernel_available;
}
VersionInfo version_info() {
    return {};
}
int features() {
    return 0;
}
bool clear_maps_rules() {
    return false;
}
bool replace_managed_maps_rules(const std::vector<MapsRule>& rules) {
    assert(rules.empty());
    maps_operations.push_back("clear");
    ++maps_clear_calls;
    if (maps_clear_error) {
        errno = maps_clear_error;
        return false;
    }
    restored_maps.clear();
    return true;
}
bool add_maps_rule(unsigned long, unsigned long, unsigned long, unsigned long, const std::string&) {
    return false;
}
bool clear_rules() {
    return false;
}
bool hide_path(const std::string&) {
    return false;
}
bool delete_rule(const std::string&) {
    return false;
}
bool fix_mounts() {
    return false;
}
bool hide_overlay_xattrs(const std::string&) {
    return false;
}
bool set_mount_hide(bool enable, MountHideMode mode) {
    mount_hide_calls.emplace_back(enable, mode);
    if (!record_mutation("mount_hide"))
        return false;
    const int error =
        mode == MountHideMode::Aggressive ? aggressive_mount_error : normal_mount_error;
    if (enable && error) {
        errno = error;
        return false;
    }
    live_mount_hide = enable;
    return true;
}
bool set_maps_spoof(bool enabled) {
    maps_operations.push_back(enabled ? "enable" : "disable");
    maps_enable_calls.push_back(enabled);
    if (!record_mutation("maps_spoof"))
        return false;
    if (maps_switch_error) {
        errno = maps_switch_error;
        return false;
    }
    live_maps = enabled;
    return true;
}
bool set_statfs_spoof(bool enabled) {
    if (!record_mutation("statfs_spoof"))
        return false;
    live_statfs = enabled;
    return true;
}
bool set_debug(bool enabled) {
    if (!record_mutation("kernel_debug"))
        return false;
    live_debug = enabled;
    return true;
}
bool set_stealth(bool enabled) {
    if (!record_mutation("stealth"))
        return false;
    live_stealth = enabled;
    return true;
}
}  // namespace sumhp::sumh

namespace sumhp {
namespace mount {
bool recovery_boot_completed(std::string&) {
    return true;
}
bool refresh_sumh_modules(const Config&) {
    ++replay_calls;
    return replay_ok;
}
namespace fsutil {
template <typename Callback>
bool run_in_init_mount_ns(Callback callback) {
    return callback();
}
}  // namespace fsutil
namespace overlay {
bool restore_xattr_hiding(const Config&) {
    ++xattr_restore_calls;
    return xattr_restore_ok;
}
}  // namespace overlay
}  // namespace mount
namespace mount::sumh {
// The dedicated preview suite compiles its real read-only controller and
// embedded routing. This unrelated mutation fixture must never enter it.
std::string maps_preview_json() {
    assert(false && "unexpected Maps preview in mutation fixture");
    return {};
}
bool is_maps_preview_command(const std::vector<std::string>& args) {
    return args.size() == 3 && args[0] == "sumh" && args[1] == "maps" && args[2] == "preview";
}
void invalidate_active_state() {}
void mlog(const std::string&) {}
bool restore_managed_maps_rules(const Config& config, std::string& error,
                                ManagedMapsRestoreState* state = nullptr,
                                RestoreTrigger = RestoreTrigger::Automatic) {
    if (state)
        *state = ManagedMapsRestoreState::Unchanged;
    // Disabled orchestration now routes its empty clear through the same
    // recorded namespace operation. This fixture models the underlying clear.
    if (!config.enable_maps_spoof && config.maps_rules.empty()) {
        if (!::sumhp::sumh::replace_managed_maps_rules({})) {
            error = "injected clear failure";
            return false;
        }
        if (state)
            *state = ManagedMapsRestoreState::Replaced;
        return true;
    }
    maps_operations.push_back("restore");
    ++maps_restore_calls;
    if (!maps_restore_ok) {
        errno = ENOENT;
        error = "injected missing maps path";
        return false;
    }
    restored_maps = config.maps_rules;
    if (state)
        *state = ManagedMapsRestoreState::Replaced;
    return true;
}
bool maps_automatic_restore_allowed(std::string&) {
    return true;
}
bool apply_maps_config(const Config&, std::string&, bool restore_rules = true,
                       FeatureApplyOutcome* outcome = nullptr,
                       RestoreTrigger = RestoreTrigger::Automatic);
bool apply_feature_config(const Config&, std::string&, bool apply_immediately = false,
                          bool apply_maps = true, FeatureApplyReport* report = nullptr);
bool has_replayable_mappings() {
    return replay_pending;
}
bool is_active() {
    return false;
}
bool reset_feature_state(std::string&) {
    ++feature_reset_calls;
    return true;
}
bool restore_persisted_hide_rules(std::string& error, RestoreTrigger trigger) {
    assert(trigger == RestoreTrigger::UserRequested);
    ++hide_apply_calls;
    if (!hide_restore_ok)
        error = "injected hide restore failure";
    return hide_restore_ok;
}
#include "sumh_build_features_under_test.inc"
}  // namespace mount::sumh
std::string arg_or_default(const std::vector<std::string>& args, std::size_t index,
                           const std::string& fallback) {
    return index < args.size() ? args[index] : fallback;
}
int print_sumh_version_json() {
    return 1;
}
int print_sumh_rules_json() {
    return 1;
}
int print_features_json(int) {
    return 1;
}
bool parse_unsigned_long(const std::string&, unsigned long&) {
    return false;
}
void print_usage() {}
int print_sumh_snapshot_json() {
    ++snapshot_calls;
    return 0;
}
#include "sumh_build_cli_under_test.inc"
#include "sumh_config_apply_under_test.inc"
#include "sumh_query_under_test.inc"
std::string embedded_external_mount_owner() {
    return mount_owner;
}
int run_via_daemon(const std::vector<std::string>& args) {
    if (args == std::vector<std::string>{"sumh", "maps", "apply", "--automatic"})
        ++maps_boot_calls;
    else {
        assert((args == std::vector<std::string>{"hide", "apply", "--automatic"}));
        ++hide_apply_calls;
    }
    return 0;
}
#include "sumh_build_boot_under_test.inc"
}  // namespace sumhp

namespace ksud {
struct utsname {
    char release[65] = {};
};
int uname(utsname* uts) {
    ++projected_reads;
    std::strcpy(uts->release, "6.1.75-android14-spoofed");
    return 0;
}
std::string read_kernel_release_from_sysfs() {
    ++projected_reads;
    return "6.1.75-android14-spoofed";
}
#define LOGE(...) ((void)0)
#include "sumh_kmi_under_test.inc"
#undef LOGE
}  // namespace ksud

namespace {
void test_config_document_contract() {
    using namespace sumhp;
    const auto defaults = json::parse(default_config_json());
    std::string error;
    ConfigDocument document;
    assert(parse_config_document("{}", document, error) && document.schema_version == 0);
    assert(parse_config_document(default_config_json(), document, error) &&
           document.schema_version == CONFIG_SCHEMA_VERSION);

    // Every field is checked on both the patch and file paths, rather than
    // silently replacing a bad type with a permissive default at boot.
    for (const auto& [key, expected] : defaults.o) {
        if (key == "schema_version")
            continue;
        JsonValue patch = JsonValue::object();
        patch.o[key] = expected.is_string() ? JsonValue(false) : JsonValue("bad type");
        assert(!validate_config_patch(patch, error) && error.find(key) != std::string::npos);
        Config old;
        old.mount_source = "unchanged";
        old.debug = true;
        assert(!parse_config_json(stringify_json(patch), old, error));
        assert(old.mount_source == "unchanged" && old.debug);
    }
    const std::vector<std::string> invalid = {
        R"({"work_dir":"relative"})",
        R"({"work_dir":""})",
        R"({"mirror_dir":"relative"})",
        R"({"work_dir":"/a\u0000b"})",
        R"({"mountsource":"a\u0000b"})",
        R"({"fs_type":"wrong"})",
        R"({"mount_backend":"kasumi"})",
        R"({"mount_hide_mode":"other"})",
        R"({"mirror_img_size_mb":0})",
        R"({"mirror_img_size_mb":-1})",
        R"({"mirror_img_size_mb":1.5})",
        R"({"mirror_img_size_mb":2147483648})",
        R"({"partitions":["../system"]})",
        R"({"partitions":["vendor/etc"]})",
        R"({"partitions":["."]})",
        R"({"partitions":[".."]})",
        R"({"partitions":["vendor\u0000"]})",
        R"({"partitions":[false]})",
        R"({"partitions":["vendor\n"]})",
        R"({"partitions":["vendor "]})",
        R"({"partitions":["vendor\\etc"]})",
    };
    for (const auto& text : invalid) {
        Config old;
        old.mount_source = "unchanged";
        assert(!parse_config_json(text, old, error));
        assert(old.mount_source == "unchanged");
        assert(!validate_config_patch(json::parse(text), error));
        saved_config = default_config_json();
        const auto before = saved_config;
        saved = false;
        assert(!merge_config_json(text, error));
        assert(!saved && saved_config == before);
    }
    Config config;
    assert(parse_config_json(R"({"work_dir":"/data/local/\u4e2d ","mirror_dir":"",
        "partitions":["vendor_dlkm","my-partition"],"mirror_img_size_mb":2147483647})",
                             config, error));
    assert(config.work_dir == "/data/local/\xe4\xb8\xad " && config.mirror_dir.empty());
    assert(config.mirror_img_size_mb == INT_MAX && config.partitions.size() == 2);
    for (const auto* key : {"fs_type", "mount_backend", "mount_hide_mode"}) {
        JsonValue empty = JsonValue::object();
        empty.o[key] = "";
        assert(parse_config_json(stringify_json(empty), config, error));
        assert(config.fs_type == "auto" && config.mount_backend == "auto" &&
               config.mount_hide_mode == "normal");
        assert(!validate_config_patch(empty, error));
        empty.o["schema_version"] = 1;
        assert(!parse_config_json(stringify_json(empty), config, error));
    }
    saved_config =
        "{\n  \"fs_type\": \"\", \"mount_backend\": \"\", \"mount_hide_mode\": \"\"\n}\n";
    const auto legacy_empty = saved_config;
    schema_backup.reset();
    saved = false;
    assert(read_config_file(config, error));
    assert(!saved && !schema_backup && saved_config == legacy_empty);
    assert(merge_config_json(R"({"debug":true})", error));
    assert(schema_backup == legacy_empty);
    assert(read_config_file(config, error) && config.debug);
    const auto migrated_empty = json::parse(*saved_config);
    assert(migrated_empty.find("schema_version")->n == CONFIG_SCHEMA_VERSION);
    assert(migrated_empty.find("fs_type")->s == "auto");
    assert(migrated_empty.find("mount_backend")->s == "auto");
    assert(migrated_empty.find("mount_hide_mode")->s == "normal");
    JsonValue long_path = JsonValue::object();
    long_path.o["work_dir"] = "/" + std::string(4095, 'x');
    assert(!parse_config_json(stringify_json(long_path), config, error));

    for (const auto* bad :
         {R"({"schema_version":0})", R"({"schema_version":-1})", R"({"schema_version":1.5})",
          R"({"schema_version":"1"})", R"({"schema_version":true})", R"({"schema_version":2})",
          R"({"schema_version":1e100})"}) {
        saved_config = bad;
        const auto before = saved_config;
        config.mount_source = "unchanged";
        saved = false;
        assert(!read_config_file(config, error) && config.mount_source == "unchanged");
        assert(!merge_config_json(R"({"debug":true})", error));
        assert(!reset_kernel_build_config(error));
        assert(!write_default_config(error));
        assert(!saved && saved_config == before);
    }
    assert(!validate_config_patch(json::parse(R"({"schema_version":1})"), error));

    // Unrecognized metadata is readable but no old writer may discard or
    // re-encode it, including values outside IEEE double's exact range.
    saved_config = R"({"future":{"number":9007199254740993},"debug":true})";
    const auto future = saved_config;
    saved = false;
    assert(read_config_file(config, error) && config.debug);
    assert(!merge_config_json(R"({"debug":false})", error));
    assert(!reset_kernel_build_config(error));
    assert(!write_default_config(error));
    assert(!saved && saved_config == future);

    saved_config = "{\n  \"mountsource\": \"old\", \"debug\": true\n}\n";
    const auto original = saved_config;
    schema_backup.reset();
    saved = false;
    assert(read_config_file(config, error));
    assert(!saved && !schema_backup && saved_config == original);
    backup_ok = false;
    assert(!merge_config_json(R"({"debug":false})", error));
    assert(!saved && !schema_backup && saved_config == original);
    backup_ok = true;
    assert(merge_config_json(R"({"debug":false})", error));
    assert(schema_backup == original);
    assert(json::parse(*saved_config).find("schema_version")->n == CONFIG_SCHEMA_VERSION);
    assert(read_config_file(config, error) && config.mount_source == "old" && !config.debug);
    assert(merge_config_json(R"({"verbose":true})", error) && schema_backup == original);
    saved_config = default_config_json();
    debug_logging = false;
    save_ok = false;
    assert(!merge_config_json(R"({"debug":true})", error));
    assert(!debug_logging);
    save_ok = true;
    assert(merge_config_json(R"({"debug":true})", error) && debug_logging);
    // Parser-only validation has no runtime side effects, even on success.
    assert(parse_config_json(R"({"debug":false})", config, error) && debug_logging);
    saved_config = default_config_json();
}

void test_config() {
    using namespace sumhp;
    Config config;
    std::string error;
    assert(parse_config_json(default_config_json(), config, error));
    assert(!config.enable_kernel_build_spoof && config.kernel_build_release.empty() &&
           config.kernel_build_version.empty() &&
           config.kernel_build_apply_stage == "post-fs-data");

    JsonValue fields = JsonValue::object();
    fields.o["enable_kernel_build_spoof"] = true;
    fields.o["kernel_build_release"] = "6.1.75-android14-11-g16c5f6cd5e9b-ab12268515";
    fields.o["kernel_build_version"] = "#1 SMP PREEMPT Fri Aug 23 03:08:10 UTC 2024";
    assert(parse_config_json(stringify_json(fields), config, error));
    assert(config.enable_kernel_build_spoof &&
           config.kernel_build_release == fields.o["kernel_build_release"].s);
    fields.o["enable_kernel_build_spoof"] = false;
    assert(parse_config_json(stringify_json(fields), config, error));
    assert(!config.enable_kernel_build_spoof && !config.kernel_build_release.empty());

    std::string utf8_limit;
    for (int i = 0; i < 32; ++i)
        utf8_limit += "\xc3\xa9";
    fields.o["kernel_build_release"] = utf8_limit;
    assert(parse_config_json(stringify_json(fields), config, error));
    fields.o["kernel_build_release"] = utf8_limit + "\xc3\xa9";
    assert(!parse_config_json(stringify_json(fields), config, error));
    assert(config.kernel_build_release == utf8_limit);
    fields.o["kernel_build_release"] = "release";
    for (const auto& bad : {std::string("version\n"), std::string("version\t"),
                            std::string("a\0b", 3), std::string("a\x7f", 2)}) {
        fields.o["kernel_build_version"] = bad;
        assert(!parse_config_json(stringify_json(fields), config, error));
    }
    fields.o["kernel_build_version"] = "version";
    fields.o["enable_kernel_build_spoof"] = "true";
    assert(!parse_config_json(stringify_json(fields), config, error));

    saved_config = default_config_json();
    saved = false;
    assert(!merge_config_json(R"({"enable_kernel_build_spoof":true})", error));
    assert(!saved);
    assert(merge_config_json(
        R"({"kernel_build_release":"release","kernel_build_version":"version"})", error));
    assert(merge_config_json(R"({"enable_kernel_build_spoof":true})", error));
    saved = false;
    assert(merge_config_json(R"({"kernel_build_version":""})", error));
    const auto enabled_config = saved_config;
    saved = false;
    assert(!merge_config_json(R"({"kernel_build_release":""})", error));
    assert(!saved && saved_config == enabled_config);
    assert(!merge_config_json(R"({"kernel_build_release":"with space"})", error));
    assert(merge_config_json(R"({"kernel_build_apply_stage":"boot-completed"})", error));
    assert(!merge_config_json(R"({"kernel_build_apply_stage":"other"})", error));
    assert(merge_config_json(R"({"enable_kernel_build_spoof":false})", error));
    Config persisted;
    assert(read_config_file(persisted, error));
    assert(!persisted.enable_kernel_build_spoof && persisted.kernel_build_release == "release" &&
           persisted.kernel_build_version.empty() &&
           persisted.kernel_build_apply_stage == "boot-completed");
    // JSON config has no magic default token; only the CLI treats it specially.
    assert(merge_config_json(R"({"kernel_build_release":"default"})", error));
    assert(read_config_file(persisted, error) && persisted.kernel_build_release == "default");
}

void test_maps_config() {
    using namespace sumhp;
    std::string error;
    Config config;
    assert(parse_config_json(default_config_json(), config, error) && config.maps_rules.empty());
    const std::string valid =
        R"({"maps_rules":[{"target_path":"/target","replacement_path":"/replacement"}]})";
    assert(parse_config_json(valid, config, error) && config.maps_rules.size() == 1);
    assert(config.maps_rules[0].target_path == "/target");
    for (
        const auto* invalid :
        {R"({"enable_maps_spoof":"true"})", R"({"maps_rules":{}})", R"({"maps_rules":[1]})",
         R"({"maps_rules":[{"target_path":"/target"}]})",
         R"({"maps_rules":[{"target_path":"relative","replacement_path":"/replacement"}]})",
         R"({"maps_rules":[{"target_path":"/target","replacement_path":""}]})",
         R"({"maps_rules":[{"target_path":"/target","replacement_path":"/r","extra":1}]})",
         R"({"maps_rules":[{"target_path":"/target\u0000","replacement_path":"/r"}]})",
         R"({"maps_rules":[{"target_path":"/target","replacement_path":"/r\n"}]})",
         R"({"maps_rules":[{"target_path":"/target\r","replacement_path":"/r"}]})",
         R"({"maps_rules":[{"target_path":"/target","replacement_path":"/r"},{"target_path":"/target","replacement_path":"/r2"}]})"}) {
        assert(!parse_config_json(invalid, config, error));
        assert(config.maps_rules.size() == 1 && config.maps_rules[0].target_path == "/target");
    }
    std::vector<MapsPathRule> rules{{"/" + std::string(4094, 't'), "/" + std::string(254, 'r')}};
    assert(validate_maps_path_rules(rules, error));
    rules[0].replacement_path += "r";
    assert(!validate_maps_path_rules(rules, error));
    rules[0].replacement_path = "/r";
    rules[0].target_path += "t";
    assert(!validate_maps_path_rules(rules, error));
    rules.clear();
    for (unsigned int index = 0; index < SUMH_MANAGED_MAPS_MAX_RULES; ++index)
        rules.push_back({"/target/" + std::to_string(index), "/replacement"});
    assert(validate_maps_path_rules(rules, error));
    rules.push_back({"/one-too-many", "/replacement"});
    assert(!validate_maps_path_rules(rules, error));
    const auto encode_rules = [](const std::vector<MapsPathRule>& values) {
        JsonValue array;
        array.type = JsonValue::Type::Array;
        for (const auto& rule : values) {
            JsonValue item = JsonValue::object();
            item.o["target_path"] = rule.target_path;
            item.o["replacement_path"] = rule.replacement_path;
            array.a.push_back(std::move(item));
        }
        return array;
    };
    const auto encoded_size = [&](const std::vector<MapsPathRule>& values) {
        std::size_t size = json_quote(stringify_json(encode_rules(values))).size();
        for (const auto& rule : values) {
            size += 3 * static_cast<std::size_t>(
                            std::count(rule.target_path.begin(), rule.target_path.end(), '/') +
                            std::count(rule.replacement_path.begin(), rule.replacement_path.end(),
                                       '/'));
        }
        return size;
    };
    // Android SDK 35 gives 83 bytes; our writer additionally retains one
    // comma space for this object. The Manager budgets these spaces too.
    assert(encoded_size({{"/\xe4\xb8\xad/\"\\\t", "/plain"}}) == 84);
    // Exercise the actual double-escaped transport representation at its byte
    // boundary, using quotes/backslashes/control bytes and multibyte UTF-8.
    rules.clear();
    for (int index = 0; index < 7; ++index)
        rules.push_back({"/" + std::to_string(index) + std::string(1900, '\\'), "/r\t\"\xc3\xa9"});
    rules.push_back({"/boundary", "/r"});
    const auto base_size = encoded_size(rules);
    assert(base_size < MAPS_RULES_MAX_ENCODED_BYTES);
    const auto padding = MAPS_RULES_MAX_ENCODED_BYTES - base_size;
    assert(padding + rules.back().target_path.size() < SUMH_USER_HIDE_PATH_MAX);
    rules.back().target_path.append(padding, 'x');
    assert(encoded_size(rules) == MAPS_RULES_MAX_ENCODED_BYTES);
    assert(validate_maps_path_rules(rules, error));
    rules.back().target_path += 'x';
    assert(!validate_maps_path_rules(rules, error));
    JsonValue oversized = JsonValue::object();
    oversized.o["maps_rules"] = encode_rules(rules);
    assert(!parse_config_json(stringify_json(oversized), config, error));
    assert(config.maps_rules.size() == 1 && config.maps_rules[0].target_path == "/target");
    saved_config = default_config_json();
    saved = false;
    assert(!merge_config_json(stringify_json(oversized), error) && !saved);
    rules.clear();
    for (int index = 0; index < 16; ++index)
        rules.push_back({"/" + std::to_string(index) + std::string(3900, 'x'), "/r"});
    assert(!validate_maps_path_rules(rules, error));
    saved_config = default_config_json();
    assert(merge_config_json(valid, error));
    const auto before = saved_config;
    saved = false;
    assert(!merge_config_json(R"({"maps_rules":[{}]})", error));
    assert(!saved && saved_config == before);
    assert(merge_config_json(R"({"debug":true})", error));
    assert(read_config_file(config, error) && config.maps_rules.size() == 1);
    assert(parse_config_json("{}", config, error) && config.maps_rules.empty());
    saved_config = default_config_json();
}

void test_kmi_source() {
    assert(ksud::read_kernel_release() == "6.6.1-android15-original");
    assert(projected_reads == 0);
    original_query_ok = false;
    assert(ksud::read_kernel_release().empty());
    assert(projected_reads == 0);
    build_supported = false;
    assert(ksud::read_kernel_release() == "6.1.75-android14-spoofed");
    assert(projected_reads == 1);
    build_supported = true;
    capabilities_error = EIO;
    assert(ksud::read_kernel_release().empty() && projected_reads == 1);
    original_query_ok = true;
    assert(ksud::read_kernel_release() == "6.6.1-android15-original" && projected_reads == 1);
    original_query_ok = false;
    for (const int error : {EIO, EACCES, EPERM, EPROTO, EINVAL, EINTR}) {
        capabilities_error = error;
        original_query_error = ENOTTY;
        assert(ksud::read_kernel_release().empty() && projected_reads == 1);
        capabilities_error = ENOTTY;
        original_query_error = error;
        assert(ksud::read_kernel_release().empty() && projected_reads == 1);
    }
    for (const int error : {ENOTTY, EOPNOTSUPP, ENOSYS, ENODEV}) {
        capabilities_error = original_query_error = error;
        assert(ksud::read_kernel_release() == "6.1.75-android14-spoofed");
    }
    assert(projected_reads == 5);
    capabilities_error = 0;
    original_query_ok = true;
}

int run_cli(const std::vector<std::string>& args) {
    std::ostringstream output;
    std::ostringstream errors;
    auto* previous_output = std::cout.rdbuf(output.rdbuf());
    auto* previous_errors = std::cerr.rdbuf(errors.rdbuf());
    std::vector<std::string> input = {"sumhp"};
    input.insert(input.end(), args.begin(), args.end());
    ksud::CliArguments parsed;
    const int parse_result = ksud::parse_cli(input, parsed);
    const int result = parse_result == -1 ? sumhp::handle_sumh(parsed.args) : parse_result;
    std::cout.rdbuf(previous_output);
    std::cerr.rdbuf(previous_errors);
    cli_output = output.str();
    return result;
}

void test_cli() {
    using namespace sumhp;
    saved_config = default_config_json();
    saved = false;
    build_supported = true;
    std::string error;
    assert(run_cli({"sumh", "kernel-build", "set", "6.1.75-android14-example", "#1 test"}) == 0);
    assert(live_build.enabled && live_build.release == "6.1.75-android14-example");
    Config config;
    assert(read_config_file(config, error));
    assert(config.enable_kernel_build_spoof && config.kernel_build_version == "#1 test");

    assert(run_cli({"sumh", "kernel-build", "show"}) == 0);
    const auto snapshot = json::parse(cli_output);
    assert(snapshot.find("enabled")->b && snapshot.find("release")->s == live_build.release);
    assert(snapshot.find("original_release")->s == "6.6.1-android15-original");
    assert(snapshot.find("original_version")->s == original_version);
    assert(snapshot.find("suggested_version")->s ==
           "#2 SMP PREEMPT_RT Sun Oct 4 12:30:00 UTC 2026");
    assert(snapshot.find("suggestion_source")->s == "ro.build.date");
    system_build_date_present = false;
    assert(run_cli({"sumh", "kernel-build", "show"}) == 0);
    const auto without_date = json::parse(cli_output);
    assert(without_date.find("suggested_version")->s.empty() &&
           without_date.find("suggestion_source")->s.empty());
    system_build_date_present = true;
    original_query_ok = false;
    assert(run_cli({"sumh", "kernel-build", "show"}) != 0);
    original_query_ok = true;
    const auto previous_config = saved_config;
    apply_ok = false;
    const auto before_failed_apply_calls = build_apply_calls;
    assert(run_cli({"sumh", "kernel-build", "set", "different", "#2 different"}) != 0);
    assert(saved_config != previous_config && live_build.release == "6.1.75-android14-example");
    assert(read_config_file(config, error) && config.kernel_build_release == "different");
    const auto failed_apply = json::parse(cli_output);
    assert(failed_apply.find("saved")->b && !failed_apply.find("applied")->b &&
           failed_apply.find("runtime_result")->s == "failed");
    assert(build_apply_calls == before_failed_apply_calls + 1);  // No replay or blind rollback.
    apply_ok = true;

    saved = false;
    assert(run_cli({"sumh", "kernel-build", "set", "missing-version"}) != 0);
    assert(run_cli({"sumh", "kernel-build", "set", "", "#1 test"}) != 0);
    assert(run_cli({"sumh", "kernel-build", "set", std::string(65, 'x'), "#1 test"}) != 0);
    assert(run_cli({"sumh", "kernel-build", "set", "release", "#1\ntest"}) != 0);
    assert(!saved);
    assert(run_cli({"sumh", "kernel-build", "reset"}) == 0);
    assert(!live_build.enabled);
    assert(read_config_file(config, error));
    assert(!config.enable_kernel_build_spoof && config.kernel_build_release == "different");
    assert(run_cli({"sumh", "kernel-build", "set", "default", "#4 new build"}) == 0);
    assert(read_config_file(config, error) && config.kernel_build_release.empty() &&
           config.kernel_build_version == "#4 new build");
    assert(run_cli({"sumh", "kernel-build", "set", "new-release", "default"}) == 0);
    assert(read_config_file(config, error) && config.kernel_build_release == "new-release" &&
           config.kernel_build_version.empty());
    const auto before_defaults = saved_config;
    assert(run_cli({"sumh", "kernel-build", "set", "default", "default"}) != 0);
    assert(saved_config == before_defaults);

    saved_config =
        R"({"mountsource":"MY-KSU","future-setting":42,"enable_kernel_build_spoof":"bad","kernel_build_release":"bad release","kernel_build_version":"#7 saved","kernel_build_apply_stage":"wrong"})";
    const auto with_unknown_field = saved_config;
    assert(run_cli({"sumh", "kernel-build", "reset"}) != 0);
    assert(saved_config == with_unknown_field && !live_build.enabled);
    saved_config =
        R"({"mountsource":"MY-KSU","enable_kernel_build_spoof":"bad","kernel_build_release":"bad release","kernel_build_version":"#7 saved","kernel_build_apply_stage":"wrong"})";
    assert(run_cli({"sumh", "kernel-build", "reset"}) == 0);
    assert(read_config_file(config, error) && !config.enable_kernel_build_spoof &&
           config.kernel_build_release.empty() && config.kernel_build_version == "#7 saved" &&
           config.kernel_build_apply_stage == "post-fs-data" && config.mount_source == "MY-KSU");
    saved_config =
        R"({"mountsource":"MY-KSU","enable_kernel_build_spoof":true,"kernel_build_release":"invalid release"})";
    capabilities_error = EIO;
    saved = false;
    assert(run_cli({"sumh", "kernel-build", "reset"}) != 0);
    assert(saved && read_config_file(config, error) && !config.enable_kernel_build_spoof &&
           config.mount_source == "MY-KSU" && config.kernel_build_release.empty());
    capabilities_error = 0;
    // An invalid JSON document cannot be repaired without losing settings;
    // reset still restores the live identity and reports persistence failure.
    saved_config = "not json";
    assert(run_cli({"sumh", "kernel-build", "reset"}) != 0 && !live_build.enabled);
    assert(*saved_config == "not json");
    saved_config = default_config_json();

    build_supported = false;
    saved = false;
    assert(run_cli({"sumh", "kernel-build", "set", "release", "version"}) != 0);
    assert(!saved);
    assert(run_cli({"sumh", "kernel-build", "reset"}) == 0);
}

void test_build_cli_conditional_outcomes() {
    using namespace sumhp;
    ksud::CliArguments parsed;
    assert(ksud::parse_cli({"sumhp", "api", "state"}, parsed) == -1);
    assert((parsed.args == std::vector<std::string>{"api", "state"}));
    assert(ksud::parse_cli({"sumhp", "api", "state", "unexpected"}, parsed) != -1);
    assert(ksud::parse_cli({"sumhp", "api", "inventory"}, parsed) == -1);
    assert(ksud::parse_cli({"sumhp", "api", "inventory", "maps_managed"}, parsed) == -1);
    assert(
        ksud::parse_cli({"sumhp", "api", "inventory", "mapping", "18446744073709551615", "1", "0"},
                        parsed) == -1);
    assert(is_query(parsed.args));
    for (const auto& suffix :
         std::vector<std::vector<std::string>>{{"mapping", "1"},
                                               {"mapping", "1", "2"},
                                               {"unknown"},
                                               {"mapping", "1", "0", "0"},
                                               {"metadata", "1", "2", "1"},
                                               {"mapping", "1", "2", "18446744073709551616"}}) {
        std::vector<std::string> request{"sumhp", "api", "inventory"};
        request.insert(request.end(), suffix.begin(), suffix.end());
        assert(ksud::parse_cli(request, parsed) != -1);
    }
    const auto old_caps = supported_features;
    const auto old_build_support = build_supported;
    build_supported = true;
    supported_features |= SUMH_FEATURE_RUNTIME_STATE | SUMH_FEATURE_KERNEL_BUILD_CAS;
    saved_config = default_config_json();
    runtime_writes.clear();
    assert(run_cli({"sumh", "kernel-build", "set", "6.1.cli-cas", "#5 CAS"}) == 0);
    assert(
        (runtime_writes == std::vector<sumh::RuntimeFeature>{sumh::RuntimeFeature::KernelBuild}));
    assert(json::parse(cli_output).find("runtime_result")->s == "applied");

    // Failed acknowledgement after commit, concurrent config replacement:
    // the command must not replay the ioctl or overwrite the newer saved intent.
    replace_saved_during_build = true;
    failed_mutations["kernel_build_spoof"].insert(mutation_counts["kernel_build_spoof"] + 1);
    const auto calls = build_apply_calls;
    assert(run_cli({"sumh", "kernel-build", "set", "6.1.unknown", "#6 unknown"}) != 0);
    assert(build_apply_calls == calls + 1 && live_build.release == "6.1.unknown");
    assert(json::parse(cli_output).find("runtime_result")->s == "outcome_unknown");
    Config config;
    std::string error;
    assert(read_config_file(config, error) && config.kernel_build_release == "external");
    replace_saved_during_build = false;
    failed_mutations.clear();
    assert(run_cli({"sumh", "kernel-build", "reset"}) == 0);
    const auto reset = json::parse(cli_output);
    assert(reset.find("saved")->b && reset.find("applied")->b && !live_build.enabled);
    assert(live_build.release == "6.6.1-android15-original" &&
           live_build.version == original_version);
    supported_features = old_caps;
    build_supported = old_build_support;
}

void test_module_configure_cli() {
    const std::string rules = R"([{"path":"/system/app/Example ","mode":"hide"}])";
    ksud::CliArguments parsed;
    assert(ksud::parse_cli({"sumhp", "module", "configure", "example.module", "sumh", rules},
                           parsed) == -1);
    assert((parsed.args ==
            std::vector<std::string>{"module", "configure", "example.module", "sumh", rules}));
    assert(ksud::parse_cli(
               {"sumhp", "daemon", "call", "module", "configure", "example.module", "auto", "[]"},
               parsed) == -1);
    assert(ksud::parse_cli({"sumhp", "module", "configure", "example.module", "hide", "[]"},
                           parsed) != -1);
    assert(ksud::parse_cli({"sumhp", "module", "configure", "../example", "sumh", "[]"}, parsed) !=
           -1);
    for (const auto* invalid : {"{}", "null", "[", "[] trailing"})
        assert(ksud::parse_cli({"sumhp", "module", "configure", "example.module", "sumh", invalid},
                               parsed) != -1);
    assert(ksud::parse_cli({"sumhp", "module", "configure", "example.module", "sumh"}, parsed) !=
           -1);
}

void test_maps_cli_and_apply() {
    using namespace sumhp;
    saved_config = default_config_json();
    maps_restore_calls = 0;
    maps_restore_ok = true;
    maps_enable_calls.clear();
    assert(run_cli({"sumh", "maps", "add-path", "/target", "/replacement"}) == 0);
    assert(maps_restore_calls == 1 && restored_maps.size() == 1);
    assert(run_cli({"sumh", "maps", "add-path", "/target", "/changed"}) == 0);
    assert(restored_maps.size() == 1 && restored_maps[0].replacement_path == "/changed");
    assert(run_cli({"sumh", "maps", "list-paths"}) == 0);
    assert(json::parse(cli_output).a[0].find("replacement_path")->s == "/changed");
    assert(maps_restore_calls == 2);
    assert(maps_enable_calls.empty());  // Editing rules does not change the feature switch.
    std::string error;
    assert(merge_config_json(R"({"enable_maps_spoof":true})", error));
    assert(run_cli({"sumh", "maps", "apply"}) == 0 && maps_restore_calls == 3);
    assert(maps_enable_calls.size() == 1 && maps_enable_calls.back());
    maps_restore_ok = false;
    assert(run_cli({"sumh", "maps", "apply"}) != 0);
    assert(maps_enable_calls.size() == 1);  // Failed resolution cannot enable stale rules.
    --maps_restore_calls;
    maps_restore_ok = true;
    const auto before = saved_config;
    save_ok = false;
    assert(run_cli({"sumh", "maps", "add-path", "/second", "/r2"}) != 0);
    assert(saved_config == before && maps_restore_calls == 3);
    save_ok = true;
    maps_restore_ok = false;
    assert(run_cli({"sumh", "maps", "add-path", "/second", "/r2"}) != 0);
    assert(errno == ENOENT && maps_restore_calls == 4);
    assert(json::parse(cli_output).find("saved")->b);
    assert(!json::parse(cli_output).find("applied")->b);
    Config config;
    assert(read_config_file(config, error) && config.maps_rules.size() == 2);
    maps_restore_ok = true;
    assert(run_cli({"sumh", "maps", "remove-path", "/target"}) == 0);
    assert(restored_maps.size() == 1 && restored_maps[0].target_path == "/second");
    const auto persisted = saved_config;
    const int calls_before = maps_restore_calls;
    assert(run_cli({"sumh", "maps", "add-path", "relative", "/r"}) != 0);
    assert(run_cli({"sumh", "maps", "apply", "unexpected"}) != 0);
    assert(saved_config == persisted && maps_restore_calls == calls_before);
    assert(run_cli({"sumh", "maps", "remove-path", "/second"}) == 0);
    assert(restored_maps.empty());
    assert(merge_config_json(R"({"enable_maps_spoof":false})", error));
    maps_restore_ok = false;
    const int before_disabled_apply = maps_restore_calls;
    const int before_disabled_clear = maps_clear_calls;
    assert(run_cli({"sumh", "maps", "apply"}) == 0);
    assert(maps_restore_calls == before_disabled_apply &&
           maps_clear_calls == before_disabled_clear + 1);
    assert(!maps_enable_calls.back());
    maps_restore_ok = true;
    saved_config.reset();
    const int before_missing_config = maps_restore_calls;
    assert(run_cli({"sumh", "maps", "apply"}) != 0);
    assert(maps_restore_calls == before_missing_config);

    config = Config{};
    config.enable_maps_spoof = true;
    config.maps_rules = {{"/missing", "/replacement"}};
    boot_completed = false;
    maps_restore_ok = false;
    maps_enable_calls.clear();
    maps_restore_calls = 0;
    assert(mount::sumh::apply_feature_config(config, error, false));
    assert(maps_restore_calls == 0);
    assert(!mount::sumh::apply_feature_config(config, error, true));
    assert(maps_restore_calls == 1 && maps_enable_calls.back());
    assert(std::find(maps_enable_calls.begin(), maps_enable_calls.end(), false) ==
           maps_enable_calls.end());
    config.enable_maps_spoof = false;
    const int clear_calls_before = maps_clear_calls;
    assert(mount::sumh::apply_feature_config(config, error, true));
    assert(maps_restore_calls == 1 && !maps_enable_calls.back());
    assert(maps_clear_calls == clear_calls_before + 1 && restored_maps.empty());
    maps_restore_ok = true;
    saved_config = default_config_json();
}

void test_automatic_maps_request() {
    using namespace sumhp;
    saved_config = R"({"enable_maps_spoof":true})";
    maps_restore_ok = kernel_available = true;
    boot_completed = true;
    safe_mode = true;
    const auto before = maps_restore_calls;
    // Lifecycle requests enter the daemon directly, without the public CLI parser.
    assert(handle_sumh({"sumh", "maps", "apply", "--automatic"}) == 0);
    assert(maps_restore_calls == before);
    safe_mode = false;
    boot_completed = false;
    assert(handle_sumh({"sumh", "maps", "apply", "--automatic"}) == 0);
    assert(maps_restore_calls == before);
    boot_completed = true;
    kernel_available = false;
    assert(handle_sumh({"sumh", "maps", "apply", "--automatic"}) == 0);
    assert(maps_restore_calls == before);
    kernel_available = true;
    mount_owner = "external.metamodule";
    assert(handle_sumh({"sumh", "maps", "apply", "--automatic"}) == 0);
    assert(maps_restore_calls == before + 1);
    assert(handle_sumh({"sumh", "maps", "apply", "--automatic", "extra"}) != 0);
    assert(maps_restore_calls == before + 1);
    mount_owner.clear();
    boot_completed = false;
}

void test_boot_restore() {
    using namespace sumhp;
    mount_owner = "external.metamodule";
    build_supported = kernel_available = apply_ok = true;
    safe_mode = false;
    saved_config =
        R"({"enable_kernel_build_spoof":true,"kernel_build_release":"release","kernel_build_version":"#1 version"})";
    const auto persisted = saved_config;
    build_apply_calls = hide_apply_calls = boot_errors = 0;
    runtime_preparation_calls = 0;
    saved = false;
    // The post-fs-data hook restores identity before Zygote, including when
    // an external backend will handle mounting. It does not restore mounts.
    embedded_restore_kernel_build();
    assert(build_apply_calls == 1 && live_build.enabled && live_build.release == "release");
    assert(hide_apply_calls == 0 && boot_errors == 0 && !saved && saved_config == persisted);
    assert(runtime_preparation_calls == 1);
    runtime_preparation_ok = false;
    embedded_restore_kernel_build();
    assert(build_apply_calls == 1 && boot_errors == 1 && saved_config == persisted);
    runtime_preparation_ok = true;
    boot_errors = 0;

    std::string error;
    assert(merge_config_json(R"({"kernel_build_apply_stage":"boot-completed"})", error));
    Config late;
    assert(read_config_file(late, error));
    build_apply_calls = 0;
    embedded_restore_kernel_build();
    assert(build_apply_calls == 0);
    // Module mount orchestration must not bypass the selected late stage.
    assert(mount::sumh::apply_feature_config(late, error, false));
    assert(build_apply_calls == 0);
    // An explicit runtime apply ignores scheduling, even before boot complete.
    assert(mount::sumh::apply_feature_config(late, error, true));
    assert(build_apply_calls == 1);
    embedded_boot_completed();
    assert(build_apply_calls == 2);
    assert(maps_boot_calls == 1);  // External mount owners still restore saved maps.
    boot_completed = true;
    assert(mount::sumh::apply_feature_config(late, error, false));
    assert(build_apply_calls == 3);
    boot_completed = false;
    late.kernel_build_apply_stage = "post-fs-data";
    assert(mount::sumh::apply_feature_config(late, error, false));
    assert(build_apply_calls == 4);
    late.kernel_build_apply_stage = "boot-completed";
    late.enable_kernel_build_spoof = false;
    assert(mount::sumh::apply_feature_config(late, error, false));
    assert(build_apply_calls == 5 && !live_build.enabled);
    saved_config = persisted;
    saved = false;

    build_apply_calls = 0;
    safe_mode = true;
    const int maps_before_safe_mode = maps_boot_calls;
    embedded_boot_completed();
    assert(build_apply_calls == 0 && hide_apply_calls == 0);
    assert(maps_boot_calls == maps_before_safe_mode);
    safe_mode = false;
    kernel_available = false;
    embedded_boot_completed();
    assert(build_apply_calls == 0);
    kernel_available = true;
    saved_config.reset();
    embedded_boot_completed();
    assert(build_apply_calls == 0 && boot_errors == 0 && !saved_config);
    saved_config = R"({"enable_kernel_build_spoof":true})";
    embedded_boot_completed();
    assert(build_apply_calls == 0 && boot_errors == 1);

    saved_config = persisted;
    apply_ok = false;
    embedded_boot_completed();
    assert(build_apply_calls == 1 && boot_errors == 2 && saved_config == persisted);
    apply_ok = true;
    saved_config = default_config_json();
    build_supported = false;
    embedded_boot_completed();
    assert(!live_build.enabled && boot_errors == 2);

    // The independent feature restore keeps the existing built-in hide flow.
    mount_owner.clear();
    embedded_boot_completed();
    assert(hide_apply_calls == 1 && !saved);
}

void test_maps_lifecycle_and_config_wrapper() {
    using namespace sumhp;
    Config config;
    config.enable_maps_spoof = true;
    config.maps_rules = {{"/target", "/replacement"}};
    config.enable_kernel_build_spoof = true;
    config.kernel_build_release = "release";
    build_supported = apply_ok = kernel_available = true;
    maps_restore_ok = true;
    maps_switch_error = maps_clear_error = 0;
    std::string error;
    maps_operations.clear();
    assert(mount::sumh::apply_maps_config(config, error));
    assert((maps_operations == std::vector<std::string>{"restore", "enable"}));
    maps_operations.clear();
    maps_restore_ok = false;
    assert(!mount::sumh::apply_maps_config(config, error));
    assert(errno == ENOENT && error == "injected missing maps path");
    assert((maps_operations == std::vector<std::string>{"restore"}));
    maps_operations.clear();
    maps_restore_ok = true;
    maps_switch_error = EACCES;
    assert(!mount::sumh::apply_maps_config(config, error));
    assert(errno == EACCES && error.find("enable") != std::string::npos);
    assert((maps_operations == std::vector<std::string>{"restore", "enable"}));
    config.enable_maps_spoof = false;
    maps_operations.clear();
    assert(!mount::sumh::apply_maps_config(config, error));
    assert(errno == EACCES && error.find("disable") != std::string::npos);
    assert((maps_operations == std::vector<std::string>{"disable"}));
    maps_switch_error = 0;
    maps_clear_error = EIO;
    maps_operations.clear();
    assert(!mount::sumh::apply_maps_config(config, error));
    assert(errno == EIO && error.find("clear") != std::string::npos);
    assert((maps_operations == std::vector<std::string>{"disable", "clear"}));
    maps_clear_error = 0;
    maps_restore_ok = false;
    maps_operations.clear();
    assert(mount::sumh::apply_maps_config(config, error));
    assert((maps_operations == std::vector<std::string>{"disable", "clear"}));
    maps_operations.clear();
    config.enable_maps_spoof = true;
    assert(mount::sumh::apply_maps_config(config, error, false));
    assert((maps_operations == std::vector<std::string>{"enable"}));

    // Module orchestration applies Maps exactly once after the base features.
    maps_operations.clear();
    assert(mount::sumh::apply_feature_config(config, error, true, false));
    assert(maps_operations.empty());

    // Exercise the production config apply wrapper, not only the feature
    // helper: its old outer cleanup turned a missing maps path into a global reset.
    saved_config =
        R"({"enable_maps_spoof":true,"enable_mount_hide":true,"enable_kernel_build_spoof":true,"kernel_build_release":"release","maps_rules":[{"target_path":"/target","replacement_path":"/replacement"}]})";
    feature_reset_calls = snapshot_calls = xattr_restore_calls = 0;
    replay_pending = false;
    mount_hide_calls.clear();
    maps_operations.clear();
    assert(apply_config_file() != 0);
    assert(feature_reset_calls == 0 && snapshot_calls == 0 && xattr_restore_calls == 1);
    assert(live_build.enabled && mount_hide_calls.size() == 1 && mount_hide_calls[0].first);
    assert((maps_operations == std::vector<std::string>{"restore"}));
    maps_restore_ok = true;
    maps_operations.clear();
    assert(apply_config_file() == 0);
    assert(snapshot_calls == 1 && xattr_restore_calls == 2 && feature_reset_calls == 0);
    assert((maps_operations == std::vector<std::string>{"restore", "enable"}));
    hide_restore_ok = false;
    assert(apply_config_file() != 0);
    assert(snapshot_calls == 1 && feature_reset_calls == 0);
    hide_restore_ok = true;
    xattr_restore_ok = false;
    const int before_xattr_failure = hide_apply_calls;
    assert(apply_config_file() != 0);
    assert(feature_reset_calls == 0 && hide_apply_calls == before_xattr_failure + 1);
    assert(live_build.enabled && live_mount_hide && live_maps);
    xattr_restore_ok = true;
    replay_pending = true;
    replay_ok = false;
    replay_calls = 0;
    assert(apply_config_file() != 0 && replay_calls == 1);
    assert(feature_reset_calls == 0);
    replay_ok = true;
    assert(apply_config_file() == 0 && replay_calls == 2);
    replay_pending = false;
    saved_config.reset();
    const auto before_missing = maps_operations.size();
    assert(apply_config_file() != 0 && maps_operations.size() == before_missing);
    assert(is_query({"sumh", "maps", "list-paths"}));
    assert(is_query({"module", "plan"}));
    assert(is_query({"module", "explain", "/system/etc/example"}));
    assert(!is_query({"sumh", "maps", "apply"}));
    assert(!is_query({"sumh", "maps", "add-path", "/target", "/replacement"}));
    saved_config = default_config_json();
}

void test_retired_mount_mode_restore() {
    using sumhp::sumh::MountHideMode;
    sumhp::Config config;
    config.enable_mount_hide = true;
    config.mount_hide_mode = "aggressive";
    std::string error;
    mount_hide_calls.clear();
    aggressive_mount_error = EOPNOTSUPP;
    assert(sumhp::mount::sumh::apply_feature_config(config, error, false));
    assert(error.empty() && mount_hide_calls.size() == 2);
    assert(mount_hide_calls[0].first && mount_hide_calls[0].second == MountHideMode::Aggressive);
    assert(mount_hide_calls[1].first && mount_hide_calls[1].second == MountHideMode::Normal);

    // Transport errors are not evidence that the requested mode was retired.
    mount_hide_calls.clear();
    aggressive_mount_error = EIO;
    assert(!sumhp::mount::sumh::apply_feature_config(config, error, false));
    assert(!error.empty() && mount_hide_calls.size() == 1);
    assert(error.find("outcome_unknown") != std::string::npos);
    assert(mount_hide_calls.back().first);

    mount_hide_calls.clear();
    error.clear();
    aggressive_mount_error = EOPNOTSUPP;
    normal_mount_error = EIO;
    assert(!sumhp::mount::sumh::apply_feature_config(config, error, false));
    assert(!error.empty() && mount_hide_calls.size() == 2);
    assert(mount_hide_calls.back().first);

    mount_hide_calls.clear();
    error.clear();
    aggressive_mount_error = normal_mount_error = 0;
    assert(sumhp::mount::sumh::apply_feature_config(config, error, false));
    assert(error.empty() && mount_hide_calls.size() == 1);
    assert(mount_hide_calls[0].second == MountHideMode::Aggressive);
    mount_hide_calls.clear();
}

void test_feature_failure_isolation() {
    using namespace sumhp;
    using namespace sumhp::mount::sumh;
    const int all_supported = supported_features;
    Config config;
    config.enable_kernel_debug = config.enable_stealth = config.enable_mount_hide = true;
    config.enable_statfs_spoof = config.enable_maps_spoof = config.enable_kernel_build_spoof = true;
    config.kernel_build_release = "new-release";
    config.kernel_build_version = "new-version";
    std::string error;
    FeatureApplyReport report;
    const auto reset_faults = [&]() {
        failed_mutations.clear();
        mutation_counts.clear();
        mutations.clear();
        failed_build_queries.clear();
        mismatched_build_queries.clear();
        mismatched_build_version_queries.clear();
        build_query_calls = 0;
        capabilities_error = maps_switch_error = maps_clear_error = 0;
        supported_features = all_supported;
        build_supported = apply_ok = maps_restore_ok = true;
        original_query_ok = true;
        boot_completed = false;
        live_debug = live_stealth = live_mount_hide = live_statfs = live_maps = false;
        live_build = {true, "old-release", "old-version"};
    };
    const auto find = [&](const char* feature) -> const FeatureApplyOutcome& {
        const auto it = std::find_if(report.outcomes.begin(), report.outcomes.end(),
                                     [&](const auto& item) { return item.feature == feature; });
        assert(it != report.outcomes.end());
        return *it;
    };
    reset_faults();
    capabilities_error = EIO;
    assert(!apply_feature_config(config, error, true, true, &report));
    assert(mutations.empty() && build_query_calls == 0 && errno == EIO);
    assert(error.find("no feature mutations") != std::string::npos);

    // Preflight rejects unsupported enables individually, without poisoning
    // supported features or replacing the managed Maps set.
    reset_faults();
    supported_features = 0;
    build_supported = false;
    const int before_maps = maps_restore_calls;
    assert(!apply_feature_config(config, error, true, true, &report));
    assert((mutations == std::vector<std::string>{"kernel_debug", "stealth"}));
    assert(maps_restore_calls == before_maps && live_debug && live_stealth);
    for (const auto* name : {"mount_hide", "statfs_spoof", "kernel_build_spoof", "maps_spoof"})
        assert(find(name).state == FeatureApplyState::Unsupported);

    // Fail each setter after the preflight. Every other feature is still
    // attempted; there is no blanket disable. A legacy Kernel Build transport
    // failure cannot prove ownership and must never trigger a second write.
    for (const auto* name : {"kernel_debug", "stealth", "mount_hide", "statfs_spoof",
                             "kernel_build_spoof", "maps_spoof"}) {
        reset_faults();
        failed_mutations[name] = {1};
        assert(!apply_feature_config(config, error, false, true, &report));
        assert(!report.ok() && report.outcomes.size() == 6);
        const auto& failure = find(name);
        assert(failure.error_number == EIO);
        assert(failure.state == FeatureApplyState::Unknown);
        for (const auto& outcome : report.outcomes)
            if (outcome.feature != name)
                assert(outcome.state == FeatureApplyState::Applied);
        assert(live_debug == (std::string(name) != "kernel_debug"));
        assert(live_stealth == (std::string(name) != "stealth"));
        assert(live_mount_hide == (std::string(name) != "mount_hide"));
        assert(live_statfs == (std::string(name) != "statfs_spoof"));
        assert(live_maps == (std::string(name) != "maps_spoof"));
        assert(live_build.release == "new-release");
        assert(mutations.size() == 6);
    }
    reset_faults();
    failed_build_queries = {1};
    assert(!apply_feature_config(config, error, true, true, &report));
    assert(find("kernel_build_spoof").state == FeatureApplyState::Failed);
    assert(mutation_counts["kernel_build_spoof"] == 0);
    assert(live_build.release == "old-release" && live_mount_hide && live_maps);

    // A successful setter is not sufficient when structured readback fails
    // or disagrees; legacy kernels cannot prove ownership for restoration.
    for (bool mismatch : {false, true}) {
        reset_faults();
        if (mismatch)
            mismatched_build_queries = {2};
        else
            failed_build_queries = {2};
        assert(!apply_feature_config(config, error, true, true, &report));
        assert(find("kernel_build_spoof").state == FeatureApplyState::Unknown);
        assert(live_build.release == "new-release" && build_query_calls == 2);
        assert(mutation_counts["kernel_build_spoof"] == 1);
        assert(live_debug && live_stealth && live_mount_hide && live_statfs && live_maps);
    }

    // Empty fields restore the original string, so both fields must be checked.
    // Exercise each single-field override and a mismatch in its implicit field.
    for (bool empty_release : {false, true}) {
        Config single = config;
        if (empty_release)
            single.kernel_build_release.clear();
        else
            single.kernel_build_version.clear();
        reset_faults();
        assert(apply_feature_config(single, error, true, true, &report));
        assert(live_build.release == (empty_release ? "6.6.1-android15-original" : "new-release"));
        assert(live_build.version == (empty_release ? "new-version" : original_version));
        reset_faults();
        if (empty_release)
            mismatched_build_queries = {2};
        else
            mismatched_build_version_queries = {2};
        assert(!apply_feature_config(single, error, true, true, &report));
        assert(find("kernel_build_spoof").state == FeatureApplyState::Unknown);
        assert(mutation_counts["kernel_build_spoof"] == 1);
        reset_faults();
        original_query_ok = false;
        assert(!apply_feature_config(single, error, true, true, &report));
        assert(find("kernel_build_spoof").state == FeatureApplyState::Failed);
        assert(mutation_counts["kernel_build_spoof"] == 0);
        assert(live_build.release == "old-release" && live_maps);
    }

    // Queued faults for a hypothetical second mutation must remain unused:
    // legacy failure performs only readback and never unconditionally reverts.
    for (bool fail_readback : {false, true}) {
        reset_faults();
        failed_mutations["kernel_build_spoof"] =
            fail_readback ? std::set<int>{1} : std::set<int>{1, 2};
        if (fail_readback)
            failed_build_queries = {2};
        assert(!apply_feature_config(config, error, true, true, &report));
        assert(find("kernel_build_spoof").state == FeatureApplyState::Unknown);
        assert(mutation_counts["kernel_build_spoof"] == 1);
        assert(live_debug && live_stealth && live_mount_hide && live_statfs && live_maps);
    }
    reset_faults();
    failed_mutations["kernel_build_spoof"] = {1};
    mismatched_build_queries = {2};
    assert(!apply_feature_config(config, error, true, true, &report));
    assert(find("kernel_build_spoof").state == FeatureApplyState::Unknown);
    assert(mutation_counts["kernel_build_spoof"] == 1);
    reset_faults();
    failed_mutations["maps_spoof"] = {1};
    assert(!apply_feature_config(config, error, true, true, &report));
    assert(find("maps_spoof").state == FeatureApplyState::Partial);
    assert(error.find("managed rules replaced") != std::string::npos);
    reset_faults();
    config.enable_maps_spoof = false;
    maps_clear_error = EIO;
    assert(!apply_feature_config(config, error, true, true, &report));
    assert(find("maps_spoof").state == FeatureApplyState::Partial && !live_maps);
    assert(error.find("maps spoof disabled") != std::string::npos);
    reset_faults();
    config.mount_hide_mode = "aggressive";
    supported_features &= ~SUMH_FEATURE_MOUNT_HIDE_AGGRESSIVE;
    mount_hide_calls.clear();
    assert(apply_feature_config(config, error, true, true, &report));
    assert(mount_hide_calls.size() == 1 &&
           mount_hide_calls.front().second == sumhp::sumh::MountHideMode::Normal);
    assert(find("mount_hide").detail.find("normal mode applied") != std::string::npos);
    reset_faults();
}

void test_conditional_feature_integration() {
    using namespace sumhp::mount::sumh;
    using sumhp::sumh::RuntimeFeature;
    const int old_caps = supported_features;
    supported_features |= SUMH_FEATURE_RUNTIME_STATE | SUMH_FEATURE_RUNTIME_SET;
    sumhp::Config config;
    config.enable_kernel_debug = true;
    config.enable_stealth = true;
    config.enable_mount_hide = true;
    config.enable_maps_spoof = true;
    config.enable_statfs_spoof = true;
    config.mount_hide_mode = "normal";
    std::string error;
    FeatureApplyReport report;
    runtime_reads = 0;
    runtime_writes.clear();
    assert(apply_feature_config(config, error, true, true, &report));
    assert((runtime_writes ==
            std::vector<RuntimeFeature>{RuntimeFeature::Debug, RuntimeFeature::Stealth,
                                        RuntimeFeature::MountHide, RuntimeFeature::StatfsSpoof,
                                        RuntimeFeature::MapsSpoof}));
    assert(runtime_reads == 10 && live_debug && live_stealth && live_mount_hide && live_statfs &&
           live_maps);
    runtime_reads = 0;
    runtime_writes.clear();
    runtime_write_error = ESTALE;
    assert(!apply_feature_config(config, error, true, true, &report));
    // Independent features still attempted once, never replayed on stale token.
    assert(runtime_writes.size() == 5 && runtime_reads == 5);
    assert(report.outcomes.front().state == FeatureApplyState::Failed);
    assert(report.outcomes.back().state ==
           FeatureApplyState::Partial);  // Maps set already replaced.
    runtime_write_error = 0;
    // GET-only kernels cannot authorize a conditional update; retain legacy behavior.
    supported_features &= ~SUMH_FEATURE_RUNTIME_SET;
    runtime_reads = 0;
    runtime_writes.clear();
    assert(apply_feature_config(config, error, true, true, &report));
    assert(runtime_reads == 0 && runtime_writes.empty());
    // Kernel Build has its own capability; the five-flag CAS bit alone must
    // never be mistaken for conditional string support.
    supported_features |= SUMH_FEATURE_RUNTIME_SET | SUMH_FEATURE_KERNEL_BUILD_CAS;
    config.enable_kernel_build_spoof = true;
    config.kernel_build_release = "6.1.conditional";
    config.kernel_build_version = "#8 conditional";
    runtime_writes.clear();
    assert(apply_feature_config(config, error, true, true, &report));
    assert((runtime_writes ==
            std::vector<RuntimeFeature>{RuntimeFeature::Debug, RuntimeFeature::Stealth,
                                        RuntimeFeature::MountHide, RuntimeFeature::StatfsSpoof,
                                        RuntimeFeature::KernelBuild, RuntimeFeature::MapsSpoof}));
    assert(live_build.release == config.kernel_build_release &&
           live_build.version == config.kernel_build_version);
    supported_features = old_caps;
}

void test_date_suggestion() {
    using sumhp::suggest_kernel_build_version;
    const std::string prefix = "#2 SMP PREEMPT_RT ";
    assert(suggest_kernel_build_version(original_version, system_build_date) ==
           prefix + system_build_date);
    assert(suggest_kernel_build_version("#17 Fri Aug 23 03:08:10 UTC 2024",
                                        "Sun Oct  4 12:30:00 +0800 2026") ==
           "#17 Sun Oct 4 12:30:00 +0800 2026");
    for (const auto& invalid :
         {std::string{}, std::string("2026-10-04"), std::string("Sun Oct 4 12:30:00 UTC 2026\n"),
          std::string("Sun Oct 4 25:30:00 UTC 2026"), std::string("Mon Feb 29 12:30:00 UTC 2025"),
          std::string("Sun Oct 4 12:30:00 +2460 2026"),
          std::string("Sun Oct 4 12:30:00 UTC 9999999999999999"),
          system_build_date + std::string("\0", 1)}) {
        assert(suggest_kernel_build_version(original_version, invalid).empty());
    }
    assert(suggest_kernel_build_version(original_version, "Thu Feb 29 12:30:00 UTC 2024") ==
           prefix + "Thu Feb 29 12:30:00 UTC 2024");
    assert(suggest_kernel_build_version("#1 unsupported date format", system_build_date).empty());
    assert(suggest_kernel_build_version("unknown Fri Aug 23 03:08:10 UTC 2024", system_build_date)
               .empty());
    const std::string long_prefix = "#1 " + std::string(33, 'X') + " ";
    const auto maximum = long_prefix + "Fri Aug 2 03:08:10 UTC 2024";
    assert(maximum.size() <= 64);
    assert(suggest_kernel_build_version(maximum, "Sun Oct 14 12:30:00 +0800 2026").empty());
}
}  // namespace

int main() {
    test_config_document_contract();
    test_config();
    test_maps_config();
    test_kmi_source();
    test_cli();
    test_build_cli_conditional_outcomes();
    test_module_configure_cli();
    test_maps_cli_and_apply();
    test_automatic_maps_request();
    test_boot_restore();
    test_maps_lifecycle_and_config_wrapper();
    test_retired_mount_mode_restore();
    test_feature_failure_isolation();
    test_conditional_feature_integration();
    test_date_suggestion();
    std::cout << "sumh_build_config_test: all checks passed\n";
}
