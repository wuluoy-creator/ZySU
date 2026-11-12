package com.zying.zysu.ui.activity.component

import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.background
import androidx.compose.foundation.LocalIndication
import androidx.compose.foundation.indication
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.layout.wrapContentSize
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.Dp
import com.zying.zysu.R
import com.zying.zysu.ui.component.LiquidGlassSurface
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.theme.AppearancePreferences
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

/** Rendering stays independent of navigation so every style shares selection and guard behavior. */
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
                                modifier = Modifier.weight(1f).fillMaxHeight().testTag("navigation-item-${item.key}"),
                            )
                        }
                    }
                }
            }
        }
        NavigationBarStyle.Material -> MaterialBottomNavigation(items, onSelected, modifier, windowInsets)
        NavigationBarStyle.FloatingStandard, NavigationBarStyle.FloatingDrawer ->
            ReferenceFloatingNavigation(style, items, onSelected, modifier, windowInsets)
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
                                    .testTag("navigation-item-${item.key}")
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

/** FinaSU's two floating forms retain the same floating header and glass material as this app. */
@Composable
private fun ReferenceFloatingNavigation(
    style: NavigationBarStyle,
    items: List<NavigationBarItem>,
    onSelected: (String) -> Unit,
    modifier: Modifier,
    windowInsets: WindowInsets,
) {
    BoxWithConstraints(modifier.fillMaxWidth().windowInsetsPadding(windowInsets)) {
        val preferredGutter = when { maxWidth > 600.dp -> 32.dp; maxWidth > 400.dp -> 24.dp; else -> 16.dp }
        val minimumWidth = 48.dp * items.size + 4.dp * (items.size - 1) + 16.dp
        val gutter = ((maxWidth - minimumWidth) / 2).coerceIn(0.dp, preferredGutter)
        val availableWidth = (maxWidth - gutter * 2).coerceAtLeast(minimumWidth)
        val width = if (style == NavigationBarStyle.FloatingStandard) {
            (52.dp * items.size + 6.dp * (items.size - 1) + 16.dp).coerceAtMost(availableWidth)
        } else {
            (56.dp * items.size + 88.dp).coerceAtMost(availableWidth)
        }
        Box(Modifier.fillMaxWidth().padding(horizontal = gutter, vertical = 16.dp), contentAlignment = Alignment.Center) {
            LiquidGlassSurface(Modifier.width(width).testTag("navigation-${style.preferenceValue}")) {
                if (style == NavigationBarStyle.FloatingStandard) {
                    FloatingBarStandard(items, onSelected, width)
                } else {
                    FloatingBarDrawer(items, onSelected, width)
                }
            }
        }
    }
}

/** Compact reference geometry: 52 dp icons, 6 dp gaps, 8 dp padding and a 68 dp capsule. */
@Composable
private fun FloatingBarStandard(items: List<NavigationBarItem>, onSelected: (String) -> Unit, barWidth: Dp) {
    val selectedIndex = items.indexOfFirst { it.selected }
    val spacing = if (barWidth >= 52.dp * items.size + 6.dp * (items.size - 1) + 16.dp) 6.dp else 4.dp
    val itemSize = ((barWidth - 16.dp - spacing * (items.size - 1)) / items.size).coerceIn(48.dp, 52.dp)
    val animatedIndex by animateFloatAsState(
        targetValue = selectedIndex.coerceAtLeast(0).toFloat(),
        animationSpec = spring(dampingRatio = Spring.DampingRatioMediumBouncy, stiffness = Spring.StiffnessLow),
        label = "floatingStandardIndicator",
    )
    Box(Modifier.fillMaxWidth().height(68.dp).padding(horizontal = 8.dp), contentAlignment = Alignment.CenterStart) {
        if (selectedIndex >= 0) {
            Box(Modifier.offset { IntOffset(((itemSize + spacing) * animatedIndex).roundToPx(), 0) }.size(itemSize)
                .background(MaterialTheme.colorScheme.secondaryContainer, CircleShape)
                .testTag("navigation-standard-indicator"))
        }
        Row(Modifier.fillMaxWidth().selectableGroup(), horizontalArrangement = Arrangement.spacedBy(spacing),
            verticalAlignment = Alignment.CenterVertically) {
            items.forEach { item ->
                key(item.key) {
                    val tint = if (item.selected) MaterialTheme.colorScheme.onSecondaryContainer else MaterialTheme.colorScheme.onSurfaceVariant
                    val interactionSource = remember { MutableInteractionSource() }
                    Box(Modifier.size(itemSize)
                        .selectable(item.selected, role = Role.Tab, interactionSource = interactionSource,
                            indication = null, onClick = { onSelected(item.key) })
                        .semantics { contentDescription = item.label }
                        .testTag("navigation-item-${item.key}"), contentAlignment = Alignment.Center) {
                        Box(Modifier.matchParentSize().clip(CircleShape).indication(interactionSource, LocalIndication.current))
                        FloatingNavigationIcon(item, tint)
                    }
                }
            }
        }
    }
}

