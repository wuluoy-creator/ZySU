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
import com.zying.zysu.ui.sumh.util.*
import org.junit.After
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
class SUMHModulePreviewDialogTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    @After fun resetFontScale() { RuntimeEnvironment.setFontScale(1f) }
    private fun text(id: Int) = compose.activity.getString(id)
    private fun go(tag: String): SemanticsNodeInteraction {
        compose.onNodeWithTag("module_preview_list").performScrollToNode(hasTestTag(tag))
        return compose.onNodeWithTag(tag)
    }
    private fun section(value: String) = go("module_preview_section_$value").performClick()
    private fun side(value: String) = go("module_preview_side_$value").performClick()
    private fun show(result: SUMHModulePreviewResult? = fixture(), stale: Boolean = false, busy: Boolean = false,
        error: String? = null, candidate: Boolean = true) {
        compose.setContent { MaterialTheme {
            SUMHModulePreviewDialog("module", "/vendor/中文 ", {}, busy, stale, true, candidate,
                result, error, {}, {}, {})
        } }
    }

    @Test fun readsAreExplicitAndExactPathInputIsPreserved() {
        val path = mutableStateOf("")
        var saved = 0; var candidate = 0; var closed = 0
        compose.setContent { MaterialTheme {
            SUMHModulePreviewDialog("module", path.value, { path.value = it }, false, false, true, true,
                null, null, { saved++ }, { candidate++ }, { closed++ })
        } }
        assertEquals(0, saved + candidate)
        go("module_preview_path").performTextReplacement("/vendor/中文 \t")
        assertEquals("/vendor/中文 \t", path.value)
        go("module_preview_read_saved").assertIsEnabled().performClick()
        go("module_preview_read_candidate").assertIsEnabled().performClick()
        assertEquals(1, saved); assertEquals(1, candidate)
        compose.onNodeWithTag("module_preview_close").assertHasClickAction().performClick()
        assertEquals(1, closed)
    }

    @Test fun invalidCandidateStillAllowsSavedReadAndBusyKeepsCloseAvailable() {
        val busy = mutableStateOf(false)
        var saved = 0; var closed = 0
        compose.setContent { MaterialTheme {
            SUMHModulePreviewDialog("module", "", {}, busy.value, false, true, false, null, null,
                { saved++ }, { error("candidate must stay disabled") }, { closed++ }, "Resolve the draft conflict")
        } }
        go("module_preview_read_candidate").assertIsNotEnabled()
        go("module_preview_read_saved").assertIsEnabled().performClick()
        go("module_preview_eligibility").assertTextContains("Resolve the draft conflict")
        compose.runOnIdle { busy.value = true }
        go("module_preview_read_saved").assertIsNotEnabled()
        go("module_preview_status").assertTextEquals(text(R.string.sumh_module_preview_loading))
        compose.onNodeWithTag("module_preview_close").assertIsEnabled().performClick()
        assertEquals(1, saved); assertEquals(1, closed)
    }

    @Test fun staleOrTransportFailureCannotPresentAnEmptySuccessfulDifference() {
        val stale = mutableStateOf(false)
        val failure = mutableStateOf<String?>(null)
        compose.setContent { MaterialTheme {
            SUMHModulePreviewDialog("module", "", {}, false, stale.value, true, true, fixture(), failure.value, {}, {}, {})
        } }
        go("module_preview_status").assertTextEquals(text(R.string.sumh_module_preview_complete))
        side("difference")
        compose.onNodeWithTag("module_preview_list").performScrollToNode(hasText(text(R.string.sumh_module_preview_diff_empty)))
        compose.onNodeWithText(text(R.string.sumh_module_preview_diff_empty)).assertIsDisplayed()
        compose.runOnIdle { stale.value = true }
        go("module_preview_status").assertTextEquals(text(R.string.sumh_module_preview_local_stale))
        compose.onNodeWithTag("module_preview_list").performScrollToNode(hasText(text(R.string.sumh_module_preview_diff_unavailable)))
        compose.onNodeWithText(text(R.string.sumh_module_preview_diff_empty)).assertDoesNotExist()
        compose.runOnIdle { stale.value = false; failure.value = "transport timeout" }
        go("module_preview_status").assertTextEquals(text(R.string.sumh_module_preview_failed))
        go("module_preview_failure").assertTextContains("transport timeout")
    }

    @Test fun earlyUnknownIsNotAnEmptyPlanAndSavedOnlyHasNoCandidateTabs() {
        val value = mutableStateOf(SUMHModulePreviewResult(SUMHModulePreview(SUMHModulePreviewStatus.Unknown,
            "namespace_unavailable", 13, false, emptyList(), null, null)))
        compose.setContent { MaterialTheme {
            SUMHModulePreviewDialog("module", "", {}, false, false, true, true, value.value, null, {}, {}, {})
        } }
        go("module_preview_status").assertTextEquals(text(R.string.sumh_module_preview_unknown))
        compose.onNodeWithTag("module_preview_side_saved").assertDoesNotExist()
        compose.runOnIdle { value.value = fixture(candidate = false) }
        go("module_preview_side_saved").assertIsDisplayed()
        compose.onNodeWithTag("module_preview_side_candidate").assertDoesNotExist()
        compose.onNodeWithTag("module_preview_side_difference").assertDoesNotExist()
    }

    @Test fun frozenRequestPathIsDistinctFromNewEditableInput() {
        val request = SUMHModulePreviewRequest.candidate("module", "sumh",
            listOf(SUMHManager.ModuleRule("/vendor/old", "hide")), "/vendor/submitted ")
        compose.setContent { MaterialTheme {
            SUMHModulePreviewDialog("module", "/vendor/new input", {}, false, true, true, true,
                null, null, {}, {}, {}, request = request)
        } }
        go("module_preview_path").assertTextContains("/vendor/new input")
        go("module_preview_frozen_path").assertTextContains("\"/vendor/submitted \"")
        go("module_preview_frozen_revision").assertTextContains(requireNotNull(request.candidateRevision), substring = true)
    }

    @Test fun partialInvalidAndNativeStaleRemainDistinctFromComplete() {
        val result = mutableStateOf(fixture())
        compose.setContent { MaterialTheme {
            SUMHModulePreviewDialog("module", "", {}, false, false, true, true, result.value, null, {}, {}, {})
        } }
        listOf(SUMHModulePreviewStatus.Partial to R.string.sumh_module_preview_partial,
            SUMHModulePreviewStatus.Invalid to R.string.sumh_module_preview_invalid,
            SUMHModulePreviewStatus.Stale to R.string.sumh_module_preview_native_stale).forEach { (status, label) ->
            compose.runOnIdle { result.value = SUMHModulePreviewResult(SUMHModulePreview(status, "test", 0, false, emptyList(), null, null)) }
            go("module_preview_status").assertTextEquals(text(label))
        }
    }

    @Test fun pathExplanationKeepsAliasHiddenAncestorAndEarlierProvider() {
        val event = event()
        show(fixture(plan = plan().copy(explainEvents = listOf(event), trace = listOf(event), hiddenAncestors = listOf(bytes("/vendor/hidden")))))
        section("explanation")
        go("module_preview_saved_explanation_hidden_0").assertTextContains("\"/vendor/hidden\"")
        go("module_preview_saved_explanation_0_matched").assertTextContains("\"/vendor\"")
        go("module_preview_saved_explanation_0_original").assertTextContains("\"/system/vendor/中文 \"")
        go("module_preview_saved_explanation_0_resolved").assertTextContains("\"/vendor/中文 \"")
        go("module_preview_saved_explanation_0_previous_module").assertTextContains("\"previous.module\"")
        go("module_preview_saved_explanation_0_previous_source").assertTextContains("\"/data/adb/modules/previous/source\"")
    }

    @Test fun emptyExplanationAndUnknownOwnerKeepProofLimits() {
        show()
        section("explanation")
        compose.onNodeWithTag("module_preview_list").performScrollToNode(hasText(text(R.string.sumh_module_preview_no_events)))
        compose.onNodeWithText(text(R.string.sumh_module_preview_no_events)).assertIsDisplayed()
        section("diagnostics")
        go("module_preview_saved_diagnostics_owner").assertTextContains("external_owner=not_observed", substring = true)
        go("module_preview_saved_diagnostics_runtime").assertTextContains("runtime_known=false", substring = true)
    }

    @Test fun repeatedTargetsRemainLazyAndLastContributionIsReachable() {
        val contributions = List(1024) { contribution(it) }
        show(fixture(plan = plan().copy(contributions = contributions)))
        section("actions")
        compose.onNodeWithTag("module_preview_saved_actions_1023").assertDoesNotExist()
        go("module_preview_saved_actions_1023").assertTextContains("#1023 ADD")
        go("module_preview_saved_actions_1023_source").assertTextContains("\"/data/source/1023\"")
    }

    @Test fun sideAndSectionRestoreWithoutCallingReaders() {
        val restore = StateRestorationTester(compose)
        var reads = 0
        restore.setContent { MaterialTheme {
            SUMHModulePreviewDialog("module", "/vendor/中文 ", {}, false, false, true, true, fixture(), null,
                { reads++ }, { reads++ }, {})
        } }
        side("candidate"); section("configuration")
        restore.emulateSavedInstanceStateRestore()
        go("module_preview_side_candidate").assertIsSelected()
        go("module_preview_section_configuration").assertIsSelected()
        assertEquals(0, reads)
    }

    @Test @Config(qualifiers = "zh-rCN-w360dp-h760dp-xhdpi")
    fun narrowChineseLargeTextKeepsManualActionsAndCloseReachable() {
        RuntimeEnvironment.setFontScale(2f)
        show()
        go("module_preview_read_candidate").assertIsDisplayed().assertIsEnabled()
        compose.onNodeWithTag("module_preview_close").assertIsDisplayed()
        capture("module-preview-narrow-large-text")
    }

    @Test @Config(qualifiers = "zh-rCN-w760dp-h360dp-land-xhdpi")
    fun landscapeLargeTextErrorKeepsInputAndCloseReachable() {
        RuntimeEnvironment.setFontScale(2f)
        compose.setContent { MaterialTheme(colorScheme = darkColorScheme()) {
            SUMHModulePreviewDialog("module", "/vendor/中文 ", {}, false, false, true, false, null,
                "Read failed; original draft retained", {}, {}, {})
        } }
        go("module_preview_failure").assertTextContains("Read failed; original draft retained")
        compose.onNodeWithTag("module_preview_close").assertIsDisplayed()
        capture("module-preview-landscape-large-text")
    }

    @Test fun exactBytesAndLargeDiagnosticChunksPreserveAllData() {
        assertEquals("\"/vendor/中文 \\t\"", modulePreviewBytesLabel(bytes("/vendor/中文 \t")))
        assertEquals("hex:ff00", modulePreviewBytesLabel(SUMHInventoryBytes("ff00", null)))
        val value = "x".repeat(2047) + "😀" + "z".repeat(7000)
        val chunks = modulePreviewTextChunks(value)
        assertEquals(value, chunks.joinToString(""))
        assertTrue(chunks.all { it.length <= 2048 && !it.last().isHighSurrogate() && !it.first().isLowSurrogate() })
    }

    private fun capture(name: String) {
        val file = File("build/reports/sumh-layout/$name.png"); file.parentFile?.mkdirs()
        val bitmap = compose.onNode(isDialog()).captureToImage().asAndroidBitmap()
        file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }

    private fun bytes(value: String): SUMHInventoryBytes = SUMHInventoryBytes(
        value.toByteArray(Charsets.UTF_8).joinToString("") { "%02x".format(it.toInt() and 255) }, value)
    private fun contribution(index: Int) = SUMHModuleContribution(index, "ADD", bytes("/vendor/same"), bytes("/data/source/$index"),
        "existing_source_write_through_not_read_only", true, emptyList())
    private fun event() = SUMHModulePlanEvent(
        moduleId = bytes("module"), originalPath = bytes("/system/vendor/中文 "), resolvedPath = bytes("/vendor/中文 "),
        source = bytes("/data/source"), action = "ADD", reason = "direct_override", matchedRulePath = bytes("/vendor"),
        requestedMode = "sumh", effectiveMode = "sumh", hiddenParent = bytes("/vendor/hidden"), alias = true, mappingIndex = 0,
        hasPreviousDirect = true, previousModuleId = bytes("previous.module"), previousSource = bytes("/data/adb/modules/previous/source"), matchesExplain = true,
    )
    private fun plan(): SUMHModulePlan {
        val probe = SUMHModuleBackendProbe(true, false, null, 13, bytes("probe unavailable"))
        val decision = SUMHModuleBackendDecision(bytes("module"), bytes("/data/adb/modules/module"), "auto", "sumh", "sumh", "global",
            "sumh", "global_override", false, true, true, false, bytes(""), probe, probe)
        return SUMHModulePlan(listOf(decision), true, true, false, listOf(bytes("module")), bytes(""), false, false,
            emptyList(), emptyList(), emptyList(), emptyList(), 0, "source-revision", "plan-revision", true)
    }
    private fun fixture(candidate: Boolean = true, plan: SUMHModulePlan = plan()): SUMHModulePreviewResult {
        val config = SUMHModuleConfiguration("sumh", false, false, listOf(bytes("system")),
            listOf(SUMHModuleSavedConfiguration(bytes("module"), "sumh", listOf(SUMHModuleSavedRule(bytes("/vendor/中文 "), "sumh")))))
        val doc = SUMHModuleDocument("missing", 0, null)
        val ownership = SUMHModuleOwnership("missing", null, SUMHModuleBootPlan(doc, null, null, null, emptyList()))
        val sample = SUMHModulePreviewSample("boot", "123", true, "config-revision", "module-revision",
            if (candidate) "candidate-revision" else null, plan, if (candidate) plan else null,
            if (candidate) SUMHModulePlanDiff(true, emptyList()) else null, config, if (candidate) config else null,
            emptyList(), SUMHModuleRuntimeContext(false, "10", "20"), ownership, bytes("/vendor/中文 "), bytes("/vendor/中文 "), emptyList(), mapOf("config" to doc))
        return SUMHModulePreviewResult(SUMHModulePreview(SUMHModulePreviewStatus.Complete, "sumh_compilation_complete", 0, true,
            listOf("observer_visibility", "external_mount_owner"), null, sample))
    }
}
