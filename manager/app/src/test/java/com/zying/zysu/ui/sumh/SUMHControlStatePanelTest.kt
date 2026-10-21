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
import com.zying.zysu.ui.sumh.util.*
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as Controller
import org.json.JSONObject
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
class SUMHControlStatePanelTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    private val fixtures = SUMHControlStateFixtures
    private fun text(id: Int) = compose.activity.getString(id)
    private fun expand() = compose.onNodeWithTag("state_expand").performScrollTo().performClick()

    @Test fun refreshAndExternalChangeReplaceDriftWithoutReplayingAfterRecreation() {
        val control = mutableStateOf(fixtures.control(fixtures.state("drift")))
        var applies = 0
        var refreshes = 0
        val restore = StateRestorationTester(compose)
        restore.setContent { MaterialTheme { Column(Modifier.verticalScroll(rememberScrollState())) {
            SUMHControlStatePanel(control.value, Controller.SUMHPConfig(), SUMHSection.Debug,
                { refreshes++; control.value = fixtures.control() }, { applies++ }, legacyPending = true)
        } } }
        compose.onNodeWithTag("state_apply").assertExists()
        expand()
        restore.emulateSavedInstanceStateRestore()
        assertEquals(0, applies)
        compose.onNodeWithTag("state_refresh").performScrollTo().performClick()
        compose.onNodeWithTag("state_apply").assertDoesNotExist()
        compose.onNodeWithTag("state_feature_kernel_debug").assertExists()
        assertEquals(1, refreshes)
        assertEquals(0, applies)
    }

    @Test fun configuredInactiveAndUnsupportedAreDistinctFromUnknown() {
        val raw = fixtures.state()
        val runtime = raw.getJSONObject("runtime").getJSONObject("runtime").getJSONArray("features")
        runtime.getJSONObject(1).put("configured", true).put("degraded", true)
        runtime.getJSONObject(2).put("supported", false)
        raw.getJSONObject("features").getJSONObject("stealth").put("reason", "configured_but_inactive")
        raw.getJSONObject("features").getJSONObject("mount_hide").put("status", "unsupported")
            .put("reason", "required_runtime_machinery_unavailable")
        compose.setContent { MaterialTheme { Column(Modifier.verticalScroll(rememberScrollState())) {
            SUMHControlStatePanel(fixtures.control(raw), Controller.SUMHPConfig(enableStealth = true), SUMHSection.Isolation, {})
        } } }
        expand()
        compose.onNodeWithTag("state_feature_stealth").assert(hasAnyDescendant(hasText(text(R.string.sumh_state_configured_inactive))))
        compose.onNodeWithTag("state_feature_mount_hide").assert(hasAnyDescendant(hasText(text(R.string.sumh_state_unsupported))))
        compose.onNodeWithTag("state_feature_maps_rules").assert(hasAnyDescendant(hasText(text(R.string.sumh_state_unknown))))
        compose.onNodeWithTag("state_apply").assertDoesNotExist()
    }

    @Test fun unknownOperationWithPartialTransportFactsOffersQueryInsteadOfApply() {
        val raw = fixtures.state("drift").put("history", JSONObject().put("status", "valid").put("error", "")
            .put("last_operation", fixtures.operation(started = true)))
        var applied = 0
        compose.setContent { MaterialTheme { Column(Modifier.verticalScroll(rememberScrollState())) {
            SUMHControlStatePanel(fixtures.control(raw, "connection closed after response"), Controller.SUMHPConfig(), SUMHSection.Debug,
                {}, { applied++ }, legacyPending = true)
        } } }
        compose.onNodeWithText(text(R.string.sumh_state_operation_unknown)).assertExists()
        compose.onNodeWithText("connection closed after response").assertExists()
        compose.onNodeWithTag("state_apply").assertDoesNotExist()
        expand()
        compose.onNodeWithTag("state_feature_kernel_debug").assert(hasAnyDescendant(hasText(text(R.string.sumh_state_drift))))
        assertEquals(0, applied)
    }

    @Test fun retainedSamplesAreExplicitlyStaleAndCannotOfferApply() {
        val old = fixtures.control(fixtures.state("drift"))
        val state = mutableStateOf(old)
        compose.setContent { MaterialTheme { Column(Modifier.verticalScroll(rememberScrollState())) {
            SUMHControlStatePanel(state.value, Controller.SUMHPConfig(), SUMHSection.Debug, {}, {}, legacyPending = true)
        } } }
        compose.runOnIdle { state.value = SUMHControlState(old.capabilities, SUMHStateQuery(error = "offline"), SUMHStateQuery(error = "offline")).retaining(old) }
        compose.onNodeWithText(text(R.string.sumh_state_stale)).assertExists()
        compose.onNodeWithTag("state_apply").assertDoesNotExist()
        expand()
        compose.onNodeWithTag("state_feature_kernel_debug").assert(hasAnyDescendant(hasText(text(R.string.sumh_state_stale))))
    }

    @Test fun registeredHideAndHistoricalRecordsNeverClaimVisibilityOrCurrentApplication() {
        val raw = fixtures.state()
        val entry = raw.getJSONObject("features").getJSONObject("user_hide")
        entry.put("status", "matches").put("reason", "registered_definitions_verified_globally_suspended")
            .put("detail", JSONObject().put("verification_scope", "registered_definitions").put("binding_consistency", "per_rule_sampled")
                .put("pending_count", 0).put("error_count", 0).put("suspended_count", 2))
            .put("last_verified", JSONObject().put("revision", fixtures.revision).put("boot_id", "old-boot")
                .put("instance_id", "1").put("generation", "1"))
        compose.setContent { MaterialTheme { Column(Modifier.verticalScroll(rememberScrollState())) {
            SUMHControlStatePanel(fixtures.control(raw), Controller.SUMHPConfig(), SUMHSection.Status, {})
        } } }
        expand()
        val hidden = compose.onNodeWithTag("state_feature_user_hide")
        hidden.assert(hasAnyDescendant(hasText(text(R.string.sumh_state_registered))))
        hidden.assert(hasAnyDescendant(hasText(text(R.string.sumh_state_hide_suspended))))
        hidden.assert(hasAnyDescendant(hasText(text(R.string.sumh_state_history_only))))
        compose.onNodeWithTag("state_feature_modules").assert(hasAnyDescendant(hasText(text(R.string.sumh_state_inventory_unknown))))
    }

    @Test fun settingsUseStateAdviceInsteadOfRememberedPendingBoolean() {
        compose.setContent { MaterialTheme {
            SettingsTab(Controller.SUMHPConfig(), Controller.SUMHStatus.AVAILABLE, null, true, true, {}, {}, {},
                { _, _, _, _, _ -> }, SUMHSection.Debug, controlState = fixtures.control(), legacyApplyPending = true)
        } }
        compose.onNodeWithTag("state_apply").assertDoesNotExist()
        compose.onNodeWithText(text(R.string.sumh_state_legacy_pending)).assertDoesNotExist()
        compose.onNodeWithText(text(R.string.sumh_state_title)).assertExists()
    }

    @Test fun legacyPendingIsLabeledAndFutureSchemaNeverUsesItAsEvidence() {
        val state = mutableStateOf(SUMHControlState(SUMHStateCapabilities(SUMHSchemaSupport.NotAdvertised)))
        compose.setContent { MaterialTheme { Column(Modifier.verticalScroll(rememberScrollState())) {
            SUMHControlStatePanel(state.value, Controller.SUMHPConfig(), SUMHSection.Debug, {}, {}, legacyPending = true)
        } } }
        compose.onNodeWithText(text(R.string.sumh_state_legacy_pending)).assertExists()
        compose.onNodeWithTag("state_apply").assertExists()
        compose.runOnIdle { state.value = SUMHControlState(SUMHStateCapabilities(SUMHSchemaSupport.Future)) }
        compose.onNodeWithTag("state_apply").assertDoesNotExist()
        expand()
        compose.onNodeWithText(text(R.string.sumh_state_schema_future)).assertExists()
        compose.onNodeWithTag("state_feature_kernel_debug").assert(hasAnyDescendant(hasText(text(R.string.sumh_state_unknown))))
    }

    @Test
    @Config(qualifiers = "zh-rCN-w360dp-h760dp-xhdpi")
    fun realStatusTabShowsUnsupportedQueryAsUnknownAtLargeTextNarrowWidth() {
        RuntimeEnvironment.setFontScale(2f)
        try {
            val runtime = SUMHControlStateParser.runtime(SUMHQueryReply(fixtures.runtime()
                .put("status", "unsupported").put("reason", "runtime_state_not_advertised")
                .put("runtime", JSONObject.NULL).toString()))
            val control = SUMHControlState(SUMHStateCapabilities(SUMHSchemaSupport.NotAdvertised, SUMHSchemaSupport.Supported),
                runtime = runtime)
            compose.setContent { MaterialTheme {
                StatusTab(Controller.SUMHStatus.AVAILABLE, "1", Controller.SystemInfo("test-kernel", "", emptyList(), emptyList(), false, null),
                    {}, controlState = control, config = Controller.SUMHPConfig(enableStealth = true))
            } }
            compose.onNodeWithText(text(R.string.sumh_state_runtime_unsupported)).performScrollTo().assertIsDisplayed()
            compose.onNodeWithTag("state_refresh").performScrollTo().assertIsDisplayed()
            expand()
            compose.onNodeWithTag("state_feature_kernel_debug").performScrollTo()
                .assert(hasAnyDescendant(hasText(text(R.string.sumh_state_unknown))))
            compose.onNodeWithText(text(R.string.sumh_state_unsupported)).assertDoesNotExist()
            val output = File("build/reports/sumh-layout/control-state-narrow-large-text.png")
            output.parentFile!!.mkdirs()
            output.outputStream().use { compose.onRoot().captureToImage().asAndroidBitmap().compress(Bitmap.CompressFormat.PNG, 100, it) }
        } finally { RuntimeEnvironment.setFontScale(1f) }
    }

    @Test
    @Config(qualifiers = "zh-rCN-w760dp-h360dp-land-xhdpi")
    fun largeTextLandscapeKeepsRefreshAndDetailsReachable() {
        RuntimeEnvironment.setFontScale(2f)
        try {
            compose.setContent { MaterialTheme { Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                SUMHControlStatePanel(fixtures.control(fixtures.state("drift")), Controller.SUMHPConfig(), SUMHSection.Debug, {}, {})
            } } }
            compose.onNodeWithTag("state_refresh").performScrollTo().assertIsDisplayed()
            compose.onNodeWithTag("state_apply").performScrollTo().assertIsDisplayed()
            expand()
            compose.onNodeWithTag("state_feature_kernel_debug").performScrollTo().assertIsDisplayed()
            val image = compose.onRoot().captureToImage().asAndroidBitmap()
            val output = File("build/reports/sumh-layout/control-state-landscape-large-text.png")
            output.parentFile!!.mkdirs()
            output.outputStream().use { image.compress(Bitmap.CompressFormat.PNG, 100, it) }
        } finally { RuntimeEnvironment.setFontScale(1f) }
    }
}
