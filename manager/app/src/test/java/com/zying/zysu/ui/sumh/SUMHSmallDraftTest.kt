package com.zying.zysu.ui.sumh

import android.app.Application
import android.app.Instrumentation
import android.os.Bundle
import android.os.Parcel
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.foundation.layout.Column
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import com.zying.zysu.R
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SUMHSmallDraftTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    @Test fun byteBudgetCountsUtf8WithoutChangingLiteralText() {
        assertTrue(sumhSmallDraftFits("中".repeat(1365)))
        assertFalse(sumhSmallDraftFits("中".repeat(1366)))
        assertTrue(sumhSmallDraftFits("x".repeat(4095)))
        assertFalse(sumhSmallDraftFits("x".repeat(4096)))
    }

    @Test fun oversizedMountSourcePasteKeepsDraftWithRecoveryMessage() {
        compose.setContent { MaterialTheme { MountSourceInput("KSU", true, {}) } }
        val field = compose.onNode(hasSetTextAction())
        field.performTextReplacement("draft")
        field.performTextReplacement("x".repeat(100000))
        field.assertTextContains("draft")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_small_draft_limit)).assertIsDisplayed()
        field.performTextReplacement("short")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_small_draft_limit)).assertDoesNotExist()
    }

    @Test fun oversizedLegacyMountSourceIsVisibleButNotCopiedIntoSavedState() {
        val original = "source".repeat(50000)
        compose.setContent { MaterialTheme { MountSourceInput(original, true, {}) } }
        compose.onNode(hasSetTextAction()).assertTextContains(original)
        val saved = Bundle()
        compose.runOnUiThread { Instrumentation().callActivityOnSaveInstanceState(compose.activity, saved) }
        val parcel = Parcel.obtain()
        try { parcel.writeBundle(saved); assertTrue("SavedState size: ${parcel.dataSize()}", parcel.dataSize() < 128 * 1024) }
        finally { parcel.recycle() }
    }

    @Test fun oversizedTemporaryMapsNumbersAndPathDoNotEnterSavedInput() {
        compose.setContent { MaterialTheme { Column { MapsSpoofControls(true, {}, { _, _, _, _, _ -> }) } } }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_maps_runtime_add)).performClick()
        val inode = compose.onNode(hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_maps_target_ino)))
        inode.performTextReplacement("123")
        inode.performTextReplacement("9".repeat(100000))
        inode.assertTextContains("123")
        val path = compose.onNode(hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_maps_spoof_path)))
        path.performScrollTo().performTextReplacement("/literal ")
        path.performTextReplacement("x".repeat(100000))
        path.assertTextContains("/literal ")
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_small_draft_limit)).performScrollTo().assertIsDisplayed()
    }
}