/** Reference drawer geometry: one expanded destination, remaining destinations are 48 dp icons. */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun FloatingBarDrawer(items: List<NavigationBarItem>, onSelected: (String) -> Unit, barWidth: Dp) {
    val selectedIndex = items.indexOfFirst { it.selected }
    val extraWidth = (barWidth - 16.dp - 4.dp * (items.size - 1) - 48.dp * items.size).coerceAtLeast(0.dp)
    val spatialSpec = MaterialTheme.motionScheme.defaultSpatialSpec<Float>()
    val expansions = items.map { item ->
        key(item.key) {
            val expansion by animateFloatAsState(if (item.selected) 1f else 0f, spatialSpec, label = "drawerExpansion-${item.key}")
            expansion.coerceIn(0f, 1f)
        }
    }
    // Normalize interrupted transitions so every icon keeps its 48 dp target throughout motion.
    val expansionTotal = expansions.sum()
    val widths = expansions.map { 48.dp + extraWidth * (if (expansionTotal > 0f) it / expansionTotal else 1f / items.size) }
    val indicatorOffset by animateDpAsState(
        targetValue = 52.dp * selectedIndex.coerceAtLeast(0),
        animationSpec = MaterialTheme.motionScheme.defaultSpatialSpec(), label = "drawerIndicatorOffset",
    )
    val density = LocalDensity.current
    val largeText = density.fontScale * AppearancePreferences.fontScale > 1.3f
    val labelStyle = MaterialTheme.typography.labelLarge
    val itemHeight = if (largeText) 40.dp + with(density) { labelStyle.lineHeight.toDp() } * 2 else 48.dp
    Box(Modifier.fillMaxWidth().height(itemHeight + 16.dp).padding(8.dp), contentAlignment = Alignment.CenterStart) {
        if (selectedIndex >= 0) {
            Box(Modifier.offset { IntOffset(indicatorOffset.roundToPx(), 0) }.width(48.dp + extraWidth).height(itemHeight)
                .background(MaterialTheme.colorScheme.secondaryContainer, CircleShape)
                .testTag("navigation-drawer-indicator"))
        }
        Row(Modifier.fillMaxWidth().selectableGroup(), horizontalArrangement = Arrangement.spacedBy(4.dp),
            verticalAlignment = Alignment.CenterVertically) {
            items.forEachIndexed { index, item ->
                key(item.key) {
                    val expansion = expansions[index]
                    val tint = if (item.selected) MaterialTheme.colorScheme.onSecondaryContainer else MaterialTheme.colorScheme.onSurfaceVariant
                    val iconScale by animateFloatAsState(if (item.selected) 1f else 0.9f, spatialSpec, label = "drawerIcon-${item.key}")
                    val interactionSource = remember { MutableInteractionSource() }
                    val itemModifier = Modifier.width(widths[index]).height(itemHeight)
                        .selectable(item.selected, role = Role.Tab, interactionSource = interactionSource,
                            indication = null, onClick = { onSelected(item.key) })
                        .semantics { contentDescription = item.label }.testTag("navigation-item-${item.key}")
                    Box(itemModifier) {
                        // Clip the ripple only; large labels keep their complete measured bounds.
                        Box(Modifier.matchParentSize().clip(CircleShape).indication(interactionSource, LocalIndication.current))
                        if (largeText) {
                            Column(Modifier.fillMaxSize().padding(vertical = 4.dp),
                                verticalArrangement = Arrangement.spacedBy(4.dp, Alignment.CenterVertically),
                                horizontalAlignment = Alignment.CenterHorizontally) {
                                FloatingNavigationIcon(item, tint, Modifier.graphicsLayer { scaleX = iconScale; scaleY = iconScale })
                                if (expansion > 0.001f && extraWidth > 0.dp) {
                                    DrawerLabel(item, expansion, tint, maxLines = 2, modifier = Modifier.padding(horizontal = 8.dp))
                                }
                            }
                        } else {
                            Row(Modifier.fillMaxSize(), horizontalArrangement = Arrangement.Center,
                                verticalAlignment = Alignment.CenterVertically) {
                                FloatingNavigationIcon(item, tint, Modifier.graphicsLayer { scaleX = iconScale; scaleY = iconScale })
                                if (expansion > 0.001f && extraWidth > 0.dp) {
                                    val labelGap = if (item.badgeCount > 0) 12.dp else 8.dp
                                    DrawerLabel(item, expansion, tint, maxLines = 1, modifier = Modifier.padding(start = labelGap * expansion))
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun DrawerLabel(item: NavigationBarItem, expansion: Float, tint: Color, maxLines: Int, modifier: Modifier = Modifier) {
    Box(modifier.graphicsLayer { alpha = expansion }.testTag("navigation-drawer-label-${item.key}")) {
        // The parent tab already announces the full label even while this visual label is collapsing.
        Box(Modifier.testTag("navigation-drawer-text-${item.key}")) {
            Text(item.label, Modifier.clearAndSetSemantics {}, color = tint, style = MaterialTheme.typography.labelLarge,
                maxLines = maxLines, overflow = TextOverflow.Ellipsis, textAlign = TextAlign.Center)
        }
    }
}

@Composable
private fun FloatingNavigationIcon(item: NavigationBarItem, tint: Color, modifier: Modifier = Modifier) {
    // Give the badge its intrinsic width and preserve Material's upper-right icon anchor.
    // The reserved row space plus label gap includes the badge's small horizontal overhang.
    Box(Modifier.size(width = 48.dp, height = 40.dp), contentAlignment = Alignment.Center) {
        BadgedBox(badge = {
            if (item.badgeCount > 0) {
                val description = pluralStringResource(R.plurals.ui_navigation_item_count, item.badgeCount, item.badgeCount)
                val density = LocalDensity.current
                CompositionLocalProvider(LocalDensity provides Density(density.density, density.fontScale.coerceAtMost(1.3f))) {
                    Badge(Modifier.wrapContentSize(unbounded = true).semantics { contentDescription = description }
                        .testTag("navigation-badge-${item.key}"),
                        containerColor = MaterialTheme.colorScheme.primary, contentColor = MaterialTheme.colorScheme.onPrimary) {
                        Text(if (item.badgeCount > 99) "99+" else item.badgeCount.toString(), Modifier.clearAndSetSemantics {},
                            style = MaterialTheme.typography.labelSmall, maxLines = 1)
                    }
                }
            }
        }) {
            YukiIcon(item.icon, null, Modifier.size(24.dp).then(modifier).testTag("navigation-glyph-${item.key}"),
                tint = tint, filled = item.selected)
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
