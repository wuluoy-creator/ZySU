package com.zying.zysu.ui.theme

import android.content.Context
import androidx.compose.material3.Typography
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.core.content.edit
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** Visual preferences are independent of device density and the system accessibility font size. */
object AppearancePreferences {
    const val MIN_FONT_SCALE = 0.85f
    const val MAX_FONT_SCALE = 1.3f
    const val DEFAULT_CORNER_RADIUS = 24f

    var amoled by mutableStateOf(false)
        private set
    var fontScale by mutableFloatStateOf(1f)
        private set
    var fontFamily by mutableStateOf("system")
        private set
    internal var customFontFamily by mutableStateOf<FontFamily?>(null)
        private set
    internal var customFontName by mutableStateOf<String?>(null)
        private set
    internal var customFontProblem by mutableStateOf<CustomFontProblem?>(null)
        private set
    var cornerRadius by mutableFloatStateOf(DEFAULT_CORNER_RADIUS)
        private set

    private fun preferences(context: Context) = context.getSharedPreferences("appearance", Context.MODE_PRIVATE)

    fun load(context: Context) {
        load(context, CustomFontStore.load(context))
    }

    internal suspend fun loadFromStorage(context: Context) {
        val font = withContext(Dispatchers.IO) { CustomFontStore.load(context) }
        load(context, font)
    }

    private fun load(context: Context, font: CustomFontState) {
        val prefs = preferences(context)
        amoled = prefs.getBoolean("amoled", false)
        fontScale = validFontScale(prefs.getFloat("font_scale", 1f))
        updateCustomFont(font)
        val selected = validFontFamily(prefs.getString("font_family", "system"))
        fontFamily = if (selected == "custom" && customFontFamily == null) "system" else selected
        cornerRadius = validCornerRadius(prefs.getFloat("corner_radius", DEFAULT_CORNER_RADIUS))
    }

    fun setAmoled(context: Context, enabled: Boolean) {
        amoled = enabled
        preferences(context).edit { putBoolean("amoled", enabled) }
    }

    fun setFontScale(context: Context, scale: Float) {
        fontScale = validFontScale(scale)
        preferences(context).edit { putFloat("font_scale", fontScale) }
    }

    fun setFontFamily(context: Context, family: String) {
        fontFamily = validFontFamily(family).let { if (it == "custom" && customFontFamily == null) "system" else it }
        preferences(context).edit { putString("font_family", fontFamily) }
    }

    internal fun applyCustomFont(state: CustomFontState) {
        updateCustomFont(state)
        if (state.typeface != null) fontFamily = "custom"
    }

    internal fun removeCustomFont(context: Context) {
        CustomFontStore.remove(context)
        if (fontFamily == "custom") fontFamily = "system"
        updateCustomFont(CustomFontState())
    }

    private fun updateCustomFont(state: CustomFontState) {
        customFontName = state.name
        customFontFamily = state.typeface?.let { FontFamily(it) }
        customFontProblem = state.problem
    }

    fun setCornerRadius(context: Context, radius: Float) {
        cornerRadius = validCornerRadius(radius)
        preferences(context).edit { putFloat("corner_radius", cornerRadius) }
    }

    fun reset(context: Context, onFailure: () -> Unit = {}) {
        try {
            CustomFontStore.resetAppearance(context)
            load(context)
        } catch (_: CustomFontException) {
            onFailure()
        }
    }

    internal fun validFontScale(value: Float) =
        if (value.isFinite()) value.coerceIn(MIN_FONT_SCALE, MAX_FONT_SCALE) else 1f

    internal fun validCornerRadius(value: Float) =
        if (value.isFinite()) value.coerceIn(12f, 32f) else DEFAULT_CORNER_RADIUS

    internal fun validFontFamily(value: String?) =
        value?.takeIf { it in setOf("system", "serif", "monospace", "custom") } ?: "system"
}

internal fun Typography.withAppearance(scale: Float, family: String, customFamily: FontFamily? = null): Typography {
    val selectedFamily = when (family) {
        "serif" -> FontFamily.Serif
        "monospace" -> FontFamily.Monospace
        "custom" -> customFamily ?: FontFamily.Default
        else -> FontFamily.Default
    }
    fun TextStyle.adjust() = copy(
        fontSize = fontSize * scale,
        lineHeight = lineHeight * scale,
        letterSpacing = letterSpacing * scale,
        fontFamily = selectedFamily,
    )
    return copy(
        displayLarge = displayLarge.adjust(), displayMedium = displayMedium.adjust(), displaySmall = displaySmall.adjust(),
        headlineLarge = headlineLarge.adjust(), headlineMedium = headlineMedium.adjust(), headlineSmall = headlineSmall.adjust(),
        titleLarge = titleLarge.adjust(), titleMedium = titleMedium.adjust(), titleSmall = titleSmall.adjust(),
        bodyLarge = bodyLarge.adjust(), bodyMedium = bodyMedium.adjust(), bodySmall = bodySmall.adjust(),
        labelLarge = labelLarge.adjust(), labelMedium = labelMedium.adjust(), labelSmall = labelSmall.adjust(),
    )
}
