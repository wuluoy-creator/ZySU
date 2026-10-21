package com.zying.zysu.ui.sumh.util

import android.app.Application
import com.zying.zysu.ui.sumh.EditorDraftRecord
import com.zying.zysu.ui.sumh.PartitionsDraft
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHPartitionsTest {
    private val f = SUMHPartitionsFixtures
    @Test fun knownNamesAndDisabledSourceRemainIndependentFacts() {
        val value = f.sample()
        assertTrue(value.selectionValid); assertEquals(listOf("old_unknown"), value.saved)
        assertTrue(value.candidates.single().sources.single().disabled)
        assertTrue(value.candidates.single().applicable)
    }
    @Test fun futureDuplicateAndWrongTypesCannotAuthorizeSelection() {
        for (raw in listOf(f.discovery().put("schema_version", 2), f.discovery().put("read_only", false),
            f.discovery().put("complete", false), f.discovery().put("stable", false), f.discovery().put("config_revision", JSONObject.NULL),
            f.discovery().put("namespace_ino", "0"), f.discovery().put("config_status", "missing"))) {
            assertFails { SUMHPartitionsParser.discovery(raw.toString()) }
        }
        assertFails { SUMHPartitionsParser.discovery("{\"schema_version\":1,\"schema_version\":1}") }
        assertFails { SUMHPartitionsParser.discovery(f.discovery().toString() + "{}") }
    }
    @Test fun partialUnknownAndStalePreserveSavedNamesWithoutAuthority() {
        for (status in listOf("partial", "unknown", "stale")) {
            val raw = f.discovery().put("status", status).put("selection_valid", false).put("complete", false).put("stable", status != "stale")
            val value = SUMHPartitionsParser.discovery(raw.toString())
            assertEquals(listOf("old_unknown"), value.saved); assertFalse(value.selectionValid)
        }
    }
    @Test fun unsafeNamesAndCandidateSavedMismatchFailClosed() {
        for (name in listOf("", "../part", "part ", "中文", "a\n", "a".repeat(256))) {
            assertFails { partitionsSaveJson(listOf(name)) }
            assertFails { SUMHPartitionsParser.discovery(f.discovery(listOf(name)).toString()) }
        }
        val raw = f.discovery(); raw.getJSONArray("candidates").getJSONObject(0).put("saved", true)
        assertFails { SUMHPartitionsParser.discovery(raw.toString()) }
        assertFails { partitionsSaveJson(listOf("x", "x")) }
        assertFails { partitionsSaveJson(List(129) { "p$it" }) }
        assertEquals(128, JSONArray(partitionsSaveJson(List(128) { "p$it" })).length())
    }
    @Test fun durableSavedAndNoWriteUnchangedAreDistinctFromPartial() {
        assertTrue(SUMHPartitionsParser.save(f.save().toString()).saved)
        assertTrue(SUMHPartitionsParser.save(f.save("unchanged").toString()).saved)
        assertFalse(SUMHPartitionsParser.save(f.save("partial").toString()).saved)
        assertFails { SUMHPartitionsParser.save(f.save().put("durability_confirmed", false).toString()) }
        assertFails { SUMHPartitionsParser.save(f.save().put("readback_confirmed", false).toString()) }
        assertFails { SUMHPartitionsParser.save(f.save().put("applied", true).toString()) }
        assertFails { SUMHPartitionsParser.save(f.save("partial").put("saved", true).toString()) }
    }
    @Test fun transportErrorsNeverBecomeConfirmedSuccess() {
        val result = SUMHPartitionsParser.saveReply(SUMHQueryReply(f.save().toString(), "timeout"))
        assertNotNull(result.value); assertEquals("timeout", result.error)
        assertNull(SUMHPartitionsParser.discoveryReply(SUMHQueryReply("broken", "offline")).value)
    }
    @Test fun draftRoundTripRebaseAndEligibilityKeepOnlyExplicitChanges() {
        val sample = f.sample()
        val draft = PartitionsDraft(f.revision, sample.saved, listOf("extra"))
        assertTrue(PartitionsDraft.valid(draft.record())); assertEquals(draft, PartitionsDraft.from(draft.record()))
        assertTrue(draft.canSave(sample))
        val updated = sample.copy(revision = f.nextRevision, saved = listOf("old_unknown", "external"))
        assertFalse(draft.canSave(updated))
        val rebased = draft.rebase(updated)
        assertEquals(listOf("external", "extra"), rebased.selected)
        assertTrue(rebased.canSave(updated))
        assertFalse(draft.canSave(sample.copy(candidates = emptyList())))
        assertFalse(PartitionsDraft.valid(EditorDraftRecord(listOf("bad", "0"))))
        assertFalse(PartitionsDraft.valid(EditorDraftRecord(listOf(f.revision, "129"))))
    }
    @Test fun unknownOutcomeRequiresFreshSampleAndNeverErasesDraft() {
        val draft = PartitionsDraft(f.revision, listOf("old_unknown"), listOf("extra"))
        assertFalse(draft.canSave(f.sample().copy(selectionValid = false)))
        assertEquals(listOf("extra"), draft.selected)
        assertFalse(draft.copy(selected = listOf("old_unknown")).canSave(f.sample()))
    }
}
