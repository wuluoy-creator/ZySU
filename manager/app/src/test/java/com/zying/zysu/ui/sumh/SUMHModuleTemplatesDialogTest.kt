package com.zying.zysu.ui.sumh

import android.app.Application
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.StateRestorationTester
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.SUMHManager
import com.zying.zysu.ui.sumh.util.SUMHManager.ModuleRule
import com.zying.zysu.ui.sumh.util.moduleTemplatePathLabel
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
class SUMHModuleTemplatesDialogTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    @After fun resetFontScale() { RuntimeEnvironment.setFontScale(1f) }
    private fun text(id: Int) = compose.activity.getString(id)
    private fun open() {
        awaitDraft("module:module")
        compose.onNodeWithText(text(R.string.sumh_template_open)).performScrollTo().assertIsEnabled().performClick()
        awaitDraft("module-template:module")
    }
    private fun select(id: Int) = compose.onNodeWithText(text(id)).performScrollTo().performClick()
    private fun paths(id: Int = R.string.sumh_template_paths) = compose.onNode(hasSetTextAction() and hasText(text(id)))
    private fun review() = select(R.string.sumh_template_review)
    private fun apply() = compose.onNodeWithText(text(R.string.sumh_template_apply))
    private fun drafts() = SUMHEditorDrafts.forDirectory(File(compose.activity.noBackupFilesDir, "sumh-drafts"))
    private fun awaitDraft(key: String) {
        val drafts = drafts()
        compose.waitUntil(5000) { ShadowLooper.idleMainLooper(); drafts.loaded && key in drafts.snapshot }
        compose.waitForIdle()
    }

    @Test fun explicitReviewReplacesCompleteDraftAndFailedSaveRetainsGeneratedRules() {
        var calls = 0
        var submitted: Pair<String, List<ModuleRule>>? = null
        val initial = SUMHManager.ModuleInfo("module", "overlay", listOf(ModuleRule("/old", "hide")))
        compose.setContent { MaterialTheme {
            SUMHMountConfigDialog("module", "Module", initial, true, "magic", {}, {}, onSave = { _, mode, rules ->
                calls++; submitted = mode to rules; error("Save failed")
            })
        } }
        open(); select(R.string.sumh_template_only)
        paths().performScrollTo().performTextReplacement("/vendor/中文 ")
        apply().assertIsNotEnabled(); review()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_template_removed, moduleTemplatePathLabel("/old"), "hide"))
            .performScrollTo().assertIsDisplayed()
        assertEquals(0, calls)
        apply().performClick()
        assertEquals(0, calls)
        compose.onNodeWithText(text(android.R.string.ok)).performClick()
        assertEquals("sumh" to listOf(ModuleRule("/", "none"), ModuleRule("/vendor/中文 ", "sumh")), submitted)
        compose.onNodeWithText("Save failed").performScrollTo().assertIsDisplayed()
        assertEquals(submitted?.second, ModuleEditorDraft.from(drafts().snapshot.getValue("module:module")).rules)
    }

    @Test fun invalidInputAndCancelKeepTemplateAndOriginalDraftAcrossRestoration() {
        val restoration = StateRestorationTester(compose)
        val initial = SUMHManager.ModuleInfo("module", "auto", listOf(ModuleRule("/old", "none")))
        var saves = 0
        restoration.setContent { MaterialTheme {
            SUMHMountConfigDialog("module", "Module", initial, true, "auto", {}, {}, onSave = { _, _, _ -> saves++ })
        } }
        open(); select(R.string.sumh_template_hide)
        paths().performScrollTo().performTextReplacement("/a\n/a")
        apply().assertIsNotEnabled()
        compose.onNodeWithText(text(R.string.sumh_module_rule_duplicate)).performScrollTo().assertIsDisplayed()
        restoration.emulateSavedInstanceStateRestore()
        paths().performScrollTo().assertTextContains("/a\n/a")
        compose.onNodeWithText(text(android.R.string.cancel)).performClick()
        assertEquals(initial.rules, ModuleEditorDraft.from(drafts().snapshot.getValue("module:module")).rules)
        open(); paths().performScrollTo().assertTextContains("/a\n/a")
        assertEquals(0, saves)
    }

    @Test fun refreshedSavedConfigBlocksReplacementUntilEditorConflictResolvedAndReviewedAgain() {
        val info = mutableStateOf(SUMHManager.ModuleInfo("module", "auto", emptyList()))
        compose.setContent { MaterialTheme {
            SUMHMountConfigDialog("module", "Module", info.value, true, "auto", {}, {}, onSave = { _, _, _ -> })
        } }
        open(); review(); apply().assertIsEnabled()
        compose.runOnIdle { info.value = info.value.copy(rules = listOf(ModuleRule("/external", "hide"))) }
        apply().assertIsNotEnabled()
        compose.onNodeWithText(text(R.string.sumh_template_external)).performScrollTo().assertIsDisplayed()
        compose.onNodeWithText(text(android.R.string.cancel)).performClick()
        select(R.string.sumh_editor_draft_keep)
        open(); apply().assertIsNotEnabled(); review(); apply().assertIsEnabled()
    }

    @Test fun changedEditingDraftInvalidatesReviewAndDiffUsesCurrentUnsavedRules() {
        compose.setContent { MaterialTheme {
            SUMHMountConfigDialog("module", "Module", null, true, "auto", {}, {}, onSave = { _, _, _ -> })
        } }
        open(); review(); apply().assertIsEnabled()
        compose.runOnIdle { drafts().change("module:module") { record ->
            ModuleEditorDraft.from(requireNotNull(record)).copy(rules = listOf(ModuleRule("/unsaved", "none"))).record()
        } }
        apply().assertIsNotEnabled(); review()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_template_removed, moduleTemplatePathLabel("/unsaved"), "none"))
            .performScrollTo().assertIsDisplayed()
        apply().performClick()
        assertEquals(emptyList(), ModuleEditorDraft.from(drafts().snapshot.getValue("module:module")).rules)
    }

    @Test fun unfinishedIndividualRuleCannotBeSilentlyReplaced() {
        compose.setContent { MaterialTheme {
            SUMHMountConfigDialog("module", "Module", null, true, "auto", {}, {}, onSave = { _, _, _ -> })
        } }
        awaitDraft("module:module")
        compose.onNodeWithText(text(R.string.sumh_module_rules_title)).performScrollTo().assertIsEnabled().performClick()
        compose.onNode(hasSetTextAction() and hasText(text(R.string.sumh_absolute_path))).performScrollTo().performTextReplacement("/unfinished ")
        compose.onNodeWithText(text(R.string.sumh_template_open)).performScrollTo().assertIsNotEnabled()
        compose.onNodeWithText(text(R.string.sumh_template_finish_edit)).performScrollTo().assertIsDisplayed()
    }

    @Test fun globalOverrideChangeInvalidatesReviewEvenWhenObservedValueReturns() {
        val global = mutableStateOf("auto")
        compose.setContent { MaterialTheme {
            SUMHMountConfigDialog("module", "Module", null, true, global.value, {}, {}, onSave = { _, _, _ -> })
        } }
        open(); review(); apply().assertIsEnabled()
        compose.runOnIdle { global.value = "magic" }
        apply().assertIsNotEnabled()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_template_override, "magic")).performScrollTo().assertIsDisplayed()
        compose.runOnIdle { global.value = "auto" }
        apply().assertIsNotEnabled(); review(); apply().assertIsEnabled()
    }

    @Test fun restoredReviewKeepsCandidateWithoutApplyingOrSaving() {
        val restoration = StateRestorationTester(compose)
        var saves = 0
        restoration.setContent { MaterialTheme {
            SUMHMountConfigDialog("module", "Module", null, true, "auto", {}, {}, onSave = { _, _, _ -> saves++ })
        } }
        open(); select(R.string.sumh_template_exclude)
        paths().performScrollTo().performTextReplacement("/vendor/keep ")
        review(); restoration.emulateSavedInstanceStateRestore()
        apply().assertIsEnabled()
        assertEquals(0, saves)
        assertEquals(emptyList(), ModuleEditorDraft.from(drafts().snapshot.getValue("module:module")).rules)
        apply().performClick()
        assertEquals(0, saves)
        assertEquals(listOf(ModuleRule("/vendor/keep ", "none")), ModuleEditorDraft.from(drafts().snapshot.getValue("module:module")).rules)
    }

    @Test @Config(qualifiers = "zh-rCN-w360dp-h760dp-xhdpi")
    fun narrowChineseLargeTextShowsReviewAndFixedActions() {
        RuntimeEnvironment.setFontScale(2f)
        compose.setContent { MaterialTheme {
            SUMHMountConfigDialog("module", "模板测试", null, true, "auto", {}, {}, onSave = { _, _, _ -> })
        } }
        open(); select(R.string.sumh_template_except_children)
        paths(R.string.sumh_template_parent).performScrollTo().performTextReplacement("/vendor/etc")
        paths(R.string.sumh_template_children).performScrollTo().performTextReplacement("/vendor/etc/保留 ")
        review(); compose.onNodeWithText(text(R.string.sumh_template_apply_hint)).performScrollTo().assertIsDisplayed()
        apply().assertIsDisplayed().assertIsEnabled(); capture("module-template-narrow-large-text")
    }

    @Test @Config(qualifiers = "zh-rCN-w760dp-h360dp-land-xhdpi")
    fun landscapeLargeTextKeepsInvalidInputAndActionsReachable() {
        RuntimeEnvironment.setFontScale(2f)
        compose.setContent { MaterialTheme(colorScheme = darkColorScheme()) {
            SUMHMountConfigDialog("module", "模板测试", null, true, "overlay", {}, {}, onSave = { _, _, _ -> })
        } }
        open(); select(R.string.sumh_template_hide)
        paths().performScrollTo().performTextReplacement("/vendor//bad")
        compose.onNodeWithText(text(R.string.sumh_module_rule_canonical)).performScrollTo().assertIsDisplayed()
        apply().assertIsDisplayed().assertIsNotEnabled(); capture("module-template-landscape-large-text")
    }

    private fun capture(name: String) {
        val file = File("build/reports/sumh-layout/$name.png"); file.parentFile?.mkdirs()
        val bitmap = compose.onNode(isDialog()).captureToImage().asAndroidBitmap()
        file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }
}
