package com.zying.zysu.ui.theme

import android.content.Context
import androidx.compose.foundation.BorderStroke
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.contentColorFor
import androidx.compose.runtime.*
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

@Stable
object CardConfig {
    // 卡片透明度
    var cardAlpha by mutableFloatStateOf(1f)
        internal set
    // 卡片亮度
    var cardDim by mutableFloatStateOf(0f)
        internal set
    var backgroundBlur by mutableFloatStateOf(0f)
        private set
    // 卡片阴影
    var cardElevation by mutableStateOf(0.dp)
        internal set

    // 功能开关
    var isShadowEnabled by mutableStateOf(true)
        internal set
    var isCustomBackgroundEnabled by mutableStateOf(false)
        internal set

    var isCustomAlphaSet by mutableStateOf(false)
        internal set
    var isCustomDimSet by mutableStateOf(false)
        internal set
    var isUserDarkModeEnabled by mutableStateOf(false)
        internal set
    var isUserLightModeEnabled by mutableStateOf(false)
        internal set

    private object Keys {
        const val CARD_ALPHA = "card_alpha"
        const val CARD_DIM = "card_dim"
        const val BACKGROUND_BLUR = "background_blur"
        const val CUSTOM_BACKGROUND_ENABLED = "custom_background_enabled"
        const val IS_SHADOW_ENABLED = "is_shadow_enabled"
        const val IS_CUSTOM_ALPHA_SET = "is_custom_alpha_set"
        const val IS_CUSTOM_DIM_SET = "is_custom_dim_set"
        const val IS_USER_DARK_MODE_ENABLED = "is_user_dark_mode_enabled"
        const val IS_USER_LIGHT_MODE_ENABLED = "is_user_light_mode_enabled"
    }

    fun updateAlpha(alpha: Float, isCustom: Boolean = true) {
        cardAlpha = alpha.takeIf { it.isFinite() }?.coerceIn(0f, 1f) ?: 1f
        if (isCustom) isCustomAlphaSet = true
    }

    fun updateDim(dim: Float, isCustom: Boolean = true) {
        cardDim = dim.takeIf { it.isFinite() }?.coerceIn(0f, 1f) ?: 0f
        if (isCustom) isCustomDimSet = true
    }

    fun updateBackgroundBlur(radius: Float) {
        backgroundBlur = radius.takeIf { it.isFinite() }?.coerceIn(0f, 32f) ?: 0f
    }

    fun updateShadow(enabled: Boolean, elevation: Dp = cardElevation) {
        isShadowEnabled = enabled
        cardElevation = if (enabled) elevation else cardElevation
    }

    fun updateBackground(enabled: Boolean) {
        isCustomBackgroundEnabled = enabled
        // 自定义背景时自动禁用阴影以获得更好的视觉效果
        if (enabled) {
            updateShadow(false)
        }
    }

    fun updateThemePreference(darkMode: Boolean?, lightMode: Boolean?) {
        isUserDarkModeEnabled = darkMode ?: false
        isUserLightModeEnabled = lightMode ?: false
    }

    fun reset() {
        cardAlpha = 1f
        cardDim = 0f
        backgroundBlur = 0f
        cardElevation = 0.dp
        isShadowEnabled = true
        isCustomBackgroundEnabled = false
        isCustomAlphaSet = false
        isCustomDimSet = false
        isUserDarkModeEnabled = false
        isUserLightModeEnabled = false
    }

    fun setThemeDefaults(isDarkMode: Boolean) {
        if (!isCustomAlphaSet) {
            updateAlpha(if (isDarkMode) 0.88f else 1f, false)
        }
        if (!isCustomDimSet) {
            updateDim(if (isDarkMode) 0.25f else 0f, false)
        }
        // 暗色模式下默认启用轻微阴影
        if (isDarkMode && !isCustomBackgroundEnabled) {
            updateShadow(true, 2.dp)
        }
    }

