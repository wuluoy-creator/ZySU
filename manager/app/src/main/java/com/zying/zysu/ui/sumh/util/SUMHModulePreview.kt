package com.zying.zysu.ui.sumh.util

import com.google.gson.JsonArray
import com.google.gson.JsonElement
import com.google.gson.JsonNull
import com.google.gson.JsonObject
import com.google.gson.JsonPrimitive
import com.google.gson.Strictness
import com.google.gson.stream.JsonReader
import com.google.gson.stream.JsonToken
import org.json.JSONObject
import java.io.StringReader
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.security.MessageDigest
import java.util.Collections

internal const val SUMH_MODULE_PREVIEW_INPUT_BYTES = 64 * 1024
internal const val SUMH_MODULE_PREVIEW_RESPONSE_BYTES = 2 * 1024 * 1024
internal const val SUMH_MODULE_PREVIEW_ENVELOPE_BYTES = 2 * SUMH_MODULE_PREVIEW_RESPONSE_BYTES + 256
private val moduleModes = setOf("auto", "sumh", "none", "magic", "overlay")
private val moduleRuleModes = moduleModes + "hide"
private val moduleIdPattern = Regex("[A-Za-z0-9_.-]+")
private val moduleRevisionPattern = Regex("sha256-v1:[0-9a-f]{64}")
private fun validPreviewModuleId(value: String) = value != "." && value != ".." && value.matches(moduleIdPattern)
private fun utf8Size(value: String, limit: Int): Int {
    require(value.length <= limit && Charsets.UTF_8.newEncoder().canEncode(value)) { "module_preview_invalid_utf8_or_capacity" }
    return value.toByteArray(Charsets.UTF_8).size.also { require(it <= limit) { "module_preview_capacity" } }
}
private fun <T> immutable(values: List<T>): List<T> = Collections.unmodifiableList(ArrayList(values))
private fun moduleDigest(bytes: String): String = "sha256-v1:" + MessageDigest.getInstance("SHA-256")
    .digest(bytes.toByteArray(Charsets.UTF_8)).joinToString("") { "%02x".format(it.toInt() and 255) }

/** A factory-only immutable snapshot. The original caller's mutable list is never retained. */
internal class SUMHModulePreviewRequest private constructor(
    val moduleId: String, val candidateJson: String?, val explainPath: String,
    val candidateRevision: String?, val candidateMode: String?, val candidateRules: List<SUMHManager.ModuleRule>?,
) {
    companion object {
        private fun validate(id: String, path: String) {
            utf8Size(id, SUMH_MODULE_PREVIEW_INPUT_BYTES)
            require(validPreviewModuleId(id)) { "module_preview_invalid_module_id" }
            utf8Size(path, 4095)
            require(path.isEmpty() || validateSUMHModuleRulePath(path) == null) { "module_preview_invalid_explain_path" }
        }
        fun saved(moduleId: String, explainPath: String = ""): SUMHModulePreviewRequest {
            validate(moduleId, explainPath)
            return SUMHModulePreviewRequest(moduleId, null, explainPath, null, null, null)
        }
        fun candidate(moduleId: String, mode: String, rules: List<SUMHManager.ModuleRule>, explainPath: String = ""): SUMHModulePreviewRequest {
            validate(moduleId, explainPath)
            require(mode in moduleModes) { "module_preview_invalid_mode" }
            val copy = immutable(rules.map { it.copy() })
            for (rule in copy) utf8Size(rule.path, 4095)
            require(validateSUMHModuleRules(copy) == null) { "module_preview_invalid_rules_or_capacity" }
            val replacement = JSONObject().put("id", moduleId).put("mode", mode).put("rules", moduleRulesJson(copy))
            val json = JSONObject().put("schema_version", 1).put("kind", "sumh.module.plan.input")
                .put("modules", org.json.JSONArray().put(replacement)).toString()
            utf8Size(json, SUMH_MODULE_PREVIEW_INPUT_BYTES)
            return SUMHModulePreviewRequest(moduleId, json, explainPath, moduleDigest(json), mode, copy)
        }
    }
}

