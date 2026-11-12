package ui.screen.moreSettings.component

import android.content.Context
import android.content.SharedPreferences
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Dashboard
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import androidx.core.content.edit
import com.zying.zysu.R
import com.zying.zysu.ui.screen.home.HomeLayoutStyle

@Composable
internal fun HomeLayoutSetting() {
    val context = LocalContext.current
    val preferences = remember(context) { context.getSharedPreferences("settings", Context.MODE_PRIVATE) }
    var selected by remember(preferences) { mutableStateOf(HomeLayoutStyle.fromValue(preferences.getString("home_layout", null))) }
    var showPicker by rememberSaveable { mutableStateOf(false) }
    DisposableEffect(preferences) {
        val listener = SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
            if (key == "home_layout" || key == null) selected = HomeLayoutStyle.fromValue(preferences.getString("home_layout", null))
        }
        preferences.registerOnSharedPreferenceChangeListener(listener)
        onDispose { preferences.unregisterOnSharedPreferenceChangeListener(listener) }
    }
    SettingItem(
        icon = Icons.Default.Dashboard,
        title = stringResource(R.string.appearance_home_layout),
        subtitle = stringResource(homeLayoutTitle(selected)),
        onClick = { showPicker = true },
    )
    if (showPicker) {
        AlertDialog(
            onDismissRequest = { showPicker = false },
            title = { Text(stringResource(R.string.appearance_home_layout)) },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    Text(stringResource(R.string.appearance_home_layout_summary),
                        style = MaterialTheme.typography.bodyMedium,
                        modifier = Modifier.padding(bottom = 16.dp))
                    HomeLayoutChoices(selected) {
                        selected = it
                        preferences.edit { putString("home_layout", it.id) }
                        showPicker = false
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showPicker = false }) { Text(stringResource(R.string.cancel)) }
            },
        )
    }
}

internal fun homeLayoutTitle(style: HomeLayoutStyle): Int = when (style) {
    HomeLayoutStyle.Circle -> R.string.home_layout_circle
    HomeLayoutStyle.Grid -> R.string.home_layout_grid
    HomeLayoutStyle.Focus -> R.string.home_layout_focus
    HomeLayoutStyle.Dashboard -> R.string.home_layout_dashboard
    HomeLayoutStyle.Stats -> R.string.home_layout_stats
}

internal fun homeLayoutDescription(style: HomeLayoutStyle): Int = when (style) {
    HomeLayoutStyle.Circle -> R.string.home_layout_circle_description
    HomeLayoutStyle.Grid -> R.string.home_layout_grid_description
    HomeLayoutStyle.Focus -> R.string.home_layout_focus_description
    HomeLayoutStyle.Dashboard -> R.string.home_layout_dashboard_description
    HomeLayoutStyle.Stats -> R.string.home_layout_stats_description
}

@Composable
internal fun HomeLayoutChoices(selected: HomeLayoutStyle, onSelected: (HomeLayoutStyle) -> Unit) {
    Column(Modifier.selectableGroup(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        HomeLayoutStyle.entries.forEach { style ->
            val active = selected == style
            Surface(shape = MaterialTheme.shapes.medium,
                color = if (active) MaterialTheme.colorScheme.secondaryContainer else MaterialTheme.colorScheme.surfaceContainerLow) {
                Row(Modifier.fillMaxWidth().selectable(active, role = Role.RadioButton, onClick = { onSelected(style) })
                    .padding(horizontal = 12.dp, vertical = 12.dp),
                    verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    HomeLayoutThumbnail(style, Modifier.size(42.dp, 56.dp))
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Text(stringResource(homeLayoutTitle(style)), style = MaterialTheme.typography.titleSmall)
                        Text(stringResource(homeLayoutDescription(style)), style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    RadioButton(selected = active, onClick = null, modifier = Modifier.size(24.dp))
                }
            }
        }
    }
}

/** A schematic of each layout's hierarchy, with no duplicate spoken labels. */
@Composable
private fun HomeLayoutThumbnail(style: HomeLayoutStyle, modifier: Modifier = Modifier) {
    val primary = MaterialTheme.colorScheme.primary
    val muted = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.18f)
    Canvas(modifier) {
        fun tile(x: Float, y: Float, w: Float, h: Float, emphasis: Boolean = false) {
            drawRoundRect(if (emphasis) primary else muted, Offset(x * size.width, y * size.height),
                Size(w * size.width, h * size.height), CornerRadius(3.dp.toPx()))
        }
        when (style) {
            HomeLayoutStyle.Circle -> {
                drawCircle(primary, size.width * 0.23f, Offset(size.width * 0.5f, size.height * 0.23f))
                tile(0f, .49f, 1f, .2f); tile(0f, .76f, 1f, .2f)
            }
            HomeLayoutStyle.Grid -> {
                tile(0f, 0f, .46f, .5f, true); tile(.55f, 0f, .45f, .22f)
                tile(.55f, .28f, .45f, .22f); tile(0f, .58f, 1f, .38f)
            }
            HomeLayoutStyle.Focus -> {
                tile(0f, 0f, .46f, .44f, true); tile(.55f, 0f, .45f, .44f)
                tile(0f, .53f, .46f, .43f); tile(.55f, .53f, .45f, .43f)
            }
            HomeLayoutStyle.Dashboard -> {
                tile(0f, 0f, 1f, .32f, true); tile(0f, .41f, .46f, .2f)
                tile(.55f, .41f, .45f, .2f); tile(0f, .7f, 1f, .26f)
            }
            HomeLayoutStyle.Stats -> {
                tile(0f, 0f, .46f, .25f, true); tile(.55f, 0f, .45f, .25f, true)
                tile(0f, .34f, 1f, .25f); tile(0f, .68f, 1f, .28f)
            }
        }
    }
}
