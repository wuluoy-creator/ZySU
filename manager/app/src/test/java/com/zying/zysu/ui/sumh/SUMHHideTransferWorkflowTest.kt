package com.zying.zysu.ui.sumh

import android.app.Application
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.unit.Density
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.*
import kotlinx.coroutines.CompletableDeferred
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.shadows.ShadowLooper
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Files
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "en-w360dp-h760dp-xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SUMHHideTransferWorkflowTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    private val revision = "sha256-v1:" + "a".repeat(64)
    private val next = "sha256-v1:" + "b".repeat(64)
    private val row = SUMHHidePortableDefinition("a".repeat(32), "/专用测试/尾 \n", true, "Name", "Note", "Group")
    private fun saved(rows: List<SUMHHidePortableDefinition> = emptyList(), rev: String = revision) = SUMHHideSavedDefinitions(rev, "boot", rows, true)
    private fun runtime() = HideDefinitionCollection(revision, "boot", complete = true, managed = true)
    private fun until(condition: () -> Boolean) {
        // Compose's clock drives recomposition, not Android Handler continuations
        // posted after production IO/Default work. Pump both schedulers; do not
        // replace production dispatchers or skip asynchronous persistence.
        compose.waitUntil(10000) { ShadowLooper.idleMainLooper(); condition() }
        compose.waitForIdle()
    }
    private fun idle(owner: SUMHHideTransferOperations) = until { !owner.busy }
    private fun owner(): Pair<SUMHHideTransferOperations, SUMHHideTransferStorage> {
        val directory = Files.createTempDirectory("sumh-hide-workflow").toFile()
        val store = SUMHHideTransferStorage(directory) {}
        val owner = SUMHHideTransferOperations()
        compose.setContent { MaterialTheme { Text("${owner.busy} ${owner.loaded} ${owner.error}") } }
        compose.runOnIdle { owner.load(directory, store) }; idle(owner); assertTrue(owner.loaded)
        return owner to store
    }
    private fun stage(owner: SUMHHideTransferOperations, rows: List<SUMHHidePortableDefinition> = listOf(row)) {
        val document = exportSUMHHideDefinitions(rows)
        compose.runOnIdle { owner.stage(owner.fingerprint) { ByteArrayInputStream(document.toByteArray()) } }
        idle(owner); assertNull(owner.error)
    }
    private fun preview(owner: SUMHHideTransferOperations, rows: List<SUMHHidePortableDefinition> = emptyList()) {
        compose.runOnIdle { owner.preview { saved(rows) } }; idle(owner)
    }
    private fun success(args: List<String>, rev: String, boot: String) = HideMutationResult(HideMutationReceipt(
        "import-id", "applied", true, next, rev, boot, "", "", 1, 1, 0, 0, true,
        listOf(HideMutationItem(args[2], "absent", 0, true, null))), operationId = "c".repeat(32))

    @Test fun importingAndPreviewingNeverMutateAndDefaultsDisableNewRows() {
        val (owner, store) = owner(); stage(owner); preview(owner)
        assertTrue(owner.reviewed); assertFalse(requireNotNull(owner.review).steps.single().after.enabled)
        assertTrue(importSUMHHideDefinitions(requireNotNull(store.readDraft()).document).single().enabled)
        assertNull(owner.batch)
    }
    @Test fun staleFileCallbackAndMalformedInputKeepPriorDraft() {
        val (owner, store) = owner(); val missing = owner.fingerprint; stage(owner)
        val original = store.readDraft()
        compose.runOnIdle { owner.stage(missing) { error("must not open stale input") } }; idle(owner)
        assertEquals("hide_transfer_draft_changed", owner.error); assertEquals(original, store.readDraft())
        compose.runOnIdle { owner.stage(owner.fingerprint) { ByteArrayInputStream("{broken".toByteArray()) } }; idle(owner)
        assertNotNull(owner.error); assertEquals(original, store.readDraft())
    }
    @Test fun choiceAndEnablePolicyRequireFreshExplicitPreview() {
        val (owner, _) = owner(); stage(owner); preview(owner); assertTrue(owner.reviewed)
        compose.runOnIdle { owner.preserve(true) }; idle(owner); assertFalse(owner.reviewed)
        preview(owner); assertTrue(owner.reviewed); assertTrue(requireNotNull(owner.review).steps.single().after.enabled)
        compose.runOnIdle { owner.choose(requireNotNull(owner.review).rows.single(), SUMHHideTransferResolution.Skip) }
        idle(owner); assertFalse(owner.reviewed); assertTrue(requireNotNull(owner.review).steps.isEmpty())
    }
    @Test fun capabilityOrChangedBaselinePreventsAllDispatch() {
        val (owner, _) = owner(); stage(owner); preview(owner); var calls = 0
        compose.runOnIdle { owner.apply({ saved() }, { runtime() }, { false }, { _, _, _ -> calls++; error("not allowed") }) }; idle(owner)
        assertEquals(0, calls); assertFalse(owner.reviewed)
        preview(owner)
        compose.runOnIdle { owner.apply({ saved(rev = next) }, { runtime() }, { true }, { _, _, _ -> calls++; error("not allowed") }) }; idle(owner)
        assertEquals(0, calls); assertEquals("hide_import_preview_expired", owner.error)
    }
    @Test fun successfulGroupRetainsDraftAndFreshOwnerNeverApprovesOrResumes() {
        val (owner, store) = owner(); stage(owner); preview(owner); val original = owner.draft.document; var calls = 0
        compose.runOnIdle { owner.apply({ saved() }, { runtime() }, { true }, { args, rev, boot -> calls++; success(args, rev, boot) }) }; idle(owner)
        assertEquals(1, calls); assertEquals(1, owner.batch?.succeededCount); assertFalse(owner.reviewed)
        assertEquals(original, store.readDraft()?.document)
        val replacement = SUMHHideTransferOperations()
        compose.runOnIdle { replacement.load(File("unused"), store) }; idle(replacement)
        assertEquals(original, replacement.draft.document); assertNull(replacement.review); assertNull(replacement.batch)
        assertEquals(1, calls)
    }
    @Test fun lostReplyStopsWithKnownIdAndNoAutomaticNextItem() {
        val (owner, _) = owner(); stage(owner, listOf(row, row.copy(id = "d".repeat(32), path = "/another"))); preview(owner)
        val gate = CompletableDeferred<HideMutationResult>(); var calls = 0
        compose.runOnIdle { owner.apply({ saved() }, { runtime() }, { true }, { _, _, _ -> calls++; gate.await() }) }
        until { calls == 1 }
        compose.runOnIdle { gate.complete(HideMutationResult(error = "lost_reply", operationId = "e".repeat(32))) }; idle(owner)
        assertEquals(1, calls); assertEquals(1, owner.batch?.unattempted?.size)
        assertEquals("e".repeat(32), owner.batch?.attempted?.single()?.second?.operationId)
        assertFalse(owner.reviewed)
    }
    @Test fun exportCallbackWritesFrozenSelectionAndReadFailureDoesNotBecomeEmptySuccess() {
        val (owner, _) = owner(); val rows = listOf(row, row.copy(id = "b".repeat(32), path = "/second"))
        val selection = mutableSetOf(row.id)
        compose.runOnIdle { owner.captureExport(selection) { saved(rows) }; selection.clear() }; idle(owner)
        val artifact = requireNotNull(owner.exportReady); assertEquals(1, artifact.count)
        compose.runOnIdle { owner.consumeExportReady() }
        val output = ByteArrayOutputStream()
        compose.runOnIdle { owner.writeExport(artifact.token) { output } }; idle(owner)
        assertTrue(owner.exportDone); assertEquals(listOf(row), importSUMHHideDefinitions(output.toString("UTF-8")))
        compose.runOnIdle { owner.captureExport(null) { saved().copy(complete = false, error = "unreadable") } }; idle(owner)
        assertNull(owner.exportReady); assertFalse(owner.exportDone); assertEquals("unreadable", owner.error)
    }
    @Test fun importCallbackBeforeAndDuringLoadRemainsPendingUntilAcceptedOnce() {
        val directory = Files.createTempDirectory("sumh-hide-import-pending").toFile()
        val store = SUMHHideTransferStorage(directory) {}
        val replacement = SUMHHideTransferOperations()
        val document = exportSUMHHideDefinitions(listOf(row))
        var pending = true; var opened = 0
        fun deliver() {
            if (pending && replacement.stage(null) { opened++; ByteArrayInputStream(document.toByteArray()) }) pending = false
        }
        compose.setContent { MaterialTheme { Text(replacement.busy.toString() + " " + replacement.loaded) } }
        compose.runOnIdle {
            deliver()
            assertTrue(pending); assertEquals(0, opened); assertFalse(replacement.loaded)
            replacement.load(directory, store)
            assertTrue(replacement.busy)
            deliver()
            assertTrue(pending); assertEquals(0, opened)
        }
        idle(replacement); assertTrue(replacement.loaded); assertTrue(pending)
        compose.runOnIdle { deliver() }; idle(replacement)
        assertFalse(pending); assertEquals(1, opened); assertEquals(document, store.readDraft()?.document)
        assertEquals(document, replacement.draft.document); assertNull(replacement.review); assertNull(replacement.batch)
        compose.runOnIdle { deliver() }; idle(replacement)
        assertEquals(1, opened)
    }
    @Test fun exportCallbackBeforeAndDuringLoadRetainsFrozenTokenUntilAcceptedOnce() {
        val directory = Files.createTempDirectory("sumh-hide-export-pending").toFile()
        val store = SUMHHideTransferStorage(directory) {}
        val document = exportSUMHHideDefinitions(listOf(row))
        val artifact = store.saveExport(document, revision, "selected", 1)
        val replacement = SUMHHideTransferOperations()
        var pendingToken: String? = artifact.token; var opened = 0
        val output = ByteArrayOutputStream()
        fun deliver() {
            val token = pendingToken ?: return
            if (replacement.writeExport(token) { opened++; output }) pendingToken = null
        }
        compose.setContent { MaterialTheme { Text(replacement.busy.toString() + " " + replacement.loaded) } }
        compose.runOnIdle {
            deliver()
            assertEquals(artifact.token, pendingToken); assertEquals(0, opened)
            replacement.load(directory, store)
            assertTrue(replacement.busy)
            deliver()
            assertEquals(artifact.token, pendingToken); assertEquals(0, opened)
        }
        idle(replacement); assertTrue(replacement.loaded)
        compose.runOnIdle { deliver() }; idle(replacement)
        assertNull(pendingToken); assertEquals(1, opened); assertTrue(replacement.exportDone)
        assertEquals(document, output.toString("UTF-8")); assertEquals(listOf(row), importSUMHHideDefinitions(output.toString("UTF-8")))
        assertNull(replacement.review); assertNull(replacement.batch)
        compose.runOnIdle { deliver() }; idle(replacement)
        assertEquals(1, opened)
    }
    @Test @Config(qualifiers = "zh-rCN-w360dp-h760dp-xhdpi")
    fun chinesePortraitReviewControlsReachable() = layout(1f, "sumh-hide-transfer-portrait")
    @Test @Config(qualifiers = "en-w760dp-h360dp-land-xhdpi")
    fun landscapeLargeFontUsesLazyRowsAndReachableActions() = layout(2f, "sumh-hide-transfer-landscape")
    private fun layout(scale: Float, name: String) {
        val rows = (1..4096).map { row.copy(id = it.toString(16).padStart(32, '0'), path = "/synthetic/$it") }
        val review = reviewSUMHHideImport(emptyList(), rows, revision, "boot")
        compose.setContent { MaterialTheme {
            val density = LocalDensity.current
            CompositionLocalProvider(LocalDensity provides Density(density.density, scale)) {
                HideTransferDialog(4096, false, review, true, false, false, true, null, 0, null,
                    {}, {}, {}, { _, _ -> }, {}, {}, {}, {})
            }
        } }
        compose.onNodeWithTag("hide_transfer_apply").assertIsDisplayed().assertIsEnabled()
        compose.onNodeWithTag("hide_transfer_review").performScrollTo().assertIsDisplayed()
        compose.onNodeWithTag("hide_transfer_choice_${rows.last().id}").assertDoesNotExist()
        val image = compose.onRoot().captureToImage().asAndroidBitmap()
        val file = File("build/reports/sumh-layout/$name.png"); requireNotNull(file.parentFile).mkdirs()
        file.outputStream().use { image.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }
}
