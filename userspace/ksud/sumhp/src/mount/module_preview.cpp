#include "mount/module_preview.hpp"
#include "core/partition_config.hpp"
#include "core/rule_inventory.hpp"
#include "core/runtime.hpp"
#include "core/state_ledger.hpp"
#include "mount/development.hpp"
#include "mount/query_worker.hpp"
#include "mount/sumh.hpp"
#include "sumhp/sumh_client.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace sumhp::mount {
namespace {
constexpr std::size_t document_limit = std::size_t{4} * 1024 * 1024;
constexpr std::size_t discovery_limit = 512;
class Fd {
    int value;

public:
    explicit Fd(int descriptor) : value(descriptor) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&&) = delete;
    Fd& operator=(Fd&&) = delete;
    [[nodiscard]] int get() const { return value; }
    ~Fd() {
        if (value >= 0)
            ::close(value);
    }
};
struct Document {
    std::string status;
    std::string bytes;
    int error = 0;
};
bool operator==(const Document& left, const Document& right) {
    return left.status == right.status && left.bytes == right.bytes && left.error == right.error;
}
Document read_document(const std::filesystem::path& path, std::size_t limit,
                       bool stdin_input = false) {
    const Fd fd{stdin_input ? ::dup(STDIN_FILENO)
                            : ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (fd.get() < 0)
        return {errno == ENOENT ? "missing" : "unreadable", {}, errno ? errno : EIO};
    struct stat metadata{};
    if (::fstat(fd.get(), &metadata) != 0)
        return {"unreadable", {}, errno ? errno : EIO};
    if ((!stdin_input && (!S_ISREG(metadata.st_mode) || metadata.st_nlink != 1)) ||
        metadata.st_size < 0)
        return {"unreadable", {}, EINVAL};
    if (static_cast<std::uint64_t>(metadata.st_size) > limit)
        return {"unreadable", {}, EOVERFLOW};
    Document result{"valid", {}, 0};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    char buffer[4096];
    while (result.bytes.size() <= limit) {
        if (std::chrono::steady_clock::now() >= deadline)
            return {"unreadable", {}, ETIMEDOUT};
        const auto count =
            ::read(fd.get(), buffer, std::min(sizeof(buffer), limit + 1 - result.bytes.size()));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return {"unreadable", {}, errno ? errno : EIO};
        if (!count)
            break;
        result.bytes.append(buffer, static_cast<std::size_t>(count));
    }
    if (result.bytes.size() > limit)
        return {"unreadable", {}, EOVERFLOW};
    return result;
}
JsonValue exact(const std::string& bytes) {
    return json::parse(inventory::bytes_json(bytes));
}
JsonValue optional(const std::string& text) {
    return text.empty() ? JsonValue{} : JsonValue(text);
}
JsonValue strings(const std::vector<std::string>& input) {
    auto result = JsonValue::array();
    for (const auto& item : input)
        result.a.emplace_back(item);
    return result;
}
bool within(const std::string& candidate_path, const std::string& ancestor) {
    return candidate_path == ancestor ||
           (!ancestor.empty() && candidate_path.size() > ancestor.size() &&
            candidate_path.compare(0, ancestor.size(), ancestor) == 0 &&
            (ancestor.back() == '/' || candidate_path[ancestor.size()] == '/'));
}
JsonValue base_result(const char* status, const std::string& reason, int code = 0) {
    auto out = JsonValue::object();
    out.o["schema_version"] = 1;
    out.o["kind"] = "sumh.module.plan";
    out.o["read_only"] = true;
    out.o["status"] = status;
    out.o["reason"] = reason;
    out.o["errno"] = code;
    out.o["comparison_valid"] = false;
    out.o["scope"] = "current_mountable_modules_hypothetical_compile";
    out.o["completeness_scope"] = "sumh_compilation_only_not_effective_view_or_apply_readiness";
    out.o["requires_reboot"] = true;
    out.o["apply_token"] = JsonValue{};
    out.o["unchecked"] =
        strings({"runtime_collection_reconciliation", "cross_backend_final_winner",
                 "observer_visibility", "pinned_source_lifetime", "full_next_boot_module_set",
                 "apply_transaction", "external_mount_owner"});
    auto limits = JsonValue::object();
    limits.o["input_bytes"] = static_cast<int>(MODULE_PREVIEW_INPUT_BYTES);
    limits.o["response_bytes"] = static_cast<int>(MODULE_PREVIEW_RESPONSE_BYTES);
    limits.o["discovery_entries"] = static_cast<int>(discovery_limit);
    limits.o["diagnostic_events"] = static_cast<int>(sumh::ModulePlanDiagnostics::max_events);
    limits.o["source_observations"] =
        static_cast<int>(sumh::ModulePlanDiagnostics::max_observations);
    limits.o["diagnostic_bytes"] = static_cast<int>(sumh::ModulePlanDiagnostics::max_bytes);
    limits.o["plan_actions"] = static_cast<int>(sumh::ModulePlanDiagnostics::max_actions);
    out.o["limits"] = std::move(limits);
    return out;
}
JsonValue probe_json(const BackendProbe& probe) {
    auto value = JsonValue::object();
    value.o["checked"] = probe.checked;
    value.o["known"] = probe.known;
    value.o["available"] = probe.known ? JsonValue(probe.available) : JsonValue{};
    value.o["errno"] = probe.error_number;
    value.o["detail"] = exact(probe.detail);
    return value;
}
JsonValue decision_json(const ModuleEntry& module, const ModuleBackendDecision& decision) {
    auto value = JsonValue::object();
    value.o["module_id"] = exact(module.id);
    value.o["module_path"] = exact(module.path.string());
    value.o["configured"] = decision.configured_mode;
    value.o["global"] = decision.global_mode;
    value.o["requested"] = decision.requested;
    value.o["selection_source"] = decision.source;
    value.o["selected"] = decision.selected;
    value.o["reason"] = decision.reason;
    value.o["fallback"] = decision.used_fallback;
    value.o["complete"] = decision.complete;
    value.o["has_content"] = decision.has_content;
    value.o["requires_magic"] = decision.requires_magic;
    value.o["detail"] = exact(decision.observation_error);
    value.o["sumh_probe"] = probe_json(decision.sumh);
    value.o["overlay_probe"] = probe_json(decision.overlay);
    return value;
}
JsonValue event_json(const sumh::ModulePlanEvent& event) {
    auto value = JsonValue::object();
    value.o["module_id"] = exact(event.module_id);
    value.o["original_path"] = exact(event.original_path);
    value.o["resolved_path"] = exact(event.resolved_path);
    value.o["source"] = exact(event.source);
    value.o["action"] = event.action;
    value.o["reason"] = event.reason;
    value.o["matched_rule_path"] = exact(event.matched_rule_path);
    value.o["requested_mode"] = event.requested_mode;
    value.o["effective_mode"] = event.effective_mode;
    value.o["hidden_parent"] = exact(event.hidden_parent);
    value.o["alias"] = event.alias;
    value.o["mapping_index"] = static_cast<int>(event.mapping_index);
    value.o["has_previous_direct"] = event.has_previous_direct;
    value.o["previous_module_id"] = exact(event.previous_module_id);
    value.o["previous_source"] = exact(event.previous_source);
    return value;
}
struct Planned {
    JsonValue output = JsonValue::object();
    std::string observation_signature;
    bool complete = false;
};
Planned compile(const std::vector<ModuleEntry>& modules, const Config& config,
                const ModuleModeMap& modes, const ModuleRuleMap& rules,
                const std::string& explain) {
    Planned result;
    auto backends = JsonValue::array();
    std::vector<ModuleEntry> selected;
    bool decisions_complete = true;
    bool other_backends = false;
    std::string rejection;
    for (const auto& module : modules) {
        const auto decision = resolve_module_backend_decision(module, config, modes);
        decisions_complete = decision.complete && decisions_complete;
        other_backends =
            other_backends || decision.selected == "magic" || decision.selected == "overlay";
        const auto found = rules.find(module.id);
        const std::vector<ModuleRule> empty;
        std::string safety_error;
        if (!validate_module_backend_rules(decision.requested, decision.selected,
                                           found == rules.end() ? empty : found->second,
                                           safety_error))
            rejection += safety_error + "\n";
        if (decision.selected == "sumh")
            selected.push_back(module);
        backends.a.push_back(decision_json(module, decision));
    }
    sumh::ModuleMountPlan plan;
    sumh::ModulePlanDiagnostics diagnostics;
    std::string error;
    bool prepared = rejection.empty() && decisions_complete;
    if (prepared && !selected.empty())
        prepared =
            sumh::prepare_modules_diagnostic(selected, config, rules, plan, diagnostics, error);
    else if (prepared)
        diagnostics.complete = true;
    if (!rejection.empty())
        error = rejection;
    result.complete = prepared && diagnostics.complete && decisions_complete;
    result.output.o["backend_decisions"] = std::move(backends);
    result.output.o["compiled"] = prepared;
    result.output.o["complete"] = result.complete;
    result.output.o["other_backends_unplanned"] = other_backends;
    auto order = JsonValue::array();
    for (auto it = selected.rbegin(); it != selected.rend(); ++it)
        order.a.push_back(exact(it->id));
    result.output.o["sumh_emission_module_order"] = std::move(order);
    result.output.o["error"] = exact(error);
    result.output.o["events_truncated"] = diagnostics.events_truncated;
    result.output.o["observations_truncated"] = diagnostics.observations_truncated;
    auto actions = JsonValue::array();
    for (std::size_t i = 0; i < plan.mappings.size(); ++i) {
        const auto& mapping = plan.mappings[i];
        auto action = JsonValue::object();
        action.o["index"] = static_cast<int>(i);
        action.o["action"] = mapping.merge ? "MERGE" : "ADD";
        action.o["path"] = exact(mapping.path);
        action.o["source"] = exact(mapping.source);
        action.o["write_policy"] = "existing_source_write_through_not_read_only";
        action.o["matches_explain"] =
            !explain.empty() &&
            (mapping.merge ? within(explain, mapping.path) : explain == mapping.path);
        auto hidden = JsonValue::array();
        for (const auto& path : plan.hide)
            if (within(mapping.path, path))
                hidden.a.push_back(exact(path));
        action.o["hidden_ancestors"] = std::move(hidden);
        actions.a.push_back(std::move(action));
    }
    for (const auto& path : plan.hide) {
        auto action = JsonValue::object();
        action.o["index"] = static_cast<int>(actions.a.size());
        action.o["action"] = "HIDE";
        action.o["path"] = exact(path);
        action.o["matches_explain"] = !explain.empty() && within(explain, path);
        actions.a.push_back(std::move(action));
    }
    result.output.o["ordered_contributions"] = std::move(actions);
    auto events = JsonValue::array();
    auto relevant = JsonValue::array();
    for (const auto& event : diagnostics.events) {
        auto value = event_json(event);
        // This is a selection of actual compiler observations, not a second
        // rule matcher. RULE includes ancestor candidates, while ADD/MERGE
        // and HIDE retain the production event's matched_rule_path.
        const bool matches =
            !explain.empty() &&
            ((!event.resolved_path.empty() && within(explain, event.resolved_path)) ||
             (!event.hidden_parent.empty() && within(explain, event.hidden_parent)));
        value.o["matches_explain"] = matches;
        if (matches)
            relevant.a.push_back(value);
        events.a.push_back(std::move(value));
    }
    result.output.o["trace"] = std::move(events);
    auto explanation = JsonValue::object();
    explanation.o["events"] = std::move(relevant);
    explanation.o["scope"] = "observed_compiler_chain_not_runtime_winner";
    explanation.o["no_event_meaning"] = "no_relevant_compiler_observation_not_proof_path_absent";
    auto covered = JsonValue::array();
    if (!explain.empty())
        for (const auto& path : plan.hide)
            if (within(explain, path))
                covered.a.push_back(exact(path));
    explanation.o["hidden_ancestors"] = std::move(covered);
    result.output.o["explain"] = std::move(explanation);
    // Sort metadata observations, since directory iteration ordering need not
    // remain stable. Ordered actual contributions are compared separately.
    std::vector<std::string> observations;
    for (const auto& observation : diagnostics.observations) {
        auto item = JsonValue::array();
        item.a.push_back(exact(observation.module_id));
        item.a.push_back(exact(observation.path));
        item.a.push_back(exact(observation.metadata));
        observations.push_back(stringify_json(item));
    }
    std::sort(observations.begin(), observations.end());
    result.observation_signature = stringify_json(strings(observations));
    result.output.o["source_observations"] = static_cast<int>(observations.size());
    const auto source_revision =
        result.complete ? state::revision_for_bytes(result.observation_signature) : std::string{};
    result.output.o["source_revision"] = optional(source_revision);
    const auto plan_revision =
        result.complete ? state::revision_for_bytes(stringify_json(result.output)) : std::string{};
    result.output.o["plan_revision"] = optional(plan_revision);
    if (result.complete && (source_revision.empty() || plan_revision.empty())) {
        result.complete = false;
        result.output.o["complete"] = false;
        result.output.o["error"] = exact("revision calculation failed");
    }
    return result;
}

JsonValue configuration_json(const Config& config, const ModuleModeMap& modes,
                             const ModuleRuleMap& rules) {
    auto out = JsonValue::object();
    out.o["mount_backend"] = config.mount_backend;
    out.o["overlayfs_enabled"] = config.overlayfs_enabled;
    out.o["magic_mount_enabled"] = config.magic_mount_enabled;
    auto parts = JsonValue::array();
    for (const auto& path : config.partitions)
        parts.a.push_back(exact(path));
    out.o["partitions"] = std::move(parts);
    std::set<std::string> ids;
    for (const auto& [id, mode] : modes)
        ids.insert(id);
    for (const auto& [id, list] : rules)
        ids.insert(id);
    auto modules = JsonValue::array();
    for (const auto& id : ids) {
        auto item = JsonValue::object();
        item.o["id"] = exact(id);
        const auto mode = modes.find(id);
        item.o["mode"] = mode == modes.end() ? "auto" : mode->second;
        auto list = JsonValue::array();
        if (const auto found = rules.find(id); found != rules.end())
            for (const auto& rule : found->second) {
                auto entry = JsonValue::object();
                entry.o["path"] = exact(rule.path);
                entry.o["mode"] = rule.mode;
                list.a.push_back(std::move(entry));
            }
        item.o["rules"] = std::move(list);
        modules.a.push_back(std::move(item));
    }
    out.o["modules"] = std::move(modules);
    return out;
}

JsonValue document_json(const Document& doc) {
    auto out = JsonValue::object();
    out.o["status"] = doc.status;
    out.o["errno"] = doc.error;
    out.o["revision"] =
        doc.status == "valid" ? optional(state::revision_for_bytes(doc.bytes)) : JsonValue{};
    return out;
}

JsonValue boot_plan_json(const Document& doc, const std::string& boot) {
    auto out = document_json(doc);
    out.o["current_boot"] = JsonValue{};
    out.o["authorizes_apply"] = false;
    auto ids = JsonValue::array();
    if (doc.status == "valid") {
        std::istringstream stream(doc.bytes);
        std::string marker_boot;
        std::string line;
        std::getline(stream, marker_boot);
        bool valid = !marker_boot.empty() && inventory::valid_utf8(marker_boot);
        std::set<std::string> seen;
        while (std::getline(stream, line)) {
            if (line.empty())
                continue;
            if (!valid_module_id(line) || !inventory::valid_utf8(line) ||
                !seen.insert(line).second || seen.size() > discovery_limit) {
                valid = false;
                break;
            }
            ids.a.push_back(exact(line));
        }
        out.o["format_valid"] = valid;
        if (valid) {
            out.o["boot_id"] = marker_boot;
            out.o["current_boot"] = marker_boot == boot;
        } else {
            out.o["status"] = "invalid";
            ids.a.clear();
        }
    }
    out.o["module_ids"] = std::move(ids);
    return out;
}
JsonValue actions_diff(const Planned& before, const Planned& after) {
    auto out = JsonValue::object();
    out.o["scope"] = "ordered_sumh_contributions_not_effective_view";
    out.o["complete"] = before.complete && after.complete;
    auto changes = JsonValue::array();
    if (before.complete && after.complete) {
        std::map<std::string, int> counts;
        for (const auto& value : before.output.o.at("ordered_contributions").a)
            --counts[stringify_json(value)];
        for (const auto& value : after.output.o.at("ordered_contributions").a)
            ++counts[stringify_json(value)];
        for (const auto& [key, count] : counts)
            if (count) {
                auto item = JsonValue::object();
                item.o["change"] = count < 0 ? "removed" : "added";
                item.o["count"] = count < 0 ? -count : count;
                item.o["contribution"] = json::parse(key);
                changes.a.push_back(std::move(item));
            }
    }
    out.o["changes"] = std::move(changes);
    return out;
}
std::string module_signature(const std::vector<ModuleEntry>& modules) {
    auto array = JsonValue::array();
    for (const auto& module : modules) {
        auto row = JsonValue::array();
        row.a.push_back(exact(module.id));
        row.a.push_back(exact(module.path.string()));
        array.a.push_back(std::move(row));
    }
    return stringify_json(array);
}
JsonValue observe(const ModulePreviewRequest& request) {
    auto out = base_result("unknown", "not_sampled");
    const auto root = runtime_data_dir();
    const auto boot = runtime_boot_id();
    // The legacy external-owner helper can scan/read unbounded module.prop
    // files. Do not call it from a bounded diagnostic. Ownership stays unknown
    // until a checked owner observation API exists; this result cannot apply.
    const auto config_doc = read_document(runtime_config_file(), document_limit);
    const auto modes_doc = read_document(root / "module_mode.json", document_limit);
    const auto rules_doc = read_document(root / "module_rules.json", document_limit);
    const auto boot_plan =
        read_document(root / "run" / "sumh_mapping_plan", MODULE_PREVIEW_INPUT_BYTES);
    auto documents = JsonValue::object();
    documents.o["config"] = document_json(config_doc);
    documents.o["module_modes"] = document_json(modes_doc);
    documents.o["module_rules"] = document_json(rules_doc);
    out.o["saved_documents"] = std::move(documents);
    const auto status_ok = [](const Document& doc) {
        return doc.status == "valid" || doc.status == "missing";
    };
    const auto fail = [&](const std::string& reason, const std::string& error, int code) {
        out.o["reason"] = reason;
        out.o["detail"] = exact(error);
        out.o["errno"] = code;
        return out;
    };
    Config config;
    ModuleModeMap modes;
    ModuleRuleMap rules;
    std::string error;
    if (boot.empty())
        return fail("boot_unknown", "boot identity unavailable", EIO);
    if (config_doc.status != "valid" || !inventory::valid_utf8(config_doc.bytes) ||
        !parse_config_json(config_doc.bytes, config, error))
        return fail("saved_config_unavailable", error,
                    config_doc.error ? config_doc.error : EINVAL);
    if (!status_ok(modes_doc) || !status_ok(rules_doc) || !inventory::valid_utf8(modes_doc.bytes) ||
        !inventory::valid_utf8(rules_doc.bytes) ||
        !parse_module_configuration_documents(
            modes_doc.status == "missing" ? "{}" : modes_doc.bytes,
            rules_doc.status == "missing" ? "{}" : rules_doc.bytes, modes, rules, error))
        return fail("module_configuration_unavailable", error, EIO);
    std::vector<ModuleEntry> modules;
    if (!enumerate_mountable_modules_checked(modules, error, discovery_limit))
        return fail("module_discovery_failed", error, errno ? errno : EIO);
    std::string explain;
    if (!request.explain_path.empty() &&
        !sumh::resolve_module_plan_path(request.explain_path, explain, error))
        return fail("explain_path_unresolved", error, errno ? errno : EIO);
    Config candidate_config = config;
    auto candidate_modes = modes;
    auto candidate_rules = rules;
    std::vector<std::string> changed_ids;
    Document input;
    const bool has_candidate = request.inline_input.has_value() || !request.input_path.empty();
    if (has_candidate) {
        input = request.inline_input ? Document{"valid", *request.inline_input, 0}
                                     : read_document(request.input_path, MODULE_PREVIEW_INPUT_BYTES,
                                                     request.input_path == "-");
        if (input.status != "valid" ||
            !apply_module_preview_input(input.bytes, candidate_config, candidate_modes,
                                        candidate_rules, changed_ids, error)) {
            out.o["status"] = "invalid";
            return fail("candidate_rejected", error, input.error ? input.error : EINVAL);
        }
    }
    ::sumhp::sumh::RuntimeState runtime_before;
    ::sumhp::sumh::RuntimeState runtime_after;
    const bool runtime_known = ::sumhp::sumh::runtime_state(runtime_before);
    const auto baseline = compile(modules, config, modes, rules, explain);
    const auto candidate = has_candidate ? compile(modules, candidate_config, candidate_modes,
                                                   candidate_rules, explain)
                                         : Planned{};
    const auto baseline_check = compile(modules, config, modes, rules, explain);
    const auto candidate_check = has_candidate ? compile(modules, candidate_config, candidate_modes,
                                                         candidate_rules, explain)
                                               : Planned{};
    const bool runtime_known_after = ::sumhp::sumh::runtime_state(runtime_after);
    std::string explain_after;
    std::string explain_error;
    const bool explain_stable =
        request.explain_path.empty() ||
        (sumh::resolve_module_plan_path(request.explain_path, explain_after, explain_error) &&
         explain_after == explain);
    std::vector<ModuleEntry> modules_after;
    const bool discovery_after =
        enumerate_mountable_modules_checked(modules_after, error, discovery_limit);
    const bool input_unchanged =
        !has_candidate || request.inline_input.has_value() || request.input_path == "-" ||
        input == read_document(request.input_path, MODULE_PREVIEW_INPUT_BYTES);
    const bool stable =
        explain_stable && input_unchanged && discovery_after &&
        module_signature(modules) == module_signature(modules_after) && boot == runtime_boot_id() &&
        config_doc == read_document(runtime_config_file(), document_limit) &&
        modes_doc == read_document(root / "module_mode.json", document_limit) &&
        rules_doc == read_document(root / "module_rules.json", document_limit) &&
        boot_plan ==
            read_document(root / "run" / "sumh_mapping_plan", MODULE_PREVIEW_INPUT_BYTES) &&
        runtime_known == runtime_known_after &&
        (!runtime_known || (runtime_before.instance_id == runtime_after.instance_id &&
                            runtime_before.generation == runtime_after.generation)) &&
        stringify_json(baseline.output) == stringify_json(baseline_check.output) &&
        baseline.observation_signature == baseline_check.observation_signature &&
        (!has_candidate ||
         (stringify_json(candidate.output) == stringify_json(candidate_check.output) &&
          candidate.observation_signature == candidate_check.observation_signature));
    out.o["boot_id"] = boot;
    out.o["stable"] = stable;
    const auto config_revision = state::revision_for_bytes(config_doc.bytes);
    const auto module_revision = state::revision_for_bytes(
        modes_doc.status + modes_doc.bytes + "\n" + rules_doc.status + rules_doc.bytes);
    const auto input_revision =
        has_candidate ? state::revision_for_bytes(input.bytes) : std::string{};
    const bool revisions_known = !config_revision.empty() && !module_revision.empty() &&
                                 (!has_candidate || !input_revision.empty());
    out.o["config_revision"] = optional(config_revision);
    out.o["module_configuration_revision"] = optional(module_revision);
    out.o["candidate_revision"] = optional(input_revision);
    out.o["source_consistency"] = "twice_compiled_metadata_not_pinned_or_content_hashed";
    out.o["baseline"] = baseline.output;
    out.o["candidate"] = has_candidate ? candidate.output : JsonValue{};
    out.o["diff"] = has_candidate ? actions_diff(baseline, candidate) : JsonValue{};
    auto config_diff = JsonValue::object();
    config_diff.o["before"] = configuration_json(config, modes, rules);
    config_diff.o["after"] =
        has_candidate ? configuration_json(candidate_config, candidate_modes, candidate_rules)
                      : JsonValue{};
    config_diff.o["changed_module_ids"] = strings(changed_ids);
    out.o["configuration"] = std::move(config_diff);
    auto runtime = JsonValue::object();
    runtime.o["known"] = runtime_known && runtime_known_after;
    runtime.o["instance_id"] =
        runtime_known ? JsonValue(std::to_string(runtime_before.instance_id)) : JsonValue{};
    runtime.o["generation"] =
        runtime_known ? JsonValue(std::to_string(runtime_before.generation)) : JsonValue{};
    out.o["runtime_context"] = std::move(runtime);
    auto ownership = JsonValue::object();
    ownership.o["reported_external_owner"] = JsonValue{};
    ownership.o["external_owner_status"] = "not_observed";
    ownership.o["external_owner_absence_proven"] = false;
    ownership.o["boot_plan_status"] = boot_plan.status;
    ownership.o["boot_plan_revision"] = boot_plan.status == "valid"
                                            ? optional(state::revision_for_bytes(boot_plan.bytes))
                                            : JsonValue{};
    ownership.o["boot_plan"] = boot_plan_json(boot_plan, boot);
    ownership.o["hot_apply_authorized"] = false;
    out.o["ownership"] = std::move(ownership);
    auto query = JsonValue::object();
    query.o["requested_path"] = exact(request.explain_path);
    query.o["resolved_path"] = exact(explain);
    query.o["final_runtime_action"] = "not_observed";
    out.o["explain"] = std::move(query);
    auto missing = JsonValue::array();
    for (const auto& id : changed_ids)
        if (std::none_of(modules.begin(), modules.end(),
                         [&](const auto& module) { return module.id == id; }))
            missing.a.emplace_back(id);
    out.o["candidate_modules_not_mountable"] = missing;
    const bool complete = baseline.complete && (!has_candidate || candidate.complete) &&
                          missing.a.empty() && revisions_known;
    out.o["comparison_valid"] = stable && complete;
    out.o["baseline"].o["valid_for_current_inputs"] =
        stable && baseline.complete && revisions_known;
    if (has_candidate) {
        out.o["candidate"].o["valid_for_current_inputs"] =
            stable && candidate.complete && missing.a.empty() && revisions_known;
        out.o["diff"].o["complete"] = stable && complete;
        if (!stable || !revisions_known)
            out.o["diff"].o["changes"] = JsonValue::array();
    }
    out.o["status"] = complete ? "complete" : "partial";
    out.o["reason"] = complete ? "sumh_compilation_complete" : "planning_incomplete";
    if (!stable) {
        out.o["status"] = "stale";
        out.o["reason"] = "inputs_or_planning_changed";
    }
    if (!stable)
        out.o["errno"] = ESTALE;
    return out;
}

struct SharedReply {
    std::uint32_t done = 0, size = 0;
    char bytes[MODULE_PREVIEW_RESPONSE_BYTES]{};
};
}  // namespace

