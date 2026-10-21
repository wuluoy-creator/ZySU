package com.zying.zysu.ui.sumh

import android.app.Application
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.StateRestorationTester
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.SUMHManager
import com.zying.zysu.ui.sumh.util.SUMHManager.ModuleRule
import org.junit.After
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.shadows.ShadowLooper
import java.io.File
import kotlin.test.assertEquals

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "w360dp-h760dp-xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SUMHModuleRulesDialogTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    @After fun resetFontScale() { RuntimeEnvironment.setFontScale(1f) }
    private fun text(id: Int) = compose.activity.getString(id)
    private fun pathField() = compose.onNode(hasSetTextAction() and hasText(text(R.string.sumh_absolute_path)))
    private fun expand() {
        val drafts = SUMHEditorDrafts.forDirectory(File(compose.activity.noBackupFilesDir, "sumh-drafts"))
        // Loading and the initial draft's LaunchedEffect must both finish before
        // the expansion control accepts a click. Drive the real IO continuation.
        compose.waitUntil(5000) { ShadowLooper.idleMainLooper(); drafts.loaded && "module:module" in drafts.snapshot }
        compose.waitForIdle()
        compose.onNodeWithText(text(R.string.sumh_module_rules_title)).performScrollTo().assertIsEnabled().performClick()
    }
    private fun confirm() = compose.onNodeWithText(text(android.R.string.ok))

    @Test fun failedSaveRetainsLiteralHideDraftAcrossStateRestoration() {
        val restoration = StateRestorationTester(compose)
        var requested: List<ModuleRule>? = null
        var saved = false
        restoration.setContent { MaterialTheme {
            SUMHMountConfigDialog("module", "Module", null, true, "auto", {}, { saved = true },
                onSave = { _, _, rules -> requested = rules; error("Write failed") })
        } }
        expand()
        pathField().performScrollTo().performTextReplacement("/system/file ")
        compose.onAllNodesWithText(text(R.string.sumh_mount_mode_sumh)).onLast().performScrollTo().performClick()
        compose.onNodeWithText(text(R.string.sumh_mount_mode_hide)).performClick()
        confirm().performClick()
        assertEquals(listOf(ModuleRule("/system/file ", "hide")), requested)
        assertEquals(false, saved)
        compose.onNodeWithText("Write failed").performScrollTo().assertIsDisplayed()
        restoration.emulateSavedInstanceStateRestore()
        pathField().performScrollTo().assertTextContains("/system/file ")
        confirm().assertIsEnabled()
        capture("module-rules-hide-draft")
    }

    @Test fun duplicateAndNonCanonicalDraftsCannotSaveAndCancelKeepsOriginal() {
        val initial = listOf(ModuleRule("/system/a", "sumh"), ModuleRule("/system/b", "hide"))
        var requested: List<ModuleRule>? = null
        compose.setContent { MaterialTheme {
            SUMHMountConfigDialog("module", "Module", SUMHManager.ModuleInfo("module", "auto", initial),
                true, "auto", {}, {}, onSave = { _, _, rules -> requested = rules })
        } }
        expand()
        compose.onNodeWithContentDescription(compose.activity.getString(R.string.sumh_module_rule_edit_label, "/system/a"))
            .performScrollTo().performClick()
        pathField().performScrollTo().performTextReplacement("/system/b")
        confirm().assertIsNotEnabled()
        compose.onNodeWithText(text(R.string.sumh_module_rule_duplicate)).performScrollTo().assertIsDisplayed()
        pathField().performScrollTo().performTextReplacement("/system/../changed")
        confirm().assertIsNotEnabled()
        compose.onNodeWithText(text(R.string.sumh_module_rule_canonical)).performScrollTo().assertIsDisplayed()
        compose.onNodeWithText(text(R.string.sumh_module_rule_cancel_edit)).performScrollTo().performClick()
        confirm().assertIsEnabled().performClick()
        assertEquals(initial, requested)
    }

    @Test fun unsupportedKernelRetainsSavedHideButDoesNotOfferNewHideOrMixedBackends() {
        val initial = listOf(ModuleRule("/system/hidden", "hide"))
        var requested: List<ModuleRule>? = null
        compose.setContent { MaterialTheme {
            SUMHMountConfigDialog("module", "Module", SUMHManager.ModuleInfo("module", "auto", initial),
                false, "auto", {}, {}, onSave = { _, _, rules -> requested = rules })
        } }
        expand()
        // A saved hide entry remains visible and removable, but the path-mode popup has only Auto / None.
        compose.onAllNodesWithText(text(R.string.sumh_mount_mode_auto)).onLast().performScrollTo().performClick()
        compose.onNode(isPopup()).assertExists()
        compose.onAllNodes(hasText(text(R.string.sumh_mount_mode_hide)) and hasAnyAncestor(isPopup())).assertCountEquals(0)
        compose.onAllNodes(hasText(text(R.string.sumh_mount_mode_magic)) and hasAnyAncestor(isPopup())).assertCountEquals(0)
        compose.onAllNodes(hasText(text(R.string.sumh_mount_mode_overlay)) and hasAnyAncestor(isPopup())).assertCountEquals(0)
        compose.onNode(hasText(text(R.string.sumh_mount_mode_none)) and hasAnyAncestor(isPopup())).performClick()
        compose.onNodeWithContentDescription(compose.activity.getString(R.string.sumh_module_rule_edit_label, "/system/hidden"))
            .performScrollTo().performClick()
        pathField().performScrollTo().performTextReplacement("/system/changed")
        confirm().assertIsNotEnabled()
        compose.onNodeWithText(text(R.string.sumh_module_rule_cancel_edit)).performScrollTo().performClick()
        compose.onNodeWithContentDescription(compose.activity.getString(R.string.sumh_module_rule_remove_label, "/system/hidden"))
            .performScrollTo().performClick()
        confirm().performClick()
        assertEquals(emptyList(), requested)
    }

    @Test
    @Config(qualifiers = "zh-rCN-w760dp-h360dp-land-xhdpi")
    fun largeTextLandscapeKeepsErrorsAndSaveReachable() {
        RuntimeEnvironment.setFontScale(2f)
        compose.setContent { MaterialTheme(colorScheme = darkColorScheme()) {
            SUMHMountConfigDialog("module", "模块", null, true, "auto", {}, {}, onSave = { _, _, _ -> })
        } }
        expand()
        pathField().performScrollTo().performTextReplacement("/system//file")
        compose.onNodeWithText(text(R.string.sumh_module_rule_canonical)).performScrollTo().assertIsDisplayed()
        confirm().assertIsDisplayed().assertIsNotEnabled()
        capture("module-rules-landscape-large-text")
    }

    private fun capture(name: String) {
        val file = File("build/reports/sumh-layout/$name.png")
        file.parentFile?.mkdirs()
        val bitmap = compose.onNode(isDialog()).captureToImage().asAndroidBitmap()
        file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }
}
