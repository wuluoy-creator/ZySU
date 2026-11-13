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
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.integrity.KsudIntegrityStatus
import com.zying.zysu.ui.activity.component.*
import com.zying.zysu.ui.screen.home.*
import com.zying.zysu.ui.sumh.SUMHStatusSummaryCard
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter
import com.zying.zysu.ui.theme.ExpressiveShapes
import com.zying.zysu.ui.theme.ThemeColors
import com.zying.zysu.ui.theme.Typography
import com.zying.zysu.ui.theme.utilitySurfaces
import com.zying.zysu.ui.screen.TopBar
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.shadows.ShadowDialog
import java.io.File
import kotlin.test.assertEquals
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "zh-rCN-w375dp-h900dp-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@OptIn(ExperimentalMaterial3Api::class)
class HomeNavigationLayoutTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    @Test fun circleWithGlass() = verify(HomeLayoutStyle.Circle, NavigationBarStyle.Glass)
    @Test fun gridWithDrawerFloating() = verify(HomeLayoutStyle.Grid, NavigationBarStyle.FloatingDrawer)
    @Test fun focusWithDrawerFloating() = verify(HomeLayoutStyle.Focus, NavigationBarStyle.FloatingDrawer)
    @Test fun retainedGlassNavigation() = verify(HomeLayoutStyle.Grid, NavigationBarStyle.Glass)
    @Test fun largeTextDarkNavigation() = verify(HomeLayoutStyle.Focus, NavigationBarStyle.FloatingDrawer, dark = true, fontScale = 2f)
    @Test fun gridLargeTextStacksServices() = verify(HomeLayoutStyle.Grid, NavigationBarStyle.Glass, fontScale = 2f)

    @Test
    @Config(qualifiers = "zh-rCN-w320dp-h900dp-mdpi")
    fun gridNarrowPhoneStacksServicesWithoutClipping() = verify(HomeLayoutStyle.Grid, NavigationBarStyle.FloatingDrawer, suffix = "-narrow")

    @Test
    @Config(qualifiers = "zh-rCN-w800dp-h450dp-land-mdpi")
    fun focusLandscapeBoard() = verify(HomeLayoutStyle.Focus, NavigationBarStyle.FloatingDrawer, suffix = "-landscape")

    @Test
    @Config(qualifiers = "zh-rCN-w800dp-h450dp-land-mdpi")
    fun circleKeepsCoreFullWidth() = verify(HomeLayoutStyle.Circle,
        NavigationBarStyle.Glass, suffix = "-landscape")

    @Test fun switchingLayoutsRearrangesActualServiceCards() = verifyDistinctUpperLayouts(1f)
    @Test fun largeTextKeepsThreeLayoutsReadable() = verifyDistinctUpperLayouts(2f)

    private fun verifyDistinctUpperLayouts(fontScale: Float) {
        val selected = mutableStateOf(HomeLayoutStyle.Circle)
        val state = HomeLayoutState(status = HomeStatusPresentation(
            title = "Working", installed = true, version = HomeFact("root", "Root", "v3.6.2")))
        compose.setContent {
            val density = LocalDensity.current
            CompositionLocalProvider(LocalDensity provides Density(density.density, fontScale)) {
                MaterialTheme {
                    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(20.dp)) {
                        HomeLayoutContent(selected.value, state, daemon = { KsudFixture(it) }, kernel = { SumhFixture(it) })
                    }
                }
            }
        }
        val paintedLayouts = mutableSetOf<String>()
        val servicePlacements = mutableSetOf<String>()
        HomeLayoutStyle.entries.forEach { style ->
            compose.runOnIdle { selected.value = style }
            val bitmap = compose.onNodeWithTag("home-upper-${style.id}").captureToImage().asAndroidBitmap()
            val pixels = IntArray(bitmap.width * bitmap.height)
            bitmap.getPixels(pixels, 0, bitmap.width, 0, 0, bitmap.width, bitmap.height)
            paintedLayouts += "${bitmap.width}:${bitmap.height}:${pixels.contentHashCode()}"
            val daemon = compose.onNodeWithTag("fixture-ksud").getUnclippedBoundsInRoot()
            val sumh = compose.onNodeWithTag("fixture-sumh").getUnclippedBoundsInRoot()
            servicePlacements += "$daemon:$sumh"
            compose.onNodeWithTag("home-install-action").assertDoesNotExist()
            saveFrame("services-${style.id}-${fontScale}x")
        }
        assertEquals(3, paintedLayouts.size, "Every layout must retain a distinct upper arrangement")
        assertTrue(servicePlacements.size >= 3, "Service cards must move with the layout, not remain in one fixed section")
    }

    @Test fun installedCardOpensInstallationWithoutAnExtraButtonAndRequiredActionsRemain() {
        val selected = mutableStateOf(HomeLayoutStyle.Circle)
        val installed = mutableStateOf(true)
        var installs = 0
        var authentications = 0
        compose.setContent {
            MaterialTheme {
                Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(20.dp)) {
                    HomeLayoutContent(selected.value, HomeLayoutState(
                        status = HomeStatusPresentation(title = "Status", installed = installed.value,
                            needsAuthentication = !installed.value)),
                        onInstall = { installs++ }, onAuthenticate = { authentications++ })
                }
            }
        }
        HomeLayoutStyle.entries.forEach { style ->
            compose.runOnIdle { selected.value = style; installed.value = true }
            compose.onNodeWithTag("home-install-action").assertDoesNotExist()
            compose.onNodeWithTag("home-authenticate-action").assertDoesNotExist()
            compose.onNodeWithTag("home-status-card").performSemanticsAction(SemanticsActions.OnClick) { it() }
            compose.runOnIdle { installed.value = false }
            compose.onNodeWithTag("home-install-action").performScrollTo().performClick()
            compose.onNodeWithTag("home-authenticate-action").performScrollTo().performClick()
        }
        assertEquals(6, installs)
        assertEquals(3, authentications)
    }

    @Test fun mainAndKsudShareHeaderVersionAndStatusRowRhythm() {
        compose.setContent {
            MaterialTheme {
                Row(Modifier.fillMaxWidth().padding(20.dp).height(IntrinsicSize.Min), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    HomeMainStatusCard(HomeLayoutStyle.Grid,
                        HomeStatusPresentation("运行正常", installed = true, version = HomeFact("root", "Root", "v3.6.2")),
                        modifier = Modifier.weight(1f).fillMaxHeight())
                    KsudFixture(Modifier.weight(1f).fillMaxHeight())
                }
            }
        }
        listOf("header", "value", "status").forEach { row ->
            val main = compose.onNodeWithTag("service-main-$row", useUnmergedTree = true).getUnclippedBoundsInRoot()
            val ksud = compose.onNodeWithTag("service-ksud-$row", useUnmergedTree = true).getUnclippedBoundsInRoot()
            assertEquals(ksud.top.value, main.top.value, 1f)
            assertEquals(ksud.bottom.value, main.bottom.value, 1f)
        }
        val card = compose.onNodeWithTag("home-status-card").getUnclippedBoundsInRoot()
        val header = compose.onNodeWithTag("service-main-header", useUnmergedTree = true).getUnclippedBoundsInRoot()
        assertEquals(16f, header.top.value - card.top.value, 1f)
        compose.onNodeWithTag("service-main-value").assertHeightIsAtLeast(48.dp)
        saveFrame("main-service-row-rhythm")
    }

    @Test fun safeModeMainCardKeepsIndependentVersionDetailsAtLargeText() {
        val selected = mutableStateOf(HomeLayoutStyle.Circle)
        val fullVersion = "v3.6.2-safe-mode-build-with-long-diagnostic-details"
        var installs = 0
        compose.setContent {
            val density = LocalDensity.current
            CompositionLocalProvider(LocalDensity provides Density(density.density, 2f)) {
                MaterialTheme(colorScheme = darkColorScheme()) {
                    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(20.dp)) {
                        HomeLayoutContent(selected.value, HomeLayoutState(status = HomeStatusPresentation(
                            title = "安全模式", installed = true, safeMode = true,
                            version = HomeFact("root", "Root 版本", fullVersion, "v3.6.2"))),
                            onInstall = { installs++ })
                    }
                }
            }
        }
        HomeLayoutStyle.entries.forEachIndexed { index, style ->
            compose.runOnIdle { selected.value = style }
            compose.onNodeWithTag("home-install-action").assertDoesNotExist()
            compose.onNodeWithTag("service-main-header", useUnmergedTree = true).performTouchInput { click() }
            assertEquals(index + 1, installs)
            compose.onNodeWithTag("service-main-value").performScrollTo().performClick()
            compose.onNodeWithText(fullVersion).assertIsDisplayed()
            assertEquals(index + 1, installs, "Version details must not bubble to the installation action")
            saveFrame("safe-${style.id}-large-detail")
            compose.onNodeWithText(compose.activity.getString(android.R.string.ok)).performClick()
            val card = compose.onNodeWithTag("home-status-card").fetchSemanticsNode().boundsInRoot
            listOf("header", "value", "status").forEach { row ->
                val bounds = compose.onNodeWithTag("service-main-$row", useUnmergedTree = true).fetchSemanticsNode().boundsInRoot
                assertTrue(bounds.left >= card.left && bounds.right <= card.right && bounds.bottom <= card.bottom)
            }
        }
    }


    private fun verify(style: HomeLayoutStyle, navigation: NavigationBarStyle, dark: Boolean = false,
        fontScale: Float = 1f, suffix: String = "") {
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
        )
        var installs = 0
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
                        YukiPageScaffold(topBar = { TopBar(showReboot = false) }) { padding ->
                            Column(Modifier.fillMaxSize().testTag("home-test-scroll").verticalScroll(rememberScrollState())
                                .padding(padding).padding(horizontal = 20.dp)
                                .padding(top = 16.dp, bottom = LocalBottomBarPadding.current + 16.dp)) {
                                HomeLayoutContent(style, state, onInstall = { installs++ },
                                    daemon = { KsudFixture(it) }, kernel = { SumhFixture(it) })
                            }
                        }
                    }
                }
            }
        }
        val frame = "${style.id}-${navigation.preferenceValue}${if (fontScale > 1f) "-large-dark" else ""}$suffix"
        compose.onNodeWithTag("home-layout-${style.id}").assertExists()
        compose.onNodeWithTag("navigation-item-home").assertIsSelected().assertIsDisplayed()
        compose.onNodeWithTag("home-install-action").assertDoesNotExist()
        compose.onNodeWithTag("home-install-toolbar").assertDoesNotExist()
        listOf("home-focus-device", "home-focus-storage", "home-stats-monitor", "home-stats-counts",
            "home-count-superuser", "home-count-modules").forEach { compose.onNodeWithTag(it).assertDoesNotExist() }
        saveFrame(frame)
        // Real service cards are now part of the chosen upper layout, with core facts after it.
        val upper = compose.onNodeWithTag("home-upper-${style.id}").getUnclippedBoundsInRoot()
        val core = compose.onNodeWithTag("home-core-facts").getUnclippedBoundsInRoot()
        assertTrue(core.top >= upper.bottom, "Core facts must follow the upper service layout")
        listOf("fixture-ksud", "fixture-sumh").forEach { tag ->
            val card = compose.onNodeWithTag(tag).getUnclippedBoundsInRoot()
            assertTrue(card.top >= upper.top && card.bottom <= upper.bottom, "$tag must be inside the rearranged upper region")
            assertTrue(card.bottom <= core.top, "$tag must appear before core facts")
        }
        if (style == HomeLayoutStyle.Grid) verifyGridGeometry(stack = fontScale >= 1.3f || suffix.contains("narrow"))
        // Each fact stays in the shared core panel exactly once.
        facts.forEach { compose.onAllNodesWithText(it.label, useUnmergedTree = true).assertCountEquals(1) }
        compose.onNodeWithText(kernel).assertDoesNotExist()
        scrollAboveDock(compose.onNodeWithTag("service-main-header", useUnmergedTree = true)).performTouchInput { click() }
        assertEquals(1, installs)
        val kernelFact = scrollAboveDock(compose.onNodeWithTag("home-fact-kernel"))
        if (suffix.contains("landscape")) kernelFact.assertWidthIsAtLeast(600.dp)
        saveFrame("$frame-core")
        kernelFact.performClick()
        compose.onNodeWithText(kernel).assertIsDisplayed()
        saveFrame("$frame-detail")
    }

    /** The real production Grid must retain the user's left-one/right-two arrangement. */
    private fun verifyGridGeometry(stack: Boolean) {
        val main = compose.onNodeWithTag("home-status-card").getUnclippedBoundsInRoot()
        val body = compose.onNodeWithTag("home-status-grid", useUnmergedTree = true).getUnclippedBoundsInRoot()
        val ksud = compose.onNodeWithTag("fixture-ksud").getUnclippedBoundsInRoot()
        val sumh = compose.onNodeWithTag("fixture-sumh").getUnclippedBoundsInRoot()
        if (stack) {
            assertTrue(ksud.top >= main.bottom && sumh.top >= ksud.bottom, "Narrow/large text Grid must stack its cards")
            assertEquals(main.left.value, ksud.left.value, 1f)
            assertEquals(main.right.value, ksud.right.value, 1f)
            assertEquals(main.left.value, sumh.left.value, 1f)
            assertEquals(main.right.value, sumh.right.value, 1f)
        } else {
            assertTrue(main.right < ksud.left && main.right < sumh.left, "Main card must remain left of BOTH services")
            assertEquals(main.top.value, ksud.top.value, 1f)
            assertEquals(main.bottom.value, sumh.bottom.value, 1f)
            assertEquals(ksud.left.value, sumh.left.value, 1f)
            assertEquals(ksud.right.value, sumh.right.value, 1f)
            assertEquals(12f, (sumh.top - ksud.bottom).value, 1f)
            assertEquals((ksud.bottom - ksud.top).value, (sumh.bottom - sumh.top).value, 1f)
            assertEquals((ksud.bottom - ksud.top + 12.dp + sumh.bottom - sumh.top).value, (main.bottom - main.top).value, 1f)
            assertEquals(0.9f / 1.1f, (main.right - main.left).value / (ksud.right - ksud.left).value, 0.02f)
            // The artwork is matchParentSize in this host, so it must cover the whole tall tile.
            assertEquals(main.top.value, body.top.value, 1f)
            assertEquals(main.bottom.value, body.bottom.value, 1f)
            listOf("header", "value", "status").forEach { row ->
                val ksudRow = compose.onNodeWithTag("service-ksud-$row", useUnmergedTree = true).getUnclippedBoundsInRoot()
                val sumhRow = compose.onNodeWithTag("service-sumh-$row", useUnmergedTree = true).getUnclippedBoundsInRoot()
                assertEquals((ksudRow.top - ksud.top).value, (sumhRow.top - sumh.top).value, 1f)
                assertEquals((ksudRow.bottom - ksudRow.top).value, (sumhRow.bottom - sumhRow.top).value, 1f)
            }
        }
        listOf("header", "value", "status").forEach { row ->
            val bounds = compose.onNodeWithTag("service-main-$row", useUnmergedTree = true).getUnclippedBoundsInRoot()
            assertTrue(bounds.left >= main.left && bounds.right <= main.right && bounds.bottom <= main.bottom)
        }
    }

    private fun scrollAboveDock(node: SemanticsNodeInteraction): SemanticsNodeInteraction {
        node.performScrollTo()
        val scroll = compose.onNodeWithTag("home-test-scroll")
        val viewport = scroll.fetchSemanticsNode().boundsInRoot
        val target = node.fetchSemanticsNode().boundsInRoot
        // Floating controls overlay the full page viewport. Position the real pointer target in
        // the clear viewport, not at the minimal scroll position beneath the floating dock.
        val extraScroll = (target.center.y - viewport.center.y).coerceAtLeast(0f)
        if (extraScroll > 0f) {
            scroll.performSemanticsAction(SemanticsActions.ScrollBy) { it(0f, extraScroll) }
        }
        compose.waitForIdle()
        assertTrue(node.fetchSemanticsNode().boundsInRoot.bottom <
            compose.onNodeWithTag("navigation-item-home").fetchSemanticsNode().boundsInRoot.top)
        return node.assertIsDisplayed()
    }

    @Composable
    private fun KsudFixture(modifier: Modifier) {
        KsudStatusCard(
            bundledVersion = "v3.6.2", installedVersion = "v3.6.2",
            integrity = KsudIntegrityStatus.MATCH, loading = false, syncing = false,
            syncResult = null, syncUnavailableReason = null, onSync = {},
            modifier = modifier.testTag("fixture-ksud"),
        )
    }

    @Composable
    private fun SumhFixture(modifier: Modifier) {
        SUMHStatusSummaryCard(
            status = SUMHUiAdapter.SUMHStatus.AVAILABLE, version = "13",
            systemInfo = SUMHUiAdapter.SystemInfo(
                kernel = "6.12.0", mountBase = "/data/adb",
                activeMounts = emptyList(), sumhModuleIds = emptyList(),
                sumhMismatch = false, mismatchMessage = null, viewsEnabled = true,
            ), onRefresh = {}, modifier = modifier.testTag("fixture-sumh"),
        )
    }

    private fun saveFrame(name: String) {
        val output = File("../../out/grid-overlay-layout-ui/$name.png")
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
