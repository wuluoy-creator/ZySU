package com.zying.zysu.ui.sumh

import android.app.Application
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.StateRestorationTester
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.*
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter.MapsPathRule
import kotlinx.coroutines.CompletableDeferred
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.File

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "w360dp-h760dp-xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SUMHMapsWorkflowTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    private val f = SUMHMapsPreviewFixtures
    private fun text(id: Int) = compose.activity.getString(id)
    private fun input(rules: List<MapsPathRule>) = compose.onNodeWithTag("maps_import_json").performScrollTo()
        .performTextReplacement(exportSUMHMapsDefinitions(rules))
    private fun review() = compose.onNodeWithTag("maps_import_review").performScrollTo().performClick()
    private fun preview() = SUMHMapsPreviewParser.reply(SUMHQueryReply(f.preview().toString()))

    @Test fun importRequiresReviewAndExplicitSaveAndRetainsFailedInput() {
        val saved = mutableStateOf(emptyList<MapsPathRule>())
        var submitted: List<MapsPathRule>? = null
        var dismissed = false
        compose.setContent { MaterialTheme { MapsImportDialog(saved.value, true, { submitted = it }, { dismissed = true }) } }
        val candidate = listOf(MapsPathRule("/目标 ", "/替换 "))
        input(candidate)
        assertNull(submitted)
        compose.onNodeWithTag("maps_import_save").assertIsNotEnabled()
        review()
        assertNull(submitted)
        compose.onNodeWithTag("maps_import_save").assertIsEnabled().performClick()
        assertEquals(candidate, submitted)
        compose.onNodeWithTag("maps_import_json").assertTextContains(exportSUMHMapsDefinitions(candidate))
        assertFalse(dismissed)
        compose.runOnIdle { saved.value = candidate }
        compose.waitForIdle()
        assertTrue(dismissed)
    }

    @Test fun sameTargetConflictsNeedChoiceAndReplacementListsRemovals() {
        val saved = listOf(MapsPathRule("/same", "/old"), MapsPathRule("/remove", "/r"))
        var submitted: List<MapsPathRule>? = null
        compose.setContent { MaterialTheme { MapsImportDialog(saved, true, { submitted = it }, {}) } }
        val incoming = listOf(MapsPathRule("/same", "/new"))
        input(incoming)
        compose.onNodeWithText(text(R.string.sumh_maps_import_replace)).performScrollTo().performClick()
        review()
        compose.onNodeWithTag("maps_import_save").assertIsNotEnabled()
        compose.onNodeWithText(text(R.string.sumh_maps_import_use)).performScrollTo().performClick()
        compose.onNodeWithTag("maps_import_save").assertIsNotEnabled()
        review()
        compose.onNodeWithText(text(R.string.sumh_maps_diff_removed)).performScrollTo().assertIsDisplayed()
        compose.onNodeWithTag("maps_import_save").performClick()
        assertEquals(incoming, submitted)
    }

    @Test fun invalidFutureFormatRetainsInputAndNeverSaves() {
        var submissions = 0
        compose.setContent { MaterialTheme { MapsImportDialog(emptyList(), true, { submissions++ }, {}) } }
        val future = "{\"schema_version\":99,\"kind\":\"sumh_managed_maps\",\"rules\":[]}"
        compose.onNodeWithTag("maps_import_json").performScrollTo().performTextReplacement(future)
        review()
        compose.onNodeWithTag("maps_import_save").assertIsNotEnabled()
        compose.onNodeWithTag("maps_import_json").assertTextContains(future)
        compose.onNodeWithText(text(R.string.sumh_maps_import_invalid)).assertExists()
        assertEquals(0, submissions)
    }

    @Test fun externalRefreshBlocksSaveWithoutDiscardingDraftAndRecreationDoesNotReplay() {
        val saved = mutableStateOf(emptyList<MapsPathRule>())
        var submissions = 0
        val restore = StateRestorationTester(compose)
        restore.setContent { MaterialTheme { MapsImportDialog(saved.value, true, { submissions++ }, {}) } }
        val imported = listOf(MapsPathRule("/draft ", "/r"))
        input(imported); review()
        compose.runOnIdle { saved.value = listOf(MapsPathRule("/external", "/r")) }
        compose.onNodeWithTag("maps_import_save").assertIsNotEnabled()
        compose.onNodeWithTag("maps_import_json").assertTextContains(exportSUMHMapsDefinitions(imported))
        compose.onNodeWithText(text(R.string.sumh_maps_import_rebase)).performScrollTo().performClick()
        review(); compose.onNodeWithTag("maps_import_save").assertIsEnabled()
        restore.emulateSavedInstanceStateRestore()
        compose.onNodeWithTag("maps_import_json").assertTextContains(exportSUMHMapsDefinitions(imported))
        compose.onNodeWithTag("maps_import_save").assertIsNotEnabled()
        assertEquals(0, submissions)
    }

    @Test fun readbackOfEarlierImportKeepsNewerInput() {
        val saved = mutableStateOf(emptyList<MapsPathRule>())
        var submitted = emptyList<MapsPathRule>()
        var dismissed = false
        compose.setContent { MaterialTheme { MapsImportDialog(saved.value, true, { submitted = it }, { dismissed = true }) } }
        input(listOf(MapsPathRule("/submitted", "/r"))); review()
        compose.onNodeWithTag("maps_import_save").performClick()
        val newer = listOf(MapsPathRule("/newer", "/r"))
        input(newer)
        compose.runOnIdle { saved.value = submitted }
        compose.onNodeWithTag("maps_import_json").assertTextContains(exportSUMHMapsDefinitions(newer))
        compose.onNodeWithText(text(R.string.sumh_maps_import_external)).assertDoesNotExist()
        assertFalse(dismissed)
    }

    @Test fun exportCopiesOnlySelectedDefinitionsAndNoRuntimeIdentity() {
        val rules = listOf(MapsPathRule("/a ", "/b "), MapsPathRule("/other", "/r"))
        compose.setContent { MaterialTheme { MapsExportDialog(rules) {} } }
        compose.onNodeWithTag("maps_export_/other").performScrollTo().performClick()
        compose.onNodeWithTag("maps_export_copy").performClick()
        val clipboard = compose.activity.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        val exported = clipboard.primaryClip!!.getItemAt(0).text.toString()
        assertEquals(listOf(rules[0]), importSUMHMapsDefinitions(exported))
        assertFalse(exported.contains("ino")); assertFalse(exported.contains("enabled"))
    }

    @Test fun exportSelectionSurvivesRecreationAndNewDefinitionsAreNotAutoSelected() {
        val original = listOf(MapsPathRule("/first ", "/r"), MapsPathRule("/second", "/r"))
        val rules = mutableStateOf(original)
        val restore = StateRestorationTester(compose)
        restore.setContent { MaterialTheme { MapsExportDialog(rules.value) {} } }
        compose.onNodeWithTag("maps_export_/second").performScrollTo().performClick()
        restore.emulateSavedInstanceStateRestore()
        compose.runOnIdle { rules.value = original + MapsPathRule("/new", "/r") }
        compose.onNodeWithTag("maps_export_copy").performClick()
        val clipboard = compose.activity.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        assertEquals(listOf(original[0]), importSUMHMapsDefinitions(clipboard.primaryClip!!.getItemAt(0).text.toString()))
        compose.runOnIdle { rules.value = listOf(original[1]) }
        compose.onNodeWithTag("maps_export_copy").performClick()
        assertTrue(importSUMHMapsDefinitions(clipboard.primaryClip!!.getItemAt(0).text.toString()).isEmpty())
    }

    @Test fun previewReadsOnlyManuallyAndRefreshRemovesOldAuthority() {
        var reads = 0
        val gate = CompletableDeferred<SUMHMapsPreviewResult>()
        val restore = StateRestorationTester(compose)
        restore.setContent { MaterialTheme { MapsPreviewDialog(read = {
            reads++; if (reads == 1) preview() else gate.await()
        }) {} } }
        assertEquals(0, reads)
        compose.onNodeWithTag("maps_preview_load").performClick()
        compose.onNodeWithTag("maps_preview_status").assertTextEquals(text(R.string.sumh_maps_preview_complete))
        compose.onNodeWithTag("maps_preview_load").performClick()
        compose.onNodeWithTag("maps_preview_status").assertTextEquals(text(R.string.sumh_maps_preview_loading))
        compose.onNodeWithText(text(R.string.sumh_maps_preview_set_matches)).assertDoesNotExist()
        compose.runOnIdle { gate.complete(SUMHMapsPreviewResult(error = "timeout")) }
        compose.onNodeWithTag("maps_preview_status").assertTextEquals(text(R.string.sumh_maps_preview_failed))
        compose.onNodeWithTag("maps_preview_empty").assertDoesNotExist()
        restore.emulateSavedInstanceStateRestore()
        compose.onNodeWithTag("maps_preview_status").assertTextEquals(text(R.string.sumh_maps_preview_not_loaded))
        assertEquals(2, reads)
    }

    @Test fun changingHistoryNeverOffersCurrentVersusSubmittedEquality() {
        val raw = f.withReceipt()
        raw.getJSONObject("last_submission").put("stable", false)
        compose.setContent { MaterialTheme { MapsPreviewDialog(read = { SUMHMapsPreviewParser.reply(SUMHQueryReply(raw.toString())) }) {} } }
        compose.onNodeWithTag("maps_preview_load").performClick()
        compose.onNodeWithTag("maps_preview_status").assertTextEquals(text(R.string.sumh_maps_preview_partial))
        compose.onNodeWithTag("maps_preview_list").performScrollToNode(hasText(text(R.string.sumh_maps_preview_history_unstable)))
        compose.onNodeWithText(text(R.string.sumh_maps_preview_history_unstable)).assertExists()
        compose.onNodeWithTag("maps_preview_list").performScrollToNode(hasTestTag("maps_preview_row_0"))
        compose.onNodeWithText(text(R.string.sumh_maps_preview_identity_same)).assertDoesNotExist()
        compose.onNodeWithText(text(R.string.sumh_maps_preview_identity_changed)).assertDoesNotExist()
    }

    @Test fun changedSavedDefinitionsAndSwitchInvalidateDelayedAndRetainedPreviewWithoutReload() {
        val rules = mutableStateOf(listOf(MapsPathRule("/initial", "/r")))
        val enabled = mutableStateOf(true)
        val gate = CompletableDeferred<SUMHMapsPreviewResult>()
        var reads = 0
        compose.setContent { MaterialTheme { MapsPreviewDialog(read = {
            reads++; if (reads == 1) gate.await() else preview()
        }, definitionFingerprint = mapsPreviewFingerprint(rules.value, enabled.value)) {} } }
        compose.onNodeWithTag("maps_preview_load").performClick()
        compose.runOnIdle { rules.value = listOf(MapsPathRule("/changed", "/r")); gate.complete(preview()) }
        compose.onNodeWithTag("maps_preview_status").assertTextEquals(text(R.string.sumh_maps_preview_local_change))
        compose.onNodeWithText(text(R.string.sumh_maps_preview_set_matches)).assertDoesNotExist()
        assertEquals(1, reads)
        compose.onNodeWithTag("maps_preview_load").performClick()
        compose.onNodeWithTag("maps_preview_status").assertTextEquals(text(R.string.sumh_maps_preview_complete))
        compose.runOnIdle { enabled.value = false }
        compose.onNodeWithTag("maps_preview_status").assertTextEquals(text(R.string.sumh_maps_preview_local_change))
        compose.runOnIdle { enabled.value = true }
        compose.onNodeWithTag("maps_preview_status").assertTextEquals(text(R.string.sumh_maps_preview_local_change))
        assertEquals(2, reads)
    }

    @Test
    @Config(qualifiers = "zh-rCN-w360dp-h760dp-xhdpi")
    fun previewShowsHistoricalDifferenceAndErrorsAtLargeText() {
        RuntimeEnvironment.setFontScale(2f)
        try {
            val raw = f.withReceipt(latest = f.receipt("pending"), confirmed = f.receipt().put("boot_id", "previous-boot"))
            val result = SUMHMapsPreviewParser.reply(SUMHQueryReply(raw.toString()))
            compose.setContent { MaterialTheme { MapsPreviewDialog(read = { result }) {} } }
            compose.onNodeWithTag("maps_preview_load").performClick()
            compose.onNodeWithTag("maps_preview_list").performScrollToNode(hasText(text(R.string.sumh_maps_preview_pending)))
            compose.onNodeWithText(text(R.string.sumh_maps_preview_history_other_boot)).assertExists()
            compose.onNodeWithTag("maps_preview_list").performScrollToNode(hasText(text(R.string.sumh_maps_preview_identity_changed)))
            compose.onNodeWithText(text(R.string.sumh_maps_preview_identity_changed)).performScrollTo().assertIsDisplayed()
            screenshot("maps-preview-large-text-history.png")
        } finally { RuntimeEnvironment.setFontScale(1f) }
    }

    @Test
    @Config(qualifiers = "zh-rCN-w760dp-h360dp-land-xhdpi")
    fun importReviewAndExplicitSaveRemainReachableInLargeTextLandscape() {
        RuntimeEnvironment.setFontScale(2f)
        try {
            compose.setContent { MaterialTheme { MapsImportDialog(emptyList(), true, {}, {}) } }
            input(listOf(MapsPathRule("/数据/路径 ", "/显示 ")))
            review()
            compose.onNodeWithTag("maps_import_save").assertIsEnabled().assertIsDisplayed()
            screenshot("maps-import-landscape-large-text.png")
        } finally { RuntimeEnvironment.setFontScale(1f) }
    }

    private fun screenshot(name: String) {
        val output = File("build/reports/sumh-layout/$name")
        output.parentFile!!.mkdirs()
        output.outputStream().use { compose.onRoot().captureToImage().asAndroidBitmap().compress(Bitmap.CompressFormat.PNG, 100, it) }
    }
}
