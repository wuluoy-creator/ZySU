// Real collector, hide status, operation query and diagnostic model. Only the
// filesystem/kernel/process boundaries are deterministic services; no device IO.
#include <ctime>
#include <iomanip>
#include <stdexcept>
#include "core/diagnostics.hpp"
#include "core/diagnostics_model.hpp"
#include "core/operation_query.hpp"
#include "core/operation_resource.hpp"
#include "core/state_ledger.hpp"
#include "diagnostics_hide_services.inc"
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
namespace integration_os {
struct Metadata {
    unsigned st_dev = 7, st_ino = 9;
};
int ticks, stats, operations;
bool namespace_change;
int open(const char*, int) {
    return 20;
}
int close(int fd) {
    fixture::check(fd == 20);
    return 0;
}
int fstat(int fd, Metadata* value) {
    fixture::check(fd == 20);
    *value = {};
    return 0;
}
int stat(const char*, Metadata* value) {
    *value = {};
    if (++stats > 2 && namespace_change)
        value->st_dev = 8;
    return 0;
}
int setns(int, int) {
    std::abort();
}
int clock_gettime(int, timespec* value) {
    value->tv_sec = 1;
    value->tv_nsec = ++ticks * 1'000'000;
    return 0;
}
}  // namespace integration_os
namespace sumhp {
namespace state {
std::string read_operation_history(Ledger&, std::string&) {
    ++integration_os::operations;
    return "missing";
}
JsonValue operation_record_json(const Operation&, const std::string&) {
    std::abort();
}
OperationResponse query_operation_resource(const std::string&) {
    std::abort();
}
}  // namespace state
namespace mount {
std::string module_preview_json(const ModulePreviewRequest&) {
    std::abort();
}
std::string recovery_status_json(bool*) {
    std::abort();
}
namespace sumh {
std::string maps_preview_json() {
    std::abort();
}
state::OperationResponse query_module_rebuild_status() {
    std::abort();
}
}  // namespace sumh
}  // namespace mount
namespace diagnostics {
bool read_input(const std::string&, std::string&, std::string&) {
    std::abort();
}
}  // namespace diagnostics
namespace preview_transport {
std::string request_diagnostics(const std::string&, unsigned) {
    std::abort();
}
}  // namespace preview_transport
}  // namespace sumhp
#include "diagnostics_hide_collector_under_test.inc"

namespace {
using namespace sumhp;
using fixture::check;
diagnostics::Request request;
void setup() {
    fixture::reset();
    integration_os::ticks = integration_os::stats = integration_os::operations = 0;
    integration_os::namespace_change = false;
    fixture::stored.document.rules.clear();
    fixture::registered.clear();
    fixture::stored.document.source_schema = 2;
    for (unsigned index = 0; index < 40; ++index) {
        std::ostringstream id;
        id << std::hex << std::setw(32) << std::setfill('0') << index + 1;
        fixture::stored.document.rules.push_back({id.str(),
                                                  "/private/path-" + std::to_string(index), true,
                                                  "private-name", "private-note", "private-group"});
    }
    request = {};
    request.scenario = diagnostics::Scenario::UserHide;
    request.definition_id = fixture::stored.document.rules[31].id;
    fixture::registered = {{7, 9, SUMH_USER_HIDE_LIVE, SUMH_USER_HIDE_BOUND, 0,
                            fixture::stored.document.rules[31].path}};
}
JsonValue collect(int expected) {
    const auto bytes = diagnostics::collect_diagnostics(request);
    JsonValue out;
    std::string error;
    check(json::Parser(bytes, true).parse(out, error));
    if (out.at("exit_code").n != expected)
        std::cerr << bytes << '\n';
    check(out.at("exit_code").n == expected);
    check(fixture::writes == 0 && fixture::locks == 0 && fixture::calls.empty() &&
          fixture::random_calls == 0);
    check(bytes.find("/private/") == std::string::npos &&
          bytes.find("private-note") == std::string::npos);
    check(!out.at("observer_verified").b && !out.at("mutations_performed").b &&
          !out.at("atomic").b);
    return out;
}
const JsonValue& evidence(const JsonValue& report, const char* id) {
    for (const auto& value : report.at("evidence").a)
        if (value.at("id").s == id)
            return value;
    static const JsonValue empty;
    return empty;
}
bool finding(const JsonValue& report, const char* code) {
    for (const auto& value : report.at("findings").a)
        if (value.at("code").s == code)
            return true;
    return false;
}
}  // namespace
int main() {
    setup();
    auto report = collect(0);
    check(fixture::reads == 3 && fixture::queries == 1 && integration_os::operations == 1);
    check(evidence(report, "hide.runtime").at("value").at("registered").b);
    bool cursor = false;
    for (const auto& step : report.at("next_steps").a)
        if (step.at("code").s == "refresh_hide_status")
            cursor = step.at("arguments").a.back().s == "31";
    check(cursor);

    setup();
    fixture::registered.clear();
    report = collect(0);
    check(!evidence(report, "hide.runtime").at("value").at("registered").b);
    check(finding(report, "saved_enabled_differs_from_membership"));
    setup();
    fixture::stored.document.rules[31].enabled = false;
    report = collect(0);
    check(evidence(report, "hide.runtime").at("value").at("state").s == "disabled_pending_removal");
    setup();
    fixture::registered[0].management = SUMH_USER_HIDE_SUSPENDED;
    report = collect(0);
    check(finding(report, "global_suspension_not_definition_disabled"));
    setup();
    fixture::managed = false;
    report = collect(0);
    check(evidence(report, "hide.runtime").at("status").s == "unsupported" &&
          fixture::queries == 0);
    setup();
    fixture::query_ok = false;
    report = collect(3);
    check(evidence(report, "hide.runtime").at("status").s == "unknown");
    setup();
    fixture::stored.document.rules.erase(fixture::stored.document.rules.begin() + 31);
    report = collect(0);
    check(!evidence(report, "hide.saved").at("value").at("definition_present").b &&
          fixture::queries == 0);
    setup();
    fixture::change_read = true;
    report = collect(4);
    check(report.at("next_steps").a.empty() && fixture::queries == 0);
    setup();
    fixture::change_boot = true;
    report = collect(4);
    check(report.at("next_steps").a.empty());
    setup();
    integration_os::namespace_change = true;
    report = collect(4);
    check(report.at("next_steps").a.empty());
    setup();
    fixture::read_ok = false;
    collect(5);
    check(fixture::queries == 0);
    setup();
    request.operation_id = std::string(32, 'b');
    report = collect(0);
    check(integration_os::operations == 2 &&
          evidence(report, "operation.selected").at("value").at("status").s == "not_retained");
    std::cout << fixture::checks
              << " actual hide/collector/model/operation-query integration checks passed\n";
}
