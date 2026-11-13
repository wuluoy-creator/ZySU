package com.zying.zysu.ui.component

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.TopAppBarScrollBehavior
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.zying.zysu.ui.theme.CardConfig

/** Shared by page headers and their tabs so custom backgrounds remain continuous. */
@Composable
fun yukiTopBarContainerColor(): Color = MaterialTheme.colorScheme.surfaceContainerLow.copy(
    alpha = if (CardConfig.isCustomBackgroundEnabled) CardConfig.cardAlpha else 1f,
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun YukiTopAppBar(
    title: @Composable () -> Unit,
    modifier: Modifier = Modifier,
    navigationIcon: @Composable () -> Unit = {},
    actions: @Composable RowScope.() -> Unit = {},
    // Page headers use pinned behavior so scrolling never changes their height or visibility.
    scrollBehavior: TopAppBarScrollBehavior? = null,
) {
    val colors = MaterialTheme.colorScheme
    val fontScale = LocalDensity.current.fontScale * com.zying.zysu.ui.theme.AppearancePreferences.fontScale
    Box(
        modifier = modifier.fillMaxWidth()
            .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal))
            .padding(horizontal = FloatingChrome.HorizontalGutter, vertical = FloatingChrome.VerticalGutter),
        contentAlignment = Alignment.Center,
    ) {
        LiquidGlassSurface(
            modifier = Modifier.widthIn(max = FloatingChrome.MaxHeaderWidth).fillMaxWidth().testTag("floating-page-header"),
            samplePageBackdrop = false,
        ) {
            TopAppBar(
                title = title,
                modifier = Modifier.padding(horizontal = 6.dp),
                navigationIcon = {
                    Box(
                        Modifier.clip(FloatingChrome.Shape)
                            .background(floatingChromeControlColor()),
                        contentAlignment = Alignment.Center,
                    ) { navigationIcon() }
                },
                actions = {
                    Row(
                        modifier = Modifier.clip(FloatingChrome.Shape)
                            .background(floatingChromeControlColor()),
                        verticalAlignment = Alignment.CenterVertically,
                        content = actions,
                    )
                },
                expandedHeight = 64.dp + (16 * (fontScale - 1f).coerceIn(0f, 1.5f)).dp,
                windowInsets = WindowInsets(0, 0, 0, 0),
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = Color.Transparent,
                    scrolledContainerColor = Color.Transparent,
                    navigationIconContentColor = colors.onSurface,
                    titleContentColor = colors.onSurface,
                    actionIconContentColor = colors.onSurface,
                ),
                scrollBehavior = scrollBehavior,
            )
        }
    }
}

@Composable
fun YukiTopBarTitle(
    text: String,
    modifier: Modifier = Modifier,
    color: Color = LocalContentColor.current,
) {
    Text(
        text = text,
        modifier = modifier,
        color = color,
        style = MaterialTheme.typography.titleLarge,
        fontWeight = FontWeight.SemiBold,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
    )
}
