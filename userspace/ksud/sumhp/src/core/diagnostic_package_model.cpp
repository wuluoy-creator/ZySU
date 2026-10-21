#include "core/diagnostic_package.hpp"
#include "userspace/common/diagnostics_request.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>

namespace sumhp::diagnostic_package {
namespace {
using V = JsonValue;
using Words = std::initializer_list<const char*>;
bool one(const V& v, Words words) {
    return v.is_string() &&
           std::any_of(words.begin(), words.end(), [&](const char* word) { return v.s == word; });
}
bool integer(const V& v, double maximum, double minimum = 0) {
    return v.is_number() && std::isfinite(v.n) && std::floor(v.n) == v.n && v.n >= minimum &&
           v.n <= maximum;
}
bool decimal(const std::string& v) {
    return !v.empty() && v.size() <= 20 && (v == "0" || v.front() != '0') &&
           v.find_first_not_of("0123456789") == std::string::npos &&
           (v.size() < 20 || v <= "18446744073709551615");
}
bool token(const std::string& v) {
    return !v.empty() && v.size() <= 80 && std::all_of(v.begin(), v.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_' || c == '.';
    });
}
bool safe_tree(const V& value) {
    if (value.is_string())
        return ksud::text::valid_utf8(value.s) && value.s.find('\0') == std::string::npos;
    for (const auto& field : value.o)
        if (!ksud::text::valid_utf8(field.first) || field.first.find('\0') != std::string::npos ||
            !safe_tree(field.second))
            return false;
    return std::all_of(value.a.begin(), value.a.end(), safe_tree);
}
V strings(Words values) {
    auto v = V::array();
    for (const char* text : values)
        v.push_back(text);
    return v;
}
V privacy(bool include) {
    auto v = V::object();
    v["policy"] = include ? "identifiers_included" : "redacted";
    v["alias_correlation_scope"] = include ? "validated_identifiers" : "within_package_only";
    v["observer_verified"] = false;
    v["omitted_fields"] =
        strings({"paths", "notes", "raw_logs", "credentials", "environment", "command_arguments",
                 "command_descriptions", "raw_monotonic_timestamps", "unknown_fields",
                 "alias_reverse_map"});
    return v;
}
enum class Identity : std::uint8_t {
    Boot,
    NamespaceDev,
    NamespaceIno,
    Definition,
    Module,
    Operation,
    Rebuild,
    Submission,
    Revision,
    Rule,
    Generation
};
const char* prefix(Identity kind) {
    switch (kind) {
    case Identity::Boot:
        return "boot";
    case Identity::NamespaceDev:
        return "namespace-dev";
    case Identity::NamespaceIno:
        return "namespace-ino";
    case Identity::Definition:
        return "definition";
    case Identity::Module:
        return "module";
    case Identity::Operation:
        return "operation";
    case Identity::Rebuild:
        return "rebuild";
    case Identity::Submission:
        return "submission";
    case Identity::Revision:
        return "revision";
    case Identity::Rule:
        return "rule";
    case Identity::Generation:
        return "generation";
    }
    return "invalid";
}
bool valid_identity(const std::string& value, Identity kind) {
    switch (kind) {
    case Identity::Boot:
        return token(value);
    case Identity::NamespaceDev:
    case Identity::NamespaceIno:
    case Identity::Generation:
        return decimal(value);
    case Identity::Rule:
        return decimal(value) && value != "0";
    case Identity::Definition:
    case Identity::Operation:
    case Identity::Rebuild:
    case Identity::Submission:
        return diagnostics::valid_stable_id(value);
    case Identity::Module:
        return value.size() <= diagnostics::input_bytes &&
               diagnostics::valid_module_selector(value);
    case Identity::Revision:
        return valid_revision(value);
    }
    return false;
}

// Every copied field is selected in its own schema context. Strings are either
// fixed vocabulary or a validated typed identity; arbitrary text never crosses.
class Projection {
    bool include, stored, ok = true;
    std::map<Identity, std::map<std::string, std::string>> aliases;
    bool clock_valid = false;
    std::uint64_t start = 0, finish = 0;
    std::set<std::string> evidence_ids;

public:
    explicit Projection(bool i, bool s) : include(i), stored(s) {}
    [[nodiscard]] bool valid() const { return ok; }
    void boolean(const V& in, V& out, const char* key, bool nullable = false, int exact = -1) {
        const auto& v = in.at(key);
        if (!in.contains(key) || !(v.is_bool() || (nullable && v.is_null())) ||
            (v.is_bool() && exact >= 0 && v.b != (exact != 0)))
            ok = false;
        else
            out[key] = v;
    }
    void number(const V& in, V& out, const char* key, double max, bool nullable = false,
                double min = 0) {
        const auto& v = in.at(key);
        if (!in.contains(key) || !(integer(v, max, min) || (nullable && v.is_null())))
            ok = false;
        else
            out[key] = v;
    }
    void code(const V& in, V& out, const char* key, Words values, bool nullable = false) {
        const auto& v = in.at(key);
        if (!in.contains(key) || !(one(v, values) || (nullable && v.is_null())))
            ok = false;
        else
            out[key] = v;
    }
    V identity(const V& v, Identity kind, bool nullable = true) {
        if (nullable && v.is_null())
            return V{};
        if (!v.is_string()) {
            ok = false;
            return V{};
        }
        auto& map = aliases[kind];
        if (stored && !include) {
            const std::string head = std::string(prefix(kind)) + '-';
            if (v.s.rfind(head, 0) != 0 || !decimal(v.s.substr(head.size())) ||
                v.s.substr(head.size()) == "0") {
                ok = false;
                return V{};
            }
            if (!map.count(v.s)) {
                // Canonical aliases are assigned on their first appearance in
                // this deterministic traversal; no hidden reverse map is saved.
                if (v.s != head + std::to_string(map.size() + 1)) {
                    ok = false;
                    return V{};
                }
                map[v.s] = v.s;
            }
            return v;
        }
        if (!valid_identity(v.s, kind)) {
            ok = false;
            return V{};
        }
        if (include)
            return v;
        auto found = map.find(v.s);
        if (found == map.end())
            found =
                map.emplace(v.s, std::string(prefix(kind)) + '-' + std::to_string(map.size() + 1))
                    .first;
        return {found->second};
    }
    void id(const V& in, V& out, const char* key, Identity kind, bool nullable = true) {
        if (!in.contains(key))
            ok = false;
        out[key] = identity(in.at(key), kind, nullable);
    }
    static bool pair(const V& a, const V& b, std::uint64_t& first, std::uint64_t& last) {
        if (!a.is_string() || !b.is_string() || !decimal(a.s) || !decimal(b.s))
            return false;
        first = std::stoull(a.s);
        last = std::stoull(b.s);
        return first <= last;
    }
    V timing(const V& in, bool root) {
        if (stored) {
            const auto& t = in.at("sample_timing");
            auto out = V::object();
            if (!t.is_object())
                ok = false;
            code(t, out, "status", {"relative", "omitted"});
            if (t.at("status").s == "relative") {
                for (const auto* key : {"start_offset_ms", "duration_ms"}) {
                    if (!t.at(key).is_string() || !decimal(t.at(key).s))
                        ok = false;
                    out[key] = t.at(key);
                }
                if (root && t.at("start_offset_ms").s != "0")
                    ok = false;
                if (root && ok) {
                    clock_valid = true;
                    start = 0;
                    finish = std::stoull(t.at("duration_ms").s);
                }
                if (!root && ok) {
                    const auto offset = std::stoull(t.at("start_offset_ms").s);
                    const auto duration = std::stoull(t.at("duration_ms").s);
                    if (!clock_valid || offset > finish || duration > finish - offset)
                        ok = false;
                }
            } else {
                if (!t.contains("start_offset_ms") || !t.at("start_offset_ms").is_null() ||
                    !t.contains("duration_ms") || !t.at("duration_ms").is_null())
                    ok = false;
                out["start_offset_ms"] = V{};
                out["duration_ms"] = V{};
            }
            return out;
        }
        std::uint64_t a = 0;
        std::uint64_t b = 0;
        const bool valid =
            pair(in.at("sample_started_monotonic_ms"), in.at("sample_finished_monotonic_ms"), a, b);
        if (root) {
            clock_valid = valid;
            start = a;
            finish = b;
        }
        const bool relative = valid && clock_valid && a >= start && b <= finish;
        auto out = V::object();
        out["status"] = relative ? "relative" : "omitted";
        out["start_offset_ms"] = relative ? V(std::to_string(a - start)) : V{};
        out["duration_ms"] = relative ? V(std::to_string(b - a)) : V{};
        return out;
    }
    V probe(const V& in) {
        auto out = V::object();
        if (!in.is_object())
            ok = false;
        boolean(in, out, "checked");
        boolean(in, out, "known");
        boolean(in, out, "available", true);
        number(in, out, "errno", 4095);
        if (in.at("known").is_bool() && in.at("known").b == in.at("available").is_null())
            ok = false;
        return out;
    }
    V record(const V& in, bool maps, bool intent = false) {
        if (in.is_null())
            return V{};
        auto out = V::object();
        if (!in.is_object())
            ok = false;
        id(in, out, maps ? "submission_id" : "rebuild_id",
           maps ? Identity::Submission : Identity::Rebuild, false);
        id(in, out, "boot_id", Identity::Boot, false);
        if (!intent) {
            if (maps)
                code(in, out, "phase", {"pending", "acknowledged", "rejected"});
            else
                code(in, out, "phase", {"pending", "acknowledged", "partial_or_unknown"});
            number(in, out, "errno", 4095);
            boolean(in, out, "runtime_verified", false, 0);
        }
        return out;
    }
    V value(const V& in, const std::string& kind) {
        if (in.is_null())
            return V{};
        auto out = V::object();
        if (!in.is_object()) {
            ok = false;
            return out;
        }
        if (kind == "hide.saved") {
            boolean(in, out, "definition_present");
            boolean(in, out, "enabled", true);
            id(in, out, "revision", Identity::Revision);
        } else if (kind == "hide.runtime") {
            boolean(in, out, "managed_ownership", true);
            boolean(in, out, "registered", true);
            code(in, out, "state",
                 {"unknown", "disabled", "disabled_pending_removal", "unregistered",
                  "globally_suspended", "bound", "binding_error", "pending"});
            const auto& r = in.at("runtime");
            auto rt = V{};
            if (!in.contains("runtime"))
                ok = false;
            if (!r.is_null()) {
                rt = V::object();
                if (!r.is_object())
                    ok = false;
                id(r, rt, "id", Identity::Rule, false);
                id(r, rt, "generation", Identity::Generation, false);
                number(r, rt, "management", 1);
                number(r, rt, "binding", 3);
                number(r, rt, "errno", 4095, false, -4095);
            }
            out["runtime"] = rt;
        } else if (kind == "module.selection") {
            id(in, out, "config_revision", Identity::Revision);
            id(in, out, "module_configuration_revision", Identity::Revision);
            boolean(in, out, "other_backends_unplanned");
            boolean(in, out, "live_owner_known", false, 0);
            boolean(in, out, "hot_apply_authorized", false, 0);
            boolean(in, out, "in_current_mountable_set", true);
            if (in.at("in_current_mountable_set").is_bool() &&
                in.at("in_current_mountable_set").b) {
                for (const auto* key : {"configured", "global", "requested", "selected"})
                    code(in, out, key, {"", "auto", "sumh", "overlay", "magic", "none"});
                code(in, out, "selection_source", {"default", "module", "global"});
                for (const auto* key : {"fallback", "complete", "has_content", "requires_magic"})
                    boolean(in, out, key);
                out["sumh_probe"] = probe(in.at("sumh_probe"));
                out["overlay_probe"] = probe(in.at("overlay_probe"));
            }
        } else if (kind == "module.rebuild") {
            for (const auto* key : {"read_status", "intent_read_status"})
                code(in, out, key, {"valid", "missing", "unreadable", "corrupt_or_unsupported"});
            for (const auto* key : {"latest", "acknowledged", "intent"}) {
                if (!in.contains(key))
                    ok = false;
                out[key] = record(in.at(key), false, std::string(key) == "intent");
            }
            boolean(in, out, "admission_is_authorization", false, 0);
            code(in, out, "rebuild_admission",
                 {"allowed_by_retained_history_only", "blocked_unresolved_same_boot",
                  "unknown_blocked"});
        } else if (kind == "maps.saved") {
            boolean(in, out, "enabled", true);
            number(in, out, "count", 256);
            id(in, out, "maps_revision", Identity::Revision);
            id(in, out, "config_revision", Identity::Revision);
        } else if (kind == "maps.sample") {
            code(in, out, "comparison", {"unknown", "matches", "drift"});
            number(in, out, "registered_count", 256, true);
            for (const auto* key : {"stable", "definitions_complete", "identities_sampled"})
                boolean(in, out, key);
            number(in, out, "row_count", 256);
            number(in, out, "omitted_count", 256);
            code(in, out, "identity_counts_scope", {"both_target_and_replacement"});
            auto counts = V::object();
            if (!in.at("counts").is_object())
                ok = false;
            for (const auto* key :
                 {"regular", "missing", "not_regular", "unrepresentable", "error", "unknown",
                  "collision", "matches", "different", "unregistered", "comparison_unknown"})
                number(in.at("counts"), counts, key, 512);
            out["counts"] = counts;
            auto rows = V::array();
            if (!in.at("rows").is_array() || in.at("rows").a.size() > 16)
                ok = false;
            std::set<int> indices;
            for (const auto& r : in.at("rows").a) {
                auto row = V::object();
                number(r, row, "index", 255);
                if (!integer(r.at("index"), 255) ||
                    !indices.insert(static_cast<int>(r.at("index").n)).second)
                    ok = false;
                for (const auto* key : {"target_status", "replacement_status"})
                    code(r, row, key,
                         {"regular", "missing", "not_regular", "unrepresentable", "error",
                          "unknown"});
                for (const auto* key : {"same_target_collision", "stable"})
                    boolean(r, row, key);
                code(r, row, "registered_match", {"unknown", "missing", "matches", "different"});
                rows.push_back(row);
            }
            out["rows"] = rows;
            if (in.at("row_count").n != in.at("omitted_count").n + rows.a.size())
                ok = false;
        } else if (kind == "maps.history") {
            code(in, out, "status",
                 {"valid", "missing", "unreadable", "corrupt_or_unsupported", "unavailable"});
            for (const auto* key : {"latest", "confirmed"}) {
                if (!in.contains(key))
                    ok = false;
                out[key] = record(in.at(key), true);
            }
            boolean(in, out, "revisions_have_different_scopes", false, 1);
        } else if (kind == "recovery.storage") {
            number(in, out, "boot_attempts", 4294967295.0, true);
            number(in, out, "max_attempts", 4294967295.0, false, 1);
            boolean(in, out, "mounting_disabled", true);
            boolean(in, out, "automatic_mount_allowed");
            boolean(in, out, "marker_payload_trusted");
            for (const auto* key : {"counter_state", "disable_marker_state"})
                code(in, out, key, {"valid", "missing", "corrupt", "unreadable"});
        } else if (kind == "recovery.origin") {
            code(in, out, "record_origin_kind", {"disable_marker", "counter"}, true);
            id(in, out, "record_origin_boot_id", Identity::Boot);
        } else if (kind == "operation.history" || kind == "operation.selected") {
            code(in, out, "ledger_status",
                 {"valid", "missing", "unreadable", "corrupt_or_unsupported"});
            code(in, out, "status",
                 {"observed", "not_retained", "unavailable", "context_changed_or_unknown"});
            boolean(in, out, "history_complete", false, 0);
            boolean(in, out, "history_pruned", true);
            boolean(in, out, "absence_proves_never_executed", false, 0);
            number(in, out, "retained_count", 16);
            if (kind == "operation.selected")
                id(in, out, "requested_operation_id", Identity::Operation, false);
        } else
            ok = false;
        return out;
    }
    V evidence(const V& in) {
        auto out = V::object();
        if (!in.is_object())
            ok = false;
        code(in, out, "id",
             {"hide.saved", "hide.runtime", "module.selection", "module.rebuild", "maps.saved",
              "maps.sample", "maps.history", "recovery.storage", "recovery.origin",
              "operation.history", "operation.selected"});
        const auto& kind = in.at("id").s;
        if (!evidence_ids.insert(kind).second)
            ok = false;
        const char* source = "operation_history";
        if (kind == "hide.saved")
            source = "user_hide_selection";
        else if (kind == "hide.runtime")
            source = "user_hide_status";
        else if (kind == "module.selection")
            source = "production_module_planner";
        else if (kind == "module.rebuild")
            source = "module_rebuild_status";
        else if (kind.rfind("maps.", 0) == 0)
            source = "managed_maps_preview";
        else if (kind.rfind("recovery.", 0) == 0)
            source = "legacy_recovery_status";
        const char* classification = "historical";
        if (kind == "hide.saved" || kind == "maps.saved")
            classification = "saved";
        else if (kind == "module.selection")
            classification = "inferred";
        else if (kind == "hide.runtime" || kind == "maps.sample" || kind == "recovery.storage")
            classification = "observed";
        code(in, out, "source", {source});
        code(in, out, "classification", {classification});
        code(in, out, "status", {"known", "unknown", "stale", "unsupported"});
        if (one(in.at("reason"),
                {"source_unavailable", "sampling_context_changed", "sampling_context_unknown"}))
            out["reason"] = in.at("reason");
        else if (kind == "hide.saved")
            code(in, out, "reason",
                 {"definition_selection_unavailable", "definition_selected", "definition_absent"});
        else if (kind == "hide.runtime")
            code(in, out, "reason",
                 {"selected_status_unavailable", "selected_item_not_returned",
                  "managed_ownership_unsupported", "registration_sampled", "registration_unknown"});
        else if (kind == "module.selection")
            code(in, out, "reason",
                 {"planner_did_not_produce_decisions", "not_in_current_mountable_set",
                  "mountable_set_incomplete", "current_input_backend_selection"});
        else if (kind == "module.rebuild")
            code(in, out, "reason",
                 {"retained_rebuild_history_only", "rebuild_history_unavailable"});
        else if (kind == "maps.saved")
            code(in, out, "reason", {"managed_path_definitions_read", "saved_maps_unavailable"});
        else if (kind == "maps.sample")
            code(in, out, "reason", {"current_identity_sample", "identity_sample_unavailable"});
        else if (kind == "maps.history")
            code(in, out, "reason",
                 {"retained_submission_history_only", "submission_history_unavailable"});
        else if (kind == "recovery.storage")
            code(in, out, "reason",
                 {"recovery_storage_gate_observed", "recovery_storage_incomplete"});
        else if (kind == "recovery.origin")
            code(in, out, "reason", {"record_origin_not_current_sample_boot"});
        else
            code(in, out, "reason", {"bounded_retained_history", "operation_history_unavailable"});
        if (one(in.at("scope"), {"source_collection"}))
            out["scope"] = in.at("scope");
        else if (kind == "hide.saved")
            code(in, out, "scope", {"persistent_user_hide_definition"});
        else if (kind == "hide.runtime")
            code(in, out, "scope",
                 {"per_rule_registered_membership",
                  "per_rule_registered_membership_not_observer_visibility"});
        else if (kind == "module.selection")
            code(in, out, "scope",
                 {"hypothetical_compile", "current_mountable_modules_not_installed_inventory",
                  "hypothetical_compile_not_live_backend"});
        else if (kind == "module.rebuild")
            code(in, out, "scope", {"diagnostic_plan_and_legacy_call_acknowledgments"});
        else if (kind == "maps.saved")
            code(in, out, "scope", {"managed_path_definitions_not_temporary_numeric_rules"});
        else if (kind == "maps.sample")
            code(in, out, "scope", {"sampled_identities_not_pinned_or_observer_verified"});
        else if (kind == "maps.history")
            code(in, out, "scope", {"past_submission_acknowledgments_not_current_membership"});
        else if (kind == "recovery.storage")
            code(in, out, "scope", {"recovery_storage_gate_not_safe_mode_owner_or_backend"});
        else if (kind == "recovery.origin")
            code(in, out, "scope", {"retained_recovery_record"});
        else
            code(in, out, "scope", {"command_family_candidates_not_target_correlation"});
        code(in, out, "time_source", {"collector_bracket"});
        out["sample_timing"] = timing(in, false);
        if (!in.contains("value"))
            ok = false;
        out["value"] = value(in.at("value"), kind);
        return out;
    }
    V references(const V& in) {
        auto out = V::array();
        if (!in.is_array() || in.a.empty() || in.a.size() > 16)
            ok = false;
        std::set<std::string> seen;
        for (const auto& r : in.a) {
            if (!r.is_string() || !evidence_ids.count(r.s) || !seen.insert(r.s).second)
                ok = false;
            else
                out.push_back(r);
        }
        return out;
    }
    V operation(const V& in) {
        auto out = V::object();
        if (!in.is_object())
            ok = false;
        id(in, out, "id", Identity::Operation, false);
        id(in, out, "boot_id", Identity::Boot, false);
        code(in, out, "phase", {"prepared", "started", "finished"});
        code(in, out, "result",
             {"not_started", "outcome_unknown", "command_completed", "failed_or_partial"});
        boolean(in, out, "same_boot", true);
        number(in, out, "exit_code", 255, true);
        number(in, out, "errno", 4095, true);
        code(in, out, "correlation",
             {"user_selected_not_target_proven", "same_command_family_candidate"});
        boolean(in, out, "target_verified", false, 0);
        boolean(in, out, "runtime_verified", false, 0);
        const auto& phase = in.at("phase").s;
        if (phase == "finished") {
            if (!in.at("exit_code").is_number() || !in.at("errno").is_number() ||
                in.at("result").s !=
                    (in.at("exit_code").n == 0 ? "command_completed" : "failed_or_partial"))
                ok = false;
        } else if (!in.at("exit_code").is_null() || !in.at("errno").is_null() ||
                   in.at("result").s != (phase == "prepared" ? "not_started" : "outcome_unknown"))
            ok = false;
        return out;
    }
    V report(const V& in) {
        auto out = V::object();
        if (!in.is_object())
            ok = false;
        number(in, out, "schema_version", 1, false, 1);
        code(in, out, "resource", {"sumh_diagnostics"});
        boolean(in, out, "read_only", false, 1);
        for (const auto* key : {"atomic", "observer_verified", "mutations_performed"})
            boolean(in, out, key, false, 0);
        code(in, out, "scenario",
             {"rule-not-effective", "module-fallback", "maps-failure", "boot-protection"}, true);
        code(in, out, "status", {"observed", "partial", "stale", "unavailable", "invalid"});
        code(in, out, "reason",
             {"sampling_context_changed",
              "limited_evidence_incomplete",
              "limited_evidence_collected_not_health_verdict",
              "source_contract_invalid",
              "primary_source_unavailable",
              "response_capacity",
              "transport_unavailable",
              "input_unavailable",
              "namespace_unavailable",
              "context_unavailable",
              "collector_unavailable",
              "input_invalid",
              "init_namespace_unavailable",
              "transport_contract_invalid",
              "response_contract_invalid",
              "invalid_arguments",
              "channel_arguments",
              "input_path_invalid",
              "input_open_failed",
              "input_stat_failed",
              "input_type_or_links_invalid",
              "input_nonblocking_failed",
              "input_timeout",
              "input_poll_failed",
              "input_read_failed",
              "input_flags_restore_failed",
              "input_capacity",
              "input_encoding",
              "input_json",
              "input_schema",
              "input_scenario",
              "input_field",
              "input_type",
              "input_definition_id",
              "input_module_id",
              "input_operation_id"});
        number(in, out, "exit_code", 5);
        int expected = 5;
        if (in.at("status").s == "observed")
            expected = 0;
        else if (in.at("status").s == "partial")
            expected = 3;
        else if (in.at("status").s == "stale")
            expected = 4;
        else if (in.at("status").s == "invalid")
            expected = 2;
        if (in.at("exit_code").n != expected)
            ok = false;
        if (in.contains("errno"))
            number(in, out, "errno", 4095);
        id(in, out, "boot_id", Identity::Boot);
        id(in, out, "namespace_dev", Identity::NamespaceDev);
        id(in, out, "namespace_ino", Identity::NamespaceIno);
        for (const auto* key : {"namespace_verified", "namespace_consistent"})
            if (in.contains(key))
                boolean(in, out, key);
        out["sample_timing"] = timing(in, true);
        const auto& t = in.at("target");
        auto target = V::object();
        if (!t.is_object())
            ok = false;
        if (!in.at("scenario").is_null()) {
            const auto& scenario = in.at("scenario").s;
            const char* resource = "recovery_storage";
            if (scenario == "rule-not-effective")
                resource = "user_hide_definition";
            else if (scenario == "module-fallback")
                resource = "module";
            else if (scenario == "maps-failure")
                resource = "managed_maps";
            // Capacity/unavailable reports can retain only a scenario, not a target.
            if (!t.o.empty()) {
                code(t, target, "resource", {resource});
                if (scenario == "rule-not-effective" || scenario == "module-fallback")
                    id(t, target, "id",
                       scenario == "rule-not-effective" ? Identity::Definition : Identity::Module);
                else {
                    if (!t.contains("id") || !t.at("id").is_null())
                        ok = false;
                    target["id"] = V{};
                }
            } else if (expected != 5 && expected != 2)
                ok = false;
        } else if (!t.o.empty())
            ok = false;
        out["target"] = target;
        if (!in.at("evidence").is_array() || in.at("evidence").a.size() > 16)
            ok = false;
        out["evidence"] = V::array();
        for (const auto& e : in.at("evidence").a)
            out["evidence"].push_back(evidence(e));
        if (!in.at("findings").is_array() || in.at("findings").a.size() > 32)
            ok = false;
        out["findings"] = V::array();
        for (const auto& f : in.at("findings").a) {
            auto item = V::object();
            code(f, item, "code",
                 {"definition_absent_not_module_hide_absence",
                  "global_suspension_not_definition_disabled", "registered_binding_error",
                  "registered_binding_pending", "saved_enabled_differs_from_membership",
                  "planner_selected_fallback", "global_backend_overrides_module",
                  "mountable_absence_does_not_prove_uninstalled", "unresolved_rebuild_no_replay",
                  "managed_maps_current_identities_differ", "maps_paths_unresolved",
                  "maps_target_identity_collision", "maps_submission_outcome_unresolved",
                  "recovery_storage_blocks_automatic_mount", "recovery_storage_needs_inspection",
                  "recorded_command_outcome_unknown_no_replay"});
            code(f, item, "severity", {"information", "warning"});
            item["evidence_ids"] = references(f.at("evidence_ids"));
            out["findings"].push_back(item);
        }
        if (!in.at("next_steps").is_array() || in.at("next_steps").a.size() > 16)
            ok = false;
        out["next_steps"] = V::array();
        for (const auto& s : in.at("next_steps").a) {
            auto item = V::object();
            code(
                s, item, "code",
                {"refresh_hide_status", "review_hide_retry", "refresh_module_plan",
                 "inspect_rebuild_history", "refresh_maps_preview", "review_maps_apply",
                 "refresh_recovery_diagnostics", "review_recovery_reset", "show_selected_operation",
                 "sample_selected_operation", "read_selected_operation_receipt",
                 "inspect_saved_profiles", "verify_observer_view"});
            code(s, item, "kind",
                 {"read_only_query", "explicit_user_change", "manual_verification"});
            boolean(s, item, "automatic", false, 0);
            item["evidence_ids"] = references(s.at("evidence_ids"));
            out["next_steps"].push_back(item);
        }
        if (!in.at("recent_operations").is_array() || in.at("recent_operations").a.size() > 4)
            ok = false;
        out["recent_operations"] = V::array();
        std::set<std::string> ops;
        for (const auto& op : in.at("recent_operations").a) {
            if (!ops.insert(op.at("id").s).second)
                ok = false;
            out["recent_operations"].push_back(operation(op));
        }
        if (!in.at("unchecked").is_array() || in.at("unchecked").a.size() > 6)
            ok = false;
        out["unchecked"] = V::array();
        std::set<std::string> unchecked;
        for (const auto& u : in.at("unchecked").a) {
            if (!one(u, {"ordinary_app_view", "isolated_process_view", "existing_file_descriptors",
                         "live_backend_owner", "cross_backend_winner", "runtime_atomicity"}) ||
                !unchecked.insert(u.s).second)
                ok = false;
            else
                out["unchecked"].push_back(u);
        }
        auto limits = V::object();
        for (const auto& pair :
             std::initializer_list<std::pair<const char*, int>>{{"input_bytes", 4096},
                                                                {"response_bytes", 65536},
                                                                {"evidence", 16},
                                                                {"findings", 32},
                                                                {"next_steps", 16},
                                                                {"recent_operations", 4},
                                                                {"maps_row_summaries", 16}})
            number(in.at("limits"), limits, pair.first, pair.second, false, pair.second);
        out["limits"] = limits;
        return out;
    }
};
bool decode(const std::string& bytes, std::size_t limit, V& out, std::string& reason) {
    if (bytes.size() > limit) {
        reason = "package_capacity";
        return false;
    }
    if (!ksud::text::valid_utf8(bytes) || bytes.find('\0') != std::string::npos) {
        reason = "package_encoding";
        return false;
    }
    std::string ignored;
    if (!json::Parser(bytes, true).parse(out, ignored) || !out.is_object() || !safe_tree(out)) {
        reason = "package_json";
        return false;
    }
    return true;
}
V envelope(const V& report, bool include) {
    auto out = V::object();
    out["schema_version"] = 1;
    out["kind"] = "sumh.diagnostic.package";
    out["privacy"] = privacy(include);
    out["diagnostic"] = report;
    return out;
}
}  // namespace