    fun save(context: Context) {
        val prefs = context.getSharedPreferences("card_settings", Context.MODE_PRIVATE)
        prefs.edit().apply {
            putFloat(Keys.CARD_ALPHA, cardAlpha)
            putFloat(Keys.CARD_DIM, cardDim)
            putFloat(Keys.BACKGROUND_BLUR, backgroundBlur)
            putBoolean(Keys.CUSTOM_BACKGROUND_ENABLED, isCustomBackgroundEnabled)
            putBoolean(Keys.IS_SHADOW_ENABLED, isShadowEnabled)
            putBoolean(Keys.IS_CUSTOM_ALPHA_SET, isCustomAlphaSet)
            putBoolean(Keys.IS_CUSTOM_DIM_SET, isCustomDimSet)
            putBoolean(Keys.IS_USER_DARK_MODE_ENABLED, isUserDarkModeEnabled)
            putBoolean(Keys.IS_USER_LIGHT_MODE_ENABLED, isUserLightModeEnabled)
            apply()
        }
    }

    fun load(context: Context) {
        val prefs = context.getSharedPreferences("card_settings", Context.MODE_PRIVATE)
        updateAlpha(prefs.getFloat(Keys.CARD_ALPHA, 1f), isCustom = false)
        updateDim(prefs.getFloat(Keys.CARD_DIM, 0f), isCustom = false)
        updateBackgroundBlur(prefs.getFloat(Keys.BACKGROUND_BLUR, 0f))
        isCustomBackgroundEnabled = prefs.getBoolean(Keys.CUSTOM_BACKGROUND_ENABLED, false)
        isShadowEnabled = prefs.getBoolean(Keys.IS_SHADOW_ENABLED, true)
        isCustomAlphaSet = prefs.getBoolean(Keys.IS_CUSTOM_ALPHA_SET, false)
        isCustomDimSet = prefs.getBoolean(Keys.IS_CUSTOM_DIM_SET, false)
        isUserDarkModeEnabled = prefs.getBoolean(Keys.IS_USER_DARK_MODE_ENABLED, false)
        isUserLightModeEnabled = prefs.getBoolean(Keys.IS_USER_LIGHT_MODE_ENABLED, false)

        updateShadow(isShadowEnabled, if (isShadowEnabled) cardElevation else 0.dp)
    }

}

object CardStyleProvider {

    @Composable
    fun getCardShape() = androidx.compose.material3.MaterialTheme.shapes.large

    /** Shared card outline; pass a semantic color for status or warning borders. */
    @Composable
    fun getCardBorder(
        color: Color = Color.Unspecified,
    ) = BorderStroke(
        1.dp,
        if (color == Color.Unspecified) MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.45f)
        else color,
    )

    @Composable
    fun getCardColors(originalColor: Color) = CardDefaults.cardColors(
        containerColor = originalColor.copy(alpha = CardConfig.cardAlpha),
        contentColor = determineContentColor(originalColor),
        disabledContainerColor = originalColor.copy(alpha = CardConfig.cardAlpha * 0.38f),
        disabledContentColor = determineContentColor(originalColor).copy(alpha = 0.38f)
    )

    @Composable
    fun getCardElevation() = CardDefaults.cardElevation(
        defaultElevation = if (CardConfig.isShadowEnabled) CardConfig.cardElevation else 0.dp,
        pressedElevation = if (CardConfig.isShadowEnabled) {
            (CardConfig.cardElevation.value + 0).dp
        } else 0.dp,
        focusedElevation = if (CardConfig.isShadowEnabled) {
            (CardConfig.cardElevation.value + 0).dp
        } else 0.dp,
        hoveredElevation = if (CardConfig.isShadowEnabled) {
            (CardConfig.cardElevation.value + 0).dp
        } else 0.dp,
        draggedElevation = if (CardConfig.isShadowEnabled) {
            (CardConfig.cardElevation.value + 0).dp
        } else 0.dp,
        disabledElevation = 0.dp
    )

    @Composable
    private fun determineContentColor(originalColor: Color): Color {
        // The active scheme already reflects forced light/dark mode and the selected palette.
        val themedColor = MaterialTheme.colorScheme.contentColorFor(originalColor)
        return if (themedColor != Color.Unspecified) themedColor
        else if (originalColor.luminance() > 0.5f) Color.Black else Color.White
    }
}

// 向后兼容
@Composable
fun getCardBorder(
    color: Color = Color.Unspecified,
) = CardStyleProvider.getCardBorder(color)

@Composable
fun getCardColors(originalColor: Color) = CardStyleProvider.getCardColors(originalColor)

@Composable
fun getCardElevation() = CardStyleProvider.getCardElevation()

@Composable
fun getCardShape() = CardStyleProvider.getCardShape()
