package com.zying.zysu.ui.component

import android.app.Application
import android.content.Context
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import com.zying.zysu.R
import com.zying.zysu.ui.screen.home.HomeLayoutStyle
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import ui.screen.moreSettings.component.HomeLayoutSetting
import kotlin.test.assertEquals

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class HomeLayoutSettingsTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    @Test fun pickerPersistsChoiceAndReflectsRestoredPreferences() {
        val prefs = compose.activity.getSharedPreferences("settings", Context.MODE_PRIVATE)
        prefs.edit().clear().putString("home_layout", "list").commit()
        compose.setContent { MaterialTheme { HomeLayoutSetting() } }
        val title = compose.activity.getString(R.string.appearance_home_layout)
        val circle = compose.activity.getString(R.string.home_layout_circle)
        val focus = compose.activity.getString(R.string.home_layout_focus)
        val stats = compose.activity.getString(R.string.home_layout_stats)
        compose.onNodeWithText(circle).assertIsDisplayed()
        compose.onNodeWithText(title).performClick()
        compose.onNodeWithText(focus).performScrollTo().performClick()
        assertEquals(HomeLayoutStyle.Focus.id, prefs.getString("home_layout", null))
        compose.onNodeWithText(focus).assertIsDisplayed()
        compose.runOnIdle { prefs.edit().putString("home_layout", HomeLayoutStyle.Stats.id).commit() }
        compose.onNodeWithText(stats).assertIsDisplayed()
    }
}
