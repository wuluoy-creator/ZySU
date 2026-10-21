package com.zying.zysu.ui.sumh

import android.app.Application
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.StateRestorationTester
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

/** Match the production Scaffold's themed background and content color. */
@Composable
private fun CapabilitiesTestTheme(dark: Boolean = false, content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = if (dark) darkColorScheme() else lightColorScheme()) {
        Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background, content = content)
    }
}

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "w360dp-h760dp-xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SUMHCapabilitiesScreenTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    @After fun resetFontScale() { RuntimeEnvironment.setFontScale(1f) }
    private fun text(id: Int) = compose.activity.getString(id)
    private fun go(tag: String): SemanticsNodeInteraction {
        compose.onNodeWithTag("capabilities_list").performScrollToNode(hasTestTag(tag))
        return compose.onNodeWithTag(tag)
    }
    private fun show(result: SUMHCapabilitiesResult? = SUMHCapabilitiesUiFixtures.result(), busy: Boolean = false) {
        compose.setContent { CapabilitiesTestTheme { SUMHCapabilitiesContent(result, busy, {}, {}) } }
    }

    @Test fun entryDoesNotReadAndBusyDisablesOnlyRead() {
        val busy = mutableStateOf(false); var reads = 0; val destinations = mutableListOf<SUMHCapabilityDestination>()
        compose.setContent { CapabilitiesTestTheme { SUMHCapabilitiesContent(null, busy.value, { reads++ }, destinations::add) } }
        assertEquals(0, reads)
        go("capabilities_status").assertTextEquals(text(R.string.sumh_capabilities_idle))
        go("capabilities_refresh").assertIsEnabled().performClick(); assertEquals(1, reads)
        compose.runOnIdle { busy.value = true }
        go("capabilities_refresh").assertIsNotEnabled()
        go("capabilities_diagnostics").assertIsEnabled().performClick()
        assertEquals(listOf(SUMHCapabilityDestination.Debug), destinations)
    }

    @Test fun unknownUnsupportedOffAndDegradedRemainDistinct() {
        show()
        go("capabilities_stealth_support").assertTextContains("Yes", substring = true)
        go("capabilities_stealth_saved").assertTextContains("Off", substring = true)
        go("capabilities_stealth_configured").assertTextContains("On", substring = true)
        go("capabilities_stealth_active").assertTextContains("Off", substring = true)
        go("capabilities_stealth_comparison").assertTextEquals(text(R.string.sumh_capabilities_drift))
        go("capabilities_statfs_spoof_support").assertTextContains("No", substring = true)
        go("capabilities_statfs_spoof_comparison").assertTextEquals(text(R.string.sumh_capabilities_unsupported))
        go("capabilities_modules_active").assertTextContains(text(R.string.sumh_capabilities_unknown), substring = true)
    }

    @Test fun refreshMarksExistingRowsHistoricalWithoutClaimingEquality() {
        val busy = mutableStateOf(false)
        compose.setContent { CapabilitiesTestTheme { SUMHCapabilitiesContent(SUMHCapabilitiesUiFixtures.result(), busy.value, {}, {}) } }
        go("capabilities_kernel_debug_comparison").assertTextEquals(text(R.string.sumh_capabilities_matches))
        compose.runOnIdle { busy.value = true }
        go("capabilities_kernel_debug_historical").assertIsDisplayed()
        go("capabilities_kernel_debug_comparison").assertTextEquals(text(R.string.sumh_capabilities_unknown))
        go("capabilities_refresh").assertIsNotEnabled()
    }

    @Test fun failedReadRetainsValueAndShowsFailureWithOldProvenance() {
        val retained = SUMHCapabilitiesResult(error = "transport_timeout").retaining(SUMHCapabilitiesUiFixtures.result())
        show(retained)
        go("capabilities_status").assertTextEquals(text(R.string.sumh_capabilities_failed))
        go("capabilities_failure_0").assertTextEquals("transport_timeout")
        go("capabilities_stealth_historical").assertIsDisplayed()
        go("capabilities_stealth_configured").assertTextContains("On", substring = true)
        go("capabilities_stealth_details").performClick()
        compose.onNodeWithTag("capabilities_list").performScrollToNode(hasText("Boot: boot-A"))
        compose.onAllNodesWithText("Boot: boot-A").onFirst().assertIsDisplayed()
    }

    @Test fun transportFailureWithValidPayloadDoesNotPresentFreshEquality() {
        show(SUMHCapabilitiesUiFixtures.result().copy(error = "connection_closed_after_payload"))
        go("capabilities_kernel_debug_historical").assertIsDisplayed()
        go("capabilities_kernel_debug_comparison").assertTextEquals(text(R.string.sumh_capabilities_unknown))
    }

    @Test fun originalKmiIsSeparateFromDisplayedBuildAndOwnerIsUnknown() {
        show()
        go("capabilities_build_current").assertTextContains("6.1.0-displayed", substring = true)
        go("capabilities_build_original").assertTextContains("6.12.52-android16", substring = true)
        go("capabilities_build_kmi").assertTextContains("android16-6.12", substring = true)
        go("capabilities_owner").assertTextEquals(text(R.string.sumh_capabilities_owner_unknown))
        go("capabilities_backend_overlay").assertTextContains(text(R.string.sumh_capabilities_backend_unknown), substring = true)
    }

    @Test fun scopeDetailsArePolicyAndNeverObserverMeasurement() {
        show()
        go("capabilities_stealth_details").performClick()
        go("capabilities_stealth_scope").assertTextContains("/dev", substring = true)
        compose.onNodeWithTag("capabilities_list").performScrollToNode(hasText(text(R.string.sumh_capabilities_observers_unmeasured)))
        compose.onNodeWithText(text(R.string.sumh_capabilities_observers_unmeasured)).assertIsDisplayed()
        go("capabilities_user_hide_details").performClick()
        go("capabilities_user_hide_scope").assertTextContains("exempts root", substring = true)
    }

    @Test fun featureActionsMapToExistingWorkflowsWithoutRead() {
        var reads = 0; val destinations = mutableListOf<SUMHCapabilityDestination>()
        compose.setContent { CapabilitiesTestTheme {
            SUMHCapabilitiesContent(SUMHCapabilitiesUiFixtures.result(), false, { reads++ }, destinations::add)
        } }
        go("capabilities_stealth_configure").performClick()
        go("capabilities_user_hide_configure").performClick()
        go("capabilities_modules_configure").performClick()
        go("capabilities_overlay_xattr_hide_configure").performClick()
        assertEquals(listOf(SUMHCapabilityDestination.Isolation, SUMHCapabilityDestination.UserHide,
            SUMHCapabilityDestination.Modules, SUMHCapabilityDestination.Mount), destinations)
        assertEquals(0, reads)
        assertTrue(sumhCapabilitiesDestination(SUMHCapabilityDestination.Isolation).route.endsWith("initialPage=1"))
        assertTrue(sumhCapabilitiesDestination(SUMHCapabilityDestination.UserHide).route.endsWith("initialPage=2"))
    }

    @Test fun savedExpansionRestoresWithoutReading() {
        val restore = StateRestorationTester(compose); var reads = 0
        restore.setContent { CapabilitiesTestTheme {
            SUMHCapabilitiesContent(SUMHCapabilitiesUiFixtures.result(), false, { reads++ }, {})
        } }
        go("capabilities_stealth_details").performClick()
        restore.emulateSavedInstanceStateRestore()
        go("capabilities_stealth_scope").assertIsDisplayed(); assertEquals(0, reads)
    }

    @Test @Config(qualifiers = "zh-rCN-w320dp-h760dp-xhdpi")
    fun narrowChineseLargeTextKeepsReadAndLastFeatureReachable() {
        RuntimeEnvironment.setFontScale(2f); show()
        go("capabilities_refresh").assertIsDisplayed().assertIsEnabled()
        capture("capabilities-narrow-large-text")
        go("capabilities_overlay_xattr_hide_configure").assertIsDisplayed().assertIsEnabled()
    }

    @Test @Config(qualifiers = "zh-rCN-w760dp-h360dp-land-xhdpi")
    fun landscapeDarkLargeTextKeepsErrorAndActionsReachable() {
        RuntimeEnvironment.setFontScale(2f)
        compose.setContent { CapabilitiesTestTheme(dark = true) {
            SUMHCapabilitiesContent(SUMHCapabilitiesResult(error = "读取失败，保留原始配置"), false, {}, {})
        } }
        go("capabilities_failure_0").assertIsDisplayed()
        capture("capabilities-landscape-large-text")
        go("capabilities_refresh").assertIsDisplayed().assertIsEnabled()
    }

    @Test fun largeUnicodeErrorIsChunkedWithoutDroppingCharacters() {
        val error = "x".repeat(2047) + "😀" + "中文".repeat(3000)
        val chunks = capabilityTextChunks(error)
        assertEquals(error, chunks.joinToString("") { it.second })
        assertTrue(chunks.all { it.second.length <= 2048 && !it.second.last().isHighSurrogate() && !it.second.first().isLowSurrogate() })
        show(SUMHCapabilitiesResult(error = error))
        go("capabilities_failure_${chunks.lastIndex}").assertTextEquals(chunks.last().second)
    }

    private fun capture(name: String) {
        val output = File("build/reports/sumh-layout/$name.png"); output.parentFile?.mkdirs()
        output.outputStream().use { compose.onRoot().captureToImage().asAndroidBitmap().compress(Bitmap.CompressFormat.PNG, 100, it) }
    }
}
