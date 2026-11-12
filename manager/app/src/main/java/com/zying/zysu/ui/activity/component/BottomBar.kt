package com.zying.zysu.ui.activity.component

import android.annotation.SuppressLint
import androidx.compose.foundation.background
import androidx.compose.foundation.LocalIndication
import androidx.compose.foundation.indication
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.Density
import androidx.navigation.NavHostController
import androidx.navigation.compose.currentBackStackEntryAsState
import com.ramcosta.composedestinations.generated.NavGraphs
import com.ramcosta.composedestinations.utils.isRouteOnBackStackAsState
import com.ramcosta.composedestinations.utils.rememberDestinationsNavigator
import app.entry.Launcher
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.activity.util.AppData
import com.zying.zysu.ui.screen.BottomBarDestination
import com.zying.zysu.ui.util.LocalNavigationLeaveGuard

@SuppressLint("ContextCastToActivity")
@Composable
fun BottomBar(navController: NavHostController) {
    val navigator = navController.rememberDestinationsNavigator()
    val navigationLeaveGuard = LocalNavigationLeaveGuard.current
    val currentRoute = navController.currentBackStackEntryAsState().value?.destination?.route
    val isFullFeatured by AppData.DataRefreshManager.isFullFeatured.collectAsState()
    val activity = LocalContext.current as Launcher
    val settings by activity.settingsStateFlow.collectAsState()
    val superuserCount by AppData.DataRefreshManager.superuserCount.collectAsState()
    val moduleCount by AppData.DataRefreshManager.moduleCount.collectAsState()
    val destinations = BottomBarDestination.entries.filter { isFullFeatured || !it.rootRequired }

    val style by rememberNavigationBarStyle()
    val items = destinations.map { destination ->
        key(destination) {
            val selected by navController.isRouteOnBackStackAsState(destination.direction)
            val count = when (destination) {
                BottomBarDestination.SuperUser -> superuserCount
                BottomBarDestination.Module -> moduleCount
                else -> 0
            }
            NavigationBarItem(
                key = destination.name,
                label = stringResource(destination.label),
                icon = if (selected) destination.iconSelected else destination.iconNotSelected,
                selected = selected,
                badgeCount = if (settings.isHideOtherInfo) 0 else count,
            )
        }
    }
    BottomNavigationBar(style = style, items = items, onSelected = { destinationKey ->
        val destination = destinations.first { it.name == destinationKey }
        val selected = items.first { it.key == destinationKey }.selected
        navigationLeaveGuard.navigateOrIntercept(currentRoute) {
            if (selected) navigator.popBackStack(destination.direction, false)
            navigator.navigate(destination.direction) {
                popUpTo(NavGraphs.root) { saveState = true }
                launchSingleTop = true
                restoreState = true
            }
        }
    })
}

/** Reserve badge space within the item instead of drawing outside an icon anchor. */
@Composable
internal fun GlassNavigationIcon(icon: ImageVector, selected: Boolean, count: Int = 0) {
    Box(Modifier.size(width = 56.dp, height = 32.dp)) {
        YukiIcon(
            icon,
            contentDescription = null,
            modifier = Modifier.align(Alignment.BottomCenter).size(24.dp),
            filled = selected,
        )
        if (count > 0) {
            val countDescription = pluralStringResource(R.plurals.ui_navigation_item_count, count, count)
            val density = LocalDensity.current
            // The full count remains spoken; only this small supplementary badge has a size cap.
            // Destination labels continue to use the user's unrestricted font scale.
            CompositionLocalProvider(
                LocalDensity provides Density(density.density, density.fontScale.coerceAtMost(1.3f)),
            ) {
                Badge(
                    modifier = Modifier.align(Alignment.TopEnd)
                        .semantics { contentDescription = countDescription },
                    containerColor = MaterialTheme.colorScheme.primary,
                    contentColor = MaterialTheme.colorScheme.onPrimary,
                ) {
                    Text(
                        if (count > 99) "99+" else count.toString(),
                        modifier = Modifier.clearAndSetSemantics {},
                        style = MaterialTheme.typography.labelSmall,
                        maxLines = 1,
                    )
                }
            }
        }
    }
}

/** The selection material surrounds both icon and label, matching the outer glass capsule. */
@Composable
internal fun GlassNavigationItem(
    selected: Boolean,
    onClick: () -> Unit,
    icon: @Composable () -> Unit,
    label: @Composable () -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = MaterialTheme.colorScheme
    val interactionSource = remember { MutableInteractionSource() }
    val dark = colors.surfaceContainerLow.luminance() < 0.5f
    val shape = RoundedCornerShape(percent = 50)
    val contentColor = when {
        !selected -> colors.onSurface
        dark -> colors.primary
        else -> colors.onPrimaryContainer
    }
    val selectionMaterial = if (selected) {
        Modifier
            .background(colors.surfaceContainerHighest.copy(alpha = if (dark) 0.90f else 0.76f))
            .background(
                Brush.linearGradient(
                    listOf(
                        colors.primary.copy(alpha = if (dark) 0.08f else 0.10f),
                        colors.primary.copy(alpha = 0.02f),
                        Color.Transparent,
                    ),
                ),
            )
    } else {
        Modifier
    }

    CompositionLocalProvider(LocalContentColor provides contentColor) {
        Box(
            modifier = modifier
                .heightIn(min = 56.dp)
                .selectable(
                    selected = selected,
                    role = Role.Tab,
                    interactionSource = interactionSource,
                    indication = null,
                    onClick = onClick,
                ),
            contentAlignment = Alignment.Center,
        ) {
            // Only the material and ripple are clipped. Text and count badges keep their bounds.
            Box(
                Modifier.matchParentSize().clip(shape).then(selectionMaterial)
                    .indication(interactionSource, LocalIndication.current),
            )
            Column(
                modifier = Modifier.fillMaxWidth().padding(horizontal = 4.dp, vertical = 4.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(2.dp, Alignment.CenterVertically),
            ) {
                icon()
                label()
            }
        }
    }
}
