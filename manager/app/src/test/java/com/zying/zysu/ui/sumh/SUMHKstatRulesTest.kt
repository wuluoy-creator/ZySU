package com.zying.zysu.ui.sumh

import android.app.Application
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.darkColorScheme
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.SemanticsNodeInteraction
import androidx.compose.ui.test.assertAll
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertTextContains
import androidx.compose.ui.test.hasSetTextAction
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.hasAnyAncestor
import androidx.compose.ui.test.isDialog
import androidx.compose.ui.test.isToggleable
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTextReplacement
import androidx.compose.ui.text.AnnotatedString
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import kotlin.test.assertEquals
import kotlin.test.assertNull

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
        compose.onNode(hasText(compose.activity.getString(R.string.sumh_kstat_target)) and hasAnyAncestor(isDialog())).assertExists()
        compose.onAllNodesWithText(target).assertCountEquals(2)
        compose.onNode(hasText(source) and hasAnyAncestor(isDialog())).assertExists()
    }

    @Test
    fun newRuleShowsEveryCustomFieldEmptyWithoutModeOrSourceControls() {
        var request: SUMHUiAdapter.SUMHPConfig? = null
        showRules { request = it }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kstat_add)).performClick()
        assertCustomOnlyEditor()
        compose.onAllNodes(hasSetTextAction() and hasAnyAncestor(isDialog()))
            .assertCountEquals(13)
            .assertAll(SemanticsMatcher.expectValue(SemanticsProperties.EditableText, AnnotatedString("")))
        completeFields.keys.forEach { fieldInput(it).assertExists() }
        setTarget("/system/target")
        save()
        assertNull(request)
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kstat_invalid)).assertExists()
    }

    @Test
    fun everyCustomFieldIsRequiredAndCompleteValuesSaveWithoutSourcePath() {
        var request: SUMHUiAdapter.SUMHPConfig? = null
        showRules { request = it }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kstat_add)).performClick()
        setTarget("/system/target")
        fillFields(completeFields)

        completeFields.forEach { (field, value) ->
            fieldInput(field).performScrollTo().performTextReplacement("")
            save()
            assertNull(request, "$field must be explicitly filled")
            fieldInput(field).performScrollTo().performTextReplacement(value)
        }
        fieldInput("mtime.nsec").performScrollTo().performTextReplacement("1000000000")
        save()
        assertNull(request, "A nonempty but invalid value must not be saved")
        fieldInput("mtime.nsec").performScrollTo().performTextReplacement(completeFields.getValue("mtime.nsec"))
        save()
        assertEquals(listOf(SUMHUiAdapter.KstatPathRule("/system/target", "", "custom", completeFields)),
            request?.kstatRules)
    }

    @Test
    fun editingPartialCustomRulePreservesValuesAndRequiresMissingFields() {
        val partialFields = mapOf("ino" to "9001", "mtime.sec" to "-5", "mtime.nsec" to "0")
        val original = SUMHUiAdapter.KstatPathRule("/system/target", "", "custom", partialFields)
        var request: SUMHUiAdapter.SUMHPConfig? = null
        showRules(SUMHUiAdapter.SUMHPConfig(kstatRules = listOf(original))) { request = it }
        compose.onNodeWithContentDescription(compose.activity.getString(R.string.sumh_kstat_edit)).performClick()
        assertCustomOnlyEditor()
        partialFields.forEach { (field, value) -> fieldInput(field).assertTextContains(value) }
        setTarget("/system/renamed")
        save()
        assertNull(request)
        fillFields(completeFields.filterKeys { it !in partialFields })
        save()
        assertEquals(listOf(original.copy(targetPath = "/system/renamed", customFields = completeFields + partialFields)),
            request?.kstatRules)
    }

    @Test
    fun existingSourceRuleAllowsAddingCustomRuleButStillRejectsDuplicateTarget() {
        val original = SUMHUiAdapter.KstatPathRule("/system/existing", "/system/source", "full")
        var request: SUMHUiAdapter.SUMHPConfig? = null
        showRules(SUMHUiAdapter.SUMHPConfig(kstatRules = listOf(original))) { request = it }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kstat_add)).performClick()
        assertCustomOnlyEditor()
        setTarget(original.targetPath)
        fillFields(completeFields)
        save()
        assertNull(request)
        setTarget("/system/new")
        save()
        assertEquals(listOf(original, SUMHUiAdapter.KstatPathRule("/system/new", "", "custom", completeFields)),
            request?.kstatRules)
    }

    @Test
    @Config(qualifiers = "zh-rCN-w360dp-h640dp-xhdpi")
    fun narrowDarkLargeTextKeepsFieldsAndSaveReachable() {
        RuntimeEnvironment.setFontScale(2f)
        showRules(dark = true) {}
        assertLastFieldAndSaveReachable()
    }

    @Test
    @Config(qualifiers = "zh-rCN-w760dp-h360dp-land-xhdpi")
    fun landscapeKeepsFieldsAndSaveReachable() {
        showRules {}
        assertLastFieldAndSaveReachable()
    }

    private fun showRules(
        config: SUMHUiAdapter.SUMHPConfig = SUMHUiAdapter.SUMHPConfig(),
        dark: Boolean = false,
        onChanged: (SUMHUiAdapter.SUMHPConfig) -> Unit,
    ) {
        compose.setContent { MaterialTheme(colorScheme = if (dark) darkColorScheme() else MaterialTheme.colorScheme) {
            Surface(Modifier.fillMaxSize()) {
                KstatRulesSettings(config, SUMHUiAdapter.SUMHStatus.AVAILABLE,
                    SUMHUiAdapter.FeaturesResult(1 shl 17, listOf("managed_kstat")), true,
                    onChanged, null)
            }
        } }
    }

    private fun assertCustomOnlyEditor() {
        listOf(R.string.sumh_kstat_mode_identity, R.string.sumh_kstat_mode_full,
            R.string.sumh_kstat_source_label, R.string.sumh_kstat_mode).forEach { label ->
            compose.onNode(hasText(compose.activity.getString(label)) and hasAnyAncestor(isDialog())).assertDoesNotExist()
        }
        compose.onAllNodes(isToggleable() and hasAnyAncestor(isDialog())).assertCountEquals(0)
    }

    private fun assertLastFieldAndSaveReachable() {
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_kstat_add)).performClick()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save)).assertIsDisplayed().assertIsEnabled()
        fieldInput("blocks").performScrollTo().assertIsDisplayed().performTextReplacement("0")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save)).assertIsDisplayed().assertIsEnabled()
    }

    private fun setTarget(value: String) {
        compose.onNode(hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_kstat_target)))
            .performScrollTo().performTextReplacement(value)
    }

    private fun save() {
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_save)).performClick()
    }

    private fun fillFields(values: Map<String, String>) {
        values.forEach { (field, value) -> fieldInput(field).performScrollTo().performTextReplacement(value) }
    }

    private fun fieldInput(field: String): SemanticsNodeInteraction {
        val label = when (field) {
            "dev" -> R.string.sumh_kstat_field_dev
            "ino" -> R.string.sumh_kstat_field_ino
            "nlink" -> R.string.sumh_kstat_field_nlink
            "size" -> R.string.sumh_kstat_field_size
            "blksize" -> R.string.sumh_kstat_field_blksize
            "blocks" -> R.string.sumh_kstat_field_blocks
            else -> if (field.endsWith(".sec")) R.string.sumh_kstat_seconds else R.string.sumh_kstat_nanoseconds
        }
        val matcher = hasSetTextAction() and hasText(compose.activity.getString(label)) and hasAnyAncestor(isDialog())
        return if ('.' in field) compose.onAllNodes(matcher)[listOf("atime", "mtime", "ctime").indexOf(field.substringBefore('.'))]
            else compose.onNode(matcher)
    }

    private val completeFields = mapOf(
        "dev" to "1048576", "ino" to "123", "nlink" to "1", "size" to "0",
        "atime.sec" to "-1", "atime.nsec" to "999999999",
        "mtime.sec" to "0", "mtime.nsec" to "0",
        "ctime.sec" to "1", "ctime.nsec" to "2",
        "blksize" to "4096", "blocks" to "0",
    )
}
