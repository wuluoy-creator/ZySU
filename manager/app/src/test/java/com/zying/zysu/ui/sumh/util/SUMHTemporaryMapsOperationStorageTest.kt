package com.zying.zysu.ui.sumh.util

import android.app.Application
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

/** Host storage and injected sync fault tests, not an Android durability claim. */
@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHTemporaryMapsOperationStorageTest {
    @get:Rule val temporary = TemporaryFolder()
    private val f = SUMHTemporaryMapsTrackingFixtures
    private fun store(directory: File) = SUMHTemporaryMapsOperationFile(directory) { }
    private fun document(directory: File) = File(directory, "temporary-maps-v1.bin")
    private fun scratch(directory: File) = File(directory, "temporary-maps-v1.tmp")
    private fun raw(fields: List<List<ByteArray>>, magic: Int = 0x53544D31, count: Int = fields.size) =
        ByteArrayOutputStream().also { buffer -> DataOutputStream(buffer).use { output ->
            output.writeInt(magic); output.writeInt(count)
            fields.forEach { record -> record.forEach { output.writeInt(it.size); output.write(it) } }
        } }.toByteArray()
    private fun fields(entry: SUMHTemporaryMapsTrackedOperation) = entry.fields().map { it.toByteArray(Charsets.UTF_8) }
    @Test fun exactTwelveFieldsRoundTripWithHistoricalAndImmediateReceiptsSeparate() {
        val entry = f.received().copy(observation = f.reply("historical-show").stdout,
            recovered = f.reply("historical-result").stdout, error = "中文🙂 trailing ")
        val bytes = encodeSUMHTemporaryMapsOperations(listOf(entry))
        assertEquals(0x53544D31, ByteBuffer.wrap(bytes).int)
        assertEquals(listOf(entry), decodeSUMHTemporaryMapsOperations(bytes)); assertEquals(12, entry.fields().size)
        assertFails { decodeSUMHTrackedOperations(bytes) } // hide SOP1 cannot reinterpret Maps selectors
        assertEquals(emptyList(), decodeSUMHTemporaryMapsOperations(encodeSUMHTemporaryMapsOperations(emptyList())))
    }
    @Test fun countLimitsDuplicateIdsAndAllTruncationsFailClosed() {
        val records = (1..16).map { f.entry().copy(id = it.toString(16).padStart(32, '0')) }
        assertEquals(records, decodeSUMHTemporaryMapsOperations(encodeSUMHTemporaryMapsOperations(records)))
        assertFails { encodeSUMHTemporaryMapsOperations(records + f.entry().copy(id = "f".repeat(32))) }
        assertFails { encodeSUMHTemporaryMapsOperations(listOf(f.entry(), f.entry())) }
        val bytes = encodeSUMHTemporaryMapsOperations(listOf(f.entry()))
        for (size in bytes.indices) assertFails { decodeSUMHTemporaryMapsOperations(bytes.copyOf(size)) }
        assertFails { decodeSUMHTemporaryMapsOperations(bytes + 0) }
        for (count in listOf(-1, 17, Int.MAX_VALUE)) assertFails { decodeSUMHTemporaryMapsOperations(raw(emptyList(), count = count)) }
    }
    @Test fun futureMagicMalformedUtf8AndLogicalIdentityAreRejected() {
        val fields = fields(f.entry())
        for (magic in listOf(0x53544D32, 0x534F5031, 0)) assertFails { decodeSUMHTemporaryMapsOperations(raw(listOf(fields), magic)) }
        for (index in fields.indices) {
            val bad = fields.toMutableList(); bad[index] = byteArrayOf(0xc0.toByte(), 0xaf.toByte())
            assertFails { decodeSUMHTemporaryMapsOperations(raw(listOf(bad))) }
        }
        for (entry in listOf(f.entry().copy(bootId = ""), f.entry().copy(stage = "future"), f.entry().copy(stage = "received"),
            f.entry().copy(requestRevision = "sha256-v1:" + "f".repeat(64)), f.entry().copy(error = "\ud800"), f.entry().copy(error = "\u0000")))
            assertFails { encodeSUMHTemporaryMapsOperations(listOf(entry)) }
    }
    @Test fun invalidReceivedAndRecoveredBindingCannotEnterTheFile() {
        val original = f.received(); val value = org.json.JSONObject(original.response)
        value.put("boot_id", "different")
        assertFails { encodeSUMHTemporaryMapsOperations(listOf(original.copy(response = value.toString()))) }
        val recovery = f.value("deleted-result").put("origin_boot_id", "different")
        assertFails { encodeSUMHTemporaryMapsOperations(listOf(f.entry().copy(recovered = recovery.toString()))) }
    }
    @Test fun aggregateByteLimitAppliesBeforeStorageMutation() {
        val value = f.value("deleted-execute")
        val stderr = "x".repeat(64 * 1024)
        value.getJSONObject("stderr").put("value", stderr).put("byte_length", stderr.length)
        val large = f.entry("received").copy(response = value.toString())
        // Use unique valid operation IDs in both outer and inner receipts.
        val records = (1..16).map { number ->
            val id = number.toString(16).padStart(32, '0')
            val response = org.json.JSONObject(large.response).put("operation_id", id)
            val out = response.getJSONObject("stdout"); val receipt = org.json.JSONObject(out.getString("value")).put("operation_id", id).toString()
            out.put("value", receipt).put("byte_length", receipt.toByteArray(Charsets.UTF_8).size)
            large.copy(id = id, response = response.toString())
        }
        assertFails { encodeSUMHTemporaryMapsOperations(records) }
        val directory = temporary.newFolder(); val before = listOf(f.entry()); store(directory).write(before)
        val bytes = document(directory).readBytes(); assertFails { store(directory).write(records) }
        assertContentEquals(bytes, document(directory).readBytes())
    }
    @Test fun futureCorruptOversizedFilesArePreservedAndNeverReset() {
        val directory = temporary.newFolder(); val doc = document(directory)
        for (bytes in listOf(raw(emptyList(), 0x53544D32), byteArrayOf(1, 2, 3), ByteArray(1024 * 1024 + 1))) {
            doc.writeBytes(bytes); assertFails { store(directory).read() }; assertFails { store(directory).write(emptyList()) }
            assertContentEquals(bytes, doc.readBytes()); assertFalse(scratch(directory).exists())
        }
    }
    @Test fun missingFileIsEmptyAndNonregularTargetsCannotBeOverwritten() {
        val parent = temporary.newFolder(); val missing = File(parent, "new")
        assertTrue(store(missing).read().isEmpty())
        store(missing).write(listOf(f.entry())); assertEquals(listOf(f.entry()), store(missing).read())
        val notDirectory = temporary.newFile(); assertFails { store(notDirectory).read() }
        val directory = temporary.newFolder(); document(directory).mkdir()
        assertFails { store(directory).write(emptyList()) }; assertTrue(document(directory).isDirectory)
    }
    @Test fun parentSyncFailurePreservesPreviousCheckpointAndIsRetried() {
        val parent = temporary.newFolder(); val directory = File(parent, "new")
        var calls = 0
        val failing = SUMHTemporaryMapsOperationFile(directory) { calls++; throw IOException("sync not confirmed") }
        assertFails { failing.write(listOf(f.entry())) }; assertTrue(directory.isDirectory); assertFalse(document(directory).exists())
        assertFails { failing.write(listOf(f.entry())) }; assertEquals(2, calls)
        store(directory).write(listOf(f.entry())); val before = document(directory).readBytes()
        assertFails { failing.write(emptyList()) }; assertContentEquals(before, document(directory).readBytes())
    }
    @Test fun landedReplacementWithDirectorySyncFailureIsStillUnconfirmed() {
        val directory = temporary.newFolder(); store(directory).write(listOf(f.entry()))
        val changed = listOf(f.entry("dispatching"))
        val failing = SUMHTemporaryMapsOperationFile(directory) { if (it == directory) throw IOException("landed but unconfirmed") }
        assertFails { failing.write(changed) }; assertEquals(changed, store(directory).read())
        assertFalse(scratch(directory).exists())
    }
    @Test fun sourceReplacementDuringWriteAndReadbackMismatchAreDetected() {
        val directory = temporary.newFolder(); store(directory).write(listOf(f.entry()))
        val future = raw(emptyList(), 0x53544D32)
        val changedDuring = SUMHTemporaryMapsOperationFile(directory) { if (it == directory.parentFile) document(directory).writeBytes(future) }
        assertFails { changedDuring.write(emptyList()) }; assertContentEquals(future, document(directory).readBytes())
        document(directory).writeBytes(encodeSUMHTemporaryMapsOperations(listOf(f.entry())))
        val badReadback = SUMHTemporaryMapsOperationFile(directory) { if (it == directory) document(directory).writeBytes(future) }
        assertFails { badReadback.write(emptyList()) }; assertContentEquals(future, document(directory).readBytes())
    }
    @Test fun staleHardLinkedScratchIsUnlinkedWithoutTruncatingItsOtherName() {
        val directory = temporary.newFolder(); val other = temporary.newFile().apply { writeText("keep") }
        try { Files.createLink(scratch(directory).toPath(), other.toPath()) } catch (error: Exception) { assumeNoException(error) }
        store(directory).write(listOf(f.entry())); assertEquals("keep", other.readText()); assertFalse(scratch(directory).exists())
    }
    @Test fun symlinkDocumentAndScratchFailClosedWhenHostPermitsLinks() {
        val directory = temporary.newFolder(); val other = temporary.newFile().apply { writeText("keep") }
        try { Files.createSymbolicLink(document(directory).toPath(), other.toPath()) } catch (error: Exception) { assumeNoException(error) }
        assertFails { store(directory).read() }; assertFails { store(directory).write(emptyList()) }; assertEquals("keep", other.readText())
        Files.delete(document(directory).toPath()); Files.createSymbolicLink(scratch(directory).toPath(), other.toPath())
        assertFails { store(directory).write(emptyList()) }; assertEquals("keep", other.readText())
    }
    @Test fun twoStoreInstancesSerializeAndExposeWholeDocuments() {
        val directory = temporary.newFolder(); val first = store(directory); val second = store(directory)
        val gate = CountDownLatch(1); val pool = Executors.newFixedThreadPool(2)
        try {
            val a = pool.submit { gate.await(); repeat(10) { first.write(listOf(f.entry("prepared"))) } }
            val b = pool.submit { gate.await(); repeat(10) { second.write(listOf(f.entry("dispatching"))) } }
            gate.countDown(); a.get(10, TimeUnit.SECONDS); b.get(10, TimeUnit.SECONDS)
            assertTrue(first.read().single().stage in setOf("prepared", "dispatching")); assertEquals(first.read(), second.read())
        } finally { pool.shutdownNow() }
    }
}
