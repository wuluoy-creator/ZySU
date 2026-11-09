// Exercise the production config parser/merge and KMI source selection without
// filesystem writes, root privileges, or a running SUMH kernel.
#include "cli_args.hpp"
#include "core/json.hpp"
#include "sumhp/config.hpp"
#include "sumhp/embedded.hpp"
#include "sumhp/kernel_build.hpp"
#include "sumhp/sumh_client.hpp"
#include "uapi/sumh.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
std::optional<std::string> saved_config;
bool saved = false;
bool save_ok = true;
bool build_supported = true;
int capabilities_error = 0;
bool original_query_ok = true;
int original_query_error = EIO;
const std::string original_version = "#2 SMP PREEMPT_RT Fri Aug 23 03:08:10 UTC 2024";
std::string system_build_date = "Sun Oct 4 12:30:00 UTC 2026";
bool system_build_date_present = true;
bool boot_completed = false;
int projected_reads = 0;
bool apply_ok = true;
bool kernel_available = true;
bool safe_mode = false;
bool runtime_preparation_ok = true;
int runtime_preparation_calls = 0;
std::string mount_owner;
int build_apply_calls = 0;
int hide_apply_calls = 0;
int maps_boot_calls = 0;
int kstat_boot_calls = 0;
int kstat_restore_calls = 0;
bool kstat_restore_ok = true;
bool kstat_status_ok = true;
int kstat_status_calls = 0;
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
std::string cli_output;
std::function<void()> lock_acquire_hook;
bool config_lock_held = false;
std::vector<bool> kstat_apply_lock_states;
std::vector<sumhp::KstatPathRule> last_restored_kstat_rules;
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
}  // namespace ksud

