package com.zying.zysu.ui.activity.component

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.key
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.zying.zysu.ui.component.LiquidGlassSurface
import com.zying.zysu.ui.theme.CardConfig
import com.zying.zysu.ui.theme.ThemeConfig

@Immutable
data class NavigationBarItem(
    val key: String,
    val label: String,
    val icon: ImageVector,
    val selected: Boolean,
    val badgeCount: Int = 0,
)

/** Rendering stays independent of navigation so both styles share selection and guard behavior. */
@Composable
fun BottomNavigationBar(
    style: NavigationBarStyle,
    items: List<NavigationBarItem>,
    onSelected: (String) -> Unit,
    modifier: Modifier = Modifier,
    windowInsets: WindowInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal + WindowInsetsSides.Bottom),
) {
    if (items.isEmpty()) return
    when (style) {
        NavigationBarStyle.Glass -> Box(
            modifier.fillMaxWidth().windowInsetsPadding(windowInsets).padding(horizontal = 16.dp, vertical = 8.dp),
            contentAlignment = Alignment.Center,
        ) {
            LiquidGlassSurface(Modifier.widthIn(max = (items.size * 104).dp).fillMaxWidth()) {
                Row(
                    Modifier.fillMaxWidth().height(IntrinsicSize.Min).padding(4.dp).selectableGroup(),
                    horizontalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    items.forEach { item ->
                        key(item.key) {
                            GlassNavigationItem(
                                selected = item.selected,
                                onClick = { onSelected(item.key) },
                                icon = { GlassNavigationIcon(item.icon, item.selected, item.badgeCount) },
                                label = { NavigationLabel(item) },
                                modifier = Modifier.weight(1f).fillMaxHeight(),
                            )
                        }
                    }
                }
            }
        }
        NavigationBarStyle.Material -> MaterialBottomNavigation(items, onSelected, modifier, windowInsets)
    }
}

/** FinaSU's docked material bar: a page-width band with a separate icon selection indicator. */
@Composable
private fun MaterialBottomNavigation(
    items: List<NavigationBarItem>,
    onSelected: (String) -> Unit,
    modifier: Modifier,
    windowInsets: WindowInsets,
) {
    val colors = MaterialTheme.colorScheme
    val hasWallpaper = CardConfig.isCustomBackgroundEnabled && ThemeConfig.customBackgroundUri != null
    // Controls remain legible even when content cards are configured to be fully transparent.
    val container = colors.surfaceContainerLow.copy(alpha = if (hasWallpaper) CardConfig.cardAlpha.coerceAtLeast(0.94f) else 1f)
    Column(modifier.fillMaxWidth().background(container)) {
        HorizontalDivider(color = colors.outlineVariant.copy(alpha = 0.55f))
        Box(
            Modifier.fillMaxWidth().windowInsetsPadding(windowInsets).padding(horizontal = 12.dp, vertical = 8.dp),
            contentAlignment = Alignment.Center,
        ) {
            Row(
                Modifier.widthIn(max = 840.dp).fillMaxWidth().height(IntrinsicSize.Min).selectableGroup(),
                horizontalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                items.forEach { item ->
                    key(item.key) {
                        val contentColor = if (item.selected) colors.primary else colors.onSurfaceVariant
                        CompositionLocalProvider(LocalContentColor provides contentColor) {
                            Column(
                                Modifier.weight(1f).fillMaxHeight().heightIn(min = 64.dp)
                                    .clip(MaterialTheme.shapes.large)
                                    .selectable(item.selected, role = Role.Tab, onClick = { onSelected(item.key) })
                                    .padding(horizontal = 2.dp, vertical = 2.dp),
                                horizontalAlignment = Alignment.CenterHorizontally,
                                verticalArrangement = Arrangement.spacedBy(4.dp, Alignment.CenterVertically),
                            ) {
                                Box(
                                    Modifier.clip(MaterialTheme.shapes.large)
                                        .background(if (item.selected) colors.secondaryContainer else Color.Transparent)
                                        .padding(horizontal = 4.dp, vertical = 2.dp),
                                    contentAlignment = Alignment.Center,
                                ) {
                                    CompositionLocalProvider(LocalContentColor provides if (item.selected) colors.onSecondaryContainer else contentColor) {
                                        GlassNavigationIcon(item.icon, item.selected, item.badgeCount)
                                    }
                                }
                                NavigationLabel(item)
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun NavigationLabel(item: NavigationBarItem) {
    Text(
        item.label,
        style = MaterialTheme.typography.labelMedium,
        fontWeight = if (item.selected) FontWeight.SemiBold else FontWeight.Medium,
        textAlign = TextAlign.Center,
        maxLines = 2,
        overflow = TextOverflow.Ellipsis,
    )
}
