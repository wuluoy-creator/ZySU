package com.zying.zysu.ui.theme

import android.app.Application
import android.content.Context
import androidx.compose.material3.darkColorScheme
import androidx.compose.ui.graphics.Color
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class AppearancePreferencesTest {
    private val context: Context get() = RuntimeEnvironment.getApplication()
    private val preferences get() = context.getSharedPreferences("appearance", Context.MODE_PRIVATE)

    @Before
    fun resetPreferences() = AppearancePreferences.reset(context)

    @Test
    fun preferencesSurviveReloadAndResetDoesNotChangeOtherSettings() {
        context.getSharedPreferences("settings", Context.MODE_PRIVATE).edit().putBoolean("home_show_device_details", false).commit()
        AppearancePreferences.setAmoled(context, true)
        AppearancePreferences.setFontScale(context, 1.2f)
        AppearancePreferences.setFontFamily(context, "serif")
        AppearancePreferences.setCornerRadius(context, 18f)
        AppearancePreferences.load(context)
        assertTrue(AppearancePreferences.amoled)
        assertEquals(1.2f, AppearancePreferences.fontScale)
        assertEquals("serif", AppearancePreferences.fontFamily)
        assertEquals(18f, AppearancePreferences.cornerRadius)

        AppearancePreferences.reset(context)
        assertFalse(AppearancePreferences.amoled)
        assertEquals(1f, AppearancePreferences.fontScale)
        assertEquals("system", AppearancePreferences.fontFamily)
        assertEquals(24f, AppearancePreferences.cornerRadius)
        assertFalse(context.getSharedPreferences("settings", Context.MODE_PRIVATE).getBoolean("home_show_device_details", true))
    }

    @Test
    fun invalidPersistedValuesCannotBreakTextOrShapes() {
        preferences.edit().putFloat("font_scale", Float.NaN).putFloat("corner_radius", Float.POSITIVE_INFINITY)
            .putString("font_family", "unknown").commit()
        AppearancePreferences.load(context)
        assertEquals(1f, AppearancePreferences.fontScale)
        assertEquals(24f, AppearancePreferences.cornerRadius)
        assertEquals("system", AppearancePreferences.fontFamily)
        AppearancePreferences.setFontScale(context, 10f)
        AppearancePreferences.setCornerRadius(context, -2f)
        assertEquals(1.3f, AppearancePreferences.fontScale)
        assertEquals(12f, AppearancePreferences.cornerRadius)
    }

    @Test
    fun amoledKeepsWallpaperTransparencyAndReadableCardHierarchy() {
        val original = darkColorScheme()
        val black = original.amoledSurfaces(enabled = true, transparent = false)
        assertEquals(Color.Black, black.background)
        assertEquals(original.onSurface, black.onSurface)
        assertTrue(black.surfaceContainerLow != black.background)
        val wallpaper = original.amoledSurfaces(enabled = true, transparent = true)
        assertEquals(Color.Transparent, wallpaper.background)
        assertEquals(Color.Transparent, wallpaper.surface)
        assertEquals(original, original.amoledSurfaces(enabled = false, transparent = false))
    }

    @Test
    fun scalingPreservesTextAndLineHeightProportions() {
        val scaled = Typography.withAppearance(1.2f, "system")
        assertEquals(Typography.bodyLarge.fontSize.value * 1.2f, scaled.bodyLarge.fontSize.value)
        assertEquals(Typography.bodyLarge.lineHeight.value * 1.2f, scaled.bodyLarge.lineHeight.value)
    }

    @Test
    fun wallpaperAdjustmentsRoundTripAndRejectInvalidNumbers() {
        val cardPreferences = context.getSharedPreferences("card_settings", Context.MODE_PRIVATE)
        cardPreferences.edit().clear().commit()
        CardConfig.reset()
        CardConfig.updateAlpha(0.65f)
        CardConfig.updateDim(0.3f)
        CardConfig.updateBackgroundBlur(12f)
        CardConfig.save(context)
        CardConfig.reset()
        CardConfig.load(context)
        assertEquals(0.65f, CardConfig.cardAlpha)
        assertEquals(0.3f, CardConfig.cardDim)
        assertEquals(12f, CardConfig.backgroundBlur)
        assertTrue(CardConfig.isCustomAlphaSet)
        assertTrue(CardConfig.isCustomDimSet)
        cardPreferences.edit().putFloat("card_alpha", Float.NaN).putFloat("card_dim", Float.NEGATIVE_INFINITY)
            .putFloat("background_blur", Float.NaN).commit()
        CardConfig.load(context)
        assertEquals(1f, CardConfig.cardAlpha)
        assertEquals(0f, CardConfig.cardDim)
        assertEquals(0f, CardConfig.backgroundBlur)
        CardConfig.updateBackgroundBlur(50f)
        assertEquals(32f, CardConfig.backgroundBlur)
        CardConfig.reset()
        cardPreferences.edit().clear().commit()
    }
}
