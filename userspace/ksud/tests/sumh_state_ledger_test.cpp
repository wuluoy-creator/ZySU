#include <cassert>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include "core/capture_buffer.hpp"
#include "core/command.hpp"
#include "core/operation_query.hpp"
#include "core/operation_resource.hpp"
#include "core/operation_submit.hpp"
#include "core/runtime_status.hpp"
#include "core/state_ledger.hpp"
#include "core/user_hide_input.hpp"
#include "core/user_hide_store.hpp"
#include "mount/maps_snapshot.hpp"
#include "mount/maps_temporary.hpp"
#include "sumhp/config.hpp"

#ifdef SUMH_TRACKED_MAPS_NATIVE_FIXTURES
extern "C" int fixture_real_sha256(const unsigned char*, std::size_t, unsigned char*, int);
#endif

namespace fixture_clock {
struct Clock {
    using duration = std::chrono::steady_clock::duration;
    using time_point = std::chrono::time_point<Clock>;
    static time_point current;
    static time_point now() { return current; }
};
Clock::time_point Clock::current{};
int sleeps = 0;
std::function<void(int)> on_sleep;
void sleep_for(Clock::duration value) {
    assert(value > Clock::duration::zero() && value <= std::chrono::milliseconds(100));
    Clock::current += value;
    ++sleeps;
    if (on_sleep)
        on_sleep(sleeps);
}
}

namespace {
using namespace sumhp;
namespace fs = std::filesystem;
constexpr int test_rdonly = 1, test_rdwr = 2, test_creat = 4, test_cloexec = 8, test_nofollow = 16,
              test_nonblock = 32;
constexpr int test_lock_ex = 1, test_lock_nb = 2;
struct File {
    std::string content;
    int mode = 1;
    int links = 1;
};
struct Descriptor {
    std::string path;
    std::size_t offset = 0;
};
struct TestStat {
    int st_mode = 1;
    int st_nlink = 1;
    std::int64_t st_size = 0;
};
std::map<std::string, File> files;
std::map<int, Descriptor> descriptors;
int next_fd = 3, held_lock = -1, write_calls = 0, fail_write = 0, command_calls = 0,
    command_result = 0;
int prepare_calls = 0, random_calls = 0, corrupt_readback_write = 0, throw_after_write = 0;
int interrupted_reads = 0;
int log_calls = 0;
struct TrackedIoctl {
    unsigned long command;
    std::function<int(void*)> invoke;
};
std::deque<TrackedIoctl> tracked_ioctls;
int tracked_delete_calls = 0, tracked_feature_calls = 0;
std::vector<std::pair<std::string, int>> opened;
std::vector<std::pair<std::string, std::string>> written;
std::function<void(const std::string&)> on_open;
bool fail_after_replace = false, fail_read = false, fail_random = false, unavailable = false,
     runtime_fail = false;
bool flip_generation = false, replace_config = false, command_throws = false,
     prepare_throws = false;
std::string boot_id = "boot-a", hashed_bytes;
std::string command_output, command_error_output;
sumh::RuntimeState live;
sumh::KernelBuild build{false, "original", "original version"};
std::vector<sumh::UserHideRule> hides;
int kernel_reads = 0;
bool inventory_supported = false;
mount::sumh::MapsSnapshot maps_snapshot;
std::string key(const char* name) {
    return (fs::path("/fixture") / name).generic_string();
}
void put(const char* name, const std::string& bytes) {
    files[key(name)].content = bytes;
}
template <typename Character>
int fixture_open(const Character* path, int flags, int = 0) {
    assert((flags & test_cloexec) && (flags & test_nofollow) && (flags & test_nonblock));
    const auto name = fs::path(path).generic_string();
    opened.emplace_back(name, flags);
    if (on_open)
        on_open(name);
    if (!files.count(name)) {
        if (flags & test_creat)
            files[name] = {};
        else {
            errno = ENOENT;
            return -1;
        }
    }
    descriptors[next_fd] = {name, 0};
    return next_fd++;
}
int fixture_close(int fd) {
    if (held_lock == fd)
        held_lock = -1;
    descriptors.erase(fd);
    return 0;
}
int fixture_fstat(int fd, TestStat* out) {
    const auto& f = files.at(descriptors.at(fd).path);
    *out = {f.mode, f.links, static_cast<std::int64_t>(f.content.size())};
    return 0;
}
std::ptrdiff_t fixture_read(int fd, void* dest, std::size_t capacity) {
    if (interrupted_reads > 0) {
        --interrupted_reads;
        fixture_clock::Clock::current += std::chrono::milliseconds(120);
        errno = EINTR;
        return -1;
    }
    if (fail_read) {
        errno = EACCES;
        return -1;
    }
    auto& d = descriptors.at(fd);
    const auto& bytes = files.at(d.path).content;
    const auto count = std::min(capacity, bytes.size() - d.offset);
    std::memcpy(dest, bytes.data() + d.offset, count);
    d.offset += count;
    return static_cast<std::ptrdiff_t>(count);
}
int fixture_flock(int fd, int flags) {
    assert(flags == (test_lock_ex | test_lock_nb));
    if (held_lock >= 0) {
        errno = EWOULDBLOCK;
        return -1;
    }
    held_lock = fd;
    return 0;
}
std::ptrdiff_t fixture_random(void* target, std::size_t count, int) {
    if (fail_random) {
        errno = EIO;
        return -1;
    }
    std::memset(target, ++random_calls, count);
    return static_cast<std::ptrdiff_t>(count);
}
// Only library calls are substituted; production framing uses the complete
// binary string and renders exactly 32 output bytes without sign extension.
int mbedtls_sha256(const unsigned char* input, std::size_t size, unsigned char* output, int is224) {
    assert(is224 == 0);
    hashed_bytes.assign(reinterpret_cast<const char*>(input), size);
#ifdef SUMH_TRACKED_MAPS_NATIVE_FIXTURES
    return fixture_real_sha256(input, size, output, is224);
#else
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t i = 0; i < size; ++i)
        hash = (hash ^ input[i]) * 1099511628211ULL;
    for (unsigned i = 0; i < 32; ++i)
        output[i] = static_cast<unsigned char>((hash >> (i % 8 * 8)) ^ i);
    return 0;
#endif
}
void reset() {
    files.clear();
    descriptors.clear();
    next_fd = 3;
    held_lock = -1;
    write_calls = fail_write = command_calls = command_result = kernel_reads = 0;
    prepare_calls = random_calls = corrupt_readback_write = throw_after_write = 0;
    opened.clear();
    written.clear();
    on_open = {};
    log_calls = 0;
    tracked_ioctls.clear();
    tracked_delete_calls = tracked_feature_calls = 0;
    interrupted_reads = 0;
    fixture_clock::Clock::current = {};
    fixture_clock::sleeps = 0;
    fixture_clock::on_sleep = {};
    fail_after_replace = fail_read = fail_random = unavailable = runtime_fail = false;
    flip_generation = replace_config = command_throws = prepare_throws = false;
    inventory_supported = false;
    maps_snapshot = {};
    boot_id = "boot-a";
    live = {};
    live.instance_id = 9;
    live.generation = 7;
    live.flags = 3;
    command_output.clear();
    command_error_output.clear();
    for (std::uint32_t i = 1; i <= 6; ++i)
        live.entries.push_back(
            {static_cast<sumh::RuntimeFeature>(i), true, false, false, false, 0});
    build = {false, "original", "original version"};
    hides.clear();
    put("config.json", "{\"enable_stealth\":false}");
}
}  // namespace

namespace ksud {
std::optional<std::string> getprop(const std::string&) {
    return "1";
}
bool write_file_durable(const fs::path& path, const std::string& content, std::string& error) {
    ++write_calls;
    written.emplace_back(path.generic_string(), content);
    const bool fail = fail_write == write_calls;
    if (!fail || fail_after_replace)
        files[path.generic_string()].content = content;
    if (write_calls == throw_after_write)
        throw std::runtime_error("injected after durable replacement");
    if (write_calls == corrupt_readback_write)
        files[path.generic_string()].content = "readback changed";
    if (fail) {
        error = fail_after_replace ? "replaced but durability unconfirmed" : "not committed";
        errno = EIO;
    }
    return !fail;
}
}
namespace sumhp {
fs::path runtime_data_dir() {
    return "/fixture";
}
std::string runtime_boot_id() {
    return boot_id;
}
bool prepare_private_directory(const fs::path&, std::string&) {
    ++prepare_calls;
    if (prepare_throws)
        throw std::runtime_error("injected before begin");
    return true;
}
// Config parsing is covered separately by the production parser regressions.
bool parse_config_json(const std::string& text, Config& config, std::string& error) {
    JsonValue value;
    if (!parse_json(text, value, error) || !value.is_object())
        return false;
    config = Config{};
    if (const auto* v = value.find("enable_kernel_debug"))
        config.enable_kernel_debug = v->b;
    if (const auto* v = value.find("enable_stealth"))
        config.enable_stealth = v->b;
    if (const auto* v = value.find("enable_maps_spoof"))
        config.enable_maps_spoof = v->b;
    if (const auto* v = value.find("maps_rules"))
        for (const auto& item : v->a)
            config.maps_rules.push_back(
                {item.o.at("target_path").s, item.o.at("replacement_path").s});
    if (const auto* v = value.find("enable_kernel_build_spoof"))
        config.enable_kernel_build_spoof = v->b;
    if (const auto* v = value.find("kernel_build_release"))
        config.kernel_build_release = v->s;
    if (const auto* v = value.find("kernel_build_version"))
        config.kernel_build_version = v->s;
    return true;
}
JsonValue maps_path_rules_json(const std::vector<MapsPathRule>& rules) {
    JsonValue out;
    out.type = JsonValue::Type::Array;
    for (const auto& rule : rules) {
        auto row = JsonValue::object();
        row.o["target_path"] = rule.target_path;
        row.o["replacement_path"] = rule.replacement_path;
        out.a.push_back(std::move(row));
    }
    return out;
}
namespace sumh {
extern "C" int ksu_sumh_ioctl(unsigned long command, void* argument) {
    assert(!tracked_ioctls.empty());
    const auto call = std::move(tracked_ioctls.front());
    tracked_ioctls.pop_front();
    assert(call.command == command && held_lock >= 0);
    state::Ledger persisted;
    std::string error;
    assert(state::parse_ledger(files.at(key("state_ledger.json")).content, persisted, error));
    assert(persisted.last.phase == "started" && persisted.last.name == "sumh maps remove");
    if (command == SUMH_IOC_DELETE_TEMPORARY_MAPS)
        ++tracked_delete_calls;
    else {
        assert(command == SUMH_IOC_GET_FEATURES);
        ++tracked_feature_calls;
    }
    return call.invoke(argument);
}
#include "tracked_maps_client_under_test.inc"
VersionInfo version_info() {
    VersionInfo out;
    out.status = unavailable ? Status::NotPresent : Status::Available;
    out.last_errno = unavailable ? ENODEV : 0;
    return out;
}
std::string active_rules() {
    assert(false);
    return {};
}
bool rule_inventory(const InventoryRequest&, InventoryPage&) {
    assert(false);
    return false;
}
FeatureCapabilities feature_capabilities() {
    return {true, 0,
            SUMH_FEATURE_RUNTIME_STATE | SUMH_FEATURE_MANAGED_HIDE |
                (inventory_supported ? SUMH_FEATURE_RULE_INVENTORY : 0)};
}
bool runtime_state(RuntimeState& out) {
    ++kernel_reads;
    if (runtime_fail) {
        errno = EIO;
        return false;
    }
    if (kernel_reads == 2 && flip_generation)
        ++live.generation;
    if (kernel_reads == 2 && replace_config)
        put("config.json", "{\"enable_kernel_debug\":true}");
    out = live;
    return true;
}
bool kernel_build(KernelBuild& out) {
    out = build;
    return true;
}
bool original_kernel_build(KernelBuild& out) {
    out = {false, "original", "original version"};
    return true;
}
bool user_hide_rules(std::vector<UserHideRule>& out) {
    out = hides;
    return true;
}
}
namespace logging {
enum class Level { Debug, Info, Warning, Error };
void write(Level, const std::string&, const std::string&) {
    ++log_calls;
}
}
bool is_query(const std::vector<std::string>& args) {
    return args[0] == "api";
}
std::string operation_description(const std::vector<std::string>&) {
    return "fixture";
}
int run_command(const std::vector<std::string>&) {
    ++command_calls;
    assert(errno == 0);
    std::cout << command_output;
    std::cerr << command_error_output;
    if (command_throws)
        throw std::runtime_error("injected after start");
    return command_result;
}
}

