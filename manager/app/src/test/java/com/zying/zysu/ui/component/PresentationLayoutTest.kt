package com.zying.zysu.ui.component

import android.app.Application
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.screen.WorkspaceTabs
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
import ui.screen.moreSettings.component.AppearancePreview
import java.io.File
import kotlin.test.assertEquals

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "w375dp-h900dp-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class PresentationLayoutTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    @Test fun lightPhone() = verify("phone-light", dark = false)
    @Test fun darkPhone() = verify("phone-dark", dark = true)
    @Test fun largeTextKeepsFiltersReachable() = verify("phone-large-text", dark = false, fontScale = 2f)

    @Test
    @Config(qualifiers = "w800dp-h375dp-land-mdpi")
    fun landscapeKeepsContentScrollable() = verify("landscape", dark = true)

    private fun verify(name: String, dark: Boolean, fontScale: Float = 1f) {
        val selection = mutableIntStateOf(0)
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
                    Surface(Modifier.fillMaxSize()) {
                        Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
                            verticalArrangement = Arrangement.spacedBy(16.dp)) {
                            Text("ZySU", style = MaterialTheme.typography.headlineMedium)
                            HomeDeviceOverview(HomeDeviceMetrics(
                                batteryPercent = 76, batteryTemperatureC = 32.5f, charging = true,
                                cpuTemperatureC = null, cpuFrequencyKHz = 2_400_000,
                                memory = HomeCapacity.fromAvailable(8_000_000_000, 3_000_000_000),
                                storage = HomeCapacity.fromAvailable(256_000_000_000, 90_000_000_000),
                                uptimeMillis = 185_400_000,
                            ))
                            ListOverview("Installed modules", "12 total · 9 enabled · 2 pending restart",
                                listOf("All", "Enabled", "Disabled", "Pending restart"), selection.intValue,
                                { selection.intValue = it })
                            AppearancePreview()
                            WorkspaceTabs(listOf("Mount", "Isolation settings", "Active rules", "Kstat", "Maps"), 0, {})
                        }
                    }
                }
            }
        }
        compose.onNodeWithText(compose.activity.getString(R.string.home_device_overview)).assertIsDisplayed()
        saveFrame(name)
        compose.onNodeWithText("Pending restart").performScrollTo().performClick()
        assertEquals(3, selection.intValue)
        compose.onNodeWithText("Pending restart").assertIsSelected()
        compose.onNodeWithText(compose.activity.getString(R.string.appearance_preview_heading)).performScrollTo().assertIsDisplayed()
        saveFrame("$name-appearance")
    }

    private fun saveFrame(name: String) {
        // Local visual artifacts complement behavior assertions; CI also exercises native rendering.
        val output = File("../../out/fina-ui/$name.png")
        output.parentFile?.mkdirs()
        val image = compose.onRoot().captureToImage().asAndroidBitmap()
        output.outputStream().use { image.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }
}
