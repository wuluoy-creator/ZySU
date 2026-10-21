package com.zying.zysu.ui.sumh

import android.app.Application
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.StateRestorationTester
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.KERNEL_BUILD_EARLY_STAGE
import com.zying.zysu.ui.sumh.util.KERNEL_BUILD_LATE_STAGE
import com.zying.zysu.ui.sumh.util.SUMHFeatureSupport
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as Controller
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
class SUMHDraftRefreshTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    private val saved = Controller.SUMHPConfig(enableKernelBuildSpoof = true,
        kernelBuildRelease = "6.1.0-original", kernelBuildVersion = "#1 original", kernelBuildApplyStage = KERNEL_BUILD_EARLY_STAGE)
    private fun text(id: Int) = compose.activity.getString(id)
    private fun field(id: Int) = compose.onNode(hasSetTextAction() and hasText(text(id)))
    private fun action(id: Int, label: Int) = compose.onNodeWithText(compose.activity.getString(id, text(label)))
    private fun saveBuild() = compose.onNodeWithText(text(R.string.sumh_rule_save))

    @Test fun cleanKernelFieldsAdoptExternalSavedValues() {
        val config = mutableStateOf(saved)
        compose.setContent { MaterialTheme { Column(Modifier.verticalScroll(rememberScrollState())) {
            KernelBuildSpoofEditor(config.value, true, KernelBuildRead.Unavailable, {}, { _, _, _ -> })
        } } }
        compose.runOnIdle { config.value = saved.copy(kernelBuildRelease = "6.1.1-external", kernelBuildVersion = "#2 external",
            kernelBuildApplyStage = KERNEL_BUILD_LATE_STAGE) }
        field(R.string.sumh_kernel_build_release).assertTextContains("6.1.1-external")
        field(R.string.sumh_kernel_build_version).assertTextContains("#2 external")
        action(R.string.sumh_field_draft_keep, R.string.sumh_kernel_build_release).assertDoesNotExist()
        saveBuild().assertIsNotEnabled()
    }

    @Test fun dirtyReleaseSurvivesRefreshAndRestorationWhileCleanVersionAdoptsRefresh() {
        val restoration = StateRestorationTester(compose)
        val config = mutableStateOf(saved)
        var request: Triple<String, String, String>? = null
        restoration.setContent { MaterialTheme { Column(Modifier.verticalScroll(rememberScrollState())) {
            KernelBuildSpoofEditor(config.value, true, KernelBuildRead.Failed, {}, { r, v, s -> request = Triple(r, v, s) })
        } } }
        field(R.string.sumh_kernel_build_release).performTextReplacement("6.1.2-draft")
        compose.runOnIdle { config.value = saved.copy(kernelBuildRelease = "6.1.3-external", kernelBuildVersion = "#2 external") }
        restoration.emulateSavedInstanceStateRestore()
        field(R.string.sumh_kernel_build_release).assertTextContains("6.1.2-draft")
        field(R.string.sumh_kernel_build_version).assertTextContains("#2 external")
        saveBuild().assertIsNotEnabled()
        action(R.string.sumh_field_draft_keep, R.string.sumh_kernel_build_release).performScrollTo().performClick()
        saveBuild().performScrollTo().performClick()
        assertEquals(Triple("6.1.2-draft", "#2 external", KERNEL_BUILD_EARLY_STAGE), request)
    }

    @Test fun decisionsAreFieldLocalWhenBothKernelFieldsConflict() {
        val config = mutableStateOf(saved)
        var request: Triple<String, String, String>? = null
        compose.setContent { MaterialTheme { Column(Modifier.verticalScroll(rememberScrollState())) {
            KernelBuildSpoofEditor(config.value, true, KernelBuildRead.Unavailable, {}, { r, v, s -> request = Triple(r, v, s) })
        } } }
        field(R.string.sumh_kernel_build_release).performTextReplacement("6.1.2-draft")
        field(R.string.sumh_kernel_build_version).performScrollTo().performTextReplacement("#2 草稿 ")
        compose.runOnIdle { config.value = saved.copy(kernelBuildRelease = "6.1.3-external", kernelBuildVersion = "#3 external") }
        action(R.string.sumh_field_draft_use_saved, R.string.sumh_kernel_build_release).performScrollTo().performClick()
        field(R.string.sumh_kernel_build_release).assertTextContains("6.1.3-external")
        field(R.string.sumh_kernel_build_version).assertTextContains("#2 草稿 ")
        saveBuild().assertIsNotEnabled()
        action(R.string.sumh_field_draft_keep, R.string.sumh_kernel_build_version).performScrollTo().performClick()
        saveBuild().performScrollTo().performClick()
        assertEquals(Triple("6.1.3-external", "#2 草稿 ", KERNEL_BUILD_EARLY_STAGE), request)
    }

    @Test fun savedSubmissionAdvancesBaselineWithoutErasingNewerKernelInput() {
        val restoration = StateRestorationTester(compose)
        val config = mutableStateOf(saved)
        var request: Triple<String, String, String>? = null
        restoration.setContent { MaterialTheme { Column(Modifier.verticalScroll(rememberScrollState())) {
            KernelBuildSpoofEditor(config.value, true, KernelBuildRead.Failed, {}, { r, v, s -> request = Triple(r, v, s) })
        } } }
        field(R.string.sumh_kernel_build_release).performTextReplacement("6.1.2-submitted")
        field(R.string.sumh_kernel_build_version).performScrollTo().performTextReplacement("#2 submitted")
        saveBuild().performScrollTo().performClick()
        field(R.string.sumh_kernel_build_version).performScrollTo().performTextReplacement("#3 newer ")
        restoration.emulateSavedInstanceStateRestore()
        compose.runOnIdle {
            val confirmed = requireNotNull(request)
            config.value = saved.copy(kernelBuildRelease = confirmed.first, kernelBuildVersion = confirmed.second)
        }
        field(R.string.sumh_kernel_build_release).assertTextContains("6.1.2-submitted")
        field(R.string.sumh_kernel_build_version).assertTextContains("#3 newer ")
        action(R.string.sumh_field_draft_keep, R.string.sumh_kernel_build_version).assertDoesNotExist()
        saveBuild().assertIsEnabled()
    }

    @Test fun unconfirmedKernelSubmissionAndUnrelatedRefreshKeepDraft() {
        val config = mutableStateOf(saved)
        var requests = 0
        compose.setContent { MaterialTheme { Column(Modifier.verticalScroll(rememberScrollState())) {
            KernelBuildSpoofEditor(config.value, true, KernelBuildRead.Unavailable, {}, { _, _, _ -> requests++ })
        } } }
        field(R.string.sumh_kernel_build_version).performScrollTo().performTextReplacement("#2 unknown result ")
        saveBuild().performScrollTo().performClick()
        compose.runOnIdle { config.value = saved.copy(debug = true) }
        field(R.string.sumh_kernel_build_version).assertTextContains("#2 unknown result ")
        saveBuild().assertIsEnabled()
        assertEquals(1, requests)
    }

    @Test fun firstEnableConfirmationDoesNotHideNewerKernelDraft() {
        val config = mutableStateOf(Controller.SUMHPConfig())
        var requested: Controller.SUMHPConfig? = null
        compose.setContent { MaterialTheme { Column(Modifier.verticalScroll(rememberScrollState())) {
            KernelBuildSpoofCard(config.value, true, true, SUMHFeatureSupport.Supported, { requested = it })
        } } }
        compose.onNodeWithText(text(R.string.sumh_kernel_build_enable)).performClick()
        field(R.string.sumh_kernel_build_release).performScrollTo().performTextReplacement("6.1.2-submitted")
        saveBuild().performScrollTo().performClick()
        field(R.string.sumh_kernel_build_version).performScrollTo().performTextReplacement("#3 newer")
        compose.runOnIdle { config.value = requireNotNull(requested) }
        field(R.string.sumh_kernel_build_version).assertTextContains("#3 newer")
        saveBuild().assertIsEnabled()
    }

    @Test fun mountSourceConflictAndOwnSaveRacePreserveInput() {
        val restoration = StateRestorationTester(compose)
        val savedSource = mutableStateOf("KSU")
        var request: String? = null
        restoration.setContent { MaterialTheme { Column(Modifier.verticalScroll(rememberScrollState())) {
            MountSourceInput(savedSource.value, true) { request = it }
        } } }
        compose.runOnIdle { savedSource.value = "clean-refresh" }
        field(R.string.sumh_mountsource).assertTextContains("clean-refresh")
        field(R.string.sumh_mountsource).performTextReplacement("draft")
        compose.runOnIdle { savedSource.value = "external" }
        restoration.emulateSavedInstanceStateRestore()
        field(R.string.sumh_mountsource).assertTextContains("draft")
        compose.onNodeWithContentDescription(text(R.string.sumh_rule_save)).assertIsNotEnabled()
        action(R.string.sumh_field_draft_keep, R.string.sumh_mountsource).performScrollTo().performClick()
        compose.onNodeWithContentDescription(text(R.string.sumh_rule_save)).performClick()
        assertEquals("draft", request)
        field(R.string.sumh_mountsource).performTextReplacement("newer")
        compose.runOnIdle { savedSource.value = requireNotNull(request) }
        field(R.string.sumh_mountsource).assertTextContains("newer")
        action(R.string.sumh_field_draft_keep, R.string.sumh_mountsource).assertDoesNotExist()
        compose.onNodeWithContentDescription(text(R.string.sumh_rule_save)).assertIsEnabled()
    }

    @Test fun pathEditorStaysOpenForInvalidUnknownAndConfirmedSubmissions() {
        val restoration = StateRestorationTester(compose)
        val config = mutableStateOf(Controller.SUMHPConfig(tempdir = "/old", mirrorPath = "/mirror"))
        var request: Controller.SUMHPConfig? = null
        restoration.setContent { MaterialTheme {
            SettingsTab(config.value, Controller.SUMHStatus.AVAILABLE, null, true, false, {}, { request = it }, {},
                { _, _, _, _, _ -> }, SUMHSection.Mount)
        } }
        compose.onNodeWithContentDescription(text(R.string.sumh_tempdir)).performScrollTo().performClick()
        field(R.string.sumh_tempdir).performScrollTo().performTextReplacement("relative")
        compose.onNodeWithContentDescription(text(R.string.sumh_rule_save)).performScrollTo().performClick()
        assertNull(request)
        field(R.string.sumh_tempdir).assertExists()
        field(R.string.sumh_tempdir).performTextReplacement("/unfinished ")
        compose.runOnIdle { config.value = config.value.copy(tempdir = "/external") }
        restoration.emulateSavedInstanceStateRestore()
        field(R.string.sumh_tempdir).assertTextContains("/unfinished ")
        compose.onNodeWithContentDescription(text(R.string.sumh_rule_save)).assertIsNotEnabled()
        action(R.string.sumh_field_draft_keep, R.string.sumh_tempdir).performScrollTo().performClick()
        compose.onNodeWithContentDescription(text(R.string.sumh_rule_save)).performScrollTo().performClick()
        assertEquals("/unfinished ", request!!.tempdir)
        field(R.string.sumh_tempdir).assertExists()
        compose.runOnIdle { config.value = requireNotNull(request) }
        field(R.string.sumh_tempdir).assertTextContains("/unfinished ")
    }

    @Test
    @Config(qualifiers = "zh-rCN-w760dp-h360dp-land-xhdpi")
    fun largeTextConflictActionsRemainReachable() {
        RuntimeEnvironment.setFontScale(2f)
        try {
            val config = mutableStateOf(saved)
            compose.setContent { MaterialTheme { Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                KernelBuildSpoofEditor(config.value, true, KernelBuildRead.Unavailable, {}, { _, _, _ -> })
            } } }
            field(R.string.sumh_kernel_build_version).performScrollTo().performTextReplacement("#2 草稿 ")
            compose.runOnIdle { config.value = saved.copy(kernelBuildVersion = "#3 外部保存") }
            action(R.string.sumh_field_draft_keep, R.string.sumh_kernel_build_version).performScrollTo().assertIsDisplayed()
            val image = compose.onRoot().captureToImage().asAndroidBitmap()
            val file = File("build/reports/sumh-layout/draft-conflict-landscape-large-text.png")
            file.parentFile!!.mkdirs()
            file.outputStream().use { image.compress(Bitmap.CompressFormat.PNG, 100, it) }
            action(R.string.sumh_field_draft_keep, R.string.sumh_kernel_build_version).performClick()
            saveBuild().performScrollTo().assertIsDisplayed().assertIsEnabled()
        } finally { RuntimeEnvironment.setFontScale(1f) }
    }
}