bool valid_id(const std::string& id) {
    return diagnostics::valid_stable_id(id);
}
bool valid_revision(const std::string& revision) {
    return revision.size() == 74 && revision.substr(0, 10) == "sha256-v1:" &&
           revision.find_first_not_of("0123456789abcdef", 10) == std::string::npos;
}
bool build_package(const std::string& report, bool include_identifiers, JsonValue& output,
                   std::string& reason) {
    V source;
    if (!decode(report, diagnostics::response_bytes, source, reason))
        return false;
    Projection project(include_identifiers, false);
    auto candidate = envelope(project.report(source), include_identifiers);
    if (!project.valid()) {
        reason = "diagnostic_contract_invalid";
        return false;
    }
    if (stringify_json(candidate).size() + 1 > package_limit) {
        reason = "package_capacity";
        return false;
    }
    output = std::move(candidate);
    reason.clear();
    return true;
}
bool parse_package(const std::string& bytes, JsonValue& output, std::string& reason) {
    V source;
    if (!decode(bytes, package_limit, source, reason))
        return false;
    if (!integer(source.at("schema_version"), 1, 1) ||
        !one(source.at("kind"), {"sumh.diagnostic.package"}) ||
        !one(source.at("privacy").at("policy"), {"redacted", "identifiers_included"})) {
        reason = "package_schema";
        return false;
    }
    const bool include = source.at("privacy").at("policy").s == "identifiers_included";
    Projection project(include, true);
    auto candidate = envelope(project.report(source.at("diagnostic")), include);
    // Stored content is closed, including all nested contexts and privacy
    // metadata: future/unknown keys are never silently re-exported or repaired.
    if (!project.valid() || stringify_json(candidate) != stringify_json(source)) {
        reason = "package_contract_invalid";
        return false;
    }
    output = std::move(candidate);
    reason.clear();
    return true;
}
}  // namespace sumhp::diagnostic_package
