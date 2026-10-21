package com.zying.zysu.ui.sumh.util

import org.json.JSONArray
import org.json.JSONObject

internal object SUMHModulePreviewFixtures {
    const val revision = "sha256-v1:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    fun bytes(value: String) = JSONObject().put("encoding", "utf-8").put("value", value).put("byte_length", value.toByteArray().size)
    fun document(status: String = "valid") = JSONObject().put("status", status).put("errno", if (status == "missing") 2 else 0)
        .put("revision", if (status == "valid") revision else JSONObject.NULL)
    fun probe() = JSONObject().put("checked", false).put("known", false).put("available", JSONObject.NULL).put("errno", 0).put("detail", bytes(""))
    fun decision(selected: String = "sumh") = JSONObject().put("module_id", bytes("m")).put("module_path", bytes("/modules/m"))
        .put("configured", "sumh").put("global", "auto").put("requested", "sumh").put("selection_source", "module").put("selected", selected)
        .put("reason", "explicit_sumh").put("fallback", false).put("complete", true).put("has_content", true).put("requires_magic", false)
        .put("detail", bytes("")).put("sumh_probe", probe()).put("overlay_probe", probe())
    fun contribution(index: Int = 0, action: String = "ADD", path: String = "/vendor/example", matches: Boolean = false): JSONObject {
        val out = JSONObject().put("index", index).put("action", action).put("path", bytes(path)).put("matches_explain", matches)
        if (action != "HIDE") out.put("source", bytes("/modules/m$path")).put("write_policy", "existing_source_write_through_not_read_only").put("hidden_ancestors", JSONArray())
        return out
    }
    fun event(index: Int = 0, action: String = "ADD", path: String = "/vendor/example", matches: Boolean = false) = JSONObject()
        .put("module_id", bytes("m")).put("original_path", bytes(path)).put("resolved_path", bytes(path)).put("source", bytes("/modules/m$path"))
        .put("action", action).put("reason", "source_entry").put("matched_rule_path", bytes("/vendor")).put("requested_mode", "sumh").put("effective_mode", "sumh")
        .put("hidden_parent", bytes("")).put("alias", false).put("mapping_index", index).put("has_previous_direct", false)
        .put("previous_module_id", bytes("")).put("previous_source", bytes("")).put("matches_explain", matches)
    fun plan(count: Int = 1, explain: Boolean = false, selected: String = "sumh"): JSONObject {
        val actions = JSONArray(); val events = JSONArray()
        repeat(count) { index ->
            val path = if (index == 0) "/vendor/example" else "/vendor/example$index"
            actions.put(contribution(index, path = path, matches = explain)); events.put(event(index, path = path, matches = explain))
        }
        return JSONObject().put("backend_decisions", JSONArray().put(decision(selected))).put("compiled", true).put("complete", true)
            .put("other_backends_unplanned", selected == "magic" || selected == "overlay")
            .put("sumh_emission_module_order", if (selected == "sumh") JSONArray().put(bytes("m")) else JSONArray())
            .put("error", bytes("")).put("events_truncated", false).put("observations_truncated", false)
            .put("ordered_contributions", actions).put("trace", events).put("explain", JSONObject().put("events", if (explain) JSONArray(events.toString()) else JSONArray())
                .put("scope", "observed_compiler_chain_not_runtime_winner").put("no_event_meaning", "no_relevant_compiler_observation_not_proof_path_absent")
                .put("hidden_ancestors", JSONArray()))
            .put("source_observations", count).put("source_revision", revision).put("plan_revision", revision).put("valid_for_current_inputs", true)
    }
    fun configuration(mode: String = "sumh", rules: List<SUMHManager.ModuleRule> = emptyList()) = JSONObject()
        .put("mount_backend", "auto").put("overlayfs_enabled", true).put("magic_mount_enabled", true).put("partitions", JSONArray())
        .put("modules", JSONArray().put(JSONObject().put("id", bytes("m")).put("mode", mode).put("rules", JSONArray(rules.map {
            JSONObject().put("path", bytes(it.path)).put("mode", it.mode)
        }))))
    fun base(status: String = "unknown") = JSONObject().put("schema_version", 1).put("kind", "sumh.module.plan").put("read_only", true)
        .put("status", status).put("reason", "private_preview_guard_unavailable").put("errno", 2).put("comparison_valid", false)
        .put("scope", "current_mountable_modules_hypothetical_compile").put("completeness_scope", "sumh_compilation_only_not_effective_view_or_apply_readiness")
        .put("requires_reboot", true).put("apply_token", JSONObject.NULL)
        .put("unchecked", JSONArray(listOf("runtime_collection_reconciliation", "cross_backend_final_winner", "observer_visibility", "pinned_source_lifetime",
            "full_next_boot_module_set", "apply_transaction", "external_mount_owner")))
        .put("limits", JSONObject().put("input_bytes", 65536).put("response_bytes", 2097152).put("discovery_entries", 512)
            .put("diagnostic_events", 2048).put("source_observations", 4096).put("diagnostic_bytes", 524288).put("plan_actions", 4096))
    fun saved(explainPath: String = "", count: Int = 1): JSONObject {
        val bootPlan = document("missing").put("current_boot", JSONObject.NULL).put("authorizes_apply", false).put("module_ids", JSONArray())
        return base("complete").put("reason", "sumh_compilation_complete").put("errno", 0).put("comparison_valid", true)
            .put("saved_documents", JSONObject().put("config", document()).put("module_modes", document()).put("module_rules", document()))
            .put("boot_id", "boot-one").put("namespace_ino", "18446744073709551615").put("stable", true)
            .put("config_revision", revision).put("module_configuration_revision", revision).put("candidate_revision", JSONObject.NULL)
            .put("source_consistency", "twice_compiled_metadata_not_pinned_or_content_hashed").put("baseline", plan(count, explainPath.isNotEmpty()))
            .put("candidate", JSONObject.NULL).put("diff", JSONObject.NULL)
            .put("configuration", JSONObject().put("before", configuration()).put("after", JSONObject.NULL).put("changed_module_ids", JSONArray()))
            .put("runtime_context", JSONObject().put("known", true).put("instance_id", "5").put("generation", "9"))
            .put("ownership", JSONObject().put("reported_external_owner", JSONObject.NULL).put("external_owner_status", "not_observed")
                .put("external_owner_absence_proven", false).put("boot_plan_status", "missing").put("boot_plan_revision", JSONObject.NULL)
                .put("boot_plan", bootPlan).put("hot_apply_authorized", false))
            .put("explain", JSONObject().put("requested_path", bytes(explainPath)).put("resolved_path", bytes(explainPath)).put("final_runtime_action", "not_observed"))
            .put("candidate_modules_not_mountable", JSONArray())
    }
    fun candidate(request: SUMHModulePreviewRequest): JSONObject {
        require(request.moduleId == "m" && request.candidateJson != null)
        val raw = saved(request.explainPath)
        val candidate = plan(if (request.candidateMode == "none") 0 else 1, request.explainPath.isNotEmpty(), if (request.candidateMode == "none") "none" else "sumh")
        raw.put("candidate", candidate).put("candidate_revision", request.candidateRevision)
        raw.getJSONObject("configuration").put("after", configuration(requireNotNull(request.candidateMode), requireNotNull(request.candidateRules)))
            .put("changed_module_ids", JSONArray().put("m"))
        val changes = JSONArray()
        if (request.candidateMode == "none") changes.put(JSONObject().put("change", "removed").put("count", 1)
            .put("contribution", raw.getJSONObject("baseline").getJSONArray("ordered_contributions").getJSONObject(0)))
        raw.put("diff", JSONObject().put("scope", "ordered_sumh_contributions_not_effective_view").put("complete", true).put("changes", changes))
        return raw
    }
    fun partial(raw: JSONObject, stale: Boolean = false): JSONObject {
        raw.put("status", if (stale) "stale" else "partial").put("reason", if (stale) "inputs_or_planning_changed" else "planning_incomplete")
            .put("errno", if (stale) 116 else 0).put("comparison_valid", false).put("stable", !stale)
        raw.getJSONObject("baseline").put("valid_for_current_inputs", false)
        if (!stale) raw.getJSONObject("baseline").put("complete", false)
        raw.optJSONObject("candidate")?.put("valid_for_current_inputs", false)
        raw.optJSONObject("diff")?.put("complete", false)?.put("changes", JSONArray())
        return raw
    }
    fun envelope(stdout: String, ok: Boolean = true) = JSONObject().put("ok", ok).put("exit_code", if (ok) 0 else 1).put("errno", if (ok) 0 else 5)
        .put("stdout", stdout).put("stderr", if (ok) "" else "transport failed").toString().toByteArray()
    fun capabilities() = JSONObject().put("module_manager_preview_transport_schema", 1).put("module_plan_schema", 1)
        .put("module_plan_input_bytes", 65536).put("module_plan_response_bytes", 2097152).put("module_manager_saved_preview", true)
        .put("module_manager_inline_candidate", true).put("module_manager_explain", true)
}
