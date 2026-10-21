package com.zying.zysu.ui.sumh

import android.app.Application
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.test.junit4.StateRestorationTester
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.SUMHFeatureSupport
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter.MapsPathRule
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter
import org.junit.Rule
import org.junit.After
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.File
import kotlin.test.assertEquals

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "w360dp-h760dp-xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SUMHManagedMapsControlsTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    @After fun resetFontScale() { RuntimeEnvironment.setFontScale(1f) }

    @Test
    fun failedSaveRetainsLiteralDraftUntilRefreshedDefinitionArrives() {
        val saved = mutableStateOf(emptyList<MapsPathRule>())
        var request = emptyList<MapsPathRule>()
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            ManagedMapsControls(saved.value, true, SUMHFeatureSupport.Supported, true, { request = it })
        } } }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_path_add)).performClick()
        compose.onNode(hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_maps_path_target)))
            .performTextReplacement("/data/target ")
        compose.onNode(hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_maps_path_replacement)))
            .performTextReplacement("/system/replacement ")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save)).performClick()
        assertEquals(listOf(MapsPathRule("/data/target ", "/system/replacement ")), request)
        compose.onNode(isDialog()).assertExists()
        compose.onNode(hasSetTextAction() and hasText("/data/target ")).assertExists()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save)).assertIsEnabled()
        capture("managed-maps-draft")
        compose.runOnIdle { saved.value = request }
        compose.onNode(isDialog()).assertDoesNotExist()
        compose.onNodeWithText("/data/target ").assertIsDisplayed()
        capture("managed-maps-saved", dialog = false)
    }

    @Test
    fun unsupportedKernelAllowsRemovingSavedRulesButNotAdding() {
        var request: List<MapsPathRule>? = null
        val rule = MapsPathRule("/target", "/replacement")
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            ManagedMapsControls(listOf(rule), true, SUMHFeatureSupport.Unsupported, false, { request = it })
        } } }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_path_add)).assertIsNotEnabled()
        compose.onNodeWithContentDescription(compose.activity.getString(R.string.sumh_maps_path_remove_label, rule.targetPath)).performClick()
        assertEquals(null, request)
        compose.onNodeWithText(compose.activity.getString(R.string.delete)).performClick()
        assertEquals(emptyList(), request)
    }

    @Test
    fun reapplyUsesExistingActionOnlyWhenMapsIsEnabledAndSupported() {
        val mapsEnabled = mutableStateOf(false)
        val support = mutableStateOf(SUMHFeatureSupport.Supported)
        var requests = 0
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            ManagedMapsControls(listOf(MapsPathRule("/target", "/replacement")), true, support.value,
                mapsEnabled.value, {}, onRetryApply = { requests++ })
        } } }
        val apply = compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_path_apply))
        apply.assertIsNotEnabled()
        compose.runOnIdle { mapsEnabled.value = true }
        apply.assertIsEnabled().performClick()
        assertEquals(1, requests)
        compose.runOnIdle { support.value = SUMHFeatureSupport.Unknown }
        apply.assertIsNotEnabled()
    }

    @Test
    fun knownDriftHasOnlyTheSharedConfigurationRetryAction() {
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            SettingsTab(
                config = SUMHUiAdapter.SUMHPConfig(enableMapsSpoof = true,
                    mapsRules = listOf(MapsPathRule("/target", "/replacement"))),
                sumhStatus = SUMHUiAdapter.SUMHStatus.AVAILABLE,
                features = SUMHUiAdapter.FeaturesResult(0, listOf("maps_spoof", "managed_maps")),
                controlsEnabled = true, runtimeApplyPending = true, onRetryApply = {}, onConfigChanged = {},
                onClearMapRules = {}, onAddMapRule = { _, _, _, _, _ -> }, section = SUMHSection.Isolation,
                controlState = com.zying.zysu.ui.sumh.util.SUMHControlStateFixtures.control(
                    com.zying.zysu.ui.sumh.util.SUMHControlStateFixtures.state("drift")),
            )
        } } }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_state_apply)).performScrollTo().assertIsDisplayed()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_path_apply)).assertDoesNotExist()
    }

    @Test
    fun editingRenamesInOneSaveAndRetainsSeparateDraftsAcrossRestore() {
        val restoration = StateRestorationTester(compose)
        val saved = mutableStateOf(listOf(MapsPathRule("/first", "/replacement"), MapsPathRule("/second", "/other")))
        val requests = mutableListOf<List<MapsPathRule>>()
        restoration.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                ManagedMapsControls(saved.value, true, SUMHFeatureSupport.Supported, true, { requests += it })
            }
        } } }
        fun open(target: String) = compose.onNodeWithContentDescription(
            compose.activity.getString(R.string.sumh_maps_path_edit_label, target)).performScrollTo().performClick()
        val targetField = hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_maps_path_target))
        val replacementField = hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_maps_path_replacement))
        open("/first")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save)).assertIsNotEnabled()
        compose.onNode(targetField).performTextReplacement("/second")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_path_duplicate)).assertIsDisplayed()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save)).assertIsNotEnabled()
        compose.onNode(targetField).performTextReplacement("/renamed ")
        compose.onNode(replacementField).performTextReplacement("/new ")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save)).performClick()
        assertEquals(listOf(listOf(MapsPathRule("/renamed ", "/new "), saved.value[1])), requests)
        compose.onNode(isDialog()).assertExists()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_cancel)).performClick()
        open("/second")
        compose.onNode(replacementField).performTextReplacement("/second draft")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_cancel)).performClick()
        restoration.emulateSavedInstanceStateRestore()
        open("/first")
        compose.onNode(targetField).assertTextContains("/renamed ")
        compose.onNode(replacementField).assertTextContains("/new ")
        capture("managed-maps-edit")
        compose.runOnIdle { saved.value = requests.single() }
        compose.onNode(isDialog()).assertDoesNotExist()
        compose.onNodeWithText("/renamed ").assertExists()
        open("/second")
        compose.onNode(replacementField).assertTextContains("/second draft")
    }

    @Test
    fun sameTargetReadbackKeepsNewerReplacementAndAdvancesDraftBaseline() {
        val saved = mutableStateOf(listOf(MapsPathRule("/target", "/old")))
        var requested = saved.value
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            ManagedMapsControls(saved.value, true, SUMHFeatureSupport.Supported, true, { requested = it })
        } } }
        compose.onNodeWithContentDescription(compose.activity.getString(R.string.sumh_maps_path_edit_label, "/target")).performClick()
        val replacement = compose.onNode(hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_maps_path_replacement)))
        replacement.performTextReplacement("/submitted")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save)).performClick()
        replacement.performTextReplacement("/newer ")
        compose.runOnIdle { saved.value = requested }
        compose.onNode(isDialog()).assertExists()
        replacement.assertTextContains("/newer ")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_editor_draft_conflict)).assertDoesNotExist()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save)).assertIsEnabled()
    }

    @Test
    fun editingShowsFullLongPathAndStopsWhenOriginalWasRemoved() {
        val longPath = "/" + "directory/".repeat(180) + "file "
        val saved = mutableStateOf(listOf(MapsPathRule(longPath, "/replacement")))
        var requests = 0
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            ManagedMapsControls(saved.value, true, SUMHFeatureSupport.Supported, true, { requests++ })
        } } }
        compose.onNodeWithContentDescription(compose.activity.getString(R.string.sumh_maps_path_edit_label, longPath)).performClick()
        compose.onNode(hasSetTextAction() and hasText(longPath)).assertExists()
        compose.onNode(hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_maps_path_replacement)))
            .performTextReplacement("/changed")
        compose.runOnIdle { saved.value = emptyList() }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_path_missing)).assertIsDisplayed()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save)).assertIsNotEnabled()
        assertEquals(0, requests)
    }

    @Test
    fun overBudgetDraftShowsInlineErrorAndCanBeShortenedBeforeSaving() {
        val rules = List(14) { MapsPathRule("/" + "a".repeat(3890) + it.toString().padStart(4, '0'), "/r") }
        var requests = 0
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                ManagedMapsControls(rules, true, SUMHFeatureSupport.Supported, true, { requests++ })
            }
        } } }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_path_add)).performScrollTo().performClick()
        val target = compose.onNode(hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_maps_path_target)))
        target.performTextReplacement("/" + "b".repeat(3000))
        compose.onNode(hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_maps_path_replacement)))
            .performTextReplacement("/replacement")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_path_budget)).performScrollTo().assertIsDisplayed()
        val save = compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save))
        save.assertIsNotEnabled()
        assertEquals(0, requests)
        target.performScrollTo().performTextReplacement("/short")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_path_budget)).assertDoesNotExist()
        save.assertIsEnabled().performClick()
        assertEquals(1, requests)
    }

    @Test
    @Config(qualifiers = "zh-rCN-w760dp-h360dp-land-xhdpi")
    fun darkLandscapeLargeTextKeepsSaveAndFieldErrorsReachable() {
        RuntimeEnvironment.setFontScale(2f)
        compose.setContent { MaterialTheme(colorScheme = darkColorScheme()) { Surface(Modifier.fillMaxSize()) {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                ManagedMapsControls(emptyList(), true, SUMHFeatureSupport.Supported, true, {})
            }
        } } }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_path_add)).performScrollTo().performClick()
        compose.onNode(hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_maps_path_target)))
            .performScrollTo().performTextReplacement("relative")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_input_path_absolute)).performScrollTo().assertIsDisplayed()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save)).assertIsDisplayed().assertIsNotEnabled()
        capture("managed-maps-landscape-large-text")
    }

    private fun capture(name: String, dialog: Boolean = true) {
        val file = File("build/reports/sumh-layout/$name.png")
        file.parentFile?.mkdirs()
        val bitmap = (if (dialog) compose.onNode(isDialog()) else compose.onRoot()).captureToImage().asAndroidBitmap()
        file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }
}
