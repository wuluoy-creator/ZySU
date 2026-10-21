package com.zying.zysu.ui.sumh.util

import android.app.Application
import com.zying.zysu.ui.sumh.*
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter.MapsPathRule
import org.json.JSONObject
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.ByteArrayInputStream
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHMapsTransferTest {
    @Test fun roundTripPreservesUnicodeAndTrailingSpacesAndExcludesRuntimeState() {
        val definitions = listOf(MapsPathRule("/数据/😀 ", "/system/名字 "), MapsPathRule("/tabs\t", "/tab\t"))
        val text = exportSUMHMapsDefinitions(definitions)
        assertEquals(definitions, importSUMHMapsDefinitions(text))
        assertEquals(setOf("schema_version", "kind", "rules"), JSONObject(text).keys().asSequence().toSet())
        assertFalse(text.contains("ino")); assertFalse(text.contains("enabled"))
    }
    @Test fun unknownFormatsFieldsDuplicateKeysAndRuntimeIdentitiesAreRejected() {
        val original = exportSUMHMapsDefinitions(listOf(MapsPathRule("/a", "/b")))
        for (bad in listOf(
            JSONObject(original).put("schema_version", 2).toString(),
            JSONObject(original).put("schema_version", "1").toString(),
            JSONObject(original).put("inode", "9").toString(),
            JSONObject(original).put("kind", "temporary_maps").toString(),
            "{\"schema_version\":1,\"schema_version\":1,\"kind\":\"sumh_managed_maps\",\"rules\":[]}",
            original + "{}")) assertFails { importSUMHMapsDefinitions(bad) }
        val nested = JSONObject(original)
        nested.getJSONArray("rules").getJSONObject(0).put("target_ino", "4")
        assertFails { importSUMHMapsDefinitions(nested.toString()) }
    }
    @Test fun duplicateTargetsControlsMalformedUnicodeAndLimitsFailWithoutPartialDefinitions() {
        for (rules in listOf(listOf(MapsPathRule("/same", "/a"), MapsPathRule("/same", "/b")),
            listOf(MapsPathRule("relative", "/a")), listOf(MapsPathRule("/newline\n", "/a")),
            listOf(MapsPathRule("/nul\u0000", "/a")), listOf(MapsPathRule("/\ud800", "/a")),
            List(257) { MapsPathRule("/target/$it", "/r") },
            List(256) { MapsPathRule("/" + "a".repeat(300) + it, "/r") })) assertFails { exportSUMHMapsDefinitions(rules) }
    }
    @Test fun maximumRuleCountAndEncodedBudgetUseExistingBoundaries() {
        val accepted = List(256) { MapsPathRule("/target/$it", "/r") }
        assertEquals(accepted, importSUMHMapsDefinitions(exportSUMHMapsDefinitions(accepted)))
        assertFails { importSUMHMapsDefinitions(" ".repeat(SUMH_MAPS_TRANSFER_BYTES) + "{}") }
    }
    @Test fun mergeRequiresExplicitConflictChoiceAndPreservesExistingOrder() {
        val saved = listOf(MapsPathRule("/a", "/old"), MapsPathRule("/keep", "/keep"))
        val imported = listOf(MapsPathRule("/new", "/new"), MapsPathRule("/a", "/new-a"))
        val unresolved = reviewSUMHMapsImport(saved, imported, SUMHMapsImportMode.Merge, SUMHMapsConflictChoice.Unresolved)
        assertTrue(unresolved.unresolved); assertEquals(1, unresolved.conflicts.size)
        val keep = reviewSUMHMapsImport(saved, imported, SUMHMapsImportMode.Merge, SUMHMapsConflictChoice.KeepSaved)
        assertEquals(saved + imported[0], keep.candidate); assertTrue(keep.changed.isEmpty())
        val replace = reviewSUMHMapsImport(saved, imported, SUMHMapsImportMode.Merge, SUMHMapsConflictChoice.UseImported)
        assertEquals(listOf(imported[1], saved[1], imported[0]), replace.candidate)
        assertEquals(1, replace.changed.size); assertTrue(replace.removed.isEmpty())
    }
    @Test fun replaceReviewsRemovalsAndEmptyImportIsExplicitClear() {
        val saved = listOf(MapsPathRule("/keep", "/r"), MapsPathRule("/remove", "/r"))
        val review = reviewSUMHMapsImport(saved, listOf(saved[0]), SUMHMapsImportMode.Replace, SUMHMapsConflictChoice.Unresolved)
        assertEquals(listOf(saved[1]), review.removed); assertFalse(review.unresolved)
        assertEquals(saved, reviewSUMHMapsImport(saved, emptyList(), SUMHMapsImportMode.Replace, SUMHMapsConflictChoice.Unresolved).removed)
        assertEquals(saved, reviewSUMHMapsImport(saved, emptyList(), SUMHMapsImportMode.Merge, SUMHMapsConflictChoice.Unresolved).candidate)
    }
    @Test fun mergeCapacityIsCheckedAfterCombiningBothCollections() {
        val saved = List(256) { MapsPathRule("/target/$it", "/r") }
        val review = reviewSUMHMapsImport(saved, listOf(MapsPathRule("/new", "/r")), SUMHMapsImportMode.Merge, SUMHMapsConflictChoice.Unresolved)
        assertFalse(review.withinBudget)
    }
    @Test fun fileReaderBoundsBytesAndRejectsMalformedUtf8() {
        val text = exportSUMHMapsDefinitions(listOf(MapsPathRule("/😀 ", "/数据 ")))
        assertEquals(text, readSUMHMapsDocument(ByteArrayInputStream(text.toByteArray())))
        assertFails { readSUMHMapsDocument(ByteArrayInputStream(byteArrayOf(0xff.toByte()))) }
        assertFails { readSUMHMapsDocument(ByteArrayInputStream(ByteArray(SUMH_MAPS_TRANSFER_BYTES + 1))) }
    }
    @Test fun privateDraftRoundTripPreservesLargeInputWithoutSplittingSurrogates() {
        val draft = MapsTransferDraft("😀".repeat(10_000), "a".repeat(64), SUMHMapsImportMode.Replace,
            SUMHMapsConflictChoice.UseImported, "b".repeat(64), "c".repeat(64))
        val snapshot = mapOf(MAPS_TRANSFER_DRAFT_KEY to draft.record())
        assertTrue(editorDraftWithinBudget(snapshot))
        assertEquals(draft, MapsTransferDraft.from(decodeEditorDrafts(encodeEditorDrafts(snapshot)).getValue(MAPS_TRANSFER_DRAFT_KEY)))
        assertFails { draft.copy(text = "a".repeat(SUMH_MAPS_TRANSFER_BYTES + 1)).record() }
        assertFalse(MapsTransferDraft.valid(EditorDraftRecord(listOf("a".repeat(64), "FutureMode", "Unresolved", "", ""))))
    }
}
