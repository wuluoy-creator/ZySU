package com.zying.zysu.ui.sumh.util

import android.app.Application
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHTemporaryMapsOperationModelsTest {
    private val f = SUMHTemporaryMapsTrackingFixtures
    @Test fun canonicalArgumentsAndRealNativeDigestAgreeWithoutUnsignedPrecisionLoss() {
        assertEquals(listOf("sumh", "maps", "remove", "18446744073709551615", "4294967295", "--instance", "18446744073709551615", "--generation", "9007199254740993"), f.args)
        assertEquals(f.revision, f.value("deleted-prepare").getString("request_revision"))
        assertEquals(f.key, temporaryMapsOperationKey(f.request()))
        assertEquals("0", SUMHTemporaryMapsOperationKey("0", "0", "1", "1").targetIno)
        for (invalid in listOf("", "00", "01", "-1", "+1", "1 ", " 1", "1\n", "1e0", "1.0", "18446744073709551616", "中文")) {
            for (index in 0..3) assertFailsWith<IllegalArgumentException> {
                val fields = mutableListOf("1", "1", "1", "1"); fields[index] = invalid
                SUMHTemporaryMapsOperationKey(fields[0], fields[1], fields[2], fields[3])
            }
        }
        assertFailsWith<IllegalArgumentException> { f.key.copy(targetDev = "4294967296") }
        assertFailsWith<IllegalArgumentException> { f.key.copy(instance = "0") }
        assertFailsWith<IllegalArgumentException> { f.key.copy(generation = "0") }
    }
    @Test fun capabilityNeedsEveryExplicitContractAndSeparatesUnknownFromUnsupported() {
        val supported = f.capabilities()
        assertEquals(SUMHTemporaryMapsSupport.Supported, SUMHTemporaryMapsOperationParser.capabilities(f.query(supported)))
        for (field in supported.keys().asSequence().toList()) {
            val missing = JSONObject(supported.toString()); missing.remove(field)
            assertEquals(SUMHTemporaryMapsSupport.Unsupported, SUMHTemporaryMapsOperationParser.capabilities(f.query(missing)), field)
        }
        for (field in listOf("operation_prepare", "operation_result", "maps_temporary_tracked_manager")) {
            assertEquals(SUMHTemporaryMapsSupport.Unsupported, SUMHTemporaryMapsOperationParser.capabilities(f.query(f.capabilities().put(field, false))))
            assertEquals(SUMHTemporaryMapsSupport.Unknown, SUMHTemporaryMapsOperationParser.capabilities(f.query(f.capabilities().put(field, "true"))))
        }
        for (value in listOf(3, "2", JSONObject.NULL))
            assertEquals(SUMHTemporaryMapsSupport.Unknown, SUMHTemporaryMapsOperationParser.capabilities(f.query(f.capabilities().put("operation_manager_temporary_maps_schema", value))))
        assertEquals(SUMHTemporaryMapsSupport.Unknown, SUMHTemporaryMapsOperationParser.capabilities(f.query(supported, "offline")))
        assertEquals(SUMHTemporaryMapsSupport.Unknown, SUMHTemporaryMapsOperationParser.capabilities(SUMHQueryReply("{\"operation_prepare\":true,\"operation_prepare\":false}")))
        assertEquals(SUMHTemporaryMapsSupport.Unknown, SUMHTemporaryMapsOperationParser.capabilities(f.query(f.capabilities().put("operation_resource_result_resources", JSONArray(listOf("temporary_maps_mutation", "temporary_maps_mutation"))))))
    }
    @Test fun actualNativeReceiptsKeepSuccessRejectionAndUnknownDistinct() {
        for ((name, status) in mapOf("deleted" to SUMHTemporaryMapsRemoveStatus.Deleted, "stale" to SUMHTemporaryMapsRemoveStatus.NotApplied,
            "missing" to SUMHTemporaryMapsRemoveStatus.NotApplied, "unsupported" to SUMHTemporaryMapsRemoveStatus.NotApplied,
            "feature-error" to SUMHTemporaryMapsRemoveStatus.NotApplied, "unknown-eintr" to SUMHTemporaryMapsRemoveStatus.Unknown,
            "unknown-eio" to SUMHTemporaryMapsRemoveStatus.Unknown)) {
            val immediate = f.received(name); validateTemporaryMapsTrackedOperation(immediate)
            val result = requireNotNull(immediate.resource()); assertEquals(status, result.status, name)
            assertEquals(name !in setOf("unsupported", "feature-error"), result.attempted)
            val historical = f.recovered(name).recovery()!!
            assertEquals(result, historical.resource); assertEquals("observed", historical.status)
        }
    }
    @Test fun nativeHistoricalReceiptDoesNotBecomeCurrentRuntimeObservation() {
        val entry = f.recovered("historical"); val result = entry.recovery()!!
        assertEquals("boot-b", result.bootId); assertEquals(f.boot, result.record!!.bootId)
        assertEquals(false, result.record.sameBoot); assertEquals(SUMHTemporaryMapsRemoveStatus.Deleted, result.resource!!.status)
        assertEquals(false, entry.history()!!.records.single().sameBoot)
        assertNull(f.recovered("resource-unavailable").recovery()!!.resource)
        assertEquals("resource_unavailable", f.recovered("resource-unavailable").recovery()!!.status)
        assertNull(f.recovered("finish-unknown").recovery()!!.resource)
        assertEquals("not_finished", f.recovered("finish-unknown").recovery()!!.status)
    }
    @Test fun trackedAndLegacyReceiptDiscriminantsCannotBeInterchanged() {
        val legacyRequest = SUMHTemporaryMapsRemoveRequest(SUMHTemporaryMapsToken("18446744073709551615", "9007199254740993", 127),
            f.request().row.copy(targetIno = "0", targetDev = "0"))
        val legacy = requireNotNull(javaClass.getResourceAsStream("/sumh-temporary-maps-native/delete-acknowledged-envelope.json")).use { it.readBytes() }
        val legacyResult = JSONObject(JSONObject(legacy.toString(Charsets.UTF_8)).getString("stdout")).getJSONObject("result")
        assertFailsWith<IllegalArgumentException> { SUMHTemporaryMapsOperationParser.receipt(legacyResult.toString(), f.entry(), 0, 0) }
        assertFailsWith<IllegalArgumentException> { SUMHTemporaryMapsParser.remove(f.bytes("deleted-execute"), legacyRequest) }
    }
    @Test fun receiptRejectsEachChangedSelectorStatusAndOperationBinding() {
        val raw = JSONObject(f.value("deleted-execute").getJSONObject("stdout").getString("value"))
        val mutations: List<(JSONObject) -> Unit> = listOf(
            { it.put("operation_id", "ff".repeat(16)) }, { it.put("resource", "temporary_maps") }, { it.put("action", "clear") },
            { it.put("schema_version", 2) }, { it.put("saved", true) }, { it.put("observer_verified", true) },
            { it.put("history_supported", false) }, { it.put("acknowledged", false) }, { it.put("ioctl_attempted", false) },
            { it.put("errno", 5) }, { it.put("status", "not_applied") }, { it.put("error_code", "outcome_unknown") },
            { it.put("extra", false) }, { it.getJSONObject("request").put("target_ino", "1") },
            { it.getJSONObject("request").put("target_dev", "0") }, { it.getJSONObject("request").put("expected_instance_id", "1") },
            { it.getJSONObject("request").put("expected_generation", "1") })
        mutations.forEach { mutate -> val bad = JSONObject(raw.toString()); mutate(bad)
            assertFailsWith<IllegalArgumentException> { SUMHTemporaryMapsOperationParser.receipt(bad.toString(), f.entry(), 0, 0) } }
        assertFailsWith<IllegalArgumentException> { SUMHTemporaryMapsOperationParser.receipt(raw.toString(), f.entry().copy(requestRevision = "sha256-v1:" + "f".repeat(64)), 0, 0) }
        assertFailsWith<IllegalArgumentException> { SUMHTemporaryMapsOperationParser.receipt(raw.toString(), f.entry(), 1, 0) }
    }
    @Test fun retainedResultRejectsChangedIdentityEvenWhenNestedReceiptLooksSuccessful() {
        val changes: List<(JSONObject) -> Unit> = listOf(
            { it.put("requested_id", "ff".repeat(16)) }, { it.put("origin_boot_id", "other") }, { it.put("same_boot", false) },
            { it.put("request_revision", "sha256-v1:" + "f".repeat(64)) }, { it.getJSONObject("record").put("name", "hide disable") },
            { it.getJSONObject("record").put("boot_id", "other") }, { it.getJSONObject("record").put("id", "ff".repeat(16)) },
            { it.getJSONObject("record").put("errno", 5) }, { it.put("read_only", false) }, { it.put("runtime_verified", true) },
            { it.getJSONObject("result").getJSONObject("request").put("expected_generation", "1") })
        changes.forEach { mutate -> val value = f.value("deleted-result"); mutate(value)
            assertFailsWith<IllegalArgumentException> { SUMHTemporaryMapsOperationParser.resourceResult(f.query(value), f.entry()) } }
    }
    @Test fun commandSummaryAndTruncatedOutputCannotBecomeDeleted() {
        for (mutate in listOf<(JSONObject) -> Unit>(
            { it.getJSONObject("stdout").put("complete", false) }, { it.getJSONObject("stderr").put("complete", false) },
            { it.getJSONObject("stdout").put("value", "").put("byte_length", 0) })) {
            val value = f.value("deleted-execute"); mutate(value)
            val entry = f.entry("received").copy(response = value.toString())
            assertFails { validateTemporaryMapsTrackedOperation(entry) }
        }
        val valid = f.value("deleted-result").toString()
        for (cut in listOf(0, 1, valid.length / 2, valid.length - 1))
            assertFails { SUMHTemporaryMapsOperationParser.resourceResult(SUMHQueryReply(valid.take(cut)), f.entry()) }
        assertFails { SUMHTemporaryMapsOperationParser.resourceResult(SUMHQueryReply(valid + "{}"), f.entry()) }
        assertFails { SUMHTemporaryMapsOperationParser.resourceResult(SUMHQueryReply(valid.replace("\"error\":\"\"", "\"error\":\"\\ud800\"")), f.entry()) }
    }
}
