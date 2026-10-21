package com.zying.zysu.ui.sumh.util

import org.json.JSONArray
import org.json.JSONObject

internal object SUMHHideDefinitionsFixtures {
    val revision = "sha256-v1:" + "a".repeat(64)
    val nextRevision = "sha256-v1:" + "b".repeat(64)
    const val boot = "synthetic-boot"
    fun id(number: Int) = number.toString(16).padStart(32, '0')
    fun runtime(binding: Int = 2, management: Int = 0) = JSONObject()
        .put("id", "18446744073709551615").put("generation", "18446744073709551614")
        .put("management", management).put("binding", binding).put("errno", if (binding == 3) -2 else 0)
    fun row(number: Int = 1, enabled: Boolean = true, registered: Boolean? = true,
        binding: Int = 2, management: Int = 0, path: String = "/test/$number"): JSONObject {
        val state = when {
            registered == null -> "unknown"
            !enabled -> if (registered) "disabled_pending_removal" else "disabled"
            !registered -> "unregistered"
            management == 1 -> "globally_suspended"
            binding == 2 -> "bound"
            binding == 3 -> "binding_error"
            else -> "pending"
        }
        return JSONObject().put("id", id(number)).put("path", path).put("enabled", enabled)
            .put("name", "名称").put("note", "备注\n保留").put("group", "组 ")
            .put("registered", registered ?: JSONObject.NULL)
            .put("runtime", if (registered == true) runtime(binding, management) else JSONObject.NULL).put("state", state)
    }
    fun page(rows: List<JSONObject> = listOf(row()), count: Int = rows.size, cursor: Int = 0,
        version: String = revision, bootId: String = boot, managed: Boolean? = true,
        query: String = "observed") = JSONObject()
        .put("schema_version", 1).put("resource", "user_hide_definitions").put("read_only", true)
        .put("runtime_verified", false).put("runtime_consistency", "per_rule_sampled")
        .put("scope", "caller_filesystem_definitions_and_kernel_registered_membership")
        .put("status", "observed").put("error", "").put("source_schema", 1)
        .put("revision", version).put("boot_id", bootId).put("managed_ownership", managed ?: JSONObject.NULL)
        .put("runtime_query", query).put("count", count).put("cursor", cursor)
        .put("next_cursor", cursor + rows.size).put("at_end", cursor + rows.size == count)
        .put("complete", cursor == 0 && rows.size == count).put("items", JSONArray(rows))
    fun item(number: Int = 1, outcome: String = "absent", error: Int = 0): JSONObject {
        val binding = when (outcome) { "registered_pending" -> 0; "registered_binding_error" -> 3; else -> 2 }
        return JSONObject().put("id", id(number)).put("outcome", outcome).put("errno", error)
            .put("membership_matches", when (outcome) { "outcome_unknown" -> JSONObject.NULL; "runtime_differs", "mutation_failed_or_partial" -> false; else -> true })
            .put("runtime", if (outcome.startsWith("registered_")) runtime(binding) else JSONObject.NULL)
    }
    fun mutation(action: String = "disable", status: String = "applied", number: Int = 1,
        previous: String = revision, version: String = nextRevision): JSONObject {
        val saved: Any = when (status) { "rejected", "not_saved" -> false; "outcome_unknown" -> JSONObject.NULL; else -> true }
        val active = status == "applied" || status == "partial"
        val outcome = when { status == "partial" -> "mutation_failed_or_partial"; action in setOf("disable", "disable-group", "remove-id") -> "absent"; else -> "registered_bound" }
        return JSONObject().put("schema_version", 1).put("resource", "user_hide_mutation")
            .put("action", action).put("status", status).put("saved", saved)
            .put("revision", if (saved == true) version else JSONObject.NULL).put("previous_revision", previous)
            .put("boot_id", boot).put("runtime_verified", false)
            .put("verification_scope", "registered_membership_not_observer_visibility")
            .put("reason", "synthetic_test").put("error", "")
            .put("affected_count", 1).put("confirmed_count", if (status == "applied") 1 else 0)
            .put("failed_count", if (status == "partial") 1 else 0).put("unknown_count", 0)
            .put("items_complete", true).put("items", JSONArray().apply { if (active) put(item(number, outcome, if (status == "partial") 5 else 0)) })
    }
    fun dispatch(stdout: String = page().toString(), code: Int = 0, stderr: String = "") = JSONObject()
        .put("schema_version", 1).put("resource", "user_hide_dispatch").put("exit_code", code)
        .put("stdout", stdout).put("stderr", stderr).put("stdout_complete", true).put("stderr_complete", true)
    fun envelope(dispatch: JSONObject = dispatch(), ok: Boolean = true) = JSONObject().put("ok", ok)
        .put("exit_code", if (ok) 0 else 1).put("errno", if (ok) 0 else 110)
        .put("stdout", dispatch.toString()).put("stderr", if (ok) "" else "timeout; outcome unknown").toString()
    fun reply(value: JSONObject, error: String? = null) = SUMHQueryReply(value.toString(), error)
    fun parsedMutation(value: JSONObject, action: String = value.getString("action"), expected: String = revision,
        expectedId: String? = id(1), error: String? = null) =
        HideDefinitionsParser.mutation(reply(value, error), action, expected, boot, expectedId)
    fun collection(rows: List<JSONObject> = listOf(row(1), row(2), row(3))): HideDefinitionCollection {
        val parsed = HideDefinitionsParser.page(reply(page(rows)))
        return HideDefinitionCollection(revision, boot, parsed.rows, true, true)
    }
}
