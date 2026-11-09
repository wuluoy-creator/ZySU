package com.zying.zysu.ui.sumh

import android.app.Application
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.semantics.SemanticsProperties
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.SUMHFeatureSupport
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter.MapsPathRule
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.assertEquals

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "w360dp-h760dp-xhdpi")
class SUMHMapsBatchImportDialogTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    @Test
    fun replacementRequiresExplicitConsentAndSavesWholeBatchOnce() {
        val saved = mutableStateOf(listOf(MapsPathRule("/old", "/before")))
        val requests = mutableListOf<List<MapsPathRule>>()
        compose.setContent { MaterialTheme {
            ManagedMapsControls(saved.value, true, SUMHFeatureSupport.Supported, true,
                { requests += it })
        } }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_import_title)).performClick()
        compose.onNode(hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_maps_import_input)))
            .performTextReplacement("/old\t/after\n/new\t/replacement")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_import_preview, 1, 1, 0))
            .assertIsDisplayed()
        val confirm = compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_import_confirm))
        confirm.assertIsNotEnabled()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_import_allow_replace))
            .performClick()
        confirm.assertIsEnabled().performClick()
        assertEquals(listOf(listOf(MapsPathRule("/old", "/after"),
            MapsPathRule("/new", "/replacement"))), requests)
        compose.onNode(isDialog()).assertExists()
        compose.runOnIdle { saved.value = requests.single() }
        compose.onNode(isDialog()).assertDoesNotExist()
    }

    @Test
    fun malformedLineBlocksImportWithoutDiscardingDraft() {
        var requests = 0
        compose.setContent { MaterialTheme {
            ManagedMapsControls(emptyList(), true, SUMHFeatureSupport.Supported, true,
                { requests++ })
        } }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_import_title)).performClick()
        val input = compose.onNode(hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_maps_import_input)))
        input.performTextReplacement("/good\t/replacement\n/bad")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_import_line_error, 2,
            compose.activity.getString(R.string.sumh_maps_import_format_error))).assertIsDisplayed()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_import_confirm)).assertIsNotEnabled()
        assertEquals(0, requests)
        assertEquals("/good\t/replacement\n/bad",
            input.fetchSemanticsNode().config[SemanticsProperties.EditableText].text)
    }
}
