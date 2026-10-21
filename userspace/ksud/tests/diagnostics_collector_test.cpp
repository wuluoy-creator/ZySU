#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include "core/diagnostics.hpp"
#include "core/diagnostics_model.hpp"
#include "core/operation_query.hpp"
#include "core/user_hide.hpp"
#include "mount/backend.hpp"
#include "mount/maps_preview.hpp"
#include "mount/module_preview.hpp"
#include "mount/module_rebuild.hpp"
#include "userspace/common/maps_preview_transport.hpp"

constexpr int O_RDONLY = 0, O_CLOEXEC = 8;
#ifndef CLONE_NEWNS
constexpr int CLONE_NEWNS = 16;
#endif
#ifndef CLOCK_MONOTONIC
constexpr int CLOCK_MONOTONIC = 1;
#endif
namespace diagnostic_model {
using namespace sumhp;
struct Metadata {
    unsigned st_dev = 7, st_ino = 9;
};
int reads, hides, modules, rebuilds, maps, recoveries, operations, shows, transports;
int closes, setns_count, stats, clock_count, assertions;
bool admission_error, changed_namespace, changed_boot, changed_store, store_error, query_error,
    same_initial;
bool source_oversized, source_throw, clock_error, initial_boot_unknown;
hide::Stored stored;
hide::Request selected_query;
diagnostics::Sources collected;
std::string input, transport_override;
bool input_error;
void check(bool value) {
    ++assertions;
    if (!value) {
        std::cerr << "collector check " << assertions << " failed\n";
        std::abort();
    }
}
int open(const char* path, int flags) {
    check(std::string(path) == "/proc/1/ns/mnt" && flags == (O_RDONLY | O_CLOEXEC));
    if (admission_error) {
        errno = EACCES;
        return -1;
    }
    return 20;
}
int close(int fd) {
    check(fd == 20);
    ++closes;
    return 0;
}
int fstat(int fd, Metadata* value) {
    check(fd == 20);
    *value = {};
    return 0;
}
int stat(const char* path, Metadata* value) {
    ++stats;
    *value = {};
    if (std::string(path) == "/proc/self/ns/mnt") {
        if (!setns_count && !same_initial)
            value->st_ino = 10;
        if (changed_namespace && stats >= 3)
            value->st_dev = 8;
    } else
        check(std::string(path) == "/proc/1/ns/mnt");
    return 0;
}
int setns(int fd, int flags) {
    check(fd == 20 && flags == CLONE_NEWNS);
    ++setns_count;
    return 0;
}
int clock_gettime(int, timespec* value) {
    ++clock_count;
    if (clock_error)
        return -1;
    value->tv_sec = 1;
    value->tv_nsec = clock_count * 1'000'000;
    return 0;
}
void reset() {
    reads = hides = modules = rebuilds = maps = recoveries = operations = shows = transports = 0;
    closes = setns_count = stats = clock_count = 0;
    admission_error = changed_namespace = changed_boot = changed_store = store_error = query_error =
        same_initial = false;
    source_oversized = source_throw = clock_error = initial_boot_unknown = input_error = false;
    stored = {};
    stored.revision = "sha256-v1:" + std::string(64, 'a');
    selected_query = {};
    collected = {};
    transport_override.clear();
    input = "{\"schema_version\":1,\"scenario\":\"boot-protection\"}";
}
JsonValue report(const diagnostics::Request* request, const char* status, int exit_code) {
    auto value = JsonValue::object();
    value.o["schema_version"] = 1;
    value.o["resource"] = "sumh_diagnostics";
    value.o["scenario"] = request ? diagnostics::scenario_name(request->scenario) : "unknown";
    value.o["status"] = status;
    value.o["exit_code"] = exit_code;
    value.o["read_only"] = true;
    value.o["mutations_performed"] = false;
    value.o["observer_verified"] = false;
    value.o["atomic"] = false;
    return value;
}
std::string source() {
    if (source_throw)
        throw std::runtime_error("fake failure");
    return source_oversized ? std::string(2 * 1024 * 1024 + 1, 'x') : "{}";
}
}  // namespace diagnostic_model

