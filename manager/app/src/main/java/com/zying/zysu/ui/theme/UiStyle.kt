package com.zying.zysu.ui.theme

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Shapes
import androidx.compose.runtime.Composable
import androidx.compose.runtime.ReadOnlyComposable
import androidx.compose.ui.unit.dp

val isExpressiveUi: Boolean
    @Composable
    @ReadOnlyComposable
    // Compatibility name for secondary components. Appearance is now unified;
    // old stored style preferences no longer select a different visual system.
    get() = true

val ExpressiveListGroupMinHeight = 64.dp
val ExpressiveListGroupMinRadius = 4.dp

val ExpressiveShapes = Shapes(
    extraSmall = RoundedCornerShape(ExpressiveListGroupMinRadius),
    small = RoundedCornerShape(14.dp),
    medium = RoundedCornerShape(20.dp),
    large = RoundedCornerShape(24.dp),
    extraLarge = RoundedCornerShape(28.dp),
    largeIncreased = RoundedCornerShape(28.dp),
    extraLargeIncreased = RoundedCornerShape(32.dp),
    extraExtraLarge = RoundedCornerShape(36.dp)
)

internal fun appearanceShapes(radius: Float) = ExpressiveShapes.copy(
    medium = RoundedCornerShape((radius - 4f).coerceAtLeast(8f).dp),
    large = RoundedCornerShape(radius.dp),
    extraLarge = RoundedCornerShape((radius + 4f).dp),
    largeIncreased = RoundedCornerShape((radius + 4f).dp),
    extraLargeIncreased = RoundedCornerShape((radius + 8f).dp),
    extraExtraLarge = RoundedCornerShape((radius + 12f).dp),
)
