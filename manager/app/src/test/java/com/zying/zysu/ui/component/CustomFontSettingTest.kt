package com.zying.zysu.ui.component

import android.app.Application
import android.graphics.Bitmap
import android.net.Uri
import androidx.activity.ComponentActivity
import androidx.activity.compose.LocalActivityResultRegistryOwner
import androidx.activity.result.ActivityResultRegistry
import androidx.activity.result.ActivityResultRegistryOwner
import androidx.activity.result.contract.ActivityResultContract
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import androidx.core.app.ActivityOptionsCompat
import com.zying.zysu.ui.theme.*
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import ui.screen.moreSettings.component.CustomFontSetting
import java.io.File
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "en-w320dp-h900dp-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class CustomFontSettingTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    private val context get() = compose.activity
    private var pickerResult: Uri? = null
    private var renderedFamily: FontFamily? = null
    private val registry = object : ActivityResultRegistry() {
        override fun <I, O> onLaunch(requestCode: Int, contract: ActivityResultContract<I, O>,
            input: I, options: ActivityOptionsCompat?) {
            dispatchResult(requestCode, pickerResult)
        }
    }
    private val owner = object : ActivityResultRegistryOwner {
        override val activityResultRegistry = registry
    }
    private fun font() = checkNotNull(javaClass.getResourceAsStream("/fixtures/custom-font/LiberationSans-Regular.ttf"))
    private fun selectedFile() = context.getSharedPreferences("appearance", 0).getString("custom_font_file", null)

    @Before fun reset() = AppearancePreferences.reset(context)

    @Test fun pickerImportsAndReplacesFontWithImmediateTypography() {
        show()
        assertEquals(FontFamily.Default, renderedFamily)
        val file = File(context.cacheDir, "Imported.ttf")
        font().use { input -> file.outputStream().use { input.copyTo(it) } }
        pickerResult = Uri.fromFile(file)
        compose.onNodeWithTag("custom-font-import").performClick()
        compose.waitUntil(10_000) { AppearancePreferences.fontFamily == "custom" ||
            compose.onAllNodesWithTag("custom-font-error").fetchSemanticsNodes().isNotEmpty() }
        compose.onNodeWithTag("custom-font-error").assertDoesNotExist()
        compose.onNodeWithTag("custom-font-select").assertIsSelected()
        assertEquals(AppearancePreferences.customFontFamily, renderedFamily)
        val first = selectedFile()
        val firstFamily = renderedFamily
        compose.onNodeWithTag("custom-font-import").performClick()
        compose.waitUntil(10_000) { selectedFile() != first && renderedFamily !== firstFamily }
        compose.waitForIdle()
        assertNotSame(firstFamily, renderedFamily)
        assertEquals(AppearancePreferences.customFontFamily, renderedFamily)
        capture("imported-font")
    }

    @Test fun cancelledPickerAndInvalidImportPreserveValidSelection() {
        AppearancePreferences.applyCustomFont(CustomFontStore.import(context, "Existing.ttf", font()))
        val original = selectedFile()
        show()
        pickerResult = null
        compose.onNodeWithTag("custom-font-import").performClick()
        compose.waitForIdle()
        assertEquals(original, selectedFile())
        compose.onNodeWithTag("custom-font-select").assertIsSelected()
        val invalid = File(context.cacheDir, "invalid.otf").apply { writeText("not a font") }
        pickerResult = Uri.fromFile(invalid)
        compose.onNodeWithTag("custom-font-import").performClick()
        compose.waitUntil(10_000) { compose.onAllNodesWithTag("custom-font-error").fetchSemanticsNodes().isNotEmpty() }
        assertEquals(original, selectedFile())
        assertEquals("Existing.ttf", AppearancePreferences.customFontName)
        assertEquals("custom", AppearancePreferences.fontFamily)
        compose.onNodeWithTag("custom-font-error").assertIsDisplayed()
    }

    @Test fun largeTextControlsCanRevertReselectAndRemoveFont() {
        AppearancePreferences.applyCustomFont(CustomFontStore.import(context, "Readable.ttf", font()))
        show(large = true)
        compose.onNodeWithTag("custom-font-revert").performScrollTo().performClick()
        compose.onNodeWithTag("custom-font-select").assertIsNotSelected()
        assertEquals("system", AppearancePreferences.fontFamily)
        assertNotNull(selectedFile())
        assertEquals(FontFamily.Default, renderedFamily)
        compose.onNodeWithTag("custom-font-select").performScrollTo().performClick()
        compose.onNodeWithTag("custom-font-select").assertIsSelected()
        assertEquals(AppearancePreferences.customFontFamily, renderedFamily)
        capture("custom-font-dark-large")
        compose.onNodeWithTag("custom-font-remove").performScrollTo().performClick()
        compose.onNodeWithTag("custom-font-select").assertDoesNotExist()
        assertNull(selectedFile())
        assertEquals("system", AppearancePreferences.fontFamily)
        assertEquals(FontFamily.Default, renderedFamily)
    }

    @Test fun damagedFontShowsRecoveryActionsAndSystemFallback() {
        AppearancePreferences.applyCustomFont(CustomFontStore.import(context, "Damaged.ttf", font()))
        File(File(context.filesDir, "custom-font"), checkNotNull(selectedFile())).writeText("damaged")
        AppearancePreferences.load(context)
        show()
        assertEquals(FontFamily.Default, renderedFamily)
        compose.onNodeWithTag("custom-font-error").assertIsDisplayed()
        compose.onNodeWithTag("custom-font-select").assertDoesNotExist()
        compose.onNodeWithTag("custom-font-import").assertIsEnabled()
        compose.onNodeWithTag("custom-font-remove").performClick()
        compose.onNodeWithTag("custom-font-error").assertDoesNotExist()
        assertNull(selectedFile())
    }

    private fun show(large: Boolean = false) {
        compose.setContent {
            val density = LocalDensity.current
            CompositionLocalProvider(LocalActivityResultRegistryOwner provides owner,
                LocalDensity provides Density(density.density, if (large) 2f else 1f)) {
                MaterialTheme(colorScheme = if (large) darkColorScheme() else lightColorScheme(),
                    typography = com.zying.zysu.ui.theme.Typography.withAppearance(
                        AppearancePreferences.fontScale, AppearancePreferences.fontFamily,
                        AppearancePreferences.customFontFamily)) {
                    val family = MaterialTheme.typography.bodyLarge.fontFamily
                    SideEffect { renderedFamily = family }
                    Surface(Modifier.fillMaxSize()) {
                        Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp)) { CustomFontSetting() }
                    }
                }
            }
        }
        compose.waitForIdle()
    }

    private fun capture(name: String) {
        val file = File("../../out/custom-home-font-ui/$name.png")
        file.parentFile?.mkdirs()
        compose.onRoot().captureToImage().asAndroidBitmap().let { bitmap ->
            file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        }
    }
}
