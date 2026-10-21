package com.zying.zysu.ui.sumh

import android.app.Application
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.StateRestorationTester
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModelProvider
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.*
import kotlinx.coroutines.CompletableDeferred
import org.junit.After
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.shadows.ShadowLooper
import java.io.File
import kotlin.test.*

@Composable
private fun TemporaryMapsTestTheme(dark: Boolean = false, content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = if (dark) darkColorScheme() else lightColorScheme()) {
        Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background, content = content)
    }
}

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "w360dp-h760dp-xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SUMHTemporaryMapsControlsTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    @After fun resetFontScale() { RuntimeEnvironment.setFontScale(1f) }
    private fun text(id: Int) = compose.activity.getString(id)
    private fun until(condition: () -> Boolean) {
        compose.waitUntil(10000) { ShadowLooper.idleMainLooper(); condition() }; compose.waitForIdle()
    }
    private fun idle(owner: SUMHTemporaryMapsOperations) = until { !owner.busy }
    private fun go(tag: String): SemanticsNodeInteraction {
        compose.onNodeWithTag("temporary-maps-list").performScrollToNode(hasTestTag(tag))
        return compose.onNodeWithTag(tag)
    }
    private fun show(owner: SUMHTemporaryMapsOperations, service: SUMHTemporaryMapsUiFixtures.Service, dark: Boolean = false) {
        compose.setContent { TemporaryMapsTestTheme(dark) { TemporaryMapsDialog(owner, true, service, service.tracking) {} } }
    }
    private fun load(owner: SUMHTemporaryMapsOperations, service: SUMHTemporaryMapsUiFixtures.Service) {
        compose.runOnIdle { owner.refresh(service, service.tracking) }; idle(owner)
    }

    @Test fun independentEntryStaysAvailableWhenMapsSwitchAndFeatureAreOff() {
        compose.setContent { TemporaryMapsTestTheme {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                ManagedMapsControls(emptyList(), true, SUMHFeatureSupport.Unsupported, false, {})
            }
        } }
        compose.onNodeWithText(text(R.string.sumh_temporary_maps_open)).performScrollTo().assertIsEnabled().performClick()
        compose.onNodeWithTag("temporary-maps-status").assertTextEquals(text(R.string.sumh_temporary_maps_start))
        compose.onNodeWithTag("temporary-maps-close").assertIsEnabled().performClick()
        compose.onNode(isDialog()).assertDoesNotExist()
    }

    @Test fun openingRestoringAndClosingNeverReadAutomatically() {
        val owner = SUMHTemporaryMapsOperations(SavedStateHandle()); val service = SUMHTemporaryMapsUiFixtures.Service()
        val restore = StateRestorationTester(compose)
        restore.setContent { TemporaryMapsTestTheme { TemporaryMapsControls(true, owner, service, service.tracking) } }
        compose.onNodeWithText(text(R.string.sumh_temporary_maps_open)).performClick()
        restore.emulateSavedInstanceStateRestore()
        compose.onNode(isDialog()).assertExists()
        assertEquals(0, service.reads); assertTrue(service.pages.isEmpty()); assertTrue(service.removes.isEmpty())
        compose.onNodeWithTag("temporary-maps-close").performClick()
        compose.onNodeWithText(text(R.string.sumh_temporary_maps_open)).performClick()
        assertEquals(0, service.reads)
    }

    @Test fun partialRowsAreLabeledAndExactSelectionRequiresConfirmation() {
        val owner = SUMHTemporaryMapsOperations(SavedStateHandle()); val service = SUMHTemporaryMapsUiFixtures.Service()
        service.pageReader = { SUMHTemporaryMapsUiFixtures.page(end = false) }
        show(owner, service); load(owner, service)
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_temporary_maps_partial, 1)).assertExists()
        assertTrue(service.removes.isEmpty())
        go("temporary-maps-select:18446744073709551615:0").performClick()
        compose.onNodeWithTag("temporary-maps-remove").assertIsEnabled()
        assertTrue(service.removes.isEmpty())
        go("temporary-maps-path:18446744073709551615:0").assertTextEquals("/example/映射.so ")
        capture("temporary-maps-confirm")
        compose.onNodeWithTag("temporary-maps-remove").performClick(); idle(owner)
        go("temporary-maps-result").assertTextEquals(text(R.string.sumh_temporary_maps_deleted))
        assertEquals(1, service.removes.size)
        assertEquals("18446744073709551615", service.removes.single().row.targetIno)
        go("temporary-maps-operation-id").assertTextEquals("01".repeat(16))
        assertEquals(0, service.legacyRemoves)
        compose.onNodeWithTag("temporary-maps-remove").assertDoesNotExist()
    }

    @Test fun unknownOutcomeRetainsIdentityAndNoDeleteUntilRefresh() {
        val owner = SUMHTemporaryMapsOperations(SavedStateHandle()); val service = SUMHTemporaryMapsUiFixtures.Service()
        service.pageReader = { SUMHTemporaryMapsUiFixtures.page(hex = "fffe2f") }
        service.remover = { SUMHTemporaryMapsRemoveResult(SUMHTemporaryMapsRemoveStatus.Unknown, "outcome_unknown") }
        show(owner, service); load(owner, service)
        go("temporary-maps-select:18446744073709551615:0").performClick()
        go("temporary-maps-path:18446744073709551615:0").assertTextEquals("fffe2f")
        compose.onNodeWithTag("temporary-maps-remove").performClick(); idle(owner)
        go("temporary-maps-result").assertTextEquals(text(R.string.sumh_temporary_maps_unknown))
            .assert(SemanticsMatcher.expectValue(SemanticsProperties.LiveRegion, LiveRegionMode.Polite))
        go("temporary-maps-back").performClick()
        go("temporary-maps-select:18446744073709551615:0").assertIsNotEnabled()
        assertEquals(1, service.removes.size); assertEquals(1, service.pages.size)
    }

    @Test fun staleRejectionHasSpecificRecoveryAndDoesNotBecomeUnknown() {
        val owner = SUMHTemporaryMapsOperations(SavedStateHandle()); val service = SUMHTemporaryMapsUiFixtures.Service()
        service.remover = { SUMHTemporaryMapsRemoveResult(SUMHTemporaryMapsRemoveStatus.NotApplied, "snapshot_changed", 116, true) }
        show(owner, service); load(owner, service)
        go("temporary-maps-select:18446744073709551615:0").performClick()
        compose.onNodeWithTag("temporary-maps-remove").performClick(); idle(owner)
        go("temporary-maps-result").assertTextEquals(text(R.string.sumh_temporary_maps_remove_stale))
            .assert(SemanticsMatcher.expectValue(SemanticsProperties.LiveRegion, LiveRegionMode.Polite))
        compose.onNodeWithText(text(R.string.sumh_temporary_maps_unknown)).assertDoesNotExist()
        go("temporary-maps-refresh").assertIsEnabled()
        assertEquals(1, service.removes.size); assertFalse(owner.canRemove)
    }

    @Test fun closeReopenAndCompositionRestoreAttachToOneInFlightRemoval() {
        val owner = SUMHTemporaryMapsOperations(SavedStateHandle()); val service = SUMHTemporaryMapsUiFixtures.Service()
        val gate = CompletableDeferred<SUMHTemporaryMapsRemoveResult>(); service.remover = { gate.await() }
        val restore = StateRestorationTester(compose)
        restore.setContent { TemporaryMapsTestTheme { TemporaryMapsControls(true, owner, service, service.tracking) } }
        compose.onNodeWithText(text(R.string.sumh_temporary_maps_open)).performClick(); load(owner, service)
        go("temporary-maps-select:18446744073709551615:0").performClick()
        compose.onNodeWithTag("temporary-maps-remove").performClick(); until { service.removes.size == 1 }
        compose.onNodeWithTag("temporary-maps-remove").assertIsNotEnabled()
        compose.onNodeWithTag("temporary-maps-close").assertIsEnabled().performClick()
        compose.onNodeWithText(text(R.string.sumh_temporary_maps_open)).performClick()
        restore.emulateSavedInstanceStateRestore()
        assertTrue(owner.busy); assertEquals(1, service.removes.size)
        compose.runOnIdle { gate.complete(SUMHTemporaryMapsRemoveResult(SUMHTemporaryMapsRemoveStatus.Deleted, "deleted", 0, true, true)) }
        idle(owner); go("temporary-maps-result").assertTextEquals(text(R.string.sumh_temporary_maps_deleted))
        assertEquals(1, service.removes.size)
    }

    @Test fun activityRecreationAndBackgroundCompletionRetainOneOwnerWithoutReplay() {
        val service = SUMHTemporaryMapsUiFixtures.Service(); val gate = CompletableDeferred<SUMHTemporaryMapsRemoveResult>()
        service.remover = { gate.await() }
        lateinit var first: SUMHTemporaryMapsOperations
        fun attach(activity: ComponentActivity): SUMHTemporaryMapsOperations {
            val owner = ViewModelProvider(activity)[SUMHTemporaryMapsOperations::class.java]
            activity.setContent { TemporaryMapsTestTheme { TemporaryMapsDialog(owner, true, service, service.tracking) {} } }
            return owner
        }
        compose.activityRule.scenario.onActivity { first = attach(it) }
        load(first, service)
        go("temporary-maps-select:18446744073709551615:0").performClick()
        compose.onNodeWithTag("temporary-maps-remove").performClick(); until { service.removes.size == 1 }
        compose.activityRule.scenario.recreate()
        compose.activityRule.scenario.onActivity { assertSame(first, attach(it)) }
        assertTrue(first.busy); assertEquals(1, service.removes.size)
        compose.activityRule.scenario.moveToState(Lifecycle.State.CREATED)
        gate.complete(SUMHTemporaryMapsRemoveResult(SUMHTemporaryMapsRemoveStatus.Deleted, "deleted", 0, true, true))
        ShadowLooper.idleMainLooper()
        compose.activityRule.scenario.moveToState(Lifecycle.State.RESUMED)
        idle(first)
        go("temporary-maps-result").assertTextEquals(text(R.string.sumh_temporary_maps_deleted))
        assertEquals(1, service.removes.size)
    }

    @Test @Config(qualifiers = "zh-rCN-w320dp-h760dp-xhdpi")
    fun narrowChineseLargeTextKeepsLongIdentityAndConfirmationReachable() {
        RuntimeEnvironment.setFontScale(2f)
        val owner = SUMHTemporaryMapsOperations(SavedStateHandle()); val service = SUMHTemporaryMapsUiFixtures.Service()
        val path = "/中文目录/".repeat(24) + "末尾 "
        service.pageReader = { SUMHTemporaryMapsUiFixtures.page(path = path) }
        show(owner, service); load(owner, service)
        go("temporary-maps-select:18446744073709551615:0").performClick()
        go("temporary-maps-path:18446744073709551615:0").assertTextEquals(path)
        compose.onNodeWithTag("temporary-maps-close").assertIsDisplayed().assertIsEnabled()
        compose.onNodeWithTag("temporary-maps-remove").assertIsDisplayed().assertIsEnabled()
        capture("temporary-maps-narrow-large-text")
    }

    @Test @Config(qualifiers = "zh-rCN-w760dp-h360dp-land-xhdpi")
    fun landscapeDarkLargeTextKeepsRecoveryAndCloseReachable() {
        RuntimeEnvironment.setFontScale(2f)
        val owner = SUMHTemporaryMapsOperations(SavedStateHandle()); val service = SUMHTemporaryMapsUiFixtures.Service()
        service.capabilityReader = { SUMHTemporaryMapsCapabilities(error = "disconnected") }
        show(owner, service, dark = true); load(owner, service)
        go("temporary-maps-status").assertTextEquals(text(R.string.sumh_temporary_maps_support_unknown))
        capture("temporary-maps-landscape-large-text")
        go("temporary-maps-refresh").assertIsDisplayed().assertIsEnabled()
        compose.onNodeWithTag("temporary-maps-close").assertIsDisplayed().assertIsEnabled()
    }

    @Test fun trackingUnsupportedShowsUpdateRequirementButKeepsManualInventoryRead() {
        val owner = SUMHTemporaryMapsOperations(SavedStateHandle()); val service = SUMHTemporaryMapsUiFixtures.Service()
        service.tracking.support = SUMHTemporaryMapsSupport.Unsupported
        show(owner, service); load(owner, service)
        assertTrue(owner.fresh); assertFalse(owner.canSelect)
        go("temporary-maps-select:18446744073709551615:0").assertIsNotEnabled()
        assertTrue(service.removes.isEmpty()); assertEquals(0, service.legacyRemoves)
    }

    @Test fun historyQueriesAreManualAndForgetRequiresSeparateConfirmation() {
        val owner = SUMHTemporaryMapsOperations(SavedStateHandle()); val service = SUMHTemporaryMapsUiFixtures.Service()
        val f = SUMHTemporaryMapsTrackingFixtures; service.tracking.records = listOf(f.entry("unknown"))
        service.tracking.queryHandler = { _, _ -> service.tracking.records = listOf(f.recovered("historical")); true }
        show(owner, service); until { owner.history.loaded }
        assertEquals(0, service.reads); assertEquals(0, service.tracking.capabilityReads)
        assertTrue(service.tracking.queries.isEmpty()); assertTrue(service.removes.isEmpty())
        go("temporary-maps-history-toggle").assert(SemanticsMatcher.expectValue(SemanticsProperties.StateDescription,
            text(R.string.sumh_temporary_maps_history_collapsed))).performClick()
        go("temporary-maps-history-toggle").assert(SemanticsMatcher.expectValue(SemanticsProperties.StateDescription,
            text(R.string.sumh_temporary_maps_history_expanded)))
        go("temporary-maps-query:${f.id}").performClick(); idle(owner)
        assertEquals(listOf(f.id to 0), service.tracking.queries); assertFalse(owner.fresh)
        assertTrue(service.pages.isEmpty()); assertTrue(service.removes.isEmpty())
        go("temporary-maps-wait:${f.id}").performClick(); idle(owner)
        assertEquals(f.id to 5000, service.tracking.queries.last())
        go("temporary-maps-forget:${f.id}").performClick()
        assertTrue(service.tracking.forgotten.isEmpty())
        compose.onNodeWithTag("temporary-maps-forget-confirm").assertIsEnabled().performClick(); idle(owner)
        assertEquals(listOf(f.id), service.tracking.forgotten); assertTrue(owner.history.records.isEmpty())
        assertTrue(service.removes.isEmpty()); assertEquals(0, service.legacyRemoves)
    }

    @Test @Config(qualifiers = "zh-rCN-w320dp-h760dp-xhdpi")
    fun narrowLargeTextHistoryKeepsQueryActionsReachableWithoutReplay() {
        RuntimeEnvironment.setFontScale(2f)
        val owner = SUMHTemporaryMapsOperations(SavedStateHandle()); val service = SUMHTemporaryMapsUiFixtures.Service()
        val f = SUMHTemporaryMapsTrackingFixtures; service.tracking.records = listOf(f.recovered("historical"))
        show(owner, service); until { owner.history.loaded }
        go("temporary-maps-history-toggle").performClick()
        go("temporary-maps-query:${f.id}").assertIsEnabled().assertIsDisplayed()
        capture("temporary-maps-tracked-history-large-text")
        compose.onNodeWithTag("temporary-maps-close").assertIsDisplayed().assertIsEnabled()
        assertTrue(service.tracking.queries.isEmpty()); assertTrue(service.removes.isEmpty())
    }

    private fun capture(name: String) {
        val output = File("build/reports/sumh-layout/$name.png"); output.parentFile?.mkdirs()
        output.outputStream().use { compose.onNode(isDialog()).captureToImage().asAndroidBitmap().compress(Bitmap.CompressFormat.PNG, 100, it) }
    }
}
