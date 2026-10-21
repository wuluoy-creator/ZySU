package com.zying.zysu.ui.sumh

import android.app.Application
import com.zying.zysu.ui.sumh.util.*
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHHideDefinitionDraftTest {
    private val revision = "sha256-v1:" + "a".repeat(64)
    private val next = "sha256-v1:" + "b".repeat(64)
    private val row = HideDefinition("1".repeat(32), "/目标 ", false, "名字", "备注\n", "分组 ", false, null, "disabled")

    @Test fun stableIdMetadataAndLiteralTextRoundTripWithOtherPrivateDrafts() {
        val draft = HideDefinitionDraft.initial(row, revision)
        val other = MapsEditorDraft("/a", "/b", MapsEditorDraft.baseline(null)).record()
        val all = mapOf(draft.key to draft.record(), "maps:/a" to other)
        assertEquals(all, decodeEditorDrafts(encodeEditorDrafts(all)))
        assertEquals(draft, HideDefinitionDraft.from(draft.record()))
        assertEquals(listOf("hide", "annotate", row.id, revision), draft.arguments().take(4))
        val fields = JSONObject(draft.arguments().last())
        assertFalse(fields.has("path")); assertFalse(fields.has("enabled"))
        assertEquals(row.note, fields.getString("note")); assertEquals(row.group, fields.getString("group"))
    }

    @Test fun newDisabledDefinitionHasExplicitEnabledAndImmutableRevision() {
        val draft = HideDefinitionDraft.initial(null, revision).copy(path = row.path, enabled = false)
        assertNull(draft.error()); assertEquals(listOf("hide", "create", revision), draft.arguments().take(3))
        assertFalse(JSONObject(draft.arguments().last()).getBoolean("enabled"))
        assertEquals(row.path, JSONObject(draft.arguments().last()).getString("path"))
    }

    @Test fun invalidOversizedFieldsRemainDraftsButCannotSubmit() {
        val draft = HideDefinitionDraft.initial(row, revision).copy(name = "中".repeat(43))
        assertTrue(HideDefinitionDraft.valid(draft.key, draft.record()))
        assertEquals("name", draft.error())
        assertNull(draft.copy(name = "中".repeat(42)).error())
        assertEquals("note", draft.copy(name = "", note = "x".repeat(1025)).error())
        assertEquals("group", draft.copy(name = "", group = "x".repeat(129)).error())
        assertFalse(HideDefinitionDraft.valid(draft.key, draft.copy(note = "\u0000").record()))
        assertFalse(HideDefinitionDraft.valid(draft.key, draft.copy(note = "\uD800").record()))
        assertFalse(HideDefinitionDraft.valid("hide-definition:new", draft.record()))
        assertFalse(HideDefinitionDraft.valid(draft.key, draft.copy(revision = "future").record()))
    }

    @Test fun explicitRebasePreservesInputOrUsesSavedValues() {
        val draft = HideDefinitionDraft.initial(row, revision).copy(name = "my input", note = "unfinished")
        val current = row.copy(name = "external", enabled = true, group = "new group")
        val kept = draft.rebased(current, next, true)
        assertEquals("my input", kept.name); assertEquals("unfinished", kept.note)
        assertEquals(row.id, kept.id); assertEquals(row.path, kept.path)
        assertTrue(kept.enabled); assertEquals(next, kept.revision)
        assertEquals(HideDefinitionDraft.baseline(current), kept.baseline)
        assertEquals(HideDefinitionDraft.initial(current, next), draft.rebased(current, next, false))
    }

    @Test fun filterSearchesMetadataKeepsWhitespaceAndUsesExactGroups() {
        val rows = listOf(row, row.copy(id = "2".repeat(32), path = "/other", note = "", group = "分组", enabled = true, state = "binding_error"))
        assertEquals(listOf(row), filterHideDefinitions(rows, "备注", null, HideDefinitionFilter.All))
        assertEquals(listOf(row), filterHideDefinitions(rows, "", "分组 ", HideDefinitionFilter.All))
        assertEquals(listOf(row), filterHideDefinitions(rows, "", null, HideDefinitionFilter.Disabled))
        assertEquals(listOf(rows[1]), filterHideDefinitions(rows, "", null, HideDefinitionFilter.Attention))
        assertTrue(filterHideDefinitions(rows, " 备注", null, HideDefinitionFilter.All).isEmpty())
    }

    @Test fun confirmationPinsReviewedRevisionAndBoot() {
        val sample = HideDefinitionCollection(revision, "boot", listOf(row), true, true)
        val reviewed = HideDefinitionConfirmation("disable-group", listOf(row.id), sample, row.group)
        assertTrue(reviewed.isCurrent(sample))
        assertFalse(reviewed.isCurrent(sample.copy(revision = next)))
        assertFalse(reviewed.isCurrent(sample.copy(bootId = "rebooted")))
        assertFalse(reviewed.isCurrent(sample.copy(complete = false)))
        assertFalse(reviewed.isCurrent(sample.copy(error = "read failed")))
    }
}
