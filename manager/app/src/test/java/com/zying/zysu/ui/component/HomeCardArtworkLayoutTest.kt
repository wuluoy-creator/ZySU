package com.zying.zysu.ui.component

import android.app.Application
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color as AndroidColor
import android.graphics.Paint
import android.net.Uri
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.screen.home.*
import com.zying.zysu.ui.theme.*
import kotlinx.coroutines.runBlocking
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.shadows.ShadowDialog
import ui.screen.moreSettings.component.HomeCardArtworkSetting
import java.io.File
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "en-w375dp-h900dp-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class HomeCardArtworkLayoutTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    private val context get() = compose.activity

    @Before fun reset() = runBlocking { HomeCardArtworkStore.clear(context); Unit }

    private fun importArtwork(transparent: Boolean) = runBlocking {
        val bitmap = Bitmap.createBitmap(240, 160, Bitmap.Config.ARGB_8888)
        bitmap.eraseColor(if (transparent) AndroidColor.TRANSPARENT else AndroidColor.rgb(40, 180, 160))
        Canvas(bitmap).drawCircle(170f, 80f, 45f, Paint().apply { color = AndroidColor.rgb(235, 70, 110); isAntiAlias = true })
        val file = File(context.cacheDir, "artwork-render.png")
        file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        bitmap.recycle()
        assertEquals(ArtworkImportResult.Success, HomeCardArtworkStore.import(context, Uri.fromFile(file)))
    }

    @Test fun transparentArtworkWorksInEveryMainCardAndPreservesInstallationClick() {
        importArtwork(true)
        val style = mutableStateOf(HomeLayoutStyle.Circle)
        var installs = 0
        compose.setContent {
            MaterialTheme {
                Surface(Modifier.fillMaxSize()) {
                    Column(Modifier.padding(16.dp)) {
                        HomeMainStatusCard(style.value, status(), onInstall = { installs++ })
                    }
                }
            }
        }
        awaitImage()
        HomeLayoutStyle.entries.forEach { layout ->
            compose.runOnIdle { style.value = layout }
            compose.onNodeWithTag("service-main-value").assertIsDisplayed()
            compose.onNodeWithTag("service-main-status", useUnmergedTree = true).assertIsDisplayed()
            compose.onNodeWithTag("home-status-card").performTouchInput { click(bottomCenter - androidx.compose.ui.geometry.Offset(0f, 3f)) }
            capture("transparent-${layout.id}")
        }
        assertEquals(3, installs)
    }

    @Test fun zoomChangesOnlyArtworkAndSurvivesReloadAtLargeText() {
        importArtwork(false)
        compose.setContent {
            val density = LocalDensity.current
            CompositionLocalProvider(LocalDensity provides Density(density.density, 2f)) {
                MaterialTheme(colorScheme = darkColorScheme()) {
                    Surface(Modifier.fillMaxSize()) {
                        Column(Modifier.padding(16.dp)) { HomeMainStatusCard(HomeLayoutStyle.Circle, status()) }
                    }
                }
            }
        }
        awaitImage()
        val originalBounds = compose.onNodeWithTag("home-status-card").getUnclippedBoundsInRoot()
        val before = compose.onNodeWithTag("home-status-card").captureToImage().asAndroidBitmap()
        compose.runOnIdle { HomeCardArtworkStore.setScale(context, 2f); HomeCardArtworkStore.setCrop(context, true) }
        compose.waitForIdle()
        val after = compose.onNodeWithTag("home-status-card").captureToImage().asAndroidBitmap()
        assertEquals(originalBounds, compose.onNodeWithTag("home-status-card").getUnclippedBoundsInRoot())
        assertFalse(before.sameAs(after), "Changing scale must actually change the drawn image")
        assertEquals(2f, HomeCardArtworkStore.read(context).scale)
        compose.onNodeWithText("Working").assertIsDisplayed()
        capture("photo-zoom-dark-large")
    }

    @Test fun settingsPreviewCanScaleSetOverlayResetAndRemoveImportedArtwork() {
        importArtwork(true)
        compose.setContent { MaterialTheme { HomeCardArtworkSetting() } }
        compose.onNodeWithText(context.getString(R.string.home_card_artwork_title)).performClick()
        awaitImage()
        compose.onNodeWithTag("home-artwork-scale").performScrollTo().performSemanticsAction(SemanticsActions.SetProgress) { it(1.5f) }
        compose.waitForIdle()
        assertEquals(1.5f, HomeCardArtworkStore.read(context).scale)
        listOf(0f, 1f, 0.37f).forEach { opacity ->
            compose.onNodeWithTag("home-artwork-overlay").performScrollTo()
                .performSemanticsAction(SemanticsActions.SetProgress) { it(opacity) }
            compose.waitForIdle()
            assertEquals(opacity, HomeCardArtworkStore.read(context).overlayOpacity)
        }
        capture("overlay-settings")
        compose.onNodeWithText(context.getString(R.string.home_card_artwork_reset)).performScrollTo().performClick()
        compose.waitForIdle()
        assertEquals(1f, HomeCardArtworkStore.read(context).scale)
        assertEquals(0.37f, HomeCardArtworkStore.read(context).overlayOpacity)
        compose.onNodeWithText(context.getString(R.string.home_card_artwork_remove)).performScrollTo().performClick()
        compose.waitUntil(5_000) { HomeCardArtworkStore.read(context).file == null }
        compose.onNodeWithTag("home-card-artwork", useUnmergedTree = true).assertDoesNotExist()
    }

    @Test fun overlayActuallyRendersTransparentIntermediateAndOpaqueInBothThemes() {
        importArtwork(false)
        val opacity = mutableFloatStateOf(0f)
        val dark = mutableStateOf(false)
        val artwork = HomeCardArtworkStore.read(context)
        compose.setContent {
            MaterialTheme(colorScheme = if (dark.value) darkColorScheme() else lightColorScheme()) {
                Box(Modifier.size(240.dp, 160.dp)) {
                    HomeCardArtworkLayer(artwork.copy(overlayOpacity = opacity.floatValue), Modifier.fillMaxSize())
                }
            }
        }
        awaitImage()
        val bounds = compose.onNodeWithTag("home-card-artwork").getUnclippedBoundsInRoot()
        listOf(false, true).forEach { isDark ->
            compose.runOnIdle { dark.value = isDark }
            val scheme = if (isDark) darkColorScheme() else lightColorScheme()
            val pixels = listOf(0f, 0.5f, 1f).map { value ->
                compose.runOnIdle { opacity.floatValue = value }
                compose.waitForIdle()
                assertEquals(bounds, compose.onNodeWithTag("home-card-artwork").getUnclippedBoundsInRoot())
                capture("overlay-${if (isDark) "dark" else "light"}-${(value * 100).toInt()}")
                compose.onNodeWithTag("home-card-artwork").captureToImage().asAndroidBitmap().getPixel(20, 20)
            }
            assertEquals(AndroidColor.rgb(40, 180, 160), pixels[0], "0% must leave the image unaltered")
            assertEquals(scheme.surfaceContainerLow.toArgb(), pixels[2], "100% must fully cover the image")
            assertNotEquals(pixels[0], pixels[1])
            assertNotEquals(pixels[2], pixels[1])
        }
    }

    @Test fun overlayControlRemainsReachableInDarkSettingsAtDoubleTextSize() {
        importArtwork(true)
        compose.setContent {
            MaterialTheme(colorScheme = darkColorScheme(), typography = Typography.withAppearance(2f, "system")) {
                HomeCardArtworkSetting()
            }
        }
        compose.onNodeWithText(context.getString(R.string.home_card_artwork_title)).performClick()
        awaitImage()
        compose.onNodeWithTag("home-artwork-overlay").performScrollTo().assertIsDisplayed()
            .performSemanticsAction(SemanticsActions.SetProgress) { it(0.65f) }
        compose.waitForIdle()
        assertEquals(0.65f, HomeCardArtworkStore.read(context).overlayOpacity)
        capture("overlay-settings-dark-large")
        compose.onNodeWithText(context.getString(R.string.home_card_artwork_close)).assertIsDisplayed().performClick()
    }

    private fun status() = HomeStatusPresentation("Working", true,
        version = HomeFact("version", "Version", "v3.6.2-build", "v3.6.2"))

    private fun awaitImage() {
        compose.waitUntil(10_000) { compose.onAllNodesWithTag("home-card-artwork-loaded", useUnmergedTree = true).fetchSemanticsNodes().isNotEmpty() }
    }

    private fun capture(name: String) {
        val file = File("../../out/grid-overlay-artwork-ui/$name.png")
        file.parentFile?.mkdirs()
        val frame = if (compose.onAllNodes(isDialog()).fetchSemanticsNodes().isNotEmpty()) {
            compose.runOnIdle {
                val decor = checkNotNull(ShadowDialog.getLatestDialog().window).decorView
                Bitmap.createBitmap(decor.width, decor.height, Bitmap.Config.ARGB_8888).also { decor.draw(Canvas(it)) }
            }
        } else compose.onRoot().captureToImage().asAndroidBitmap()
        frame.let { bitmap ->
            file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        }
    }
}
