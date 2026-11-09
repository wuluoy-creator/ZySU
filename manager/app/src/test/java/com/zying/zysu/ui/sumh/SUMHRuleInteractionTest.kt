package com.zying.zysu.ui.sumh

import android.app.Application
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.test.junit4.StateRestorationTester
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter.ActiveRule
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter
import com.zying.zysu.ui.sumh.util.SUMHManagedFeature
import com.zying.zysu.ui.sumh.util.SUMHHideBatchResult
import com.zying.zysu.ui.sumh.util.SUMHHideItemResult
import com.zying.zysu.ui.util.rememberSnackbarController
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.File
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "w360dp-h760dp-xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SUMHRuleInteractionTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    @Test
    @Config(qualifiers = "zh-rCN-w360dp-h760dp-xhdpi")
    fun narrowRulesPageOnlyShowsBatchActionsWhenSelected() {
        val path = "/storage/emulated/0/My Files/config "
        val rules = listOf(
            ActiveRule("hide", path, isUserDefined = true, hideState = 2),
            ActiveRule("hide", "/data/adb/modules/example/system/app", isUserDefined = true, hideState = 0),
        )
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            UserHideRulesScreen(true, rememberSnackbarController(), {}, true, loadRules = { rules })
        } } }
        val remove = compose.activity.getString(R.string.delete)
        compose.onNodeWithText(remove).assertDoesNotExist()
        capture("rules-page", dialog = false)
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_select)).performClick()
        compose.onNodeWithText(path).performClick()
        compose.onNodeWithText(remove).assertIsDisplayed().assertIsEnabled()
        capture("rules-selection", dialog = false)
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_done)).performClick()
        compose.onNodeWithText(remove).assertDoesNotExist()
        compose.onNodeWithText(path).performClick()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save)).assertIsDisplayed()
        compose.onNode(hasSetTextAction() and hasText(path)).assertExists()
    }

    @Test
    fun tappingPathEditsDirectlyAndSelectionModeSelectsInstead() {
        var edited: String? = null
        var selected: String? = null
        val selecting = mutableStateOf(false)
        val path = "/storage/emulated/0/My Files/config "
        compose.setContent {
            MaterialTheme { Surface { UserHideRuleList(listOf(ActiveRule("hide", path, isUserDefined = true)),
                selecting.value, emptySet(), true, Modifier.fillMaxSize(),
                onSelect = { selected = it }, onEdit = { edited = it.src }, onRemove = {}, onRetry = {}, onCopy = {}, retryEnabled = true) } }
        }
        compose.onNodeWithText(path).performClick()
        assertEquals(path, edited)
        compose.runOnIdle { selecting.value = true; edited = null }
        compose.onNodeWithText(path).performClick()
        assertEquals(path, selected)
        assertEquals(null, edited)
    }

    @Test
    fun editKeepsLiteralPathAndSubmitsWithoutPreview() {
        var submitted: List<String>? = null
        val path = "/data/../data/My Files/ "
        compose.setContent { MaterialTheme {
            Surface(Modifier.fillMaxSize()) {}
            var draft by remember { mutableStateOf("/data/old") }
            UserHideRuleEditor("/data/old", draft, false, false, true, emptySet(), null,
                onDraftChange = { draft = it }, onDismiss = {}, onSubmit = { submitted = it })
        } }
        compose.onNode(hasSetTextAction()).performTextReplacement(path)
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save)).assertIsEnabled().performClick()
        assertEquals(listOf(path), submitted)
        capture("edit-path")
    }

    @Test
    fun bulkImportSubmitsValidRowsWhileInvalidRowsRemainEditable() {
        var submitted: List<String>? = null
        compose.setContent { MaterialTheme {
            Surface(Modifier.fillMaxSize()) {}
            var draft by remember { mutableStateOf("/data/new \nrelative\n/data/existing\n/data/new ") }
            UserHideRuleEditor(null, draft, true, false, true, setOf("/data/existing"), null,
                onDraftChange = { draft = it }, onDismiss = {}, onSubmit = { submitted = it })
        } }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_add_valid)).assertIsEnabled().performClick()
        assertEquals(listOf("/data/new "), submitted)
        compose.onNode(hasSetTextAction()).assertTextContains("relative", substring = true)
        capture("import-paths")
    }

    @Test
    @Config(qualifiers = "zh-rCN-w360dp-h760dp-xhdpi")
    fun narrowDarkLargeTextKeepsEditSaveAccessible() {
        RuntimeEnvironment.setFontScale(2f)
        compose.setContent { MaterialTheme(colorScheme = darkColorScheme()) {
            Surface(Modifier.fillMaxSize()) {}
            UserHideRuleEditor("/data/old", "/data/My Files/配置 ", false, false, true, emptySet(), null,
                onDraftChange = {}, onDismiss = {}, onSubmit = {})
        } }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save)).assertIsDisplayed().assertIsEnabled()
        capture("edit-dark-large-text")
    }

    @Test
    @Config(qualifiers = "zh-rCN-w360dp-h640dp-xhdpi")
    fun largeTextRuleListKeepsSelectedActionsAndRowsReachable() {
        RuntimeEnvironment.setFontScale(2f)
        val path = "/data/0"
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            UserHideRulesScreen(true, rememberSnackbarController(), {}, true,
                loadRules = { (0..40).map { ActiveRule("hide", "/data/$it", hideState = 2) } })
        } } }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_select)).performClick()
        compose.onNodeWithText(path).performClick()
        compose.onNodeWithText(compose.activity.getString(R.string.delete)).assertIsDisplayed().assertIsEnabled()
        compose.onNode(hasScrollToIndexAction()).performScrollToIndex(40)
        compose.onNodeWithText("/data/40").assertIsDisplayed()
        capture("rules-large-text", dialog = false)
    }

    @Test
    @Config(qualifiers = "zh-rCN-w760dp-h360dp-land-xhdpi")
    fun landscapeImportKeepsConfirmButtonAccessible() {
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {}
            UserHideRuleEditor(null, "/data/a\n/data/b\nrelative", true, false, true, emptySet(), null,
                onDraftChange = {}, onDismiss = {}, onSubmit = {})
        } }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_add_valid)).assertIsDisplayed().assertIsEnabled()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_user_rules_preview_line, 3, "relative"), substring = true)
            .performScrollTo().assertIsDisplayed()
        capture("import-landscape")
    }

    @Test
    @Config(qualifiers = "zh-rCN-w360dp-h760dp-xhdpi")
    fun standardPageContainsKernelSpoofingWithoutAutomaticFeatureSwitches() {
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            SettingsTab(
                config = SUMHUiAdapter.SUMHPConfig(
                    enableKernelBuildSpoof = true, kernelBuildRelease = "6.1.75-custom"),
                sumhStatus = SUMHUiAdapter.SUMHStatus.AVAILABLE,
                features = SUMHUiAdapter.FeaturesResult(0, SUMHManagedFeature.entries.map { it.capability }),
                controlsEnabled = true, runtimeApplyPending = false, onRetryApply = {}, onConfigChanged = {},
                section = SUMHSection.Isolation,
            )
        } } }
        compose.onAllNodes(hasSetTextAction()).assertCountEquals(0)
        capture("isolation-page", dialog = false)
        compose.onAllNodes(isSelectable()).assertCountEquals(0)
        listOf(
            R.string.sumh_stealth,
            R.string.sumh_mount_hide,
            R.string.sumh_overlay_xattr,
            R.string.sumh_statfs_spoof,
        ).forEach { title -> compose.onNodeWithText(compose.activity.getString(title)).assertDoesNotExist() }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_spoof)).assertDoesNotExist()
        compose.onAllNodes(isToggleable()).assertCountEquals(0)
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kernel_build_spoof))
            .performScrollTo().assertIsDisplayed()
    }

    @Test
    fun standardPageDoesNotOfferAutomaticFeatureCleanup() {
        val config = SUMHUiAdapter.SUMHPConfig()
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            SettingsTab(config = config, sumhStatus = SUMHUiAdapter.SUMHStatus.AVAILABLE,
                features = SUMHUiAdapter.FeaturesResult(0, emptyList()),
                controlsEnabled = true, runtimeApplyPending = false, onRetryApply = {}, onConfigChanged = {},
                section = SUMHSection.Isolation)
        } } }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_statfs_requires_mount_hide), substring = true)
            .assertDoesNotExist()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_features_cleanup)).assertDoesNotExist()
        compose.onAllNodes(isToggleable()).assertCountEquals(0)
    }

    @Test
    @Config(qualifiers = "zh-rCN-w375dp-h760dp-xhdpi")
    fun kernelSpoofSavesDirectlyAndEmptyFieldsRestoreOriginalValues() {
        val config = mutableStateOf(SUMHUiAdapter.SUMHPConfig(
            enableKernelBuildSpoof = false, kernelBuildRelease = "6.1.75-saved",
        ))
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            SettingsTab(config.value, SUMHUiAdapter.SUMHStatus.AVAILABLE,
                SUMHUiAdapter.FeaturesResult(0, listOf("kernel_build_spoof")),
                true, false, {}, { config.value = it }, SUMHSection.Isolation)
        } } }
        val title = compose.activity.getString(R.string.sumh_kernel_build_spoof)
        val release = hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_kernel_build_release))
        val version = hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_kernel_build_version))
        val save = compose.activity.getString(R.string.sumh_rule_save)
        compose.onNodeWithText(title).performClick()
        compose.onNode(release).assertEditableText("6.1.75-saved")
        compose.onNode(release).performTextReplacement(" 6.1.75-custom ")
        compose.onNode(version).performTextReplacement(" #1 SMP PREEMPT ")
        compose.onAllNodes(isToggleable()).assertCountEquals(0)
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kernel_build_apply_stage)).assertDoesNotExist()
        capture("kernel-spoof-edit")
        compose.onNodeWithText(save).assertIsEnabled().performClick()
        compose.onNode(isDialog()).assertDoesNotExist()
        assertTrue(config.value.enableKernelBuildSpoof)
        assertEquals("6.1.75-custom", config.value.kernelBuildRelease)
        assertEquals("#1 SMP PREEMPT", config.value.kernelBuildVersion)
        assertEquals("post-fs-data", config.value.kernelBuildApplyStage)

        compose.onNodeWithText(title).performClick()
        compose.onNode(release).performTextReplacement("")
        compose.onNode(version).performTextReplacement("")
        compose.onNodeWithText(save).assertIsEnabled().performClick()
        compose.onNode(isDialog()).assertDoesNotExist()
        assertFalse(config.value.enableKernelBuildSpoof)
        assertEquals("", config.value.kernelBuildRelease)
        assertEquals("", config.value.kernelBuildVersion)
    }

    @Test
    @Config(qualifiers = "zh-rCN-w760dp-h375dp-land-xhdpi")
    fun kernelSpoofRejectsInvalidInputAndKeepsDraftWhenSaveFails() {
        RuntimeEnvironment.setFontScale(1.5f)
        var attempts = 0
        compose.setContent { MaterialTheme(colorScheme = darkColorScheme()) { Surface(Modifier.fillMaxSize()) {
            SettingsTab(SUMHUiAdapter.SUMHPConfig(), SUMHUiAdapter.SUMHStatus.AVAILABLE,
                SUMHUiAdapter.FeaturesResult(0, listOf("kernel_build_spoof")),
                true, false, {}, { attempts++ }, SUMHSection.Isolation)
        } } }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kernel_build_spoof)).performClick()
        val release = hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_kernel_build_release))
        val save = compose.activity.getString(R.string.sumh_rule_save)
        compose.onNode(release).performTextReplacement("invalid release")
        compose.onNodeWithText(save).assertIsNotEnabled()
        compose.onNode(release).performTextReplacement("6.1.75-custom")
        compose.onNodeWithText(save).assertIsDisplayed().performClick()
        assertEquals(1, attempts)
        compose.onNode(isDialog()).assertExists()
        compose.onNode(release).assertEditableText("6.1.75-custom")
        capture("kernel-spoof-landscape-dark-large-text")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_cancel)).assertIsDisplayed().performClick()
        compose.onNode(isDialog()).assertDoesNotExist()
    }

    @Test
    fun unfinishedImportRetriesPersistedPathsWithoutResubmittingSuccesses() {
        val saved = mutableListOf<ActiveRule>()
        val requests = mutableListOf<List<String>>()
        var refreshedSavedPaths = emptyList<String>()
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            UserHideRulesScreen(true, rememberSnackbarController(), {}, true, loadRules = {
                saved.toList().also { refreshedSavedPaths = it.map { rule -> rule.src } }
            },
                changeRules = { action, paths, _, progress ->
                    kotlinx.coroutines.delay(32)
                    requests += paths
                    saved.clear()
                    saved += listOf(ActiveRule("hide", "/data/ok"), ActiveRule("hide", "/data/retry "))
                    progress(paths.size, paths.size)
                    SUMHHideBatchResult(action, paths.map { SUMHHideItemResult(it,
                        if (requests.size == 1 && it == "/data/retry ") "Saved but kernel rejected" else null) })
                })
        } } }
        compose.onNodeWithContentDescription(compose.activity.getString(R.string.sumh_rules_more)).performClick()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_import)).performClick()
        compose.onNode(hasSetTextAction() and hasAnyAncestor(isDialog())).performTextReplacement("/data/ok\n/data/retry \nrelative")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_add_valid)).performClick()
        compose.onNode(hasSetTextAction() and hasAnyAncestor(isDialog())).assertEditableText("/data/retry \nrelative")
        assertEquals(listOf("/data/ok", "/data/retry "), refreshedSavedPaths)
        capture("import-retry")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_continue_import)).assertIsEnabled().performClick()
        compose.onNode(hasSetTextAction() and hasAnyAncestor(isDialog())).assertEditableText("relative")
        assertEquals(listOf(listOf("/data/ok", "/data/retry "), listOf("/data/retry ")), requests)
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_add_valid)).assertIsNotEnabled()
    }

    @Test
    fun draftsSurviveSwitchingRulesAndScreenStateRestoration() {
        val restoration = StateRestorationTester(compose)
        restoration.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            UserHideRulesScreen(true, rememberSnackbarController(), {}, true,
                loadRules = { listOf(ActiveRule("hide", "/data/a"), ActiveRule("hide", "/data/b")) })
        } } }
        val field = hasSetTextAction() and hasAnyAncestor(isDialog())
        compose.onNodeWithText("/data/a").performClick()
        compose.onNode(field).performTextReplacement("/data/a edited ")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_cancel)).performClick()
        compose.onNodeWithText("/data/b").performClick()
        compose.onNode(field).performTextReplacement("/data/b edited")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_cancel)).performClick()
        restoration.emulateSavedInstanceStateRestore()
        compose.onNodeWithText("/data/a").performClick()
        compose.onNode(field).assertEditableText("/data/a edited ")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_reset_draft)).performClick()
        compose.onNode(field).assertEditableText("/data/a")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_cancel)).performClick()
        compose.onNodeWithText("/data/b").performClick()
        compose.onNode(field).assertEditableText("/data/b edited")
    }

    @Test
    fun longPressSelectsAndBackExitsSelectionBeforeLeavingThePage() {
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            UserHideRulesScreen(true, rememberSnackbarController(), {}, true,
                loadRules = { listOf(ActiveRule("hide", "/data/a"), ActiveRule("hide", "/data/b")) })
        } } }
        compose.onNodeWithText("/data/a").performTouchInput { longClick() }
        compose.onNodeWithText("/data/a").assertIsOn()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_invert_visible)).performClick()
        compose.onNodeWithText("/data/a").assertIsOff()
        compose.onNodeWithText("/data/b").assertIsOn()
        compose.runOnUiThread { compose.activity.onBackPressedDispatcher.onBackPressed() }
        compose.onNodeWithText(compose.activity.getString(R.string.delete)).assertDoesNotExist()
        compose.onNodeWithText("/data/a").performClick()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save)).assertIsDisplayed()
    }

    @Test
    fun narrowingSearchAfterScrollingShowsItsFirstMatchAndOffersResetForNoMatches() {
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            UserHideRulesScreen(true, rememberSnackbarController(), {}, true,
                loadRules = { (0..100).map { ActiveRule("hide", "/data/$it") } })
        } } }
        compose.onNode(hasScrollToIndexAction()).performScrollToIndex(80)
        compose.onNode(hasScrollToIndexAction()).performScrollToNode(hasSetTextAction())
        compose.onNode(hasSetTextAction()).performTextReplacement("/data/1")
        compose.onNode(hasText("/data/1") and !hasSetTextAction()).assertIsDisplayed()
        compose.onNode(hasSetTextAction()).performTextReplacement("no match")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_clear_filter)).performClick()
        compose.onNodeWithText("/data/0").assertIsDisplayed()
    }

    private fun SemanticsNodeInteraction.assertEditableText(value: String) =
        assert(SemanticsMatcher.expectValue(SemanticsProperties.EditableText, AnnotatedString(value)))

    private fun capture(name: String, dialog: Boolean = true) {
        val file = File("build/reports/sumh-layout/$name.png")
        file.parentFile?.mkdirs()
        val bitmap = (if (dialog) compose.onNode(isDialog()) else compose.onRoot()).captureToImage().asAndroidBitmap()
        file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }
}
