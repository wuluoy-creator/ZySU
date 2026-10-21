#include "mount/maps_preview.hpp"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <tuple>
#include <vector>
#include "core/config_document.hpp"
#include "core/diagnostic_package.hpp"
#include "core/diagnostics.hpp"
#include "core/operation_manager_input.hpp"
#include "core/operation_query.hpp"
#include "core/operation_submit.hpp"
#include "core/profile.hpp"
#include "core/user_hide.hpp"
#include "mount/development.hpp"
#include "mount/maps_history.hpp"
#include "mount/maps_snapshot.hpp"
#include "mount/module_preview.hpp"
#include "uapi/sumh.h"
#include "userspace/common/maps_preview_transport.hpp"
#include "userspace/common/profile_manager_transport.hpp"

// Linux descriptor flags are modeled, not Windows file API behavior.
constexpr int O_RDONLY = 1, O_CLOEXEC = 2, O_NOFOLLOW = 4, O_NONBLOCK = 8;
namespace preview_model {
struct Metadata {
    int st_mode = 1, st_nlink = 1;
    std::int64_t st_size = 0;
} metadata;
struct File {
    std::string bytes;
    int error = 0;
};
std::vector<File> configs;
std::vector<std::string> boots;
std::vector<std::string> history_status;
std::vector<sumhp::mount::sumh::MapsHistory> histories;
sumhp::mount::sumh::MapsSnapshot snapshot;
int opens = 0, closes = 0, offset = 0, boots_read = 0, histories_read = 0, samples = 0;
int guard_calls = 0, guards_closed = 0, guard_error = 0, stat_error = 0, read_error = 0, eintrs = 0;
int uid = 0, direct_calls = 0, daemon_calls = 0, terminal_errors = 0, channel_calls = 0;
bool hash_failure = false, size_override = false;
std::int64_t ticks = 0, clock_step = 0;
struct Clock {
    static auto now() {
        ticks += clock_step;
        return std::chrono::steady_clock::time_point(std::chrono::milliseconds(ticks));
    }
};
bool is_regular(int mode) {
    return mode == 1;
}
int open(const std::filesystem::path::value_type*, int flags) {
    assert(flags == (O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    assert(opens < static_cast<int>(configs.size()));
    offset = 0;
    const auto& input = configs[opens++];
    if (input.error) {
        errno = input.error;
        return -1;
    }
    return 10;
}
int fstat(int fd, Metadata* out) {
    assert(fd == 10);
    if (stat_error) {
        errno = stat_error;
        return -1;
    }
    *out = metadata;
    if (!size_override)
        out->st_size = configs[opens - 1].bytes.size();
    return 0;
}
int read(int fd, void* buffer, std::size_t size) {
    assert(fd == 10);
    if (eintrs) {
        --eintrs;
        errno = EINTR;
        return -1;
    }
    if (read_error) {
        errno = read_error;
        return -1;
    }
    const auto& bytes = configs[opens - 1].bytes;
    const auto count = std::min(size, bytes.size() - offset);
    std::memcpy(buffer, bytes.data() + offset, count);
    offset += count;
    return static_cast<int>(count);
}
int close(int fd) {
    if (fd == 20)
        ++guards_closed;
    else {
        assert(fd == 10);
        ++closes;
    }
    return 0;
}
int geteuid() {
    return uid;
}
}
namespace sumhp {
std::filesystem::path runtime_config_file() {
    return "/config.json";
}
std::string runtime_boot_id() {
    return preview_model::boots.at(preview_model::boots_read++);
}
namespace state {
std::string revision_for_bytes(const std::string& bytes) {
    if (preview_model::hash_failure)
        return {};
    // Hash implementation is separately exercised; retain exact input here.
    std::ostringstream out;
    out << "test-hash:" << std::hash<std::string>{}(bytes);
    return out.str();
}
}
#include "maps_preview_config_under_test.inc"
namespace mount::sumh {
MapsSnapshot sample_maps(const Config& config, bool resolve_disabled,
                         const ::sumhp::sumh::RuntimeState* expected) {
    ++preview_model::samples;
    assert(resolve_disabled && !expected);
    std::string error;
    assert(validate_maps_path_rules(config.maps_rules, error));
    return preview_model::snapshot;
}
std::string read_maps_history(MapsHistory& out, std::string& error) {
    const int i = preview_model::histories_read++;
    const auto status = preview_model::history_status.at(i);
    if (status == "valid")
        out = preview_model::histories.at(i);
    else if (status != "missing")
        error = "fixture history read failure";
    return status;
}
}  // namespace mount::sumh
namespace preview_transport {
int acquire_preview_guard() {
    ++preview_model::guard_calls;
    if (preview_model::guard_error) {
        errno = preview_model::guard_error;
        return -1;
    }
    return 20;
}
int serve(const std::function<std::string()>& callback, Service service) {
    assert(service == Service::Maps);
    ++preview_model::channel_calls;
    assert(!callback().empty());
    return 0;
}
}
}  // namespace sumhp
#include "maps_preview_under_test.inc"
namespace sumhp::mount::sumh {
#include "maps_preview_comparison_under_test.inc"
namespace ksud::terminal {
int error(const std::string&, const std::string& = {}) {
    ++preview_model::terminal_errors;
    return 99;
}
}
namespace sumhp {
// Keep the full production dispatcher: a Maps input entering any unrelated
// endpoint is a test failure. Their parsers/types are the production headers;
// their effects are covered by the corresponding endpoint suites.
namespace diagnostic_package {
std::string manager_dispatch(const diagnostic_package_manager::Request&) {
    assert(false);
    return {};
}
int command(const std::vector<std::string>&) {
    assert(false);
    return 1;
}
}
namespace diagnostics {
std::string collect_diagnostics(const Request&) {
    assert(false);
    return {};
}
int doctor_command(const std::vector<std::string>&) {
    assert(false);
    return 1;
}
}
namespace profiles {
std::string manager_dispatch(const std::string&) {
    assert(false);
    return {};
}
int command(const std::vector<std::string>&) {
    assert(false);
    return 1;
}
}
namespace profile_transport {
int serve(const std::function<std::string(const std::string&)>&) {
    assert(false);
    return 1;
}
}
namespace temporary_maps {
std::string dispatch(const Request&) {
    assert(false);
    return {};
}
}
namespace maps_temporary {
int command(const std::vector<std::string>&, std::ostream&, std::ostream&) {
    assert(false);
    return 1;
}
}
std::string capability_snapshot_json() {
    assert(false);
    return {};
}
namespace state {
OperationResponse submit_operation_bytes(bool, const std::string&, const std::string&) {
    assert(false);
    return {};
}
OperationResponse query_operation(const OperationQueryRequest&) {
    assert(false);
    return {};
}
bool parse_operation_submit_arguments(const std::vector<std::string>&, OperationSubmitRequest&,
                                      std::string&) {
    assert(false);
    return false;
}
int print_operation_submit(const OperationSubmitRequest&) {
    assert(false);
    return 1;
}
bool parse_operation_query_arguments(const std::vector<std::string>&, OperationQueryRequest&,
                                     std::string&) {
    assert(false);
    return false;
}
int print_operation_query(const OperationQueryRequest&) {
    assert(false);
    return 1;
}
}
namespace hide {
std::string manager_dispatch_json(const std::function<int()>&) {
    assert(false);
    return {};
}
int print_status(const Request&) {
    assert(false);
    return 1;
}
int print_definitions(const Request&) {
    assert(false);
    return 1;
}
int print_export() {
    assert(false);
    return 1;
}
}
namespace mount::sumh {
int print_module_rebuild_status() {
    assert(false);
    return 1;
}
}
namespace mount {
// Unrelated module preview is covered by the real full-chain CLI and its own
// controller suite; this fixture must never route a Maps case into that work.
bool is_module_preview_prefix(const std::vector<std::string>&) {
    return false;
}
bool is_development_prefix(const std::vector<std::string>&) {
    return false;
}
bool parse_development_arguments(const std::vector<std::string>&, DevelopmentRequest&,
                                 std::string&) {
    assert(false);
    return false;
}
std::string development_query_json(const DevelopmentRequest&) {
    assert(false);
    return {};
}
std::string partition_save_json(const DevelopmentRequest&) {
    assert(false);
    return {};
}
bool parse_module_preview_arguments(const std::vector<std::string>&, ModulePreviewRequest&,
                                    std::string&) {
    assert(false);
    return false;
}
std::string module_preview_json(const ModulePreviewRequest&) {
    assert(false);
    return {};
}
}
int run_command(const std::vector<std::string>&) {
    ++preview_model::direct_calls;
    return 10;
}
int run_via_daemon(const std::vector<std::string>&, bool) {
    ++preview_model::daemon_calls;
    return 20;
}
std::string arg_or_default(const std::vector<std::string>& args, std::size_t index,
                           const std::string& fallback) {
    return index < args.size() ? args[index] : fallback;
}
#include "maps_preview_embedded_under_test.inc"
#include "maps_preview_query_under_test.inc"
int preview_cli(const std::vector<std::string>& args) {
    const std::string op = arg_or_default(args, 2, "");
#include "maps_preview_cli_under_test.inc"
    return 100;
}
}

using namespace sumhp;
using namespace sumhp::mount::sumh;
using namespace preview_model;
int observations = 0;
JsonValue config_json(std::vector<MapsPathRule> paths = {{"/target", "/replacement"}},
                      bool enabled = true) {
    auto value = JsonValue::object();
    value.o["schema_version"] = 1;
    value.o["enable_maps_spoof"] = enabled;
    value.o["maps_rules"] = maps_path_rules_json(paths);
    return value;
}
void set_config(const JsonValue& value) {
    configs = {{stringify_json(value), 0}, {stringify_json(value), 0}};
}
void reset() {
    set_config(config_json());
    metadata = {};
    boots = {"boot-one", "boot-one"};
    history_status = {"missing", "missing"};
    histories = {{}, {}};
    opens = closes = offset = boots_read = histories_read = samples = guard_calls = guards_closed =
        0;
    guard_error = stat_error = read_error = eintrs = uid = direct_calls = daemon_calls =
        terminal_errors = channel_calls = 0;
    hash_failure = size_override = false;
    ticks = clock_step = 0;
    snapshot = {};
    snapshot.instance_id = 9;
    snapshot.generation = 7;
    snapshot.namespace_ino = 33;
    snapshot.identities_sampled = snapshot.definitions_complete = snapshot.stable = true;
    snapshot.reason = "maps_sampled";
    snapshot.paths = {{{11, 8, MapsIdentityKind::Regular, 0},
                       {22, 8, MapsIdentityKind::Regular, 0},
                       false,
                       true}};
    ::sumhp::sumh::InventoryEntry row;
    row.target_ino = 11;
    row.target_dev = 8;
    row.spoofed_ino = 22;
    row.spoofed_dev = 8;
    row.path2 = "/replacement";
    snapshot.registered = {row};
}
JsonValue observe(const char* status) {
    ++observations;
    const auto bytes = maps_preview_json();
    assert(bytes.size() <= MAPS_PREVIEW_MAX_BYTES);
    JsonValue result;
    std::string error;
    assert(parse_json(bytes, result, error));
    if (result.at("status").s != status)
        std::cerr << "observation " << observations << ": " << bytes << '\n';
    assert(result.at("status").s == status);
    assert(result.at("read_only").b);
    assert(guard_calls == 1 && guards_closed == (guard_error ? 0 : 1));
    if (!guard_error) {
        assert(opens == 2 && boots_read == 2 && histories_read == 2);
    }
    return result;
}
const JsonValue& first(const JsonValue& value) {
    return value.at("rows").a.at(0);
}
MapsSubmission receipt() {
    MapsSubmission value;
    value.id = std::string(32, 'a');
    value.boot_id = "older-boot";
    value.definition_revision = "sha256-v1:" + std::string(64, 'a');
    value.namespace_ino = "33";
    value.phase = "acknowledged";
    value.definitions = {{"/target", "/replacement"}};
    value.identities = {{5, 8, 6, 8}};
    return value;
}

int main() {
    reset();
    auto out = observe("complete");
    assert(out.at("sample").at("comparison").s == "matches" &&
           first(out).at("registered_match").s == "matches");
    assert(out.at("last_submission").at("status").s == "missing" &&
           out.at("last_submission").at("confirmed").is_null());
    reset();
    snapshot.registered.clear();
    out = observe("complete");
    assert(first(out).at("registered_match").s == "missing" &&
           out.at("sample").at("comparison").s == "drift");
    reset();
    snapshot.registered[0].spoofed_ino = 23;
    assert(first(observe("complete")).at("registered_match").s == "different");
    reset();
    snapshot.paths[0].target = {0, 0, MapsIdentityKind::Missing, ENOENT};
    out = observe("complete");
    assert(first(out).at("target").at("status").s == "missing" &&
           first(out).at("target").at("ino").is_null());
    assert(out.at("sample").at("comparison").s == "unknown");
    reset();
    snapshot.paths[0].replacement = {0, 0, MapsIdentityKind::NotRegular, EINVAL};
    assert(first(observe("complete")).at("replacement").at("status").s == "not_regular");
    reset();
    snapshot.paths[0].target = {0, 0, MapsIdentityKind::Unrepresentable, EOVERFLOW};
    assert(first(observe("complete")).at("target").at("status").s == "unrepresentable");
    reset();
    snapshot.paths[0].target = {0, 0, MapsIdentityKind::Error, EACCES};
    assert(first(observe("complete")).at("target").at("errno").n == EACCES);
    reset();
    snapshot.paths[0].collision = true;
    out = observe("complete");
    assert(first(out).at("same_target_collision").b &&
           first(out).at("registered_match").s == "unknown");
    assert(out.at("sample").at("comparison").s == "drift");
    reset();
    snapshot.definitions_complete = false;
    snapshot.error = EOPNOTSUPP;
    snapshot.reason = "managed_maps_inventory_failed";
    out = observe("partial");
    assert(first(out).at("target").at("ino").s == "11" && out.at("registered_count").is_null());
    reset();
    snapshot = {};
    snapshot.error = ETIMEDOUT;
    out = observe("unknown");
    assert(first(out).at("target").at("status").s == "unknown" &&
           out.at("saved").at("count").n == 1);
    reset();
    snapshot.stable = false;
    snapshot.paths[0].stable = false;
    snapshot.reason = "maps_sample_changed";
    out = observe("stale");
    assert(!first(out).at("stable").b && out.at("sample").at("comparison").s == "unknown");
    reset();
    set_config(config_json({}, false));
    snapshot.paths.clear();
    snapshot.registered.clear();
    out = observe("complete");
    assert(out.at("rows").a.empty() && out.at("saved").at("status").s == "valid");
    reset();
    set_config(config_json({{"/target", "/replacement"}}, false));
    snapshot.registered.clear();
    out = observe("complete");
    assert(samples == 1 && out.at("sample").at("comparison").s == "matches" &&
           first(out).at("target").at("ino").s == "11");
    reset();
    set_config(config_json({{"/target", "/replacement"}}, false));
    assert(observe("complete").at("sample").at("comparison").s == "drift");
    reset();
    configs = {{"", ENOENT}, {"", ENOENT}};
    out = observe("unknown");
    assert(out.at("saved").at("status").s == "missing" && samples == 0 && closes == 0);
    reset();
    configs = {{"", EACCES}, {"", EACCES}};
    assert(observe("unknown").at("saved").at("status").s == "unreadable");
    reset();
    configs = {{"{", 0}, {"{", 0}};
    assert(observe("unknown").at("saved").at("status").s == "invalid" && samples == 0);
    reset();
    auto future = config_json();
    future.o["schema_version"] = 2;
    set_config(future);
    assert(observe("unknown").at("saved").at("status").s == "invalid" && samples == 0);
    reset();
    configs[1].bytes += " ";
    out = observe("stale");
    assert(out.at("saved").at("status").s == "changed" && !first(out).at("stable").b &&
           out.at("sample").at("comparison").s == "unknown");
    reset();
    configs[1].error = ENOENT;
    assert(observe("stale").at("reason").s == "saved_config_changed");
    reset();
    boots[1] = "boot-two";
    assert(observe("stale").at("reason").s == "boot_id_changed");
    reset();
    boots = {"", ""};
    assert(observe("unknown").at("reason").s == "boot_id_unavailable");
    reset();
    history_status = {"unreadable", "unreadable"};
    assert(observe("partial").at("last_submission").at("status").s == "unreadable");
    reset();
    history_status = {"corrupt_or_unsupported", "corrupt_or_unsupported"};
    assert(observe("partial").at("last_submission").at("reason").s ==
           "maps_submission_history_invalid");
    reset();
    history_status = {"missing", "valid"};
    assert(observe("stale").at("reason").s == "maps_submission_history_changed");
    reset();
    history_status = {"valid", "valid"};
    histories[0].confirmed = histories[1].confirmed = receipt();
    out = observe("complete");
    assert(out.at("last_submission").at("confirmed").at("rows").a[0].at("target_ino").s == "5");
    assert(first(out).at("target").at("ino").s == "11");
    reset();
    history_status = {"valid", "valid"};
    histories[0].confirmed = histories[1].confirmed = receipt();
    histories[1].confirmed->id = std::string(32, 'b');
    assert(observe("stale").at("reason").s == "maps_submission_history_changed");
    reset();
    set_config(config_json({{u8"/目录/target ", u8"/替换 "}}));
    snapshot.paths[0].target.ino = UINT64_MAX;
    snapshot.paths[0].replacement.ino = 9007199254740993ULL;
    out = observe("complete");
    assert(first(out).at("target_path").s == u8"/目录/target ");
    assert(first(out).at("replacement_path").s == u8"/替换 " &&
           first(out).at("target").at("ino").s == "18446744073709551615");
    assert(first(out).at("replacement").at("ino").s == "9007199254740993");
    reset();
    metadata.st_mode = 2;
    assert(observe("unknown").at("errno").n == EINVAL && samples == 0 && closes == 2);
    reset();
    metadata.st_nlink = 2;
    assert(observe("unknown").at("errno").n == EINVAL);
    reset();
    stat_error = EIO;
    assert(observe("unknown").at("errno").n == EIO && closes == 2);
    reset();
    read_error = EIO;
    assert(observe("unknown").at("errno").n == EIO && closes == 2);
    reset();
    size_override = true;
    metadata.st_size = 4 * 1024 * 1024 + 1;
    assert(observe("unknown").at("errno").n == EOVERFLOW && samples == 0);
    reset();
    size_override = true;
    metadata.st_size = 0;
    configs[0].bytes = configs[1].bytes = std::string(4 * 1024 * 1024 + 1, ' ');
    assert(observe("unknown").at("errno").n == EOVERFLOW && samples == 0);
    reset();
    eintrs = 1;
    observe("complete");
    reset();
    eintrs = 1000;
    clock_step = 500;
    assert(observe("unknown").at("errno").n == ETIMEDOUT && closes == 2);
    reset();
    hash_failure = true;
    assert(observe("unknown").at("reason").s == "revision_unavailable");
    reset();
    guard_error = EADDRINUSE;
    assert(observe("unknown").at("reason").s == "maps_preview_guard_unavailable" && samples == 0 &&
           opens == 0);
    reset();
    std::vector<MapsPathRule> many;
    for (int i = 0; i < 257; ++i)
        many.push_back({"/t" + std::to_string(i), "/r"});
    set_config(config_json(many));
    assert(observe("unknown").at("saved").at("status").s == "invalid" && samples == 0);
    reset();
    many.pop_back();
    set_config(config_json(many));
    snapshot.paths.assign(256, snapshot.paths[0]);
    assert(observe("complete").at("rows").a.size() == 256);
    reset();
    many.clear();
    for (int i = 0; i < 20; ++i)
        many.push_back({"/" + std::string(4000, 'x') + std::to_string(i), "/r"});
    set_config(config_json(many));
    assert(observe("unknown").at("saved").at("status").s == "invalid" && samples == 0);
    reset();
    history_status = {"valid", "valid"};
    auto huge = receipt();
    huge.definitions[0].target_path = std::string(MAPS_PREVIEW_MAX_BYTES, 'x');
    histories[0].confirmed = histories[1].confirmed = huge;
    assert(observe("unknown").at("reason").s == "maps_preview_response_capacity_exceeded");
    reset();
    set_config(config_json({{"/target", "/r"}, {"/target", "/x"}}));
    assert(observe("unknown").at("saved").at("status").s == "invalid");
    reset();
    set_config(config_json({{"/target\n", "/r"}}));
    assert(observe("unknown").at("saved").at("status").s == "invalid");

    // Execute production embedded route, with all daemon/direct dispatches counted.
    std::ostringstream output, errors;
    auto* old_out = std::cout.rdbuf(output.rdbuf());
    auto* old_err = std::cerr.rdbuf(errors.rdbuf());
    reset();
    assert(embedded_command({"sumh", "maps", "preview"}) == 0);
    assert(samples == 1 && daemon_calls == 0 && direct_calls == 0 && terminal_errors == 0);
    reset();
    assert(embedded_command({"sumh", "maps", "preview", "--channel"}) == 0);
    assert(channel_calls == 1 && samples == 1 && daemon_calls == 0 && direct_calls == 0);
    reset();
    assert(embedded_command({"sumh", "maps", "preview", "extra"}) == 1);
    assert(samples == 0 && daemon_calls == 0 && direct_calls == 0 && guard_calls == 0);
    reset();
    assert(embedded_command({"sumh", "maps", "preview", "--channel", "extra"}) == 1);
    assert(channel_calls == 0 && daemon_calls == 0 && direct_calls == 0);
    reset();
    uid = 1000;
    assert(embedded_command({"sumh", "maps", "preview"}) == 99);
    assert(terminal_errors == 1 && samples == 0 && daemon_calls == 0);
    reset();
    uid = 1000;
    assert(embedded_command({"sumh", "maps", "preview", "--channel"}) == 99);
    assert(channel_calls == 0);
    for (const auto& cmd : std::vector<std::vector<std::string>>{
             {"sumh", "maps", "apply"}, {"sumh", "maps", "list-paths"}, {"config", "show"}}) {
        reset();
        assert(embedded_command(cmd) == 20 && daemon_calls == 1 && guard_calls == 0);
    }
    reset();
    assert(embedded_command({"help"}) == 10 && direct_calls == 1 && daemon_calls == 0);
    reset();
    assert(preview_cli({"sumh", "maps", "preview"}) == 0 && samples == 1);
    reset();
    assert(preview_cli({"sumh", "maps", "preview", "extra"}) == 1 && samples == 0);
    assert(is_query({"sumh", "maps", "preview"}));
    assert(!is_query({"sumh", "maps", "preview", "extra"}));
    assert(is_query({"sumh", "maps", "list-paths"}));
    assert(!is_query({"sumh", "maps", "apply"}));
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    std::cout << "Maps preview: " << observations
              << " observation scenarios and 16 routing/classification checks passed\n";
}