internal enum class SUMHModulePreviewStatus { Complete, Partial, Stale, Unknown, Invalid }
internal data class SUMHModulePreviewCapabilities(val saved: Boolean, val candidate: Boolean, val explain: Boolean, val error: String? = null)
internal data class SUMHModulePreviewResult(val value: SUMHModulePreview? = null, val error: String? = null)
internal data class SUMHModulePreview(
    val status: SUMHModulePreviewStatus, val reason: String, val errno: Int, val comparisonValid: Boolean,
    val unchecked: List<String>, val detail: SUMHInventoryBytes?, val sample: SUMHModulePreviewSample?,
)
internal data class SUMHModulePreviewSample(
    val bootId: String, val namespace: String, val stable: Boolean,
    val configRevision: String?, val moduleRevision: String?, val candidateRevision: String?,
    val baseline: SUMHModulePlan, val candidate: SUMHModulePlan?, val diff: SUMHModulePlanDiff?,
    val before: SUMHModuleConfiguration, val after: SUMHModuleConfiguration?, val changedModuleIds: List<String>,
    val runtime: SUMHModuleRuntimeContext, val ownership: SUMHModuleOwnership,
    val requestedPath: SUMHInventoryBytes, val resolvedPath: SUMHInventoryBytes,
    val missingCandidateModules: List<String>, val documents: Map<String, SUMHModuleDocument>,
)
internal data class SUMHModulePlan(
    val decisions: List<SUMHModuleBackendDecision>, val compiled: Boolean, val complete: Boolean,
    val otherBackendsUnplanned: Boolean, val moduleOrder: List<SUMHInventoryBytes>, val error: SUMHInventoryBytes,
    val eventsTruncated: Boolean, val observationsTruncated: Boolean, val contributions: List<SUMHModuleContribution>,
    val trace: List<SUMHModulePlanEvent>, val explainEvents: List<SUMHModulePlanEvent>, val hiddenAncestors: List<SUMHInventoryBytes>,
    val sourceObservations: Int, val sourceRevision: String?, val planRevision: String?, val validForCurrentInputs: Boolean,
)
internal data class SUMHModuleBackendProbe(val checked: Boolean, val known: Boolean, val available: Boolean?, val errno: Int, val detail: SUMHInventoryBytes)
internal data class SUMHModuleBackendDecision(
    val moduleId: SUMHInventoryBytes, val modulePath: SUMHInventoryBytes,
    val configured: String, val global: String, val requested: String, val selectionSource: String, val selected: String,
    val reason: String, val fallback: Boolean, val complete: Boolean, val hasContent: Boolean, val requiresMagic: Boolean,
    val detail: SUMHInventoryBytes, val sumhProbe: SUMHModuleBackendProbe, val overlayProbe: SUMHModuleBackendProbe,
)
internal data class SUMHModuleContribution(
    val index: Int, val action: String, val path: SUMHInventoryBytes,
    val source: SUMHInventoryBytes?, val writePolicy: String?, val matchesExplain: Boolean, val hiddenAncestors: List<SUMHInventoryBytes>,
)
internal data class SUMHModulePlanEvent(
    val moduleId: SUMHInventoryBytes, val originalPath: SUMHInventoryBytes, val resolvedPath: SUMHInventoryBytes,
    val source: SUMHInventoryBytes, val action: String, val reason: String, val matchedRulePath: SUMHInventoryBytes,
    val requestedMode: String, val effectiveMode: String, val hiddenParent: SUMHInventoryBytes,
    val alias: Boolean, val mappingIndex: Int, val hasPreviousDirect: Boolean,
    val previousModuleId: SUMHInventoryBytes, val previousSource: SUMHInventoryBytes, val matchesExplain: Boolean,
)
internal data class SUMHModulePlanChange(val change: String, val count: Int, val contribution: SUMHModuleContribution)
internal data class SUMHModulePlanDiff(val complete: Boolean, val changes: List<SUMHModulePlanChange>)
internal data class SUMHModuleSavedRule(val path: SUMHInventoryBytes, val mode: String)
internal data class SUMHModuleSavedConfiguration(val id: SUMHInventoryBytes, val mode: String, val rules: List<SUMHModuleSavedRule>)
internal data class SUMHModuleConfiguration(val mountBackend: String, val overlayEnabled: Boolean, val magicEnabled: Boolean,
    val partitions: List<SUMHInventoryBytes>, val modules: List<SUMHModuleSavedConfiguration>)
internal data class SUMHModuleRuntimeContext(val known: Boolean, val instance: String?, val generation: String?)
internal data class SUMHModuleDocument(val status: String, val errno: Int, val revision: String?)
internal data class SUMHModuleBootPlan(val document: SUMHModuleDocument, val currentBoot: Boolean?, val formatValid: Boolean?,
    val bootId: String?, val moduleIds: List<SUMHInventoryBytes>)
internal data class SUMHModuleOwnership(val bootPlanStatus: String, val bootPlanRevision: String?, val bootPlan: SUMHModuleBootPlan)

