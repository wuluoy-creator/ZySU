package com.zying.zysu.ui.sumh

import android.app.Application
import com.zying.zysu.ui.sumh.util.*
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

/** Host filesystem/fault-model tests. Injected sync callbacks do not claim real-device durability. */
@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHHideTransferStorageTest {
    @get:Rule val temporary = TemporaryFolder()
    private val revision = "sha256-v1:" + "a".repeat(64)
    private fun id(number: Int) = number.toString(16).padStart(32, '0')
    private fun definition(number: Int = 1) = SUMHHidePortableDefinition(id(number), "/路径/😀\n\r\t $number ", true,
        "名字 ", "备注\n\r\t ", "组 ")
    private fun portable() = exportSUMHHideDefinitions(listOf(definition()))
    private fun draft() = SUMHHideTransferDraft(portable(), mapOf(id(1) to SUMHHideTransferChoice(
        SUMHHideTransferResolution.ExplicitNewIdentity, id(2))), true, revision, "boot-😀 ")
    private fun store(directory: File) = SUMHHideTransferStorage(directory) { }
    private fun draftFile(directory: File) = File(directory, "hide-transfer-v1.bin")
    private fun exportFile(directory: File) = File(directory, "hide-export-v1.bin")
    private fun scratch(directory: File) = File(directory, "hide-transfer-v1.tmp")
    private fun rawDraft(document: ByteArray = byteArrayOf(), preserve: Int = 0,
        choices: List<Triple<String, String, String?>> = emptyList(), magic: Int = 0x53485431): ByteArray =
        ByteArrayOutputStream().also { buffer -> DataOutputStream(buffer).use { out ->
            fun text(bytes: ByteArray) { out.writeInt(bytes.size); out.write(bytes) }
            fun text(value: String) = text(value.toByteArray(Charsets.UTF_8))
            out.writeInt(magic); text(document); out.writeByte(preserve); out.writeByte(0); out.writeByte(0)
            out.writeInt(choices.size)
            choices.forEach { (id, resolution, newId) ->
                text(id); text(resolution); out.writeBoolean(newId != null); newId?.let(::text)
            }
        } }.toByteArray()

    @Test fun draftRoundTripPreservesOriginalIntentAndCanonicalChoiceFingerprint() {
        val original = draft().copy(choices = linkedMapOf(id(2) to SUMHHideTransferChoice(SUMHHideTransferResolution.Skip),
            id(1) to draft().choices.getValue(id(1))))
        val restored = decodeSUMHHideTransferDraft(encodeSUMHHideTransferDraft(original))
        assertEquals(original, restored)
        assertEquals(original.fingerprint, original.copy(choices = original.choices.toSortedMap()).fingerprint)
        assertNotEquals(original.fingerprint, original.copy(document = original.document + " ").fingerprint)
        assertNotEquals(original.fingerprint, original.copy(preserveImportedEnabled = false).fingerprint)
        assertNotEquals(original.fingerprint, original.copy(baselineBootId = "other-boot").fingerprint)
        assertNotEquals(original.fingerprint, original.copy(choices = emptyMap()).fingerprint)
    }

    @Test fun incompleteInputIsInactiveAndCanBeRestoredWithoutParsingOrTrimming() {
        val directory = temporary.newFolder()
        val input = SUMHHideTransferDraft(" {\"rules\": [\n未完成\u0000 ")
        val saved = store(directory).saveDraft(input, null)
        assertEquals(input, saved)
        assertEquals(input, store(directory).readDraft())
        assertFails { importSUMHHideDefinitions(saved.document) }
    }

    @Test fun maximumRawDocumentAnd4096ChoicesFitDedicatedBudget() {
        val large = SUMHHideTransferDraft("x".repeat(SUMH_HIDE_TRANSFER_BYTES), (1..4096).associate { number ->
            id(number) to SUMHHideTransferChoice(SUMHHideTransferResolution.ExplicitNewIdentity, id(number + 4096))
        })
        val bytes = encodeSUMHHideTransferDraft(large)
        assertTrue(bytes.size < SUMH_HIDE_TRANSFER_STORAGE_BYTES)
        assertEquals(large, decodeSUMHHideTransferDraft(bytes))
        assertFails { encodeSUMHHideTransferDraft(large.copy(document = large.document + "x")) }
        assertFails { encodeSUMHHideTransferDraft(large.copy(choices = large.choices + (id(9000) to SUMHHideTransferChoice()))) }
    }

    @Test fun draftRejectsFutureTruncatedTrailingMalformedAndInvalidLengths() {
        val encoded = encodeSUMHHideTransferDraft(draft())
        for (count in 0 until encoded.size) assertFails("truncated at $count") { decodeSUMHHideTransferDraft(encoded.copyOf(count)) }
        assertFails { decodeSUMHHideTransferDraft(rawDraft(magic = 0x53485432)) }
        assertFails { decodeSUMHHideTransferDraft(encoded + byteArrayOf(0)) }
        assertFails { decodeSUMHHideTransferDraft(rawDraft(document = byteArrayOf(0xc0.toByte(), 0x80.toByte()))) }
        assertFails { decodeSUMHHideTransferDraft(rawDraft(preserve = 2)) }
        for (length in listOf(-1, SUMH_HIDE_TRANSFER_BYTES + 1, Int.MAX_VALUE)) {
            assertFails { decodeSUMHHideTransferDraft(encoded.copyOf().also { ByteBuffer.wrap(it).putInt(4, length) }) }
        }
        assertFails { decodeSUMHHideTransferDraft(ByteArray(SUMH_HIDE_TRANSFER_STORAGE_BYTES + 1)) }
    }

    @Test fun draftRejectsInvalidChoicesAndContextWithoutDiscardingTheSource() {
        val badChoices = listOf(
            mapOf("not-id" to SUMHHideTransferChoice()),
            mapOf(id(1) to SUMHHideTransferChoice(SUMHHideTransferResolution.ExplicitNewIdentity)),
            mapOf(id(1) to SUMHHideTransferChoice(SUMHHideTransferResolution.Skip, id(2))),
        )
        badChoices.forEach { assertFails { encodeSUMHHideTransferDraft(draft().copy(choices = it)) } }
        assertFails { encodeSUMHHideTransferDraft(draft().copy(document = "\uD800")) }
        assertFails { encodeSUMHHideTransferDraft(draft().copy(baselineRevision = "future")) }
        assertFails { encodeSUMHHideTransferDraft(draft().copy(baselineBootId = "x".repeat(81))) }
        assertFails { decodeSUMHHideTransferDraft(rawDraft(choices = listOf(Triple(id(1), "Skip", null), Triple(id(1), "Skip", null)))) }
        assertFails { decodeSUMHHideTransferDraft(rawDraft(choices = listOf(Triple(id(1), "FutureResolution", null)))) }
    }

    @Test fun staleDraftCallbackCannotOverwriteANewerDraftAcrossStoreInstances() {
        val directory = temporary.newFolder(); val first = store(directory); val second = store(directory)
        assertNull(first.readDraft())
        val original = first.saveDraft(draft(), null)
        val changed = second.saveDraft(original.copy(preserveImportedEnabled = false), original.fingerprint)
        assertFailsWith<IllegalArgumentException> { first.saveDraft(original.copy(document = "late file result"), original.fingerprint) }
        assertFails { first.saveDraft(original, null) }
        assertEquals(changed, first.readDraft())
        assertEquals(changed, second.saveDraft(changed, changed.fingerprint))
    }

    @Test fun callerMapMutationDoesNotChangeTheReturnedOrPersistedSnapshot() {
        val choices = mutableMapOf(id(1) to SUMHHideTransferChoice(SUMHHideTransferResolution.Skip))
        val directory = temporary.newFolder(); val saved = store(directory).saveDraft(draft().copy(choices = choices), null)
        choices.clear()
        assertEquals(1, saved.choices.size)
        assertEquals(saved, store(directory).readDraft())
    }

    @Test fun concurrentInitialCasHasOnlyOneWinner() {
        val directory = temporary.newFolder(); val ready = CountDownLatch(2); val go = CountDownLatch(1)
        val pool = Executors.newFixedThreadPool(2)
        try {
            val futures = listOf("one", "two").map { value -> pool.submit<Boolean> {
                ready.countDown(); check(go.await(5, TimeUnit.SECONDS))
                runCatching { store(directory).saveDraft(SUMHHideTransferDraft(value), null) }.isSuccess
            } }
            assertTrue(ready.await(5, TimeUnit.SECONDS)); go.countDown()
            assertEquals(1, futures.count { it.get(10, TimeUnit.SECONDS) })
            assertTrue(store(directory).readDraft()!!.document in setOf("one", "two"))
        } finally { go.countDown(); pool.shutdownNow() }
    }

    @Test fun unknownOrCorruptDraftAndExportAreNeverOverwritten() {
        val directory = temporary.newFolder()
        for (bytes in listOf(byteArrayOf(1, 2), rawDraft(magic = 0x53485432))) {
            draftFile(directory).writeBytes(bytes)
            assertFails { store(directory).readDraft() }
            assertFails { store(directory).saveDraft(draft(), null) }
            assertContentEquals(bytes, draftFile(directory).readBytes())
        }
        val future = byteArrayOf(0x53, 0x48, 0x45, 0x32)
        exportFile(directory).writeBytes(future)
        assertFails { store(directory).saveExport(portable(), revision, "all", 1) }
        assertContentEquals(future, exportFile(directory).readBytes())
    }

    @Test fun parentSyncIsRetriedAfterAnUnconfirmedDirectoryCreation() {
        val parent = temporary.newFolder(); val directory = File(parent, "private")
        var calls = 0
        val storage = SUMHHideTransferStorage(directory) { target ->
            if (target == parent && ++calls == 1) throw IOException("parent sync failed")
        }
        assertFailsWith<IOException> { storage.saveDraft(draft(), null) }
        assertTrue(directory.isDirectory); assertNull(storage.readDraft())
        storage.saveDraft(draft(), null)
        assertEquals(2, calls); assertEquals(draft(), storage.readDraft())
    }

    @Test fun directorySyncFailureIsUnknownEvenWhenTheNewBytesAreVisible() {
        val directory = temporary.newFolder(); val old = store(directory).saveDraft(draft(), null)
        val next = old.copy(document = "changed")
        val storage = SUMHHideTransferStorage(directory) { if (it == directory) throw IOException("rename not confirmed") }
        assertFailsWith<IOException> { storage.saveDraft(next, old.fingerprint) }
        assertEquals(next, store(directory).readDraft())
        assertFails { store(directory).saveDraft(old, old.fingerprint) }
        assertFalse(scratch(directory).exists())
    }

    @Test fun readbackMismatchIsReportedAndScratchIsCleaned() {
        val directory = temporary.newFolder()
        val changed = SUMHHideTransferDraft("external replacement")
        val storage = SUMHHideTransferStorage(directory) { target ->
            if (target == directory) draftFile(directory).writeBytes(encodeSUMHHideTransferDraft(changed))
        }
        assertFailsWith<IOException> { storage.saveDraft(draft(), null) }
        assertEquals(changed, store(directory).readDraft()); assertFalse(scratch(directory).exists())
    }

    @Test fun staleRegularScratchIsUnlinkedWithoutTruncatingItsHardLinkedVictim() {
        val directory = temporary.newFolder(); val victim = temporary.newFile().apply { writeText("keep this") }
        Files.createLink(scratch(directory).toPath(), victim.toPath())
        store(directory).saveDraft(draft(), null)
        assertEquals("keep this", victim.readText()); assertFalse(scratch(directory).exists())
        assertEquals(draft(), store(directory).readDraft())
    }

    @Test fun replacingAHardLinkedValidDestinationDoesNotModifyTheOtherLink() {
        val directory = temporary.newFolder(); val old = draft()
        val victim = temporary.newFile().apply { writeBytes(encodeSUMHHideTransferDraft(old)) }
        Files.createLink(draftFile(directory).toPath(), victim.toPath())
        val changed = old.copy(document = "new text")
        store(directory).saveDraft(changed, old.fingerprint)
        assertEquals(old, decodeSUMHHideTransferDraft(victim.readBytes()))
        assertEquals(changed, store(directory).readDraft())
    }

    @Test fun scratchDirectoriesAreRejectedWithoutTouchingTheExistingDraft() {
        val directory = temporary.newFolder(); val old = store(directory).saveDraft(draft(), null)
        assertTrue(scratch(directory).mkdir())
        assertFails { store(directory).saveDraft(old.copy(document = "new"), old.fingerprint) }
        assertEquals(old, store(directory).readDraft()); assertTrue(scratch(directory).isDirectory)
    }

    @Test fun symlinkScratchAndDestinationAreRejectedWhenSupported() {
        val directory = temporary.newFolder(); val victim = temporary.newFile().apply { writeText("keep") }
        try { Files.createSymbolicLink(scratch(directory).toPath(), victim.toPath()) }
        catch (error: Exception) { assumeNoException("Host cannot create symbolic links", error); return }
        assertFails { store(directory).saveDraft(draft(), null) }
        assertEquals("keep", victim.readText()); assertTrue(Files.isSymbolicLink(scratch(directory).toPath()))
        Files.delete(scratch(directory).toPath())
        Files.createSymbolicLink(draftFile(directory).toPath(), victim.toPath())
        assertFails { store(directory).readDraft() }; assertFails { store(directory).saveDraft(draft(), null) }
        assertEquals("keep", victim.readText())
    }

    @Test fun exportTokenSurvivesProcessRecreationAndRejectsALaterSnapshot() {
        val directory = temporary.newFolder(); val first = store(directory).saveExport(portable(), revision, "selected", 1)
        assertEquals(first, store(directory).readExport(first.token))
        val second = store(directory).saveExport(exportSUMHHideDefinitions(emptyList()), revision, "selected", 0)
        assertNotEquals(first.token, second.token)
        assertFails { store(directory).readExport(first.token) }
        assertEquals(second, store(directory).readExport(second.token))
        assertEquals(1, importSUMHHideDefinitions(first.document).size)
        assertEquals(0, importSUMHHideDefinitions(second.document).size)
        assertFails { store(directory).readExport("invalid token") }
    }

    @Test fun exportAcceptsMaximumArtifactWithoutUsingBundleOrClipboardSizedLimits() {
        val base = exportSUMHHideDefinitions(emptyList())
        val text = base + " ".repeat(SUMH_HIDE_TRANSFER_BYTES - base.toByteArray(Charsets.UTF_8).size)
        val directory = temporary.newFolder(); val artifact = store(directory).saveExport(text, revision, "all", 0)
        assertEquals(text, store(directory).readExport(artifact.token).document)
        assertTrue(exportFile(directory).length() < SUMH_HIDE_TRANSFER_STORAGE_BYTES)
        assertFails { store(directory).saveExport(text + " ", revision, "all", 0) }
        assertEquals(artifact, store(directory).readExport(artifact.token))
    }

    @Test fun exportRejectsBadMetadataAndBrokenPortableDocumentsBeforeReplacingTheSlot() {
        val directory = temporary.newFolder(); val old = store(directory).saveExport(portable(), revision, "all", 1)
        listOf(old.copy(token = "A".repeat(32)), old.copy(scope = "runtime"), old.copy(revision = "unknown"),
            old.copy(count = 0), old.copy(document = "{}"), old.copy(document = "\uD800")).forEach {
            assertFails { encodeSUMHHideExportArtifact(it) }
        }
        assertFails { store(directory).saveExport(portable(), revision, "all", 0) }
        assertEquals(old, store(directory).readExport(old.token))
        val bytes = encodeSUMHHideExportArtifact(old)
        for (count in 0 until bytes.size) assertFails { decodeSUMHHideExportArtifact(bytes.copyOf(count)) }
        assertFails { decodeSUMHHideExportArtifact(bytes + byteArrayOf(0)) }
        assertFails { decodeSUMHHideExportArtifact(bytes.copyOf().also { ByteBuffer.wrap(it).putInt(0, 0x53484532) }) }
    }
}
