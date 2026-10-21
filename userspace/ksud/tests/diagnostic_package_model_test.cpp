#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include "core/diagnostic_package.hpp"
#include "core/diagnostics_model.hpp"

using V = sumhp::JsonValue;
using namespace sumhp::diagnostic_package;
namespace {
std::size_t checks = 0, cases = 0;
void require(bool value, const char* description) {
    ++checks;
    if (!value) {
        std::cerr << "FAIL " << description << '\n';
        std::exit(1);
    }
}
std::string bytes(const V& value) {
    return sumhp::stringify_json(value);
}
V& evidence(V& report, const std::string& id) {
    for (auto& e : report["evidence"].a)
        if (e.at("id").s == id)
            return e;
    std::cerr << "missing fixture evidence " << id << '\n';
    std::exit(1);
}
V load(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good(), "fixture exists");
    std::string content((std::istreambuf_iterator<char>(stream)), {}), error;
    V out;
    require(json::Parser(content, true).parse(out, error), "fixture parses");
    return out;
}
V package(const V& report, bool include = false) {
    V out;
    std::string reason;
    require(build_package(bytes(report), include, out, reason), "source report packages");
    require(reason.empty(), "success clears reason");
    return out;
}
void roundtrip(const V& p) {
    V parsed = "sentinel";
    std::string reason = "sentinel";
    require(parse_package(bytes(p) + '\n', parsed, reason),
            "canonical package parses with export LF");
    require(bytes(parsed) == bytes(p) && reason.empty(), "canonical parse exact content");
}
void rejected_report(const std::string& report) {
    V result = "sentinel";
    std::string reason;
    require(!build_package(report, false, result, reason), "malformed source rejected");
    require(result.s == "sentinel" && !reason.empty(),
            "source failure preserves output and has fixed reason");
    require(reason.find("CANARY") == std::string::npos, "reason never repeats source");
}
void rejected_package(const V& p) {
    V result = "sentinel";
    std::string reason;
    require(!parse_package(bytes(p), result, reason), "tampered package rejected");
    require(result.s == "sentinel", "stored failure preserves output");
}
void add_unknowns(V& value) {
    if (value.is_object()) {
        for (auto& item : value.o)
            add_unknowns(item.second);
        value["CANARY_UNKNOWN_KEY"] = "/private/CANARY_SECRET/凭据";
    } else if (value.is_array())
        for (auto& item : value.a)
            add_unknowns(item);
}
V operation(const std::string& id, const std::string& boot) {
    auto v = V::object();
    v["id"] = id;
    v["boot_id"] = boot;
    v["phase"] = "started";
    v["result"] = "outcome_unknown";
    v["same_boot"] = true;
    v["exit_code"] = V{};
    v["errno"] = V{};
    v["correlation"] = "user_selected_not_target_proven";
    v["target_verified"] = false;
    v["runtime_verified"] = false;
    return v;
}
void fixtures(const std::filesystem::path& directory) {
    for (const auto* scenario :
         {"rule-not-effective", "module-fallback", "maps-failure", "boot-protection"}) {
        ++cases;
        auto r = load(directory / (std::string(scenario) + "-report.json"));
        require(r.at("status").s == "observed", "real producer fixture observed");
        auto p = package(r);
        roundtrip(p);
        require(p.at("kind").s == "sumh.diagnostic.package" && p.at("schema_version").n == 1,
                "package schema");
        require(p.at("privacy").at("policy").s == "redacted", "redaction default metadata");
        require(p.at("privacy").at("alias_correlation_scope").s == "within_package_only",
                "correlation scope explicit");
        auto& d = p["diagnostic"];
        require(d.at("boot_id").s == "boot-1" && d.at("namespace_dev").s == "namespace-dev-1" &&
                    d.at("namespace_ino").s == "namespace-ino-1",
                "typed context aliases");
        require(!d.at("atomic").b && !d.at("observer_verified").b && !d.at("mutations_performed").b,
                "no atomicity or observer claims");
        require(d.at("sample_timing").at("status").s == "relative" &&
                    d.at("sample_timing").at("start_offset_ms").s == "0",
                "relative report timing");
        require(!d.contains("sample_started_monotonic_ms") &&
                    !d.contains("sample_finished_monotonic_ms"),
                "raw uptime absent");
        for (const auto& e : d.at("evidence").a) {
            require(e.at("sample_timing").at("start_offset_ms").s == "2" &&
                        e.at("sample_timing").at("duration_ms").s == "6",
                    "relative query timing useful");
        }
        for (const auto& s : d.at("next_steps").a)
            require(!s.contains("description") && !s.contains("arguments") &&
                        !s.contains("argv_prefix"),
                    "no executable or descriptive commands");
        auto noisy = r;
        add_unknowns(noisy);
        for (auto& step : noisy["next_steps"].a) {
            step["description"] = "CANARY_COMMAND_DESCRIPTION";
            step["argv_prefix"] = V::array();
            step["argv_prefix"].push_back("CANARY_COMMAND");
            step["arguments"] = V::array();
            step["arguments"].push_back("/CANARY_PATH");
        }
        auto clean = package(noisy);
        require(bytes(clean) == bytes(p) && bytes(clean).find("CANARY") == std::string::npos,
                "all nested unknowns and command text omitted");
        auto raw = package(noisy, true);
        roundtrip(raw);
        require(raw.at("diagnostic").at("boot_id").s == r.at("boot_id").s,
                "explicit identifiers retain validated boot");
        require(raw.at("privacy").at("policy").s == "identifiers_included" &&
                    bytes(raw).find("CANARY") == std::string::npos,
                "identifier opt-in never raw fields");
        auto tampered = p;
        tampered["privacy"]["notes"] = "CANARY";
        rejected_package(tampered);
        tampered = p;
        tampered["diagnostic"]["evidence"].a[0]["secret"] = "CANARY";
        rejected_package(tampered);
        tampered = p;
        tampered["diagnostic"]["boot_id"] = "boot-a";
        rejected_package(tampered);
        tampered = p;
        tampered["diagnostic"]["boot_id"] = "boot-7";
        rejected_package(tampered);
        tampered = p;
        tampered["diagnostic"]["observer_verified"] = true;
        rejected_package(tampered);
        tampered = p;
        tampered["diagnostic"]["sample_timing"]["raw"] = "100";
        rejected_package(tampered);
        noisy = r;
        noisy["evidence"].a[0]["source"] = "CANARY_SOURCE";
        rejected_report(bytes(noisy));
        noisy = r;
        noisy["evidence"].a[0]["reason"] = "CANARY_REASON";
        rejected_report(bytes(noisy));
        noisy = r;
        noisy["schema_version"] = 2;
        rejected_report(bytes(noisy));
        auto missing_time = r;
        missing_time["sample_finished_monotonic_ms"] = "1";
        auto omitted = package(missing_time);
        roundtrip(omitted);
        require(omitted.at("diagnostic").at("sample_timing").at("status").s == "omitted",
                "regressed clock explicitly omitted");
        for (const auto& e : omitted.at("diagnostic").at("evidence").a)
            require(e.at("sample_timing").at("status").s == "omitted",
                    "query timing omitted without base");
    }
}
void aliases(const std::filesystem::path& directory) {
    ++cases;
    auto r = load(directory / "rule-not-effective-report.json");
    const std::string id(32, 'f');
    r["target"]["id"] = id;
    r["recent_operations"].push_back(operation(id, r.at("boot_id").s));
    auto selected = evidence(r, "operation.history");
    selected["id"] = "operation.selected";
    selected["value"]["requested_operation_id"] = id;
    r["evidence"].push_back(selected);
    auto p = package(r);
    roundtrip(p);
    require(p.at("diagnostic").at("target").at("id").s == "definition-1",
            "stable definition pseudonym");
    auto& d = p["diagnostic"];
    require(evidence(d, "operation.selected").at("value").at("requested_operation_id").s ==
                    "operation-1" &&
                d.at("recent_operations").a[0].at("id").s == "operation-1",
            "selected operation equality retained");
    require(d.at("recent_operations").a[0].at("boot_id").s == "boot-1",
            "boot equality across contexts");
    require(bytes(p).find(id) == std::string::npos, "raw ID absent");
    r["recent_operations"].push_back(operation(std::string(32, 'e'), "older-boot"));
    r["recent_operations"].a[1]["same_boot"] = false;
    p = package(r);
    roundtrip(p);
    require(p.at("diagnostic").at("recent_operations").a[1].at("id").s == "operation-2" &&
                p.at("diagnostic").at("recent_operations").a[1].at("boot_id").s == "boot-2",
            "distinct operation and boot identities distinct");
    auto m = load(directory / "module-fallback-report.json");
    evidence(m, "module.selection")["value"]["module_configuration_revision"] =
        evidence(m, "module.selection").at("value").at("config_revision");
    auto& rebuild = evidence(m, "module.rebuild")["value"];
    auto rec = V::object();
    rec["rebuild_id"] = id;
    rec["boot_id"] = m.at("boot_id");
    rec["phase"] = "acknowledged";
    rec["errno"] = 0;
    rec["runtime_verified"] = false;
    rebuild["latest"] = rec;
    rebuild["acknowledged"] = rec;
    rebuild["intent"] = V::object();
    rebuild["intent"]["rebuild_id"] = id;
    rebuild["intent"]["boot_id"] = "older-boot";
    p = package(m);
    roundtrip(p);
    const auto& red = evidence(p["diagnostic"], "module.rebuild").at("value");
    require(red.at("latest").at("rebuild_id").s == "rebuild-1" &&
                red.at("acknowledged").at("rebuild_id").s == "rebuild-1" &&
                red.at("intent").at("rebuild_id").s == "rebuild-1",
            "rebuild references share alias");
    const auto& selection = evidence(p["diagnostic"], "module.selection").at("value");
    require(selection.at("config_revision").s == selection.at("module_configuration_revision").s,
            "equal revisions retain equality");
    auto maps = load(directory / "maps-failure-report.json");
    auto& history = evidence(maps, "maps.history")["value"];
    rec.o.erase("rebuild_id");
    rec["submission_id"] = id;
    history["latest"] = rec;
    history["confirmed"] = rec;
    p = package(maps);
    roundtrip(p);
    require(
        evidence(p["diagnostic"], "maps.history").at("value").at("latest").at("submission_id").s ==
            "submission-1",
        "submission domain alias");
}
void boundaries(const std::filesystem::path& directory) {
    ++cases;
    auto r = load(directory / "boot-protection-report.json");
    for (const auto* status : {"partial", "stale", "unavailable", "invalid"}) {
        auto v = r;
        v["status"] = status;
        v["exit_code"] = std::string(status) == "partial"   ? 3
                         : std::string(status) == "stale"   ? 4
                         : std::string(status) == "invalid" ? 2
                                                            : 5;
        auto p = package(v);
        roundtrip(p);
        require(p.at("diagnostic").at("status").s == status, "source outcome never upgraded");
    }
    auto encoded = bytes(r);
    require(encoded.size() < 65536, "fixture fits boundary");
    V output;
    std::string reason;
    require(
        build_package(encoded + std::string(65536 - encoded.size(), ' '), false, output, reason),
        "exact source byte cap accepted");
    rejected_report(encoded + std::string(65537 - encoded.size(), ' '));
    rejected_report("{\"schema_version\":1,\"schema_version\":1}");
    rejected_report("{\"schema_version\":1,\"CANARY\":\"\\u0000\"}");
    rejected_report(encoded + std::string(1, static_cast<char>(0xff)));
    rejected_report("{\"CANARY\":\"\\ud800\"}");
    auto p = package(r);
    auto tampered = p;
    tampered["schema_version"] = 2;
    rejected_package(tampered);
    tampered = p;
    tampered["privacy"]["policy"] = "raw";
    rejected_package(tampered);
    tampered = p;
    tampered["diagnostic"]["next_steps"].a[0]["arguments"] = V::array();
    rejected_package(tampered);
    output = "sentinel";
    require(!parse_package("{\"schema_version\":1,\"schema_version\":1}", output, reason) &&
                output.s == "sentinel",
            "duplicate stored keys rejected");
    require(!parse_package(std::string(package_limit + 1, ' '), output, reason),
            "stored cap plus one rejected");
    auto include = package(r, true);
    include["diagnostic"]["boot_id"] = "/private/CANARY";
    rejected_package(include);
    auto invalid = r;
    invalid["boot_id"] = "/private/CANARY";
    rejected_report(bytes(invalid));
    invalid = r;
    invalid["evidence"].a[0]["value"]["counter_state"] = "CANARY";
    rejected_report(bytes(invalid));
    require(valid_id(std::string(32, 'a')) && !valid_id(std::string(32, 'A')) &&
                !valid_id("../package"),
            "package ID grammar");
    require(valid_revision("sha256-v1:" + std::string(64, 'a')) &&
                !valid_revision("sha256-v1:" + std::string(64, 'g')),
            "revision grammar");
    auto unavailable = sumhp::diagnostics::unavailable(nullptr, "transport_unavailable", 5);
    auto unavailable_package = package(unavailable.value);
    roundtrip(unavailable_package);
    require(
        unavailable_package.at("diagnostic").at("status").s == "unavailable" &&
            unavailable_package.at("diagnostic").at("target").o.empty() &&
            unavailable_package.at("diagnostic").at("sample_timing").at("status").s == "omitted",
        "real unavailable report retained without invented context");
    for (const auto scenario :
         {sumhp::diagnostics::Scenario::UserHide, sumhp::diagnostics::Scenario::ModuleFallback,
          sumhp::diagnostics::Scenario::MapsFailure,
          sumhp::diagnostics::Scenario::BootProtection}) {
        sumhp::diagnostics::Request request;
        request.scenario = scenario;
        request.definition_id = std::string(32, 'a');
        request.module_id = "example.module";
        unavailable = sumhp::diagnostics::unavailable(&request, "source_contract_invalid", 0);
        unavailable_package = package(unavailable.value);
        roundtrip(unavailable_package);
        require(unavailable_package.at("diagnostic").at("reason").s == "source_contract_invalid",
                "real four-scenario unavailable reason preserved");
    }
}
}  // namespace
int main(int argc, char** argv) {
    require(argc == 2, "fixture directory argument");
    const std::filesystem::path directory(argv[1]);
    fixtures(directory);
    aliases(directory);
    boundaries(directory);
    std::cout << "PASS diagnostic package model: " << cases << " groups, " << checks
              << " always-on checks; four current production doctor fixtures; host-only\n";
}