/** Local budget; it deliberately does not alter the generic 8192-node state parser. */
internal object SUMHModulePreviewParser {
    fun capabilities(reply: SUMHQueryReply): SUMHModulePreviewCapabilities = try {
        require(reply.error == null) { reply.error ?: "module_preview_capabilities_unavailable" }
        val value = document(reply.stdout, 384 * 1024, 8192)
        val advertised = value.get("module_manager_preview_transport_schema")
        if (advertised == null) SUMHModulePreviewCapabilities(false, false, false, "module_preview_not_advertised")
        else {
            require(value.number("module_manager_preview_transport_schema", 1, Int.MAX_VALUE) == 1 &&
                value.number("module_plan_schema", 1, Int.MAX_VALUE) == 1 &&
                value.number("module_plan_input_bytes", 1, Int.MAX_VALUE) == SUMH_MODULE_PREVIEW_INPUT_BYTES &&
                value.number("module_plan_response_bytes", 1, Int.MAX_VALUE) == SUMH_MODULE_PREVIEW_RESPONSE_BYTES) { "module_preview_unsupported_contract" }
            SUMHModulePreviewCapabilities(value.boolean("module_manager_saved_preview"), value.boolean("module_manager_inline_candidate"),
                value.boolean("module_manager_explain"))
        }
    } catch (error: Exception) { SUMHModulePreviewCapabilities(false, false, false, error.message ?: "module_preview_capabilities_invalid") }

