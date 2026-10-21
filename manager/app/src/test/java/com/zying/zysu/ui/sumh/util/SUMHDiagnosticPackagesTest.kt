package com.zying.zysu.ui.sumh.util

import com.google.gson.JsonArray
import com.google.gson.JsonNull
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import kotlinx.coroutines.runBlocking
import org.junit.Test
import kotlin.test.*

internal object DiagnosticPackageFixtures {
    val id = "a".repeat(32)
    val other = "b".repeat(32)
    fun packageText(policy: String = "redacted", status: String = "unavailable", scenario: String? = "boot-protection"): String {
        val diagnostic = JsonObject().apply {
            addProperty("schema_version", 1); addProperty("resource", "sumh_diagnostics"); addProperty("read_only", true)
            listOf("atomic", "observer_verified", "mutations_performed").forEach { addProperty(it, false) }
            addProperty("scenario", scenario); addProperty("status", status); addProperty("reason", "transport_unavailable")
            addProperty("exit_code", when (status) { "observed" -> 0; "partial" -> 3; "stale" -> 4; "invalid" -> 2; else -> 5 })
            listOf("boot_id", "namespace_dev", "namespace_ino").forEach { add(it, JsonNull.INSTANCE) }
            add("sample_timing", JsonObject().apply { addProperty("status", "omitted"); add("start_offset_ms", JsonNull.INSTANCE); add("duration_ms", JsonNull.INSTANCE) })
            add("target", JsonObject()); listOf("evidence", "findings", "next_steps", "recent_operations", "unchecked").forEach { add(it, JsonArray()) }
            add("limits", JsonObject().apply {
                mapOf("input_bytes" to 4096, "response_bytes" to 65536, "evidence" to 16, "findings" to 32, "next_steps" to 16, "recent_operations" to 4, "maps_row_summaries" to 16).forEach { (k, v) -> addProperty(k, v) }
            })
        }
        return JsonObject().apply {
            addProperty("schema_version", 1); addProperty("kind", "sumh.diagnostic.package")
            add("privacy", JsonObject().apply {
                addProperty("policy", policy); addProperty("alias_correlation_scope", if (policy == "redacted") "within_package_only" else "validated_identifiers")
                addProperty("observer_verified", false)
                add("omitted_fields", JsonArray().apply { listOf("paths", "notes", "raw_logs", "credentials", "environment", "command_arguments", "command_descriptions",
                    "raw_monotonic_timestamps", "unknown_fields", "alias_reverse_map").forEach(::add) })
            }); add("diagnostic", diagnostic)
        }.toString() + "\n"
    }
    fun preview(text: String = packageText(), id: String = this.id): SUMHDiagnosticPackagePreview {
        val root = JsonParser.parseString(text).asJsonObject
        return SUMHDiagnosticPackagesParser.verifiedPreview(id, diagnosticPackageRevision(text.toByteArray()),
            root["privacy"].asJsonObject["policy"].asString, root["diagnostic"].asJsonObject["status"].asString, text, text.toByteArray().size)
    }
    fun receipt(action: String, id: String? = this.id, status: String = "observed", p: SUMHDiagnosticPackagePreview = preview()): JsonObject = JsonObject().apply {
        addProperty("schema_version", 1); addProperty("kind", "sumh.diagnostic.package.result"); addProperty("action", action); addProperty("id", id)
        addProperty("status", status); addProperty("reason", when (status) { "saved" -> "package_saved"; "deleted" -> "package_deleted"; "not_found" -> "package_missing"; else -> "package_observed" })
        addProperty("package_revision", if (status == "not_found") null else p.item.revision)
        addProperty("privacy_policy", if (status == "not_found") null else p.item.privacyPolicy); addProperty("diagnostic_status", if (status == "not_found") null else p.item.diagnosticStatus)
        addProperty("saved", status == "saved"); addProperty("deleted", status == "deleted")
        addProperty("durability_confirmed", status in setOf("saved", "deleted")); addProperty("readback_confirmed", status in setOf("saved", "deleted"))
        addProperty("active_configuration_changed", false); addProperty("runtime_changed", false)
        if (status == "observed" && action == "show") { addProperty("export_bytes", p.exportText); addProperty("export_byte_count", p.item.byteCount) }
    }
    fun dispatch(action: String, id: String?, value: JsonObject, code: Int = 0): ByteArray = JsonObject().apply {
        addProperty("ok", true); addProperty("exit_code", 0); addProperty("errno", 0); addProperty("stderr", "")
        addProperty("stdout", JsonObject().apply {
            addProperty("schema_version", 1); addProperty("resource", "diagnostic_package_dispatch"); addProperty("action", action)
            addProperty("id", id); addProperty("exit_code", code); add("result", value)
        }.toString())
    }.toString().toByteArray()
    fun capabilities(supported: Boolean = true): ByteArray = dispatch("capabilities", null, JsonObject().apply {
        addProperty("schema_version", 1); addProperty("kind", "sumh.diagnostic.package.capabilities"); addProperty("package_schema", 1); addProperty("manager_schema", 1)
        listOf("create", "list", "preview", "export", "remove").forEach { addProperty(it, supported) }
        addProperty("default_privacy", "redacted"); addProperty("identifiers_opt_in", true); addProperty("package_bytes", 98304)
        addProperty("package_count_limit", 8); addProperty("bank_bytes", 1048576); addProperty("sampling_read_only", true); addProperty("storage_mutations", true)
    })
}

