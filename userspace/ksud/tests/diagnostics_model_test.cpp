#include "core/diagnostics_model.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

using sumhp::json_quote;
using sumhp::JsonValue;
using sumhp::stringify_json;
using namespace sumhp::diagnostics;
using V = JsonValue;
namespace producer {
struct BackendProbe {
    bool checked = false, known = false, available = false;
    int error_number = 0;
    std::string detail;
};
struct ModuleEntry {
    std::string id;
    std::filesystem::path path;
};
struct ModuleBackendDecision {
    std::string configured_mode = "auto", global_mode = "auto", requested = "sumh",
                source = "module", selected = "magic", reason = "sumh_protocol_mismatch",
                observation_error;
    bool used_fallback = true, complete = true, has_content = true, requires_magic = false;
    BackendProbe sumh{true, true, false, 0, "protocol_mismatch"}, overlay;
};
V exact(const std::string& text) {
    auto v = V::object();
    v["encoding"] = "utf-8";
    v["value"] = text;
    v["byte_length"] = static_cast<int>(text.size());
    return v;
}
#include "diagnostic_module_serializers.inc"
#include "diagnostic_module_validator.inc"
enum class MapsIdentityKind { Regular, Missing, NotRegular, Unrepresentable, Error, Unknown };
struct MapsFileIdentity {
    MapsIdentityKind kind = MapsIdentityKind::Regular;
    unsigned long long ino = 5, dev = 0;
    int error = 0;
};
constexpr int SUMH_MANAGED_MAPS_MAX_RULES = 256, MAPS_RULES_MAX_ENCODED_BYTES = 56 * 1024,
              MAPS_PREVIEW_MAX_BYTES = 512 * 1024;
#include "diagnostic_maps_serializers.inc"
struct Operation {
    std::string id, name, phase, boot_id;
    int exit_code = 0, error_number = 0;
    std::string detail, request_revision;
};
#include "diagnostic_operation_serializers.inc"
enum class ReadState { Missing, Valid, Corrupt, Unreadable };
struct RecoveryRecord {
    unsigned attempts = 0;
    std::string boot_id, phase, reason;
};
bool lock_available = true;
struct RecoveryLock {
    bool acquire(bool, std::string&) { return lock_available; }
};
ReadState counter_state = ReadState::Missing, marker_state = ReadState::Missing;
RecoveryRecord counter_record, marker_record;
constexpr unsigned kMaxBootAttempts = 3;
bool healthy(ReadState s) {
    return s == ReadState::Missing || s == ReadState::Valid;
}
const char* state_name(ReadState s) {
    return s == ReadState::Missing   ? "missing"
           : s == ReadState::Valid   ? "valid"
           : s == ReadState::Corrupt ? "corrupt"
                                     : "unreadable";
}
ReadState load_record(const char* name, bool, RecoveryRecord& out, std::string&) {
    const bool marker = std::string(name) == "mount_disabled";
    out = marker ? marker_record : counter_record;
    return marker ? marker_state : counter_state;
}
#include "diagnostic_recovery_serializer.inc"
}  // namespace producer

