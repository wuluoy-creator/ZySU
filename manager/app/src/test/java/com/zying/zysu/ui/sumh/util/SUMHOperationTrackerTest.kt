package com.zying.zysu.ui.sumh.util

import android.app.Application
import kotlinx.coroutines.*
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.*

internal object SUMHTrackerFixtures {
    val f = SUMHHideDefinitionsFixtures
    val args = listOf("hide", "disable", f.id(1), f.revision)
    val request = sumhOperationRevision(args)
    val id = "c".repeat(32)
    fun entry(stage: String = "prepared") = SUMHTrackedOperation(id, request, f.boot, "disable", f.id(1), f.revision, stage)
    private fun output(value: String = "") = JSONObject().put("encoding", "utf-8").put("value", value)
        .put("byte_length", value.toByteArray(Charsets.UTF_8).size).put("complete", true)
    fun submission(status: String = "prepared", mutation: JSONObject = f.mutation()) = JSONObject()
        .put("schema_version", 1).put("kind", "sumh.operation.submission").put("operation_id", id)
        .put("request_revision", request).put("boot_id", f.boot).put("status", status)
        .put("command_executed_this_attempt", if (status == "transport_unknown") JSONObject.NULL else status != "prepared")
        .put("final_record_confirmed", status in setOf("command_completed", "failed_or_partial"))
        .put("runtime_verified", false).put("command_exit_code", when(status) { "command_completed" -> 0; "failed_or_partial" -> 1; else -> JSONObject.NULL })
        .put("command_errno", if (status in setOf("command_completed", "failed_or_partial")) 0 else JSONObject.NULL)
        .put("observed_record", JSONObject.NULL).put("stdout", if (status in setOf("command_completed", "failed_or_partial")) output(mutation.toString()) else output())
        .put("stderr", output()).put("error", "").apply { if (status == "prepared") put("reservation_confirmed", true) }
    fun record(phase: String = "finished", currentBoot: String = f.boot) = JSONObject()
        .put("id", id).put("name", "hide disable").put("phase", phase).put("boot_id", f.boot).put("request_revision", request)
        .put("result", when(phase) { "prepared" -> "not_started"; "started" -> "outcome_unknown"; else -> "command_completed" })
        .put("same_boot", currentBoot == f.boot).put("exit_code", if (phase == "finished") 0 else JSONObject.NULL)
        .put("errno", if (phase == "finished") 0 else JSONObject.NULL).put("detail", "")
    fun history(phase: String = "finished", currentBoot: String = f.boot) = JSONObject()
        .put("schema_version", 2).put("resource", "operation_history").put("read_only", true).put("runtime_verified", false)
        .put("scope", "recorded_commands_in_caller_filesystem_namespace").put("durability_confirmation", "not_observable_from_read")
        .put("ledger_status", "valid").put("error", "").put("boot_id", currentBoot).put("requested_id", id)
        .put("history_complete", false).put("history_pruned", false).put("retention_limit", 16)
        .put("records", JSONArray(listOf(record(phase, currentBoot)))).put("status", "observed").put("wait_budget_ms", 0).put("elapsed_ms", 0)
    fun recovered(currentBoot: String = f.boot) = JSONObject()
        .put("schema_version", 1).put("resource", "operation_resource_result").put("read_only", true).put("runtime_verified", false)
        .put("durability_confirmation", "not_observable_from_read").put("status", "observed").put("error", "")
        .put("ledger_status", "valid").put("receipt_status", "valid").put("requested_id", id).put("boot_id", currentBoot)
        .put("origin_boot_id", f.boot).put("same_boot", currentBoot == f.boot).put("request_revision", request)
        .put("record", record(currentBoot = currentBoot)).put("result", f.mutation()).put("retention_limit", 16)
    fun reply(value: JSONObject) = SUMHQueryReply(value.toString())
    class Memory(var records: List<SUMHTrackedOperation> = emptyList()) : SUMHOperationStorage {
        var writes = 0; var failAt = 0; var failRead = false
        override fun read(): List<SUMHTrackedOperation> { check(!failRead) { "future format" }; return records }
        override fun write(records: List<SUMHTrackedOperation>) {
            writes++; if (writes == failAt) error("sync failed")
            this.records = decodeSUMHTrackedOperations(encodeSUMHTrackedOperations(records))
        }
    }
}

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHOperationTrackerTest {
    private val t = SUMHTrackerFixtures
    private val f = SUMHHideDefinitionsFixtures
    private fun successTransport(actions: MutableList<String>, storage: SUMHTrackerFixtures.Memory): SUMHOperationTransport = { raw ->
        val action = JSONObject(raw).getString("action"); actions += action
        if (action == "execute") { assertEquals("dispatching", storage.records.single().stage); assertEquals(2, storage.writes) }
        t.reply(t.submission(if (action == "prepare") "prepared" else "command_completed"))
    }
    @Test fun checkpointsAreConfirmedBeforeExactlyOneExecuteAndReceiptPersists() = runBlocking {
        val storage = SUMHTrackerFixtures.Memory(); val calls = mutableListOf<String>()
        val tracker = SUMHOperationTracker(storage, successTransport(calls, storage))
        val result = tracker.changeHide(t.args, f.revision, f.boot)
        assertTrue(result.succeeded); assertEquals(t.id, result.operationId)
        assertEquals(listOf("prepare", "execute"), calls)
        assertEquals("received", storage.records.single().stage); assertTrue(storage.records.single().safelyPrunable)
    }
    @Test fun importDefinitionUsesDurableCheckpointAndTypedSelectorReceipt() = runBlocking {
        val imported = SUMHHidePortableDefinition(f.id(1), "/exact\n尾 ", false, "name", "note", "--literal")
        val args = SUMHHideImportStep(imported.id, null, imported, "absent").arguments(f.revision)
        val requestRevision = sumhOperationRevision(args)
        val storage = SUMHTrackerFixtures.Memory(); val calls = mutableListOf<String>()
        val tracker = SUMHOperationTracker(storage) { raw ->
            val action = JSONObject(raw).getString("action"); calls += action
            if (action == "execute") {
                val checkpoint = storage.records.single()
                assertEquals("dispatching", checkpoint.stage); assertEquals("import-id", checkpoint.action)
                assertEquals(imported.id, checkpoint.expectedDefinitionId); assertEquals(2, storage.writes)
            }
            t.reply(t.submission(if (action == "prepare") "prepared" else "command_completed",
                f.mutation().put("action", "import-id")).put("request_revision", requestRevision))
        }
        val result = tracker.changeHide(args, f.revision, f.boot)
        assertTrue(result.succeeded, result.error); assertEquals(listOf("prepare", "execute"), calls)
        assertEquals(imported.id, storage.records.single().resource()?.value?.items?.single()?.id)
        assertTrue(storage.records.single().safelyPrunable)
    }
    @Test fun eitherPreExecuteWriteFailurePreventsExecute() = runBlocking {
        for (fail in 1..2) {
            val storage = SUMHTrackerFixtures.Memory().apply { failAt = fail }; val calls = mutableListOf<String>()
            val tracker = SUMHOperationTracker(storage, successTransport(calls, storage))
            val result = tracker.changeHide(t.args, f.revision, f.boot)
            assertFalse(result.succeeded); assertEquals(listOf("prepare"), calls)
            assertEquals(t.id, result.operationId); assertTrue(result.error!!.contains("not_dispatched"))
        }
    }
    @Test fun corruptStorageNeverEvenReservesAndIsNotOverwritten() = runBlocking {
        val storage = SUMHTrackerFixtures.Memory().apply { failRead = true }; var calls = 0
        val tracker = SUMHOperationTracker(storage) { calls++; error("must not call") }
        assertFalse(tracker.changeHide(t.args, f.revision, f.boot).succeeded)
        assertEquals(0, calls); assertEquals(0, storage.writes)
        assertFalse(tracker.load().loaded); assertFalse(tracker.acknowledge(t.id))
    }
    @Test fun lostReplyRecoversExactTypedReceiptAfterNewOwnerWithoutExecute() = runBlocking {
        val storage = SUMHTrackerFixtures.Memory(); var execute = 0
        val first = SUMHOperationTracker(storage) { raw ->
            if (JSONObject(raw).getString("action") == "prepare") t.reply(t.submission())
            else { execute++; error("lost response") }
        }
        val result = first.changeHide(t.args, f.revision, f.boot)
        assertFalse(result.succeeded); assertEquals(t.id, result.operationId); assertEquals(1, execute)
        assertEquals("unknown", storage.records.single().stage)
        val calls = mutableListOf<String>()
        val recovered = SUMHOperationTracker(storage) { raw ->
            val action = JSONObject(raw).getString("action"); calls += action
            t.reply(when(action) { "show" -> t.history(); "result" -> t.recovered(); else -> error("no replay") })
        }
        assertTrue(recovered.load().loaded); assertTrue(recovered.recover(t.id))
        assertEquals(listOf("show", "result"), calls)
        assertNull(storage.records.single().resource()); assertTrue(storage.records.single().recovery()!!.resource!!.succeeded)
        assertFalse(storage.records.single().safelyPrunable) // recovered associations require explicit review
    }
    @Test fun preparedOwnerReloadOnlyQueriesAndDoesNotResumeSubmission() = runBlocking {
        val storage = SUMHTrackerFixtures.Memory(listOf(t.entry())); val calls = mutableListOf<String>()
        val tracker = SUMHOperationTracker(storage) { raw ->
            val action = JSONObject(raw).getString("action"); calls += action
            if(action == "show") t.reply(t.history("prepared")) else t.reply(t.recovered().put("status", "not_finished")
                .put("receipt_status", "not_read").put("record", t.record("prepared")).put("result", JSONObject.NULL))
        }
        tracker.load(); assertTrue(calls.isEmpty()); assertTrue(tracker.recover(t.id))
        assertEquals(listOf("show", "result"), calls); assertNull(storage.records.single().recovery()!!.resource)
    }
    @Test fun successfulSummaryWithoutRetainedResourceNeverBecomesResourceSuccess() = runBlocking {
        val storage = SUMHTrackerFixtures.Memory(listOf(t.entry("unknown")))
        val tracker = SUMHOperationTracker(storage) { raw -> t.reply(if(JSONObject(raw).getString("action") == "show") t.history()
            else t.recovered().put("status", "resource_unavailable").put("receipt_status", "missing").put("result", JSONObject.NULL)) }
        assertTrue(tracker.recover(t.id)); val entry = storage.records.single()
        assertEquals("command_completed", SUMHOperationsParser.observation(SUMHQueryReply(entry.observation), t.id).records.single().result)
        assertNull(entry.resource()); assertNull(entry.recovery()!!.resource); assertFalse(entry.safelyPrunable)
    }
    @Test fun postResponseStorageFailureStopsBatchAndRetainsRecoveryId() = runBlocking {
        val storage = SUMHTrackerFixtures.Memory().apply { failAt = 3 }; val calls = mutableListOf<String>()
        val tracker = SUMHOperationTracker(storage, successTransport(calls, storage))
        val result = changeHideDefinitionSelection("disable", listOf(f.id(1), f.id(2)), f.collection(), tracker::changeHide)
        assertEquals(1, result.attempted.size); assertEquals(listOf(f.id(2)), result.unattempted)
        assertFalse(result.attempted.single().second.succeeded)
        assertEquals(t.id, storage.records.single().id); assertEquals("unknown", storage.records.single().stage)
        assertEquals(listOf("prepare", "execute"), calls)
    }
    @Test fun fullUnresolvedHistoryRefusesNewReservationAndExplicitRemovalIsLocalOnly() = runBlocking {
        val storage = SUMHTrackerFixtures.Memory((0..15).map { t.entry().copy(id = f.id(it)) }); var calls = 0
        val tracker = SUMHOperationTracker(storage) { calls++; error("no network") }
        val result = tracker.changeHide(t.args, f.revision, f.boot)
        assertTrue(result.error!!.contains("full_review_required")); assertEquals(0, calls); assertEquals(16, storage.records.size)
        assertTrue(tracker.acknowledge(f.id(3))); assertEquals(15, storage.records.size); assertEquals(0, calls)
    }
    @Test fun oldBootRecoveryRemainsHistoricalAndIdentityMismatchCannotReplaceIt() = runBlocking {
        val storage = SUMHTrackerFixtures.Memory(listOf(t.entry("unknown"))); var mismatch = false
        val tracker = SUMHOperationTracker(storage) { raw -> t.reply(if(JSONObject(raw).getString("action") == "show") t.history(currentBoot = "new-boot")
            else t.recovered("new-boot").apply { if(mismatch) put("request_revision", f.nextRevision) }) }
        assertTrue(tracker.recover(t.id)); val raw = storage.records.single().recovered
        assertEquals(false, storage.records.single().recovery()!!.record!!.sameBoot)
        mismatch = true; assertFalse(tracker.recover(t.id)); assertEquals(raw, storage.records.single().recovered)
        assertTrue(storage.records.single().error.isNotEmpty())
    }
    @Test fun concurrentChangeIsRejectedAndCancellationDoesNotReplayRunningExecution() = runBlocking {
        val storage = SUMHTrackerFixtures.Memory(); val entered = CompletableDeferred<Unit>(); val release = CompletableDeferred<Unit>(); var executions = 0
        val tracker = SUMHOperationTracker(storage) { raw ->
            if(JSONObject(raw).getString("action") == "prepare") t.reply(t.submission())
            else { executions++; entered.complete(Unit); release.await(); t.reply(t.submission("command_completed")) }
        }
        val job = launch { tracker.changeHide(t.args, f.revision, f.boot) }; entered.await()
        assertTrue(tracker.changeHide(t.args, f.revision, f.boot).error!!.contains("busy_not_started"))
        job.cancel(); release.complete(Unit); job.join()
        assertEquals(1, executions); assertEquals("received", storage.records.single().stage)
    }
    @Test fun errorTruncationPreservesValidUtf8AndCheckpoint() = runBlocking {
        val text = "中文😀".repeat(1000); val bounded = sumhOperationError(text)
        assertTrue(bounded.toByteArray(Charsets.UTF_8).size <= 1500); assertTrue(Charsets.UTF_8.newEncoder().canEncode(bounded))
        val storage = SUMHTrackerFixtures.Memory(); val tracker = SUMHOperationTracker(storage) { raw ->
            if(JSONObject(raw).getString("action") == "prepare") t.reply(t.submission()) else error(text)
        }
        assertFalse(tracker.changeHide(t.args, f.revision, f.boot).succeeded)
        assertTrue(storage.records.single().error.toByteArray(Charsets.UTF_8).size <= 1500)
    }
}