namespace sumhp {
bool prepare_runtime(std::string& error) {
    ++runtime_preparation_calls;
    if (!runtime_preparation_ok)
        error = "injected migration failure";
    return runtime_preparation_ok;
}
namespace logging {
void set_debug_enabled(bool) {}
enum class Level { Info, Error };
void write(Level level, const std::string&, const std::string&) {
    if (level == Level::Error)
        ++boot_errors;
}
}  // namespace logging

struct ConfigFileLock {
    bool acquire(std::string&) {
        auto hook = std::move(lock_acquire_hook);
        lock_acquire_hook = {};
        if (hook)
            hook();
        config_lock_held = true;
        return true;
    }
    ~ConfigFileLock() { config_lock_held = false; }
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
FeatureCapabilities feature_capabilities() {
    if (capabilities_error)
        return {false, capabilities_error, 0};
    return {true, 0, build_supported ? SUMH_FEATURE_KERNEL_BUILD_SPOOF : 0};
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
    state = live_build;
    return true;
}
bool set_kernel_build(bool enable, const std::string& release, const std::string& version) {
    ++build_apply_calls;
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
    live_build = {enable, release, version};
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
    const int error =
        mode == MountHideMode::Aggressive ? aggressive_mount_error : normal_mount_error;
    if (enable && error) {
        errno = error;
        return false;
    }
    return true;
}
bool set_maps_spoof(bool enabled) {
    maps_operations.push_back(enabled ? "enable" : "disable");
    maps_enable_calls.push_back(enabled);
    if (maps_switch_error) {
        errno = maps_switch_error;
        return false;
    }
    return true;
}
bool set_statfs_spoof(bool) {
    return true;
}
bool managed_kstat_status(ManagedKstatStatus& status) {
    ++kstat_status_calls;
    if (!kstat_status_ok) {
        errno = EIO;
        return false;
    }
    status = {2, 9007199254740993ULL};
    return true;
}
bool set_debug(bool) {
    return true;
}
bool set_stealth(bool) {
    return true;
}
}  // namespace sumhp::sumh

namespace sumhp {
namespace mount {
void recovery_boot_completed() {}
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
void invalidate_active_state() {}
void mlog(const std::string&) {}
bool restore_managed_maps_rules(const Config& config, std::string& error) {
    maps_operations.push_back("restore");
    ++maps_restore_calls;
    if (!maps_restore_ok) {
        errno = ENOENT;
        error = "injected missing maps path";
        return false;
    }
    restored_maps = config.maps_rules;
    return true;
}
bool restore_managed_kstat_rules(const Config& config, std::string& error) {
    ++kstat_restore_calls;
    kstat_apply_lock_states.push_back(config_lock_held);
    last_restored_kstat_rules = config.kstat_rules;
    if (!kstat_restore_ok) {
        errno = ENOENT;
        error = "injected missing kstat path";
        return false;
    }
    return true;
}
bool apply_maps_config(const Config&, std::string&, bool restore_rules = true);
bool apply_feature_config(const Config&, std::string&, bool apply_immediately = false,
                          bool apply_maps = true);
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
bool restore_persisted_hide_rules(std::string&) {
    ++hide_apply_calls;
    return true;
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
#include "sumh_status_json_under_test.inc"
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
    if (args == std::vector<std::string>{"sumh", "maps", "apply"})
        ++maps_boot_calls;
    else if (args == std::vector<std::string>{"sumh", "kstat", "apply"})
        ++kstat_boot_calls;
    else {
        assert((args == std::vector<std::string>{"hide", "apply"}));
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

void test_kstat_config() {
    using namespace sumhp;
    std::string error;
    Config config;
    assert(parse_config_json(default_config_json(), config, error) && config.kstat_rules.empty());
    const std::string valid =
        R"({"kstat_rules":[{"target_path":"/target","source_path":"/source","mode":"full"}]})";
    assert(parse_config_json(valid, config, error) && config.kstat_rules.size() == 1);
    assert(config.kstat_rules[0].mode == "full");
    const std::string custom =
        R"({"kstat_rules":[{"target_path":"/target","mode":"custom","fields":{"ino":"18446744073709551615","size":"0","mtime":{"sec":"-1","nsec":"0"}}}]})";
    assert(parse_config_json(custom, config, error));
    assert(config.kstat_rules.size() == 1 && config.kstat_rules[0].source_path.empty());
    assert(config.kstat_rules[0].custom_fields.at("ino") == "18446744073709551615");
    KstatCustomValues custom_values;
    assert(parse_kstat_custom_fields(config.kstat_rules[0].custom_fields, custom_values, error));
    assert(custom_values.ino == UINT64_MAX && custom_values.size == 0);
    assert(custom_values.mtime_sec == -1 && custom_values.mtime_nsec == 0);
    assert(custom_values.field_mask ==
           (SUMH_KSTAT_FIELD_INO | SUMH_KSTAT_FIELD_SIZE | SUMH_KSTAT_FIELD_MTIME));
    const auto original_custom = json::parse(custom);
    assert(stringify_json(kstat_path_rules_json(config.kstat_rules)) ==
           stringify_json(*original_custom.find("kstat_rules")));
    assert(parse_config_json(valid, config, error));
    for (
        const auto* invalid_custom :
        {R"({"kstat_rules":[{"target_path":"/target","mode":"custom","fields":{}}]})",
         R"({"kstat_rules":[{"target_path":"/target","mode":"custom","source_path":"/source"}]})",
         R"({"kstat_rules":[{"target_path":"/target","mode":"custom","fields":{"ino":9007199254740993}}]})",
         R"({"kstat_rules":[{"target_path":"/target","mode":"custom","fields":{"ino":"18446744073709551616"}}]})",
         R"({"kstat_rules":[{"target_path":"/target","mode":"custom","fields":{"size":"-1"}}]})",
         R"({"kstat_rules":[{"target_path":"/target","mode":"custom","fields":{"size":"01"}}]})",
         R"({"kstat_rules":[{"target_path":"/target","mode":"custom","fields":{"mtime":{"sec":"0"}}}]})",
         R"({"kstat_rules":[{"target_path":"/target","mode":"custom","fields":{"mtime":{"sec":"0","nsec":"1000000000"}}}]})",
         R"({"kstat_rules":[{"target_path":"/target","mode":"custom","fields":{"unknown":"1"}}]})",
         R"({"kstat_rules":[{"target_path":"/target","mode":"custom","fields":{"mtime.sec":"0","mtime.nsec":"0"}}]})",
         R"({"kstat_rules":[{"target_path":"/target","mode":"identity","source_path":"/source","fields":{"ino":"1"}}]})"}) {
        assert(!parse_config_json(invalid_custom, config, error));
        assert(config.kstat_rules.size() == 1 && config.kstat_rules[0].mode == "full");
    }
    for (
        const auto* invalid :
        {R"({"kstat_rules":{}})", R"({"kstat_rules":[1]})",
         R"({"kstat_rules":[{"target_path":"/target","source_path":"/source"}]})",
         R"({"kstat_rules":[{"target_path":"relative","source_path":"/source","mode":"full"}]})",
         R"({"kstat_rules":[{"target_path":"/target","source_path":"relative","mode":"full"}]})",
         R"({"kstat_rules":[{"target_path":"/target","source_path":"/source","mode":"bad"}]})",
         R"({"kstat_rules":[{"target_path":"/target","source_path":"/source","mode":"full","extra":1}]})",
         R"({"kstat_rules":[{"target_path":"/target\u0000","source_path":"/source","mode":"full"}]})",
         R"({"kstat_rules":[{"target_path":"/target","source_path":"/source\n","mode":"full"}]})",
         R"({"kstat_rules":[{"target_path":"/target","source_path":"/source","mode":"full"},{"target_path":"/target","source_path":"/source","mode":"identity"}]})"}) {
        assert(!parse_config_json(invalid, config, error));
        assert(config.kstat_rules.size() == 1 && config.kstat_rules[0].mode == "full");
    }
    std::vector<KstatPathRule> rules{
        {"/" + std::string(254, 't'), "/" + std::string(4094, 's'), "identity"}};
    assert(validate_kstat_path_rules(rules, error));
    rules[0].source_path += 's';
    assert(!validate_kstat_path_rules(rules, error));
    rules[0].source_path = "/source";
    assert(validate_kstat_path_rules(rules, error));
    rules[0].target_path += 't';
    assert(!validate_kstat_path_rules(rules, error));
    rules.clear();
    for (unsigned int index = 0; index < SUMH_MANAGED_KSTAT_MAX_RULES; ++index)
        rules.push_back({"/target/" + std::to_string(index), "/source", "identity"});
    assert(validate_kstat_path_rules(rules, error));
    rules.push_back({"/overflow", "/source", "full"});
    assert(!validate_kstat_path_rules(rules, error));
    saved_config = default_config_json();
    assert(merge_config_json(valid, error));
    const auto persisted = saved_config;
    saved = false;
    assert(!merge_config_json(R"({"kstat_rules":[{}]})", error));
    assert(!saved && saved_config == persisted);
    assert(merge_config_json(R"({"debug":true})", error));
    assert(read_config_file(config, error) && config.kstat_rules.size() == 1);
    assert(parse_config_json("{}", config, error) && config.kstat_rules.empty());
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
    assert(run_cli({"sumh", "kernel-build", "set", "different", "#2 different"}) != 0);
    assert(saved_config == previous_config && live_build.release == "6.1.75-android14-example");
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
    assert(!config.enable_kernel_build_spoof &&
           config.kernel_build_release == "6.1.75-android14-example");
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
    assert(run_cli({"sumh", "kernel-build", "reset"}) == 0);
    assert(read_config_file(config, error) && !config.enable_kernel_build_spoof &&
           config.kernel_build_release.empty() && config.kernel_build_version == "#7 saved" &&
           config.kernel_build_apply_stage == "post-fs-data" && config.mount_source == "MY-KSU");
    assert(json::parse(*saved_config).find("future-setting")->n == 42);
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

void test_kstat_cli_and_apply() {
    using namespace sumhp;
    saved_config = default_config_json();
    kstat_restore_calls = 0;
    kstat_restore_ok = true;
    assert(run_cli({"sumh", "kstat", "add-path", "/target", "/source", "identity"}) == 0);
    assert(kstat_restore_calls == 1);
    assert(run_cli({"sumh", "kstat", "add-path", "/target", "/other", "full"}) == 0);
    assert(run_cli({"sumh", "kstat", "list-paths"}) == 0);
    const auto list = json::parse(cli_output);
    assert(list.a.size() == 1 && list.a[0].find("source_path")->s == "/other");
    assert(list.a[0].find("mode")->s == "full");
    assert(run_cli({"sumh", "kstat", "apply"}) == 0 && kstat_restore_calls == 3);
    assert(run_cli({"sumh", "kstat", "add-custom", "/target",
                    R"({"size":"0","mtime":{"sec":"-1","nsec":"0"}})"}) == 0);
    assert(run_cli({"sumh", "kstat", "list-paths"}) == 0);
    const auto custom_list = json::parse(cli_output);
    assert(custom_list.a.size() == 1 && custom_list.a[0].find("mode")->s == "custom");
    assert(custom_list.a[0].find("source_path") == nullptr);
    assert(custom_list.a[0].find("fields")->find("size")->s == "0");
    assert(custom_list.a[0].find("fields")->find("mtime")->find("sec")->s == "-1");
    const auto persisted = saved_config;
    assert(run_cli({"sumh", "kstat", "add-path", "relative", "/source", "identity"}) != 0);
    assert(run_cli({"sumh", "kstat", "add-path", "/target", "/source", "bad"}) != 0);
    assert(run_cli({"sumh", "kstat", "apply", "extra"}) != 0);
    assert(run_cli({"sumh", "kstat", "add-custom", "/target", R"({"size":0})"}) != 0);
    assert(saved_config == persisted && kstat_restore_calls == 4);
    kstat_restore_ok = false;
    assert(run_cli({"sumh", "kstat", "remove-path", "/target"}) != 0);
    assert(json::parse(cli_output).find("saved")->b);
    assert(!json::parse(cli_output).find("applied")->b && errno == ENOENT);
    Config config;
    std::string error;
    assert(read_config_file(config, error) && config.kstat_rules.empty());
    kstat_restore_ok = true;
    assert(run_cli({"sumh", "kstat", "apply"}) == 0);
    assert(is_query({"sumh", "kstat", "list-paths"}));
    saved_config = "{invalid";
    assert(run_cli({"sumh", "kstat", "status"}) == 0);
    const auto status = json::parse(cli_output);
    assert(status.find("active_count")->n == 2);
    assert(status.find("generation")->s == "9007199254740993");
    assert(kstat_status_calls == 1);
    kstat_status_ok = false;
    assert(run_cli({"sumh", "kstat", "status"}) != 0);
    assert(cli_output.empty() && kstat_status_calls == 2);
    kstat_status_ok = true;
    assert(is_query({"sumh", "kstat", "status"}));
    assert(!is_query({"sumh", "kstat", "apply"}));
    assert(!is_query({"sumh", "kstat", "add-path", "/target", "/source", "identity"}));
    saved_config = default_config_json();
}

void test_kstat_cli_edits_latest_locked_config() {
    using namespace sumhp;
    saved_config = default_config_json();
    kstat_apply_lock_states.clear();
    lock_acquire_hook = [] {
        saved_config =
            R"({"kstat_rules":[{"target_path":"/external","source_path":"/source","mode":"identity"}]})";
    };
    assert(run_cli({"sumh", "kstat", "add-path", "/local", "/source", "full"}) == 0);
    Config config;
    std::string error;
    assert(read_config_file(config, error) && config.kstat_rules.size() == 2);
    assert(config.kstat_rules[0].target_path == "/external");
    assert(config.kstat_rules[1].target_path == "/local");

    lock_acquire_hook = [] {
        saved_config =
            R"({"kstat_rules":[{"target_path":"/external","source_path":"/source","mode":"identity"},{"target_path":"/local","source_path":"/source","mode":"full"},{"target_path":"/concurrent","source_path":"/source","mode":"identity"}]})";
    };
    assert(run_cli({"sumh", "kstat", "remove-path", "/local"}) == 0);
    assert(read_config_file(config, error) && config.kstat_rules.size() == 2);
    assert(config.kstat_rules[0].target_path == "/external");
    assert(config.kstat_rules[1].target_path == "/concurrent");
    assert((kstat_apply_lock_states == std::vector<bool>{true, true}));

    lock_acquire_hook = [] {
        saved_config =
            R"({"kstat_rules":[{"target_path":"/external","source_path":"/source","mode":"identity"},{"target_path":"/concurrent","source_path":"/source","mode":"identity"},{"target_path":"/latest","source_path":"/source","mode":"full"}]})";
    };
    assert(run_cli({"sumh", "kstat", "apply"}) == 0);
    assert((kstat_apply_lock_states == std::vector<bool>{true, true, true}));
    assert(last_restored_kstat_rules.size() == 3 &&
           last_restored_kstat_rules.back().target_path == "/latest");
    saved_config = default_config_json();
}

void test_kstat_status_json() {
    using namespace sumhp;
    std::ostringstream output;
    auto* previous = std::cout.rdbuf(output.rdbuf());
    append_managed_kstat_status_json(true, SUMH_FEATURE_MANAGED_KSTAT);
    std::cout.rdbuf(previous);
    const auto status = json::parse("{" + output.str().substr(1) + "}");
    const auto* entry = status.find("managed_kstat_status");
    assert(entry && entry->find("active_count")->n == 2);
    assert(entry->find("generation")->s == "9007199254740993");
    output.str("");
    output.clear();
    kstat_status_ok = false;
    previous = std::cout.rdbuf(output.rdbuf());
    append_managed_kstat_status_json(true, SUMH_FEATURE_MANAGED_KSTAT);
    append_managed_kstat_status_json(true, 0);
    append_managed_kstat_status_json(false, SUMH_FEATURE_MANAGED_KSTAT);
    std::cout.rdbuf(previous);
    assert(output.str().empty());
    kstat_status_ok = true;
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
    assert(kstat_boot_calls == 1);
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
    const int kstat_before_safe_mode = kstat_boot_calls;
    embedded_boot_completed();
    assert(build_apply_calls == 0 && hide_apply_calls == 0);
    assert(maps_boot_calls == maps_before_safe_mode);
    assert(kstat_boot_calls == kstat_before_safe_mode);
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
    assert(feature_reset_calls == 0 && snapshot_calls == 0 && xattr_restore_calls == 0);
    assert(live_build.enabled && mount_hide_calls.size() == 1 && mount_hide_calls[0].first);
    assert((maps_operations == std::vector<std::string>{"restore"}));
    maps_restore_ok = true;
    maps_operations.clear();
    assert(apply_config_file() == 0);
    assert(snapshot_calls == 1 && xattr_restore_calls == 1 && feature_reset_calls == 0);
    assert((maps_operations == std::vector<std::string>{"restore", "enable"}));
    xattr_restore_ok = false;
    assert(apply_config_file() != 0);
    assert(feature_reset_calls == 1);  // Existing independent xattr failure cleanup remains.
    xattr_restore_ok = true;
    replay_pending = true;
    replay_ok = false;
    replay_calls = 0;
    assert(apply_config_file() != 0 && replay_calls == 1);
    assert(feature_reset_calls == 1);
    replay_ok = true;
    assert(apply_config_file() == 0 && replay_calls == 2);
    replay_pending = false;
    saved_config.reset();
    const auto before_missing = maps_operations.size();
    assert(apply_config_file() != 0 && maps_operations.size() == before_missing);
    assert(is_query({"sumh", "maps", "list-paths"}));
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
    assert(!error.empty() && mount_hide_calls.size() == 2);
    assert(!mount_hide_calls.back().first);  // Existing failure cleanup remains active.

    mount_hide_calls.clear();
    error.clear();
    aggressive_mount_error = EOPNOTSUPP;
    normal_mount_error = EIO;
    assert(!sumhp::mount::sumh::apply_feature_config(config, error, false));
    assert(!error.empty() && mount_hide_calls.size() == 3);
    assert(!mount_hide_calls.back().first);

    mount_hide_calls.clear();
    error.clear();
    aggressive_mount_error = normal_mount_error = 0;
    assert(sumhp::mount::sumh::apply_feature_config(config, error, false));
    assert(error.empty() && mount_hide_calls.size() == 1);
    assert(mount_hide_calls[0].second == MountHideMode::Aggressive);
    mount_hide_calls.clear();
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
    test_config();
    test_maps_config();
    test_kstat_config();
    test_kmi_source();
    test_cli();
    test_module_configure_cli();
    test_maps_cli_and_apply();
    test_kstat_cli_and_apply();
    test_kstat_cli_edits_latest_locked_config();
    test_kstat_status_json();
    test_boot_restore();
    test_maps_lifecycle_and_config_wrapper();
    test_retired_mount_mode_restore();
    test_date_suggestion();
    std::cout << "sumh_build_config_test: all checks passed\n";
}
