package com.zying.zysu.ui.sumh

import android.app.Application
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.*
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Density
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.StateRestorationTester
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.*
import kotlinx.coroutines.CompletableDeferred
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.File

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "en-w360dp-h760dp-xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SUMHHideDefinitionsScreenTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    private val revision = "sha256-v1:" + "a".repeat(64)
    private val next = "sha256-v1:" + "b".repeat(64)
    private val row = HideDefinition("1".repeat(32), "/data/disabled ", false, "Personal", "search note", "exact ", false, null, "disabled")
    private val other = row.copy(id = "2".repeat(32), path = "/data/hidden-by-search", name = "Other", note = "", enabled = true, state = "unregistered")
    private fun sample(rows: List<HideDefinition> = listOf(row)) = HideDefinitionCollection(revision, "boot", rows, true, true)
    private fun text(id: Int) = compose.activity.getString(id)
    private fun drafts() = SUMHEditorDrafts.forDirectory(File(compose.activity.applicationContext.noBackupFilesDir, "sumh-drafts"))
    private fun loaded(operation: HideDefinitionOperations) {
        compose.waitUntil(5000) { operation.sample.complete && drafts().loaded && !operation.loading }
        compose.waitForIdle()
    }
    private fun success(action: String = "annotate", ids: List<String> = listOf(row.id)): HideMutationResult {
        val metadata = action == "annotate"
        return HideMutationResult(HideMutationReceipt(action, if (metadata) "saved" else "applied", true, next, revision,
            "boot", "", "", ids.size, if (metadata) 0 else ids.size, 0, 0, true,
            if (metadata) emptyList() else ids.map { HideMutationItem(it, "absent", 0, true, null) }))
    }

    @Test fun disabledRowsAreVisibleSearchableAndMetadataEditableWithoutRuntimeSupport() {
        val operation = HideDefinitionOperations()
        compose.setContent { MaterialTheme { HideDefinitionsScreen(true, {}, { sample(listOf(row, other)).copy(managed = false) },
            { _, _, _ -> error("must not mutate") }, operation) } }
        loaded(operation)
        compose.onNodeWithText(row.path).assertExists()
        compose.onNodeWithTag("hide_definitions_add").assertIsNotEnabled()
        compose.onNode(hasSetTextAction()).performTextReplacement("search note")
        compose.onNodeWithText(other.path).assertDoesNotExist()
        compose.onNodeWithText(row.path).performClick()
        compose.onNodeWithTag("hide_definition_name").assertTextContains(row.name)
        compose.onNodeWithTag("hide_definition_save").assertIsEnabled()
        compose.onNodeWithTag("hide_definition_path").assertDoesNotExist()
    }

    @Test fun failedRefreshRetainsRowsAndDisablesChangesWithoutLegacyFallback() {
        val operation = HideDefinitionOperations(); var current = sample()
        compose.setContent { MaterialTheme { HideDefinitionsScreen(true, {}, { current }, { _, _, _ -> error("must not mutate") }, operation) } }
        loaded(operation)
        compose.runOnIdle { current = HideDefinitionCollection(error = "timeout") }
        compose.onNodeWithTag("hide_definitions_refresh").performClick()
        compose.onNodeWithText(row.path).assertExists()
        compose.onNodeWithTag("hide_definitions_incomplete").assertExists()
        compose.onNodeWithTag("hide_definitions_add").assertIsNotEnabled()
        compose.onNodeWithText(row.path).performClick()
        compose.onNodeWithTag("hide_definition_save").assertDoesNotExist()
    }

    @Test fun wholeGroupConfirmationIncludesFilteredRowsAndBecomesStaleOnRefresh() {
        val operation = HideDefinitionOperations(); var current = sample(listOf(row, other)); var calls = 0
        compose.setContent { MaterialTheme { HideDefinitionsScreen(true, {}, { current }, { _, _, _ -> calls++; success() }, operation) } }
        loaded(operation)
        compose.onNodeWithText(text(R.string.sumh_definitions_groups)).performClick()
        compose.onNode(hasText(row.group) and hasAnyAncestor(isDialog())).performClick()
        compose.onNode(hasSetTextAction()).performTextReplacement("Personal")
        compose.onNodeWithText(text(R.string.sumh_definitions_disable_group)).performClick()
        compose.onNode(hasText(other.path) and hasAnyAncestor(isDialog())).assertExists()
        compose.runOnIdle { current = current.copy(revision = next); operation.refresh { current } }
        compose.onNodeWithText(text(android.R.string.ok)).assertIsNotEnabled()
        compose.onNodeWithText(text(R.string.sumh_definitions_confirmation_changed)).assertExists()
        assertEquals(0, calls)
    }

    @Test fun groupSubmitsExactLiteralOnceAndVerifiesReturnedMembership() {
        val operation = HideDefinitionOperations(); val calls = ArrayList<List<String>>()
        compose.setContent { MaterialTheme { HideDefinitionsScreen(true, {}, { sample(listOf(row, other)) }, { args, _, _ ->
            calls += args; success("disable-group", listOf(row.id)) // Incomplete affected set is never a successful group receipt.
        }, operation) } }
        loaded(operation)
        compose.onNodeWithText(text(R.string.sumh_definitions_groups)).performClick()
        compose.onNode(hasText(row.group) and hasAnyAncestor(isDialog())).performClick()
        compose.onNodeWithText(text(R.string.sumh_definitions_disable_group)).performClick()
        compose.onNodeWithText(text(android.R.string.ok)).performClick()
        compose.waitUntil(5000) { !operation.working && operation.result != null }
        assertEquals(listOf(listOf("hide", "disable-group", row.group, revision)), calls)
        assertFalse(requireNotNull(operation.result).succeeded)
        assertEquals("hide_group_membership_unknown", operation.result?.error)
    }

    @Test fun recreatedCompositionClosesSuccessfulSaveWithoutReplay() = saveAcrossRecreation(success = true, newer = false)
    @Test fun recreatedCompositionRetainsFailedInputWithoutReplay() = saveAcrossRecreation(success = false, newer = false)
    @Test fun olderSuccessfulCompletionCannotEraseNewerDraft() = saveAcrossRecreation(success = true, newer = true)

    @Test fun activityRecreationKeepsViewModelAndConsumesSuccessfulCompletion() {
        val gate = CompletableDeferred<HideMutationResult>(); var calls = 0
        lateinit var first: HideDefinitionOperations
        @Composable fun content(operation: HideDefinitionOperations) { MaterialTheme {
            HideDefinitionsScreen(true, {}, { sample() }, { _, _, _ -> calls++; gate.await() }, operation)
        } }
        fun attach(activity: ComponentActivity): HideDefinitionOperations {
            val operation = ViewModelProvider(activity)[HideDefinitionOperations::class.java]
            activity.setContent { content(operation) }
            return operation
        }
        compose.activityRule.scenario.onActivity { activity ->
            first = attach(activity)
        }
        loaded(first)
        compose.onNodeWithText(row.path).performClick()
        compose.onNodeWithTag("hide_definition_name").performTextReplacement("activity draft")
        compose.onNodeWithTag("hide_definition_save").performClick()
        compose.activityRule.scenario.recreate()
        compose.activityRule.scenario.onActivity { activity ->
            val current = attach(activity)
            assertSame(first, current)
        }
        compose.onNodeWithTag("hide_definition_name").assertTextContains("activity draft")
        compose.runOnIdle { gate.complete(success()) }
        compose.waitUntil(5000) { !first.working }
        compose.onNodeWithTag("hide_definition_save").assertDoesNotExist()
        assertEquals(1, calls)
    }

    @Test @Config(qualifiers = "en-w760dp-h360dp-land-xhdpi")
    fun largeLandscapeSelectionRetainsVisibleRowsAndActions() {
        val operation = HideDefinitionOperations()
        compose.setContent { MaterialTheme {
            CompositionLocalProvider(LocalDensity provides Density(LocalDensity.current.density, 2f)) {
                HideDefinitionsScreen(true, {}, { sample(listOf(row, other)) }, { _, _, _ -> HideMutationResult(error = "unknown") }, operation)
            }
        } }
        loaded(operation)
        compose.onNodeWithText(text(R.string.sumh_rules_select)).performClick()
        compose.onNodeWithText(row.path).assertIsDisplayed().performClick()
        compose.onNodeWithText(row.path).assertIsDisplayed()
        compose.onNodeWithText(text(R.string.sumh_definitions_disable)).assertIsDisplayed()
        val image = compose.onRoot().captureToImage().asAndroidBitmap()
        val file = File("build/reports/sumh-layout/hide-definitions-selection-landscape.png"); file.parentFile!!.mkdirs()
        file.outputStream().use { image.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }

    private fun saveAcrossRecreation(success: Boolean, newer: Boolean) {
        val operation = HideDefinitionOperations(); val gate = CompletableDeferred<HideMutationResult>(); var calls = 0
        val restoration = StateRestorationTester(compose)
        restoration.setContent { MaterialTheme { HideDefinitionsScreen(true, {}, { sample() }, { _, _, _ -> calls++; gate.await() }, operation) } }
        loaded(operation)
        compose.onNodeWithText(row.path).performClick()
        compose.onNodeWithTag("hide_definition_name").performTextReplacement("unfinished")
        compose.onNodeWithTag("hide_definition_save").performClick()
        assertEquals(1, calls)
        restoration.emulateSavedInstanceStateRestore()
        compose.onNodeWithTag("hide_definition_name").assertTextContains("unfinished")
        compose.onNodeWithTag("hide_definition_save").assertIsNotEnabled()
        compose.runOnIdle {
            if (newer) drafts().change("hide-definition:${row.id}") { HideDefinitionDraft.from(requireNotNull(it)).copy(name = "newer").record() }
            gate.complete(if (success) success() else HideMutationResult(error = "timeout_unknown"))
        }
        compose.waitUntil(5000) { !operation.working }
        compose.waitForIdle()
        if (success && !newer) {
            compose.onNodeWithTag("hide_definition_save").assertDoesNotExist()
            assertNull(drafts().snapshot["hide-definition:${row.id}"])
        } else {
            compose.onNodeWithTag("hide_definition_name").assertTextContains(if (newer) "newer" else "unfinished")
            assertNotNull(drafts().snapshot["hide-definition:${row.id}"])
        }
        assertEquals(1, calls)
    }

    @Test fun refreshedEditorRequiresExplicitChoiceAndRemovedDefinitionIsNotRecreated() {
        val operation = HideDefinitionOperations(); var current = sample(); var calls = 0
        compose.setContent { MaterialTheme { HideDefinitionsScreen(true, {}, { current }, { _, _, _ -> calls++; success() }, operation) } }
        loaded(operation)
        compose.onNodeWithText(row.path).performClick()
        compose.onNodeWithTag("hide_definition_name").performTextReplacement("my edit")
        compose.runOnIdle { current = current.copy(revision = next, rows = listOf(row.copy(name = "external"))); operation.refresh { current } }
        compose.onNodeWithTag("hide_definition_save").assertIsNotEnabled()
        compose.onNodeWithTag("hide_definition_name").assertTextContains("my edit")
        compose.onNodeWithText(text(R.string.sumh_definitions_keep_input)).performScrollTo().performClick()
        compose.onNodeWithTag("hide_definition_save").assertIsEnabled()
        compose.runOnIdle { current = current.copy(rows = emptyList()); operation.refresh { current } }
        compose.onNodeWithTag("hide_definition_save").assertIsNotEnabled()
        compose.onNodeWithText(text(R.string.sumh_definitions_removed)).performScrollTo().assertIsDisplayed()
        assertEquals(0, calls)
    }

    @Test @Config(qualifiers = "zh-rCN-w360dp-h760dp-xhdpi")
    fun disabledCreationAndChinesePortraitEditorKeepActionsReachable() = checkEditorLayout(1f, "hide-definition-portrait")

    @Test @Config(qualifiers = "en-w760dp-h360dp-land-xhdpi")
    fun largeLandscapeEditorRetainsInvalidInputAndAccessibleActions() = checkEditorLayout(2f, "hide-definition-landscape")

    private fun checkEditorLayout(scale: Float, imageName: String) {
        val operation = HideDefinitionOperations(); var submitted: List<String>? = null
        compose.setContent { MaterialTheme(colorScheme = darkColorScheme()) {
            CompositionLocalProvider(LocalDensity provides Density(LocalDensity.current.density, scale)) {
                HideDefinitionsScreen(true, {}, { sample(emptyList()) }, { args, _, _ -> submitted = args; HideMutationResult(error = "timeout_unknown") }, operation)
            }
        } }
        loaded(operation)
        compose.onNodeWithTag("hide_definitions_add").performClick()
        compose.onNodeWithTag("hide_definition_path").performScrollTo().performTextReplacement("/data/中文 ")
        compose.onNodeWithTag("hide_definition_name").performScrollTo().performTextReplacement("中".repeat(43))
        compose.onNodeWithTag("hide_definition_save").assertIsNotEnabled()
        compose.onNodeWithTag("hide_definition_name").performTextReplacement("Personal")
        compose.onNodeWithText(text(R.string.sumh_definitions_enable_on_create)).performScrollTo().performClick()
        compose.onNodeWithTag("hide_definition_save").assertIsDisplayed().assertIsEnabled()
        val image = compose.onNode(isDialog()).captureToImage().asAndroidBitmap()
        val file = File("build/reports/sumh-layout/$imageName.png"); file.parentFile!!.mkdirs()
        file.outputStream().use { image.compress(Bitmap.CompressFormat.PNG, 100, it) }
        compose.onNodeWithTag("hide_definition_save").performClick()
        compose.waitUntil(5000) { !operation.working && submitted != null }
        assertFalse(org.json.JSONObject(requireNotNull(submitted).last()).getBoolean("enabled"))
        compose.onNodeWithTag("hide_definition_path").assertTextContains("/data/中文 ")
    }
}
