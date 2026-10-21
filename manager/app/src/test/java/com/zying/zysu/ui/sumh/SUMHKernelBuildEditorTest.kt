package com.zying.zysu.ui.sumh

import android.app.Application
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.StateRestorationTester
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.KERNEL_BUILD_EARLY_STAGE
import com.zying.zysu.ui.sumh.util.KernelBuildState
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter.SUMHPConfig
import org.junit.After
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import kotlin.test.assertEquals

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "w360dp-h760dp-xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SUMHKernelBuildEditorTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    @After fun resetFontScale() { RuntimeEnvironment.setFontScale(1f) }
    private val runtime = KernelBuildState(true, "6.1.75-current", "#2 SMP current",
        "6.1.75-android14-original", "#1 SMP original", "#1 SMP Fri Aug 23 03:08:10 UTC 2024", "ro.build.date")
    private val saved = SUMHPConfig(enableKernelBuildSpoof = true,
        kernelBuildRelease = "6.1.75-saved", kernelBuildVersion = "#2 SMP saved")
    private fun text(id: Int) = compose.activity.getString(id)
    private fun field(id: Int) = compose.onNode(hasSetTextAction() and hasText(text(id)))
    private fun button(id: Int) = compose.onNodeWithText(text(id))

    @Test
    fun helpersOnlyFillTheirFieldAndSaveIsExplicit() {
        val requests = mutableListOf<Triple<String, String, String>>()
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                KernelBuildSpoofEditor(saved, true, KernelBuildRead.Ready(runtime), {},
                    { release, version, stage -> requests += Triple(release, version, stage) })
            }
        } } }
        button(R.string.sumh_kernel_build_fill_original_release).performScrollTo().performClick()
        field(R.string.sumh_kernel_build_release).assertTextContains(runtime.originalRelease)
        field(R.string.sumh_kernel_build_version).assertTextContains(saved.kernelBuildVersion)
        button(R.string.sumh_kernel_build_fill_original_version).performScrollTo().performClick()
        field(R.string.sumh_kernel_build_version).assertTextContains(runtime.originalVersion)
        button(R.string.sumh_kernel_build_fill_system_date).performScrollTo().performClick()
        field(R.string.sumh_kernel_build_version).assertTextContains(runtime.suggestedVersion)
        field(R.string.sumh_kernel_build_release).assertTextContains(runtime.originalRelease)
        assertEquals(emptyList(), requests)
        button(R.string.sumh_rule_save).performScrollTo().performClick()
        assertEquals(listOf(Triple(runtime.originalRelease, runtime.suggestedVersion, KERNEL_BUILD_EARLY_STAGE)), requests)
        // A save request is not an acknowledgement; failed saves keep the draft.
        field(R.string.sumh_kernel_build_version).assertTextContains(runtime.suggestedVersion)
    }

    @Test
    fun runtimeRefreshAndStateRestorationPreserveDraftAndBlankField() {
        val restoration = StateRestorationTester(compose)
        val read = mutableStateOf<KernelBuildRead>(KernelBuildRead.Ready(runtime))
        var request: Triple<String, String, String>? = null
        restoration.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                KernelBuildSpoofEditor(saved, true, read.value, { read.value = KernelBuildRead.Loading },
                    { release, version, stage -> request = Triple(release, version, stage) })
            }
        } } }
        field(R.string.sumh_kernel_build_release).performTextReplacement("")
        field(R.string.sumh_kernel_build_version).performScrollTo().performTextReplacement("#3 SMP draft")
        button(R.string.sumh_kernel_build_refresh).performScrollTo().performClick()
        compose.runOnIdle { read.value = KernelBuildRead.Ready(runtime.copy(version = "#4 SMP changed")) }
        restoration.emulateSavedInstanceStateRestore()
        field(R.string.sumh_kernel_build_release).assertTextContains("")
        field(R.string.sumh_kernel_build_version).assertTextContains("#3 SMP draft")
        button(R.string.sumh_rule_save).performScrollTo().performClick()
        assertEquals(Triple("", "#3 SMP draft", KERNEL_BUILD_EARLY_STAGE), request)
    }

    @Test
    fun missingSuggestionAndReadFailureDisableHelpersWithoutLosingManualInput() {
        val read = mutableStateOf<KernelBuildRead>(KernelBuildRead.Ready(runtime.copy(suggestedVersion = "")))
        var retries = 0
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                KernelBuildSpoofEditor(saved, true, read.value, { retries++ }, { _, _, _ -> })
            }
        } } }
        button(R.string.sumh_kernel_build_fill_system_date).assertIsNotEnabled()
        field(R.string.sumh_kernel_build_version).performScrollTo().performTextReplacement("#3 SMP draft")
        compose.runOnIdle { read.value = KernelBuildRead.Failed }
        button(R.string.sumh_kernel_build_fill_original_release).assertIsNotEnabled()
        button(R.string.sumh_kernel_build_fill_original_version).assertIsNotEnabled()
        button(R.string.sumh_kernel_build_fill_system_date).assertIsNotEnabled()
        button(R.string.retry).performScrollTo().performClick()
        assertEquals(1, retries)
        field(R.string.sumh_kernel_build_version).assertTextContains("#3 SMP draft")
        button(R.string.sumh_rule_save).assertIsEnabled()
    }

    @Test
    @Config(qualifiers = "zh-rCN-w760dp-h360dp-land-xhdpi")
    fun largeTextKeepsActionsReachableAndInvalidUtf8DraftCannotSave() {
        RuntimeEnvironment.setFontScale(2f)
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                KernelBuildSpoofEditor(saved, true, KernelBuildRead.Ready(runtime), {}, { _, _, _ -> })
            }
        } } }
        field(R.string.sumh_kernel_build_version).performScrollTo().performTextReplacement("界".repeat(22))
        button(R.string.sumh_kernel_build_invalid).performScrollTo().assertIsDisplayed()
        button(R.string.sumh_rule_save).performScrollTo().assertIsDisplayed().assertIsNotEnabled()
        button(R.string.sumh_kernel_build_fill_system_date).performScrollTo().performClick()
        button(R.string.sumh_rule_save).performScrollTo().assertIsDisplayed().assertIsEnabled()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kernel_build_original_kmi, "android14-6.1"))
            .performScrollTo().assertIsDisplayed()
    }
}
