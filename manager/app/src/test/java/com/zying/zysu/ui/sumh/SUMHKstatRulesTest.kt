package com.zying.zysu.ui.sumh

import android.app.Application
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.hasSetTextAction
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTextReplacement
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.assertEquals

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "w360dp-h760dp-xhdpi")
class SUMHKstatRulesTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    @Test
    fun runtimeStatusKeepsSavedAndActiveCountsSeparate() {
        val config = SUMHUiAdapter.SUMHPConfig(kstatRules = listOf(
            SUMHUiAdapter.KstatPathRule("/system/target", "/system/source", "identity")))
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            KstatRulesSettings(config, SUMHUiAdapter.SUMHStatus.AVAILABLE,
                SUMHUiAdapter.FeaturesResult(1 shl 17, listOf("managed_kstat")), true,
                {}, null, SUMHUiAdapter.ManagedKstatStatus(0, "7"))
        } } }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kstat_saved_count, 1)).assertExists()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kstat_active_count, 0)).assertExists()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kstat_generation, "7")).assertExists()
    }

    @Test
    fun missingRuntimeStatusIsUnknownEvenWithSavedRules() {
        val config = SUMHUiAdapter.SUMHPConfig(kstatRules = listOf(
            SUMHUiAdapter.KstatPathRule("/system/target", "/system/source", "identity")))
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            KstatRulesSettings(config, SUMHUiAdapter.SUMHStatus.AVAILABLE,
                SUMHUiAdapter.FeaturesResult(1 shl 17, listOf("managed_kstat")), true,
                {}, null)
        } } }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kstat_saved_count, 1)).assertExists()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kstat_active_unknown)).assertExists()
    }

    @Test
    fun unsupportedKernelStillAllowsReadingCompleteSavedPaths() {
        val target = "/system/" + "target".repeat(32)
        val source = "/system/" + "source".repeat(32)
        val config = SUMHUiAdapter.SUMHPConfig(kstatRules = listOf(
            SUMHUiAdapter.KstatPathRule(target, source, "identity")))
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            KstatRulesSettings(config, SUMHUiAdapter.SUMHStatus.AVAILABLE,
                SUMHUiAdapter.FeaturesResult(0, emptyList()), true, {}, null)
        } } }
        compose.onNodeWithContentDescription(compose.activity.getString(R.string.sumh_kstat_edit)).assertIsNotEnabled()
        compose.onNodeWithContentDescription(compose.activity.getString(R.string.sumh_kstat_details)).performClick()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kstat_target)).assertExists()
        compose.onAllNodesWithText(target).assertCountEquals(2)
        compose.onAllNodesWithText(source).assertCountEquals(1)
    }

    @Test
    fun fullSnapshotRuleSavesBothPathsAndMode() {
        var request: SUMHUiAdapter.SUMHPConfig? = null
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            KstatRulesSettings(SUMHUiAdapter.SUMHPConfig(), SUMHUiAdapter.SUMHStatus.AVAILABLE,
                SUMHUiAdapter.FeaturesResult(1 shl 17, listOf("managed_kstat")), true,
                { request = it }, null)
        } } }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kstat_add)).performClick()
        compose.onNode(hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_kstat_target)))
            .performTextReplacement("/system/target")
        compose.onNode(hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_kstat_source_label)))
            .performTextReplacement("/system/source")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kstat_mode_full)).performScrollTo().performClick()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save)).performClick()
        assertEquals(listOf(SUMHUiAdapter.KstatPathRule("/system/target", "/system/source", "full")),
            request?.kstatRules)
    }

    @Test
    fun customRuleSavesSelectedZeroValueWithoutSourcePath() {
        var request: SUMHUiAdapter.SUMHPConfig? = null
        compose.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) {
            KstatRulesSettings(SUMHUiAdapter.SUMHPConfig(), SUMHUiAdapter.SUMHStatus.AVAILABLE,
                SUMHUiAdapter.FeaturesResult(1 shl 17, listOf("managed_kstat")), true,
                { request = it }, null)
        } } }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kstat_add)).performClick()
        compose.onNode(hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_kstat_target)))
            .performTextReplacement("/system/target")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kstat_mode_custom)).performScrollTo().performClick()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kstat_field_ino)).performScrollTo().performClick()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save)).performClick()
        assertEquals(listOf(SUMHUiAdapter.KstatPathRule("/system/target", "", "custom", mapOf("ino" to "0"))),
            request?.kstatRules)
    }
}
