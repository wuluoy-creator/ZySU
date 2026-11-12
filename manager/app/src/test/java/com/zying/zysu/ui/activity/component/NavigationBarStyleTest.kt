package com.zying.zysu.ui.activity.component

import android.app.Application
import androidx.activity.ComponentActivity
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Settings
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.material3.Text
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.assertIsSelected
import androidx.compose.ui.test.performClick
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import com.zying.zysu.R
import kotlin.test.assertEquals

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class NavigationBarStyleTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    private val context: Application get() = RuntimeEnvironment.getApplication()

    @Before
    fun reset() {
        NavigationBarStylePreferences.preferences(context).edit().clear().commit()
    }

    @Test
    fun missingAndUnknownValuesKeepExistingCapsule() {
        assertEquals(NavigationBarStyle.Glass, NavigationBarStylePreferences.read(context))
        NavigationBarStylePreferences.preferences(context).edit()
            .putString(NavigationBarStylePreferences.STYLE_KEY, "removed-style").commit()
        assertEquals(NavigationBarStyle.Glass, NavigationBarStylePreferences.read(context))
        assertEquals(listOf("glass", "material", "floating_standard", "floating_drawer"),
            NavigationBarStyle.entries.map { it.preferenceValue })
    }

    @Test
    fun eachChoiceSurvivesReadback() {
        NavigationBarStyle.entries.forEach { style ->
            NavigationBarStylePreferences.set(context, style)
            assertEquals(style, NavigationBarStylePreferences.read(context))
        }
    }

    @Test
    fun activeObserverFollowsSelectionAndReset() {
        compose.setContent {
            val style by rememberNavigationBarStyle()
            Text(style.preferenceValue)
        }
        compose.onNodeWithText("glass").assertExists()
        NavigationBarStyle.entries.forEach { style ->
            compose.runOnIdle { NavigationBarStylePreferences.set(context, style) }
            compose.onNodeWithText(style.preferenceValue).assertExists()
        }
        compose.runOnIdle { NavigationBarStylePreferences.preferences(context).edit().clear().commit() }
        compose.onNodeWithText("glass").assertExists()
    }

    @Test
    fun switchingStylePreservesDestinationSelectionAndBadgeMeaning() {
        compose.setContent {
            val style by rememberNavigationBarStyle()
            var selected by remember { mutableStateOf("home") }
            MaterialTheme {
                BottomNavigationBar(
                    style = style,
                    items = listOf(
                        NavigationBarItem("home", "Home", Icons.Default.Home, selected == "home", badgeCount = 12),
                        NavigationBarItem("settings", "Settings", Icons.Default.Settings, selected == "settings"),
                    ),
                    onSelected = { selected = it },
                    windowInsets = WindowInsets(0, 0, 0, 0),
                )
            }
        }
        compose.onNodeWithTag("navigation-item-settings").performClick().assertIsSelected()
        NavigationBarStyle.entries.forEach { style ->
            compose.runOnIdle { NavigationBarStylePreferences.set(context, style) }
            compose.onNodeWithTag("navigation-item-settings").assertIsSelected()
            compose.onNodeWithContentDescription(context.resources.getQuantityString(R.plurals.ui_navigation_item_count, 12, 12))
                .assertExists()
        }
        compose.onNodeWithTag("navigation-item-home").performClick().assertIsSelected()
    }
}