namespace {
int assertions = 0;
void require(bool value, const char* message) {
    ++assertions;
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
V parsed(const std::string& bytes) {
    V value;
    std::string error;
    require(json::Parser(bytes, true).parse(value, error), "fixture JSON parses");
    return value;
}
const std::string id(32, 'a'), opid(32, 'b'), rebuildid(32, 'c'), submissionid(32, 'd');
const std::string rev = "sha256-v1:" + std::string(64, 'a');
const V& evidence(const Report& r, const char* id) {
    for (const auto& e : r.value.at("evidence").a)
        if (e.at("id").s == id)
            return e;
    static const V absent;
    return absent;
}
bool finding(const Report& r, const char* code) {
    for (const auto& f : r.value.at("findings").a)
        if (f.at("code").s == code)
            return true;
    return false;
}
bool step(const Report& r, const char* code) {
    for (const auto& s : r.value.at("next_steps").a)
        if (s.at("code").s == code)
            return true;
    return false;
}
Sample sample(SourceKind kind, const V& value) {
    return {kind, "102", "108", 0, stringify_json(value), {}, 0};
}
V history(const std::string& selected = {}) {
    V v = V::object();
    v["schema_version"] = 2;
    v["resource"] = "operation_history";
    v["read_only"] = true;
    v["scope"] = "recorded_commands_in_caller_filesystem_namespace";
    v["runtime_verified"] = false;
    v["durability_confirmation"] = "not_observable_from_read";
    v["ledger_status"] = "missing";
    v["error"] = "";
    v["boot_id"] = "boot-a";
    v["requested_id"] = selected.empty() ? V{} : V(selected);
    v["history_complete"] = false;
    v["history_pruned"] = V{};
    v["retention_limit"] = 16;
    v["records"] = V::array();
    v["status"] = selected.empty() ? "observed" : "not_retained";
    v["wait_budget_ms"] = 0;
    v["elapsed_ms"] = 1;
    return v;
}
Sources sources(Scenario scenario, const V& primary) {
    Sources s;
    s.context = {"boot-a", "boot-a", "4", "42", true, true, "100", "110"};
    s.primary = sample(scenario == Scenario::UserHide         ? SourceKind::HideSelection
                       : scenario == Scenario::ModuleFallback ? SourceKind::ModulePlan
                       : scenario == Scenario::MapsFailure    ? SourceKind::MapsPreview
                                                              : SourceKind::RecoveryLegacy,
                       primary);
    s.operations = sample(SourceKind::Operations, history());
    return s;
}
V hide() {
    V wrapper = V::object();
    wrapper["schema_version"] = 1;
    wrapper["resource"] = "diagnostic_hide_selection";
    wrapper["definition_id"] = id;
    wrapper["selection_status"] = "found";
    wrapper["revision"] = rev;
    wrapper["definition_store_stable"] = true;
    wrapper["saved_definition"] = V::object();
    wrapper["saved_definition"]["id"] = id;
    wrapper["saved_definition"]["enabled"] = true;
    V page = V::object();
    page["schema_version"] = 1;
    page["resource"] = "user_hide_definitions";
    page["read_only"] = true;
    page["runtime_consistency"] = "per_rule_sampled";
    page["runtime_verified"] = false;
    page["scope"] = "caller_filesystem_definitions_and_kernel_registered_membership";
    page["boot_id"] = "boot-a";
    page["status"] = "observed";
    page["error"] = "";
    page["revision"] = rev;
    page["items"] = V::array();
    page["complete"] = false;
    page["managed_ownership"] = true;
    page["runtime_query"] = "observed";
    page["source_schema"] = 2;
    page["count"] = 4096;
    page["cursor"] = 4095;
    page["next_cursor"] = 4096;
    page["at_end"] = true;
    V item = V::object();
    item["id"] = id;
    item["path"] = "/private/do-not-disclose";
    item["name"] = "private-name";
    item["note"] = "private-note";
    item["group"] = "private-group";
    item["enabled"] = true;
    item["registered"] = true;
    item["state"] = "globally_suspended";
    item["runtime"] = V::object();
    auto& rt = item["runtime"];
    rt["id"] = "22";
    rt["generation"] = "0";
    rt["management"] = 1;
    rt["binding"] = 2;
    rt["errno"] = 0;
    page["items"].push_back(item);
    wrapper["status_page"] = page;
    return wrapper;
}
V rebuild() {
    V v = V::object();
    v["schema_version"] = 1;
    v["resource"] = "module_rebuild_status";
    v["read_only"] = true;
    v["read_status"] = "missing";
    v["intent_read_status"] = "missing";
    v["intent"] = V{};
    v["boot_id"] = "boot-a";
    v["context_consistent"] = true;
    v["same_boot"] = V{};
    for (const auto* key : {"runtime_verified", "owner_verified", "atomic_runtime_publication",
                            "history_snapshot_atomic", "admission_is_authorization"})
        v[key] = false;
    v["durability_confirmation"] = "not_observable_from_read";
    v["evidence_scope"] = "diagnostic_plan_and_legacy_call_acknowledgments";
    v["rebuild_admission"] = "allowed_by_retained_history_only";
    v["latest"] = V{};
    v["acknowledged"] = V{};
    return v;
}
V module() {
    V v = V::object();
    v["schema_version"] = 1;
    v["kind"] = "sumh.module.plan";
    v["read_only"] = true;
    v["status"] = "complete";
    v["errno"] = 0;
    v["stable"] = true;
    v["boot_id"] = "boot-a";
    v["config_revision"] = rev;
    v["module_configuration_revision"] = "sha256-v1:" + std::string(64, 'b');
    auto b = V::object();
    b["backend_decisions"] = V::array();
    b["backend_decisions"].push_back(producer::decision_json({"example.module", "/private/module"},
                                                             producer::ModuleBackendDecision{}));
    b["compiled"] = true;
    b["complete"] = true;
    b["other_backends_unplanned"] = true;
    b["events_truncated"] = false;
    b["observations_truncated"] = false;
    b["valid_for_current_inputs"] = true;
    v["baseline"] = b;
    auto own = V::object();
    own["external_owner_status"] = "not_observed";
    own["external_owner_absence_proven"] = false;
    own["hot_apply_authorized"] = false;
    v["ownership"] = own;
    return v;
}
V maps(unsigned count = 1) {
    auto v = producer::envelope();
    v["status"] = "complete";
    v["boot_id"] = "boot-a";
    v["config_revision"] = rev;
    auto& saved = v["saved"];
    saved["status"] = "valid";
    saved["enabled"] = true;
    saved["count"] = static_cast<int>(count);
    saved["maps_revision"] = "sha256-v1:" + std::string(64, 'b');
    auto& current = v["sample"];
    current["namespace_ino"] = "42";
    current["instance_id"] = "4";
    current["generation"] = "0";
    current["identities_sampled"] = true;
    current["definitions_complete"] = true;
    current["stable"] = true;
    current["comparison"] = "matches";
    for (unsigned i = 0; i < count; ++i) {
        V row = V::object();
        row["index"] = static_cast<int>(i);
        row["target_path"] = "/private/target";
        row["replacement_path"] = "/private/replacement";
        row["target"] = producer::identity_json({producer::MapsIdentityKind::Regular, 1 + i, 0, 0});
        row["replacement"] = producer::identity_json({});
        row["same_target_collision"] = false;
        row["stable"] = true;
        row["registered_match"] = "matches";
        v["rows"].push_back(row);
    }
    v["registered_count"] = static_cast<int>(count);
    v["last_submission"]["status"] = "missing";
    v["last_submission"]["stable"] = true;
    return v;
}
V recovery() {
    producer::lock_available = true;
    producer::counter_state = producer::marker_state = producer::ReadState::Missing;
    producer::counter_record = {};
    producer::marker_record = {};
    return parsed(producer::recovery_status_json(nullptr));
}
V retained(const std::string& id, const char* phase) {
    V v = V::object();
    v["id"] = id;
    v["boot_id"] = "older-boot";
    v["phase"] = phase;
    v["errno"] = 0;
    return v;
}
Request request(Scenario scenario) {
    Request r;
    r.scenario = scenario;
    if (scenario == Scenario::UserHide)
        r.definition_id = id;
    if (scenario == Scenario::ModuleFallback)
        r.module_id = "example.module";
    return r;
}
void validate_safety(const Report& r) {
    const auto bytes = encode_report(r);
    require(bytes.size() <= response_bytes, "bounded response");
    require(bytes.find("/private/") == std::string::npos &&
                bytes.find("private-note") == std::string::npos,
            "paths and notes not disclosed");
    require(!r.value.at("observer_verified").b && !r.value.at("atomic").b &&
                !r.value.at("mutations_performed").b,
            "limited observation flags");
    for (const auto& s : r.value.at("next_steps").a) {
        require(!s.at("evidence_ids").a.empty(), "advice cites evidence");
        if (s.contains("arguments"))
            require(s.at("kind").s == "read_only_query", "no mutation machine argv");
    }
}
void requests() {
    Request r;
    std::string reason;
    for (const auto scenario : {Scenario::UserHide, Scenario::ModuleFallback, Scenario::MapsFailure,
                                Scenario::BootProtection}) {
        auto input = V::object();
        input["schema_version"] = 1;
        input["scenario"] = scenario_name(scenario);
        if (scenario == Scenario::UserHide)
            input["definition_id"] = id;
        if (scenario == Scenario::ModuleFallback)
            input["module_id"] = "example.module";
        require(parse(stringify_json(input), r, reason), "four strict request scenarios");
        input["operation_id"] = opid;
        require(parse(stringify_json(input), r, reason), "optional operation selector");
        require(parse_channel_arguments({"doctor", "manager", stringify_json(input), "--channel"},
                                        r, reason),
                "exact private grammar");
        require(!parse_channel_arguments({"doctor", "manager", stringify_json(input)}, r, reason),
                "missing channel rejected");
        const Request before = r;
        for (const auto* key : {"path", "profile_id", "index", "enabled", "definition_revision"}) {
            auto bad = input;
            bad[key] = "extra";
            require(!parse(stringify_json(bad), r, reason), "unknown request fields rejected");
            require(r.scenario == before.scenario && r.operation_id == before.operation_id &&
                        r.module_id == before.module_id && r.definition_id == before.definition_id,
                    "failed parse preserves request");
        }
    }
    for (const auto& bytes : std::vector<std::string>{
             "{\"schema_version\":1,\"schema_version\":1,\"scenario\":\"maps-failure\"}",
             "{\"schema_version\":2,\"scenario\":\"maps-failure\"}",
             "{\"schema_version\":true,\"scenario\":\"maps-failure\"}",
             "{\"schema_version\":1,\"scenario\":\"maps-failure\",\"operation_id\":null}",
             "{\"schema_version\":1,\"scenario\":\"module-fallback\",\"module_id\":\"..\"}",
             "{\"schema_version\":1,\"scenario\":\"module-fallback\",\"module_id\":\"a\\u0000b\"}",
             "{\"schema_version\":1,\"scenario\":\"module-fallback\",\"module_id\":\"a\\ud800\"}",
             std::string(
                 "{\"schema_version\":1,\"scenario\":\"module-fallback\",\"module_id\":\"") +
                 char(0xff) + "\"}"})
        require(!parse(bytes, r, reason), "malformed strict request rejected");
    const std::string minimal = "{\"schema_version\":1,\"scenario\":\"maps-failure\"}";
    require(parse(minimal + std::string(input_bytes - minimal.size(), ' '), r, reason),
            "exact request capacity accepted");
    require(!parse(minimal + std::string(input_bytes - minimal.size() + 1, ' '), r, reason),
            "request capacity plus one rejected");
    const std::string module_prefix =
        "{\"schema_version\":1,\"scenario\":\"module-fallback\",\"module_id\":\"";
    const auto maximum_module =
        module_prefix + std::string(input_bytes - module_prefix.size() - 2, 'a') + "\"}";
    require(parse(maximum_module, r, reason), "exact capacity module selector accepted");
    auto at_capacity = sources(Scenario::ModuleFallback, module());
    at_capacity.rebuild = sample(SourceKind::ModuleRebuild, rebuild());
    require(diagnose(r, at_capacity).exit_code == 0,
            "accepted at capacity request remains valid through diagnose");
    for (unsigned byte = 0; byte < 256; ++byte) {
        const std::string value = std::string("a") + static_cast<char>(byte);
        require(valid_module_selector(value) == producer::valid_module_id(value),
                "module selector matches production validator for every byte");
    }
    for (const std::string value :
         {"", ".", "..", "a.b_c-d", "../foo", "a b", "\xe6\xa8\xa1\xe5\x9d\x97"})
        require(valid_module_selector(value) == producer::valid_module_id(value),
                "module selector edge equivalence");
}
void hide_tests() {
    auto v = hide();
    auto s = sources(Scenario::UserHide, v);
    auto r = request(Scenario::UserHide);
    auto result = diagnose(r, s);
    require(result.exit_code == 0, "selected hide beyond page16 observed");
    validate_safety(result);
    require(evidence(result, "hide.saved").at("value").at("enabled").b,
            "suspension does not disable saved definition");
    require(finding(result, "global_suspension_not_definition_disabled"),
            "global suspension finding");
    require(encode_report(result).find("4095") != std::string::npos,
            "read-only status uses exact cursor");
    auto& page = v["status_page"];
    auto& item = page["items"].a[0];
    item["runtime"]["management"] = 0;
    item["runtime"]["binding"] = 3;
    item["runtime"]["errno"] = -2;
    item["state"] = "binding_error";
    s.primary = sample(SourceKind::HideSelection, v);
    result = diagnose(r, s);
    require(result.exit_code == 0 && finding(result, "registered_binding_error"),
            "binding error is observed not healthy");
    page["managed_ownership"] = false;
    page["runtime_query"] = "unsupported";
    item["registered"] = V{};
    item["runtime"] = V{};
    item["state"] = "unknown";
    s.primary = sample(SourceKind::HideSelection, v);
    result = diagnose(r, s);
    require(
        result.exit_code == 0 && evidence(result, "hide.runtime").at("status").s == "unsupported",
        "unsupported is known fact");
    page["managed_ownership"] = V{};
    page["runtime_query"] = "unavailable";
    s.primary = sample(SourceKind::HideSelection, v);
    result = diagnose(r, s);
    require(result.exit_code == 3 && evidence(result, "hide.runtime").at("value").is_null(),
            "unknown bool not false membership");
    page.o.erase("managed_ownership");
    s.primary = sample(SourceKind::HideSelection, v);
    require(diagnose(r, s).value.at("reason").s == "source_contract_invalid",
            "missing required nullable field rejected");
    v = hide();
    v["status_page"]["items"] = V::array();
    s.primary = sample(SourceKind::HideSelection, v);
    require(diagnose(r, s).exit_code == 3, "missing selected page item not absent");
    v["selection_status"] = "absent";
    v["saved_definition"] = V{};
    v["status_page"] = V{};
    s.primary = sample(SourceKind::HideSelection, v);
    require(diagnose(r, s).exit_code == 0 &&
                !evidence(diagnose(r, s), "hide.saved").at("value").at("definition_present").b,
            "stable full-store absence known");
    v["definition_store_stable"] = false;
    s.primary = sample(SourceKind::HideSelection, v);
    require(diagnose(r, s).exit_code == 5, "unstable absent wrapper rejected");
    v = hide();
    v["selection_status"] = "unavailable";
    v["definition_store_stable"] = false;
    v["revision"] = V{};
    v["saved_definition"] = V{};
    v["status_page"] = V{};
    s.primary = sample(SourceKind::HideSelection, v);
    require(diagnose(r, s).exit_code == 5 &&
                diagnose(r, s).value.at("reason").s == "primary_source_unavailable",
            "history cannot elevate unavailable hide selection to observed");
}
void module_tests() {
    auto v = module();
    auto s = sources(Scenario::ModuleFallback, v);
    s.rebuild = sample(SourceKind::ModuleRebuild, rebuild());
    auto r = request(Scenario::ModuleFallback);
    auto result = diagnose(r, s);
    require(result.exit_code == 0, "actual encoded module decision observed");
    validate_safety(result);
    require(evidence(result, "module.selection").at("classification").s == "inferred",
            "Planner is inference");
    require(finding(result, "planner_selected_fallback"), "fallback preserved");
    v["status"] = "partial";
    v["baseline"]["complete"] = false;
    v["baseline"]["valid_for_current_inputs"] = false;
    s.primary = sample(SourceKind::ModulePlan, v);
    require(diagnose(r, s).exit_code == 3, "complete selected row does not erase producer partial");
    v = module();
    v["baseline"]["backend_decisions"] = V::array();
    s.primary = sample(SourceKind::ModulePlan, v);
    result = diagnose(r, s);
    require(
        result.exit_code == 0 && finding(result, "mountable_absence_does_not_prove_uninstalled"),
        "mountable absence not uninstalled");
    v["status"] = "partial";
    v["baseline"]["complete"] = false;
    s.primary = sample(SourceKind::ModulePlan, v);
    require(diagnose(r, s).exit_code == 5, "incomplete set cannot establish absent target");
    v = module();
    v["baseline"]["backend_decisions"].a[0]["module_id"] = "example.module";
    s.primary = sample(SourceKind::ModulePlan, v);
    require(diagnose(r, s).exit_code == 5, "string module id source contract rejected");
    v = module();
    v["baseline"]["backend_decisions"].a[0]["module_id"]["byte_length"] = 2;
    s.primary = sample(SourceKind::ModulePlan, v);
    require(diagnose(r, s).exit_code == 5, "encoded length checked");
    v = module();
    auto unrelated = v["baseline"]["backend_decisions"].a[0];
    unrelated["module_id"] = producer::exact("other.module");
    unrelated["sumh_probe"].o.erase("available");
    v["baseline"]["backend_decisions"].push_back(unrelated);
    s.primary = sample(SourceKind::ModulePlan, v);
    require(diagnose(r, s).value.at("reason").s == "source_contract_invalid",
            "unselected module decisions validated too");
    v = module();
    s.primary = sample(SourceKind::ModulePlan, v);
    auto h = rebuild();
    h["read_status"] = "valid";
    h["latest"] = retained(rebuildid, "partial_or_unknown");
    h["rebuild_admission"] = "blocked_unresolved_same_boot";
    s.rebuild = sample(SourceKind::ModuleRebuild, h);
    result = diagnose(r, s);
    require(result.exit_code == 0 && finding(result, "unresolved_rebuild_no_replay"),
            "rebuild latch advisory only");
    require(!step(result, "show_selected_operation"), "rebuild ID not operation ID");
    s.rebuild->failure = "raw /private/error";
    result = diagnose(r, s);
    require(result.exit_code == 3, "ancillary unavailable partial");
    validate_safety(result);
}
void maps_tests() {
    auto v = maps(256);
    auto s = sources(Scenario::MapsFailure, v);
    auto r = request(Scenario::MapsFailure);
    auto result = diagnose(r, s);
    require(result.exit_code == 0, "maps maximum row count accepted");
    validate_safety(result);
    const auto& summary = evidence(result, "maps.sample").at("value");
    require(summary.at("row_count").n == 256 && summary.at("omitted_count").n == 240 &&
                summary.at("rows").a.size() == 16,
            "all rows counted bounded summaries");
    v["rows"].a[255]["target"] =
        producer::identity_json({producer::MapsIdentityKind::Missing, 0, 0, 2});
    v["rows"].a[255]["registered_match"] = "unknown";
    v["sample"]["comparison"] = "unknown";
    s.primary = sample(SourceKind::MapsPreview, v);
    result = diagnose(r, s);
    require(result.exit_code == 0 && finding(result, "maps_paths_unresolved"),
            "out of summary missing path counted; observed need not healthy");
    v["rows"].a[255]["same_target_collision"] = true;
    v["sample"]["comparison"] = "drift";
    s.primary = sample(SourceKind::MapsPreview, v);
    require(finding(diagnose(r, s), "maps_target_identity_collision"), "collision from full set");
    v["last_submission"]["status"] = "valid";
    v["last_submission"]["latest"] = retained(submissionid, "acknowledged");
    v["last_submission"]["latest"]["definition_revision"] = "sha256-v1:" + std::string(64, 'c');
    v["last_submission"]["confirmed"] = v["last_submission"]["latest"];
    s.primary = sample(SourceKind::MapsPreview, v);
    result = diagnose(r, s);
    require(result.exit_code == 0 &&
                evidence(result, "maps.history").at("classification").s == "historical",
            "unlike revision hashes not compared");
    require(!step(result, "show_selected_operation"), "Maps ID not operation ID");
    v["last_submission"]["latest"]["phase"] = "pending";
    s.primary = sample(SourceKind::MapsPreview, v);
    require(!step(diagnose(r, s), "review_maps_apply"),
            "pending submission suppresses apply advice");
    v = maps();
    v["last_submission"]["status"] = "unreadable";
    v["status"] = "partial";
    s.primary = sample(SourceKind::MapsPreview, v);
    require(diagnose(r, s).exit_code == 3 && !step(diagnose(r, s), "review_maps_apply"),
            "unknown submission history suppresses changes");
    for (const auto* flag : {"stable", "definitions_complete", "identities_sampled"}) {
        v = maps();
        v["sample"][flag] = false;
        s.primary = sample(SourceKind::MapsPreview, v);
        require(diagnose(r, s).value.at("reason").s == "source_contract_invalid",
                "complete Maps snapshot must have complete stable samples");
    }
    v = maps();
    v["status"] = "partial";
    v["sample"]["definitions_complete"] = false;
    v["sample"]["comparison"] = "unknown";
    v["registered_count"] = V{};
    v["rows"].a[0]["registered_match"] = "unknown";
    v["sample"]["errno"] = 95;
    s.primary = sample(SourceKind::MapsPreview, v);
    result = diagnose(r, s);
    require(result.exit_code == 3 && evidence(result, "maps.sample").at("status").s == "known",
            "partial identity sample preserved without claiming unsupported inventory");
    require(evidence(result, "maps.sample").at("value").at("comparison").s == "unknown",
            "unknown inventory not replaced by comparison inference");
    v["sample"]["identities_sampled"] = false;
    s.primary = sample(SourceKind::MapsPreview, v);
    require(evidence(diagnose(r, s), "maps.sample").at("value").is_null(),
            "unsampled paths not observed");
    v = producer::envelope();
    v["saved"]["status"] = "missing";
    s.primary = sample(SourceKind::MapsPreview, v);
    require(diagnose(r, s).exit_code == 5, "missing config count0 is unknown not empty");
    v = maps();
    v["saved"].o.erase("enabled");
    s.primary = sample(SourceKind::MapsPreview, v);
    require(diagnose(r, s).exit_code == 5, "missing bool schema rejected");
    v = maps();
    v["sample"]["namespace_ino"] = "43";
    s.primary = sample(SourceKind::MapsPreview, v);
    require(diagnose(r, s).exit_code == 4 && diagnose(r, s).value.at("next_steps").a.empty(),
            "Maps namespace mismatch stale no advice");
}
void recovery_tests() {
    auto s = sources(Scenario::BootProtection, recovery());
    auto r = request(Scenario::BootProtection);
    auto result = diagnose(r, s);
    require(result.exit_code == 0 &&
                evidence(result, "recovery.storage").at("value").at("boot_attempts").n == 0,
            "actual legacy serializer zero missing state");
    producer::marker_state = producer::ReadState::Valid;
    producer::marker_record.boot_id = "older-boot";
    s.primary = sample(SourceKind::RecoveryLegacy, parsed(producer::recovery_status_json(nullptr)));
    result = diagnose(r, s);
    require(result.exit_code == 0 && result.value.at("boot_id").s == "boot-a",
            "record boot not current boot");
    require(evidence(result, "recovery.origin").at("value").at("record_origin_boot_id").s ==
                "older-boot",
            "marker record labeled historical");
    require(step(result, "review_recovery_reset"), "explicit reset description available");
    validate_safety(result);
    producer::marker_state = producer::ReadState::Corrupt;
    producer::marker_record = {};
    s.primary = sample(SourceKind::RecoveryLegacy, parsed(producer::recovery_status_json(nullptr)));
    result = diagnose(r, s);
    require(result.exit_code == 3 &&
                !evidence(result, "recovery.storage").at("value").at("marker_payload_trusted").b,
            "corrupt marker conservative gate not trusted payload");
    require(!step(result, "review_recovery_reset"), "corrupt recovery no reset advice");
    auto v = recovery();
    v["schema_version"] = 1;
    s.primary = sample(SourceKind::RecoveryLegacy, v);
    require(diagnose(r, s).exit_code == 5, "unknown legacy wrapper rejected");
}
void operation_tests() {
    auto s = sources(Scenario::UserHide, hide());
    auto r = request(Scenario::UserHide);
    auto h = history();
    h["ledger_status"] = "valid";
    h["history_pruned"] = true;
    for (int i = 0; i < 16; ++i) {
        const std::string record_id(32, "0123456789abcdef"[i]);
        producer::Operation op{record_id, "hide retry",      "started", "boot-a", 0,
                               0,         "/private/secret", rev};
        h["records"].push_back(producer::operation_record_json(op, "boot-a"));
    }
    s.operations = sample(SourceKind::Operations, h);
    auto result = diagnose(r, s);
    require(result.exit_code == 0 && result.value.at("recent_operations").a.size() == 4,
            "bounded operation history candidates");
    require(result.value.at("recent_operations").a[0].at("correlation").s ==
                "same_command_family_candidate",
            "category not target correlation");
    r.operation_id = opid;
    auto selected = history(opid);
    selected["ledger_status"] = "valid";
    selected["history_pruned"] = false;
    selected["status"] = "observed";
    producer::Operation op{opid, "hide retry", "started", "boot-a", 0, 0, "", rev};
    selected["records"].push_back(producer::operation_record_json(op, "boot-a"));
    s.selected_operation = sample(SourceKind::OperationSelected, selected);
    result = diagnose(r, s);
    require(result.exit_code == 0 && step(result, "sample_selected_operation"),
            "selected running operation zero-budget query");
    require(encode_report(result).find("\"wait\", \"" + opid + "\", \"0\"") != std::string::npos,
            "wait grammar requires explicit zero timeout");
    require(!step(result, "read_selected_operation_receipt"),
            "no receipt for unfinished operation");
    require(!step(result, "review_hide_retry"), "unresolved operation blocks hide retry prose");
    op.phase = "finished";
    selected["records"].a[0] = producer::operation_record_json(op, "boot-a");
    s.selected_operation = sample(SourceKind::OperationSelected, selected);
    require(step(diagnose(r, s), "read_selected_operation_receipt"),
            "finished stable hide operation receipt navigation");
    op.name = "sumh maps apply";
    selected["records"].a[0] = producer::operation_record_json(op, "boot-a");
    s.selected_operation = sample(SourceKind::OperationSelected, selected);
    require(!step(diagnose(r, s), "read_selected_operation_receipt"),
            "no incompatible operation receipt navigation");
    s.selected_operation = sample(SourceKind::OperationSelected, history(opid));
    s.selected_operation->query_exit_code = 3;
    require(diagnose(r, s).exit_code == 0, "not retained exit3 is normal absence");
    s.operations.failure = "history unreadable";
    result = diagnose(r, s);
    require(result.exit_code == 3 && !step(result, "review_hide_retry"),
            "history unavailable downgrades partial and suppresses change prose");
    s.selected_operation->kind = SourceKind::Operations;
    require(diagnose(r, s).exit_code == 5, "source slot mismatch fails closed");
    auto maps_sources = sources(Scenario::MapsFailure, maps());
    auto maps_request = request(Scenario::MapsFailure);
    maps_request.operation_id = opid;
    op.phase = "started";
    selected["records"].a[0] = producer::operation_record_json(op, "boot-a");
    maps_sources.selected_operation = sample(SourceKind::OperationSelected, selected);
    result = diagnose(maps_request, maps_sources);
    require(result.exit_code == 0 && !step(result, "review_maps_apply") &&
                step(result, "sample_selected_operation"),
            "selected started operation blocks Maps apply and offers query");
    maps_sources.selected_operation->failure = "history unavailable";
    require(!step(diagnose(maps_request, maps_sources), "review_maps_apply"),
            "failed selected history also blocks Maps apply");
    producer::marker_state = producer::ReadState::Valid;
    producer::marker_record.boot_id = "older-boot";
    auto recovery_sources =
        sources(Scenario::BootProtection, parsed(producer::recovery_status_json(nullptr)));
    recovery_sources.operations.failure = "history unavailable";
    require(!step(diagnose(request(Scenario::BootProtection), recovery_sources),
                  "review_recovery_reset"),
            "unknown history blocks recovery reset advice");
    h = history();
    h["ledger_status"] = "valid";
    h["history_pruned"] = false;
    h["records"].push_back(producer::operation_record_json(op, "boot-a"));
    h["records"].a[0]["same_boot"] = false;
    s = sources(Scenario::UserHide, hide());
    s.operations = sample(SourceKind::Operations, h);
    require(
        diagnose(request(Scenario::UserHide), s).value.at("reason").s == "source_contract_invalid",
        "same boot output cannot contradict record origin");
}
void boundary_tests() {
    auto r = request(Scenario::BootProtection);
    auto s = sources(Scenario::BootProtection, recovery());
    s.context.boot_after = "boot-b";
    auto result = diagnose(r, s);
    require(result.exit_code == 4 && result.value.at("findings").a.empty() &&
                result.value.at("next_steps").a.empty(),
            "boot change retracts findings advice");
    require(evidence(result, "recovery.storage").at("value").is_null(),
            "stale current values null");
    s.context.boot_after = "boot-a";
    s.context.namespace_consistent = false;
    require(diagnose(r, s).exit_code == 4, "namespace change stale");
    s.context.namespace_consistent = true;
    s.context.namespace_verified = false;
    result = diagnose(r, s);
    require(result.exit_code == 3 &&
                evidence(result, "recovery.storage").at("status").s == "unknown" &&
                result.value.at("next_steps").a.empty(),
            "unknown namespace no current inference");
    s.context.namespace_verified = true;
    s.primary.bytes += std::string(65536 - s.primary.bytes.size(), ' ');
    require(diagnose(r, s).exit_code == 0, "exact source capacity accepted");
    s.primary.query_exit_code = 7;
    require(diagnose(r, s).exit_code == 3, "producer failure exit cannot become fully observed");
    s.primary.query_exit_code = 0;
    s.primary.bytes += ' ';
    require(diagnose(r, s).exit_code == 5, "source capacity plus one rejected");
    s.primary.failure = "untrusted /private/error";
    result = diagnose(r, s);
    require(result.exit_code == 5, "failed source never parses retained bytes");
    validate_safety(result);
    s = sources(Scenario::BootProtection, recovery());
    auto v = parsed(s.primary.bytes);
    v["boot_attempts"] = false;
    s.primary.bytes = stringify_json(v);
    require(diagnose(r, s).value.at("reason").s == "source_contract_invalid",
            "wrong source types rejected");
    result = unavailable(&r, "raw /private/error", 9999);
    require(
        result.value.at("reason").s == "collector_unavailable" && result.value.at("errno").n == 0,
        "raw errors not echoed");
    for (const auto* reason : {"init_namespace_unavailable", "transport_contract_invalid",
                               "response_contract_invalid", "input_capacity", "invalid_arguments"})
        require(unavailable(&r, reason, 5).value.at("reason").s == reason,
                "fixed collector reasons retained");
    result.value["padding"] = std::string(response_bytes, 'x');
    const auto fallback = parsed(encode_report(result));
    require(fallback.at("reason").s == "response_capacity" && fallback.at("exit_code").n == 5,
            "oversize report small valid fallback");
    require(fallback.at("scenario").s == "boot-protection",
            "capacity fallback preserves validated scenario for public collector validation");
    result = unavailable(&r, "collector_unavailable", 0);
    result.value["evidence"] = V::array();
    for (int i = 0; i < 17; ++i)
        result.value["evidence"].push_back(V::object());
    require(parsed(encode_report(result)).at("reason").s == "response_capacity",
            "evidence budget enforced");
    r.module_id = "unexpected";
    require(diagnose(r, s).exit_code == 2, "invalid direct request rejected");
}
void fixtures(const std::filesystem::path& root) {
    for (const auto scenario : {Scenario::UserHide, Scenario::ModuleFallback, Scenario::MapsFailure,
                                Scenario::BootProtection}) {
        const auto v = scenario == Scenario::UserHide         ? hide()
                       : scenario == Scenario::ModuleFallback ? module()
                       : scenario == Scenario::MapsFailure    ? maps()
                                                              : recovery();
        auto s = sources(scenario, v);
        if (scenario == Scenario::ModuleFallback)
            s.rebuild = sample(SourceKind::ModuleRebuild, rebuild());
        std::ofstream(root / (std::string(scenario_name(scenario)) + "-source.json"))
            << stringify_json(v) << '\n';
        std::ofstream(root / (std::string(scenario_name(scenario)) + "-report.json"))
            << encode_report(diagnose(request(scenario), s)) << '\n';
    }
}
}  // namespace
int main(int argc, char** argv) {
    requests();
    hide_tests();
    module_tests();
    maps_tests();
    recovery_tests();
    operation_tests();
    boundary_tests();
    if (argc > 1)
        fixtures(argv[1]);
    std::cout << "PASS diagnostics model: " << assertions
              << " assertions; production module/maps/operation/recovery serializers and module "
                 "selector; host-only\n";
}
