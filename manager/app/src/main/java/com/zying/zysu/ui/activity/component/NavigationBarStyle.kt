package com.zying.zysu.ui.activity.component

import android.content.Context
import android.content.SharedPreferences
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.edit

/** Stable persisted names keep the existing capsule as the default for every upgrade. */
enum class NavigationBarStyle(val preferenceValue: String) {
    Glass("glass"),
    Material("material");

    companion object {
        fun fromPreference(value: String?): NavigationBarStyle =
            entries.firstOrNull { it.preferenceValue == value } ?: Glass
    }
}

object NavigationBarStylePreferences {
    internal const val FILE_NAME = "navigation_appearance"
    internal const val STYLE_KEY = "bottom_bar_style"

    internal fun preferences(context: Context): SharedPreferences =
        context.applicationContext.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)

    fun read(context: Context): NavigationBarStyle = read(preferences(context))

    internal fun read(preferences: SharedPreferences): NavigationBarStyle =
        NavigationBarStyle.fromPreference(preferences.getString(STYLE_KEY, null))

    fun set(context: Context, style: NavigationBarStyle) {
        preferences(context).edit { putString(STYLE_KEY, style.preferenceValue) }
    }
}

/** Settings and the live bar observe one preference, including external restoration/clearing. */
@Composable
fun rememberNavigationBarStyle(): State<NavigationBarStyle> {
    val context = LocalContext.current
    val preferences = remember(context) { NavigationBarStylePreferences.preferences(context) }
    val style = remember(preferences) { mutableStateOf(NavigationBarStylePreferences.read(preferences)) }
    DisposableEffect(preferences) {
        val listener = SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
            if (key == null || key == NavigationBarStylePreferences.STYLE_KEY) {
                style.value = NavigationBarStylePreferences.read(preferences)
            }
        }
        preferences.registerOnSharedPreferenceChangeListener(listener)
        style.value = NavigationBarStylePreferences.read(preferences)
        onDispose { preferences.unregisterOnSharedPreferenceChangeListener(listener) }
    }
    return style
}
