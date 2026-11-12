package com.zying.zysu.ui.component

import android.app.Application
import android.graphics.Bitmap
import android.graphics.Canvas
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.*
import androidx.compose.material3.*
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.activity.component.*
import com.zying.zysu.ui.screen.home.*
import com.zying.zysu.ui.theme.ExpressiveShapes
import com.zying.zysu.ui.theme.ThemeColors
import com.zying.zysu.ui.theme.Typography
import com.zying.zysu.ui.theme.utilitySurfaces
import com.zying.zysu.ui.viewmodel.HomeCapacity
import com.zying.zysu.ui.viewmodel.HomeDeviceMetrics
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.shadows.ShadowDialog
import java.io.File
import kotlin.test.assertEquals

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "zh-rCN-w375dp-h900dp-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@OptIn(ExperimentalMaterial3Api::class)
class HomeNavigationLayoutTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    @Test fun circleWithGlass() = verify(HomeLayoutStyle.Circle, NavigationBarStyle.Glass)
    @Test fun gridWithMaterial() = verify(HomeLayoutStyle.Grid, NavigationBarStyle.Material)
    @Test fun focusWithMaterial() = verify(HomeLayoutStyle.Focus, NavigationBarStyle.Material)
    @Test fun dashboardWithGlass() = verify(HomeLayoutStyle.Dashboard, NavigationBarStyle.Glass)
    @Test fun statisticsWithMaterial() = verify(HomeLayoutStyle.Stats, NavigationBarStyle.Material)
    @Test fun largeTextDarkNavigation() = verify(HomeLayoutStyle.Stats, NavigationBarStyle.Material, dark = true, fontScale = 2f)

    @Test
    @Config(qualifiers = "zh-rCN-w800dp-h450dp-land-mdpi")
    fun focusLandscapeBoard() = verify(HomeLayoutStyle.Focus, NavigationBarStyle.Material, suffix = "-landscape")

    @Test
    @Config(qualifiers = "zh-rCN-w800dp-h450dp-land-mdpi")
    fun statisticsWithoutOptionalPanelsUsesFullWidth() = verify(HomeLayoutStyle.Stats,
        NavigationBarStyle.Material, suffix = "-simple-landscape", optionalPanels = false)

    private fun verify(style: HomeLayoutStyle, navigation: NavigationBarStyle, dark: Boolean = false,
        fontScale: Float = 1f, suffix: String = "", optionalPanels: Boolean = true) {
        val kernel = "6.12.0-android16-2026-11-device-very-long-build-suffix\nrelease@builder"
        val facts = listOf(
            HomeFact("device", "设备", "Pixel 9 Pro"),
            HomeFact("android", "Android", "16 / API 36"),
            HomeFact("kernel", "内核", kernel, "6.12.0"),
            HomeFact("manager", "管理器", "3.6.2 (6908)"),
            HomeFact("selinux", "SELinux", "Enforcing"),
        )
        val state = HomeLayoutState(
            status = HomeStatusPresentation(title = "运行正常", installed = true,
                version = HomeFact("root", "Root 版本", "v3.6.2-custom-long-version-build-6908-uapi-13", "v3.6.2")),
            facts = facts,
            buildFacts = listOf(HomeFact("fingerprint", "构建指纹", "google/caiman/caiman:16/BP4A.260901.001/12345678:user/release-keys")),
            superuserCount = 12, moduleCount = 9,
            showCounts = optionalPanels, showMetrics = optionalPanels,
            metrics = HomeDeviceMetrics(
                batteryPercent = 76, batteryTemperatureC = 32.5f, charging = true,
                cpuTemperatureC = null, cpuFrequencyKHz = 2_400_000,
                memory = HomeCapacity.fromAvailable(8_000_000_000, 3_000_000_000),
                storage = HomeCapacity.fromAvailable(256_000_000_000, 90_000_000_000),
                uptimeMillis = 185_400_000,
                memoryHistory = listOf(.61f, .64f, .62f, .63f, .625f),
                cpuTemperatureHistory = listOf(null, null, null, null, null)),
        )
        var moduleClicks = 0
        val destinations = listOf(
            NavigationBarItem("home", "首页", Icons.Outlined.Home, true),
            NavigationBarItem("auth", "授权", Icons.Outlined.AdminPanelSettings, false, 12),
            NavigationBarItem("modules", "模块", Icons.Outlined.Extension, false, 9),
            NavigationBarItem("settings", "设置", Icons.Outlined.Settings, false),
        )
        NavigationBarStylePreferences.set(compose.activity, navigation)
        compose.setContent {
            val density = LocalDensity.current
            CompositionLocalProvider(LocalDensity provides Density(density.density, fontScale)) {
                val brand = ThemeColors.Default
                val scheme = (if (dark) darkColorScheme() else lightColorScheme()).copy(
                    primary = if (dark) brand.primaryDark else brand.primaryLight,
                    onPrimary = if (dark) brand.onPrimaryDark else brand.onPrimaryLight,
                    primaryContainer = if (dark) brand.primaryContainerDark else brand.primaryContainerLight,
                    onPrimaryContainer = if (dark) brand.onPrimaryContainerDark else brand.onPrimaryContainerLight,
                ).utilitySurfaces(dark, false)
                MaterialTheme(colorScheme = scheme, typography = Typography, shapes = ExpressiveShapes) {
                    FloatingNavigationScaffold(showBottomBar = true, containerColor = scheme.background,
                        bottomBar = { BottomNavigationBar(navigation, destinations, {}) }, snackbarHost = {}) {
                        YukiPageScaffold(topBar = { YukiTopAppBar(title = { YukiTopBarTitle("ZySU") }) }) { padding ->
                            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())
                                .padding(padding).padding(horizontal = 20.dp)
                                .padding(top = 16.dp, bottom = LocalBottomBarPadding.current + 16.dp)) {
                                HomeLayoutContent(style, state, onModules = { moduleClicks++ })
                            }
                        }
                    }
                }
            }
        }
        val frame = "${style.id}-${navigation.preferenceValue}${if (fontScale > 1f) "-large-dark" else ""}$suffix"
        compose.onNodeWithTag("home-layout-${style.id}").assertExists()
        compose.onNodeWithText("首页").assertIsSelected().assertIsDisplayed()
        saveFrame(frame)
        // Each fact has one visible owner across the hero and facts panel.
        facts.forEach { compose.onAllNodesWithText(it.label, useUnmergedTree = true).assertCountEquals(1) }
        compose.onNodeWithText(kernel).assertDoesNotExist()
        if (optionalPanels) {
            compose.onNodeWithText(compose.activity.getString(R.string.home_installed_modules)).performScrollTo().performClick()
            assertEquals(1, moduleClicks)
        }
        val kernelFact = compose.onNodeWithText("内核").performScrollTo()
        if (!optionalPanels) kernelFact.assertWidthIsAtLeast(600.dp)
        kernelFact.performClick()
        compose.onNodeWithText(kernel).assertIsDisplayed()
        saveFrame("$frame-detail")
    }

    private fun saveFrame(name: String) {
        val output = File("../../out/home-nav-ui/$name.png")
        output.parentFile?.mkdirs()
        val frame = if (name.endsWith("-detail")) {
            compose.onNode(isDialog()).assertIsDisplayed()
            // Robolectric's PixelCopy-based capture can crop the Activity surface for a
            // dialog semantics root. Draw the actual Dialog decor view on its UI thread.
            compose.runOnIdle {
                val dialog = checkNotNull(ShadowDialog.getLatestDialog())
                check(dialog.isShowing)
                val decor = checkNotNull(dialog.window).decorView
                check(decor.width > 0 && decor.height > 0)
                Bitmap.createBitmap(decor.width, decor.height, Bitmap.Config.ARGB_8888).also {
                    decor.draw(Canvas(it))
                }
            }
        } else compose.onAllNodes(isRoot()).onFirst().captureToImage().asAndroidBitmap()
        output.outputStream().use { frame.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }
}