JsonValue module_inspect_observation(const std::string& path) {
    auto out = base_result("unknown", "inspect_not_sampled");
    out.o["kind"] = "sumh.module.inspect";
    out.o["complete"] = false;
    out.o["stable"] = false;
    out.o["sumh_compiled"] = false;
    out.o["limits"].o["response_bytes"] = static_cast<int>(DEVELOPMENT_RESPONSE_BYTES);
    out.o["scope"] = "isolated_module_production_planner_not_installed_set_or_observer_view";
    out.o["module"] = exact(path);
    std::string resolved;
    std::string error;
    if (!magic::inspect_module_root(path, resolved, error)) {
        out.o["status"] = "invalid";
        out.o["reason"] = "unsafe_module_root";
        out.o["detail"] = exact(error);
        return out;
    }
    const auto config_doc = read_document(runtime_config_file(), document_limit);
    Config config;
    if (config_doc.status != "valid" || !parse_partition_config(config_doc.bytes, config, error)) {
        out.o["reason"] = "saved_config_unavailable";
        out.o["detail"] = exact(error);
        return out;
    }
    const auto module_path = std::filesystem::path(resolved);
    const bool installed = module_path.parent_path() == runtime_modules_dir();
    const auto id = installed ? module_path.filename().string() : "inspect.candidate";
    if (installed && !valid_module_id(id)) {
        out.o["status"] = "invalid";
        out.o["reason"] = "invalid_installed_module_id";
        return out;
    }
    ModuleModeMap modes;
    ModuleRuleMap rules;
    Document modes_doc;
    Document rules_doc;
    if (installed) {
        modes_doc = read_document(runtime_data_dir() / "module_mode.json", document_limit);
        rules_doc = read_document(runtime_data_dir() / "module_rules.json", document_limit);
        if ((modes_doc.status != "valid" && modes_doc.status != "missing") ||
            (rules_doc.status != "valid" && rules_doc.status != "missing") ||
            !inventory::valid_utf8(modes_doc.bytes) || !inventory::valid_utf8(rules_doc.bytes) ||
            !parse_module_configuration_documents(
                modes_doc.status == "missing" ? "{}" : modes_doc.bytes,
                rules_doc.status == "missing" ? "{}" : rules_doc.bytes, modes, rules, error)) {
            out.o["reason"] = "module_configuration_unavailable";
            out.o["detail"] = exact(error);
            return out;
        }
    } else {
        // A staging basename never inherits a same-named installed module's rules.
        // Saved global backend settings still participate in the real resolver.
        modes[id] = "sumh";
    }
    const auto boot = runtime_boot_id();
    const auto first = compile({ModuleEntry{id, module_path}}, config, modes, rules, "");
    magic::NormalizationPlan normalization;
    magic::NormalizationPlan normalization_after;
    std::string normalize_error;
    std::string normalize_error_after;
    const bool normalizable = magic::prepare_normalization(path, normalization, normalize_error);
    const auto second = compile({ModuleEntry{id, module_path}}, config, modes, rules, "");
    const bool normalizable_after =
        magic::prepare_normalization(path, normalization_after, normalize_error_after);
    std::string after;
    std::string after_error;
    const bool stable =
        !boot.empty() && boot == runtime_boot_id() &&
        magic::inspect_module_root(path, after, after_error) && after == resolved &&
        config_doc == read_document(runtime_config_file(), document_limit) &&
        (!installed ||
         (modes_doc == read_document(runtime_data_dir() / "module_mode.json", document_limit) &&
          rules_doc == read_document(runtime_data_dir() / "module_rules.json", document_limit))) &&
        first.observation_signature == second.observation_signature &&
        stringify_json(first.output) == stringify_json(second.output) &&
        normalizable == normalizable_after && normalize_error == normalize_error_after &&
        stringify_json(normalization_plan_json(normalization)) ==
            stringify_json(normalization_plan_json(normalization_after));
    out.o["installed_path"] = installed;
    out.o["identity_source"] = installed ? "exact_installed_path_saved_rules"
                                         : "synthetic_sumh_candidate_no_saved_module_rules";
    out.o["saved_global_backend_used"] = true;
    out.o["activation_markers_ignored_for_isolated_inspection"] = true;
    out.o["module_id"] = exact(id);
    out.o["boot_id"] = boot;
    out.o["stable"] = stable;
    out.o["plan"] = first.output;
    const auto revision = state::revision_for_bytes(config_doc.bytes);
    out.o["config_revision"] = optional(revision);
    out.o["normalization"] = normalization_plan_json(normalization);
    out.o["normalization_error"] = exact(normalize_error);
    // A non-SUMH selection was resolved, but its compiler has not been inspected.
    const bool other = first.output.o.at("other_backends_unplanned").b;
    const bool selected = !first.output.o.at("sumh_emission_module_order").a.empty();
    out.o["sumh_compiled"] = selected && first.complete;
    const bool complete = stable && first.complete && !other && selected && !revision.empty();
    out.o["complete"] = complete;
    out.o["comparison_valid"] = false;
    out.o["status"] = complete ? "complete" : "partial";
    out.o["reason"] =
        complete ? "isolated_sumh_compilation_complete" : "backend_or_compilation_incomplete";
    if (!stable) {
        out.o["status"] = "stale";
        out.o["reason"] = "inspection_inputs_changed";
    }
    out.o["source_consistency"] = "twice_compiled_metadata_not_pinned_or_content_hashed";
    return out;
}

