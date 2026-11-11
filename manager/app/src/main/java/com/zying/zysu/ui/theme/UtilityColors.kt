package com.zying.zysu.ui.theme

import androidx.compose.material3.ColorScheme
import androidx.compose.ui.graphics.Color

/** Preserve distinct surfaces and readable modal controls when the page uses true black. */
internal fun ColorScheme.amoledSurfaces(enabled: Boolean, transparent: Boolean): ColorScheme =
    if (!enabled) this else copy(
        background = if (transparent) Color.Transparent else Color.Black,
        surface = if (transparent) Color.Transparent else Color.Black,
        surfaceDim = Color.Black,
        surfaceContainerLowest = Color.Black,
        surfaceContainerLow = Color(0xFF101216),
        surfaceContainer = Color(0xFF191C21),
        surfaceContainerHigh = Color(0xFF24282E),
        surfaceContainerHighest = Color(0xFF30363F),
    )

/** Neutral reading surfaces shared by every palette; brand and status colors stay intact. */
internal fun ColorScheme.utilitySurfaces(dark: Boolean, transparent: Boolean): ColorScheme =
    if (dark) copy(
        background = if (transparent) Color.Transparent else Color(0xFF101216),
        surface = if (transparent) Color.Transparent else Color(0xFF101216),
        onBackground = Color(0xFFE9EBEF),
        onSurface = Color(0xFFE9EBEF),
        onSurfaceVariant = Color(0xFFADB4BF),
        surfaceVariant = Color(0xFF30363F),
        outline = Color(0xFF838D9A),
        outlineVariant = Color(0xFF343A44),
        surfaceDim = Color(0xFF101216),
        surfaceBright = Color(0xFF353B44),
        surfaceContainerLowest = Color(0xFF1A1E24),
        surfaceContainerLow = Color(0xFF1A1E24),
        surfaceContainer = Color(0xFF20252C),
        surfaceContainerHigh = Color(0xFF282E36),
        surfaceContainerHighest = Color(0xFF343B45),
    ) else copy(
        background = if (transparent) Color.Transparent else Color(0xFFF5F6F8),
        surface = if (transparent) Color.Transparent else Color(0xFFF5F6F8),
        onBackground = Color(0xFF20242B),
        onSurface = Color(0xFF20242B),
        onSurfaceVariant = Color(0xFF596270),
        surfaceVariant = Color(0xFFE9ECF0),
        outline = Color(0xFF788290),
        outlineVariant = Color(0xFFDCE1E7),
        surfaceDim = Color(0xFFE8EBEF),
        surfaceBright = Color.White,
        surfaceContainerLowest = Color.White,
        surfaceContainerLow = Color.White,
        surfaceContainer = Color(0xFFF0F2F5),
        surfaceContainerHigh = Color(0xFFE9ECF0),
        surfaceContainerHighest = Color(0xFFE1E5EA),
    )
