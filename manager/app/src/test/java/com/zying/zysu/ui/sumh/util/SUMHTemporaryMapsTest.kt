package com.zying.zysu.ui.sumh.util

import android.app.Application
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHTemporaryMapsTest {
    private val f = SUMHTemporaryMapsFixtures
    private val p = SUMHTemporaryMapsParser
    private fun caps(value: JSONObject, exit: Int = 0) = p.capabilities(f.wire("capabilities", value, exit))
    private fun parse(value: JSONObject, exit: Int = 0, cursor: SUMHTemporaryMapsCursor? = null) = p.page(f.wire("list", value, exit), cursor)
    private fun remove(value: JSONObject = f.receipt(), exit: Int = 0) = p.remove(f.wire("remove", value, exit), f.request)

    @Test fun independentCapabilitiesAllowInventoryWithoutDeletion() {
        assertEquals(SUMHTemporaryMapsSupport.Supported, caps(f.capabilities()).delete)
        val inventoryOnly = caps(f.capabilities(delete = "unsupported"))
        assertEquals(SUMHTemporaryMapsSupport.Supported, inventoryOnly.inventory)
        assertEquals(SUMHTemporaryMapsSupport.Unsupported, inventoryOnly.delete)
        val neither = caps(f.capabilities("unsupported", "unsupported"))
        assertEquals(SUMHTemporaryMapsSupport.Unsupported, neither.inventory)
        assertEquals(SUMHTemporaryMapsSupport.Unsupported, neither.delete)
        assertFails { caps(f.capabilities("unsupported", "supported")) }
    }

    @Test fun unknownCapabilitiesRequireFailedSampleAndDoNotMeanUnsupported() {
        val unknown = caps(f.capabilities("unknown", "unknown", 5), 1)
        assertEquals(SUMHTemporaryMapsSupport.Unknown, unknown.inventory)
        assertEquals(SUMHTemporaryMapsSupport.Unknown, unknown.delete)
        assertEquals(5, unknown.errno)
        for (value in listOf(f.capabilities("unknown", "supported", 5), f.capabilities("supported", "unknown", 5),
            f.capabilities("unknown", "unknown", 0), f.capabilities(errno = 5))) assertFails { caps(value, 1) }
        assertFails { caps(f.capabilities("unknown", "unknown", 5), 0) }
        assertFails { caps(f.capabilities(), 1) }
    }

    @Test fun absentFutureAndMalformedCapabilitiesFailClosed() = runBlocking {
        for (value in listOf(JSONObject(), f.capabilities().put("schema_version", 2), f.capabilities().put("schema_version", "1"),
            f.capabilities().put("delete", true), f.capabilities().put("inventory", "ready"),
            f.capabilities().put("errno", -1), f.capabilities().put("errno", 4096), f.capabilities().put("extra", true))) {
            val result = NativeSUMHTemporaryMapsService { f.wire("capabilities", value) }.capabilities()
            assertEquals(SUMHTemporaryMapsSupport.Unknown, result.inventory)
            assertEquals(SUMHTemporaryMapsSupport.Unknown, result.delete)
            assertNotNull(result.error)
        }
    }

    @Test fun requestsKeepMaximumUnsignedTokensAndZeroExactKeys() {
        val token = SUMHTemporaryMapsToken(ULong.MAX_VALUE.toString(), ULong.MAX_VALUE.toString(), 127)
        val row = f.row.copy(targetIno = "0", targetDev = "0")
        val remove = JSONObject(p.removeRequest(SUMHTemporaryMapsRemoveRequest(token, row)).toString(Charsets.UTF_8))
        assertEquals("0", remove.getString("target_ino")); assertEquals("0", remove.getString("target_dev"))
        assertEquals(ULong.MAX_VALUE.toString(), remove.getString("instance_id"))
        assertEquals(ULong.MAX_VALUE.toString(), remove.getString("generation"))
        assertEquals(setOf("schema_version", "action", "instance_id", "generation", "target_ino", "target_dev"), remove.keys().asSequence().toSet())
        val first = JSONObject(p.listRequest().toString(Charsets.UTF_8))
        assertEquals(setOf("schema_version", "action"), first.keys().asSequence().toSet())
        val next = JSONObject(p.listRequest(SUMHTemporaryMapsCursor(token, "18446744073709551615")).toString(Charsets.UTF_8))
        assertEquals("18446744073709551615", next.getString("cursor"))
        assertEquals("capabilities", JSONObject(p.capabilitiesRequest().toString(Charsets.UTF_8)).getString("action"))
    }

    @Test fun noncanonicalOverflowAndZeroTokensCannotReachTransport() {
        for (value in listOf("", "00", "01", "-1", "+1", " 1", "1 ", "1\n", "1.0", "1e1", "18446744073709551616")) {
            assertFails { SUMHTemporaryMapsToken(value, "1", 127) }
            assertFails { SUMHTemporaryMapsCursor(f.token, value) }
            assertFails { f.row.copy(targetIno = value) }
        }
        assertFails { SUMHTemporaryMapsToken("0", "1", 127) }
        assertFails { SUMHTemporaryMapsToken("1", "0", 127) }
        assertFails { SUMHTemporaryMapsToken("1", "1", 0) }
        assertFails { f.row.copy(targetDev = "4294967296") }
        assertFails { f.row.copy(spoofedDev = "4294967296") }
    }

    @Test fun knownLocalPageNeedsNoBootSampleAndPreservesU64() {
        val page = parse(f.page(entries = listOf(f.entry(ino = "18446744073709551615", dev = "4294967295"))))
        assertNull(page.page!!.bootId)
        val snapshot = appendSUMHTemporaryMapsPage(null, page)
        assertEquals("18446744073709551615", snapshot.rows.single().targetIno)
        assertEquals("4294967295", snapshot.rows.single().targetDev)
        assertEquals("18446744073709551615", snapshot.rows.single().spoofedIno)
        assertTrue(snapshot.complete); assertNull(snapshot.continuation)
        assertEquals(f.token, snapshot.token)
    }

    @Test fun queryNeverInventsSourcePathAndKeepsInvalidUtf8AsHex() {
        val bytes = JSONObject().put("encoding", "hex").put("value", "2fff80").put("byte_length", 3)
        val snapshot = appendSUMHTemporaryMapsPage(null, parse(f.page(entries = listOf(f.entry().put("spoofed_path", bytes)))))
        assertEquals("2fff80", snapshot.rows.single().spoofedPath.hex)
        assertNull(snapshot.rows.single().spoofedPath.utf8)
        assertEquals(setOf("spoofed_path"), parse(f.page()).page!!.entries.single().paths.keys)
    }

    @Test fun unicodeNewlineAndTrailingSpacePathRemainByteExact() {
        val path = "/名字\nend "
        val item = f.entry().put("spoofed_path", SUMHInventoryFixtures.bytes(path))
        val snapshot = appendSUMHTemporaryMapsPage(null, parse(f.page(entries = listOf(item))))
        assertEquals(path, snapshot.rows.single().spoofedPath.utf8)
        assertEquals(path.toByteArray().size, snapshot.rows.single().spoofedPath.size)
    }

    @Test fun nulBadEncodingWrongOwnerCategoryAndExtraFieldsFail() {
        val entries = listOf(f.entry().put("owner", "managed_maps"), f.entry().put("category", "maps_managed"),
            f.entry().put("mutation_handle", true), f.entry().put("id_kind", "managed_definition"),
            f.entry().put("definition_generation", "1"), f.entry().put("target_dev", "4294967296"),
            f.entry().put("spoofed_dev", "4294967296"), f.entry().put("target_ino", 1),
            f.entry().put("spoofed_path", SUMHInventoryFixtures.bytes("/a\u0000")),
            f.entry().put("spoofed_path", SUMHInventoryFixtures.bytes("/a").put("byte_length", 9)),
            f.entry().put("source_path", "/invented"))
        for (entry in entries) assertFails { parse(f.page(entries = listOf(entry))) }
        assertFails { parse(f.page().put("boot_id", "boot-a")) }
        assertFails { parse(f.page().put("legacy", SUMHInventoryFixtures.bytes("legacy"))) }
        assertFails { parse(f.page().put("schema_version", 2)) }
        assertFails { parse(f.page().put("extra", true)) }
        assertFails { parse(f.page().put("reason", "invalid\u0000reason")) }
        // Keep the invalid scalar as an ASCII JSON escape through both JSON layers.
        // Default String.toByteArray would replace a literal unpaired surrogate with '?'.
        fun escapedSurrogateWire(page: JSONObject): ByteArray {
            val dispatch = f.dispatch("list", page)
            val inner = dispatch.toString().replace("SURROGATE_ESCAPE", "\\ud800")
            assertTrue(inner.contains("\\ud800"))
            return f.envelope(dispatch).put("stdout", inner).toString().toByteArray(Charsets.UTF_8)
        }
        val invalidPath = SUMHInventoryFixtures.bytes("/a").put("value", "/SURROGATE_ESCAPE")
        assertFails { p.page(escapedSurrogateWire(f.page(entries = listOf(f.entry().put("spoofed_path", invalidPath))))) }
        assertFails { p.page(escapedSurrogateWire(f.page().put("reason", "invalidSURROGATE_ESCAPEreason"))) }
    }

    @Test fun firstAndContinuationPagesRequireExactCursorTokenMask() {
        assertFails { parse(f.page(cursor = 1)) }
        val cursor = SUMHTemporaryMapsCursor(f.token, "1")
        assertEquals("1", parse(f.page(cursor = 1), cursor = cursor).page!!.cursor)
        assertFails { parse(f.page(cursor = 2), cursor = cursor) }
        for ((key, value) in listOf("instance_id" to "24", "generation" to "10", "supported_categories" to 255)) {
            val page = f.page(cursor = 1)
            page.getJSONObject("page").put(key, value)
            assertFails { parse(page, cursor = cursor) }
        }
        val page = f.page(); page.getJSONObject("page").put("next_cursor", "2")
        assertFails { parse(page) }
    }

    @Test fun unsupportedUnavailableAndStaleAreExplicitNotEmptySuccess() {
        for ((status, errno, reason) in listOf(Triple("unsupported", 0, "rule_inventory_not_advertised"),
            Triple("unsupported", 95, "category_not_supported"), Triple("unavailable", 93, "protocol_mismatch"),
            Triple("error", 5, "inventory_query_failed"), Triple("stale", 116, "snapshot_changed"))) {
            val result = parse(f.failure(status, errno, reason), if (errno == 0) 0 else 1)
            assertNotEquals(SUMHInventoryPageStatus.Known, result.page!!.status)
            assertFails { appendSUMHTemporaryMapsPage(null, result) }
        }
        assertFails { parse(f.failure(), 0) }
        assertFails { parse(f.page(), 1) }
    }

    @Test fun manualPartialAppendCompletesOnlyAtEndAndDoesNotRestart() {
        val first = appendSUMHTemporaryMapsPage(null, f.pageResult(end = false))
        assertFalse(first.complete); assertEquals("1", first.continuation!!.cursor)
        val next = appendSUMHTemporaryMapsPage(first, f.pageResult(cursor = 1))
        assertTrue(next.complete); assertEquals(listOf("1", "2"), next.rows.map { it.ordinal })
        assertNull(next.continuation)
        assertFails { appendSUMHTemporaryMapsPage(next, f.pageResult(cursor = 2)) }
        assertFails { appendSUMHTemporaryMapsPage(first, f.pageResult()) }
    }

    @Test fun emptyCompleteDiffersFromPartialAndError() {
        val empty = appendSUMHTemporaryMapsPage(null, f.pageResult(entries = emptyList()))
        assertTrue(empty.complete); assertTrue(empty.rows.isEmpty())
        assertFails { parse(f.page(entries = emptyList(), end = false)) }
        val prior = appendSUMHTemporaryMapsPage(null, f.pageResult(end = false))
        assertFails { appendSUMHTemporaryMapsPage(prior, SUMHTemporaryMapsPageResult(error = "lost reply")) }
        assertEquals(1, prior.rows.size); assertFalse(prior.complete)
    }

    @Test fun exactDuplicateKeysAreRejectedButWildcardAndExactCoexist() {
        val first = appendSUMHTemporaryMapsPage(null, f.pageResult(entries = listOf(f.entry(ino = "7", dev = "0")), end = false))
        assertFails { appendSUMHTemporaryMapsPage(first, f.pageResult(1, listOf(f.entry(2, "7", "0")))) }
        val exact = appendSUMHTemporaryMapsPage(first, f.pageResult(1, listOf(f.entry(2, "7", "1"))))
        assertEquals(listOf("7:0", "7:1"), exact.rows.map { it.key })
        assertFails { appendSUMHTemporaryMapsPage(first, f.pageResult(1, listOf(f.entry(1, "8", "0")))) }
    }

    @Test fun appendRejectsMixedTokensEvenWhenResultWasParsedIndependently() {
        val first = appendSUMHTemporaryMapsPage(null, f.pageResult(end = false))
        val next = f.pageResult(cursor = 1)
        assertFails { appendSUMHTemporaryMapsPage(first, next.copy(page = next.page!!.copy(instance = "24"))) }
        assertFails { appendSUMHTemporaryMapsPage(first, next.copy(page = next.page!!.copy(generation = "10"))) }
        assertFails { appendSUMHTemporaryMapsPage(first, next.copy(page = next.page!!.copy(mask = 255))) }
    }

    @Test fun collectionRecordBudgetAccepts1024AndRejects1025WithoutLoss() {
        val base = f.pageResult(end = false)
        var snapshot: SUMHTemporaryMapsSnapshot? = null
        repeat(1024) { index ->
            val entry = base.page!!.entries.single().copy(id = (index + 1).toString(),
                numbers = base.page.entries.single().numbers + ("target_ino" to index.toString()))
            val page = base.page.copy(cursor = index.toString(), nextCursor = (index + 1).toString(), entries = listOf(entry))
            snapshot = appendSUMHTemporaryMapsPage(snapshot, base.copy(page = page, responseBytes = 1))
        }
        assertEquals(1024, snapshot!!.rows.size)
        val entry = base.page!!.entries.single().copy(id = "1025", numbers = base.page.entries.single().numbers + ("target_ino" to "1025"))
        val overflow = base.copy(page = base.page.copy(cursor = "1024", nextCursor = "1025", entries = listOf(entry)))
        assertFails { appendSUMHTemporaryMapsPage(snapshot, overflow) }
        assertEquals(1024, snapshot!!.rows.size)
    }

    @Test fun collectionByteBudgetIsInclusiveAndNeverWraps() {
        val first = appendSUMHTemporaryMapsPage(null, f.pageResult(end = false))
        val next = f.pageResult(cursor = 1)
        val full = first.copy(responseBytes = SUMH_TEMPORARY_MAPS_COLLECTION_BYTES - next.responseBytes)
        assertEquals(SUMH_TEMPORARY_MAPS_COLLECTION_BYTES, appendSUMHTemporaryMapsPage(full, next).responseBytes)
        assertFails { appendSUMHTemporaryMapsPage(full.copy(responseBytes = full.responseBytes + 1), next) }
        assertFails { appendSUMHTemporaryMapsPage(first.copy(responseBytes = Int.MAX_VALUE), next) }
        assertFails { appendSUMHTemporaryMapsPage(null, next.copy(responseBytes = 0)) }
        assertFails { appendSUMHTemporaryMapsPage(null, next.copy(responseBytes = SUMH_TEMPORARY_MAPS_ENVELOPE_BYTES + 1)) }
    }

    @Test fun acknowledgedDeleteHasExactUnsignedRequestEcho() {
        val result = remove()
        assertEquals(SUMHTemporaryMapsRemoveStatus.Deleted, result.status)
        assertEquals(true, result.attempted); assertTrue(result.acknowledged); assertEquals(0, result.errno)
        for (key in listOf("target_ino", "target_dev", "expected_instance_id", "expected_generation")) {
            val value = f.receipt(); value.getJSONObject("request").put(key, "1")
            assertFails { remove(value) }
        }
        assertFails { remove(f.receipt().put("request", JSONObject.NULL)) }
        assertFails { remove(f.receipt().put("request", JSONObject(f.receipt().getJSONObject("request").toString()).put("extra", "x"))) }
    }

    @Test fun documentedPrepublicationRejectionsAreNeverReportedAsDeletion() {
        val errors = mapOf(116 to "snapshot_changed", 2 to "temporary_key_not_found", 22 to "invalid_request", 75 to "key_not_representable",
            12 to "insufficient_memory", 108 to "runtime_stopped", 14 to "invalid_user_memory", 95 to "unsupported", 25 to "unsupported",
            13 to "permission_denied", 1 to "permission_denied")
        for ((errno, code) in errors) {
            val result = remove(f.receipt("not_applied", errno, code = code), 1)
            assertEquals(SUMHTemporaryMapsRemoveStatus.NotApplied, result.status)
            assertFalse(result.acknowledged)
            assertFails { remove(f.receipt("outcome_unknown", errno, code = code), 1) }
        }
        assertEquals(SUMHTemporaryMapsRemoveStatus.NotApplied,
            remove(f.receipt("not_applied", 5, attempted = false, code = "capability_query_failed"), 1).status)
        assertEquals(SUMHTemporaryMapsRemoveStatus.NotApplied,
            remove(f.receipt("not_applied", 2, attempted = false, code = "capability_query_failed"), 1).status)
    }

    @Test fun ambiguousAttemptIsUnknownAndInconsistentReceiptsAreRejected() {
        assertEquals(SUMHTemporaryMapsRemoveStatus.Unknown,
            remove(f.receipt("outcome_unknown", 5, code = "outcome_unknown"), 1).status)
        val invalid = listOf(f.receipt(attempted = false), f.receipt(acknowledged = false), f.receipt(errno = 5),
            f.receipt("not_applied", 5, code = "outcome_unknown"), f.receipt("outcome_unknown", 5, attempted = false, code = "capability_query_failed"),
            f.receipt("not_applied", 0, attempted = false), f.receipt(code = "other"), f.receipt().put("status", "ok"),
            f.receipt().put("saved", true), f.receipt().put("observer_verified", true), f.receipt().put("history_supported", true),
            f.receipt().put("operation_id", "abc"), f.receipt().put("extra", 1), f.receipt().put("schema_version", 2))
        for (value in invalid) assertFails { remove(value, if (value.getString("status") == "deleted") 0 else 1) }
        assertFails { remove(f.receipt(), 1) }
        assertFails { remove(f.receipt("not_applied", 116, code = "snapshot_changed"), 0) }
    }

    @Test fun outerTransportFailureDiscardsEvenPlausibleSuccessPayload() = runBlocking {
        for ((key, value) in listOf("ok" to false, "exit_code" to 1, "errno" to 5)) {
            val raw = f.envelope(f.dispatch("remove", f.receipt())).put(key, value).toString().toByteArray()
            val result = NativeSUMHTemporaryMapsService { raw }.remove(f.request)
            assertEquals(SUMHTemporaryMapsRemoveStatus.Unknown, result.status)
            assertFalse(result.acknowledged); assertNull(result.attempted)
        }
    }

    @Test fun malformedTruncatedRawDispatchDuplicateAndFutureEnvelopesFailClosed() = runBlocking {
        val valid = f.envelope(f.dispatch("remove", f.receipt())).toString()
        val cases = listOf(valid.dropLast(1), valid + "{}", valid.replaceFirst("{", "{\"ok\":true,"),
            f.dispatch("remove", f.receipt()).toString(),
            f.envelope(f.dispatch("list", f.receipt())).toString(),
            f.envelope(f.dispatch("remove", f.receipt()).put("schema_version", 2)).toString(),
            f.envelope(f.dispatch("remove", f.receipt()).put("extra", true)).toString(),
            f.envelope(f.dispatch("remove", f.receipt())).put("extra", true).toString())
        for (raw in cases) {
            var calls = 0
            val result = NativeSUMHTemporaryMapsService { calls++; raw.toByteArray() }.remove(f.request)
            assertEquals(SUMHTemporaryMapsRemoveStatus.Unknown, result.status); assertEquals(1, calls)
        }
    }

    @Test fun invalidUtf8NulOversizeAndDuplicateInnerFieldsAreUnknown() = runBlocking {
        val inner = f.dispatch("remove", f.receipt()).toString()
        val duplicate = inner.replaceFirst("{", "{\"schema_version\":1,")
        val cases = listOf(byteArrayOf(0xc3.toByte(), 0x28), ByteArray(SUMH_TEMPORARY_MAPS_ENVELOPE_BYTES + 1) { 32 },
            f.envelope(f.dispatch("remove", f.receipt())).put("stdout", duplicate).toString().toByteArray(),
            f.envelope(f.dispatch("remove", f.receipt())).put("stdout", "x".repeat(SUMH_TEMPORARY_MAPS_PAYLOAD_BYTES + 1)).toString().toByteArray(),
            f.envelope(f.dispatch("remove", f.receipt())).put("stderr", "\u0000").toString().toByteArray())
        for (raw in cases) assertEquals(SUMHTemporaryMapsRemoveStatus.Unknown, NativeSUMHTemporaryMapsService { raw }.remove(f.request).status)
    }

    @Test fun exceptionsAndCancellationNeverRetryAMutation() = runBlocking {
        for (failure in listOf(IllegalStateException("timeout"), CancellationException("cancelled"))) {
            var calls = 0
            val result = NativeSUMHTemporaryMapsService { calls++; throw failure }.remove(f.request)
            assertEquals(1, calls); assertEquals(SUMHTemporaryMapsRemoveStatus.Unknown, result.status)
            assertFalse(result.acknowledged); assertNull(result.attempted)
        }
        var reads = 0
        val service = NativeSUMHTemporaryMapsService { reads++; throw CancellationException("cancelled") }
        assertFailsWith<CancellationException> { service.capabilities() }
        assertFailsWith<CancellationException> { service.list() }
        assertEquals(2, reads)
    }

    @Test fun unavailableNativeSymbolFailsClosedWithoutFallback() = runBlocking {
        var calls = 0
        val service = NativeSUMHTemporaryMapsService { calls++; throw UnsatisfiedLinkError("old native library") }
        assertEquals(SUMHTemporaryMapsSupport.Unknown, service.capabilities().inventory)
        assertNotNull(service.list().error)
        assertEquals(SUMHTemporaryMapsRemoveStatus.Unknown, service.remove(f.request).status)
        assertEquals(3, calls)
    }

    @Test fun serviceNeverImplicitlyQueriesOrCollectsOrRetries() = runBlocking {
        val actions = mutableListOf<String>()
        val service = NativeSUMHTemporaryMapsService { input ->
            val request = JSONObject(input.toString(Charsets.UTF_8)); val action = request.getString("action")
            actions.add(action)
            when (action) {
                "capabilities" -> f.wire(action, f.capabilities())
                "list" -> f.wire(action, f.page(end = false))
                else -> f.wire(action, f.receipt("not_applied", 116, code = "snapshot_changed"), 1)
            }
        }
        assertTrue(actions.isEmpty())
        service.capabilities(); service.list(); service.remove(f.request)
        assertEquals(listOf("capabilities", "list", "remove"), actions)
    }
}
