package com.zying.zysu.ui.component

import android.app.Application
import android.content.Context
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.unit.dp
import androidx.lifecycle.ViewModelStore
import com.zying.zysu.R
import com.zying.zysu.ui.screen.home.HomeLayoutContent
import com.zying.zysu.ui.screen.home.HomeLayoutState
import com.zying.zysu.ui.screen.home.HomeLayoutStyle
import com.zying.zysu.ui.screen.home.HomeStatusPresentation
import com.zying.zysu.ui.viewmodel.HomePreferenceOsShadow
import com.zying.zysu.ui.viewmodel.HomeViewModel
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import ui.screen.moreSettings.component.HomeLayoutSetting
import ui.screen.moreSettings.component.homeLayoutTitle
import kotlin.test.assertEquals

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], shadows = [HomePreferenceOsShadow::class])
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

    @Test fun everyPickerChoiceUpdatesTheRetainedHomeWithoutRestarting() {
        val prefs = compose.activity.getSharedPreferences("settings", Context.MODE_PRIVATE)
        prefs.edit().clear().commit()
        val model = HomeViewModel()
        val store = ViewModelStore().apply { put("home", model) }
        model.loadUserSettings(compose.activity)
        val homeState = HomeLayoutState(
            status = HomeStatusPresentation(title = "Working", installed = true),
            superuserCount = 12, moduleCount = 9,
        )
        compose.setContent {
            MaterialTheme {
                Column {
                    HomeLayoutSetting()
                    Column(Modifier.heightIn(max = 500.dp).verticalScroll(rememberScrollState())) {
                        HomeLayoutContent(style = model.homeLayout, state = homeState)
                    }
                }
            }
        }
        try {
            HomeLayoutStyle.entries.forEach { style ->
                compose.onNodeWithText(compose.activity.getString(R.string.appearance_home_layout)).performClick()
                compose.onAllNodesWithText(compose.activity.getString(homeLayoutTitle(style)))
                    .onLast().performScrollTo().performClick()
                compose.waitForIdle()
                assertEquals(style, model.homeLayout)
                assertEquals(style.id, prefs.getString("home_layout", null))
                compose.onNodeWithTag("home-layout-${style.id}").assertExists()
                HomeLayoutStyle.entries.filter { it != style }.forEach { other ->
                    compose.onNodeWithTag("home-layout-${other.id}").assertDoesNotExist()
                }
            }
        } finally {
            compose.runOnIdle { store.clear() }
        }
    }
}
