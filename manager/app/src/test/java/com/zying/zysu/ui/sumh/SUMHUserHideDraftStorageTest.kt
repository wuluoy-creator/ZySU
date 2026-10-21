package com.zying.zysu.ui.sumh

import android.app.Application
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.ByteArrayInputStream
import java.io.File
import java.io.IOException

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHUserHideDraftStorageTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test fun fileRestoresExactLargeDraftsAndRetryPathsInANewInstance() {
        val directory = File(temporary.root, "drafts")
        val value = UserHideDraftSnapshot("/data/中文\n\t ", "/data/a \ninvalid\n/data/中文",
            (0 until 180).associate { "/data/$it" to ("/" + "文".repeat(1300) + " ") }, listOf("/data/a ", "/data/中文"))
        assertNull(userHideDraftLimit(value))
        assertTrue(encodeUserHideDraft(value).size > 512 * 1024)
        UserHideDraftFile(directory).write(value)
        assertEquals(value, UserHideDraftFile(directory).read())
        val updated = value.copy(bulk = "", retryPaths = emptyList())
        UserHideDraftFile(directory).write(updated)
        assertEquals(updated, UserHideDraftFile(directory).read())
    }

    @Test fun missingAndUnreadableStorageAreDifferent() {
        val directory = File(temporary.root, "drafts")
        assertEquals(UserHideDraftSnapshot(), UserHideDraftFile(directory).read())
        directory.writeText("not a directory")
        assertThrows(IOException::class.java) { UserHideDraftFile(directory).read() }
        assertThrows(IOException::class.java) { UserHideDraftFile(directory).write(UserHideDraftSnapshot()) }
        assertEquals("not a directory", directory.readText())
    }

    @Test fun interruptedTemporaryFileIsNeverLoadedAndUsesOneBoundedSlot() {
        val directory = temporary.newFolder()
        val store = UserHideDraftFile(directory)
        val original = UserHideDraftSnapshot(add = "/original ")
        store.write(original)
        val interrupted = File(directory, "$USER_HIDE_DRAFT_ID.tmp")
        interrupted.writeBytes(encodeUserHideDraft(UserHideDraftSnapshot(add = "/uncommitted")))
        assertEquals(original, UserHideDraftFile(directory).read())
        store.write(original.copy(add = "/new "))
        assertEquals(listOf("$USER_HIDE_DRAFT_ID.bin"), directory.list()!!.toList())
        assertEquals("/new ", UserHideDraftFile(directory).read().add)
    }

    @Test fun corruptFutureAndOversizedDocumentsAreNotTreatedAsEmpty() {
        val encoded = encodeUserHideDraft(UserHideDraftSnapshot(add = "/data/exact "))
        val inputs = listOf(encoded.copyOf(2), encoded.copyOf(encoded.size - 1), encoded + byteArrayOf(0),
            encoded.copyOf().apply { this[3] = '2'.code.toByte() }, ByteArray(USER_HIDE_DRAFT_BYTES + 1))
        inputs.forEach { assertThrows(IOException::class.java) { decodeUserHideDraft(it) } }
        val directory = temporary.newFolder()
        val file = File(directory, "$USER_HIDE_DRAFT_ID.bin")
        file.writeBytes(inputs[3])
        assertThrows(IOException::class.java) { UserHideDraftFile(directory).read() }
        assertArrayEquals(inputs[3], file.readBytes())
    }

    @Test fun budgetsCountUtf8BytesAllEditsAndRetryPaths() {
        assertNull(userHideDraftLimit(UserHideDraftSnapshot(add = "/" + "文".repeat(1364) + "ab"))) // 4095 bytes
        assertEquals(UserHideDraftLimit.Line, userHideDraftLimit(UserHideDraftSnapshot(add = "/" + "文".repeat(1365))))
        assertEquals(UserHideDraftLimit.Bulk, userHideDraftLimit(UserHideDraftSnapshot(bulk = "/a\n".repeat(1024))))
        assertEquals(UserHideDraftLimit.Bulk, userHideDraftLimit(UserHideDraftSnapshot(bulk = ("x".repeat(1023) + "\n").repeat(257))))
        val edits = (0 until 260).associate { "/data/$it" to "/${"x".repeat(4094)}" }
        assertEquals(UserHideDraftLimit.Total, userHideDraftLimit(UserHideDraftSnapshot(edits = edits)))
        assertEquals(UserHideDraftLimit.Total, userHideDraftLimit(UserHideDraftSnapshot(edits = (0..1024).associate { "/$it" to "/a" })))
        assertEquals(UserHideDraftLimit.Total, userHideDraftLimit(UserHideDraftSnapshot(retryPaths = (0..1024).map { "/$it" })))
    }

    @Test fun fileChunksBoundMemoryAndKeepUnicodeSpacesAndLineOrder() {
        val lines = (0..700).map { "/data/中文 $it " }
        SUMHHideFileChunks(ByteArrayInputStream(lines.joinToString("\r\n").toByteArray())).use { chunks ->
            val restored = mutableListOf<String>()
            while (chunks.hasMore) {
                val chunk = chunks.next()
                assertTrue(chunk.toByteArray().size <= 64 * 1024)
                assertTrue(chunk.lines().size <= 256)
                restored += chunk.lines()
            }
            assertEquals(lines, restored)
        }
        val longLines = (0..50).map { "/$it/" + "文".repeat(1300) }
        SUMHHideFileChunks(ByteArrayInputStream(longLines.joinToString("\n").toByteArray())).use { chunks ->
            val restored = mutableListOf<String>()
            while (chunks.hasMore) chunks.next().let { assertTrue(it.toByteArray().size <= 64 * 1024); restored += it.lines() }
            assertEquals(longLines, restored)
        }
    }

    @Test fun acceptedNearBudgetBulkAlwaysHasRoomForAllFailedRetryIdentities() {
        val paths = (0 until 50).map { "/data/$it/" + "x".repeat(3900) }
        val edits = (0 until 160).associate { "/edit/$it" to "/${"x".repeat(3900)}" }
        val accepted = UserHideDraftSnapshot(bulk = paths.joinToString("\n"), edits = edits)
        assertNull(userHideDraftLimit(accepted))
        val failed = accepted.copy(retryPaths = paths)
        assertNull(userHideDraftLimit(failed))
        val directory = temporary.newFolder()
        UserHideDraftFile(directory).write(failed)
        assertEquals(failed, UserHideDraftFile(directory).read())
        assertEquals(UserHideDraftLimit.Total, userHideDraftLimit(accepted.copy(edits =
            (0 until 200).associate { "/edit/$it" to "/${"x".repeat(3900)}" })))
    }

    @Test fun malformedUtf8AndHugeSingleLinesFailWithoutTruncation() {
        for (bytes in listOf(byteArrayOf(0xc0.toByte(), 0x80.toByte()), "/${"x".repeat(500_000)}".toByteArray(),
            "/${"文".repeat(2000)}".toByteArray())) {
            SUMHHideFileChunks(ByteArrayInputStream(bytes)).use { chunks -> assertThrows(IOException::class.java) { chunks.next() } }
        }
    }
}
