package com.zying.zysu.ui.component

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.layer.drawLayer
import androidx.compose.ui.graphics.rememberGraphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.unit.dp

/**
 * Floating headers share the scroll viewport. Callers apply [PaddingValues] inside their
 * scrolling content (LazyColumn.contentPadding or after verticalScroll), not around it.
 */
@Composable
internal fun YukiPageScaffold(
    topBar: @Composable () -> Unit,
    modifier: Modifier = Modifier,
    floatingActionButton: @Composable () -> Unit = {},
    snackbarHost: @Composable () -> Unit = {},
    contentWindowInsets: WindowInsets = WindowInsets.safeDrawing,
    content: @Composable (PaddingValues) -> Unit,
) {
    val layer = rememberGraphicsLayer()
    val backdrop = remember(layer) { SurfaceBackdrop(layer) }
    val colors = MaterialTheme.colorScheme
    val fadeColor = colors.background.takeIf { it.alpha > 0f } ?: colors.surfaceContainerLow
    Scaffold(
        modifier = modifier,
        topBar = {
            // This source contains only the body, never this header or its own sampled layer.
            CompositionLocalProvider(LocalTopBarBackdrop provides backdrop) {
                // Empty header space must not activate a row scrolling underneath it.
                // Child buttons consume their taps before this fallback detector.
                Box(Modifier.pointerInput(Unit) { detectTapGestures(onTap = {}) }) {
                    Box(
                        Modifier.matchParentSize().background(
                            Brush.verticalGradient(
                                0f to fadeColor,
                                0.3f to fadeColor.copy(alpha = 0.92f),
                                1f to fadeColor.copy(alpha = 0f),
                            ),
                        ),
                    )
                    topBar()
                }
            }
        },
        floatingActionButton = floatingActionButton,
        snackbarHost = snackbarHost,
        contentWindowInsets = contentWindowInsets,
    ) { padding ->
        BoxWithConstraints(
            Modifier.fillMaxSize()
                .consumeWindowInsets(padding)
                .onGloballyPositioned { backdrop.coordinates = it }
                .drawWithContent {
                    layer.record { this@drawWithContent.drawContent() }
                    drawLayer(layer)
                },
        ) {
            val direction = LocalLayoutDirection.current
            val gutter = ((maxWidth - 840.dp) / 2).coerceAtLeast(0.dp)
            content(PaddingValues(
                start = padding.calculateStartPadding(direction) + gutter,
                top = padding.calculateTopPadding(),
                end = padding.calculateEndPadding(direction) + gutter,
                bottom = padding.calculateBottomPadding(),
            ))
        }
    }
}
