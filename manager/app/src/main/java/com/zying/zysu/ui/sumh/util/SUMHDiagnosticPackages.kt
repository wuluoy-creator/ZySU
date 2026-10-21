package com.zying.zysu.ui.sumh.util

import com.google.gson.JsonElement
import com.google.gson.JsonObject
import com.zying.zysu.Natives
import com.zying.zysu.ui.util.getKsud
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.nio.ByteBuffer
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction
import java.security.MessageDigest
import java.util.Collections

internal const val SUMH_DIAGNOSTIC_PACKAGE_BYTES = 96 * 1024
internal const val SUMH_DIAGNOSTIC_DISPATCH_BYTES = 208 * 1024
internal const val SUMH_DIAGNOSTIC_ENVELOPE_BYTES = 432 * 1024

internal enum class SUMHDiagnosticScenario(val wireName: String, val requiresTarget: Boolean) {
    UserHide("rule-not-effective", true), ModuleFallback("module-fallback", true),
    MapsFailure("maps-failure", false), BootProtection("boot-protection", false),
}
internal data class SUMHDiagnosticPackageDraft(
    val scenario: SUMHDiagnosticScenario = SUMHDiagnosticScenario.BootProtection,
    val targetId: String = "", val operationId: String = "", val includeIdentifiers: Boolean = false,
) {
    val validationError: String? get() = when {
        scenario == SUMHDiagnosticScenario.UserHide && !validDiagnosticPackageId(targetId) -> "diagnostic_definition_id_invalid"
        scenario == SUMHDiagnosticScenario.ModuleFallback && !validDiagnosticModuleId(targetId) -> "diagnostic_module_id_invalid"
        operationId.isNotEmpty() && !validDiagnosticPackageId(operationId) -> "diagnostic_operation_id_invalid"
        else -> if (diagnosticPackageUtf8(diagnosticObject(this).toString()).size > 4096) "diagnostic_request_capacity" else null
    }
}
internal enum class SUMHDiagnosticPackageSupport { Supported, Unsupported, Unknown }
internal data class SUMHDiagnosticPackageCapabilities(
    val create: SUMHDiagnosticPackageSupport = SUMHDiagnosticPackageSupport.Unknown,
    val list: SUMHDiagnosticPackageSupport = SUMHDiagnosticPackageSupport.Unknown,
    val preview: SUMHDiagnosticPackageSupport = SUMHDiagnosticPackageSupport.Unknown,
    val export: SUMHDiagnosticPackageSupport = SUMHDiagnosticPackageSupport.Unknown,
    val remove: SUMHDiagnosticPackageSupport = SUMHDiagnosticPackageSupport.Unknown,
    val error: String? = null,
)
internal data class SUMHDiagnosticPackageItem(
    val id: String, val revision: String, val privacyPolicy: String, val diagnosticStatus: String,
    val scenario: String?, val byteCount: Int,
)
internal data class SUMHDiagnosticPackagePreview(val item: SUMHDiagnosticPackageItem, val exportText: String) {
    fun bytes(): ByteArray = diagnosticPackageUtf8(exportText)
}
internal data class SUMHDiagnosticPackageResult(
    val action: String, val id: String? = null, val status: String = "unknown", val reason: String = "result_unknown",
    val exitCode: Int = 5, val revision: String? = null, val privacyPolicy: String? = null,
    val diagnosticStatus: String? = null, val saved: Boolean = false, val deleted: Boolean = false,
    val durabilityConfirmed: Boolean = false, val readbackConfirmed: Boolean = false,
    val items: List<SUMHDiagnosticPackageItem> = emptyList(), val preview: SUMHDiagnosticPackagePreview? = null,
    val error: String? = null,
) {
    val unknown get() = status == "unknown" || error != null
}
internal fun validDiagnosticPackageId(value: String) = value.length == 32 && value.all { it in '0'..'9' || it in 'a'..'f' }
internal fun validDiagnosticPackageRevision(value: String) = value.length == 74 && value.startsWith("sha256-v1:") &&
    value.substring(10).all { it in '0'..'9' || it in 'a'..'f' }
