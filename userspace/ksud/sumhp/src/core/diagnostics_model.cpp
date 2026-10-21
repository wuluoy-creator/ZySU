#include "core/diagnostics_model.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>

namespace sumhp::diagnostics {
namespace {
using V = JsonValue;
bool text(const V& v, std::size_t limit = 80) {
    return v.is_string() && v.s.size() <= limit && v.s.find('\0') == std::string::npos &&
           ksud::text::valid_utf8(v.s);
}
bool number(const V& v, double max = 4095, double min = 0) {
    return v.is_number() && std::isfinite(v.n) && std::floor(v.n) == v.n && v.n >= min &&
           v.n <= max;
}
bool one(const V& v, std::initializer_list<const char*> values) {
    return v.is_string() &&
           std::any_of(values.begin(), values.end(), [&](const char* s) { return v.s == s; });
}
bool boolean(const V& v, bool expected) {
    return v.is_bool() && v.b == expected;
}
bool nullable_bool(const V& v, const char* key) {
    return v.contains(key) && (v.at(key).is_null() || v.at(key).is_bool());
}
bool token(const std::string& v) {
    return !v.empty() && v.size() <= 80 && std::all_of(v.begin(), v.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               c == '-' || c == '_' || c == '.';
    });
}
bool decimal(const std::string& v) {
    return !v.empty() && v.size() <= 20 && (v == "0" || v.front() != '0') &&
           v.find_first_not_of("0123456789") == std::string::npos &&
           (v.size() < 20 || v <= "18446744073709551615");
}
bool decimal(const V& v) {
    return v.is_string() && decimal(v.s);
}
bool ordered(const std::string& a, const std::string& b) {
    return decimal(a) && decimal(b) && (a.size() < b.size() || (a.size() == b.size() && a <= b));
}
bool revision(const V& v) {
    if (!v.is_string() || v.s.size() != 74 || v.s.substr(0, 10) != "sha256-v1:")
        return false;
    return std::all_of(v.s.begin() + 10, v.s.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool nullable_revision(const V& v, const char* key) {
    return v.contains(key) && (v.at(key).is_null() || revision(v.at(key)));
}
bool nullable_token(const V& v, const char* key) {
    return v.contains(key) && (v.at(key).is_null() ||
                               (text(v.at(key)) && (v.at(key).s.empty() || token(v.at(key).s))));
}
V strings(std::initializer_list<const char*> values) {
    auto result = V::array();
    for (const auto* value : values)
        result.push_back(value);
    return result;
}
V maybe(const std::string& value, bool valid) {
    return valid ? V(value) : V{};
}
bool schema(const V& value, int version, const char* key, const char* resource) {
    return value.is_object() && number(value.at("schema_version"), version) &&
           value.at("schema_version").n == version && text(value.at(key)) &&
           value.at(key).s == resource && boolean(value.at("read_only"), true);
}
bool safe_tree(const V& v) {
    if (v.is_string())
        return ksud::text::valid_utf8(v.s) && v.s.find('\0') == std::string::npos;
    for (const auto& field : v.o)
        if (!ksud::text::valid_utf8(field.first) || field.first.find('\0') != std::string::npos ||
            !safe_tree(field.second))
            return false;
    return std::all_of(v.a.begin(), v.a.end(), safe_tree);
}
enum class Decoded : std::uint8_t { Ready, Failed, Contract };
Decoded decode(const Sample& sample, std::size_t limit, V& value) {
    if (!sample.failure.empty())
        return Decoded::Failed;  // Never decode bytes after a collector failure.
    std::string ignored;
    if (sample.bytes.size() > limit || !ksud::text::valid_utf8(sample.bytes) ||
        !json::Parser(sample.bytes, true).parse(value, ignored) || !value.is_object() ||
        !safe_tree(value))
        return Decoded::Contract;
    return Decoded::Ready;
}
V base(const Request* request) {
    auto out = V::object();
    out["schema_version"] = 1;
    out["resource"] = "sumh_diagnostics";
    out["read_only"] = true;
    out["atomic"] = false;
    out["observer_verified"] = false;
    out["mutations_performed"] = false;
    out["scenario"] = request ? V(scenario_name(request->scenario)) : V{};
    for (const auto* key : {"boot_id", "namespace_dev", "namespace_ino",
                            "sample_started_monotonic_ms", "sample_finished_monotonic_ms"})
        out[key] = V{};
    out["target"] = V::object();
    if (request) {
        switch (request->scenario) {
        case Scenario::UserHide:
            out["target"]["resource"] = "user_hide_definition";
            break;
        case Scenario::ModuleFallback:
            out["target"]["resource"] = "module";
            break;
        case Scenario::MapsFailure:
            out["target"]["resource"] = "managed_maps";
            break;
        case Scenario::BootProtection:
            out["target"]["resource"] = "recovery_storage";
            break;
        }
        out["target"]["id"] = V{};
        if (request->scenario == Scenario::UserHide && valid_stable_id(request->definition_id))
            out["target"]["id"] = request->definition_id;
        else if (request->scenario == Scenario::ModuleFallback &&
                 valid_module_selector(request->module_id) &&
                 request->module_id.size() <= input_bytes)
            out["target"]["id"] = request->module_id;
    }
    for (const auto* key : {"evidence", "findings", "next_steps", "recent_operations"})
        out[key] = V::array();
    out["unchecked"] =
        strings({"ordinary_app_view", "isolated_process_view", "existing_file_descriptors",
                 "live_backend_owner", "cross_backend_winner", "runtime_atomicity"});
    auto limits = V::object();
    limits["input_bytes"] = static_cast<int>(input_bytes);
    limits["response_bytes"] = static_cast<int>(response_bytes);
    limits["evidence"] = 16;
    limits["findings"] = 32;
    limits["next_steps"] = 16;
    limits["recent_operations"] = 4;
    limits["maps_row_summaries"] = 16;
    out["limits"] = std::move(limits);
    return out;
}
struct ModelState {
    const Request* request;
    const Sources* sources;
    V out;
    bool invalid = false, partial = false, stale = false, credible = false, context_known = false,
         changes_blocked = false;
};
struct Model : ModelState {
    explicit Model(const Request& r, const Sources& s) : ModelState{&r, &s, base(&r)} {
        const auto& c = s.context;
        stale = !c.namespace_consistent ||
                (!c.boot_before.empty() && !c.boot_after.empty() && c.boot_before != c.boot_after);
        context_known = c.namespace_verified && c.namespace_consistent && token(c.boot_before) &&
                        c.boot_before == c.boot_after && decimal(c.namespace_dev) &&
                        decimal(c.namespace_ino) && c.namespace_ino != "0";
        out["boot_id"] =
            maybe(c.boot_before, token(c.boot_before) && c.boot_before == c.boot_after);
        out["namespace_dev"] = maybe(c.namespace_dev, decimal(c.namespace_dev));
        out["namespace_ino"] = maybe(c.namespace_ino, decimal(c.namespace_ino));
        out["sample_started_monotonic_ms"] =
            maybe(c.started_ms, ordered(c.started_ms, c.finished_ms));
        out["sample_finished_monotonic_ms"] =
            maybe(c.finished_ms, ordered(c.started_ms, c.finished_ms));
        out["namespace_verified"] = c.namespace_verified;
        out["namespace_consistent"] = c.namespace_consistent;
        if (!context_known || !ordered(c.started_ms, c.finished_ms))
            partial = true;
    }
    void evidence(const char* id, const char* source, const char* classification,
                  const char* status, const char* reason, const char* scope, V value,
                  const Sample& sample) {
        auto e = V::object();
        e["id"] = id;
        e["source"] = source;
        e["classification"] = classification;
        e["status"] = status;
        e["reason"] = reason;
        e["scope"] = scope;
        e["time_source"] = "collector_bracket";
        e["sample_started_monotonic_ms"] =
            maybe(sample.started_ms, ordered(sample.started_ms, sample.finished_ms));
        e["sample_finished_monotonic_ms"] =
            maybe(sample.finished_ms, ordered(sample.started_ms, sample.finished_ms));
        e["value"] = std::move(value);
        out["evidence"].push_back(e);
        if (!ordered(sample.started_ms, sample.finished_ms))
            partial = true;
    }
    void finding(const char* code, const char* id, const char* severity = "information") {
        auto f = V::object();
        f["code"] = code;
        f["severity"] = severity;
        f["evidence_ids"] = strings({id});
        out["findings"].push_back(f);
    }
    void step(const char* code, const char* id, const char* kind, const char* description,
              const std::vector<std::string>& arguments = {}) {
        auto s = V::object();
        s["code"] = code;
        s["kind"] = kind;
        s["automatic"] = false;
        s["description"] = description;
        s["evidence_ids"] = strings({id});
        if (std::string(kind) == "read_only_query" && !arguments.empty()) {
            s["argv_prefix"] = strings({"ksud", "sumhp"});
            auto a = V::array();
            for (const auto& arg : arguments)
                a.push_back(arg);
            s["arguments"] = a;
        }
        out["next_steps"].push_back(s);
    }
    bool source(const Sample& s, std::size_t limit, V& v, const char* id, const char* origin,
                const char* classification) {
        const auto decoded = decode(s, limit, v);
        if (decoded == Decoded::Ready) {
            if (s.query_exit_code < 0 || s.query_exit_code > 255) {
                invalid = true;
                return false;
            }
            const bool normal_absence =
                (s.kind == SourceKind::Operations || s.kind == SourceKind::OperationSelected) &&
                s.query_exit_code == 3 && v.at("status").s == "not_retained";
            if (s.query_exit_code != 0 && !normal_absence) {
                partial = true;
                changes_blocked = true;
            }
            return true;
        }
        if (decoded == Decoded::Contract)
            invalid = true;
        else {
            partial = true;
            changes_blocked = true;
            evidence(id, origin, classification, "unknown", "source_unavailable",
                     "source_collection", V{}, s);
        }
        return false;
    }
    void check_boot(const V& boot) {
        if (boot.is_string() && token(boot.s) && token(sources->context.boot_before)) {
            if (boot.s != sources->context.boot_before)
                stale = true;
        } else
            partial = true;
    }
};

bool hide_runtime(const V& runtime) {
    return runtime.is_object() && decimal(runtime.at("id")) && runtime.at("id").s != "0" &&
           decimal(runtime.at("generation")) && number(runtime.at("management"), 1) &&
           number(runtime.at("binding"), 3) && number(runtime.at("errno"), 4095, -4095);
}
void hide(Model& m) {
    V v;
    const auto& s = m.sources->primary;
    if (!m.source(s, 65536, v, "hide.saved", "user_hide_selection", "saved"))
        return;
    if (!v.is_object() || !number(v.at("schema_version"), 1) || v.at("schema_version").n != 1 ||
        v.at("resource").s != "diagnostic_hide_selection" ||
        v.at("definition_id").s != m.request->definition_id ||
        !one(v.at("selection_status"), {"found", "absent", "stale", "unavailable"}) ||
        !v.at("definition_store_stable").is_bool() || !nullable_revision(v, "revision")) {
        m.invalid = true;
        return;
    }
    const auto& selection = v.at("selection_status").s;
    if (selection == "stale")
        m.stale = true;
    if (selection == "stale" || selection == "unavailable") {
        m.evidence("hide.saved", "user_hide_selection", "saved",
                   selection == "stale" ? "stale" : "unknown", "definition_selection_unavailable",
                   "persistent_user_hide_definition", V{}, s);
        return;
    }
    if (!v.at("definition_store_stable").b || !revision(v.at("revision"))) {
        m.invalid = true;
        return;
    }
    auto saved = V::object();
    saved["definition_present"] = selection == "found";
    saved["revision"] = v.at("revision");
    if (selection == "found") {
        const auto& d = v.at("saved_definition");
        if (!d.is_object() || d.at("id").s != m.request->definition_id ||
            !d.at("enabled").is_bool()) {
            m.invalid = true;
            return;
        }
        saved["enabled"] = d.at("enabled");
    } else
        saved["enabled"] = V{};
    m.credible = true;
    m.evidence("hide.saved", "user_hide_selection", "saved", "known",
               selection == "found" ? "definition_selected" : "definition_absent",
               "persistent_user_hide_definition", saved, s);
    if (selection == "absent") {
        m.finding("definition_absent_not_module_hide_absence", "hide.saved");
        return;
    }
    const auto& page = v.at("status_page");
    if (!schema(page, 1, "resource", "user_hide_definitions") ||
        !one(page.at("status"),
             {"observed", "unavailable", "stale_or_invalid_cursor", "changed_or_unavailable"}) ||
        !nullable_revision(page, "revision") || !page.at("items").is_array() ||
        page.at("items").a.size() > 16 || !page.at("complete").is_bool() ||
        !text(page.at("boot_id"))) {
        m.invalid = true;
        return;
    }
    m.check_boot(page.at("boot_id"));
    if (page.at("status").s != "observed") {
        m.partial = true;
        if (page.at("status").s == "stale_or_invalid_cursor" ||
            page.at("status").s == "changed_or_unavailable")
            m.stale = true;
        m.evidence("hide.runtime", "user_hide_status", "observed", m.stale ? "stale" : "unknown",
                   "selected_status_unavailable", "per_rule_registered_membership", V{}, s);
        return;
    }
    if (page.at("revision").s != v.at("revision").s || !nullable_bool(page, "managed_ownership") ||
        !one(page.at("runtime_query"), {"observed", "unsupported", "unavailable"}) ||
        !number(page.at("cursor"), 4096) || !number(page.at("count"), 4096) ||
        !number(page.at("next_cursor"), 4096) || !page.at("at_end").is_bool() ||
        !boolean(page.at("runtime_verified"), false) ||
        page.at("runtime_consistency").s != "per_rule_sampled") {
        m.invalid = true;
        return;
    }
    const V* selected = nullptr;
    std::set<std::string> ids;
    for (const auto& item : page.at("items").a) {
        if (!text(item.at("id"), 32) || !valid_stable_id(item.at("id").s) ||
            !ids.insert(item.at("id").s).second) {
            m.invalid = true;
            return;
        }
        if (item.at("id").s == m.request->definition_id)
            selected = &item;
    }
    if (!selected) {
        m.partial = true;
        m.evidence("hide.runtime", "user_hide_status", "observed", "unknown",
                   "selected_item_not_returned", "per_rule_registered_membership", V{}, s);
        return;
    }
    const auto& item = *selected;
    if (!item.at("enabled").is_bool() || item.at("enabled").b != saved.at("enabled").b ||
        !nullable_bool(item, "registered") || !item.contains("runtime") ||
        (!item.at("runtime").is_null() && !hide_runtime(item.at("runtime"))) ||
        !one(item.at("state"), {"unknown", "disabled", "disabled_pending_removal", "unregistered",
                                "globally_suspended", "bound", "binding_error", "pending"})) {
        m.invalid = true;
        return;
    }
    const auto& query = page.at("runtime_query").s;
    if ((query == "observed" &&
         (!boolean(page.at("managed_ownership"), true) || !item.at("registered").is_bool() ||
          item.at("registered").b == item.at("runtime").is_null())) ||
        (query == "unsupported" && !boolean(page.at("managed_ownership"), false)) ||
        (query != "observed" &&
         (!item.at("registered").is_null() || !item.at("runtime").is_null()))) {
        m.invalid = true;
        return;
    }
    auto runtime = V::object();
    runtime["managed_ownership"] = page.at("managed_ownership");
    runtime["registered"] = item.at("registered");
    runtime["runtime"] = item.at("runtime");
    runtime["state"] = item.at("state");
    const char* runtime_status = "unknown";
    const char* runtime_reason = "registration_unknown";
    if (query == "observed") {
        runtime_status = "known";
        runtime_reason = "registration_sampled";
    } else if (query == "unsupported") {
        runtime_status = "unsupported";
        runtime_reason = "managed_ownership_unsupported";
    }
    m.evidence("hide.runtime", "user_hide_status", "observed", runtime_status, runtime_reason,
               "per_rule_registered_membership_not_observer_visibility",
               query == "unavailable" ? V{} : runtime, s);
    if (query == "unavailable")
        m.partial = true;
    if (query == "observed") {
        const auto& rt = item.at("runtime");
        if (!rt.is_null() && rt.at("management").n == 1)
            m.finding("global_suspension_not_definition_disabled", "hide.runtime", "warning");
        if (!rt.is_null() && rt.at("binding").n == 3)
            m.finding("registered_binding_error", "hide.runtime", "warning");
        if (!rt.is_null() && rt.at("binding").n < 2)
            m.finding("registered_binding_pending", "hide.runtime");
        if (saved.at("enabled").b != item.at("registered").b)
            m.finding("saved_enabled_differs_from_membership", "hide.runtime", "warning");
    }
    m.step("refresh_hide_status", "hide.saved", "read_only_query",
           "Read the selected definition page using its current revision and cursor.",
           {"hide", "status", v.at("revision").s,
            std::to_string(static_cast<int>(page.at("cursor").n))});
    if (query == "observed" && saved.at("enabled").b)
        m.step("review_hide_retry", "hide.runtime", "explicit_user_change",
               "An explicit hide retry may register the saved definition again. Review its current "
               "status and unresolved operations before choosing it; registration does not verify "
               "application visibility.");
}

bool probe(const V& v) {
    return v.is_object() && v.at("checked").is_bool() && v.at("known").is_bool() &&
           nullable_bool(v, "available") && number(v.at("errno")) &&
           (v.at("known").b ? v.at("available").is_bool() : v.at("available").is_null());
}
V probe_summary(const V& v) {
    auto out = V::object();
    for (const auto* key : {"checked", "known", "available", "errno"})
        out[key] = v.at(key);
    return out;
}
bool encoded_id(const V& v) {
    if (!v.is_object() || !one(v.at("encoding"), {"utf-8", "hex"}) || !text(v.at("value"), 8192) ||
        !number(v.at("byte_length"), 4096))
        return false;
    if (v.at("encoding").s == "utf-8")
        return v.at("value").s.size() == v.at("byte_length").n;
    return v.at("value").s.size() == v.at("byte_length").n * 2 &&
           v.at("value").s.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool decision(const V& d) {
    if (!encoded_id(d.at("module_id")))
        return false;
    for (const auto* key : {"configured", "global", "requested", "selected"})
        if (!one(d.at(key), {"", "auto", "sumh", "overlay", "magic", "none"}))
            return false;
    return one(d.at("selection_source"), {"default", "module", "global"}) &&
           d.at("fallback").is_bool() && d.at("complete").is_bool() &&
           d.at("has_content").is_bool() && d.at("requires_magic").is_bool() &&
           probe(d.at("sumh_probe")) && probe(d.at("overlay_probe"));
}
void module(Model& m) {
    V v;
    const auto& s = m.sources->primary;
    if (!m.source(s, 2UL * 1024UL * 1024UL, v, "module.selection", "production_module_planner",
                  "inferred"))
        return;
    if (!schema(v, 1, "kind", "sumh.module.plan") ||
        !one(v.at("status"), {"unknown", "invalid", "stale", "partial", "complete"}) ||
        !number(v.at("errno"))) {
        m.invalid = true;
        return;
    }
    const auto status = v.at("status").s;
    if (status == "stale")
        m.stale = true;
    if (!v.contains("baseline")) {
        if (status != "unknown" && status != "invalid") {
            m.invalid = true;
            return;
        }
        m.evidence("module.selection", "production_module_planner", "inferred", "unknown",
                   "planner_did_not_produce_decisions", "hypothetical_compile", V{}, s);
        return;
    }
    const auto& b = v.at("baseline");
    const auto& owner = v.at("ownership");
    if (!v.at("stable").is_bool() || !text(v.at("boot_id")) ||
        !nullable_revision(v, "config_revision") ||
        !nullable_revision(v, "module_configuration_revision") || !b.is_object() ||
        !b.at("backend_decisions").is_array() || b.at("backend_decisions").a.size() > 512 ||
        !owner.is_object() || owner.at("external_owner_status").s != "not_observed" ||
        !boolean(owner.at("external_owner_absence_proven"), false) ||
        !boolean(owner.at("hot_apply_authorized"), false)) {
        m.invalid = true;
        return;
    }
    for (const auto* key : {"compiled", "complete", "other_backends_unplanned", "events_truncated",
                            "observations_truncated", "valid_for_current_inputs"})
        if (!b.at(key).is_bool()) {
            m.invalid = true;
            return;
        }
    m.check_boot(v.at("boot_id"));
    if (v.contains("namespace_ino")) {
        if (!decimal(v.at("namespace_ino"))) {
            m.invalid = true;
            return;
        }
        if (v.at("namespace_ino").s != m.sources->context.namespace_ino)
            m.stale = true;
    }
    if (!v.at("stable").b)
        m.stale = true;
    if (status != "complete" || !b.at("complete").b || !b.at("valid_for_current_inputs").b ||
        b.at("events_truncated").b || b.at("observations_truncated").b)
        m.partial = true;
    const V* selected = nullptr;
    std::set<std::string> ids;
    for (const auto& item : b.at("backend_decisions").a) {
        if (!decision(item)) {
            m.invalid = true;
            return;
        }
        const auto id = stringify_json(item.at("module_id"));
        if (!ids.insert(id).second) {
            m.invalid = true;
            return;
        }
        if (item.at("module_id").at("encoding").s == "utf-8" &&
            item.at("module_id").at("value").s == m.request->module_id)
            selected = &item;
    }
    auto value = V::object();
    value["config_revision"] = v.at("config_revision");
    value["module_configuration_revision"] = v.at("module_configuration_revision");
    value["other_backends_unplanned"] = b.at("other_backends_unplanned");
    value["live_owner_known"] = false;
    value["hot_apply_authorized"] = false;
    if (!selected) {
        const bool complete = status == "complete" && v.at("stable").b && b.at("complete").b;
        value["in_current_mountable_set"] = complete ? V(false) : V{};
        m.credible = complete;
        m.evidence("module.selection", "production_module_planner", "inferred",
                   complete ? "known" : "unknown",
                   complete ? "not_in_current_mountable_set" : "mountable_set_incomplete",
                   "current_mountable_modules_not_installed_inventory", complete ? value : V{}, s);
        if (complete)
            m.finding("mountable_absence_does_not_prove_uninstalled", "module.selection");
    } else {
        const auto& d = *selected;
        value["in_current_mountable_set"] = true;
        for (const auto* key : {"configured", "global", "requested", "selected", "selection_source",
                                "fallback", "complete", "has_content", "requires_magic"})
            value[key] = d.at(key);
        value["sumh_probe"] = probe_summary(d.at("sumh_probe"));
        value["overlay_probe"] = probe_summary(d.at("overlay_probe"));
        m.credible = true;
        if (!d.at("complete").b)
            m.partial = true;
        m.evidence("module.selection", "production_module_planner", "inferred", "known",
                   "current_input_backend_selection", "hypothetical_compile_not_live_backend",
                   value, s);
        if (d.at("fallback").b)
            m.finding("planner_selected_fallback", "module.selection", "warning");
        if (d.at("selection_source").s == "global")
            m.finding("global_backend_overrides_module", "module.selection");
    }
    m.step("refresh_module_plan", "module.selection", "read_only_query",
           "Read the production plan for all currently mountable modules; this does not establish "
           "a live backend owner.",
           {"module", "plan"});
}

bool record_brief(const V& record, V& out, bool maps) {
    if (record.is_null()) {
        out = V{};
        return true;
    }
    if (!record.is_object() || !text(record.at("id"), 32) || !valid_stable_id(record.at("id").s) ||
        !text(record.at("boot_id")) || !token(record.at("boot_id").s) ||
        !number(record.at("errno")) ||
        !(maps ? one(record.at("phase"), {"pending", "acknowledged", "rejected"})
               : one(record.at("phase"), {"pending", "acknowledged", "partial_or_unknown"})))
        return false;
    out = V::object();
    out[maps ? "submission_id" : "rebuild_id"] = record.at("id");
    for (const auto* key : {"boot_id", "phase", "errno"})
        out[key] = record.at(key);
    out["runtime_verified"] = false;
    return true;
}
void rebuild(Model& m) {
    if (!m.sources->rebuild) {
        m.invalid = true;
        return;
    }
    const auto& s = *m.sources->rebuild;
    V v;
    if (!m.source(s, (1024 * 1024) + 4096, v, "module.rebuild", "module_rebuild_status",
                  "historical"))
        return;
    if (!schema(v, 1, "resource", "module_rebuild_status") ||
        !one(v.at("read_status"), {"valid", "missing", "unreadable", "corrupt_or_unsupported"}) ||
        !one(v.at("intent_read_status"),
             {"valid", "missing", "unreadable", "corrupt_or_unsupported"}) ||
        !v.at("context_consistent").is_bool() || !nullable_token(v, "boot_id") ||
        !nullable_bool(v, "same_boot") ||
        !one(v.at("rebuild_admission"), {"allowed_by_retained_history_only",
                                         "blocked_unresolved_same_boot", "unknown_blocked"}) ||
        !boolean(v.at("runtime_verified"), false) || !boolean(v.at("owner_verified"), false) ||
        !boolean(v.at("admission_is_authorization"), false) || !v.contains("intent") ||
        !v.contains("latest") || !v.contains("acknowledged")) {
        m.invalid = true;
        return;
    }
    V latest;
    V acknowledged;
    if (!record_brief(v.at("latest"), latest, false) ||
        !record_brief(v.at("acknowledged"), acknowledged, false)) {
        m.invalid = true;
        return;
    }
    auto intent = V::object();
    if (!v.at("intent").is_null()) {
        const auto& i = v.at("intent");
        if (!number(i.at("schema_version"), 1) || i.at("schema_version").n != 1 ||
            i.at("resource").s != "module_rebuild_intent" || !valid_stable_id(i.at("id").s) ||
            !token(i.at("boot_id").s)) {
            m.invalid = true;
            return;
        }
        intent["rebuild_id"] = i.at("id");
        intent["boot_id"] = i.at("boot_id");
    } else
        intent = V{};
    if ((v.at("intent_read_status").s == "valid") != !intent.is_null()) {
        m.invalid = true;
        return;
    }
    auto value = V::object();
    value["read_status"] = v.at("read_status");
    value["intent_read_status"] = v.at("intent_read_status");
    value["latest"] = latest;
    value["acknowledged"] = acknowledged;
    value["intent"] = intent;
    value["admission_is_authorization"] = false;
    value["rebuild_admission"] = v.at("rebuild_admission");
    const bool known = one(v.at("read_status"), {"valid", "missing"}) &&
                       one(v.at("intent_read_status"), {"valid", "missing"}) &&
                       v.at("context_consistent").b && v.at("boot_id").is_string() &&
                       v.at("boot_id").s == m.sources->context.boot_before;
    if (!known)
        m.partial = true;
    if (!known || v.at("rebuild_admission").s != "allowed_by_retained_history_only")
        m.changes_blocked = true;
    m.evidence("module.rebuild", "module_rebuild_status", "historical", known ? "known" : "unknown",
               known ? "retained_rebuild_history_only" : "rebuild_history_unavailable",
               "diagnostic_plan_and_legacy_call_acknowledgments", known ? value : V{}, s);
    if (known && v.at("rebuild_admission").s == "blocked_unresolved_same_boot")
        m.finding("unresolved_rebuild_no_replay", "module.rebuild", "warning");
    m.step("inspect_rebuild_history", "module.rebuild", "read_only_query",
           "Read retained rebuild history and the intent latch. Admission based on this history is "
           "not authorization to apply.",
           {"module", "rebuild-status"});
}

bool identity(const V& v) {
    if (!v.is_object() ||
        !one(v.at("status"),
             {"regular", "missing", "not_regular", "unrepresentable", "error", "unknown"}) ||
        !number(v.at("errno")) || !v.contains("ino") || !v.contains("dev"))
        return false;
    return v.at("status").s == "regular" ? decimal(v.at("ino")) && decimal(v.at("dev"))
                                         : v.at("ino").is_null() && v.at("dev").is_null();
}
void maps(Model& m) {
    V v;
    const auto& s = m.sources->primary;
    if (!m.source(s, 512UL * 1024UL, v, "maps.saved", "managed_maps_preview", "saved"))
        return;
    if (!schema(v, 1, "kind", "sumh.maps.preview") ||
        !one(v.at("status"), {"unknown", "stale", "partial", "complete"}) ||
        !number(v.at("errno")) || !nullable_token(v, "boot_id") ||
        !nullable_revision(v, "config_revision")) {
        m.invalid = true;
        return;
    }
    const auto& saved = v.at("saved");
    const auto& sample = v.at("sample");
    const auto& history = v.at("last_submission");
    if (!saved.is_object() ||
        !one(saved.at("status"), {"valid", "missing", "unreadable", "invalid", "changed"}) ||
        !nullable_bool(saved, "enabled") || !number(saved.at("count"), 256) ||
        !nullable_revision(saved, "maps_revision") || !sample.is_object() ||
        !sample.at("stable").is_bool() || !sample.at("definitions_complete").is_bool() ||
        !sample.at("identities_sampled").is_bool() ||
        !one(sample.at("comparison"), {"unknown", "matches", "drift"}) ||
        !number(sample.at("errno")) || !v.at("rows").is_array() || v.at("rows").a.size() > 256 ||
        !v.contains("registered_count") ||
        !(v.at("registered_count").is_null() || number(v.at("registered_count"), 256)) ||
        !history.is_object() ||
        !one(history.at("status"),
             {"valid", "missing", "unreadable", "corrupt_or_unsupported", "unavailable"}) ||
        !history.contains("latest") || !history.contains("confirmed")) {
        m.invalid = true;
        return;
    }
    for (const auto* key : {"namespace_ino", "instance_id", "generation"})
        if (!sample.contains(key) || (!sample.at(key).is_null() && !decimal(sample.at(key)))) {
            m.invalid = true;
            return;
        }
    // The early producer envelope has no history.stable yet.
    if (history.contains("stable") && !history.at("stable").is_bool()) {
        m.invalid = true;
        return;
    }
    if (history.at("status").s != "unavailable" && !history.at("stable").is_bool()) {
        m.invalid = true;
        return;
    }
    const bool saved_known = saved.at("status").s == "valid" && saved.at("enabled").is_bool() &&
                             revision(saved.at("maps_revision")) &&
                             revision(v.at("config_revision"));
    if (saved_known && v.at("rows").a.size() != saved.at("count").n) {
        m.invalid = true;
        return;
    }
    const bool history_known =
        one(history.at("status"), {"valid", "missing"}) && boolean(history.at("stable"), true);
    if ((v.at("status").s == "complete" &&
         (!saved_known || !sample.at("stable").b || !sample.at("definitions_complete").b ||
          !sample.at("identities_sampled").b || !history_known)) ||
        (sample.at("comparison").s != "unknown" &&
         (!sample.at("stable").b || !sample.at("definitions_complete").b)) ||
        (!v.at("registered_count").is_null() &&
         (!sample.at("stable").b || !sample.at("definitions_complete").b))) {
        m.invalid = true;
        return;
    }
    if (v.at("status").s == "stale")
        m.stale = true;
    m.check_boot(v.at("boot_id"));
    if (sample.at("namespace_ino").is_string() &&
        sample.at("namespace_ino").s != m.sources->context.namespace_ino)
        m.stale = true;
    auto desired = V::object();
    desired["enabled"] = saved.at("enabled");
    desired["count"] = saved.at("count");
    desired["maps_revision"] = saved.at("maps_revision");
    desired["config_revision"] = v.at("config_revision");
    m.evidence("maps.saved", "managed_maps_preview", "saved", saved_known ? "known" : "unknown",
               saved_known ? "managed_path_definitions_read" : "saved_maps_unavailable",
               "managed_path_definitions_not_temporary_numeric_rules", saved_known ? desired : V{},
               s);
    m.credible = saved_known;
    auto rows = V::array();
    auto counts = V::object();
    for (const auto* key :
         {"regular", "missing", "not_regular", "unrepresentable", "error", "unknown", "collision",
          "matches", "different", "unregistered", "comparison_unknown"})
        counts[key] = 0;
    std::set<int> indices;
    for (const auto& row : v.at("rows").a) {
        if (!number(row.at("index"), 255) ||
            !indices.insert(static_cast<int>(row.at("index").n)).second ||
            !identity(row.at("target")) || !identity(row.at("replacement")) ||
            !row.at("same_target_collision").is_bool() || !row.at("stable").is_bool() ||
            !one(row.at("registered_match"), {"unknown", "missing", "matches", "different"})) {
            m.invalid = true;
            return;
        }
        if (row.at("registered_match").s != "unknown" &&
            (!sample.at("stable").b || !sample.at("definitions_complete").b ||
             !sample.at("identities_sampled").b || !row.at("stable").b ||
             row.at("same_target_collision").b || row.at("target").at("status").s != "regular" ||
             row.at("replacement").at("status").s != "regular")) {
            m.invalid = true;
            return;
        }
        for (const auto* key : {"target", "replacement"})
            counts[row.at(key).at("status").s].n += 1;
        if (row.at("same_target_collision").b)
            counts["collision"].n += 1;
        const auto match = row.at("registered_match").s;
        if (match == "missing")
            counts["unregistered"].n += 1;
        else if (match == "unknown")
            counts["comparison_unknown"].n += 1;
        else
            counts[match].n += 1;
        if (rows.a.size() < 16) {
            auto summary = V::object();
            summary["index"] = row.at("index");
            summary["target_status"] = row.at("target").at("status");
            summary["replacement_status"] = row.at("replacement").at("status");
            for (const auto* key : {"same_target_collision", "stable", "registered_match"})
                summary[key] = row.at(key);
            rows.push_back(summary);
        }
    }
    auto observed = V::object();
    observed["comparison"] = sample.at("comparison");
    observed["registered_count"] = v.at("registered_count");
    observed["stable"] = sample.at("stable");
    observed["definitions_complete"] = sample.at("definitions_complete");
    observed["identities_sampled"] = sample.at("identities_sampled");
    observed["counts"] = counts;
    observed["rows"] = rows;
    observed["row_count"] = static_cast<int>(v.at("rows").a.size());
    observed["omitted_count"] = static_cast<int>(v.at("rows").a.size() - rows.a.size());
    observed["identity_counts_scope"] = "both_target_and_replacement";
    const bool sampled = saved_known && sample.at("identities_sampled").b && sample.at("stable").b;
    m.evidence("maps.sample", "managed_maps_preview", "observed", sampled ? "known" : "unknown",
               sampled ? "current_identity_sample" : "identity_sample_unavailable",
               "sampled_identities_not_pinned_or_observer_verified", sampled ? observed : V{}, s);
    if (!sampled || v.at("status").s != "complete")
        m.partial = true;
    if (sampled && sample.at("comparison").s == "drift")
        m.finding("managed_maps_current_identities_differ", "maps.sample", "warning");
    if (sampled && (counts.at("missing").n || counts.at("not_regular").n ||
                    counts.at("unrepresentable").n || counts.at("error").n))
        m.finding("maps_paths_unresolved", "maps.sample", "warning");
    if (sampled && counts.at("collision").n)
        m.finding("maps_target_identity_collision", "maps.sample", "warning");
    V latest;
    V confirmed;
    if (!record_brief(history.at("latest"), latest, true) ||
        !record_brief(history.at("confirmed"), confirmed, true)) {
        m.invalid = true;
        return;
    }
    auto retained = V::object();
    retained["status"] = history.at("status");
    retained["latest"] = latest;
    retained["confirmed"] = confirmed;
    retained["revisions_have_different_scopes"] = true;
    m.evidence(
        "maps.history", "managed_maps_preview", "historical", history_known ? "known" : "unknown",
        history_known ? "retained_submission_history_only" : "submission_history_unavailable",
        "past_submission_acknowledgments_not_current_membership", history_known ? retained : V{},
        s);
    if (!history_known)
        m.partial = true;
    if (!history_known || (!latest.is_null() && latest.at("phase").s == "pending"))
        m.changes_blocked = true;
    if (!latest.is_null() && latest.at("phase").s == "pending")
        m.finding("maps_submission_outcome_unresolved", "maps.history", "warning");
    m.step("refresh_maps_preview", "maps.saved", "read_only_query",
           "Refresh saved paths and current file identities. This does not redirect file contents "
           "or establish temporary-rule precedence.",
           {"sumh", "maps", "preview"});
    if (saved_known && sampled && history_known &&
        (latest.is_null() || latest.at("phase").s != "pending") && v.at("status").s == "complete")
        m.step("review_maps_apply", "maps.sample", "explicit_user_change",
               "An explicit sumh maps apply resolves the current saved paths and replaces the "
               "managed set. Review missing files, collisions and pending operations first; never "
               "replay historical numeric identities.");
}

void recovery(Model& m) {
    V v;
    const auto& s = m.sources->primary;
    if (!m.source(s, 65536, v, "recovery.storage", "legacy_recovery_status", "observed"))
        return;
    static const std::set<std::string> keys{"ok",
                                            "boot_attempts",
                                            "max_attempts",
                                            "mounting_disabled",
                                            "automatic_mount_allowed",
                                            "counter_state",
                                            "disable_marker_state",
                                            "boot_id",
                                            "phase",
                                            "reason",
                                            "error"};
    if (v.o.size() != keys.size()) {
        m.invalid = true;
        return;
    }
    for (const auto& field : v.o)
        if (!keys.count(field.first)) {
            m.invalid = true;
            return;
        }
    if (!v.at("ok").is_bool() || !v.contains("boot_attempts") ||
        !(v.at("boot_attempts").is_null() || number(v.at("boot_attempts"), 4294967295.0)) ||
        !number(v.at("max_attempts"), 4294967295.0, 1) || !nullable_bool(v, "mounting_disabled") ||
        !v.at("automatic_mount_allowed").is_bool() ||
        !one(v.at("counter_state"), {"valid", "missing", "corrupt", "unreadable"}) ||
        !one(v.at("disable_marker_state"), {"valid", "missing", "corrupt", "unreadable"}) ||
        !text(v.at("boot_id")) || !text(v.at("phase"), 4096) || !text(v.at("reason"), 4096) ||
        !text(v.at("error"), 65536)) {
        m.invalid = true;
        return;
    }
    const bool healthy = one(v.at("counter_state"), {"valid", "missing"}) &&
                         one(v.at("disable_marker_state"), {"valid", "missing"});
    if (v.at("ok").b != healthy ||
        (healthy && (!v.at("boot_attempts").is_number() || !v.at("mounting_disabled").is_bool()))) {
        m.invalid = true;
        return;
    }
    auto value = V::object();
    for (const auto* key : {"boot_attempts", "max_attempts", "mounting_disabled",
                            "automatic_mount_allowed", "counter_state", "disable_marker_state"})
        value[key] = v.at(key);
    value["marker_payload_trusted"] = v.at("disable_marker_state").s == "valid";
    m.credible = true;
    if (!healthy)
        m.partial = true;
    m.evidence("recovery.storage", "legacy_recovery_status", "observed", "known",
               healthy ? "recovery_storage_gate_observed" : "recovery_storage_incomplete",
               "recovery_storage_gate_not_safe_mode_owner_or_backend", value, s);
    auto origin = V::object();
    const bool marker = v.at("disable_marker_state").s == "valid";
    const bool counter = v.at("counter_state").s == "valid";
    origin["record_origin_kind"] = V{};
    if (marker)
        origin["record_origin_kind"] = "disable_marker";
    else if (counter)
        origin["record_origin_kind"] = "counter";
    origin["record_origin_boot_id"] =
        maybe(v.at("boot_id").s, (marker || counter) && token(v.at("boot_id").s));
    m.evidence("recovery.origin", "legacy_recovery_status", "historical",
               (marker || counter) && !token(v.at("boot_id").s) ? "unknown" : "known",
               "record_origin_not_current_sample_boot", "retained_recovery_record", origin, s);
    if (!v.at("automatic_mount_allowed").b)
        m.finding("recovery_storage_blocks_automatic_mount", "recovery.storage", "warning");
    if (!healthy)
        m.finding("recovery_storage_needs_inspection", "recovery.storage", "warning");
    m.step("refresh_recovery_diagnostics", "recovery.storage", "manual_verification",
           "Run doctor again with the boot-protection input after reviewing recent operations. The "
           "ordinary recovery status route is not treated here as an end-to-end read-only query.");
    if (healthy && !v.at("automatic_mount_allowed").b)
        m.step("review_recovery_reset", "recovery.storage", "explicit_user_change",
               "An explicit recovery reset changes the counter and disable marker. It does not "
               "restore configuration or runtime mounts; read doctor boot-protection again to "
               "confirm the resulting storage state.");
}

bool hide_action(const std::string& name) {
    static const std::set<std::string> names{
        "hide create",       "hide import-id",     "hide annotate", "hide enable",   "hide disable",
        "hide enable-group", "hide disable-group", "hide retry",    "hide remove-id"};
    return names.count(name) != 0;
}
bool family(const std::string& name, Scenario scenario) {
    switch (scenario) {
    case Scenario::UserHide:
        return name.rfind("hide ", 0) == 0;
    case Scenario::ModuleFallback:
        return name.rfind("module ", 0) == 0 || name == "config apply";
    case Scenario::MapsFailure:
        return name.rfind("sumh maps ", 0) == 0 || name == "config apply";
    case Scenario::BootProtection:
        return name.rfind("recovery ", 0) == 0;
    }
    return false;
}
bool operation(const V& record) {
    if (!record.is_object() || !valid_stable_id(record.at("id").s) ||
        !text(record.at("name"), 128) || !token(record.at("boot_id").s) ||
        !one(record.at("phase"), {"prepared", "started", "finished"}) ||
        !nullable_bool(record, "same_boot") || !nullable_revision(record, "request_revision") ||
        !one(record.at("result"),
             {"not_started", "outcome_unknown", "command_completed", "failed_or_partial"}) ||
        !record.contains("exit_code") || !record.contains("errno"))
        return false;
    if (record.at("phase").s == "finished")
        return number(record.at("exit_code"), 255) && number(record.at("errno")) &&
               record.at("result").s ==
                   (record.at("exit_code").n == 0 ? "command_completed" : "failed_or_partial");
    return record.at("exit_code").is_null() && record.at("errno").is_null() &&
           record.at("result").s ==
               (record.at("phase").s == "prepared" ? "not_started" : "outcome_unknown");
}
void history(Model& m, const Sample& s, bool selected) {
    V v;
    const char* id = selected ? "operation.selected" : "operation.history";
    if (!m.source(s, 65536, v, id, "operation_history", "historical"))
        return;
    if (!schema(v, 2, "resource", "operation_history") ||
        !one(v.at("ledger_status"), {"valid", "missing", "unreadable", "corrupt_or_unsupported"}) ||
        !one(v.at("status"),
             {"observed", "not_retained", "unavailable", "context_changed_or_unknown"}) ||
        !nullable_token(v, "boot_id") || !v.contains("requested_id") ||
        !boolean(v.at("history_complete"), false) || !nullable_bool(v, "history_pruned") ||
        !number(v.at("retention_limit"), 16) || v.at("retention_limit").n != 16 ||
        !v.at("records").is_array() || v.at("records").a.size() > (selected ? 1 : 16) ||
        !boolean(v.at("runtime_verified"), false) ||
        v.at("scope").s != "recorded_commands_in_caller_filesystem_namespace" ||
        v.at("durability_confirmation").s != "not_observable_from_read" ||
        (selected ? v.at("requested_id").s != m.request->operation_id
                  : !v.at("requested_id").is_null())) {
        m.invalid = true;
        return;
    }
    std::set<std::string> seen;
    if ((v.at("ledger_status").s != "valid" && !v.at("records").a.empty()) ||
        (v.at("ledger_status").s == "valid" ? !v.at("history_pruned").is_bool()
                                            : !v.at("history_pruned").is_null()) ||
        (v.at("status").s == "not_retained" && (!selected || !v.at("records").a.empty())) ||
        (selected && v.at("status").s == "observed" && v.at("records").a.size() != 1)) {
        m.invalid = true;
        return;
    }
    for (const auto& row : v.at("records").a) {
        if (!operation(row) || !seen.insert(row.at("id").s).second ||
            (selected && row.at("id").s != m.request->operation_id)) {
            m.invalid = true;
            return;
        }
        if (token(v.at("boot_id").s)
                ? (!row.at("same_boot").is_bool() ||
                   row.at("same_boot").b != (row.at("boot_id").s == v.at("boot_id").s))
                : !row.at("same_boot").is_null()) {
            m.invalid = true;
            return;
        }
        if ((selected || family(row.at("name").s, m.request->scenario)) &&
            row.at("phase").s != "finished")
            m.changes_blocked = true;
    }
    const bool known = one(v.at("ledger_status"), {"valid", "missing"}) &&
                       one(v.at("status"), {"observed", "not_retained"}) &&
                       v.at("boot_id").is_string() &&
                       v.at("boot_id").s == m.sources->context.boot_before;
    if (!known) {
        m.partial = true;
        m.changes_blocked = true;
    }
    auto value = V::object();
    value["ledger_status"] = v.at("ledger_status");
    value["status"] = v.at("status");
    value["history_complete"] = false;
    value["history_pruned"] = v.at("history_pruned");
    value["retained_count"] = static_cast<int>(v.at("records").a.size());
    value["absence_proves_never_executed"] = false;
    if (selected)
        value["requested_operation_id"] = m.request->operation_id;
    m.evidence(id, "operation_history", "historical", known ? "known" : "unknown",
               known ? "bounded_retained_history" : "operation_history_unavailable",
               "command_family_candidates_not_target_correlation", known ? value : V{}, s);
    if (!known)
        return;
    for (const auto& row : v.at("records").a) {
        if (!selected && !family(row.at("name").s, m.request->scenario))
            continue;
        auto summary = V::object();
        for (const auto* key :
             {"id", "phase", "boot_id", "exit_code", "errno", "result", "same_boot"})
            summary[key] = row.at(key);
        summary["correlation"] =
            selected ? "user_selected_not_target_proven" : "same_command_family_candidate";
        summary["target_verified"] = false;
        summary["runtime_verified"] = false;
        auto& recent = m.out["recent_operations"].a;
        const auto found = std::find_if(recent.begin(), recent.end(), [&](const V& old) {
            return old.at("id").s == row.at("id").s;
        });
        if (found != recent.end()) {
            if (selected)
                *found = summary;
            else
                continue;
        } else if (selected) {
            if (recent.size() == 4)
                recent.pop_back();
            recent.insert(recent.begin(), summary);
        } else if (recent.size() < 4)
            recent.push_back(summary);
        else
            continue;
        if (selected) {
            m.step("show_selected_operation", id, "read_only_query",
                   "Read this user-selected operation. Its command family does not establish that "
                   "it targeted the selected resource.",
                   {"operation", "show", row.at("id").s});
            if (row.at("phase").s != "finished")
                m.step("sample_selected_operation", id, "read_only_query",
                       "Sample this operation once without replaying it.",
                       {"operation", "wait", row.at("id").s, "0"});
            else if (hide_action(row.at("name").s) && revision(row.at("request_revision")))
                m.step("read_selected_operation_receipt", id, "read_only_query",
                       "Read the retained resource receipt if available. The receipt may have been "
                       "pruned independently.",
                       {"operation", "result", row.at("id").s});
        }
        if (row.at("phase").s == "started")
            m.finding("recorded_command_outcome_unknown_no_replay", id, "warning");
    }
}

bool valid_request(const Request& request) {
    // Preserve the request byte boundary even when the accepted input omitted
    // optional whitespace (the common JSON dumper inserts spaces after commas).
    std::string bytes =
        "{\"schema_version\":1,\"scenario\":" + json_quote(scenario_name(request.scenario));
    if (!request.definition_id.empty())
        bytes += ",\"definition_id\":" + json_quote(request.definition_id);
    if (!request.module_id.empty())
        bytes += ",\"module_id\":" + json_quote(request.module_id);
    if (!request.operation_id.empty())
        bytes += ",\"operation_id\":" + json_quote(request.operation_id);
    bytes += '}';
    Request parsed;
    std::string reason;
    return parse(bytes, parsed, reason);
}
}  // namespace

Report unavailable(const Request* request, const char* reason, int error_number) {
    auto out = base(request);
    out["status"] = "unavailable";
    out["exit_code"] = 5;
    // Callers may carry raw errno strings; only these stable codes cross the report boundary.
    static const std::set<std::string> reasons{"source_contract_invalid",
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
                                               "input_operation_id"};
    const std::string code = reason ? reason : "";
    out["reason"] = reasons.count(code) ? code : "collector_unavailable";
    out["errno"] = error_number >= 0 && error_number <= 4095 ? error_number : 0;
    return {5, std::move(out)};
}
Report diagnose(const Request& request, const Sources& sources) {
    if (!valid_request(request)) {
        auto r = unavailable(nullptr, "input_invalid", 0);
        r.exit_code = 2;
        r.value["status"] = "invalid";
        r.value["exit_code"] = 2;
        return r;
    }
    auto expected = SourceKind::RecoveryLegacy;
    if (request.scenario == Scenario::UserHide)
        expected = SourceKind::HideSelection;
    else if (request.scenario == Scenario::ModuleFallback)
        expected = SourceKind::ModulePlan;
    else if (request.scenario == Scenario::MapsFailure)
        expected = SourceKind::MapsPreview;
    if (sources.primary.kind != expected || sources.operations.kind != SourceKind::Operations ||
        (request.scenario == Scenario::ModuleFallback) != sources.rebuild.has_value() ||
        (sources.rebuild && sources.rebuild->kind != SourceKind::ModuleRebuild) ||
        request.operation_id.empty() == sources.selected_operation.has_value() ||
        (sources.selected_operation &&
         sources.selected_operation->kind != SourceKind::OperationSelected))
        return unavailable(&request, "source_contract_invalid", 0);
    Model m(request, sources);
    switch (request.scenario) {
    case Scenario::UserHide:
        hide(m);
        break;
    case Scenario::ModuleFallback:
        module(m);
        rebuild(m);
        break;
    case Scenario::MapsFailure:
        maps(m);
        break;
    case Scenario::BootProtection:
        recovery(m);
        break;
    }
    history(m, sources.operations, false);
    if (sources.selected_operation)
        history(m, *sources.selected_operation, true);
    if (m.changes_blocked) {
        auto& steps = m.out["next_steps"].a;
        steps.erase(std::remove_if(
                        steps.begin(), steps.end(),
                        [](const V& step) { return step.at("kind").s == "explicit_user_change"; }),
                    steps.end());
    }
    if (m.invalid)
        return unavailable(&request, "source_contract_invalid", 0);
    if (!m.credible && !m.stale)
        return unavailable(&request, "primary_source_unavailable", sources.primary.error_number);
    if (m.credible) {
        const char* id = "recovery.storage";
        if (request.scenario == Scenario::UserHide)
            id = "hide.saved";
        else if (request.scenario == Scenario::ModuleFallback)
            id = "module.selection";
        else if (request.scenario == Scenario::MapsFailure)
            id = "maps.saved";
        m.step("inspect_saved_profiles", id, "read_only_query",
               "List saved profiles before explicitly selecting a configuration to inspect. A "
               "profile is not proof of a previous runtime state.",
               {"profile", "list"});
        m.step("verify_observer_view", id, "manual_verification",
               "Use dedicated synthetic test resources in the intended application or isolated "
               "process. Record UID, namespace and boot identity; the root control-plane view does "
               "not establish that observer's view.");
    }
    if (m.stale || !m.context_known) {
        for (auto& e : m.out["evidence"].a) {
            if (e.at("classification").s == "historical")
                continue;
            if (!m.stale && e.at("classification").s == "saved")
                continue;
            e["status"] = m.stale ? "stale" : "unknown";
            e["reason"] = m.stale ? "sampling_context_changed" : "sampling_context_unknown";
            e["value"] = V{};
        }
        m.out["findings"] = V::array();
        m.out["next_steps"] = V::array();
    }
    int code = 0;
    m.out["status"] = "observed";
    m.out["reason"] = "limited_evidence_collected_not_health_verdict";
    if (m.stale) {
        code = 4;
        m.out["status"] = "stale";
        m.out["reason"] = "sampling_context_changed";
    } else if (m.partial) {
        code = 3;
        m.out["status"] = "partial";
        m.out["reason"] = "limited_evidence_incomplete";
    }
    m.out["exit_code"] = code;
    return {code, std::move(m.out)};
}
std::string encode_report(const Report& report) {
    const auto fallback = [&] {
        auto value = unavailable(nullptr, "response_capacity", 0).value;
        if (one(report.value.at("scenario"),
                {"rule-not-effective", "module-fallback", "maps-failure", "boot-protection"}))
            value["scenario"] = report.value.at("scenario");
        return stringify_json(value);
    };
    const auto over = [&](const char* field, std::size_t maximum) {
        return !report.value.at(field).is_array() || report.value.at(field).a.size() > maximum;
    };
    if (over("evidence", 16) || over("findings", 32) || over("next_steps", 16) ||
        over("recent_operations", 4))
        return fallback();
    const auto bytes = stringify_json(report.value);
    return bytes.size() <= response_bytes ? bytes : fallback();
}
}  // namespace sumhp::diagnostics
