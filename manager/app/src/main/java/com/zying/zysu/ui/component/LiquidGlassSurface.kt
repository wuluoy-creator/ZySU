package com.zying.zysu.ui.component

import android.graphics.RenderEffect
import android.graphics.RuntimeShader
import android.graphics.Shader
import android.os.Build
import androidx.annotation.RequiresApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asComposeRenderEffect
import androidx.compose.ui.graphics.drawscope.translate
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.layer.drawLayer
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.layout.LayoutCoordinates
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.dp
import com.zying.zysu.ui.theme.CardConfig
import com.zying.zysu.ui.theme.ThemeConfig

private class GlassCoordinates {
    var target: LayoutCoordinates? = null
    var offset by mutableStateOf<Offset?>(null)
}

/** Shared geometry and control surfaces for the floating page header and navigation. */
internal object FloatingChrome {
    val HorizontalGutter = 16.dp
    val VerticalGutter = 8.dp
    val MaxHeaderWidth = 920.dp
    val Shape = RoundedCornerShape(percent = 50)
}

@Composable
internal fun floatingChromeControlColor(selected: Boolean = false): Color {
    val colors = MaterialTheme.colorScheme
    val dark = colors.surfaceContainerLow.luminance() < 0.5f
    return colors.surfaceContainerHighest.copy(alpha = when {
        !selected -> 0.45f
        dark -> 0.90f
        else -> 0.76f
    })
}

/** Samples wallpaper and page content with a sharp foreground for text and touch targets. */
@Composable
internal fun LiquidGlassSurface(
    modifier: Modifier = Modifier,
    samplePageBackdrop: Boolean = true,
    content: @Composable BoxScope.() -> Unit,
) {
    // Headers use a body-only source; the dock's page source also contains the header.
    // Keeping those separate prevents a header from recording its own glass into itself.
    val backdrops = listOfNotNull(
        LocalWallpaperBackdrop.current,
        if (samplePageBackdrop) LocalPageBackdrop.current else LocalTopBarBackdrop.current,
    )
    val coordinates = remember(backdrops) { backdrops.map { GlassCoordinates() } }
    val dark = MaterialTheme.colorScheme.surfaceContainerLow.luminance() < 0.5f
    val hasWallpaper = CardConfig.isCustomBackgroundEnabled && ThemeConfig.customBackgroundUri != null
    val opacity = if (hasWallpaper) {
        // Retain a readable material even when the user's content cards are fully transparent.
        if (dark) 0.72f + CardConfig.cardAlpha * 0.16f else 0.60f + CardConfig.cardAlpha * 0.22f
    } else {
        if (dark) 0.88f else 0.82f
    }
    val tint = MaterialTheme.colorScheme.surfaceContainerLow.copy(alpha = opacity)
    val shape = FloatingChrome.Shape
    val density = LocalDensity.current
    val blur = remember(density) {
        val radius = with(density) { 20.dp.toPx() }
        RenderEffect.createBlurEffect(radius, radius, Shader.TileMode.CLAMP)
    }
    val plainBlur = remember(blur) { blur.asComposeRenderEffect() }
    val lens = remember { if (Build.VERSION.SDK_INT >= 33) GlassLens() else null }

    Box(
        modifier
            .shadow(
                elevation = 16.dp,
                shape = shape,
                clip = false,
                ambientColor = Color.Black.copy(alpha = if (dark) 0.22f else 0.10f),
                spotColor = Color.Black.copy(alpha = if (dark) 0.28f else 0.14f),
            ),
    ) {
        Box(
            Modifier.matchParentSize().clip(shape)
                .onGloballyPositioned { target ->
                    backdrops.forEachIndexed { index, backdrop ->
                        coordinates[index].target = target
                        coordinates[index].offset = backdrop.coordinates?.takeIf { it.isAttached }
                            ?.localPositionOf(target, Offset.Zero)
                    }
                }
                .graphicsLayer {
                    clip = true
                    renderEffect = if (Build.VERSION.SDK_INT >= 33 && lens != null) {
                        lens.effect(size.width, size.height, 4.dp.toPx(), blur)
                    } else {
                        plainBlur
                    }
                }
                .drawWithContent {
                    backdrops.forEachIndexed { index, backdrop ->
                        val source = backdrop.coordinates
                        val target = coordinates[index].target
                        if (source?.isAttached == true && target?.isAttached == true) {
                            // Both sources share the dock's predictive-back coordinate space.
                            val offset = coordinates[index].offset ?: source.localPositionOf(target, Offset.Zero)
                            translate(-offset.x, -offset.y) { drawLayer(backdrop.layer) }
                        }
                    }
                },
        )
        Box(
            Modifier.matchParentSize().clip(shape)
                .background(tint)
                .background(
                    Brush.verticalGradient(
                        listOf(
                            Color.White.copy(alpha = if (dark) 0.08f else 0.38f),
                            Color.White.copy(alpha = 0.01f),
                            Color.White.copy(alpha = if (dark) 0.025f else 0.09f),
                        ),
                    ),
                )
                .border(
                    1.5.dp,
                    Brush.linearGradient(
                        listOf(
                            Color.White.copy(alpha = if (dark) 0.32f else 0.96f),
                            Color.White.copy(alpha = if (dark) 0.06f else 0.42f),
                            Color.White.copy(alpha = if (dark) 0.18f else 0.78f),
                        ),
                    ),
                    shape,
                ),
        )
        // Large navigation labels can extend into the capsule's curved corners. Clip only
        // the material layers so the foreground keeps its full measured bounds.
        content()
    }
}

/** Android 13 adds a small edge refraction; Android 12 keeps the same blurred glass material. */
@RequiresApi(33)
private class GlassLens {
    private val shader = RuntimeShader(
        """
        uniform shader backdrop;
        uniform float2 resolution;
        uniform float strength;
        half4 main(float2 position) {
            float2 p = position / max(resolution, float2(1.0)) * 2.0 - 1.0;
            float edge = pow(max(abs(p.x), abs(p.y)), 8.0);
            return backdrop.eval(position - p * edge * strength);
        }
        """.trimIndent(),
    )

    fun effect(width: Float, height: Float, strength: Float, blur: RenderEffect): androidx.compose.ui.graphics.RenderEffect {
        shader.setFloatUniform("resolution", width, height)
        shader.setFloatUniform("strength", strength)
        return RenderEffect.createChainEffect(
            RenderEffect.createRuntimeShaderEffect(shader, "backdrop"),
            blur,
        ).asComposeRenderEffect()
    }
}