private fun validDiagnosticModuleId(value: String) = value.isNotEmpty() && value.length <= 4096 && value != "." && value != ".." &&
    value.all { it in 'a'..'z' || it in 'A'..'Z' || it in '0'..'9' || it in "._-" }
internal fun diagnosticPackageUtf8(value: String): ByteArray {
    val buffer = Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
        .onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(value))
    return ByteArray(buffer.remaining()).also(buffer::get)
}
internal fun diagnosticPackageRevision(bytes: ByteArray): String = "sha256-v1:" +
    MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) }
private fun diagnosticObject(input: SUMHDiagnosticPackageDraft) = JsonObject().apply {
    addProperty("schema_version", 1); addProperty("scenario", input.scenario.wireName)
    if (input.scenario == SUMHDiagnosticScenario.UserHide) addProperty("definition_id", input.targetId)
    if (input.scenario == SUMHDiagnosticScenario.ModuleFallback) addProperty("module_id", input.targetId)
    if (input.operationId.isNotEmpty()) addProperty("operation_id", input.operationId)
}

internal interface SUMHDiagnosticPackagesService {
    suspend fun capabilities(): SUMHDiagnosticPackageCapabilities
    suspend fun list(): SUMHDiagnosticPackageResult
    suspend fun create(id: String, draft: SUMHDiagnosticPackageDraft): SUMHDiagnosticPackageResult
    suspend fun preview(id: String): SUMHDiagnosticPackageResult
    suspend fun export(id: String, revision: String): SUMHDiagnosticPackageResult
    suspend fun remove(id: String, revision: String): SUMHDiagnosticPackageResult
}

/** Only the packaged executable's fixed private service is used. No shell or raw bugreport. */
internal class NativeSUMHDiagnosticPackagesService(
    private val transport: suspend (ByteArray) -> ByteArray = { request ->
        withContext(Dispatchers.IO) { Natives.sumhDiagnosticPackages(getKsud().toByteArray(Charsets.UTF_8), request) }
    },
) : SUMHDiagnosticPackagesService {
    override suspend fun capabilities(): SUMHDiagnosticPackageCapabilities = try {
        withContext(Dispatchers.Default) {
            SUMHDiagnosticPackagesParser.capabilities(transport(SUMHDiagnosticPackagesParser.request("capabilities")))
        }
    } catch (cancelled: CancellationException) { throw cancelled }
    catch (_: Exception) { SUMHDiagnosticPackageCapabilities(error = "diagnostic_capabilities_unknown") }
    catch (_: LinkageError) { SUMHDiagnosticPackageCapabilities(error = "diagnostic_transport_unavailable") }

    private suspend fun call(action: String, id: String? = null, revision: String? = null,
        draft: SUMHDiagnosticPackageDraft? = null): SUMHDiagnosticPackageResult = try {
        withContext(Dispatchers.Default) {
            val request = SUMHDiagnosticPackagesParser.request(action, id, revision, draft)
            SUMHDiagnosticPackagesParser.result(transport(request), action, id, revision, draft?.includeIdentifiers)
        }
    } catch (cancelled: CancellationException) {
        if (action != "create" && action != "remove") throw cancelled
        SUMHDiagnosticPackageResult(action, id, error = "diagnostic_result_unknown")
    } catch (_: Exception) { SUMHDiagnosticPackageResult(action, id, error = "diagnostic_result_unknown") }
    catch (_: LinkageError) { SUMHDiagnosticPackageResult(action, id, error = "diagnostic_transport_unavailable") }

    override suspend fun list() = call("list")
    override suspend fun create(id: String, draft: SUMHDiagnosticPackageDraft) = call("create", id, draft = draft)
    override suspend fun preview(id: String) = call("preview", id)
    override suspend fun export(id: String, revision: String) = call("export", id, revision)
    override suspend fun remove(id: String, revision: String) = call("remove", id, revision)
}