namespace sumhp::mount::sumh {
MapsSnapshot sample_maps(const Config&, bool resolve_disabled,
                         const ::sumhp::sumh::RuntimeState* expected) {
    assert(!resolve_disabled && expected && expected->instance_id == 9 &&
           expected->generation == 7);
    return maps_snapshot;
}
#include "maps_comparison_under_test.inc"

#define open fixture_open
#define close fixture_close
#define fstat fixture_fstat
#define stat TestStat
#define read fixture_read
#define flock fixture_flock
#define getrandom fixture_random
#define S_ISREG(mode) ((mode) == 1)
#define O_RDONLY test_rdonly
#define O_RDWR test_rdwr
#define O_CREAT test_creat
#define O_CLOEXEC test_cloexec
#define O_NOFOLLOW test_nofollow
#define O_NONBLOCK test_nonblock
#define LOCK_EX test_lock_ex
#define LOCK_NB test_lock_nb
#include "state_ledger_under_test.inc"
#undef read
#undef stat
#include "operation_query_under_test.inc"
namespace sumhp {
#include "state_capture_under_test.inc"
}

namespace {
const state::FeatureObservation& feature(const state::Snapshot& snapshot, const char* id) {
    for (const auto& item : snapshot.features)
        if (item.id == id)
            return item;
    assert(false);
    return snapshot.features.front();
}
void test_model_and_sampling() {
    reset();
    const auto binary_revision = state::revision_for_bytes(std::string("a\0b", 3));
    assert(binary_revision.size() == 74 && hashed_bytes.size() == 3);
    auto snapshot = state::collect_snapshot();
    assert(feature(snapshot, "kernel_debug").status == "matches");
    assert(feature(snapshot, "maps_rules").status == "unknown");
    assert(feature(snapshot, "modules").status == "unknown");
    assert(feature(snapshot, "kernel_build_spoof").status == "matches");
    build.version = "unrestored version";
    assert(feature(state::collect_snapshot(), "kernel_build_spoof").status == "drift");
    build = {false, "original", "original version"};
    state::Ledger ledger;
    ledger.last = {std::string(32, 'a'), "config apply", "started", "boot-a", 0, 0, {}};
    state::finish_ledger(ledger, snapshot, 1, EIO, "failed_or_partial");
    // A failed overall command can coexist with individually verified state.
    assert(ledger.verified.count("kernel_debug") && !ledger.verified.count("maps_rules"));
    auto encoded = state::encode_ledger(ledger);
    std::string error;
    state::Ledger restored;
    assert(state::parse_ledger(encoded, restored, error));
    assert(restored.last.exit_code == 1);
    auto json = state::state_json(snapshot, &restored, "valid", {});
    assert(json.o.at("features").o.at("kernel_debug").o.at("applied_revision").is_string());
    snapshot.instance_id = "10";
    json = state::state_json(snapshot, &restored, "valid", {});
    assert(!json.o.at("features").o.at("kernel_debug").o.at("applied_revision").is_string());
    snapshot.instance_id = "9";
    snapshot.boot_id = "boot-b";
    json = state::state_json(snapshot, &restored, "valid", {});
    assert(!json.o.at("features").o.at("kernel_debug").o.at("applied_revision").is_string());
    JsonValue corrupted;
    assert(parse_json(encoded, corrupted, error));
    corrupted.o["schema_version"] = 4;
    assert(!state::parse_ledger(stringify_json(corrupted), restored, error) &&
           restored.last.exit_code == 1);
    assert(!state::parse_ledger("{}", restored, error));
    assert(!state::parse_ledger(std::string(state::LEDGER_MAX_BYTES + 1, 'x'), restored, error));
    reset();
    put("config.json", "{\"enable_kernel_debug\":true,\"enable_stealth\":false}");
    snapshot = state::collect_snapshot();
    assert(feature(snapshot, "kernel_debug").status == "drift");
    assert(feature(snapshot, "kernel_debug").desired_revision !=
           ledger.verified.at("kernel_debug").revision);
    reset();
    flip_generation = true;
    snapshot = state::collect_snapshot();
    assert(feature(snapshot, "kernel_debug").status == "unknown" &&
           feature(snapshot, "kernel_debug").reason == "runtime_changed_during_sample");
    reset();
    replace_config = true;
    snapshot = state::collect_snapshot();
    assert(feature(snapshot, "kernel_debug").reason == "definitions_changed_during_sample");
    reset();
    runtime_fail = true;
    assert(feature(state::collect_snapshot(), "kernel_debug").status == "unknown");
    reset();
    put("config.json", "broken");
    snapshot = state::collect_snapshot();
    assert(feature(snapshot, "kernel_debug").desired_revision.empty() &&
           feature(snapshot, "kernel_debug").status == "unknown");
    reset();
    put("module_rules.json", "[]");
    snapshot = state::collect_snapshot();
    assert(feature(snapshot, "modules").desired_revision.empty() &&
           feature(snapshot, "modules").reason == "module_definitions_unreadable_or_invalid");
    reset();
    put("module_mode.json", "broken");
    assert(feature(state::collect_snapshot(), "modules").desired_revision.empty());
    reset();
    put("module_rules.json", std::string(4 * 1024 * 1024 + 1, 'x'));
    assert(feature(state::collect_snapshot(), "modules").desired_revision.empty());
    reset();
    put("user_hide_rules.json", "[\"/path with spaces \"]");
    hides = {{1, 3, SUMH_USER_HIDE_LIVE, SUMH_USER_HIDE_BOUND, 0, "/path with spaces "}};
    assert(feature(state::collect_snapshot(), "user_hide").status == "matches");
    hides.front().management = SUMH_USER_HIDE_SUSPENDED;
    snapshot = state::collect_snapshot();
    assert(feature(snapshot, "user_hide").status == "matches" &&
           feature(snapshot, "user_hide").reason ==
               "registered_definitions_verified_globally_suspended" &&
           feature(snapshot, "user_hide").detail.o.at("verification_scope").s ==
               "registered_definitions" &&
           feature(snapshot, "user_hide").detail.o.at("binding_consistency").s ==
               "per_rule_sampled");
    hides.front().management = SUMH_USER_HIDE_LIVE;
    hides.front().binding = SUMH_USER_HIDE_PENDING;
    assert(feature(state::collect_snapshot(), "user_hide").reason ==
           "definitions_registered_binding_pending");
    hides.front().binding = SUMH_USER_HIDE_ERROR;
    assert(feature(state::collect_snapshot(), "user_hide").status == "drift");
    assert(write_calls == 0 && descriptors.empty());
}
void test_journal_and_capture() {
    reset();
    std::string error;
    assert(state::command_needs_journal({"debug", "stealth", "disable"}));
    assert(state::command_needs_journal({"sumh", "kernel-build", "reset"}));
    for (const auto& args :
         std::vector<std::vector<std::string>>{{"module", "unknown"},
                                               {"module", "list"},
                                               {"sumh", "maps", "list-paths"},
                                               {"sumh", "kernel-build", "show"},
                                               {"sumh", "kernel-build", "unrecognized"},
                                               {"config", "sync-partitions"},
                                               {"api", "state"}})
        assert(!state::command_needs_journal(args));
    {
        state::Journal first;
        assert(first.begin("config apply", error));
        state::Journal second;
        assert(!second.begin("config apply", error) && errno == EWOULDBLOCK);
    }
    state::Ledger ledger;
    assert(state::parse_ledger(files.at(key("state_ledger.json")).content, ledger, error));
    assert(ledger.last.phase == "started");  // Simulated process exit without finish.
    auto json = state::state_json(state::collect_snapshot(), &ledger, "valid", {});
    assert(json.o.at("history").o.at("last_operation").o.at("result").s == "outcome_unknown");
    auto result = run_command_capture({"config", "apply"});
    assert(result.exit_code == 0 && command_calls == 1);
    assert(state::parse_ledger(files.at(key("state_ledger.json")).content, ledger, error));
    assert(ledger.last.phase == "finished" && ledger.verified.count("kernel_debug"));
    reset();
    command_result = 1;
    result = run_command_capture({"config", "apply"});
    assert(result.exit_code == 1 && result.error_number == EIO && command_calls == 1);
    reset();
    fail_write = 1;
    result = run_command_capture({"config", "apply"});
    assert(result.exit_code != 0 && command_calls == 0 &&
           result.stderr_text.find("not started") != std::string::npos);
    reset();
    fail_write = 1;
    fail_after_replace = true;
    result = run_command_capture({"config", "apply"});
    assert(result.exit_code != 0 && command_calls == 0);
    assert(state::parse_ledger(files.at(key("state_ledger.json")).content, ledger, error) &&
           ledger.last.phase == "started");
    reset();
    fail_write = 2;
    result = run_command_capture({"config", "apply"});
    assert(result.exit_code != 0 && command_calls == 1 &&
           result.stderr_text.find("persistence unconfirmed") != std::string::npos);
    assert(state::parse_ledger(files.at(key("state_ledger.json")).content, ledger, error) &&
           ledger.last.phase == "started");
    reset();
    command_throws = true;
    result = run_command_capture({"config", "apply"});
    assert(result.exit_code == 1 && command_calls == 1);
    assert(state::parse_ledger(files.at(key("state_ledger.json")).content, ledger, error) &&
           ledger.last.phase == "finished" && ledger.last.exit_code == 1);
    reset();
    prepare_throws = true;
    result = run_command_capture({"config", "apply"});
    assert(result.exit_code == 1 && command_calls == 0 && write_calls == 0 &&
           !files.count(key("state_ledger.json")));
    reset();
    put("state_ledger.json", "future schema bytes");
    result = run_command_capture({"config", "apply"});
    assert(command_calls == 0 && write_calls == 0 && result.exit_code != 0);
    assert(files.at(key("state_ledger.json")).content == "future schema bytes");
    reset();
    boot_id.clear();
    result = run_command_capture({"config", "apply"});
    assert(command_calls == 0 && result.exit_code != 0);
    reset();
    fail_random = true;
    result = run_command_capture({"config", "apply"});
    assert(command_calls == 0 && result.exit_code != 0);
    reset();
    result = run_command_capture({"api", "state"});
    assert(write_calls == 0 && command_calls == 1 && result.exit_code == 0);
    reset();
    files[key("config.json")].mode = 2;
    assert(feature(state::collect_snapshot(), "kernel_debug").status == "unknown");
    reset();
    files[key("config.json")].links = 2;
    assert(feature(state::collect_snapshot(), "kernel_debug").status == "unknown");
    reset();
    fail_read = true;
    assert(feature(state::collect_snapshot(), "kernel_debug").status == "unknown");
    assert(descriptors.empty());
}
void test_maps_reconciliation() {
    reset();
    inventory_supported = true;
    maps_snapshot.stable = maps_snapshot.definitions_complete = true;
    maps_snapshot.namespace_ino = 33;
    put("config.json", R"({"enable_maps_spoof":false,"enable_stealth":false})");
    const auto disabled = state::collect_snapshot();
    assert(feature(disabled, "maps_rules").status == "matches");
    put("config.json", R"({"enable_maps_spoof":true,"enable_stealth":false})");
    maps_snapshot.identities_sampled = true;
    auto snapshot = state::collect_snapshot();
    assert(feature(snapshot, "maps_rules").status == "matches");
    assert(feature(disabled, "maps_rules").desired_revision !=
           feature(snapshot, "maps_rules").desired_revision);
    put("config.json",
        R"({"enable_maps_spoof":true,"maps_rules":[{"target_path":"/target","replacement_path":"/replacement"}]})");
    maps_snapshot.paths = {{{11, 1, mount::sumh::MapsIdentityKind::Regular, 0},
                            {22, 1, mount::sumh::MapsIdentityKind::Regular, 0},
                            false,
                            true}};
    sumh::InventoryEntry record;
    record.target_ino = 11;
    record.target_dev = 1;
    record.spoofed_ino = 22;
    record.spoofed_dev = 1;
    record.path2 = "/replacement";
    maps_snapshot.registered = {record};
    snapshot = state::collect_snapshot();
    assert(feature(snapshot, "maps_rules").status == "matches");
    assert(feature(snapshot, "maps_rules").detail.o.at("identity_consistency").s ==
           "twice_sampled_not_pinned");
    state::Ledger ledger;
    ledger.last = {std::string(32, 'a'), "maps apply", "started", "boot-a", 0, 0, {}};
    state::finish_ledger(ledger, snapshot, 0, 0, {});
    assert(ledger.verified.count("maps_rules"));
    maps_snapshot.paths.front().target.ino = 12;
    snapshot = state::collect_snapshot();
    assert(feature(snapshot, "maps_rules").status == "drift");
    assert(!state::state_json(snapshot, &ledger, "valid", {})
                .o.at("features")
                .o.at("maps_rules")
                .o.at("applied_revision")
                .is_string());
    maps_snapshot.stable = false;
    assert(feature(state::collect_snapshot(), "maps_rules").status == "unknown");
    maps_snapshot.stable = true;
    maps_snapshot.paths.front().target.ino = 11;
    kernel_reads = 0;
    flip_generation = true;
    snapshot = state::collect_snapshot();
    assert(feature(snapshot, "maps_rules").status == "unknown" &&
           feature(snapshot, "maps_rules").reason == "runtime_changed_during_sample");
    assert(write_calls == 0);
}

int extended_checks = 0;
void check_at(bool condition, const char* expression, int line) {
    ++extended_checks;
    if (!condition)
        std::cerr << "Contract check " << extended_checks << " failed at line " << line << ": "
                  << expression << '\n';
    assert(condition);
}
#define check(...) check_at(static_cast<bool>(__VA_ARGS__), #__VA_ARGS__, __LINE__)
state::Operation operation(unsigned number, const std::string& phase = "finished") {
    std::ostringstream id;
    id << std::hex << std::setfill('0') << std::setw(32) << number;
    return {id.str(), "config apply", phase, "boot-a", 0, 0, {}};
}
state::Ledger sample_ledger() {
    state::Ledger ledger;
    ledger.last = operation(100);
    ledger.history = {operation(1), operation(2, "started")};
    return ledger;
}
JsonValue decoded(const std::string& bytes) {
    JsonValue value;
    std::string error;
    assert(parse_json(bytes, value, error));
    return value;
}
std::string schema_one_bytes() {
    auto value = decoded(state::encode_ledger(sample_ledger()));
    value.o["schema_version"] = 1;
    value.o.erase("history");
    value.o.erase("history_pruned");
    value.o["last_operation"].o.erase("request_revision");
    return " \n" + stringify_json(value) + "\n  ";
}
std::string schema_two_bytes() {
    auto value = decoded(state::encode_ledger(sample_ledger()));
    value.o["schema_version"] = 2;
    value.o["last_operation"].o.erase("request_revision");
    for (auto& item : value.o["history"].a)
        item.o.erase("request_revision");
    return " \n" + stringify_json(value) + "\n  ";
}
void test_history_model() {
    reset();
    std::string error;
    auto original = sample_ledger();
    state::Ledger parsed;
    check(state::parse_ledger(state::encode_ledger(original), parsed, error));
    check(parsed.source_schema == 3 && !parsed.history_pruned && parsed.history.size() == 2 &&
          parsed.history.front().id == operation(1).id && parsed.history.back().phase == "started");
    check(state::parse_ledger(schema_one_bytes(), parsed, error) && parsed.source_schema == 1 &&
          parsed.history.empty() && parsed.history_pruned && parsed.last.id == operation(100).id);
    const auto saved = state::encode_ledger(parsed);
    const auto rejected = [&](JsonValue malformed) {
        check(!state::parse_ledger(stringify_json(malformed), parsed, error) &&
              state::encode_ledger(parsed) == saved);
    };
    for (const auto& id :
         {std::string{}, std::string(31, 'a'), std::string(33, 'a'), std::string(32, 'A'),
          std::string(31, 'a') + "g", std::string(31, 'a') + "\n"})
        check(!state::valid_operation_id(id));
    check(state::valid_operation_id(std::string(32, '0')) &&
          state::valid_operation_id(std::string(32, 'f')));
    auto value = decoded(state::encode_ledger(original));
    auto malformed = value;
    malformed.o["schema_version"] = 4;
    rejected(malformed);
    malformed = value;
    malformed.o["future_field"] = true;
    rejected(malformed);
    malformed = value;
    malformed.o.erase("history");
    rejected(malformed);
    malformed = value;
    malformed.o["history_pruned"] = "false";
    rejected(malformed);
    malformed = value;
    malformed.o["history"] = JsonValue::object();
    rejected(malformed);
    malformed = value;
    malformed.o["history"].a.push_back(malformed.o.at("last_operation"));
    rejected(malformed);
    malformed = value;
    malformed.o["history"].a.push_back(malformed.o["history"].a.front());
    rejected(malformed);
    malformed = value;
    malformed.o["history"].a.front().o["phase"] = "queued";
    rejected(malformed);
    malformed = value;
    malformed.o["last_operation"].o["exit_code"] = -1;
    rejected(malformed);
    malformed = value;
    malformed.o["last_operation"].o["exit_code"] = 256;
    rejected(malformed);
    malformed = value;
    malformed.o["last_operation"].o["errno"] = 4096;
    rejected(malformed);
    malformed = value;
    malformed.o["last_operation"].o["name"] = std::string(81, 'x');
    rejected(malformed);
    malformed = value;
    malformed.o["last_operation"].o["detail"] = std::string(1025, 'x');
    rejected(malformed);
    malformed = value;
    malformed.o["last_operation"].o["boot_id"] = "";
    rejected(malformed);
    malformed = value;
    malformed.o["history"].a.front().o["extra"] = true;
    rejected(malformed);
    malformed = value;
    malformed.o["last_operation"].o["detail"] = std::string("invalid\xc0\xaf", 9);
    rejected(malformed);
    const auto bytes = state::encode_ledger(original);
    check(!state::parse_ledger("{\"schema_version\":2," + bytes.substr(1), parsed, error));
    auto duplicate_nested = bytes;
    const auto id_at = duplicate_nested.find("\"id\":");
    assert(id_at != std::string::npos);
    duplicate_nested.insert(id_at, "\"id\":\"" + operation(999).id + "\",");
    check(!state::parse_ledger(duplicate_nested, parsed, error));
    original.history.clear();
    for (unsigned n = 1; n < state::LEDGER_MAX_OPERATIONS; ++n)
        original.history.push_back(operation(n));
    check(state::parse_ledger(state::encode_ledger(original), parsed, error));
    original.history.push_back(operation(50));
    check(!state::parse_ledger(state::encode_ledger(original), parsed, error));
    state::trim_ledger_history(original);
    check(original.history.size() == 15 && original.history.front().id == operation(2).id &&
          original.history.back().id == operation(50).id && original.last.id == operation(100).id &&
          original.history_pruned);
    original = sample_ledger();
    for (unsigned n = 3; n <= 15; ++n)
        original.history.push_back(operation(n));
    for (auto& item : original.history)
        item.detail = std::string(1024, '\x01');
    original.last.detail = std::string(1024, '\x01');
    check(state::encode_ledger(original).size() > state::LEDGER_MAX_BYTES);
    const auto newest = original.history.back().id;
    state::trim_ledger_history(original);
    check(state::encode_ledger(original).size() <= state::LEDGER_MAX_BYTES &&
          original.history_pruned && original.history.back().id == newest);
    check(state::parse_ledger(state::encode_ledger(original), parsed, error));
    const auto unknown = state::operation_record_json(operation(1, "started"), "boot-a");
    check(unknown.o.at("result").s == "outcome_unknown" && unknown.o.at("same_boot").b &&
          !unknown.o.at("exit_code").is_number() && !unknown.o.at("errno").is_number());
    check(state::operation_record_json(operation(2), "boot-b").o.at("same_boot").b == false);
    check(!state::operation_record_json(operation(2), "").o.at("same_boot").is_bool());
    check(state::operation_record_json(operation(2), "boot-a").o.at("result").s ==
          "command_completed");
    auto failed = operation(3);
    failed.exit_code = 1;
    failed.error_number = EIO;
    check(state::operation_record_json(failed, "boot-a").o.at("result").s == "failed_or_partial");
    check(write_calls == 0 && prepare_calls == 0 && kernel_reads == 0);
}

void test_history_persistence() {
    reset();
    std::string error;
    state::Ledger observed;
    const auto legacy = schema_one_bytes();
    put("state_ledger.json", legacy);
    check(state::read_operation_history(observed, error) == "valid" && observed.source_schema == 1);
    check(write_calls == 0 && prepare_calls == 0 && kernel_reads == 0 &&
          !files.count(key("state_ledger.lock")) &&
          files.at(key("state_ledger.json")).content == legacy);
    {
        state::Journal journal;
        check(journal.operation_id().empty());
        check(journal.begin("config apply", error) &&
              state::valid_operation_id(journal.operation_id()));
        check(written.size() == 2 && written.front().second == legacy &&
              written.front().first != key("state_ledger.json") &&
              written.back().first == key("state_ledger.json"));
        check(state::read_operation_history(observed, error) == "valid" &&
              observed.source_schema == 3 && observed.history_pruned &&
              observed.history.size() == 1 && observed.history.front().id == operation(100).id &&
              observed.last.phase == "started" && observed.last.id == journal.operation_id());
        check(journal.finish(0, 0, "completed", error));
    }
    check(descriptors.empty());
    // A start without a finish remains queryable after a later command.
    reset();
    std::string interrupted;
    {
        state::Journal journal;
        check(journal.begin("first", error));
        interrupted = journal.operation_id();
    }
    {
        state::Journal journal;
        check(journal.begin("second", error));
        check(journal.finish(0, 0, "completed", error));
    }
    check(state::read_operation_history(observed, error) == "valid" &&
          observed.history.size() == 1 && observed.history.front().id == interrupted &&
          observed.history.front().phase == "started");
    for (unsigned n = 0; n < 20; ++n) {
        state::Journal journal;
        assert(journal.begin("later", error));
        assert(journal.finish(0, 0, "completed", error));
    }
    check(state::read_operation_history(observed, error) == "valid" && observed.history_pruned &&
          observed.history.size() == 15);
    check(std::none_of(observed.history.begin(), observed.history.end(),
                       [&](const auto& item) { return item.id == interrupted; }));
    // Backup, begin and finish persistence failures never run/replay an unstarted command.
    for (int failure = 1; failure <= 3; ++failure) {
        reset();
        put("state_ledger.json", legacy);
        fail_write = failure;
        const auto result = run_command_capture({"config", "apply"});
        check(result.exit_code != 0 && command_calls == (failure == 3 ? 1 : 0));
        check(state::read_operation_history(observed, error) == "valid");
        check(failure == 3 ? observed.source_schema == 3 && observed.last.phase == "started"
                           : files.at(key("state_ledger.json")).content == legacy);
        check(!result.operation_recorded &&
              (failure == 3 ? state::valid_operation_id(result.operation_id)
                            : result.operation_id.empty()));
    }
    reset();
    put("state_ledger.json", legacy);
    fail_write = 1;
    fail_after_replace = true;
    auto result = run_command_capture({"config", "apply"});
    check(result.exit_code != 0 && command_calls == 0 &&
          files.at(key("state_ledger.json")).content == legacy);
    reset();
    corrupt_readback_write = 1;
    result = run_command_capture({"config", "apply"});
    check(result.exit_code != 0 && command_calls == 0 && !result.operation_recorded &&
          result.operation_id.empty());
    reset();
    result = run_command_capture({"config", "apply"});
    check(result.exit_code == 0 && result.operation_recorded &&
          state::valid_operation_id(result.operation_id));
    check(state::read_operation_history(observed, error) == "valid" &&
          observed.last.id == result.operation_id);
    reset();
    result = run_command_capture({"api", "state"});
    check(!result.operation_recorded && result.operation_id.empty() && write_calls == 0);
    for (int bad = 0; bad < 4; ++bad) {
        reset();
        auto malformed = decoded(state::encode_ledger(sample_ledger()));
        if (bad == 0)
            malformed.o["schema_version"] = 4;
        if (bad == 1)
            malformed.o["unknown"] = true;
        auto raw = stringify_json(malformed);
        if (bad == 2)
            raw = "{\"schema_version\":2," + raw.substr(1);
        if (bad == 3)
            raw = std::string(state::LEDGER_MAX_BYTES + 1, 'x');
        put("state_ledger.json", raw);
        result = run_command_capture({"config", "apply"});
        check(result.exit_code != 0 && command_calls == 0 && write_calls == 0 &&
              files.at(key("state_ledger.json")).content == raw);
    }
    reset();
    check(state::read_operation_history(observed, error) == "missing" && write_calls == 0 &&
          prepare_calls == 0);
    for (int bad = 0; bad < 3; ++bad) {
        reset();
        put("state_ledger.json", state::encode_ledger(sample_ledger()));
        if (bad == 0)
            files[key("state_ledger.json")].mode = 2;
        if (bad == 1)
            files[key("state_ledger.json")].links = 2;
        if (bad == 2)
            fail_read = true;
        check(state::read_operation_history(observed, error) == "unreadable" && write_calls == 0 &&
              prepare_calls == 0 && kernel_reads == 0);
        check(descriptors.empty());
    }
    // An identical existing backup is re-synced; a conflicting or unsafe backup
    // blocks migration without modifying either document.
    reset();
    put("state_ledger.json", legacy);
    put("state_ledger.schema1.bak", legacy);
    result = run_command_capture({"config", "apply"});
    check(result.exit_code == 0 && result.operation_recorded && write_calls == 3 &&
          written.front().first == key("state_ledger.schema1.bak") &&
          written.front().second == legacy);
    for (int bad = 0; bad < 3; ++bad) {
        reset();
        put("state_ledger.json", legacy);
        put("state_ledger.schema1.bak", bad == 0 ? "different original" : legacy);
        if (bad == 1)
            files[key("state_ledger.schema1.bak")].mode = 2;
        if (bad == 2)
            files[key("state_ledger.schema1.bak")].links = 2;
        const auto backup = files.at(key("state_ledger.schema1.bak")).content;
        result = run_command_capture({"config", "apply"});
        check(result.exit_code != 0 && command_calls == 0 && write_calls == 0 &&
              result.operation_id.empty());
        check(files.at(key("state_ledger.json")).content == legacy &&
              files.at(key("state_ledger.schema1.bak")).content == backup);
    }
    reset();
    put("state_ledger.json", legacy);
    corrupt_readback_write = 1;
    result = run_command_capture({"config", "apply"});
    check(result.exit_code != 0 && command_calls == 0 && write_calls == 1 &&
          files.at(key("state_ledger.json")).content == legacy);
    for (int failure = 2; failure <= 3; ++failure) {
        reset();
        put("state_ledger.json", legacy);
        fail_write = failure;
        fail_after_replace = true;
        result = run_command_capture({"config", "apply"});
        check(result.exit_code != 0 && command_calls == (failure == 3 ? 1 : 0) &&
              !result.operation_recorded);
        check(state::read_operation_history(observed, error) == "valid" &&
              observed.last.phase == (failure == 3 ? "finished" : "started"));
        check(failure == 3 ? state::valid_operation_id(result.operation_id)
                           : result.operation_id.empty());
    }
    reset();
    corrupt_readback_write = 2;
    result = run_command_capture({"config", "apply"});
    check(result.exit_code != 0 && command_calls == 1 && !result.operation_recorded &&
          state::valid_operation_id(result.operation_id));
    reset();
    command_throws = true;
    result = run_command_capture({"config", "apply"});
    check(result.exit_code == 1 && command_calls == 1 && result.operation_recorded &&
          state::valid_operation_id(result.operation_id));
    check(state::read_operation_history(observed, error) == "valid" &&
          observed.last.id == result.operation_id && observed.last.phase == "finished" &&
          observed.last.exit_code == 1);
    // One Journal instance never replays begin or finish, including after a
    // failed durable write. Identity becomes observable only after begin.
    reset();
    {
        state::Journal journal;
        check(!journal.finish(0, 0, "early", error) && write_calls == 0 && kernel_reads == 0 &&
              journal.operation_id().empty());
        check(journal.begin("once", error));
        const auto id = journal.operation_id();
        const auto writes = write_calls;
        check(!journal.begin("again", error) && errno == EALREADY && write_calls == writes &&
              journal.operation_id() == id);
        check(journal.finish(0, 0, "completed", error));
        const auto completed_writes = write_calls, completed_reads = kernel_reads;
        check(!journal.finish(0, 0, "again", error) && write_calls == completed_writes &&
              kernel_reads == completed_reads && journal.operation_id() == id);
    }
    reset();
    fail_write = 1;
    {
        state::Journal journal;
        check(!journal.begin("once", error) && journal.operation_id().empty());
        check(!journal.begin("again", error) && write_calls == 1);
        check(!journal.finish(0, 0, "again", error) && write_calls == 1 && kernel_reads == 0);
    }
    reset();
    {
        state::Journal journal;
        assert(journal.begin("first", error));
    }
    const auto before_collision = files.at(key("state_ledger.json")).content;
    random_calls = 0;
    const auto before_writes = write_calls;
    result = run_command_capture({"config", "apply"});
    check(result.exit_code != 0 && command_calls == 0 && write_calls == before_writes &&
          result.operation_id.empty() &&
          files.at(key("state_ledger.json")).content == before_collision);
}

std::pair<int, JsonValue> query(const std::vector<std::string>& args) {
    state::OperationQueryRequest request;
    std::string error;
    assert(state::parse_operation_query_arguments(args, request, error));
    const auto before_writes = write_calls, before_prepare = prepare_calls,
               before_kernel = kernel_reads;
    const auto before_commands = command_calls, before_logs = log_calls;
    const auto before_lock = held_lock;
    const auto before_descriptors = descriptors.size();
    const auto before_open = opened.size();
    std::ostringstream output, errors;
    auto* old_output = std::cout.rdbuf(output.rdbuf());
    auto* old_errors = std::cerr.rdbuf(errors.rdbuf());
    const auto exit = state::print_operation_query(request);
    std::cout.rdbuf(old_output);
    std::cerr.rdbuf(old_errors);
    check(write_calls == before_writes && prepare_calls == before_prepare &&
          kernel_reads == before_kernel && command_calls == before_commands &&
          log_calls == before_logs && held_lock == before_lock &&
          descriptors.size() == before_descriptors);
    check(std::all_of(opened.begin() + before_open, opened.end(), [&](const auto& item) {
        return (item.first == key("state_ledger.json") ||
                (args.size() > 1 && args[1] == "result" &&
                 item.first == key("operation_resources.json"))) &&
               !(item.second & test_creat) && !(item.second & test_rdwr);
    }));
    check(errors.str().empty());
    auto value = decoded(output.str());
    const bool resource = args.size() > 1 && args[1] == "result";
    check(value.o.at("schema_version").n == (resource ? 1 : 2) && value.o.at("read_only").b &&
          !value.o.at("runtime_verified").b && (resource || !value.o.at("history_complete").b) &&
          value.o.at("durability_confirmation").s == "not_observable_from_read");
    return {exit, std::move(value)};
}
void test_operation_query_input() {
    reset();
    state::OperationQueryRequest request;
    std::string error;
    const auto id = operation(1).id;
    check(state::parse_operation_query_arguments({"operation", "list"}, request, error) &&
          request.kind == state::OperationQueryRequest::Kind::List && request.id.empty() &&
          error.empty());
    check(state::parse_operation_query_arguments({"operation", "show", id}, request, error) &&
          request.kind == state::OperationQueryRequest::Kind::Show && request.id == id &&
          request.timeout_ms == 0);
    for (const auto& duration : {"0", "1", "99", "100", "9999", "10000"})
        check(state::parse_operation_query_arguments({"operation", "wait", id, duration}, request,
                                                     error) &&
              request.kind == state::OperationQueryRequest::Kind::Wait &&
              request.timeout_ms == std::stoul(duration));
    const auto preserved = request;
    auto rejected = [&](const std::vector<std::string>& args) {
        check(!state::parse_operation_query_arguments(args, request, error) && !error.empty() &&
              request.kind == preserved.kind && request.id == preserved.id &&
              request.timeout_ms == preserved.timeout_ms);
    };
    for (const auto& args : std::vector<std::vector<std::string>>{
             {},
             {"operation"},
             {"operation", "list", "extra"},
             {"operation", "show"},
             {"operations", "show", id},
             {"operation", "SHOW", id},
             {"operation", "show", id, "extra"},
             {"operation", "wait", id},
             {"operation", "wait", id, "0", "extra"},
             {"operation", "show", std::string(32, 'A')},
             {"operation", "show", std::string(31, 'a')},
             {"operation", "show", std::string(33, 'a')},
             {"operation", "show", id + "\n"},
             {"operation", "show", std::string(31, 'a') + std::string(1, '\0')},
             {"operation", "retry", id}})
        rejected(args);
    for (const auto& duration : {"", "00", "01", "+1", "-1", " 1", "1 ", "1\n", "0.5", "10001",
                                 "99999", "100000", "4294967296"})
        rejected({"operation", "wait", id, duration});
    check(write_calls == 0 && opened.empty() && prepare_calls == 0);
}
void test_operation_queries() {
    reset();
    auto ledger = sample_ledger();
    put("state_ledger.json", state::encode_ledger(ledger));
    auto result = query({"operation", "list"});
    check(result.first == 0 && result.second.o.at("status").s == "observed" &&
          result.second.o.at("records").a.size() == 3);
    check(result.second.o.at("records").a.front().o.at("id").s == operation(100).id &&
          result.second.o.at("records").a.back().o.at("id").s == operation(1).id);
    result = query({"operation", "show", operation(2).id});
    check(result.first == 0 && result.second.o.at("records").a.size() == 1 &&
          result.second.o.at("records").a.front().o.at("result").s == "outcome_unknown");
    result = query({"operation", "show", operation(100).id});
    check(result.first == 0 &&
          result.second.o.at("records").a.front().o.at("result").s == "command_completed");
    result = query({"operation", "show", operation(999).id});
    check(result.first == 3 && result.second.o.at("status").s == "not_retained" &&
          result.second.o.at("records").a.empty());
    reset();
    const auto legacy = schema_one_bytes();
    put("state_ledger.json", legacy);
    result = query({"operation", "list"});
    check(result.first == 0 && result.second.o.at("history_pruned").b &&
          result.second.o.at("records").a.size() == 1 &&
          files.at(key("state_ledger.json")).content == legacy && files.size() == 2);
    reset();
    result = query({"operation", "list"});
    check(result.first == 0 && result.second.o.at("ledger_status").s == "missing" &&
          result.second.o.at("records").a.empty());
    result = query({"operation", "show", operation(1).id});
    check(result.first == 3 && result.second.o.at("ledger_status").s == "missing");
    reset();
    put("state_ledger.json", "broken");
    result = query({"operation", "list"});
    check(result.first == 5 && result.second.o.at("status").s == "unavailable" &&
          result.second.o.at("ledger_status").s == "corrupt_or_unsupported");
    reset();
    put("state_ledger.json", state::encode_ledger(ledger));
    fail_read = true;
    result = query({"operation", "list"});
    check(result.first == 5 && result.second.o.at("ledger_status").s == "unreadable");
    reset();
    put("state_ledger.json", state::encode_ledger(ledger));
    boot_id.clear();
    result = query({"operation", "list"});
    check(result.first == 5 && result.second.o.at("status").s == "context_changed_or_unknown");
    reset();
    put("state_ledger.json", state::encode_ledger(ledger));
    boot_id = "next-boot";
    result = query({"operation", "show", operation(2).id});
    check(result.first == 0 && !result.second.o.at("records").a.front().o.at("same_boot").b &&
          result.second.o.at("records").a.front().o.at("result").s == "outcome_unknown");
    reset();
    ledger.last = operation(100, "started");
    put("state_ledger.json", state::encode_ledger(ledger));
    result = query({"operation", "wait", operation(100).id, "0"});
    check(result.first == 4 && result.second.o.at("status").s == "wait_timeout" &&
          fixture_clock::sleeps == 0 && opened.size() == 1);
    reset();
    put("state_ledger.json", state::encode_ledger(ledger));
    result = query({"operation", "wait", operation(100).id, "250"});
    check(result.first == 4 && fixture_clock::sleeps == 3 && opened.size() == 4 &&
          result.second.o.at("elapsed_ms").n == 250);
    reset();
    put("state_ledger.json", state::encode_ledger(ledger));
    result = query({"operation", "wait", operation(100).id, "10000"});
    check(result.first == 4 && fixture_clock::sleeps == 100 && opened.size() == 101 &&
          result.second.o.at("elapsed_ms").n == 10000);
    reset();
    put("state_ledger.json", state::encode_ledger(ledger));
    fixture_clock::on_sleep = [&](int count) {
        if (count == 2) {
            ledger.last.phase = "finished";
            ledger.last.exit_code = 1;
            put("state_ledger.json", state::encode_ledger(ledger));
        }
    };
    result = query({"operation", "wait", operation(100).id, "1000"});
    check(result.first == 0 && fixture_clock::sleeps == 2 &&
          result.second.o.at("records").a.front().o.at("result").s == "failed_or_partial");
    reset();
    ledger.last = operation(100, "started");
    put("state_ledger.json", state::encode_ledger(ledger));
    fixture_clock::on_sleep = [&](int) {
        ledger.last = operation(101);
        put("state_ledger.json", state::encode_ledger(ledger));
    };
    result = query({"operation", "wait", operation(100).id, "1000"});
    check(result.first == 3 && fixture_clock::sleeps == 1 &&
          result.second.o.at("status").s == "not_retained");
    reset();
    ledger.last = operation(100, "started");
    put("state_ledger.json", state::encode_ledger(ledger));
    fixture_clock::on_sleep = [](int) { fail_read = true; };
    result = query({"operation", "wait", operation(100).id, "1000"});
    check(result.first == 5 && fixture_clock::sleeps == 1 &&
          result.second.o.at("status").s == "unavailable");
    reset();
    put("state_ledger.json", state::encode_ledger(ledger));
    fixture_clock::on_sleep = [](int) { boot_id = "next-boot"; };
    result = query({"operation", "wait", operation(100).id, "1000"});
    check(result.first == 5 && fixture_clock::sleeps == 1 &&
          result.second.o.at("status").s == "context_changed_or_unknown");
    reset();
    put("state_ledger.json", state::encode_ledger(ledger));
    interrupted_reads = 10000;
    result = query({"operation", "wait", operation(100).id, "10000"});
    check(result.first == 5 && fixture_clock::sleeps == 0 && interrupted_reads > 9900 &&
          result.second.o.at("error").s == "snapshot read budget exceeded");
    reset();
    std::string error;
    {
        state::Journal journal;
        assert(journal.begin("running", error));
        result = query({"operation", "show", journal.operation_id()});
        check(result.first == 0 && held_lock >= 0 &&
              result.second.o.at("records").a.front().o.at("result").s == "outcome_unknown");
    }
    check(descriptors.empty());
}

const state::Operation* retained(const state::Ledger& ledger, const std::string& id) {
    if (ledger.last.id == id)
        return &ledger.last;
    for (const auto& item : ledger.history)
        if (item.id == id)
            return &item;
    return nullptr;
}
std::string reservation(const std::string& revision, const std::string& name = "config apply") {
    state::Journal journal;
    std::string error;
    assert(journal.prepare(name, revision, error));
    return journal.operation_id();
}
void test_reserved_model() {
    reset();
    std::string error;
    const auto revision = state::revision_for_bytes("reserved input");
    auto ledger = sample_ledger();
    ledger.last.phase = "prepared";
    ledger.last.request_revision = revision;
    auto value = decoded(state::encode_ledger(ledger));
    state::Ledger parsed;
    check(value.o.at("schema_version").n == 3 &&
          state::parse_ledger(state::encode_ledger(ledger), parsed, error));
    check(parsed.last.request_revision == revision && parsed.last.phase == "prepared");
    const auto encoded = state::encode_ledger(parsed);
    const auto reject = [&](JsonValue bad) {
        check(!state::parse_ledger(stringify_json(bad), parsed, error) &&
              state::encode_ledger(parsed) == encoded);
    };
    auto bad = value;
    bad.o["last_operation"].o.erase("request_revision");
    reject(bad);
    bad = value;
    bad.o["last_operation"].o["request_revision"] = "";
    reject(bad);
    bad = value;
    bad.o["last_operation"].o["request_revision"] = JsonValue{};
    reject(bad);
    bad = value;
    bad.o["last_operation"].o["request_revision"] = "sha256-v1:" + std::string(64, 'A');
    reject(bad);
    bad = value;
    bad.o["last_operation"].o["exit_code"] = 1;
    reject(bad);
    bad = value;
    bad.o["last_operation"].o["errno"] = EIO;
    reject(bad);
    bad = value;
    bad.o["last_operation"].o["detail"] = "claimed result";
    reject(bad);
    bad = value;
    bad.o["history"].a.front().o["request_revision"] = "invalid";
    reject(bad);
    bad = value;
    bad.o["history"].a.front().o.erase("request_revision");
    reject(bad);
    auto legacy = decoded(schema_two_bytes());
    legacy.o["last_operation"].o["phase"] = "prepared";
    reject(legacy);
    legacy = decoded(schema_two_bytes());
    legacy.o["last_operation"].o["request_revision"] = "";
    reject(legacy);
    check(state::parse_ledger(schema_two_bytes(), parsed, error) && parsed.source_schema == 2 &&
          parsed.history.size() == 2 && !parsed.history_pruned &&
          parsed.last.request_revision.empty() && parsed.history.front().request_revision.empty());
    auto prepared = state::operation_record_json(ledger.last, "boot-a");
    check(prepared.o.at("result").s == "not_started" &&
          prepared.o.at("request_revision").s == revision &&
          !prepared.o.at("exit_code").is_number() && !prepared.o.at("errno").is_number());
    auto legacy_record = state::operation_record_json(operation(1), "boot-a");
    check(!legacy_record.o.at("request_revision").is_string());
    state::Snapshot snapshot;
    snapshot.boot_id = "boot-a";
    auto projection = state::state_json(snapshot, &ledger, "valid", {}).o.at("history");
    check(projection.o.at("last_operation").o.at("id").s == operation(2).id &&
          projection.o.at("last_operation").o.at("phase").s == "started" &&
          !projection.o.at("last_operation").find("request_revision"));
    ledger.history.clear();
    projection = state::state_json(snapshot, &ledger, "valid", {}).o.at("history");
    check(projection.o.at("status").s == "valid" && !projection.o.at("last_operation").is_object());
    auto second = ledger.last;
    second.id = operation(200).id;
    ledger.history.push_back(second);
    projection = state::state_json(snapshot, &ledger, "valid", {}).o.at("history");
    check(!projection.o.at("last_operation").is_object());
    ledger.last.phase = "finished";
    projection = state::state_json(snapshot, &ledger, "valid", {}).o.at("history");
    check(projection.o.at("last_operation").o.at("result").s == "completed");
    check(write_calls == 0 && prepare_calls == 0 && kernel_reads == 0 && command_calls == 0);
}
void test_reserved_lifecycle() {
    reset();
    const auto revision = state::revision_for_bytes("reserved input");
    const auto other = state::revision_for_bytes("different input");
    std::string error, first;
    state::Ledger ledger;
    {
        state::Journal journal;
        check(!journal.observed_operation() && journal.operation_id().empty());
        check(journal.prepare("config apply", revision, error));
        first = journal.operation_id();
        check(state::valid_operation_id(first) && journal.observed_operation() &&
              journal.observed_operation()->phase == "prepared");
        check(write_calls == 1 && kernel_reads == 0 && command_calls == 0 && log_calls == 0);
        check(!journal.finish(0, 0, "invalid", error) && write_calls == 1 && kernel_reads == 0);
        check(!journal.prepare("config apply", revision, error) && write_calls == 1 &&
              errno == EALREADY);
        state::Journal competing;
        check(competing.begin_prepared(first, "config apply", revision, error) ==
                  state::ReservedStart::Failed &&
              !competing.observed_operation() && competing.operation_id().empty() &&
              errno == EWOULDBLOCK);
    }
    check(state::read_operation_history(ledger, error) == "valid" &&
          ledger.last.phase == "prepared" && ledger.last.request_revision == revision);
    const auto second = reservation(revision);
    const auto ordinary = run_command_capture({"config", "apply"});
    check(ordinary.command_executed && ordinary.operation_recorded &&
          ordinary.operation_id != first && ordinary.operation_id != second);
    auto executed = run_command_capture({"config", "apply"}, first, revision);
    check(executed.exit_code == 0 && executed.command_executed && executed.operation_recorded &&
          executed.operation_id == first && executed.operation_status == "command_completed");
    check(state::read_operation_history(ledger, error) == "valid" && ledger.last.id == first &&
          ledger.history.size() == 2 && ledger.history[0].id == second &&
          ledger.history[1].id == ordinary.operation_id &&
          retained(ledger, second)->phase == "prepared");
    executed = run_command_capture({"config", "apply"}, second, revision);
    check(executed.exit_code == 0 && executed.command_executed && executed.operation_id == second &&
          command_calls == 3);
    check(state::read_operation_history(ledger, error) == "valid" && ledger.last.id == second &&
          ledger.history.size() == 2 && ledger.history[0].id == ordinary.operation_id &&
          ledger.history[1].id == first);
    const auto writes = write_calls, calls = command_calls;
    executed = run_command_capture({"config", "apply"}, first, revision);
    check(executed.exit_code != 0 && !executed.command_executed && executed.operation_id == first &&
          !executed.operation_recorded &&
          executed.operation_status == "already_started_or_finished" && write_calls == writes &&
          command_calls == calls);
    check(decoded(executed.observed_record_json).o.at("id").s == first &&
          decoded(executed.observed_record_json).o.at("phase").s == "finished");
    executed = run_command_capture({"config", "apply"}, first, other);
    check(!executed.command_executed && executed.operation_status == "request_conflict" &&
          write_calls == writes);
    executed = run_command_capture({"hide", "apply"}, first, revision);
    check(!executed.command_executed && executed.operation_status == "request_conflict" &&
          write_calls == writes);
    executed = run_command_capture({"config", "apply"}, ordinary.operation_id, revision);
    check(!executed.command_executed && executed.operation_status == "request_conflict" &&
          write_calls == writes);
    boot_id = "boot-b";
    executed = run_command_capture({"config", "apply"}, first, revision);
    check(!executed.command_executed && executed.operation_status == "boot_mismatch" &&
          write_calls == writes);
    boot_id.clear();
    executed = run_command_capture({"config", "apply"}, first, revision);
    check(!executed.command_executed && executed.operation_status == "boot_mismatch" &&
          write_calls == writes);
    boot_id = "boot-a";
    executed = run_command_capture({"api", "state"}, first, revision);
    check(!executed.command_executed && write_calls == writes && command_calls == calls);
    reset();
    {
        state::Journal missing;
        check(missing.begin_prepared(first, "config apply", revision, error) ==
                  state::ReservedStart::NotRetained &&
              !missing.observed_operation() && missing.operation_id().empty() && write_calls == 0 &&
              !files.count(key("state_ledger.json")));
    }
    first = reservation(revision);
    {
        state::Journal journal;
        check(journal.begin_prepared(first, "config apply", revision, error) ==
                  state::ReservedStart::Started &&
              journal.operation_id() == first && journal.observed_operation()->phase == "started");
        const auto started_writes = write_calls;
        check(journal.begin_prepared(first, "config apply", revision, error) ==
                  state::ReservedStart::Failed &&
              write_calls == started_writes && errno == EALREADY);
        check(journal.finish(0, 0, "completed", error) &&
              journal.observed_operation()->phase == "finished");
        check(!journal.finish(0, 0, "again", error));
    }
    {
        state::Journal duplicate;
        check(duplicate.begin_prepared(first, "config apply", revision, error) ==
                  state::ReservedStart::Existing &&
              duplicate.operation_id().empty() && duplicate.observed_operation()->id == first);
        const auto duplicate_writes = write_calls;
        check(!duplicate.finish(0, 0, "invalid", error) && write_calls == duplicate_writes);
    }
    check(descriptors.empty());
}
void test_reserved_faults_and_migration() {
    const auto revision = state::revision_for_bytes("reserved input");
    std::string error;
    state::Ledger ledger;
    for (int schema = 1; schema <= 2; ++schema) {
        reset();
        const auto original = schema == 1 ? schema_one_bytes() : schema_two_bytes();
        put("state_ledger.json", original);
        check(state::read_operation_history(ledger, error) == "valid" &&
              ledger.source_schema == schema && write_calls == 0 && prepare_calls == 0);
        const auto old_query = query({"operation", "list"});
        check(old_query.first == 0 && files.at(key("state_ledger.json")).content == original &&
              files.size() == 2);
        const auto id = reservation(revision);
        check(write_calls == 2 && kernel_reads == 0 && command_calls == 0 &&
              files.at(key(("state_ledger.schema" + std::to_string(schema) + ".bak").c_str()))
                      .content == original);
        check(state::read_operation_history(ledger, error) == "valid" &&
              ledger.source_schema == 3 && ledger.last.id == id &&
              ledger.history.size() == static_cast<std::size_t>(schema == 1 ? 1 : 3) &&
              ledger.history_pruned == (schema == 1));
    }
    for (int failure = 1; failure <= 2; ++failure) {
        reset();
        const auto original = schema_two_bytes();
        put("state_ledger.json", original);
        fail_write = failure;
        state::Journal journal;
        check(!journal.prepare("config apply", revision, error) && journal.operation_id().empty() &&
              !journal.observed_operation());
        check(command_calls == 0 && kernel_reads == 0 &&
              files.at(key("state_ledger.json")).content == original);
    }
    reset();
    put("state_ledger.json", schema_two_bytes());
    put("state_ledger.schema2.bak", "conflicting backup");
    {
        state::Journal journal;
        check(!journal.prepare("config apply", revision, error) && write_calls == 0);
    }
    reset();
    put("state_ledger.json", schema_two_bytes());
    int ledger_opens = 0;
    const auto externally_changed = schema_two_bytes() + "\n";
    on_open = [&](const std::string& path) {
        if (path == key("state_ledger.json") && ++ledger_opens == 2)
            put("state_ledger.json", externally_changed);
    };
    {
        state::Journal journal;
        check(!journal.prepare("config apply", revision, error) && write_calls == 0 &&
              journal.operation_id().empty() &&
              files.at(key("state_ledger.json")).content == externally_changed);
    }
    on_open = {};
    for (const auto& invalid :
         {std::string{}, std::string("wrong"), std::string("sha256-v1:") + std::string(64, 'A')}) {
        reset();
        state::Journal journal;
        check(!journal.prepare("config apply", invalid, error) && write_calls == 0 &&
              kernel_reads == 0 && command_calls == 0);
    }
    for (bool replaced : {false, true}) {
        reset();
        fail_write = 1;
        fail_after_replace = replaced;
        {
            state::Journal journal;
            check(!journal.prepare("config apply", revision, error) &&
                  journal.operation_id().empty());
        }
        check(command_calls == 0 && kernel_reads == 0);
        check(replaced ? state::read_operation_history(ledger, error) == "valid" &&
                             ledger.last.phase == "prepared"
                       : !files.count(key("state_ledger.json")));
        reset();
        const auto id = reservation(revision);
        fail_write = 2;
        fail_after_replace = replaced;
        {
            state::Journal journal;
            check(journal.begin_prepared(id, "config apply", revision, error) ==
                      state::ReservedStart::Failed &&
                  journal.operation_id().empty() && !journal.observed_operation() &&
                  command_calls == 0 && kernel_reads == 0);
            check(!journal.finish(0, 0, "invalid", error) && write_calls == 2);
        }
        check(state::read_operation_history(ledger, error) == "valid" &&
              ledger.last.phase == (replaced ? "started" : "prepared"));
        fail_write = 0;
        fail_after_replace = false;
        const auto retried = run_command_capture({"config", "apply"}, id, revision);
        check(retried.command_executed == !replaced && command_calls == (replaced ? 0 : 1));
        reset();
        const auto final_id = reservation(revision);
        fail_write = 3;
        fail_after_replace = replaced;
        auto result = run_command_capture({"config", "apply"}, final_id, revision);
        check(result.command_executed && command_calls == 1 && result.operation_id == final_id &&
              !result.operation_recorded && result.operation_status == "outcome_unknown");
        check(state::read_operation_history(ledger, error) == "valid" &&
              ledger.last.phase == (replaced ? "finished" : "started"));
        fail_write = 0;
        fail_after_replace = false;
        result = run_command_capture({"config", "apply"}, final_id, revision);
        check(!result.command_executed && command_calls == 1 &&
              result.operation_status == "already_started_or_finished");
    }
    reset();
    const auto id = reservation(revision);
    corrupt_readback_write = 2;
    auto result = run_command_capture({"config", "apply"}, id, revision);
    check(!result.command_executed && !result.operation_recorded && command_calls == 0 &&
          result.operation_status == "journal_unavailable");
    corrupt_readback_write = 0;
    result = run_command_capture({"config", "apply"}, id, revision);
    check(!result.command_executed && command_calls == 0 &&
          result.operation_status == "journal_unavailable");
    reset();
    const auto interrupted = reservation(revision);
    {
        state::Journal journal;
        assert(journal.begin_prepared(interrupted, "config apply", revision, error) ==
               state::ReservedStart::Started);
    }
    result = run_command_capture({"config", "apply"}, interrupted, revision);
    check(!result.command_executed && command_calls == 0 &&
          result.operation_status == "already_started_or_finished");
    reset();
    const auto thrown = reservation(revision);
    command_throws = true;
    result = run_command_capture({"config", "apply"}, thrown, revision);
    check(result.command_executed && result.operation_recorded && result.command_exit_code == 1 &&
          result.operation_id == thrown);
    result = run_command_capture({"config", "apply"}, thrown, revision);
    check(!result.command_executed && command_calls == 1);
}
void test_reserved_retention_and_query() {
    reset();
    const auto revision = state::revision_for_bytes("reserved input");
    std::string error;
    state::Ledger ledger;
    const auto evicted = reservation(revision);
    for (int index = 0; index < 16; ++index)
        (void)reservation(revision);
    check(state::read_operation_history(ledger, error) == "valid" && ledger.history_pruned &&
          ledger.history.size() == 15 && !retained(ledger, evicted));
    const auto before = files.at(key("state_ledger.json")).content;
    const auto writes = write_calls;
    const auto result = run_command_capture({"config", "apply"}, evicted, revision);
    check(!result.command_executed && command_calls == 0 &&
          result.operation_status == "not_retained" && write_calls == writes &&
          files.at(key("state_ledger.json")).content == before);
    reset();
    const auto id = reservation(revision);
    auto observed = query({"operation", "show", id});
    check(observed.first == 0 &&
          observed.second.o.at("records").a.front().o.at("result").s == "not_started" &&
          observed.second.o.at("records").a.front().o.at("request_revision").s == revision);
    observed = query({"operation", "wait", id, "0"});
    check(observed.first == 4 &&
          observed.second.o.at("records").a.front().o.at("phase").s == "prepared");
    check(state::read_operation_history(ledger, error) == "valid");
    fixture_clock::on_sleep = [&](int count) {
        if (count == 1)
            ledger.last.phase = "started";
        if (count == 2)
            ledger.last.phase = "finished";
        put("state_ledger.json", state::encode_ledger(ledger));
    };
    observed = query({"operation", "wait", id, "1000"});
    check(observed.first == 0 && fixture_clock::sleeps == 2 &&
          observed.second.o.at("records").a.front().o.at("result").s == "command_completed");
    reset();
    const auto output_id = reservation(revision);
    command_output = std::string(state::OPERATION_OUTPUT_BYTES + 7, 'x');
    command_error_output = std::string(state::OPERATION_OUTPUT_BYTES + 5, 'y');
    const auto bounded = run_command_capture({"config", "apply"}, output_id, revision);
    check(bounded.command_executed && bounded.operation_recorded &&
          bounded.stdout_text.size() == state::OPERATION_OUTPUT_BYTES &&
          bounded.stderr_text.size() == state::OPERATION_OUTPUT_BYTES && !bounded.stdout_complete &&
          !bounded.stderr_complete && command_calls == 1);
    reset();
    command_output = std::string(state::OPERATION_OUTPUT_BYTES + 7, 'x');
    const auto legacy = run_command_capture({"config", "apply"});
    check(legacy.stdout_complete && legacy.stdout_text.size() == state::OPERATION_OUTPUT_BYTES + 7);
}
void test_hide_definition_projection() {
    reset();
    hide::Document definitions;
    definitions.rules = {{std::string(32, 'a'), "/enabled", true, "name", "note", "group"},
                         {std::string(32, 'b'), "/disabled", false, "other", "retain", "group"}};
    put("user_hide_rules.json", hide::encode_document(definitions));
    hides = {{1, 1, SUMH_USER_HIDE_LIVE, SUMH_USER_HIDE_BOUND, 0, "/enabled"}};
    auto hidden = feature(state::collect_snapshot(), "user_hide");
    check(hidden.status == "matches" && hidden.detail.o.at("enabled_count").n == 1 &&
          hidden.detail.o.at("disabled_count").n == 1);
    check(hidden.detail.o.at("revision_scope").s == "enabled_path_set_only");
    const auto desired = hidden.desired_revision;
    definitions.rules[0].name = "updated";
    definitions.rules[0].note = "metadata only";
    definitions.rules[0].group = "new group";
    put("user_hide_rules.json", hide::encode_document(definitions));
    hidden = feature(state::collect_snapshot(), "user_hide");
    check(hidden.status == "matches" && hidden.desired_revision == desired);
    std::reverse(definitions.rules.begin(), definitions.rules.end());
    put("user_hide_rules.json", hide::encode_document(definitions));
    check(feature(state::collect_snapshot(), "user_hide").desired_revision == desired);
    hides.push_back({2, 2, SUMH_USER_HIDE_LIVE, SUMH_USER_HIDE_BOUND, 0, "/disabled"});
    check(feature(state::collect_snapshot(), "user_hide").status == "drift");
    definitions.rules[0].enabled = true;
    put("user_hide_rules.json", hide::encode_document(definitions));
    hidden = feature(state::collect_snapshot(), "user_hide");
    check(hidden.status == "matches" && hidden.desired_revision != desired &&
          hidden.detail.o.at("enabled_count").n == 2);
    put("user_hide_rules.json", "{\"schema_version\":2,\"rules\":[]}");
    hidden = feature(state::collect_snapshot(), "user_hide");
    check(hidden.status == "unknown" && hidden.desired_revision.empty() &&
          hidden.reason == "saved_hide_definitions_unreadable_or_invalid");
    check(write_calls == 0 && command_calls == 0);
}
JsonValue hide_resource_fixture(const std::string& action = "disable",
                                const std::string& status = "applied", int count = 1) {
    const auto revision = state::revision_for_bytes("hide revision");
    auto v = JsonValue::object();
    v.o["schema_version"] = 1;
    v.o["resource"] = "user_hide_mutation";
    v.o["action"] = action;
    v.o["status"] = status;
    v.o["reason"] = "registration_only_binding_and_observers_separate";
    v.o["error"] = "";
    v.o["saved"] = true;
    v.o["revision"] = state::revision_for_bytes("next hide revision");
    v.o["previous_revision"] = revision;
    v.o["boot_id"] = boot_id;
    v.o["runtime_verified"] = false;
    v.o["verification_scope"] = "registered_membership_not_observer_visibility";
    v.o["affected_count"] = count;
    v.o["confirmed_count"] = action == "annotate" ? 0 : count;
    v.o["failed_count"] = 0;
    v.o["unknown_count"] = 0;
    v.o["items_complete"] = count <= 64;
    v.o["items"] = JsonValue::array();
    if (action != "annotate")
        for (int i = 0; i < std::min(count, 64); ++i) {
            auto row = JsonValue::object();
            row.o["id"] = operation(i + 1).id;
            row.o["outcome"] = "absent";
            row.o["errno"] = 0;
            row.o["membership_matches"] = true;
            row.o["runtime"] = JsonValue{};
            v.o["items"].a.push_back(row);
        }
    return v;
}
void test_operation_resources() {
    using namespace state;
    reset();
    const auto request_revision = revision_for_bytes("resource input");
    const auto revision = revision_for_bytes("hide revision");
    const auto definition_id = operation(1).id;
    const std::vector<std::string> args{"hide", "disable", definition_id, revision};
    auto op = operation(91);
    op.phase = "started";
    op.request_revision = request_revision;
    op.name = "hide disable";
    op.boot_id = boot_id;
    auto receipt = hide_resource_fixture();
    OperationResource resource;
    std::string error;
    check(make_operation_resource(op, args, stringify_json(receipt), true, 0, 0, resource, error));
    OperationResources decoded_resources;
    const auto encoded = encode_operation_resources({resource});
    check(parse_operation_resources(encoded, decoded_resources, error) &&
          stringify_json(decoded_resources.front().result) == stringify_json(receipt));
    check(!operation_resource_matches(resource, op));
    op.phase = "finished";
    op.exit_code = op.error_number = 0;
    check(operation_resource_matches(resource, op));
    auto altered = op;
    altered.request_revision = revision;
    check(!operation_resource_matches(resource, altered));
    altered = op;
    altered.boot_id = "other";
    check(!operation_resource_matches(resource, altered));
    for (const auto& change : std::vector<std::function<void(JsonValue&)>>{
             [](auto& v) { v.o["schema_version"] = 2; }, [](auto& v) { v.o["extra"] = true; },
             [](auto& v) { v.o["runtime_verified"] = true; }, [](auto& v) { v.o["saved"] = false; },
             [](auto& v) { v.o["boot_id"] = "other"; }, [](auto& v) { v.o["affected_count"] = 0; },
             [](auto& v) { v.o["confirmed_count"] = 0; },
             [](auto& v) { v.o["items_complete"] = false; },
             [](auto& v) { v.o["action"] = "enable"; },
             [](auto& v) { v.o["items"].a.front().o["id"] = std::string(32, 'f'); },
             [](auto& v) { v.o["items"].a.front().o["outcome"] = "registered_bound"; },
             [](auto& v) { v.o["items"].a.front().o["errno"] = 1; },
             [](auto& v) { v.o["previous_revision"] = "sha256-v1:" + std::string(64, 'f'); }}) {
        auto bad = receipt;
        change(bad);
        check(!make_operation_resource(op, args, stringify_json(bad), true, 0, 0, resource, error));
    }
    check(
        !make_operation_resource(op, args, stringify_json(receipt), false, 0, 0, resource, error));
    check(!make_operation_resource(op, args, stringify_json(receipt), true, 1, 5, resource, error));
    check(!make_operation_resource(op, args, stringify_json(receipt) + " {}", true, 0, 0, resource,
                                   error));
    check(!parse_operation_resources(encoded + " {}", decoded_resources, error));
    check(!parse_operation_resources(std::string(OPERATION_RESOURCE_MAX_BYTES + 1, ' '),
                                     decoded_resources, error));
    auto store = decoded(encoded);
    store.o["schema_version"] = 3;
    check(!parse_operation_resources(stringify_json(store), decoded_resources, error));
    store = decoded(encoded);
    store.o["receipts"].a.push_back(store.o["receipts"].a.front());
    check(!parse_operation_resources(stringify_json(store), decoded_resources, error));
    store = decoded(encoded);
    store.o["receipts"].a.front().o["result"].o["error"] = std::string(4097, 'x');
    check(!parse_operation_resources(stringify_json(store), decoded_resources, error));

    reset();
    const auto id = reservation(request_revision, "hide disable");
    command_output = stringify_json(receipt);
    auto execution = run_command_capture(args, id, request_revision);
    check(execution.command_executed && execution.operation_recorded && command_calls == 1 &&
          write_calls == 4);
    check(written[2].first == key("operation_resources.json") &&
          written[3].first == key("state_ledger.json"));
    const auto baseline_files = files;
    const auto writes = write_calls, kernel = kernel_reads, prepares = prepare_calls;
    auto result = query_operation_resource(id);
    check(result.exit_code == 0 && result.value.o.size() == 17 &&
          result.value.o.at("status").s == "observed" &&
          stringify_json(result.value.o.at("result")) == command_output &&
          result.value.o.at("same_boot").b);
    check(result.value.o.at("durability_confirmation").s == "not_observable_from_read" &&
          !result.value.o.at("runtime_verified").b);
    check(write_calls == writes && kernel_reads == kernel && prepare_calls == prepares);
    const auto via_cli = query({"operation", "result", id});
    check(via_cli.first == 0 && via_cli.second.o.at("resource").s == "operation_resource_result");
    execution = run_command_capture(args, id, request_revision);
    check(!execution.command_executed && command_calls == 1);
    boot_id = "next-boot";
    result = query_operation_resource(id);
    check(result.exit_code == 0 && !result.value.o.at("same_boot").b &&
          result.value.o.at("origin_boot_id").s == "boot-a");
    boot_id.clear();
    check(query_operation_resource(id).value.o.at("status").s == "context_changed_or_unknown");
    boot_id = "boot-a";
    files.erase(key("operation_resources.json"));
    result = query_operation_resource(id);
    check(result.exit_code == 6 && result.value.o.at("result").is_null());
    for (const auto& bytes :
         std::vector<std::string>{"{}", "{\"schema_version\":3,\"receipts\":[]}",
                                  std::string(OPERATION_RESOURCE_MAX_BYTES + 1, 'x')}) {
        put("operation_resources.json", bytes);
        result = query_operation_resource(id);
        check(result.exit_code == 5 && result.value.o.at("result").is_null());
    }
    files = baseline_files;
    files[key("operation_resources.json")].links = 2;
    check(query_operation_resource(id).exit_code == 5);
    files = baseline_files;
    files[key("operation_resources.json")].mode = 2;
    check(query_operation_resource(id).exit_code == 5);
    files = baseline_files;
    auto document = decoded(files[key("operation_resources.json")].content);
    document.o["receipts"].a.front().o["request_revision"] = revision;
    put("operation_resources.json", stringify_json(document));
    result = query_operation_resource(id);
    check(result.exit_code == 5 && result.value.o.at("receipt_status").s == "mismatch");
    files = baseline_files;
    on_open = [&](const auto& path) {
        if (path == key("operation_resources.json"))
            files.erase(key("state_ledger.json"));
    };
    result = query_operation_resource(id);
    check(result.exit_code == 5 && result.value.o.at("status").s == "context_changed_or_unknown" &&
          result.value.o.at("record").is_null());
    on_open = {};
    files = baseline_files;
    on_open = [&](const auto& path) {
        if (path == key("operation_resources.json"))
            boot_id = "next-boot";
    };
    check(query_operation_resource(id).value.o.at("status").s == "context_changed_or_unknown");
    on_open = {};
    boot_id = "boot-a";
    files = baseline_files;
    Ledger ledger;
    check(read_operation_history(ledger, error) == "valid");
    ledger.last.phase = "started";
    put("state_ledger.json", encode_ledger(ledger));
    check(query_operation_resource(id).value.o.at("status").s == "not_finished");
    ledger.last.phase = "prepared";
    ledger.last.detail.clear();
    put("state_ledger.json", encode_ledger(ledger));
    check(query_operation_resource(id).exit_code == 4);
    on_open = [&](const auto& path) {
        if (path == key("state_ledger.json"))
            boot_id = "next-boot";
    };
    check(query_operation_resource(id).value.o.at("status").s == "context_changed_or_unknown");
    on_open = {};
    boot_id = "boot-a";
    files = baseline_files;
    check(query_operation_resource(operation(200).id).exit_code == 3);
    check(query_operation_resource("invalid").exit_code == 5);
    put("state_ledger.json", "{}");
    check(query_operation_resource(id).exit_code == 5);

    for (int fault = 0; fault < 5; ++fault) {
        reset();
        const auto target = reservation(request_revision, "hide disable");
        command_output = stringify_json(receipt);
        if (fault < 2) {
            fail_write = 3;
            fail_after_replace = fault == 1;
        }  // Receipt write uncertainty must not suppress summary finish.
        if (fault == 2)
            corrupt_readback_write = 3;
        if (fault >= 3) {
            fail_write = 4;
            fail_after_replace = fault == 4;
        }
        execution = run_command_capture(args, target, request_revision);
        check(execution.command_executed && command_calls == 1 &&
              execution.operation_recorded == (fault < 3));
        result = query_operation_resource(target);
        check(result.value.o.at("status").s == (fault == 0   ? "resource_unavailable"
                                                : fault == 2 ? "unavailable"
                                                : fault == 3 ? "not_finished"
                                                             : "observed"));
        fail_write = 0;
        corrupt_readback_write = 0;
        execution = run_command_capture(args, target, request_revision);
        check(!execution.command_executed && command_calls == 1);
    }
    reset();
    put("operation_resources.json", "{\"schema_version\":9,\"receipts\":[]}");
    const auto future = files[key("operation_resources.json")].content;
    const auto target = reservation(request_revision, "hide disable");
    command_output = stringify_json(receipt);
    execution = run_command_capture(args, target, request_revision);
    check(execution.operation_recorded && files[key("operation_resources.json")].content == future);
    reset();
    const auto truncated = reservation(request_revision, "hide disable");
    command_output = stringify_json(receipt) + std::string(64 * 1024, ' ');
    execution = run_command_capture(args, truncated, request_revision);
    check(execution.operation_recorded && !execution.stdout_complete &&
          query_operation_resource(truncated).exit_code == 6);
    reset();
    std::string oldest, last;
    for (int i = 0; i < 18; ++i) {
        last = reservation(request_revision, "hide disable");
        if (i == 0)
            oldest = last;
        command_output = stringify_json(receipt);
        check(run_command_capture(args, last, request_revision).operation_recorded);
    }
    check(query_operation_resource(oldest).exit_code == 3 &&
          query_operation_resource(last).exit_code == 0);
    check(parse_operation_resources(files[key("operation_resources.json")].content,
                                    decoded_resources, error) &&
          decoded_resources.size() == 16);
    // Large valid partial receipts force the independent byte cap before the count cap.
    reset();
    auto large = hide_resource_fixture("disable-group", "partial", 64);
    large.o["confirmed_count"] = 0;
    large.o["failed_count"] = 64;
    large.o["reason"] = std::string(1024, '\1');
    large.o["error"] = std::string(4096, '\1');
    for (auto& row : large.o["items"].a) {
        row.o["outcome"] = "mutation_failed_or_partial";
        row.o["errno"] = 5;
    }
    const std::vector<std::string> group_args{"hide", "disable-group", "literal group", revision};
    for (int i = 0; i < 16; ++i) {
        last = reservation(request_revision, "hide disable-group");
        command_output = stringify_json(large);
        command_result = 1;
        check(run_command_capture(group_args, last, request_revision).operation_recorded);
    }
    check(parse_operation_resources(files[key("operation_resources.json")].content,
                                    decoded_resources, error) &&
          decoded_resources.size() < 16 && !decoded_resources.empty());
    check(query_operation_resource(last).exit_code == 0 &&
          query_operation_resource(last).value.o.at("result").o.at("status").s == "partial");
    // Complete metadata and zero-runtime-count failures are retained, but do
    // not invent registration work. No generic stderr or raw argv is stored.
    reset();
    const auto metadata_id = reservation(request_revision, "hide annotate");
    const std::vector<std::string> metadata_args{"hide", "annotate", definition_id, revision,
                                                 "{\"name\":\"sensitive name\"}"};
    command_output = stringify_json(hide_resource_fixture("annotate", "saved"));
    command_error_output = "sensitive diagnostic";
    check(run_command_capture(metadata_args, metadata_id, request_revision).operation_recorded);
    check(query_operation_resource(metadata_id).exit_code == 0 &&
          files[key("operation_resources.json")].content.find("sensitive") == std::string::npos);
    reset();
    const auto rejection_id = reservation(request_revision, "hide disable");
    auto rejection = hide_resource_fixture();
    rejection.o["status"] = "rejected";
    rejection.o["saved"] = false;
    rejection.o["revision"] = JsonValue{};
    rejection.o["affected_count"] = 0;
    rejection.o["confirmed_count"] = 0;
    rejection.o["items"] = JsonValue::array();
    command_output = stringify_json(rejection);
    command_result = 1;
    check(run_command_capture(args, rejection_id, request_revision).operation_recorded &&
          query_operation_resource(rejection_id).exit_code == 0);
    for (const auto& binding : std::vector<std::pair<int, int>>{
             {0, -2}, {1, -2}, {3, -13}, {2, 0}, {3, 4095}, {3, -4095}}) {
        reset();
        const auto enabled_id = reservation(request_revision, "hide enable");
        const std::vector<std::string> enable_args{"hide", "enable", definition_id, revision};
        auto enabled = hide_resource_fixture("enable");
        auto& item = enabled.o["items"].a.front();
        item.o["outcome"] = binding.first == 2   ? "registered_bound"
                            : binding.first == 3 ? "registered_binding_error"
                                                 : "registered_pending";
        auto live = JsonValue::object();
        live.o["id"] = "18446744073709551615";
        live.o["generation"] = "18446744073709551614";
        if (binding.first == 1)
            live.o["generation"] = "0";
        live.o["management"] = 0;
        live.o["binding"] = binding.first;
        live.o["errno"] = binding.second;
        item.o["runtime"] = live;
        command_output = stringify_json(enabled);
        check(run_command_capture(enable_args, enabled_id, request_revision).operation_recorded);
        result = query_operation_resource(enabled_id);
        check(result.exit_code == 0 &&
              result.value.o.at("result").o.at("items").a.front().o.at("runtime").o.at("errno").n ==
                  binding.second);
        auto value = decoded(files[key("operation_resources.json")].content);
        for (double invalid : {-4096., 4096., -0.5, 0.5}) {
            auto bad = value;
            bad.o["receipts"].a.front().o["result"].o["items"].a.front().o["runtime"].o["errno"] =
                invalid;
            check(!parse_operation_resources(stringify_json(bad), decoded_resources, error));
        }
        value.o["receipts"].a.front().o["result"].o["items"].a.front().o["errno"] = -2;
        check(!parse_operation_resources(stringify_json(value), decoded_resources, error));
    }
    const std::string imported_fields =
        "{\"path\":\"/private\\n \",\"enabled\":false,\"name\":\"private "
        "metadata\",\"note\":\"\",\"group\":\"\",\"expect\":\"absent\"}";
    const std::vector<std::string> imported_args{"hide", "import-id", definition_id, revision,
                                                 imported_fields};
    reset();
    const auto import_id = reservation(request_revision, "hide import-id");
    command_output = stringify_json(hide_resource_fixture("import-id"));
    check(run_command_capture(imported_args, import_id, request_revision).operation_recorded);
    result = query_operation_resource(import_id);
    check(result.exit_code == 0 && result.value.o.at("result").o.at("action").s == "import-id");
    check(files[key("operation_resources.json")].content.find("private") == std::string::npos);
    reset();
    const auto mismatched_id = reservation(request_revision, "hide import-id");
    auto mismatch = hide_resource_fixture("import-id");
    mismatch.o["items"].a.front().o["id"] = std::string(32, 'f');
    command_output = stringify_json(mismatch);
    check(run_command_capture(imported_args, mismatched_id, request_revision).operation_recorded &&
          query_operation_resource(mismatched_id).exit_code == 6);
    reset();
    const auto false_saved_id = reservation(request_revision, "hide import-id");
    command_output = stringify_json(hide_resource_fixture("import-id", "saved"));
    check(run_command_capture(imported_args, false_saved_id, request_revision).operation_recorded &&
          query_operation_resource(false_saved_id).exit_code == 6);
}
}  // namespace
#include "tracked_temporary_maps_test.inc"
#ifndef SUMH_TRACKED_MAPS_NATIVE_FIXTURES
int main() {
    test_model_and_sampling();
    test_journal_and_capture();
    test_maps_reconciliation();
    test_history_model();
    test_history_persistence();
    test_operation_query_input();
    test_operation_queries();
    test_reserved_model();
    test_reserved_lifecycle();
    test_reserved_faults_and_migration();
    test_reserved_retention_and_query();
    test_hide_definition_projection();
    test_operation_resources();
    test_tracked_temporary_maps();
    std::cout << "SUMH state ledger: production snapshot, codec, journal and capture fault checks "
                 "passed\n";
    std::cout << "SUMH operation history: " << extended_checks
              << " extended contract checks passed\n";
}
#endif