namespace sumhp {
std::string runtime_boot_id() {
    using namespace diagnostic_model;
    if (initial_boot_unknown)
        return {};
    return changed_boot && operations ? "boot-b" : "boot-a";
}
namespace hide {
bool read_store(Stored& out, std::string&) {
    using namespace diagnostic_model;
    ++reads;
    if (store_error)
        return false;
    out = stored;
    if (changed_store && reads > 1)
        out.revision += "changed";
    return true;
}
state::OperationResponse query_status(const Request& query) {
    using namespace diagnostic_model;
    ++hides;
    selected_query = query;
    auto value = JsonValue::object();
    value.o["status"] = query_error ? "changed_or_unavailable" : "observed";
    value.o["revision"] = stored.revision;
    value.o["items"] = JsonValue::array();
    return {query_error ? 1 : 0, value};
}
}  // namespace hide
namespace mount {
std::string module_preview_json(const ModulePreviewRequest& request) {
    using namespace diagnostic_model;
    ++modules;
    check(request.input_path.empty() && !request.inline_input && request.explain_path.empty());
    return source();
}
std::string recovery_status_json(bool* ok) {
    ++diagnostic_model::recoveries;
    *ok = true;
    return diagnostic_model::source();
}
namespace sumh {
std::string maps_preview_json() {
    ++diagnostic_model::maps;
    return diagnostic_model::source();
}
state::OperationResponse query_module_rebuild_status() {
    ++diagnostic_model::rebuilds;
    return {0, JsonValue::object()};
}
}  // namespace sumh
}  // namespace mount
namespace state {
OperationResponse query_operation(const OperationQueryRequest& query) {
    using namespace diagnostic_model;
    ++operations;
    check(query.kind == OperationQueryRequest::Kind::List ||
          query.kind == OperationQueryRequest::Kind::Show);
    if (query.kind == OperationQueryRequest::Kind::Show) {
        ++shows;
        check(query.id == std::string(32, 'a'));
    }
    return {0, JsonValue::object()};
}
}  // namespace state
namespace diagnostics {
Report diagnose(const Request& request, const Sources& sources) {
    diagnostic_model::collected = sources;
    return {0, diagnostic_model::report(&request, "observed", 0)};
}
Report unavailable(const Request* request, const char* reason, int) {
    auto value = diagnostic_model::report(request, "unavailable", 5);
    value.o["reason"] = reason;
    return {5, value};
}
std::string encode_report(const Report& report) {
    return stringify_json(report.value);
}
bool read_input(const std::string&, std::string& out, std::string& reason) {
    if (diagnostic_model::input_error) {
        reason = "input_timeout";
        return false;
    }
    out = diagnostic_model::input;
    return true;
}
}  // namespace diagnostics
namespace preview_transport {
std::string request_diagnostics(const std::string& bytes, unsigned) {
    using namespace diagnostic_model;
    ++transports;
    if (!transport_override.empty())
        return transport_override;
    diagnostics::Request request;
    std::string reason;
    check(diagnostics::parse(bytes, request, reason));
    auto value = JsonValue::object();
    value.o["ok"] = true;
    value.o["errno"] = 0;
    value.o["stdout"] = stringify_json(report(&request, "observed", 0));
    return stringify_json(value);
}
}  // namespace preview_transport
}  // namespace sumhp
#include "diagnostics_collector_under_test.inc"

