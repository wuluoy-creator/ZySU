package com.zying.zysu.ui.sumh

import com.zying.zysu.ui.sumh.util.SUMHManager.ModuleRule
import org.junit.Assert.*
import org.junit.Test
import java.io.File
import java.io.IOException
import java.nio.file.Files

class SUMHEditorDraftStorageTest {
    private fun record(path: String = "/data/中文 ") = MapsEditorDraft(path, "/replacement ", MapsEditorDraft.baseline(null)).record()

    @Test fun exactUtf8PathsAndAllMaximumRuleListsRoundTrip() {
        val rules = List(256) { ModuleRule("/system/" + "a".repeat(150) + it, "sumh") }
        val module = ModuleEditorDraft("sumh", rules, newPath = "/data/中文 ", baseline = ModuleEditorDraft.baseline("sumh", rules)).record()
        val snapshot = mapOf("module:example" to module, "maps:/original " to record())
        assertEquals(snapshot, decodeEditorDrafts(encodeEditorDrafts(snapshot)))
        val dir = Files.createTempDirectory("sumh-editor-test").toFile()
        try {
            val file = EditorDraftFile(dir)
            assertTrue(file.read().isEmpty())
            file.write(snapshot)
            assertEquals(snapshot, EditorDraftFile(dir).read())
            assertEquals(listOf("editors-v1.bin"), dir.list()!!.toList())
        } finally { dir.deleteRecursively() }
    }

    @Test fun globalCountBytesAndFieldBudgetsRejectWholeUpdates() {
        assertTrue(editorDraftWithinBudget((0 until EDITOR_DRAFT_COUNT).associate { "maps:$it" to record() }))
        assertFalse(editorDraftWithinBudget((0..EDITOR_DRAFT_COUNT).associate { "maps:$it" to record() }))
        assertFalse(editorDraftWithinBudget(mapOf("maps:" to record("x".repeat(4096)))))
        assertFalse(editorDraftWithinBudget(mapOf("maps:" to record("中".repeat(1366)))))
        val rules = List(256) { ModuleRule("/" + "x".repeat(200) + it, "sumh") }
        val module = ModuleEditorDraft("sumh", rules, baseline = ModuleEditorDraft.baseline("sumh", rules)).record()
        val many = (0..100).associate { "module:$it" to module }
        assertFalse(editorDraftWithinBudget(many))
        assertThrows(IllegalArgumentException::class.java) { encodeEditorDrafts(many) }
    }

    @Test fun futureCorruptOversizedAndInvalidUtf8DocumentsAreRejectedWithoutRewrite() {
        val valid = encodeEditorDrafts(mapOf("maps:" to record()))
        val dir = Files.createTempDirectory("sumh-editor-corrupt").toFile()
        try {
            val file = File(dir, "editors-v1.bin")
            val variants = listOf(valid.copyOf(6), valid + byteArrayOf(0), valid.copyOf().also { it[3] = 99 },
                ByteArray(EDITOR_DRAFT_BYTES + 1), valid.copyOf().also { it[16] = 0xff.toByte() })
            variants.forEach { bytes ->
                file.writeBytes(bytes)
                assertThrows(IOException::class.java) { EditorDraftFile(dir).read() }
                assertArrayEquals(bytes, file.readBytes())
            }
        } finally { dir.deleteRecursively() }
    }

    @Test fun failedReplacementKeepsOldDocument() {
        val dir = Files.createTempDirectory("sumh-editor-write").toFile()
        try {
            val store = EditorDraftFile(dir)
            val old = mapOf("maps:" to record("/old")); store.write(old)
            val slot = File(dir, "editors-v1.tmp"); slot.mkdir(); File(slot, "block").writeText("x")
            assertThrows(IOException::class.java) { store.write(mapOf("maps:" to record("/new"))) }
            assertEquals(old, store.read())
        } finally { dir.deleteRecursively() }
    }
}
