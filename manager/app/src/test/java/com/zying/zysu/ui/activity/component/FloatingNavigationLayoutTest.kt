package com.zying.zysu.ui.activity.component

import android.app.Application
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.AdminPanelSettings
import androidx.compose.material.icons.outlined.Extension
import androidx.compose.material.icons.outlined.Home
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.component.FloatingNavigationScaffold
import com.zying.zysu.ui.component.LocalBottomBarPadding
import com.zying.zysu.ui.component.YukiPageScaffold
import com.zying.zysu.ui.component.YukiTopAppBar
import com.zying.zysu.ui.component.YukiTopBarTitle
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.File
import kotlin.test.assertEquals
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "en-w320dp-h800dp-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@OptIn(ExperimentalMaterial3Api::class)
class FloatingNavigationLayoutTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    private val keys = listOf("home", "auth", "modules", "settings")
    private var requiredBadgeWidthPx = 0f

    @Test fun standardUsesIconOnlyCompactGeometryAndSlidingSelection() {
        show(NavigationBarStyle.FloatingStandard)
        compose.onNodeWithTag("navigation-floating_standard").assertWidthIsEqualTo(242.dp).assertHeightIsEqualTo(68.dp)
        keys.forEach { compose.onNodeWithTag("navigation-item-$it").assertWidthIsEqualTo(52.dp).assertHeightIsEqualTo(52.dp) }
        compose.onAllNodesWithText("Home", useUnmergedTree = true).assertCountEquals(0)
        compose.onNodeWithContentDescription("Home").assertIsSelected()
        assertIndicatorMatches("navigation-standard-indicator", "home")
        saveFrame("standard-home")
        compose.onNodeWithTag("navigation-item-settings").performClick().assertIsSelected()
        assertIndicatorMatches("navigation-standard-indicator", "settings")
        saveFrame("standard-settings")
    }

    @Test fun drawerShowsOnlyTheExpandedDestinationLabel() {
        show(NavigationBarStyle.FloatingDrawer)
        compose.onNodeWithTag("navigation-floating_drawer").assertWidthIsEqualTo(288.dp).assertHeightIsEqualTo(64.dp)
        compose.onNodeWithTag("navigation-item-home").assertWidthIsEqualTo(116.dp)
        keys.drop(1).forEach { compose.onNodeWithTag("navigation-item-$it").assertWidthIsEqualTo(48.dp) }
        assertDrawerLabel("home")
        assertFullBadgeFits()
        saveFrame("drawer-home")
        compose.onNodeWithTag("navigation-item-modules").performClick().assertIsSelected()
        assertDrawerLabel("modules")
        assertIndicatorMatches("navigation-drawer-indicator", "modules")
        assertFullBadgeFits()
        val badge = compose.onNodeWithTag("navigation-badge-modules", useUnmergedTree = true).fetchSemanticsNode().boundsInRoot
        val label = compose.onNodeWithTag("navigation-drawer-text-modules", useUnmergedTree = true).fetchSemanticsNode().boundsInRoot
        assertTrue(badge.right <= label.left, "The selected destination label must not overlap its count badge")
        compose.onNodeWithTag("navigation-item-home").assertWidthIsEqualTo(48.dp)
        compose.onNodeWithContentDescription(compose.activity.resources.getQuantityString(R.plurals.ui_navigation_item_count, 128, 128))
            .assertExists()
        saveFrame("drawer-modules")
    }

    @Test fun drawerLargeTextKeepsTargetsAndSelectedLabelInsideTheCapsule() {
        show(NavigationBarStyle.FloatingDrawer, largeText = true, dark = true)
        compose.onNodeWithTag("navigation-item-settings").performClick().assertIsSelected()
        assertDrawerLabel("settings")
        assertFullBadgeFits()
        keys.forEach { compose.onNodeWithTag("navigation-item-$it").assertWidthIsAtLeast(48.dp).assertHeightIsAtLeast(48.dp) }
        val item = compose.onNodeWithTag("navigation-item-settings").fetchSemanticsNode().boundsInRoot
        val label = compose.onNodeWithTag("navigation-drawer-label-settings", useUnmergedTree = true).fetchSemanticsNode().boundsInRoot
        assertTrue(label.left >= item.left && label.right <= item.right && label.bottom <= item.bottom)
        compose.onNodeWithTag("navigation-floating_drawer").assertHeightIsAtLeast(96.dp)
        saveFrame("drawer-large-dark")
    }

    @Test fun interruptedDrawerSelectionSettlesOnTheLastDestinationWithoutShrinkingTargets() {
        show(NavigationBarStyle.FloatingDrawer)
        compose.mainClock.autoAdvance = false
        listOf("settings", "auth", "modules").forEach { destination ->
            compose.onNodeWithTag("navigation-item-$destination").performClick()
            compose.mainClock.advanceTimeBy(48)
            keys.forEach { compose.onNodeWithTag("navigation-item-$it").assertWidthIsAtLeast(48.dp) }
        }
        compose.mainClock.advanceTimeBy(5_000)
        compose.mainClock.autoAdvance = true
        compose.onNodeWithTag("navigation-item-modules").assertIsSelected()
        assertDrawerLabel("modules")
        assertIndicatorMatches("navigation-drawer-indicator", "modules")
    }

    @Test fun floatingStandardIndicatorFollowsRtlSelection() {
        show(NavigationBarStyle.FloatingStandard, rtl = true)
        compose.onNodeWithTag("navigation-item-settings").performClick()
        assertIndicatorMatches("navigation-standard-indicator", "settings")
        val first = compose.onNodeWithTag("navigation-item-home").fetchSemanticsNode().boundsInRoot
        val last = compose.onNodeWithTag("navigation-item-settings").fetchSemanticsNode().boundsInRoot
        assertTrue(first.left > last.left)
    }

    @Test fun floatingDrawerIndicatorFollowsRtlSelection() {
        show(NavigationBarStyle.FloatingDrawer, rtl = true)
        compose.onNodeWithTag("navigation-item-auth").performClick()
        assertIndicatorMatches("navigation-drawer-indicator", "auth")
        assertDrawerLabel("auth")
    }

    private fun assertDrawerLabel(selected: String) {
        keys.forEach { key ->
            val label = compose.onNodeWithTag("navigation-drawer-label-$key", useUnmergedTree = true)
            if (key == selected) label.assertIsDisplayed() else label.assertDoesNotExist()
        }
    }

    private fun assertFullBadgeFits() {
        val badge = compose.onNodeWithTag("navigation-badge-modules", useUnmergedTree = true).fetchSemanticsNode().boundsInRoot
        assertTrue(badge.width >= requiredBadgeWidthPx - 1f,
            "The 99+ badge needs ${requiredBadgeWidthPx}px including padding, but has only ${badge.width}px")
        val glyph = compose.onNodeWithTag("navigation-glyph-modules", useUnmergedTree = true).fetchSemanticsNode().boundsInRoot
        assertTrue(badge.left >= glyph.center.x - 1f, "The count badge must stay on the right half of its icon")
        assertTrue(badge.bottom <= glyph.center.y + 3f, "The count badge must leave the lower half of its icon recognizable")
    }

    private fun assertIndicatorMatches(tag: String, key: String) {
        compose.waitForIdle()
        val indicator = compose.onNodeWithTag(tag, useUnmergedTree = true).fetchSemanticsNode().boundsInRoot
        val item = compose.onNodeWithTag("navigation-item-$key").fetchSemanticsNode().boundsInRoot
        assertEquals(item.left, indicator.left, 1f)
        assertEquals(item.right, indicator.right, 1f)
        assertEquals(item.top, indicator.top, 1f)
        assertEquals(item.bottom, indicator.bottom, 1f)
    }

    private fun show(style: NavigationBarStyle, largeText: Boolean = false, dark: Boolean = false, rtl: Boolean = false) {
        NavigationBarStylePreferences.set(compose.activity, style)
        compose.setContent {
            var selected by remember { mutableStateOf("home") }
            val density = LocalDensity.current
            CompositionLocalProvider(LocalDensity provides Density(density.density, if (largeText) 2f else 1f),
                LocalLayoutDirection provides if (rtl) LayoutDirection.Rtl else LayoutDirection.Ltr) {
                MaterialTheme(colorScheme = if (dark) darkColorScheme() else lightColorScheme()) {
                    val badgeDensity = Density(density.density, (if (largeText) 2f else 1f).coerceAtMost(1.3f))
                    val fullBadgeText = rememberTextMeasurer().measure("99+", style = MaterialTheme.typography.labelSmall,
                        density = badgeDensity)
                    SideEffect { requiredBadgeWidthPx = fullBadgeText.size.width + with(badgeDensity) { 8.dp.toPx() } }
                    val destinations = listOf(
                        NavigationBarItem("home", "Home", Icons.Outlined.Home, selected == "home"),
                        NavigationBarItem("auth", "Authorization", Icons.Outlined.AdminPanelSettings, selected == "auth", 12),
                        NavigationBarItem("modules", "Modules", Icons.Outlined.Extension, selected == "modules", 128),
                        NavigationBarItem("settings", "Settings", Icons.Outlined.Settings, selected == "settings"),
                    )
                    FloatingNavigationScaffold(showBottomBar = true, containerColor = MaterialTheme.colorScheme.background,
                        bottomBar = { BottomNavigationBar(style, destinations, { selected = it }, windowInsets = WindowInsets(0, 0, 0, 0)) },
                        snackbarHost = {}) {
                        YukiPageScaffold(topBar = { YukiTopAppBar(title = { YukiTopBarTitle("ZySU") }) }) { padding ->
                            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(padding).padding(20.dp)
                                .padding(bottom = LocalBottomBarPadding.current)) {
                                Text("Navigation preview", style = MaterialTheme.typography.headlineSmall)
                                Text("The selected destination stays visible above page content.", Modifier.padding(top = 16.dp))
                            }
                        }
                    }
                }
            }
        }
    }

    private fun saveFrame(name: String) {
        val output = File("../../out/navigation-correction-ui/$name.png")
        output.parentFile?.mkdirs()
        val bitmap = compose.onAllNodes(isRoot()).onFirst().captureToImage().asAndroidBitmap()
        output.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }
}