int main() {
    using namespace diagnostic_model;
    using namespace sumhp::diagnostics;
    for (auto scenario : {Scenario::UserHide, Scenario::ModuleFallback, Scenario::MapsFailure,
                          Scenario::BootProtection}) {
        reset();
        Request request;
        request.scenario = scenario;
        request.definition_id = std::string(32, 'b');
        request.module_id = "example.module";
        collect_diagnostics(request);
        check(hides + modules + maps + recoveries <= 1);
        check(operations == 1 && shows == 0 && closes == 1 && setns_count == 1);
        check(collected.context.namespace_verified && collected.context.namespace_consistent &&
              collected.context.boot_before == "boot-a");
        check(collected.context.namespace_dev == "7" && collected.context.namespace_ino == "9");
        check(collected.rebuild.has_value() == (scenario == Scenario::ModuleFallback));
        check(rebuilds == (scenario == Scenario::ModuleFallback ? 1 : 0));
        check(!collected.primary.started_ms.empty() && !collected.primary.finished_ms.empty());
    }
    reset();
    Request hide;
    hide.scenario = Scenario::UserHide;
    hide.definition_id = std::string(32, 'c');
    for (int i = 0; i < 40; ++i)
        stored.document.rules.push_back({std::string(32, 'a'), "/path", true, {}, {}, {}});
    stored.document.rules[31].id = hide.definition_id;
    collect_diagnostics(hide);
    check(reads == 1 && hides == 1 && selected_query.cursor == 31 &&
          selected_query.revision == stored.revision);
    auto selection = json::parse(collected.primary.bytes);
    check(selection.at("selection_status").s == "found" &&
          selection.at("saved_definition").at("id").s == hide.definition_id);
    query_error = true;
    collect_diagnostics(hide);
    check(json::parse(collected.primary.bytes).at("selection_status").s == "stale");
    reset();
    collect_diagnostics(hide);
    check(reads == 2 && hides == 0 &&
          json::parse(collected.primary.bytes).at("selection_status").s == "absent");
    reset();
    changed_store = true;
    collect_diagnostics(hide);
    check(json::parse(collected.primary.bytes).at("selection_status").s == "stale");
    reset();
    store_error = true;
    collect_diagnostics(hide);
    check(hides == 0 &&
          json::parse(collected.primary.bytes).at("selection_status").s == "unavailable");
    reset();
    Request boot;
    boot.operation_id = std::string(32, 'a');
    collect_diagnostics(boot);
    check(recoveries == 1 && operations == 2 && shows == 1 &&
          collected.selected_operation.has_value());
    reset();
    admission_error = true;
    check(json::parse(collect_diagnostics(boot)).at("status").s == "unavailable" && !operations &&
          !recoveries && !closes);
    reset();
    same_initial = true;
    collect_diagnostics(boot);
    check(!setns_count);
    reset();
    changed_namespace = true;
    collect_diagnostics(boot);
    check(!collected.context.namespace_consistent);
    reset();
    changed_boot = true;
    collect_diagnostics(boot);
    check(collected.context.boot_before != collected.context.boot_after);
    reset();
    initial_boot_unknown = true;
    collect_diagnostics(boot);
    check(collected.context.boot_before.empty());
    reset();
    source_oversized = true;
    collect_diagnostics(boot);
    check(!collected.primary.failure.empty() && collected.primary.bytes.empty());
    reset();
    source_throw = true;
    collect_diagnostics(boot);
    check(!collected.primary.failure.empty() && collected.primary.bytes.empty());
    reset();
    clock_error = true;
    collect_diagnostics(boot);
    check(!recoveries && !operations);
    std::ostringstream output;
    auto* old = std::cout.rdbuf(output.rdbuf());
    reset();
    check(doctor_command({"doctor", "--input", "-"}) == 0 && transports == 1 && !recoveries);
    for (const auto& args :
         std::vector<std::vector<std::string>>{{"doctor"},
                                               {"doctor", "manager", "{}", "--channel"},
                                               {"doctor", "--input", "-", "extra"}}) {
        reset();
        check(doctor_command(args) == 2 && !transports && !operations);
    }
    reset();
    input = "{}";
    check(doctor_command({"doctor", "--input", "-"}) == 2 && !transports);
    reset();
    input_error = true;
    check(doctor_command({"doctor", "--input", "-"}) == 2 && !transports);
    for (auto bad : {"{}", "{\"ok\":true,\"errno\":0,\"stdout\":\"{}\"}",
                     "{\"ok\":false,\"errno\":16,\"stdout\":\"\"}"}) {
        reset();
        transport_override = bad;
        check(doctor_command({"doctor", "--input", "-"}) == 5 && transports == 1);
    }
    std::cout.rdbuf(old);
    std::cout << assertions
              << " diagnostic collector/CLI orchestration checks passed (source/model/OS services "
                 "mocked)\n";
}
