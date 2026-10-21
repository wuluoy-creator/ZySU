package com.zying.zysu.ui.sumh

import com.zying.zysu.ui.sumh.util.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.IOException
import kotlin.test.*

class SUMHProfileStorageTest {
    @get:Rule val temporary = TemporaryFolder()
    private fun directory() = File(temporary.root, "profiles")
    private fun hint() = SUMHProfilePending("capture", "known-id", null, ProfileFixtures.bank, null, "b".repeat(64))
    @Test fun exactDraftRoundtripAndIndependentExportSlot() {
        val directory = directory(); val store = SUMHProfileStorage(directory) { }
        val text = " \n" + ProfileFixtures.document().text + " \r\n"
        val draft = store.saveDraft(SUMHProfileDraft(text, setOf(SUMHProfileResource.Maps)), null)
        assertEquals(text, store.readDraft()!!.document)
        val document = ProfileFixtures.document(); val export = store.saveExport(document, "example", SUMHProfilesParser.revision(document))
        assertEquals(export, SUMHProfileStorage(directory) { }.readExport(export.token))
        assertEquals(draft, store.readDraft())
        assertFails { store.readExport("0".repeat(32)) }
    }
    @Test fun draftCasPreventsLostEdits() {
        val store = SUMHProfileStorage(directory()) { }; val first = store.saveDraft(SUMHProfileDraft("first"), null)
        val next = store.saveDraft(first.copy(document = "next"), first.fingerprint)
        assertFails { store.saveDraft(first.copy(document = "stale"), first.fingerprint) }
        assertEquals(next, store.readDraft())
    }
    @Test fun unknownDraftAndCheckpointFormatsArePreserved() {
        val directory = temporary.newFolder("profiles"); val future = byteArrayOf(0x53, 0x50, 0x44, 0x32)
        File(directory, "draft-v1.bin").writeBytes(future)
        val store = SUMHProfileStorage(directory) { }
        assertFails { store.saveDraft(SUMHProfileDraft("replace"), null) }
        assertContentEquals(future, File(directory, "draft-v1.bin").readBytes())
        File(directory, "pending-v1.bin").writeBytes(future)
        assertFails { store.comparePending(null, hint()) }
        assertContentEquals(future, File(directory, "pending-v1.bin").readBytes())
    }
    @Test fun checkpointCarriesIdentityAndFingerprintOnly() {
        val store = SUMHProfileStorage(directory()) { }; val hint = hint()
        assertTrue(store.comparePending(null, hint)); assertFalse(store.comparePending(null, hint.copy(id = "other")))
        assertTrue(encodeSUMHProfilePending(hint).size < 1024)
        assertEquals(hint, store.readPending())
        assertTrue(store.comparePending(hint, hint.copy(phase = "dispatching")))
        assertTrue(store.comparePending(hint.copy(phase = "dispatching"), null)); assertNull(store.readPending())
    }
    @Test fun afterRenameSyncFailureLeavesRecoverableCheckpoint() {
        val directory = directory(); var fail = true
        val store = SUMHProfileStorage(directory) { if (it == directory && fail) throw IOException("sync") }
        assertFails { store.comparePending(null, hint()) }
        assertEquals(hint(), store.readPending())
        fail = false
        assertTrue(store.comparePending(hint(), null)); assertNull(store.readPending())
    }
    @Test fun parentSyncIsRetriedAfterUnconfirmedDirectoryCreation() {
        var syncCalls = 0; val store = SUMHProfileStorage(directory()) { if (++syncCalls == 1) throw IOException("parent sync") }
        assertFails { store.comparePending(null, hint()) }
        assertNull(store.readPending()); assertTrue(store.comparePending(null, hint())); assertTrue(syncCalls >= 3)
    }
    @Test fun corruptExportIsNotOverwrittenAndWrongRevisionRejected() {
        val directory = temporary.newFolder("profiles"); val store = SUMHProfileStorage(directory) { }
        File(directory, "export-v1.bin").writeBytes(byteArrayOf(1, 2, 3))
        val document = ProfileFixtures.document()
        assertFails { store.saveExport(document, "example", SUMHProfilesParser.revision(document)) }
        assertContentEquals(byteArrayOf(1, 2, 3), File(directory, "export-v1.bin").readBytes())
        assertFails { encodeSUMHProfileExport(SUMHProfileExport("a".repeat(32), document.text, "example", ProfileFixtures.bank)) }
    }
    @Test fun malformedAndOversizedBinaryInputsFailClosed() {
        assertFails { decodeSUMHProfileDraft(encodeSUMHProfileDraft(SUMHProfileDraft()) + byteArrayOf(0)) }
        assertFails { decodeSUMHProfilePending(ByteArray(1025)) }
        assertFails { encodeSUMHProfileDraft(SUMHProfileDraft("x".repeat(SUMH_PROFILE_BYTES + 1))) }
        assertFails { encodeSUMHProfileDraft(SUMHProfileDraft("\uD800")) }
    }
    @Test fun nonRegularSlotsAreRefused() {
        val directory = temporary.newFolder("profiles"); File(directory, "pending-v1.bin").mkdir()
        val store = SUMHProfileStorage(directory) { }
        assertFails { store.readPending() }; assertFails { store.comparePending(null, hint()) }
    }
}
