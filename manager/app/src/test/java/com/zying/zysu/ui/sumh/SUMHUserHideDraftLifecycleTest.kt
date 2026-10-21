package com.zying.zysu.ui.sumh

import android.app.Application
import android.app.Instrumentation
import android.graphics.Bitmap
import android.os.Bundle
import android.os.Parcel
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Density
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.lifecycle.viewmodel.compose.viewModel
import com.zying.zysu.ui.util.rememberSnackbarController
import com.zying.zysu.R
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.shadows.ShadowLooper
import java.io.File
import java.io.IOException

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SUMHUserHideDraftLifecycleTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    @Test fun failedWriteKeepsInputAndExplicitRetryPersistsIt() {
        var fail = true
        var saved = UserHideDraftSnapshot()
        val storage = object : UserHideDraftStorage {
            override fun read() = saved
            override fun write(snapshot: UserHideDraftSnapshot) { if (fail) throw IOException("full"); saved = snapshot }
        }
        lateinit var drafts: UserHideDrafts
        compose.setContent { val scope = rememberCoroutineScope(); drafts = remember { UserHideDrafts(scope, storage) } }
        compose.waitUntil(5000) { drafts.loaded }
        compose.runOnIdle { assertTrue(drafts.updateAdd("/data/中文 ")) }
        compose.waitUntil(5000) { drafts.storageError }
        assertEquals("/data/中文 ", drafts.add)
        assertEquals("", saved.add)
        compose.runOnIdle { fail = false; drafts.retry() }
        compose.waitUntil(5000) { !drafts.saving && !drafts.storageError }
        assertEquals("/data/中文 ", saved.add)
        compose.runOnIdle { assertFalse(drafts.updateAdd("/" + "x".repeat(4095))) }
        assertEquals("/data/中文 ", drafts.add)
        assertEquals("/data/中文 ", saved.add)
    }

    @Test fun failedReadBlocksWritesUntilExplicitRetryRecoversOriginal() {
        var fail = true
        var writes = 0
        val storage = object : UserHideDraftStorage {
            override fun read(): UserHideDraftSnapshot { if (fail) throw IOException("future format"); return UserHideDraftSnapshot(add = "/old ") }
            override fun write(snapshot: UserHideDraftSnapshot) { writes++ }
        }
        lateinit var drafts: UserHideDrafts
        compose.setContent { val scope = rememberCoroutineScope(); drafts = remember { UserHideDrafts(scope, storage) } }
        compose.waitUntil(5000) { drafts.storageError }
        compose.runOnIdle { assertFalse(drafts.updateAdd("/new")); fail = false; drafts.retry() }
        compose.waitUntil(5000) { drafts.loaded }
        assertEquals("/old ", drafts.add)
        assertEquals(0, writes)
    }

    @Test fun largeDraftBodiesStayOutOfActivitySavedStateAndRecoverFromDisk() {
        lateinit var drafts: UserHideDrafts
        val directory = File(compose.activity.noBackupFilesDir, "sumh-drafts")
        compose.setContent { MaterialTheme {
            val operation: UserHideOperationStore = viewModel()
            drafts = operation.drafts(directory)
            UserHideRulesScreen(true, rememberSnackbarController(), {}, true, loadRules = { emptyList() })
        } }
        // The process-owned store resumes real IO on Android's main looper,
        // outside the Compose test clock used by rememberCoroutineScope.
        compose.waitUntil(5000) { ShadowLooper.idleMainLooper(); drafts.loaded }
        compose.runOnIdle {
            repeat(200) { assertTrue(drafts.update("/data/$it", "/" + "x".repeat(4000))) }
            assertTrue(drafts.finishBulk("/data/retry ", listOf("/data/retry ")))
            // A new Activity owner shares queued changes instead of loading stale disk data.
            assertSame(drafts, UserHideOperationStore().drafts(directory))
        }
        compose.waitUntil(10000) { ShadowLooper.idleMainLooper(); !drafts.saving || drafts.storageError }
        assertFalse(drafts.storageFailure.toString(), drafts.storageError)
        assertEquals(drafts.snapshot, UserHideDraftFile(directory).read())
        assertTrue(encodeUserHideDraft(drafts.snapshot).size > 750 * 1024)
        val saved = Bundle()
        compose.runOnUiThread { Instrumentation().callActivityOnSaveInstanceState(compose.activity, saved) }
        val parcel = Parcel.obtain()
        try {
            parcel.writeBundle(saved)
            assertTrue("SavedState unexpectedly contains large draft bodies: ${parcel.dataSize()}", parcel.dataSize() < 128 * 1024)
        } finally { parcel.recycle() }
    }

    @Test
    @Config(qualifiers = "zh-rCN-w360dp-h760dp-xhdpi")
    fun oversizedPasteKeepsInputAndFileRecoveryActionOnNarrowScreen() = checkOverflowUi(1f, "draft-limit-portrait")

    @Test
    @Config(qualifiers = "en-w760dp-h360dp-land-xhdpi")
    fun oversizedPasteKeepsActionsReachableWithLargeLandscapeText() = checkOverflowUi(2f, "draft-limit-landscape")

    private fun checkOverflowUi(fontScale: Float, imageName: String) {
        lateinit var drafts: UserHideDrafts
        compose.setContent { MaterialTheme {
            val operation: UserHideOperationStore = viewModel()
            drafts = operation.drafts(File(compose.activity.noBackupFilesDir, "sumh-drafts"))
            CompositionLocalProvider(LocalDensity provides Density(LocalDensity.current.density, fontScale)) {
                UserHideRulesScreen(true, rememberSnackbarController(), {}, true, loadRules = { emptyList() })
            }
        } }
        compose.waitUntil(5000) { ShadowLooper.idleMainLooper(); drafts.loaded }
        compose.onNodeWithContentDescription(compose.activity.getString(R.string.sumh_rules_more)).performClick()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_import)).performClick()
        val field = hasSetTextAction() and hasAnyAncestor(isDialog())
        compose.onNode(field).performTextReplacement("/data/保留 ")
        compose.onNode(field).performTextReplacement("x".repeat(USER_HIDE_BULK_BYTES + 1))
        compose.onNode(field).assertTextContains("/data/保留 ")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_draft_bulk_limit)).performScrollTo().assertIsDisplayed()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_rules_cancel)).assertIsDisplayed()
        val image = compose.onNode(isDialog()).captureToImage().asAndroidBitmap()
        val file = File("build/reports/sumh-layout/$imageName.png")
        file.parentFile!!.mkdirs()
        file.outputStream().use { image.compress(Bitmap.CompressFormat.PNG, 100, it) }
        compose.onNode(field).performScrollTo().performTextReplacement("")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_draft_import_file)).performScrollTo().assertIsEnabled()
    }
}