class SUMHDiagnosticPackagesTest {
    private val fixture = DiagnosticPackageFixtures
    @Test fun requestHasFixedGrammarAndDefaultPrivacyForEveryScenario() {
        for (scenario in SUMHDiagnosticScenario.entries) {
            val draft = SUMHDiagnosticPackageDraft(scenario, if (scenario == SUMHDiagnosticScenario.ModuleFallback) "example.module" else fixture.id)
            val request = JsonParser.parseString(SUMHDiagnosticPackagesParser.request("create", fixture.id, draft = draft).toString(Charsets.UTF_8)).asJsonObject
            assertFalse(request["include_identifiers"].asBoolean)
            assertEquals(scenario.wireName, request["diagnostic"].asJsonObject["scenario"].asString)
            assertFalse(request["diagnostic"].asJsonObject.has("operation_id"))
        }
        assertNotNull(SUMHDiagnosticPackageDraft(SUMHDiagnosticScenario.UserHide, " ../bad ").validationError)
        assertNotNull(SUMHDiagnosticPackageDraft(SUMHDiagnosticScenario.ModuleFallback, "路径").validationError)
        assertNotNull(SUMHDiagnosticPackageDraft(operationId = fixture.id + " ").validationError)
        assertNotNull(SUMHDiagnosticPackageDraft(SUMHDiagnosticScenario.ModuleFallback, "a".repeat(4096)).validationError)
        assertFails { SUMHDiagnosticPackagesParser.request("remove", fixture.id) }
        assertFails { SUMHDiagnosticPackagesParser.request("list", fixture.id) }
    }
    @Test fun explicitCapabilitiesDistinguishUnknownFromUnsupported() = runBlocking {
        assertEquals(SUMHDiagnosticPackageSupport.Supported, SUMHDiagnosticPackagesParser.capabilities(fixture.capabilities()).create)
        assertEquals(SUMHDiagnosticPackageSupport.Unsupported, SUMHDiagnosticPackagesParser.capabilities(fixture.capabilities(false)).export)
        val service = NativeSUMHDiagnosticPackagesService { throw IllegalStateException("CANARY_SECRET") }
        val unknown = service.capabilities()
        assertEquals(SUMHDiagnosticPackageSupport.Unknown, unknown.create)
        assertEquals("diagnostic_capabilities_unknown", unknown.error)
    }
    @Test fun exactExportBytesDigestAndMetadataAreBoundToRequestedId() {
        for (status in listOf("observed", "partial", "stale", "unavailable", "invalid")) {
            val p = fixture.preview(fixture.packageText(status = status))
            val raw = fixture.dispatch("export", fixture.id, fixture.receipt("show", p = p))
            val result = SUMHDiagnosticPackagesParser.result(raw, "export", fixture.id, p.item.revision)
            assertEquals(p, result.preview); assertEquals(status, result.diagnosticStatus)
            assertContentEquals(p.exportText.toByteArray(), result.preview!!.bytes())
        }
        val p = fixture.preview()
        val raw = fixture.dispatch("preview", fixture.id, fixture.receipt("show"))
        assertFails { SUMHDiagnosticPackagesParser.result(raw, "preview", fixture.other) }
        assertFails { SUMHDiagnosticPackagesParser.result(raw, "export", fixture.id, p.item.revision) }
        assertFails { SUMHDiagnosticPackagesParser.result(fixture.dispatch("export", fixture.id, fixture.receipt("show")), "export", fixture.id, "sha256-v1:" + "f".repeat(64)) }
    }
    @Test fun reserializationNewlineTruncationAndChangedMetadataFail() {
        val p = fixture.preview()
        val changes: List<(JsonObject) -> Unit> = listOf(
            { it.addProperty("export_bytes", p.exportText.dropLast(1)) },
            { it.addProperty("export_bytes", p.exportText + " ") },
            { it.addProperty("export_byte_count", p.item.byteCount - 1) },
            { it.addProperty("diagnostic_status", "observed") },
            { it.addProperty("privacy_policy", "identifiers_included") },
            { it.addProperty("unexpected", "CANARY") },
            { it.addProperty("package_revision", "sha256-v1:" + "0".repeat(64)) },
        )
        for (change in changes) {
            val r = fixture.receipt("show"); change(r)
            assertFails { SUMHDiagnosticPackagesParser.result(fixture.dispatch("preview", fixture.id, r), "preview", fixture.id) }
        }
    }
    @Test fun storedFutureDuplicateAndSensitiveFieldsFailDespiteMatchingDigest() {
        val p = fixture.preview()
        val changed = JsonParser.parseString(p.exportText).asJsonObject
        changed.addProperty("schema_version", 2)
        fun invalid(text: String) = assertFails { SUMHDiagnosticPackagesParser.verifiedPreview(fixture.id, diagnosticPackageRevision(text.toByteArray()), "redacted", "unavailable", text, text.toByteArray().size) }
        invalid(changed.toString() + "\n")
        invalid(p.exportText.replace("\"schema_version\":1", "\"schema_version\":1,\"schema_version\":1"))
        changed.addProperty("schema_version", 1)
        changed["diagnostic"].asJsonObject.addProperty("argv", "CANARY")
        invalid(changed.toString() + "\n")
        val malformed = byteArrayOf(0x7b, 0xff.toByte(), 0x7d)
        assertFails { SUMHDiagnosticPackagesParser.result(malformed, "list") }
        assertFails { SUMHDiagnosticPackagesParser.result(ByteArray(SUMH_DIAGNOSTIC_ENVELOPE_BYTES + 1), "list") }
        invalid(p.exportText.replace("transport_unavailable", "\\ud800"))
    }
    @Test fun successRequiresDurabilityAndReadbackWhileOptInRemainsExplicit() {
        val r = fixture.receipt("create", status = "saved")
        val raw = fixture.dispatch("create", fixture.id, r)
        assertTrue(SUMHDiagnosticPackagesParser.result(raw, "create", fixture.id, includedIdentifiers = false).saved)
        assertFails { SUMHDiagnosticPackagesParser.result(raw, "create", fixture.id, includedIdentifiers = true) }
        r.addProperty("readback_confirmed", false)
        assertFails { SUMHDiagnosticPackagesParser.result(fixture.dispatch("create", fixture.id, r), "create", fixture.id, includedIdentifiers = false) }
        val opted = fixture.preview(fixture.packageText("identifiers_included"))
        assertEquals("identifiers_included", SUMHDiagnosticPackagesParser.result(fixture.dispatch("preview", fixture.id, fixture.receipt("show", p = opted)), "preview", fixture.id).privacyPolicy)
    }
    @Test fun failuresNeverReturnRawTransportOrParserText() = runBlocking {
        val service = NativeSUMHDiagnosticPackagesService { "CANARY_凭据".toByteArray() }
        val reply = service.create(fixture.id, SUMHDiagnosticPackageDraft())
        assertTrue(reply.unknown); assertEquals("diagnostic_result_unknown", reply.error); assertNull(reply.preview)
        assertFalse(reply.toString().contains("CANARY"))
    }
}
