package com.zying.zysu.ui.sumh.util

import android.app.Application
import kotlinx.coroutines.*
import org.json.JSONObject
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHTemporaryMapsOperationTrackerTest {
    private val f = SUMHTemporaryMapsTrackingFixtures
    @Test fun defaultServiceConstructionNeedsNeitherRealAppNorNativeTransport() {
        // This class uses plain Application; merely constructing the singleton
        // must not resolve ksuApp or touch its noBackup directory.
        assertNotNull(SUMHTemporaryMapsTracking.service)
    }
    private fun tracker(storage: SUMHTemporaryMapsOperationStorage, calls: MutableList<String> = mutableListOf(),
        support: SUMHTemporaryMapsSupport = SUMHTemporaryMapsSupport.Supported,
        respond: suspend (String) -> SUMHQueryReply = { action -> f.reply("deleted-$action") }) =
        SUMHTemporaryMapsOperationTracker(storage, { raw ->
            val request = JSONObject(raw); assertEquals(2, request.getInt("schema_version"))
            assertEquals("temporary_maps", request.getString("resource"))
            val action = request.getString("action"); calls += action
            if (action in setOf("prepare", "execute")) {
                val args = request.getJSONObject("input").getJSONArray("args")
                assertEquals(f.args, (0 until args.length()).map { args.getString(it) })
                assertEquals(1, request.getJSONObject("input").getInt("schema_version"))
            } else assertFalse(request.has("input"))
            if (action != "prepare") assertEquals(f.id, request.getString("operation_id"))
            respond(action)
        }, { support })

    @Test fun actualNativeSuccessRequiresBothCheckpointsBeforeSingleExecute() = runBlocking<Unit> {
        val storage = SUMHTemporaryMapsTrackingFixtures.Memory(); val calls = mutableListOf<String>()
        val owner = tracker(storage, calls) { action ->
            if (action == "execute") {
                assertEquals(2, storage.writes); assertEquals("dispatching", storage.records.single().stage)
                assertEquals(f.key, storage.records.single().key); assertEquals(f.boot, storage.records.single().bootId)
            }
            f.reply("deleted-$action")
        }
        val result = owner.remove(f.request())
        assertEquals(SUMHTemporaryMapsRemoveStatus.Deleted, result.result.status); assertTrue(result.dispatched)
        assertEquals(f.id, result.operationId); assertEquals(listOf("prepare", "execute"), calls)
        assertEquals(3, storage.writes); assertTrue(storage.records.single().safelyPrunable)
    }
    @Test fun eitherPreExecuteCheckpointFailureIncludingLandedBytesNeverExecutes() = runBlocking<Unit> {
        for (stage in 1..2) for (landed in listOf(false, true)) {
            val storage = SUMHTemporaryMapsTrackingFixtures.Memory().apply { failAt = stage; this.landed = landed }; val calls = mutableListOf<String>()
            val result = tracker(storage, calls).remove(f.request())
            assertFalse(result.dispatched); assertEquals(SUMHTemporaryMapsRemoveStatus.NotApplied, result.result.status)
            assertEquals(f.id, result.operationId); assertEquals(listOf("prepare"), calls)
            val restoredCalls = mutableListOf<String>(); val restored = tracker(storage, restoredCalls)
            restored.load(); assertTrue(restoredCalls.isEmpty())
            if (storage.records.isNotEmpty()) { restored.recover(f.id); assertEquals(listOf("show", "result"), restoredCalls) }
        }
    }
    @Test fun unknownOrUnsupportedCapabilitiesAndCorruptStorageNeverPrepare() = runBlocking<Unit> {
        for (support in listOf(SUMHTemporaryMapsSupport.Unsupported, SUMHTemporaryMapsSupport.Unknown)) {
            val storage = SUMHTemporaryMapsTrackingFixtures.Memory(); val calls = mutableListOf<String>()
            val result = tracker(storage, calls, support).remove(f.request())
            assertFalse(result.dispatched); assertTrue(calls.isEmpty()); assertEquals(0, storage.writes)
        }
        val storage = SUMHTemporaryMapsTrackingFixtures.Memory().apply { failRead = true }; val calls = mutableListOf<String>()
        val owner = tracker(storage, calls)
        assertFalse(owner.remove(f.request()).dispatched); assertFalse(owner.load().loaded); assertFalse(owner.forget(f.id))
        assertTrue(calls.isEmpty()); assertEquals(0, storage.writes)
    }
    @Test fun unconfirmedPreparationOrMalformedPrepareNeverDispatches() = runBlocking<Unit> {
        for (mode in 0..2) {
            val storage = SUMHTemporaryMapsTrackingFixtures.Memory(); val calls = mutableListOf<String>()
            val owner = tracker(storage, calls) {
                when (mode) {
                    0 -> throw IllegalStateException("prepare reply lost")
                    1 -> f.query(f.value("deleted-prepare").put("reservation_confirmed", false))
                    else -> f.query(f.value("deleted-prepare").put("request_revision", "sha256-v1:" + "f".repeat(64)))
                }
            }
            assertFalse(owner.remove(f.request()).dispatched); assertEquals(listOf("prepare"), calls); assertEquals(0, storage.writes)
        }
    }
    @Test fun lostReplyRecreatedOwnerLoadsAndRecoversOnlyHistoricalQueries() = runBlocking<Unit> {
        val storage = SUMHTemporaryMapsTrackingFixtures.Memory(); val calls = mutableListOf<String>()
        val owner = tracker(storage, calls) { action -> if (action == "execute") error("reply lost") else f.reply("deleted-$action") }
        val result = owner.remove(f.request())
        assertTrue(result.dispatched); assertEquals(SUMHTemporaryMapsRemoveStatus.Unknown, result.result.status)
        assertEquals("unknown", storage.records.single().stage); assertEquals(listOf("prepare", "execute"), calls)
        val recoveryCalls = mutableListOf<String>(); val restored = tracker(storage, recoveryCalls)
        restored.load(); assertTrue(recoveryCalls.isEmpty()); assertTrue(restored.recover(f.id))
        assertEquals(listOf("show", "result"), recoveryCalls); assertNull(storage.records.single().resource())
        assertEquals(SUMHTemporaryMapsRemoveStatus.Deleted, storage.records.single().recovery()!!.resource!!.status)
    }
    @Test fun postExecuteWriteFailureAndChangedFutureFileRemainUnknownWithoutOverwrite() = runBlocking<Unit> {
        for (landed in listOf(false, true)) {
            val storage = SUMHTemporaryMapsTrackingFixtures.Memory().apply { failAt = 3; this.landed = landed }; val calls = mutableListOf<String>()
            val result = tracker(storage, calls).remove(f.request())
            assertTrue(result.dispatched); assertEquals(SUMHTemporaryMapsRemoveStatus.Unknown, result.result.status)
            assertEquals(listOf("prepare", "execute"), calls); assertEquals(f.id, storage.records.single().id)
        }
        val storage = SUMHTemporaryMapsTrackingFixtures.Memory(); val calls = mutableListOf<String>()
        val owner = tracker(storage, calls) { action ->
            if (action == "execute") { storage.failRead = true; error("future file appeared") }
            f.reply("deleted-$action")
        }
        assertTrue(owner.remove(f.request()).dispatched); assertEquals(2, storage.writes)
        assertEquals("dispatching", storage.records.single().stage); assertEquals(listOf("prepare", "execute"), calls)
    }
    @Test fun fullUnresolvedHistoryBlocksReservationAndForgetIsLocalOnly() = runBlocking<Unit> {
        val records = (1..16).map { f.entry("unknown").copy(id = it.toString(16).padStart(32, '0')) }
        val storage = SUMHTemporaryMapsTrackingFixtures.Memory(records); val calls = mutableListOf<String>(); val owner = tracker(storage, calls)
        assertFalse(owner.remove(f.request()).dispatched); assertTrue(calls.isEmpty()); assertEquals(records, storage.records)
        assertTrue(owner.forget(records.first().id)); assertEquals(15, storage.records.size); assertTrue(calls.isEmpty())
        assertFalse(owner.forget(records.first().id)); assertEquals(15, storage.records.size)
    }
    @Test fun capacityPrunesOnlyOneVerifiedSuccessAndKeepsEveryUnresolvedAssociation() = runBlocking<Unit> {
        val unresolved = (1..15).map { f.entry("unknown").copy(id = it.toString(16).padStart(32, '0')) }
        val oldId = "ff".repeat(16)
        for (historical in listOf(false, true)) {
            val original = if (historical) f.recovered() else f.received()
            val success = original.copy(id = oldId, response = original.response.replace(f.id, oldId),
                observation = original.observation.replace(f.id, oldId), recovered = original.recovered.replace(f.id, oldId))
            val storage = SUMHTemporaryMapsTrackingFixtures.Memory(listOf(success) + unresolved); val calls = mutableListOf<String>()
            val result = tracker(storage, calls).remove(f.request())
            assertEquals(SUMHTemporaryMapsRemoveStatus.Deleted, result.result.status)
            assertEquals(listOf("prepare", "execute"), calls); assertEquals(16, storage.records.size)
            assertEquals(unresolved.map { it.id }.toSet() + f.id, storage.records.map { it.id }.toSet())
            assertTrue(storage.records.filter { it.id != f.id }.all { it.stage == "unknown" })
            assertFalse(storage.records.any { it.id == oldId })
        }
    }
    @Test fun preparedAndDispatchingRestorationNeverResumesEvenWhenNativeRecordIsUnfinished() = runBlocking<Unit> {
        for (stage in listOf("prepared", "dispatching", "unknown")) {
            val storage = SUMHTemporaryMapsTrackingFixtures.Memory(listOf(f.entry(stage))); val calls = mutableListOf<String>()
            val owner = tracker(storage, calls) { action -> f.reply("finish-unknown-$action") }
            assertTrue(owner.load().loaded); assertTrue(calls.isEmpty()); assertTrue(owner.recover(f.id))
            assertEquals(listOf("show", "result"), calls); assertNull(storage.records.single().recovery()!!.resource)
            assertEquals("not_finished", storage.records.single().recovery()!!.status)
        }
    }
    @Test fun historyWaitAndMissingResourceNeverInventDeletionSuccess() = runBlocking<Unit> {
        val storage = SUMHTemporaryMapsTrackingFixtures.Memory(listOf(f.entry("unknown"))); val calls = mutableListOf<String>()
        val owner = tracker(storage, calls) { action -> f.reply("resource-unavailable-$action") }
        assertTrue(owner.recover(f.id, 5000)); assertEquals(listOf("wait", "result"), calls)
        assertNull(storage.records.single().recovery()!!.resource); assertFalse(storage.records.single().safelyPrunable)
        assertFailsWith<IllegalArgumentException> { owner.recover(f.id, 5001) }
    }
    @Test fun mismatchedRetainedReceiptCannotOverwritePreviousHistoricalEvidence() = runBlocking<Unit> {
        val original = f.recovered(); val storage = SUMHTemporaryMapsTrackingFixtures.Memory(listOf(original)); val calls = mutableListOf<String>()
        val owner = tracker(storage, calls) { action ->
            if (action == "result") f.query(f.value("deleted-result").put("origin_boot_id", "wrong")) else f.reply("deleted-$action")
        }
        assertFalse(owner.recover(f.id)); assertEquals(original.recovered, storage.records.single().recovered)
        assertTrue(storage.records.single().error.isNotEmpty()); assertEquals(listOf("show", "result"), calls)
    }
    @Test fun duplicateCallAndCallerCancellationCannotCreateSecondExecute() = runBlocking<Unit> {
        val storage = SUMHTemporaryMapsTrackingFixtures.Memory(); val calls = mutableListOf<String>()
        val entered = CompletableDeferred<Unit>(); val release = CompletableDeferred<Unit>()
        val owner = tracker(storage, calls) { action ->
            if (action == "execute") { entered.complete(Unit); release.await() }
            f.reply("deleted-$action")
        }
        val pending = launch { owner.remove(f.request()) }; withTimeout(5000) { entered.await() }
        assertFalse(owner.remove(f.request()).dispatched); pending.cancel(); release.complete(Unit); pending.join()
        assertEquals(listOf("prepare", "execute"), calls); assertEquals(SUMHTemporaryMapsRemoveStatus.Deleted, storage.records.single().resource()!!.status)
    }
    @Test fun thrownCancellationAndInvalidTerminalPayloadDoNotTriggerFallback() = runBlocking<Unit> {
        for (mode in 0..2) {
            val storage = SUMHTemporaryMapsTrackingFixtures.Memory(); val calls = mutableListOf<String>()
            val owner = tracker(storage, calls) { action ->
                if (action != "execute") f.reply("deleted-$action")
                else when (mode) {
                    0 -> throw CancellationException("transport cancelled")
                    1 -> f.query(f.value("deleted-execute").put("operation_id", "ff".repeat(16)))
                    else -> f.query(f.value("deleted-execute").apply { getJSONObject("stdout").put("complete", false) })
                }
            }
            val result = owner.remove(f.request()); assertTrue(result.dispatched)
            assertEquals(SUMHTemporaryMapsRemoveStatus.Unknown, result.result.status); assertEquals(listOf("prepare", "execute"), calls)
            assertEquals("unknown", storage.records.single().stage)
        }
    }
}