internal object SUMHDiagnosticPackagesParser {
    private val scenarios = SUMHDiagnosticScenario.entries.map { it.wireName }.toSet()
    private val sourceStatuses = setOf("observed", "partial", "stale", "unavailable", "invalid")
    private val policies = setOf("redacted", "identifiers_included")
    private val reasons = setOf("storage_unavailable", "invalid_request", "private_directory_unavailable", "store_lock_unavailable",
        "store_unreadable", "id_exists", "package_missing", "store_capacity", "revision_unavailable", "revision_conflict",
        "directory_durability_unconfirmed", "store_recheck_failed", "store_changed", "store_not_replaced", "replaced_durability_unconfirmed",
        "replaced_readback_unconfirmed", "package_saved", "package_deleted", "store_observed", "package_observed", "package_id_conflict",
        "store_preflight_failed", "package_projection_failed", "namespace_unavailable", "enter_init_namespace_failed",
        "namespace_identity_changed", "namespace_changed_query_known_id", "namespace_restore_failed_query_known_id",
        "invalid_arguments", "input_unavailable", "invalid_diagnostic_request", "request_failed_query_known_id")

    fun request(action: String, id: String? = null, revision: String? = null,
        draft: SUMHDiagnosticPackageDraft? = null): ByteArray {
        require(action in setOf("capabilities", "list", "create", "preview", "export", "remove"))
        val value = JsonObject().apply { addProperty("schema_version", 1); addProperty("action", action) }
        if (action in setOf("capabilities", "list")) require(id == null && revision == null && draft == null)
        else { require(validDiagnosticPackageId(requireNotNull(id))); value.addProperty("id", id) }
        if (action in setOf("export", "remove")) { require(validDiagnosticPackageRevision(requireNotNull(revision))); value.addProperty("revision", revision) }
        else require(revision == null)
        if (action == "create") {
            val input = requireNotNull(draft); require(input.validationError == null)
            val diagnostic = diagnosticObject(input)
            require(diagnosticPackageUtf8(diagnostic.toString()).size <= 4096)
            value.addProperty("include_identifiers", input.includeIdentifiers); value.add("diagnostic", diagnostic)
        } else require(draft == null)
        return diagnosticPackageUtf8(value.toString()).also { require(it.size <= 8192) }
    }
    private data class Dispatch(val exit: Int, val value: JsonObject)
    private fun dispatch(raw: ByteArray, action: String, id: String?): Dispatch {
        require(raw.size in 1..SUMH_DIAGNOSTIC_ENVELOPE_BYTES)
        val decoded = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(raw)).toString()
        val envelope = document(decoded, SUMH_DIAGNOSTIC_ENVELOPE_BYTES)
        envelope.fields("ok", "exit_code", "errno", "stdout", "stderr")
        require(envelope.bool("ok") && envelope.int("exit_code", Int.MIN_VALUE, Int.MAX_VALUE) == 0 && envelope.int("errno", 0, 4095) == 0)
        envelope.text("stderr", SUMH_DIAGNOSTIC_DISPATCH_BYTES) // Never display raw transport diagnostics.
        val stdout = envelope.text("stdout", SUMH_DIAGNOSTIC_DISPATCH_BYTES)
        require(stdout.none { it < ' ' })
        val value = document(stdout, SUMH_DIAGNOSTIC_DISPATCH_BYTES)
        value.fields("schema_version", "resource", "action", "id", "exit_code", "result")
        require(value.int("schema_version", 1, 1) == 1 && value.text("resource") == "diagnostic_package_dispatch" && value.text("action") == action)
        require(value.nullableText("id", 32) == id)
        return Dispatch(value.int("exit_code", 0, 255), value.obj("result"))
    }
    fun capabilities(raw: ByteArray): SUMHDiagnosticPackageCapabilities {
        val dispatch = dispatch(raw, "capabilities", null); require(dispatch.exit == 0)
        val value = dispatch.value
        value.fields("schema_version", "kind", "package_schema", "manager_schema", "create", "list", "preview", "export", "remove",
            "default_privacy", "identifiers_opt_in", "package_bytes", "package_count_limit", "bank_bytes", "sampling_read_only", "storage_mutations")
        require(value.int("schema_version", 1, 1) == 1 && value.text("kind") == "sumh.diagnostic.package.capabilities")
        require(value.int("package_schema", 1, 1) == 1 && value.int("manager_schema", 1, 1) == 1)
        require(value.text("default_privacy") == "redacted" && value.bool("identifiers_opt_in") && value.bool("sampling_read_only") && value.bool("storage_mutations"))
        require(value.int("package_bytes", SUMH_DIAGNOSTIC_PACKAGE_BYTES, SUMH_DIAGNOSTIC_PACKAGE_BYTES) == SUMH_DIAGNOSTIC_PACKAGE_BYTES &&
            value.int("package_count_limit", 8, 8) == 8 && value.int("bank_bytes", 1048576, 1048576) == 1048576)
        fun support(key: String) = if (value.bool(key)) SUMHDiagnosticPackageSupport.Supported else SUMHDiagnosticPackageSupport.Unsupported
        return SUMHDiagnosticPackageCapabilities(support("create"), support("list"), support("preview"), support("export"), support("remove"))
    }
    fun result(raw: ByteArray, action: String, id: String? = null, expectedRevision: String? = null,
        includedIdentifiers: Boolean? = null): SUMHDiagnosticPackageResult {
        val dispatch = dispatch(raw, action, id); val v = dispatch.value
        val base = setOf("schema_version", "kind", "action", "id", "status", "reason", "package_revision", "saved", "deleted",
            "durability_confirmed", "readback_confirmed", "active_configuration_changed", "runtime_changed")
        val extras = setOf("privacy_policy", "diagnostic_status", "items", "export_bytes", "export_byte_count")
        require(v.keySet().containsAll(base) && (v.keySet() - base).all { it in extras })
        require(v.int("schema_version", 1, 1) == 1 && v.text("kind") == "sumh.diagnostic.package.result")
        val receiptAction = v.text("action")
        require(receiptAction == action || action in setOf("preview", "export") && receiptAction == "show")
        require(v.nullableText("id", 32) == id)
        val status = v.text("status").also { require(it in setOf("saved", "deleted", "observed", "not_saved", "not_found", "conflict", "unknown", "invalid", "unavailable")) }
        val reason = v.text("reason").also { require(it in reasons) }
        val revision = v.nullableText("package_revision", 74)?.also { require(validDiagnosticPackageRevision(it)) }
        val policy = if (v.has("privacy_policy")) v.nullableText("privacy_policy", 32)?.also { require(it in policies) } else null
        val diagnosticStatus = if (v.has("diagnostic_status")) v.nullableText("diagnostic_status", 32)?.also { require(it in sourceStatuses) } else null
        val saved = v.bool("saved"); val deleted = v.bool("deleted")
        val durable = v.bool("durability_confirmed"); val readback = v.bool("readback_confirmed")
        require(!v.bool("active_configuration_changed") && !v.bool("runtime_changed"))
        val expectedExit = when (status) { "saved", "deleted", "observed" -> 0; "invalid" -> 2; "not_found" -> 3; "conflict" -> 4; else -> 5 }
        require(dispatch.exit == expectedExit)
        require(saved == (status == "saved") && deleted == (status == "deleted"))
        if (saved || deleted) require(durable && readback && revision != null && policy != null && diagnosticStatus != null)
        if (saved) require(action == "create" && includedIdentifiers != null && policy == if (includedIdentifiers) "identifiers_included" else "redacted")
        if (deleted) require(action == "remove" && revision == expectedRevision)
        val items = if (action == "list" && status == "observed") {
            val rows = v.array("items", 8).map { item(it.obj()) }
            require(rows.map { it.id }.distinct().size == rows.size)
            Collections.unmodifiableList(rows)
        } else { require(!v.has("items")); emptyList() }
        val preview = if (action in setOf("preview", "export") && status == "observed") {
            require(id != null && revision != null && policy != null && diagnosticStatus != null)
            if (expectedRevision != null) require(revision == expectedRevision)
            verifiedPreview(requireNotNull(id), requireNotNull(revision), requireNotNull(policy), requireNotNull(diagnosticStatus),
                v.text("export_bytes", SUMH_DIAGNOSTIC_PACKAGE_BYTES), v.int("export_byte_count", 1, SUMH_DIAGNOSTIC_PACKAGE_BYTES))
        } else { require(!v.has("export_bytes") && !v.has("export_byte_count")); null }
        if (status == "observed") require(action in setOf("list", "preview", "export"))
        return SUMHDiagnosticPackageResult(action, id, status, reason, dispatch.exit, revision, policy, diagnosticStatus,
            saved, deleted, durable, readback, items, preview)
    }
    private fun item(value: JsonObject): SUMHDiagnosticPackageItem {
        value.fields("id", "package_revision", "privacy_policy", "diagnostic_status", "scenario", "export_byte_count")
        val id = value.text("id", 32).also { require(validDiagnosticPackageId(it)) }
        val revision = value.text("package_revision", 74).also { require(validDiagnosticPackageRevision(it)) }
        val policy = value.text("privacy_policy").also { require(it in policies) }
        val status = value.text("diagnostic_status").also { require(it in sourceStatuses) }
        val scenario = value.nullableText("scenario", 32)?.also { require(it in scenarios) }
        return SUMHDiagnosticPackageItem(id, revision, policy, status, scenario, value.int("export_byte_count", 1, SUMH_DIAGNOSTIC_PACKAGE_BYTES))
    }
    /** The export string is never reserialized; its exact encoded bytes are the revision input. */
    fun verifiedPreview(id: String, revision: String, policy: String, status: String, text: String, count: Int): SUMHDiagnosticPackagePreview {
        require(validDiagnosticPackageId(id) && validDiagnosticPackageRevision(revision) && policy in policies && status in sourceStatuses)
        val bytes = diagnosticPackageUtf8(text)
        require(bytes.size == count && count in 1..SUMH_DIAGNOSTIC_PACKAGE_BYTES && text.endsWith('\n') && text.dropLast(1).none { it < ' ' })
        require(diagnosticPackageRevision(bytes) == revision)
        val root = document(text, SUMH_DIAGNOSTIC_PACKAGE_BYTES)
        root.fields("schema_version", "kind", "privacy", "diagnostic")
        require(root.int("schema_version", 1, 1) == 1 && root.text("kind") == "sumh.diagnostic.package")
        val privacy = root.obj("privacy"); privacy.fields("policy", "alias_correlation_scope", "observer_verified", "omitted_fields")
        require(privacy.text("policy") == policy && !privacy.bool("observer_verified"))
        require(privacy.text("alias_correlation_scope") == if (policy == "redacted") "within_package_only" else "validated_identifiers")
        val omissions = privacy.array("omitted_fields", 10).map { it.stringValue(64) }
        require(omissions == listOf("paths", "notes", "raw_logs", "credentials", "environment", "command_arguments", "command_descriptions",
            "raw_monotonic_timestamps", "unknown_fields", "alias_reverse_map"))
        val diagnostic = root.obj("diagnostic")
        val required = setOf("schema_version", "resource", "read_only", "atomic", "observer_verified", "mutations_performed", "scenario", "status", "reason",
            "exit_code", "boot_id", "namespace_dev", "namespace_ino", "sample_timing", "target", "evidence", "findings", "next_steps", "recent_operations", "unchecked", "limits")
        require(diagnostic.keySet().containsAll(required) && (diagnostic.keySet() - required).all { it in setOf("errno", "namespace_verified", "namespace_consistent") })
        require(diagnostic.int("schema_version", 1, 1) == 1 && diagnostic.text("resource") == "sumh_diagnostics" && diagnostic.bool("read_only") &&
            !diagnostic.bool("atomic") && !diagnostic.bool("observer_verified") && !diagnostic.bool("mutations_performed"))
        require(diagnostic.text("status") == status && diagnostic.int("exit_code", 0, 5) == when (status) { "observed" -> 0; "partial" -> 3; "stale" -> 4; "invalid" -> 2; else -> 5 })
        val scenario = diagnostic.nullableText("scenario", 32)?.also { require(it in scenarios) }
        diagnostic.array("evidence", 16); diagnostic.array("findings", 32); diagnostic.array("recent_operations", 4)
        diagnostic.array("next_steps", 16).forEach { step ->
            step.obj().fields("code", "kind", "automatic", "evidence_ids")
            require(!step.obj().bool("automatic"))
        }
        // The production store has already checked the complete package schema.
        // Locally enforce its privacy boundary again before displaying the full
        // immutable document; nested fields are not used as executable actions.
        forbidSensitiveFields(diagnostic)
        return SUMHDiagnosticPackagePreview(SUMHDiagnosticPackageItem(id, revision, policy, status, scenario, count), text)
    }
    private fun forbidSensitiveFields(value: JsonElement) {
        if (value.isJsonObject) value.asJsonObject.entrySet().forEach { (key, child) ->
            require(key !in setOf("path", "paths", "target_path", "replacement_path", "description", "argv", "argv_prefix", "arguments",
                "logs", "raw_logs", "credentials", "environment", "notes", "note", "alias_reverse_map", "sample_started_monotonic_ms", "sample_finished_monotonic_ms"))
            forbidSensitiveFields(child)
        } else if (value.isJsonArray) value.asJsonArray.forEach(::forbidSensitiveFields)
    }
    private fun document(text: String, maximum: Int): JsonObject = SUMHControlStateParser.document(text, maximum).also(::validateStrings)
    private fun validateStrings(value: JsonElement) {
        when {
            value.isJsonObject -> value.asJsonObject.entrySet().forEach { (key, child) -> require('\u0000' !in key); diagnosticPackageUtf8(key); validateStrings(child) }
            value.isJsonArray -> value.asJsonArray.forEach(::validateStrings)
            value.isJsonPrimitive && value.asJsonPrimitive.isString -> { require('\u0000' !in value.asString); diagnosticPackageUtf8(value.asString) }
        }
    }
    private fun JsonElement.obj(): JsonObject { require(isJsonObject); return asJsonObject }
    private fun JsonObject.obj(key: String) = required(key).obj()
    private fun JsonObject.fields(vararg keys: String) { require(keySet() == keys.toSet()) }
    private fun JsonObject.required(key: String): JsonElement = requireNotNull(get(key))
    private fun JsonElement.stringValue(max: Int): String {
        require(isJsonPrimitive && asJsonPrimitive.isString)
        return asString.also { require(diagnosticPackageUtf8(it).size <= max && '\u0000' !in it) }
    }
    private fun JsonObject.text(key: String, max: Int = 256) = required(key).stringValue(max)
    private fun JsonObject.nullableText(key: String, max: Int) = required(key).let { if (it.isJsonNull) null else it.stringValue(max) }
    private fun JsonObject.bool(key: String) = required(key).let { require(it.isJsonPrimitive && it.asJsonPrimitive.isBoolean); it.asBoolean }
    private fun JsonObject.int(key: String, min: Int, max: Int) = required(key).let {
        require(it.isJsonPrimitive && it.asJsonPrimitive.isNumber)
        requireNotNull(it.toString().toIntOrNull()).also { v -> require(v in min..max) }
    }
    private fun JsonObject.array(key: String, max: Int): List<JsonElement> = required(key).let {
        require(it.isJsonArray && it.asJsonArray.size() <= max); it.asJsonArray.toList()
    }
}