    fun envelope(raw: ByteArray): SUMHQueryReply {
        require(raw.size <= SUMH_MODULE_PREVIEW_ENVELOPE_BYTES) { "module_preview_envelope_capacity" }
        val text = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(raw)).toString()
        val value = document(text, SUMH_MODULE_PREVIEW_ENVELOPE_BYTES, 32)
        value.fields("ok", "exit_code", "errno", "stdout", "stderr")
        val ok = value.boolean("ok"); val exit = value.number("exit_code", Int.MIN_VALUE, Int.MAX_VALUE)
        val errno = value.number("errno", Int.MIN_VALUE, Int.MAX_VALUE)
        val stdout = value.text("stdout", SUMH_MODULE_PREVIEW_RESPONSE_BYTES)
        val stderr = value.text("stderr", 64 * 1024)
        require(stdout.none { it.code < 32 }) { "module_preview_noncompact_payload" }
        return SUMHQueryReply(stdout, if (ok && exit == 0 && errno == 0) null else
            stderr.take(1500).ifEmpty { "module_preview_transport_failed (exit=$exit, errno=$errno)" })
    }

    fun reply(reply: SUMHQueryReply, request: SUMHModulePreviewRequest): SUMHModulePreviewResult = try {
        SUMHModulePreviewResult(parse(reply.stdout, request), reply.error)
    } catch (error: Exception) {
        SUMHModulePreviewResult(error = listOfNotNull(reply.error, error.message ?: "module_preview_invalid_contract").joinToString("\n"))
    }

    fun parse(text: String, request: SUMHModulePreviewRequest): SUMHModulePreview {
        val root = document(text)
        val baseFields = setOf("schema_version", "kind", "read_only", "status", "reason", "errno", "comparison_valid", "scope",
            "completeness_scope", "requires_reboot", "apply_token", "unchecked", "limits")
        val sampledFields = setOf("saved_documents", "boot_id", "stable", "config_revision", "module_configuration_revision", "candidate_revision",
            "source_consistency", "baseline", "candidate", "diff", "configuration", "runtime_context", "ownership", "explain", "candidate_modules_not_mountable", "namespace_ino")
        root.allowed(baseFields + sampledFields + "detail")
        require(root.number("schema_version", 1, Int.MAX_VALUE) == 1 && root.text("kind") == "sumh.module.plan" && root.boolean("read_only"))
        require(root.text("scope") == "current_mountable_modules_hypothetical_compile" &&
            root.text("completeness_scope") == "sumh_compilation_only_not_effective_view_or_apply_readiness" &&
            root.boolean("requires_reboot") && root.required("apply_token").isJsonNull)
        val status = when (root.text("status")) {
            "complete" -> SUMHModulePreviewStatus.Complete; "partial" -> SUMHModulePreviewStatus.Partial
            "stale" -> SUMHModulePreviewStatus.Stale; "unknown" -> SUMHModulePreviewStatus.Unknown
            "invalid" -> SUMHModulePreviewStatus.Invalid; else -> error("module_preview_unknown_status")
        }
        val reason = root.text("reason"); val errno = root.number("errno"); val comparison = root.boolean("comparison_valid")
        val unchecked = immutable(root.array("unchecked", 7).map { require(it.isJsonPrimitive && it.asJsonPrimitive.isString); it.asString })
        require(unchecked.toSet() == setOf("runtime_collection_reconciliation", "cross_backend_final_winner", "observer_visibility",
            "pinned_source_lifetime", "full_next_boot_module_set", "apply_transaction", "external_mount_owner") && unchecked.size == 7)
        val limits = root.obj("limits")
        val expectedLimits = mapOf("input_bytes" to 65536, "response_bytes" to 2097152, "discovery_entries" to 512,
            "diagnostic_events" to 2048, "source_observations" to 4096, "diagnostic_bytes" to 524288, "plan_actions" to 4096)
        limits.fields(*expectedLimits.keys.toTypedArray())
        expectedLimits.forEach { (key, value) -> require(limits.number(key, 0, Int.MAX_VALUE) == value) }
        val detail = root.get("detail")?.let { exact(it, SUMH_MODULE_PREVIEW_RESPONSE_BYTES, false) }
        if (status == SUMHModulePreviewStatus.Unknown || status == SUMHModulePreviewStatus.Invalid) {
            root.allowed(baseFields + setOf("saved_documents", "detail", "namespace_ino"))
            require(!comparison)
            root.get("saved_documents")?.let { documents(it.objectValue()) }
            if (root.has("namespace_ino")) require(root.unsigned("namespace_ino") != null)
            return SUMHModulePreview(status, reason, errno, false, unchecked, detail, null)
        }
        val docs = documents(root.obj("saved_documents"))
        val boot = root.text("boot_id", 80).also { require(it.isNotEmpty() && '\u0000' !in it) }
        val namespace = requireNotNull(root.unsigned("namespace_ino"))
        val stable = root.boolean("stable")
        val configRevision = root.revision("config_revision"); val moduleRevision = root.revision("module_configuration_revision")
        val candidateRevision = root.revision("candidate_revision")
        require(root.text("source_consistency") == "twice_compiled_metadata_not_pinned_or_content_hashed")
        val baseline = plan(root.obj("baseline"))
        val candidate = root.required("candidate").takeUnless { it.isJsonNull }?.let { plan(it.objectValue()) }
        val diff = root.required("diff").takeUnless { it.isJsonNull }?.let { diff(it.objectValue()) }
        val configurationObject = root.obj("configuration").also { it.fields("before", "after", "changed_module_ids") }
        val before = configuration(configurationObject.obj("before"))
        val after = configurationObject.required("after").takeUnless { it.isJsonNull }?.let { configuration(it.objectValue()) }
        val changed = configurationObject.ids("changed_module_ids", 256)
        val missing = root.ids("candidate_modules_not_mountable", 256)
        val live = root.obj("runtime_context").also { it.fields("known", "instance_id", "generation") }
        val runtime = SUMHModuleRuntimeContext(live.boolean("known"), live.unsigned("instance_id"), live.unsigned("generation"))
        require((runtime.instance == null) == (runtime.generation == null) && (!runtime.known || runtime.instance != null))
        val ownership = ownership(root.obj("ownership"), boot)
        val explain = root.obj("explain").also { it.fields("requested_path", "resolved_path", "final_runtime_action") }
        require(explain.text("final_runtime_action") == "not_observed")
        val requested = explain.bytes("requested_path"); val resolved = explain.bytes("resolved_path", SUMH_MODULE_PREVIEW_RESPONSE_BYTES)
        require(requested.utf8 == request.explainPath) { "module_preview_explain_mismatch" }
        require((requested.size == 0) == (resolved.size == 0))
        if (request.candidateJson == null) {
            require(candidateRevision == null && candidate == null && diff == null && after == null && changed.isEmpty() && missing.isEmpty()) {
                "module_preview_unexpected_candidate"
            }
        } else {
            require(candidate != null && diff != null && after != null)
            require(candidateRevision == null || candidateRevision == request.candidateRevision) { "module_preview_candidate_digest_mismatch" }
            require(changed.isEmpty() || changed == listOf(request.moduleId)) { "module_preview_candidate_target_mismatch" }
            require(missing.all { it == request.moduleId })
            val target = after.modules.singleOrNull { it.id.utf8 == request.moduleId }
            require(target != null && target.mode == request.candidateMode &&
                target.rules.map { it.path.utf8 to it.mode } == request.candidateRules!!.map { it.path to it.mode }) { "module_preview_candidate_definition_mismatch" }
            if (changed.isEmpty()) require(before.modules.singleOrNull { it.id.utf8 == request.moduleId } == target) { "module_preview_omitted_changed_target" }
            // The Manager submits one replacement only; other saved configuration must be preserved.
            require(before.mountBackend == after.mountBackend && before.overlayEnabled == after.overlayEnabled && before.magicEnabled == after.magicEnabled && before.partitions == after.partitions &&
                before.modules.filterNot { it.id.utf8 == request.moduleId } == after.modules.filterNot { it.id.utf8 == request.moduleId }) { "module_preview_unrequested_configuration_change" }
        }
        val revisionsKnown = configRevision != null && moduleRevision != null && (candidate == null || candidateRevision != null)
        require(docs.getValue("config").status == "valid" && docs.getValue("config").revision == configRevision &&
            docs.filterKeys { it != "config" }.values.all { it.status == "valid" || it.status == "missing" })
        val complete = baseline.complete && (candidate == null || candidate.complete) && missing.isEmpty() && revisionsKnown
        require(comparison == (stable && complete)) { "module_preview_comparison_conflict" }
        require(baseline.validForCurrentInputs == (stable && baseline.complete && revisionsKnown))
        if (candidate != null) require(candidate.validForCurrentInputs == (stable && candidate.complete && missing.isEmpty() && revisionsKnown))
        when (status) {
            SUMHModulePreviewStatus.Complete -> require(stable && complete && errno == 0 && reason == "sumh_compilation_complete")
            SUMHModulePreviewStatus.Partial -> require(stable && !complete && errno == 0 && reason == "planning_incomplete")
            SUMHModulePreviewStatus.Stale -> require(!stable && !comparison && errno == 116 && reason == "inputs_or_planning_changed")
            else -> error("module_preview_status")
        }
        if (diff != null) {
            require(diff.complete == (stable && complete))
            if (!stable || !revisionsKnown || !baseline.complete || candidate?.complete != true) require(diff.changes.isEmpty())
            else {
                val counts = HashMap<SUMHModuleContribution, Int>()
                for (item in baseline.contributions) counts[item] = (counts[item] ?: 0) - 1
                for (item in requireNotNull(candidate).contributions) counts[item] = (counts[item] ?: 0) + 1
                val expected = counts.filterValues { it != 0 }.map { (item, count) ->
                    SUMHModulePlanChange(if (count < 0) "removed" else "added", kotlin.math.abs(count), item)
                }.toSet()
                require(diff.changes.toSet() == expected) { "module_preview_diff_mismatch" }
            }
        }
        if (request.explainPath.isEmpty()) for (item in listOfNotNull(baseline, candidate)) {
            require(item.explainEvents.isEmpty() && item.hiddenAncestors.isEmpty() && item.trace.none { it.matchesExplain } && item.contributions.none { it.matchesExplain })
        }
        val sample = SUMHModulePreviewSample(boot, namespace, stable, configRevision, moduleRevision, candidateRevision,
            baseline, candidate, diff, before, after, changed, runtime, ownership, requested, resolved, missing, docs)
        return SUMHModulePreview(status, reason, errno, comparison, unchecked, detail, sample)
    }

    private fun documents(value: JsonObject): Map<String, SUMHModuleDocument> {
        value.fields("config", "module_modes", "module_rules")
        return Collections.unmodifiableMap(value.keySet().associateWith { documentStatus(value.obj(it)) })
    }
    private fun documentStatus(value: JsonObject, extra: Boolean = false): SUMHModuleDocument {
        if (!extra) value.fields("status", "errno", "revision")
        val status = value.text("status", 30).also { require(it in setOf("valid", "missing", "unreadable", "invalid")) }
        val revision = value.revision("revision")
        require(status == "valid" || status == "invalid" || revision == null)
        return SUMHModuleDocument(status, value.number("errno"), revision)
    }
    private fun ownership(value: JsonObject, boot: String): SUMHModuleOwnership {
        value.fields("reported_external_owner", "external_owner_status", "external_owner_absence_proven", "boot_plan_status", "boot_plan_revision", "boot_plan", "hot_apply_authorized")
        require(value.required("reported_external_owner").isJsonNull && value.text("external_owner_status") == "not_observed" &&
            !value.boolean("external_owner_absence_proven") && !value.boolean("hot_apply_authorized"))
        val status = value.text("boot_plan_status", 30).also { require(it in setOf("valid", "missing", "unreadable")) }
        val revision = value.revision("boot_plan_revision")
        val raw = value.obj("boot_plan")
        raw.allowed(setOf("status", "errno", "revision", "current_boot", "authorizes_apply", "module_ids", "format_valid", "boot_id"))
        require(!raw.boolean("authorizes_apply"))
        val document = documentStatus(raw, true)
        val current = raw.nullableBoolean("current_boot")
        val format = raw.get("format_valid")?.let { raw.boolean("format_valid") }
        val bootId = raw.get("boot_id")?.let { raw.text("boot_id", SUMH_MODULE_PREVIEW_INPUT_BYTES) }
        val ids = raw.byteList("module_ids", 512, SUMH_MODULE_PREVIEW_INPUT_BYTES)
        require(ids.all { it.utf8 != null && validPreviewModuleId(it.utf8) } && ids.map { it.hex }.distinct().size == ids.size)
        if (status == "valid") {
            require(format != null && document.revision == revision)
            if (format) require(document.status == "valid" && bootId != null && bootId.isNotEmpty() && current == (bootId == boot))
            else require(document.status == "invalid" && bootId == null && current == null && ids.isEmpty())
        } else require(document.status == status && revision == null && format == null && bootId == null && current == null && ids.isEmpty())
        return SUMHModuleOwnership(status, revision, SUMHModuleBootPlan(document, current, format, bootId, ids))
    }
    private fun configuration(value: JsonObject): SUMHModuleConfiguration {
        value.fields("mount_backend", "overlayfs_enabled", "magic_mount_enabled", "partitions", "modules")
        val modules = immutable(value.array("modules").map { raw ->
            val item = raw.objectValue().also { it.fields("id", "mode", "rules") }
            val id = item.bytes("id", SUMH_MODULE_PREVIEW_RESPONSE_BYTES).also { require(it.utf8 != null && validPreviewModuleId(it.utf8)) }
            val rules = immutable(item.array("rules").map { entry ->
                val rule = entry.objectValue().also { it.fields("path", "mode") }
                val path = rule.bytes("path").also {
                    require(it.utf8 != null && it.utf8.startsWith('/') && '\r' !in it.utf8 && '\n' !in it.utf8)
                }
                SUMHModuleSavedRule(path, rule.mode("mode", true))
            })
            SUMHModuleSavedConfiguration(id, item.mode("mode"), rules)
        })
        require(modules.map { it.id.hex }.distinct().size == modules.size)
        return SUMHModuleConfiguration(value.mode("mount_backend"), value.boolean("overlayfs_enabled"), value.boolean("magic_mount_enabled"),
            value.byteList("partitions", 262144, SUMH_MODULE_PREVIEW_RESPONSE_BYTES), modules)
    }
    private fun probe(value: JsonObject): SUMHModuleBackendProbe {
        value.fields("checked", "known", "available", "errno", "detail")
        val checked = value.boolean("checked"); val known = value.boolean("known"); val available = value.nullableBoolean("available")
        require((available != null) == known && (!known || checked))
        return SUMHModuleBackendProbe(checked, known, available, value.number("errno"), value.bytes("detail", SUMH_MODULE_PREVIEW_RESPONSE_BYTES, false))
    }
    private fun decision(value: JsonObject): SUMHModuleBackendDecision {
        value.fields("module_id", "module_path", "configured", "global", "requested", "selection_source", "selected", "reason", "fallback", "complete",
            "has_content", "requires_magic", "detail", "sumh_probe", "overlay_probe")
        val selected = value.mode("selected").also { require(it != "auto") }
        val source = value.text("selection_source", 20).also { require(it in setOf("default", "module", "global")) }
        return SUMHModuleBackendDecision(value.bytes("module_id", SUMH_MODULE_PREVIEW_RESPONSE_BYTES), value.bytes("module_path", SUMH_MODULE_PREVIEW_RESPONSE_BYTES),
            value.mode("configured"), value.mode("global"), value.mode("requested"), source, selected, value.text("reason"), value.boolean("fallback"), value.boolean("complete"),
            value.boolean("has_content"), value.boolean("requires_magic"), value.bytes("detail", SUMH_MODULE_PREVIEW_RESPONSE_BYTES, false), probe(value.obj("sumh_probe")), probe(value.obj("overlay_probe")))
    }
    private fun contribution(value: JsonObject): SUMHModuleContribution {
        val action = value.text("action", 10).also { require(it in setOf("ADD", "MERGE", "HIDE")) }
        val base = arrayOf("index", "action", "path", "matches_explain")
        if (action == "HIDE") value.fields(*base) else value.fields(*base, "source", "write_policy", "hidden_ancestors")
        val write = if (action == "HIDE") null else value.text("write_policy").also { require(it == "existing_source_write_through_not_read_only") }
        return SUMHModuleContribution(value.number("index", 0, 4095), action, value.bytes("path", SUMH_MODULE_PREVIEW_RESPONSE_BYTES),
            if (action == "HIDE") null else value.bytes("source", SUMH_MODULE_PREVIEW_RESPONSE_BYTES), write, value.boolean("matches_explain"),
            if (action == "HIDE") emptyList() else value.byteList("hidden_ancestors", 4096, SUMH_MODULE_PREVIEW_RESPONSE_BYTES))
    }
    private fun event(value: JsonObject): SUMHModulePlanEvent {
        value.fields("module_id", "original_path", "resolved_path", "source", "action", "reason", "matched_rule_path", "requested_mode", "effective_mode",
            "hidden_parent", "alias", "mapping_index", "has_previous_direct", "previous_module_id", "previous_source", "matches_explain")
        val action = value.text("action", 20).also { require(it in setOf("RULE", "WALK", "SKIP", "POLICY", "ADD", "MERGE", "HIDE", "ERROR")) }
        val requested = value.text("requested_mode", 20).also { require(it.isEmpty() || it in moduleRuleModes) }
        val effective = value.text("effective_mode", 20).also { require(it.isEmpty() || it in moduleRuleModes) }
        val previous = value.boolean("has_previous_direct")
        val previousId = value.bytes("previous_module_id", SUMH_MODULE_PREVIEW_RESPONSE_BYTES)
        val previousSource = value.bytes("previous_source", SUMH_MODULE_PREVIEW_RESPONSE_BYTES)
        require(if (previous) action == "ADD" && previousId.size > 0 && previousSource.size > 0 else previousId.size == 0 && previousSource.size == 0)
        return SUMHModulePlanEvent(value.bytes("module_id", SUMH_MODULE_PREVIEW_RESPONSE_BYTES), value.bytes("original_path", SUMH_MODULE_PREVIEW_RESPONSE_BYTES),
            value.bytes("resolved_path", SUMH_MODULE_PREVIEW_RESPONSE_BYTES), value.bytes("source", SUMH_MODULE_PREVIEW_RESPONSE_BYTES), action, value.text("reason"),
            value.bytes("matched_rule_path", SUMH_MODULE_PREVIEW_RESPONSE_BYTES), requested, effective, value.bytes("hidden_parent", SUMH_MODULE_PREVIEW_RESPONSE_BYTES),
            value.boolean("alias"), value.number("mapping_index", 0, 4095), previous, previousId, previousSource, value.boolean("matches_explain"))
    }
    private fun plan(value: JsonObject): SUMHModulePlan {
        value.fields("backend_decisions", "compiled", "complete", "other_backends_unplanned", "sumh_emission_module_order", "error", "events_truncated",
            "observations_truncated", "ordered_contributions", "trace", "explain", "source_observations", "source_revision", "plan_revision", "valid_for_current_inputs")
        val decisions = immutable(value.array("backend_decisions", 512).map { decision(it.objectValue()) })
        require(decisions.map { it.moduleId.hex }.distinct().size == decisions.size)
        val order = value.byteList("sumh_emission_module_order", 512, SUMH_MODULE_PREVIEW_RESPONSE_BYTES)
        require(order == decisions.filter { it.selected == "sumh" }.map { it.moduleId }.reversed())
        val compiled = value.boolean("compiled"); val complete = value.boolean("complete")
        val eventsTruncated = value.boolean("events_truncated"); val observationsTruncated = value.boolean("observations_truncated")
        val sourceRevision = value.revision("source_revision"); val planRevision = value.revision("plan_revision")
        val other = value.boolean("other_backends_unplanned")
        require(other == decisions.any { it.selected == "magic" || it.selected == "overlay" })
        if (complete) require(compiled && !eventsTruncated && !observationsTruncated && decisions.all { it.complete } && sourceRevision != null && planRevision != null)
        val contributions = immutable(value.array("ordered_contributions", 4096).map { contribution(it.objectValue()) })
        require(contributions.withIndex().all { it.index == it.value.index })
        var hides = false
        for (item in contributions) { if (item.action == "HIDE") hides = true else require(!hides) }
        val trace = immutable(value.array("trace", 2048).map { event(it.objectValue()) })
        val explanation = value.obj("explain").also { it.fields("events", "scope", "no_event_meaning", "hidden_ancestors") }
        require(explanation.text("scope") == "observed_compiler_chain_not_runtime_winner" &&
            explanation.text("no_event_meaning") == "no_relevant_compiler_observation_not_proof_path_absent")
        val explainEvents = immutable(explanation.array("events", 2048).map { event(it.objectValue()) })
        require(explainEvents == trace.filter { it.matchesExplain })
        // Failed preparation retains useful trace but deliberately leaves the output plan empty.
        if (complete) for (item in trace.filter { it.action == "ADD" || it.action == "MERGE" }) {
            val mapping = contributions.getOrNull(item.mappingIndex)
            require(mapping != null && mapping.action == item.action && mapping.path == item.resolvedPath && mapping.source == item.source)
        }
        return SUMHModulePlan(decisions, compiled, complete, other, order, value.bytes("error", SUMH_MODULE_PREVIEW_RESPONSE_BYTES, false),
            eventsTruncated, observationsTruncated, contributions, trace, explainEvents,
            explanation.byteList("hidden_ancestors", 4096, SUMH_MODULE_PREVIEW_RESPONSE_BYTES), value.number("source_observations", 0, 4096),
            sourceRevision, planRevision, value.boolean("valid_for_current_inputs"))
    }
    private fun diff(value: JsonObject): SUMHModulePlanDiff {
        value.fields("scope", "complete", "changes")
        require(value.text("scope") == "ordered_sumh_contributions_not_effective_view")
        val changes = immutable(value.array("changes", 8192).map { raw ->
            val change = raw.objectValue().also { it.fields("change", "count", "contribution") }
            val kind = change.text("change", 10).also { require(it == "added" || it == "removed") }
            SUMHModulePlanChange(kind, change.number("count", 1, 4096), contribution(change.obj("contribution")))
        })
        require(changes.map { it.change to it.contribution }.distinct().size == changes.size)
        return SUMHModulePlanDiff(value.boolean("complete"), changes)
    }

    private fun document(text: String, maximum: Int = SUMH_MODULE_PREVIEW_RESPONSE_BYTES, nodeLimit: Int = 262144): JsonObject {
        utf8Size(text, maximum)
        require(!text.startsWith('\uFEFF')) { "module_preview_bom" }
        val reader = JsonReader(StringReader(text)).apply { strictness = Strictness.STRICT }
        var nodes = 0
        fun read(depth: Int): JsonElement {
            require(depth <= 16 && ++nodes <= nodeLimit) { "module_preview_structure_capacity" }
            return when (reader.peek()) {
                JsonToken.BEGIN_OBJECT -> JsonObject().also { obj ->
                    reader.beginObject()
                    while (reader.hasNext()) { val key = reader.nextName(); require(!obj.has(key)) { "module_preview_duplicate_key" }; obj.add(key, read(depth + 1)) }
                    reader.endObject()
                }
                JsonToken.BEGIN_ARRAY -> JsonArray().also { list -> reader.beginArray(); while (reader.hasNext()) list.add(read(depth + 1)); reader.endArray() }
                JsonToken.STRING -> JsonPrimitive(reader.nextString().also { utf8Size(it, maximum) })
                JsonToken.NUMBER -> JsonPrimitive(reader.nextString().let { require(it.length <= 32); it.toBigDecimal() })
                JsonToken.BOOLEAN -> JsonPrimitive(reader.nextBoolean())
                JsonToken.NULL -> { reader.nextNull(); JsonNull.INSTANCE }
                else -> error("module_preview_invalid_json")
            }
        }
        return reader.use { val root = read(0).objectValue(); require(reader.peek() == JsonToken.END_DOCUMENT); root }
    }

    private fun JsonElement.objectValue(): JsonObject { require(isJsonObject); return asJsonObject }
    private fun JsonObject.required(key: String) = get(key) ?: error("module_preview_missing_$key")
    private fun JsonObject.obj(key: String) = required(key).objectValue()
    private fun JsonObject.fields(vararg names: String) { require(keySet() == names.toSet()) { "module_preview_fields" } }
    private fun JsonObject.allowed(names: Set<String>) { require(keySet().all { it in names }) { "module_preview_unknown_field" } }
    private fun JsonObject.text(key: String, maximum: Int = 4096): String = required(key).let {
        require(it.isJsonPrimitive && it.asJsonPrimitive.isString)
        it.asString.also { text -> utf8Size(text, maximum) }
    }
    private fun JsonObject.nullableText(key: String, maximum: Int = 4096) = if (required(key).isJsonNull) null else text(key, maximum)
    private fun JsonObject.boolean(key: String): Boolean = required(key).let { require(it.isJsonPrimitive && it.asJsonPrimitive.isBoolean); it.asBoolean }
    private fun JsonObject.nullableBoolean(key: String) = if (required(key).isJsonNull) null else boolean(key)
    private fun JsonObject.number(key: String, minimum: Int = 0, maximum: Int = 4095): Int = required(key).let {
        require(it.isJsonPrimitive && it.asJsonPrimitive.isNumber)
        requireNotNull(it.toString().toIntOrNull()).also { number -> require(number in minimum..maximum) }
    }
    private fun JsonObject.revision(key: String) = nullableText(key, 74)?.also { require(it.matches(moduleRevisionPattern)) }
    private fun JsonObject.unsigned(key: String): String? = nullableText(key, 20)?.also {
        require(it.matches(Regex("[1-9][0-9]{0,19}")) && it.toULongOrNull() != null)
    }
    private fun JsonObject.array(key: String, maximum: Int = 262144): List<JsonElement> = required(key).let {
        require(it.isJsonArray && it.asJsonArray.size() <= maximum); it.asJsonArray.toList()
    }
    private fun JsonObject.bytes(key: String, maximum: Int = 4095, path: Boolean = true): SUMHInventoryBytes = exact(required(key), maximum, path)
    private fun exact(raw: JsonElement, maximum: Int = 4095, path: Boolean = true): SUMHInventoryBytes {
        val value = raw.objectValue(); value.fields("encoding", "value", "byte_length")
        return SUMHInventoryParser.bytes(value, maximum, path)
    }
    private fun JsonObject.byteList(key: String, maximum: Int, bytes: Int = 4095): List<SUMHInventoryBytes> =
        immutable(array(key, maximum).map { exact(it, bytes) })
    private fun JsonObject.mode(key: String, rule: Boolean = false): String = text(key, 20).also { require(it in if (rule) moduleRuleModes else moduleModes) }
    private fun JsonObject.ids(key: String, maximum: Int): List<String> = immutable(array(key, maximum).map {
        require(it.isJsonPrimitive && it.asJsonPrimitive.isString); it.asString.also { id -> require(validPreviewModuleId(id)); utf8Size(id, SUMH_MODULE_PREVIEW_RESPONSE_BYTES) }
    }.also { require(it.distinct().size == it.size) })
}
