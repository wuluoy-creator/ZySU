package com.zying.zysu.ui.sumh

import android.app.Application
import android.app.Instrumentation
import android.os.Bundle
import android.os.Parcel
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import com.zying.zysu.ui.sumh.util.*
import kotlinx.coroutines.CompletableDeferred
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.shadows.ShadowLooper
import java.io.File
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "en-w400dp-h900dp-xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SUMHModulePreviewIntegrationTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    private val id = "preview." + java.util.UUID.randomUUID().toString()
    private fun until(condition: () -> Boolean) {
        compose.waitUntil(10000) { ShadowLooper.idleMainLooper(); condition() }
        compose.waitForIdle()
    }
    private fun drafts() = SUMHEditorDrafts.forDirectory(File(compose.activity.noBackupFilesDir, "sumh-drafts"))
    private fun ready() = until { drafts().loaded && "module:$id" in drafts().snapshot }
    private fun open() { compose.onNodeWithTag("module_preview_open").performScrollTo().performClick() }
    private fun click(tag: String) { compose.onNodeWithTag(tag).performScrollTo().performClick() }

    @Test fun openingMakesNoCallsAndComparesCompleteCommittedUnsavedDraft() {
        val owner = SUMHModulePreviewOperations(); var reads = 0; var saves = 0
        var captured: SUMHModulePreviewRequest? = null
        val savedRules = listOf(SUMHManager.ModuleRule("/", "none"))
        compose.setContent { MaterialTheme {
            SUMHMountConfigDialog(id, "Module", SUMHManager.ModuleInfo(id, "auto", savedRules), false, "magic", {}, {},
                onSave = { _, _, _ -> saves++ }, previewOperation = owner,
                previewRead = { reads++; captured = it; SUMHModulePreviewResult(error = "test_read") })
        } }
        ready()
        val full = savedRules + SUMHManager.ModuleRule("/vendor/etc/keep ", "sumh")
        compose.runOnIdle { drafts().change("module:$id") { ModuleEditorDraft.from(requireNotNull(it)).copy(mode = "sumh", rules = full).record() } }
        open(); assertEquals(0, reads); assertEquals(0, saves)
        click("module_preview_read_candidate"); until { !owner.busy && reads == 1 }
        assertEquals(full, captured?.candidateRules); assertEquals("sumh", captured?.candidateMode)
        assertFalse(requireNotNull(captured?.candidateJson).contains("\"config\"")); assertEquals(0, saves)
        assertEquals(full, ModuleEditorDraft.from(requireNotNull(drafts().snapshot["module:$id"])).rules)
    }

    @Test fun pendingInlineEditBlocksOpeningAndConflictedDraftOnlyBlocksCandidate() {
        val info = mutableStateOf(SUMHManager.ModuleInfo(id, "sumh", emptyList()))
        val owner = SUMHModulePreviewOperations(); var reads = 0
        compose.setContent { MaterialTheme {
            SUMHMountConfigDialog(id, "Module", info.value, true, "auto", {}, {}, onSave = { _, _, _ -> },
                previewOperation = owner, previewRead = { reads++; assertNull(it.candidateJson); SUMHModulePreviewResult(error = "saved") })
        } }
        ready()
        compose.runOnIdle { drafts().change("module:$id") { ModuleEditorDraft.from(requireNotNull(it)).copy(newPath = "/unfinished ").record() } }
        compose.onNodeWithTag("module_preview_open").assertIsNotEnabled()
        compose.runOnIdle {
            drafts().change("module:$id") { ModuleEditorDraft.from(requireNotNull(it)).copy(newPath = "").record() }
            info.value = info.value.copy(mode = "none")
        }
        open(); compose.onNodeWithTag("module_preview_read_candidate").assertIsNotEnabled()
        click("module_preview_read_saved"); until { !owner.busy && reads == 1 }
        assertEquals("saved", owner.result?.error)
        assertEquals("sumh", ModuleEditorDraft.from(requireNotNull(drafts().snapshot["module:$id"])).mode)
    }

    @Test fun editRevertBetweenCompositionsMarksResultStaleAndNewReadIsManual() {
        val owner = SUMHModulePreviewOperations(); var calls = 0
        compose.setContent { MaterialTheme {
            SUMHMountConfigDialog(id, "Module", SUMHManager.ModuleInfo(id, "sumh", emptyList()), true, "auto", {}, {},
                onSave = { _, _, _ -> }, previewOperation = owner,
                previewRead = { calls++; SUMHModulePreviewResult(error = "sample") })
        } }
        ready(); open(); click("module_preview_read_candidate"); until { calls == 1 && !owner.busy }
        compose.runOnIdle {
            val original = requireNotNull(drafts().snapshot["module:$id"])
            val beforeEpoch = drafts().editEpoch
            val originalDraft = ModuleEditorDraft.from(original)
            val otherMode = if (originalDraft.mode == "none") "sumh" else "none"
            assertTrue(drafts().change("module:$id") { originalDraft.copy(mode = otherMode).record() })
            assertTrue(drafts().change("module:$id") { original })
            assertEquals(beforeEpoch + 2, drafts().editEpoch)
        }
        // This assertion depends on the editor's SideEffect after recomposition,
        // unlike the IO/Default continuations handled by until's Main looper pump.
        compose.waitForIdle()
        until { owner.stale }; assertEquals(1, calls)
        click("module_preview_read_candidate"); until { calls == 2 && !owner.busy }; assertFalse(owner.stale)
    }

    @Test fun closingDuringReadDetachesResultAndReopeningDoesNotReplay() {
        val owner = SUMHModulePreviewOperations(); val gate = CompletableDeferred<SUMHModulePreviewResult>(); var calls = 0
        compose.setContent { MaterialTheme {
            SUMHMountConfigDialog(id, "Module", SUMHManager.ModuleInfo(id, "sumh", emptyList()), true, "auto", {}, {},
                onSave = { _, _, _ -> }, previewOperation = owner, previewRead = { calls++; gate.await() })
        } }
        ready(); open(); click("module_preview_read_saved"); until { calls == 1 }
        compose.onNodeWithTag("module_preview_close").performClick(); open()
        assertTrue(owner.busy); assertNull(owner.result)
        compose.runOnIdle { gate.complete(SUMHModulePreviewResult(error = "late")) }
        until { !owner.busy }; assertNull(owner.result); assertNull(owner.request); assertEquals(1, calls)
    }

    @Test fun largeRequestAndResultRemainOutsideRealActivitySavedState() {
        val owner = SUMHModulePreviewOperations(); var calls = 0
        val rules = List(240) { SUMHManager.ModuleRule("/system/" + "x".repeat(130) + it, "sumh") }
        compose.setContent { MaterialTheme {
            SUMHMountConfigDialog(id, "Module", SUMHManager.ModuleInfo(id, "sumh", rules), true, "auto", {}, {},
                onSave = { _, _, _ -> }, previewOperation = owner,
                previewRead = { calls++; SUMHModulePreviewResult(error = "large_diagnostic_" + "x".repeat(500_000)) })
        } }
        ready(); open(); click("module_preview_read_candidate"); until { calls == 1 && !owner.busy }
        assertTrue(requireNotNull(owner.request?.candidateJson).length > 30000)
        val saved = Bundle()
        compose.runOnUiThread { Instrumentation().callActivityOnSaveInstanceState(compose.activity, saved) }
        val parcel = Parcel.obtain()
        try { parcel.writeBundle(saved); assertTrue(parcel.dataSize() < 32 * 1024, "Bundle size ${parcel.dataSize()}") }
        finally { parcel.recycle() }
    }
}
