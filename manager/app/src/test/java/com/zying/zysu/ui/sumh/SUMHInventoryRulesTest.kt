package com.zying.zysu.ui.sumh

import android.app.Application
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.StateRestorationTester
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.*
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as Controller
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.awaitCancellation
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
class SUMHInventoryRulesTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    private val f = SUMHInventoryFixtures
    private val mapping = SUMHInventoryCategory.Mapping
    private fun text(id: Int) = compose.activity.getString(id)
    private fun entry(category: SUMHInventoryCategory = mapping, value: JSONObject = f.entry(category)) =
        SUMHInventoryParser.parse(f.page(category, entries = listOf(value)).toString()).entries.single()
    private fun complete(entries: List<SUMHInventoryEntry> = emptyList()) = SUMHInventoryCollection(
        status = SUMHInventoryRead.Complete, entries = entries,
        identity = SUMHInventoryIdentity("boot", "23", "9", 127),
        completedCategories = SUMHInventoryCategory.entries.filter { it.readable }.toSet(),
        unsupportedCategories = setOf(SUMHInventoryCategory.Kstat, SUMHInventoryCategory.OverlayXattr, SUMHInventoryCategory.Vnode))
    private fun load() = compose.onNodeWithTag("inventory_load").performScrollTo().performClick()
    private fun show(state: SUMHInventoryCollection) = compose.setContent {
        MaterialTheme { SUMHInventoryView(state, false, false, true, {}, {}, {}) }
    }
    private fun clipboard() = (compose.activity.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager)
        .primaryClip!!.getItemAt(0).text.toString()

    @Test fun productionWorkspaceLoadsOnlyExplicitlyAndDoesNotReplayOnRecreation() {
        var reads = 0
        val restore = StateRestorationTester(compose)
        restore.setContent { MaterialTheme {
            SUMHRuntimeInventoryWorkspace(emptyList(), Controller.SUMHStatus.AVAILABLE, true, {}, {},
                read = { reads++; complete() })
        } }
        assertEquals(0, reads)
        compose.onNodeWithTag("inventory_empty").assertDoesNotExist()
        load()
        compose.onNodeWithTag("inventory_status").assertTextEquals(text(R.string.sumh_inventory_complete))
        assertEquals(1, reads)
        restore.emulateSavedInstanceStateRestore()
        compose.onNodeWithTag("inventory_status").assertTextEquals(text(R.string.sumh_inventory_not_loaded))
        compose.onNodeWithTag("inventory_empty").assertDoesNotExist()
        assertEquals(1, reads)
    }

    @Test fun refreshImmediatelyClearsOldCompleteCollectionAndCancellationRetainsOnlyNewPartialRecords() {
        var reads = 0
        val gate = CompletableDeferred<Unit>()
        val first = entry()
        val next = entry(SUMHInventoryCategory.UserHide)
        compose.setContent { MaterialTheme {
            SUMHRuntimeInventoryWorkspace(emptyList(), Controller.SUMHStatus.AVAILABLE, true, {}, {}, read = { progress ->
                reads++
                if (reads == 1) complete(listOf(first)) else {
                    gate.await()
                    progress(complete(listOf(next)).copy(status = SUMHInventoryRead.Loading))
                    awaitCancellation()
                }
            })
        } }
        load()
        compose.onNodeWithTag("inventory_list").performScrollToNode(hasTestTag("inventory_mapping:1"))
        compose.onNodeWithTag("inventory_mapping:1").assertExists()
        load()
        compose.onNodeWithTag("inventory_status").assertTextEquals(text(R.string.sumh_inventory_loading))
        compose.onNodeWithTag("inventory_mapping:1").assertDoesNotExist()
        compose.runOnIdle { gate.complete(Unit) }
        compose.waitForIdle()
        compose.onNodeWithTag("inventory_cancel").performScrollTo().performClick()
        compose.onNodeWithTag("inventory_status").assertTextEquals(text(R.string.sumh_inventory_cancelled))
        compose.onNodeWithTag("inventory_list").performScrollToNode(hasTestTag("inventory_user_hide:1"))
        compose.onNodeWithTag("inventory_user_hide:1").assertExists()
        compose.onNodeWithText(text(R.string.sumh_inventory_hide_scope)).assertExists()
        assertEquals(2, reads)
    }

    @Test fun unknownPartialAndUnsupportedEmptyNeverBecomeConfirmedEmpty() {
        val state = mutableStateOf(SUMHInventoryCollection())
        compose.setContent { MaterialTheme { SUMHInventoryView(state.value, false, false, true, {}, {}, {}) } }
        for (status in listOf(SUMHInventoryRead.NotLoaded, SUMHInventoryRead.Partial, SUMHInventoryRead.Unsupported,
            SUMHInventoryRead.Stale, SUMHInventoryRead.Failed, SUMHInventoryRead.Cancelled, SUMHInventoryRead.Unavailable)) {
            compose.runOnIdle { state.value = SUMHInventoryCollection(status = status) }
            compose.onNodeWithTag("inventory_empty").assertDoesNotExist()
        }
        compose.runOnIdle { state.value = complete() }
        compose.onNodeWithTag("inventory_list").performScrollToNode(hasTestTag("inventory_empty"))
        compose.onNodeWithTag("inventory_empty").assertExists()
        compose.onNodeWithText(text(R.string.sumh_inventory_empty)).assertExists()
    }

    @Test fun exactUtf8AndHexCopiesPreserveControlsAndNoOrdinalDeletionIsOffered() {
        val item = entry()
        show(complete(listOf(item)))
        compose.onNodeWithTag("inventory_list").performScrollToNode(hasTestTag("inventory_mapping:1"))
        compose.onNodeWithTag("inventory_mapping:1").performClick()
        compose.onNode(hasText(item.paths.getValue("visible_path").display()) and hasAnyAncestor(isDialog())).assertExists()
        compose.onNodeWithText(text(R.string.sumh_inventory_identity_scope)).assertExists()
        compose.onAllNodesWithText(text(R.string.sumh_inventory_copy_utf8))[0].performScrollTo().performClick()
        assertEquals("/visible 1\n名字 ", clipboard())
        compose.onAllNodesWithText(text(R.string.sumh_inventory_copy_hex))[0].performScrollTo().performClick()
        assertEquals(item.paths.getValue("visible_path").hex, clipboard())
        compose.onNodeWithText(text(R.string.delete)).assertDoesNotExist()
    }

    @Test fun invalidUtf8StaysLabeledHexAndNeverOffersTextCopy() {
        val item = entry(SUMHInventoryCategory.Inject, f.entry(SUMHInventoryCategory.Inject).put("path",
            JSONObject().put("encoding", "hex").put("value", "2fff80").put("byte_length", 3)))
        show(complete(listOf(item)))
        compose.onNodeWithTag("inventory_list").performScrollToNode(hasTestTag("inventory_inject:1"))
        compose.onNodeWithTag("inventory_inject:1").performClick()
        compose.onNode(hasText("2fff80") and hasAnyAncestor(isDialog())).assertExists()
        compose.onNode(hasText(compose.activity.getString(R.string.sumh_inventory_hex, 3)) and hasAnyAncestor(isDialog())).assertExists()
        compose.onNodeWithText(text(R.string.sumh_inventory_copy_utf8)).assertDoesNotExist()
        compose.onNodeWithText(text(R.string.sumh_inventory_copy_hex)).performScrollTo().performClick()
        assertEquals("2fff80", clipboard())
    }

    @Test fun productionLegacyFallbackRemainsExplicitlyIncomplete() {
        var refreshes = 0
        var clears = 0
        compose.setContent { MaterialTheme {
            SUMHRuntimeInventoryWorkspace(emptyList(), Controller.SUMHStatus.AVAILABLE, true,
                { refreshes++ }, { clears++ }, read = { error("unexpected read") })
        } }
        compose.onNodeWithTag("inventory_legacy").performScrollTo().performClick()
        compose.onNodeWithText(text(R.string.sumh_rules_runtime_incomplete)).assertExists()
        compose.onNodeWithText(text(R.string.sumh_inventory_back)).performClick()
        compose.onNodeWithTag("inventory_status").assertTextEquals(text(R.string.sumh_inventory_not_loaded))
        assertEquals(0, refreshes)
        assertEquals(0, clears)
    }

    @Test
    @Config(qualifiers = "zh-rCN-w360dp-h760dp-xhdpi")
    fun narrowLargeTextKeepsManualActionsAndRowDetailsReachable() {
        RuntimeEnvironment.setFontScale(2f)
        try {
            compose.setContent { MaterialTheme {
                SUMHRuntimeInventoryWorkspace(emptyList(), Controller.SUMHStatus.AVAILABLE, true, {}, {},
                    read = { complete(listOf(entry())) })
            } }
            load()
            compose.onNodeWithTag("inventory_load").performScrollTo().assertIsDisplayed()
            screenshot("inventory-narrow-large-text-actions.png")
            compose.onNodeWithTag("inventory_list").performScrollToNode(hasTestTag("inventory_mapping:1"))
            compose.onNodeWithTag("inventory_mapping:1").performClick()
            compose.onAllNodesWithText(text(R.string.sumh_inventory_copy_hex))[0].performScrollTo().assertIsDisplayed()
            screenshot("inventory-narrow-large-text-details.png")
        } finally { RuntimeEnvironment.setFontScale(1f) }
    }

    @Test
    @Config(qualifiers = "zh-rCN-w760dp-h360dp-land-xhdpi")
    fun landscapeLargeTextKeepsCancelReachable() {
        RuntimeEnvironment.setFontScale(2f)
        try {
            compose.setContent { MaterialTheme {
                SUMHRuntimeInventoryWorkspace(emptyList(), Controller.SUMHStatus.AVAILABLE, true, {}, {},
                    read = { awaitCancellation() })
            } }
            load()
            compose.onNodeWithTag("inventory_cancel").performScrollTo().assertIsDisplayed()
            screenshot("inventory-landscape-large-text-cancel.png")
            compose.onNodeWithTag("inventory_cancel").performClick()
            compose.onNodeWithTag("inventory_status").assertTextEquals(text(R.string.sumh_inventory_cancelled))
        } finally { RuntimeEnvironment.setFontScale(1f) }
    }

    private fun screenshot(name: String) {
        val output = File("build/reports/sumh-layout/$name")
        output.parentFile!!.mkdirs()
        output.outputStream().use { compose.onRoot().captureToImage().asAndroidBitmap().compress(Bitmap.CompressFormat.PNG, 100, it) }
    }
}
