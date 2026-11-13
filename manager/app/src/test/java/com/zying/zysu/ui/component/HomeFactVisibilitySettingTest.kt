package com.zying.zysu.ui.component

import android.app.Application
import android.content.Context
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.Density
import androidx.lifecycle.ViewModelStore
import com.zying.zysu.R
import com.zying.zysu.ui.theme.withAppearance
import com.zying.zysu.ui.screen.home.*
import com.zying.zysu.ui.viewmodel.HomePreferenceOsShadow
import com.zying.zysu.ui.viewmodel.HomeViewModel
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import ui.screen.moreSettings.component.HomeFactVisibilitySetting
import java.io.File
import kotlin.test.assertEquals
import kotlin.test.assertFalse

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "en-w375dp-h900dp-mdpi",
    shadows = [HomePreferenceOsShadow::class])
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class HomeFactVisibilitySettingTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    @Test fun allThirteenPickerSwitchesUpdateRetainedHomeAndSurviveRestoration() {
        val preferences = compose.activity.getSharedPreferences("settings", Context.MODE_PRIVATE)
        preferences.edit().clear().commit()
        val model = HomeViewModel()
        val store = ViewModelStore().apply { put("home", model) }
        model.loadUserSettings(compose.activity)
        compose.setContent { MaterialTheme { HomeFactVisibilitySetting() } }
        try {
            compose.onNodeWithText(compose.activity.getString(R.string.home_fact_visibility_title)).performClick()
            HomeFactField.entries.forEachIndexed { index, field ->
                compose.onNodeWithText(compose.activity.getString(field.label)).performScrollTo().assertIsOn().performClick()
                compose.waitForIdle()
                assertFalse(field.id in model.visibleHomeFactIds)
                assertEquals(HomeFactField.entries.size - index - 1, HomeFactVisibility.read(preferences).size)
                compose.onNodeWithText(compose.activity.getString(field.label)).assertIsOff()
            }
            val directory = File("../../out/custom-home-facts-ui").apply { mkdirs() }
            File(directory, "visibility-picker.png").outputStream().use {
                compose.onNode(isDialog()).captureToImage().asAndroidBitmap().compress(Bitmap.CompressFormat.PNG, 100, it)
            }
            compose.onNodeWithText(compose.activity.getString(android.R.string.ok)).performClick()
            compose.onNodeWithText(compose.activity.getString(R.string.home_fact_visibility_count, 0, 13)).assertIsDisplayed()
            compose.runOnIdle { HomeFactVisibility.setVisible(preferences, HomeFactField.Device, true) }
            compose.onNodeWithText(compose.activity.getString(R.string.home_fact_visibility_count, 1, 13)).assertIsDisplayed()
            compose.onNodeWithText(compose.activity.getString(R.string.home_fact_visibility_title)).performClick()
            compose.onNodeWithText(compose.activity.getString(HomeFactField.Device.label)).performScrollTo().assertIsOn()
        } finally {
            compose.runOnIdle { store.clear() }
        }
    }

    @Test fun darkLargeTextPickerKeepsEverySwitchReachable() {
        val preferences = compose.activity.getSharedPreferences("settings", Context.MODE_PRIVATE)
        preferences.edit().clear().commit()
        compose.setContent {
            // Dialogs create a new window; scale inherited typography rather than an outer density.
            MaterialTheme(colorScheme = darkColorScheme(),
                typography = com.zying.zysu.ui.theme.Typography.withAppearance(2f, "system")) { HomeFactVisibilitySetting() }
        }
        compose.onNodeWithText(compose.activity.getString(R.string.home_fact_visibility_title)).performClick()
        HomeFactField.entries.forEach { field ->
            compose.onNodeWithText(compose.activity.getString(field.label)).performScrollTo()
                .assertIsDisplayed().assertHeightIsAtLeast(48.dp).performClick().assertIsOff()
        }
        val directory = File("../../out/custom-home-facts-ui").apply { mkdirs() }
        File(directory, "visibility-picker-dark-large.png").outputStream().use {
            compose.onNode(isDialog()).captureToImage().asAndroidBitmap().compress(Bitmap.CompressFormat.PNG, 100, it)
        }
        compose.onNodeWithText(compose.activity.getString(android.R.string.ok)).assertIsDisplayed().performClick()
    }

    @Test fun hidingEveryFieldRemovesCorePanelAndRestoringOneFieldShowsItAgain() {
        val preferences = compose.activity.getSharedPreferences("settings", Context.MODE_PRIVATE)
        preferences.edit().clear().commit()
        val model = HomeViewModel()
        val store = ViewModelStore().apply { put("home", model) }
        model.loadUserSettings(compose.activity)
        val facts = HomeFactField.entries.filterNot { it.buildDetail }.map {
            HomeFact(it.id, compose.activity.getString(it.label), "Value ${it.id}")
        }
        val buildFacts = HomeFactField.entries.filter { it.buildDetail }.map {
            HomeFact(it.id, compose.activity.getString(it.label), "Value ${it.id}")
        }
        compose.setContent {
            MaterialTheme {
                Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp)) {
                    HomeLayoutContent(HomeLayoutStyle.Circle, HomeLayoutState(
                        status = HomeStatusPresentation("Working", installed = true),
                        facts = visibleHomeFacts(facts, model.visibleHomeFactIds),
                        buildFacts = visibleHomeFacts(buildFacts, model.visibleHomeFactIds),
                        showBuildDetails = true,
                    ))
                }
            }
        }
        try {
            HomeFactField.entries.forEach { field ->
                compose.onNodeWithTag("home-fact-${field.id}").performScrollTo().assertIsDisplayed()
                compose.runOnIdle { HomeFactVisibility.setVisible(preferences, field, false) }
                compose.onNodeWithTag("home-fact-${field.id}").assertDoesNotExist()
            }
            compose.onNodeWithText(compose.activity.getString(R.string.home_layout_essential)).assertDoesNotExist()
            compose.runOnIdle { HomeFactVisibility.setVisible(preferences, HomeFactField.Fingerprint, true) }
            compose.onNodeWithTag("home-fact-fingerprint").performScrollTo().assertIsDisplayed()
            compose.onNodeWithText(compose.activity.getString(R.string.home_layout_essential)).assertExists()
        } finally {
            compose.runOnIdle { store.clear() }
        }
    }
}
