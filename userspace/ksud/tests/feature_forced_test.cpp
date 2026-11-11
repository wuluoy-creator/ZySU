#include <algorithm>
#include <cassert>
#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include "uapi/feature.h"

template <typename... Args>
void log_fixture(const char*, Args...) {}
#define LOGE(...) log_fixture(__VA_ARGS__)
#define LOGW(...) log_fixture(__VA_ARGS__)
#define LOGI(...) log_fixture(__VA_ARGS__)
constexpr const char* FEATURE_CONFIG_PATH = "fixture";
std::map<uint32_t, uint64_t> runtime, text_config, saved_config;
std::optional<std::map<uint32_t, uint64_t>> binary_config;
std::optional<std::string> text_file;
std::set<uint32_t> forced, writes;
int daemon_starts, rebinds;
std::pair<uint64_t, bool> get_feature(uint32_t id) {
    const auto it = runtime.find(id);
    return it == runtime.end() ? std::make_pair(uint64_t(0), false)
                               : std::make_pair(it->second, true);
}
bool is_feature_forced(uint32_t id) {
    return forced.count(id);
}
int set_feature(uint32_t id, uint64_t value) {
    assert(!forced.count(id));  // Even issuing a SET for a forced feature is a regression.
    writes.insert(id);
    runtime[id] = value;
    return 0;
}
auto load_binary_config() {
    return binary_config;
}
auto read_file(const std::string&) {
    return text_file;
}
auto parse_text_feature_config(const std::string&) {
    return text_config;
}
int ensure_sulogd_running() {
    ++daemon_starts;
    return 0;
}
int refresh_sucompat_vfs_locked() {
    ++rebinds;
    return 0;
}
const char* feature_id_to_name(uint32_t) {
    return "fixture";
}
int restore_su_path() {
    return 0;
}
void print_selinux_hide_runtime_status() {}
void print_unicode_path_guard_runtime_status() {}
namespace sumhp::sumh {
bool is_available() {
    return true;
}
}  // namespace sumhp::sumh
struct SucompatTransitionLock {
    bool locked() const { return true; }
};
int save_feature_config_files(const std::map<uint32_t, uint64_t>& values) {
    saved_config = values;
    return 0;
}

#include "feature_persistence_under_test.inc"

int main() {
    // Name and numeric entry points reject the removed feature even if an
    // older kernel still advertises its former ID.
    assert(!parse_feature_id("kernel_umount").second);
    assert(!parse_feature_id("1").second);
    assert(parse_feature_id("sulog") == std::make_pair(uint32_t(2), true));
    assert(parse_feature_id("2") == std::make_pair(uint32_t(2), true));
    runtime = {{1, 1}, {2, 1}, {3, 1}, {4, 0}, {5, 1}, {6, 1}};
    forced = {2, 6};
    binary_config = {{{1, 0}, {2, 0}, {3, 0}, {5, 0}, {6, 0}, {106, 0}, {108, 3}}};
    auto saved = get_current_feature_values();
    assert(saved.at(2) == 0 && saved.at(6) == 0);  // Forced-on must not overwrite saved-off.
    assert(saved.at(3) == 1 && saved.at(5) == 1);  // ID 5 remains a distinct local feature.
    assert(!saved.count(1) && !saved.count(106) && !saved.count(108));
    forced.clear();
    saved = get_current_feature_values();
    assert(saved.at(2) == 1 && saved.at(6) == 1);
    forced = {2, 6};
    binary_config.reset();
    text_file = "kernel_umount=0\nsulog=0";
    text_config = {{1, 0}, {2, 0}};
    saved = get_current_feature_values();
    assert(saved.at(2) == 0 && !saved.count(6));  // No old setting: do not invent one.
    assert(!saved.count(1));  // Old text preferences must not revive a global bypass.

    std::map<uint32_t, uint64_t> config = {{1, 0}, {2, 0}, {3, 0}, {6, 0}, {106, 0}};
    assert(!apply_config_locked(config));
    assert(runtime.at(2) == 1 && runtime.at(3) == 0 && runtime.at(6) == 1);
    assert(config.at(2) == 0 && config.at(6) == 0 && !config.count(106));
    assert(runtime.at(1) == 1 && !config.count(1));
    assert(daemon_starts == 1 && rebinds == 1 && writes == std::set<uint32_t>{3});
    writes.clear();
    assert(feature_set("kernel_umount", 0) == 1 && writes.empty());
    assert(feature_set("kernel_umount", 1) == 1 && writes.empty());
    assert(feature_set("1", 0) == 1 && writes.empty());
    assert(feature_set("1", 1) == 1 && writes.empty());
    assert(feature_set_impl("sulog", 2, 0) == 1 && writes.empty());
    assert(!feature_set_impl("adb_root", 3, 1) && writes.count(3));

    writes.clear();
    text_config = {{1, 0}, {2, 0}, {3, 0}, {6, 0}};
    assert(!feature_load_config());
    auto expected_config = text_config;
    expected_config.erase(1);
    assert(runtime.at(1) == 1 && runtime.at(2) == 1 && runtime.at(6) == 1);
    assert(saved_config == expected_config);
    assert(daemon_starts == 2 && writes == std::set<uint32_t>{3});
    forced.clear();
    writes.clear();
    assert(!apply_config_locked(saved_config));
    assert(runtime.at(2) == 0 &&
           runtime.at(6) == 0);  // Old preferences return after removing force.
    puts(
        "PASS forced-feature persistence, removed umount migration, SET refusal and sulog startup");
}