std::string module_preview_json(const ModulePreviewRequest& request) {
    const auto failure = [](const char* reason, int code) {
        return stringify_json(base_result("unknown", reason, code));
    };
    if (request.inline_input &&
        (!request.input_path.empty() ||
         !module_preview::valid({request.inline_input, request.explain_path})))
        return stringify_json(base_result("invalid", "candidate_rejected", EINVAL));
    // Lock an existing private directory, without creating or modifying any
    // filesystem object. A stuck query child inherits the lock and prevents
    // a new short-lived CLI from starting unlimited replacement workers.
    const Fd guard{::open(runtime_data_dir().c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    struct stat directory{};
    if (guard.get() < 0 || ::fstat(guard.get(), &directory) != 0 || !S_ISDIR(directory.st_mode) ||
        directory.st_uid != 0 || (directory.st_mode & 0077) != 0)
        return failure("private_preview_guard_unavailable", errno ? errno : EACCES);
    if (::flock(guard.get(), LOCK_EX | LOCK_NB) != 0)
        return failure("module_preview_busy", errno ? errno : EBUSY);
    auto* shared = static_cast<SharedReply*>(::mmap(
        nullptr, sizeof(SharedReply), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (shared == MAP_FAILED)
        return failure("reply_memory_unavailable", errno ? errno : ENOMEM);
    shared->done = 0;
    shared->size = 0;
    const bool ok = fsutil::run_query_in_init_mount_ns([&] {
        struct stat init_directory{};
        struct stat own_ns{};
        struct stat init_ns{};
        if (::stat(runtime_data_dir().c_str(), &init_directory) != 0 ||
            init_directory.st_ino != directory.st_ino ||
            init_directory.st_dev != directory.st_dev ||
            ::stat("/proc/self/ns/mnt", &own_ns) != 0 || ::stat("/proc/1/ns/mnt", &init_ns) != 0 ||
            own_ns.st_ino != init_ns.st_ino || own_ns.st_dev != init_ns.st_dev) {
            errno = ESTALE;
            return false;
        }
        auto value = observe(request);
        value.o["namespace_ino"] = std::to_string(own_ns.st_ino);
        auto bytes = stringify_json(value);
        if (bytes.size() > MODULE_PREVIEW_RESPONSE_BYTES)
            bytes = failure("module_preview_response_capacity", EOVERFLOW);
        std::memcpy(shared->bytes, bytes.data(), bytes.size());
        shared->size = static_cast<std::uint32_t>(bytes.size());
        shared->done = 1;
        return true;
    });
    const int code = errno;
    const std::string output =
        ok && shared->done == 1 && shared->size <= MODULE_PREVIEW_RESPONSE_BYTES
            ? std::string(shared->bytes, shared->size)
            : failure("module_preview_query_failed", code ? code : EIO);
    ::munmap(shared, sizeof(SharedReply));
    return output;
}
}  // namespace sumhp::mount
