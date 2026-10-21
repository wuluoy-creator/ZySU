package com.zying.zysu.ui.sumh

import android.app.Application
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.*
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.StateRestorationTester
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.unit.Density
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.*
import org.json.JSONObject
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.shadows.ShadowLooper
import java.io.File
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "en-w360dp-h760dp-xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SUMHOperationHistoryTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    private val t = SUMHTrackerFixtures
    private fun text(id: Int) = compose.activity.getString(id)
    private fun open(tracker: SUMHOperationTracker) {
        compose.waitUntil(5000) { tracker.snapshot.value.loaded }
        compose.onNodeWithTag("sumh_operation_history").performClick()
    }
    @Test fun recoveryRequiresExplicitQueryAndLocalRemovalHasNoServerEffect() {
        val storage = SUMHTrackerFixtures.Memory(listOf(t.entry("unknown"))); val calls = mutableListOf<String>()
        val tracker = SUMHOperationTracker(storage) { raw ->
            val action = JSONObject(raw).getString("action"); calls += action
            t.reply(when(action) { "show" -> t.history(); "result" -> t.recovered(); else -> error("no mutation") })
        }
        compose.setContent { MaterialTheme { SUMHOperationHistory(tracker, true) } }
        open(tracker); assertTrue(calls.isEmpty())
        compose.onNodeWithTag("sumh_operation_query_${t.id}").performScrollTo().performClick()
        compose.waitUntil(5000) { tracker.snapshot.value.records.single().recovered.isNotEmpty() }
        assertEquals(listOf("show", "result"), calls)
        compose.onNodeWithText(text(R.string.sumh_operations_forget)).performScrollTo().performClick()
        compose.onNodeWithText(text(R.string.sumh_operations_forget_hint)).assertIsDisplayed()
        compose.onAllNodesWithText(text(android.R.string.ok)).onLast().performClick()
        compose.waitUntil(5000) { tracker.snapshot.value.records.isEmpty() }
        assertEquals(listOf("show", "result"), calls)
    }
    @Test fun recreationRestoresSelectedIdWithoutQueryOrClearingNewerDraft() {
        val storage = SUMHTrackerFixtures.Memory(listOf(t.entry("unknown").copy(observation = t.history().toString(), recovered = t.recovered().toString())))
        var calls = 0
        fun owner() = SUMHOperationTracker(storage) { calls++; error("no automatic network") }
        var tracker by mutableStateOf(owner())
        val draftStore = SUMHEditorDrafts.forDirectory(File(compose.activity.applicationContext.noBackupFilesDir, "sumh-drafts"))
        val restoration = StateRestorationTester(compose)
        restoration.setContent { MaterialTheme { SUMHOperationHistory(tracker, true) } }
        open(tracker)
        compose.waitUntil(5000) { ShadowLooper.idleMainLooper(); draftStore.loaded }
        val draft = HideDefinitionDraft.initial(SUMHHideDefinitionsFixtures.collection().rows.first(), SUMHHideDefinitionsFixtures.revision)
            .copy(name = "newer local edit")
        compose.runOnIdle { assertTrue(draftStore.ensure(draft.key, draft.record())) }
        compose.onNodeWithTag("sumh_operation_detail_${t.id}").performScrollTo().performClick()
        compose.runOnIdle { tracker = owner() }
        restoration.emulateSavedInstanceStateRestore()
        compose.waitUntil(5000) { tracker.snapshot.value.loaded }
        compose.onAllNodesWithText(text(R.string.sumh_operations_details)).onLast().assertExists()
        compose.onNodeWithText(text(R.string.sumh_operations_received_heading)).assertExists()
        assertEquals(0, calls); assertTrue(storage.records.single().recovered.isNotEmpty())
        assertEquals(draft.record(), draftStore.snapshot[draft.key])
    }
    @Test @Config(qualifiers = "zh-rCN-w360dp-h760dp-xhdpi")
    fun chinesePortraitSeparatesHistoricReceiptFromCurrentState() = layout(1f, "sumh-operation-history-portrait")
    @Test @Config(qualifiers = "en-w760dp-h360dp-land-xhdpi")
    fun landscapeLargeTextKeepsQueryAndDismissReachable() = layout(2f, "sumh-operation-history-landscape")
    private fun layout(scale: Float, imageName: String) {
        val storage = SUMHTrackerFixtures.Memory(listOf(t.entry("unknown").copy(observation = t.history(currentBoot = "new-boot").toString(),
            recovered = t.recovered("new-boot").toString())))
        val tracker = SUMHOperationTracker(storage) { error("must not execute") }
        compose.setContent { MaterialTheme(colorScheme = darkColorScheme()) {
            CompositionLocalProvider(LocalDensity provides Density(LocalDensity.current.density, scale)) { SUMHOperationHistory(tracker, true) }
        } }
        open(tracker)
        compose.onNodeWithTag("sumh_operation_detail_${t.id}").performScrollTo().performClick()
        compose.onAllNodesWithText(text(R.string.sumh_operations_query)).onLast().assertIsDisplayed().assertIsEnabled()
        compose.onAllNodesWithText(text(android.R.string.ok)).onLast().assertIsDisplayed()
        val image = compose.onAllNodes(isDialog()).onLast().captureToImage().asAndroidBitmap()
        val file = File("build/reports/sumh-layout/$imageName.png"); file.parentFile!!.mkdirs()
        file.outputStream().use { image.compress(Bitmap.CompressFormat.PNG, 100, it) }
        compose.onNodeWithTag("sumh_operation_detail_content").performScrollToNode(hasText(text(R.string.sumh_operations_current_hint)))
        compose.onNodeWithText(text(R.string.sumh_operations_current_hint)).assertIsDisplayed()
        compose.onAllNodesWithText(text(android.R.string.ok)).onLast().performClick()
    }
}
