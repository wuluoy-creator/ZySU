package com.zying.zysu.ui.sumh.util

import android.app.Application
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assume.assumeNoException
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.ByteArrayOutputStream
import java.io.DataOutputStream
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.file.Files
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import kotlin.test.*

/** Host filesystem/fault-model tests; injected sync callbacks do not claim Android durability. */
@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHOperationStorageTest {
    @get:Rule val temporary = TemporaryFolder()
    private val f = SUMHHideDefinitionsFixtures
    private val request = "sha256-v1:" + "d".repeat(64)
    private fun prepared(number: Int = 1) = SUMHTrackedOperation(number.toString(16).padStart(32, '0'),
        request, f.boot, "disable", f.id(1), f.revision, "prepared")
    private fun store(directory: File) = SUMHOperationFile(directory) { }
    private fun document(directory: File) = File(directory, "operations-v1.bin")
    private fun scratch(directory: File) = File(directory, "operations-v1.tmp")
    private fun output(value: String = "") = JSONObject().put("encoding", "utf-8").put("value", value)
        .put("byte_length", value.toByteArray(Charsets.UTF_8).size).put("complete", true)
    private fun submission(entry: SUMHTrackedOperation, stdout: String = f.mutation().toString(), stderr: String = "") = JSONObject()
        .put("schema_version", 1).put("kind", "sumh.operation.submission").put("operation_id", entry.id)
        .put("request_revision", entry.requestRevision).put("boot_id", entry.bootId).put("status", "command_completed")
        .put("command_executed_this_attempt", true).put("final_record_confirmed", true).put("runtime_verified", false)
        .put("command_exit_code", 0).put("command_errno", 0).put("observed_record", JSONObject.NULL)
        .put("stdout", output(stdout)).put("stderr", output(stderr)).put("error", "").toString()
    private fun finished(entry: SUMHTrackedOperation) = JSONObject().put("id", entry.id).put("name", entry.name)
        .put("phase", "finished").put("boot_id", entry.bootId).put("request_revision", entry.requestRevision)
        .put("result", "command_completed").put("same_boot", true).put("exit_code", 0).put("errno", 0).put("detail", "command status only")
    private fun history(entry: SUMHTrackedOperation) = JSONObject().put("schema_version", 2).put("resource", "operation_history")
        .put("read_only", true).put("runtime_verified", false).put("history_complete", false).put("history_pruned", false)
        .put("scope", "recorded_commands_in_caller_filesystem_namespace").put("durability_confirmation", "not_observable_from_read")
        .put("ledger_status", "valid").put("error", "").put("boot_id", entry.bootId).put("requested_id", entry.id)
        .put("retention_limit", 16).put("records", JSONArray(listOf(finished(entry)))).put("status", "observed")
        .put("wait_budget_ms", 0).put("elapsed_ms", 0).toString()
    private fun recovered(entry: SUMHTrackedOperation) = JSONObject().put("schema_version", 1).put("resource", "operation_resource_result")
        .put("read_only", true).put("runtime_verified", false).put("durability_confirmation", "not_observable_from_read")
        .put("status", "observed").put("error", "").put("ledger_status", "valid").put("receipt_status", "valid")
        .put("requested_id", entry.id).put("boot_id", entry.bootId).put("origin_boot_id", entry.bootId).put("same_boot", true)
        .put("request_revision", entry.requestRevision).put("record", finished(entry)).put("result", f.mutation())
        .put("retention_limit", 16).toString()
    private fun raw(fields: List<List<ByteArray>>, magic: Int = 0x534F5031, count: Int = fields.size): ByteArray =
        ByteArrayOutputStream().also { buffer -> DataOutputStream(buffer).use { out ->
            out.writeInt(magic); out.writeInt(count)
            fields.forEach { record -> record.forEach { value -> out.writeInt(value.size); out.write(value) } }
        } }.toByteArray()
    private fun fields(record: SUMHTrackedOperation) = record.fields().map { it.toByteArray(Charsets.UTF_8) }

    @Test fun codecRoundTripsAllElevenFieldsWithoutLosingExactText() {
        val base = prepared()
        val received = base.copy(stage = "received", response = submission(base), observation = history(base),
            error = "诊断 😀\n尾部 ", recovered = recovered(base))
        assertEquals(11, received.fields().size)
        assertEquals(listOf(received), decodeSUMHTrackedOperations(encodeSUMHTrackedOperations(listOf(received))))
        assertTrue(received.resource()!!.succeeded)
        assertTrue(received.recovery()!!.resource!!.succeeded)
        val group = prepared(2).copy(action = "enable-group", selector = "工作资料 😀\n ", stage = "unknown", error = "等待 ")
        assertEquals(listOf(received, group), decodeSUMHTrackedOperations(encodeSUMHTrackedOperations(listOf(received, group))))
    }

    @Test fun codecAcceptsEmptyAndExactlySixteenRecords() {
        assertEquals(emptyList(), decodeSUMHTrackedOperations(encodeSUMHTrackedOperations(emptyList())))
        val records = (1..16).map(::prepared)
        assertEquals(records, decodeSUMHTrackedOperations(encodeSUMHTrackedOperations(records)))
        assertFails { encodeSUMHTrackedOperations(records + prepared(17)) }
        assertFails { decodeSUMHTrackedOperations(raw(records.map(::fields), count = 17)) }
    }

    @Test fun codecRejectsFutureFormatEveryTruncationAndTrailingBytes() {
        val encoded = encodeSUMHTrackedOperations(listOf(prepared()))
        assertFails { decodeSUMHTrackedOperations(raw(listOf(fields(prepared())), magic = 0x534F5032)) }
        for (length in 0 until encoded.size) assertFails("truncated at $length") { decodeSUMHTrackedOperations(encoded.copyOf(length)) }
        assertFails { decodeSUMHTrackedOperations(encoded + byteArrayOf(0)) }
        assertFails { decodeSUMHTrackedOperations(raw(emptyList(), count = -1)) }
    }

    @Test fun codecRejectsDuplicateIdentityAndInvalidFieldLengths() {
        val record = prepared()
        assertFails { encodeSUMHTrackedOperations(listOf(record, record)) }
        assertFails { decodeSUMHTrackedOperations(raw(listOf(fields(record), fields(record)))) }
        val encoded = encodeSUMHTrackedOperations(listOf(record))
        for (length in listOf(-1, 33, Int.MAX_VALUE)) {
            val changed = encoded.copyOf().also { ByteBuffer.wrap(it).putInt(8, length) }
            assertFails { decodeSUMHTrackedOperations(changed) }
        }
        val oversized = fields(record).toMutableList().apply { this[9] = ByteArray(1501) { 'x'.code.toByte() } }
        assertFails { decodeSUMHTrackedOperations(raw(listOf(oversized))) }
        assertFails { decodeSUMHTrackedOperations(ByteArray(SUMH_OPERATION_LOCAL_BYTES + 1)) }
    }

    @Test fun codecRejectsMalformedUtf8AndInvalidLogicalRecords() {
        val malformed = fields(prepared()).toMutableList().apply { this[9] = byteArrayOf(0xc0.toByte(), 0x80.toByte()) }
        assertFails { decodeSUMHTrackedOperations(raw(listOf(malformed))) }
        val records = listOf(prepared().copy(id = "A".repeat(32)), prepared().copy(requestRevision = "future:revision"),
            prepared().copy(bootId = ""), prepared().copy(action = "clear"), prepared().copy(selector = "/path"),
            prepared().copy(stage = "complete"), prepared().copy(stage = "received"), prepared().copy(error = "\uD800"),
            prepared().copy(error = "文".repeat(501)), prepared().copy(recovered = "{}"), prepared().copy(observation = "{}"))
        records.forEach { record ->
            assertFails { encodeSUMHTrackedOperations(listOf(record)) }
            if (Charsets.UTF_8.newEncoder().canEncode(record.error)) assertFails { decodeSUMHTrackedOperations(raw(listOf(fields(record)))) }
        }
    }

    @Test fun aggregateBudgetStillAppliesToIndividuallyValidLargeReceipts() {
        val payload = "x".repeat(64 * 1024)
        val records = (1..9).map { number -> prepared(number).let { it.copy(stage = "received", response = submission(it, payload, payload)) } }
        records.forEach(::validateTrackedOperation)
        assertFails { encodeSUMHTrackedOperations(records) }
        val encoded = encodeSUMHTrackedOperations(records.take(7))
        assertTrue(encoded.size < SUMH_OPERATION_LOCAL_BYTES)
        assertEquals(records.take(7), decodeSUMHTrackedOperations(encoded))
    }

    @Test fun missingStorageIsEmptyButNonDirectoryAndNonRegularTargetsFailClosed() {
        val absent = File(temporary.root, "absent")
        assertEquals(emptyList(), store(absent).read())
        absent.writeText("not a directory")
        assertFails { store(absent).read() }; assertFails { store(absent).write(listOf(prepared())) }
        assertEquals("not a directory", absent.readText())
        val directory = temporary.newFolder("non-regular")
        assertTrue(document(directory).mkdir())
        assertFails { store(directory).read() }; assertFails { store(directory).write(listOf(prepared())) }
        assertTrue(document(directory).isDirectory)
    }

    @Test fun corruptFutureAndOversizedDocumentsAreNeverOverwritten() {
        val good = encodeSUMHTrackedOperations(listOf(prepared()))
        val bad = listOf(good.copyOf(3), raw(listOf(fields(prepared())), magic = 0x534F5032), good + byteArrayOf(0),
            ByteArray(SUMH_OPERATION_LOCAL_BYTES + 1))
        bad.forEachIndexed { index, bytes ->
            val directory = temporary.newFolder("bad-$index")
            document(directory).writeBytes(bytes)
            var syncs = 0
            val storage = SUMHOperationFile(directory) { syncs++ }
            assertFails { storage.read() }; assertFails { storage.write(listOf(prepared(2))) }
            assertContentEquals(bytes, document(directory).readBytes())
            assertEquals(0, syncs)
        }
    }

    @Test fun parentSyncFailureMustBeRetriedEvenAfterMkdirSucceeded() {
        val directory = File(temporary.root, "checkpoint")
        val calls = mutableListOf<File>(); var failParent = true
        val storage = SUMHOperationFile(directory) { path ->
            calls += path
            if (path == directory.parentFile && failParent) throw IOException("parent sync failed")
        }
        assertFailsWith<IOException> { storage.write(listOf(prepared())) }
        assertTrue(directory.isDirectory); assertFalse(document(directory).exists())
        assertEquals(emptyList(), storage.read())
        failParent = false
        storage.write(listOf(prepared()))
        assertEquals(listOf(directory.parentFile, directory.parentFile, directory), calls)
        assertEquals(listOf(prepared()), storage.read())
    }

    @Test fun parentSyncFailurePreservesPreviousCheckpointBytes() {
        val directory = temporary.newFolder("previous")
        store(directory).write(listOf(prepared()))
        val original = document(directory).readBytes()
        val storage = SUMHOperationFile(directory) { if (it == directory.parentFile) throw IOException("parent failed") }
        assertFailsWith<IOException> { storage.write(listOf(prepared(2))) }
        assertContentEquals(original, document(directory).readBytes())
        assertEquals(listOf(prepared()), storage.read())
    }

    @Test fun directorySyncFailureReportsUnconfirmedEvenWhenNewCheckpointIsReadable() {
        val directory = temporary.newFolder("unconfirmed")
        store(directory).write(listOf(prepared()))
        var fail = true
        val storage = SUMHOperationFile(directory) { if (it == directory && fail) throw IOException("directory sync failed") }
        val next = listOf(prepared(2).copy(stage = "dispatching"))
        assertFailsWith<IOException> { storage.write(next) }
        assertEquals(next, storage.read()) // Readability alone does not retroactively confirm the failed write.
        assertFalse(scratch(directory).exists())
        fail = false; storage.write(next)
        assertEquals(next, storage.read())
    }

    @Test fun readbackMismatchFailsAfterReplacementWithoutInventingSuccess() {
        val directory = temporary.newFolder("readback")
        val other = listOf(prepared(3))
        val storage = SUMHOperationFile(directory) { if (it == directory) document(directory).writeBytes(encodeSUMHTrackedOperations(other)) }
        assertFailsWith<IOException> { storage.write(listOf(prepared())) }
        assertEquals(other, storage.read())
        assertFalse(scratch(directory).exists())
    }

    @Test fun staleScratchIsIgnoredAndWholeDocumentsReplaceAcrossStoreInstances() {
        val directory = temporary.newFolder("replace")
        val first = listOf(prepared())
        store(directory).write(first)
        scratch(directory).writeText("interrupted and incomplete")
        assertEquals(first, store(directory).read())
        val second = listOf(prepared(2), prepared(3))
        store(directory).write(second)
        assertEquals(second, store(directory).read())
        assertEquals(listOf("operations-v1.bin"), directory.list()!!.toList())
        store(directory).write(emptyList())
        assertEquals(emptyList(), store(directory).read())
    }

    @Test fun hardLinkedScratchIsUnlinkedWithoutTruncatingItsOtherName() {
        val directory = temporary.newFolder("hardlink")
        val target = temporary.newFile("keep-bytes").apply { writeText("must remain unchanged") }
        try { Files.createLink(scratch(directory).toPath(), target.toPath()) }
        catch (error: Exception) { assumeNoException("Host cannot create hard links", error) }
        store(directory).write(listOf(prepared()))
        assertEquals("must remain unchanged", target.readText())
        assertEquals(listOf(prepared()), store(directory).read())
        assertFalse(scratch(directory).exists())
    }

    @Test fun symbolicDirectoryDocumentAndScratchAreRejectedWhenHostSupportsLinks() {
        val real = temporary.newFolder("real")
        val alias = File(temporary.root, "alias")
        try { Files.createSymbolicLink(alias.toPath(), real.toPath()) }
        catch (error: Exception) { assumeNoException("Host cannot create symbolic links", error) }
        assertFails { store(alias).read() }; assertFails { store(alias).write(listOf(prepared())) }
        val directory = temporary.newFolder("symbolic")
        val target = temporary.newFile("outside").apply { writeBytes(encodeSUMHTrackedOperations(listOf(prepared(2)))) }
        val old = target.readBytes()
        Files.createSymbolicLink(document(directory).toPath(), target.toPath())
        assertFails { store(directory).read() }; assertFails { store(directory).write(listOf(prepared())) }
        Files.delete(document(directory).toPath())
        Files.createSymbolicLink(scratch(directory).toPath(), target.toPath())
        assertFails { store(directory).write(listOf(prepared())) }
        assertContentEquals(old, target.readBytes())
    }

    @Test fun twoStoreInstancesSerializeReplacementAndExposeOnlyWholeDocuments() {
        val directory = temporary.newFolder("parallel")
        val first = listOf(prepared(1)); val second = listOf(prepared(2), prepared(3))
        store(directory).write(first)
        val start = CountDownLatch(1); val pool = Executors.newFixedThreadPool(2)
        try {
            val tasks = listOf(first, second).map { records -> pool.submit {
                start.await()
                repeat(8) {
                    store(directory).write(records)
                    assertTrue(store(directory).read() in listOf(first, second))
                }
            } }
            start.countDown()
            tasks.forEach { it.get(20, TimeUnit.SECONDS) }
            assertTrue(store(directory).read() in listOf(first, second))
            assertEquals(listOf("operations-v1.bin"), directory.list()!!.toList())
        } finally { pool.shutdownNow() }
    }
}
