package com.zying.zysu.ui.sumh

import android.app.Application
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.*
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.*
import org.junit.After
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.File
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "w360dp-h760dp-xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SUMHDiagnosticPackagesScreenTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    @After fun resetFontScale() { RuntimeEnvironment.setFontScale(1f) }
    private val draft = mutableStateOf(SUMHDiagnosticPackageDraft())
    private val preview = mutableStateOf<SUMHDiagnosticPackagePreview?>(null)
    private val busy = mutableStateOf(false)
    private var creates = 0; private var reads = 0; private var exports = 0; private var removals = 0; private var recovers = 0
    private fun go(tag: String): SemanticsNodeInteraction {
        compose.onNodeWithTag("packages_list").performScrollToNode(hasTestTag(tag))
        return compose.onNodeWithTag(tag)
    }
    private fun show(support: SUMHDiagnosticPackageCapabilities? = SUMHDiagnosticPackageUiFixtures.capabilities(),
        pending: SUMHDiagnosticPackagePending? = null, error: String? = null, dark: Boolean = false) {
        compose.setContent { MaterialTheme(colorScheme = if (dark) darkColorScheme() else lightColorScheme()) {
            Surface(Modifier.fillMaxSize()) {
                SUMHDiagnosticPackagesContent(true, busy.value, draft.value, support, preview.value?.let { listOf(it.item) }.orEmpty(),
                    preview.value, null, error, pending, null, SUMHDiagnosticExportStatus.Idle,
                    onDraft = { draft.value = it }, onRefresh = { reads++ }, onCreate = { creates++ },
                    onPreview = {}, onRemove = { removals++ }, onRecover = { recovers++ }, onAcknowledge = {}, onExport = { exports++ })
            }
        } }
    }

    @Test fun openingHasNoSamplingAndManualLoadIsExplicit() {
        show(support = null)
        assertEquals(0, reads); assertEquals(0, creates)
        go("packages_create").assertIsNotEnabled()
        go("packages_refresh").assertIsEnabled().performClick(); assertEquals(1, reads)
    }

    @Test fun identifiersRequireSeparateConfirmationAndCancelKeepsDraft() {
        show()
        go("packages_identifiers").performClick(); assertTrue(draft.value.includeIdentifiers)
        go("packages_create").performClick(); assertEquals(0, creates)
        compose.onNodeWithTag("packages_confirm_create").assertIsDisplayed().performClick()
        assertEquals(1, creates); assertTrue(draft.value.includeIdentifiers)
    }

    @Test fun changingScenarioRequiresItsOwnTargetAndKeepsOperationDraft() {
        show()
        go("packages_operation").performTextInput("a".repeat(32))
        go("packages_scenario").performClick()
        compose.onNodeWithTag("packages_scenario_rule-not-effective").performClick()
        go("packages_create").assertIsNotEnabled()
        go("packages_target").performTextInput("b".repeat(32))
        go("packages_create").assertIsEnabled()
        assertEquals("a".repeat(32), draft.value.operationId)
    }

    @Test fun exportRequiresReviewAndDeleteRequiresExactConfirmation() {
        preview.value = SUMHDiagnosticPackageUiFixtures.preview(); show()
        go("packages_export").assertIsNotEnabled(); go("packages_remove").assertIsNotEnabled()
        go("packages_full_content").performClick()
        go("packages_content_0").assertTextEquals(preview.value!!.exportText)
        go("packages_reviewed").performClick()
        go("packages_export").performClick(); assertEquals(1, exports)
        go("packages_remove").performClick(); assertEquals(0, removals)
        compose.onNodeWithTag("packages_confirm_remove").performClick(); assertEquals(1, removals)
    }

    @Test fun changedPreviewInvalidatesReviewAndBusyDisablesMutation() {
        preview.value = SUMHDiagnosticPackageUiFixtures.preview(); show()
        go("packages_reviewed").performClick(); go("packages_export").assertIsEnabled()
        compose.runOnIdle { preview.value = SUMHDiagnosticPackageUiFixtures.preview(id = "b".repeat(32)) }
        go("packages_export").assertIsNotEnabled()
        compose.runOnIdle { busy.value = true }
        go("packages_create").assertIsNotEnabled(); go("packages_refresh").assertIsNotEnabled()
    }

    @Test fun pendingChangeShowsQueryAndDoesNotReplay() {
        show(pending = SUMHDiagnosticPackagePending("create", "a".repeat(32), null))
        assertEquals(0, creates); assertEquals(0, recovers)
        go("packages_create").assertIsNotEnabled(); go("packages_acknowledge").assertIsNotEnabled()
        go("packages_recover").performClick(); assertEquals(1, recovers); assertEquals(0, creates)
    }

    @Test @Config(qualifiers = "zh-rCN-w320dp-h760dp-xhdpi")
    fun narrowChineseLargeTextKeepsInputsAndCreateReachable() {
        RuntimeEnvironment.setFontScale(2f); show()
        go("packages_refresh").assertIsDisplayed(); capture("packages-narrow-large-text")
        go("packages_identifiers").assertIsDisplayed(); go("packages_create").assertIsDisplayed().assertIsEnabled()
    }

    @Test @Config(qualifiers = "zh-rCN-w760dp-h360dp-land-xhdpi")
    fun landscapeDarkLargeTextKeepsPreviewAndActionsReachable() {
        RuntimeEnvironment.setFontScale(2f); preview.value = SUMHDiagnosticPackageUiFixtures.preview(policy = "identifiers_included")
        show(dark = true)
        go("packages_full_content").assertIsDisplayed(); capture("packages-landscape-dark")
        go("packages_reviewed").assertIsDisplayed().performClick(); go("packages_export").assertIsDisplayed().assertIsEnabled()
    }

    private fun capture(name: String) {
        val output = File("build/reports/sumh-layout/$name.png"); output.parentFile?.mkdirs()
        output.outputStream().use { compose.onRoot().captureToImage().asAndroidBitmap().compress(Bitmap.CompressFormat.PNG, 100, it) }
    }
}
