package ui.screen.moreSettings.component

import android.content.Context
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.DarkMode
import androidx.compose.material.icons.filled.Dashboard
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.MonitorHeart
import androidx.compose.material.icons.filled.RestartAlt
import androidx.compose.material.icons.filled.TextFields
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.core.content.edit
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.theme.AppearancePreferences
import com.zying.zysu.ui.theme.CardConfig
import com.zying.zysu.ui.theme.getCardBorder
import com.zying.zysu.ui.theme.getCardColors
import com.zying.zysu.ui.theme.getCardShape
import kotlin.math.roundToInt

@Composable
fun AppearancePreview() {
    SettingsCard(title = stringResource(R.string.appearance_preview_title)) {
        SettingsControlGroup {
            Surface(
                modifier = Modifier.fillMaxWidth(),
                shape = getCardShape(),
                border = getCardBorder(),
                color = getCardColors(MaterialTheme.colorScheme.surfaceContainer).containerColor,
            ) {
                Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        SettingsIconBadge(Icons.Default.Check, selected = true)
                        Column(Modifier.weight(1f)) {
                            Text(stringResource(R.string.appearance_preview_heading), style = MaterialTheme.typography.titleMedium)
                            Text(stringResource(R.string.appearance_preview_body), style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        listOf(MaterialTheme.colorScheme.primary, MaterialTheme.colorScheme.secondary, MaterialTheme.colorScheme.tertiary).forEach {
                            Surface(color = it, shape = MaterialTheme.shapes.small, modifier = Modifier.size(32.dp)) {}
                        }
                    }
                }
            }
        }
    }
}

@Composable
fun AmoledSetting() {
    val context = LocalContext.current
    SwitchSettingItem(
        icon = Icons.Default.DarkMode,
        title = stringResource(R.string.appearance_amoled),
        summary = stringResource(R.string.appearance_amoled_summary),
        checked = AppearancePreferences.amoled,
        groupPosition = MoreSettingsItemPosition.Last,
        onChange = { AppearancePreferences.setAmoled(context, it) },
    )
}

@Composable
fun TypographyAndShapeSettings() {
    val context = LocalContext.current
    var resetDialog by remember { mutableStateOf(false) }
    SettingsCard(title = stringResource(R.string.appearance_type_shape), icon = Icons.Default.TextFields) {
        SettingsControlGroup {
            Text(stringResource(R.string.appearance_font_family), style = MaterialTheme.typography.titleSmall)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf("system" to R.string.appearance_font_system, "serif" to R.string.appearance_font_serif,
                    "monospace" to R.string.appearance_font_monospace).forEach { (family, title) ->
                    FilterChip(
                        selected = AppearancePreferences.fontFamily == family,
                        onClick = { AppearancePreferences.setFontFamily(context, family) },
                        label = { Text(stringResource(title)) },
                    )
                }
            }
        }
        AppearanceSlider(
            title = stringResource(R.string.appearance_font_scale),
            summary = stringResource(R.string.appearance_font_scale_summary),
            value = AppearancePreferences.fontScale,
            range = AppearancePreferences.MIN_FONT_SCALE..AppearancePreferences.MAX_FONT_SCALE,
            steps = 8,
            valueLabel = { stringResource(R.string.appearance_percentage, (it * 100).roundToInt()) },
            onCommit = { AppearancePreferences.setFontScale(context, it) },
        )
        AppearanceSlider(
            title = stringResource(R.string.appearance_card_corners),
            summary = stringResource(R.string.appearance_card_corners_summary),
            value = AppearancePreferences.cornerRadius,
            range = 12f..32f,
            steps = 9,
            valueLabel = { stringResource(R.string.appearance_dp, it.roundToInt()) },
            onCommit = { AppearancePreferences.setCornerRadius(context, it) },
        )
        TextButton(onClick = { resetDialog = true }, modifier = Modifier.fillMaxWidth().padding(8.dp)) {
            YukiIcon(Icons.Default.RestartAlt, null)
            Text(stringResource(R.string.appearance_reset), modifier = Modifier.padding(start = 8.dp))
        }
    }
    if (resetDialog) ConfirmDialog(
        title = stringResource(R.string.appearance_reset),
        message = stringResource(R.string.appearance_reset_summary),
        onConfirm = { AppearancePreferences.reset(context); resetDialog = false },
        onDismiss = { resetDialog = false },
    )
}

@Composable
private fun AppearanceSlider(
    title: String,
    summary: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    steps: Int,
    valueLabel: @Composable (Float) -> String,
    onCommit: (Float) -> Unit,
) {
    var draft by remember(value) { mutableFloatStateOf(value) }
    SettingsControlGroup {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(title, style = MaterialTheme.typography.titleSmall)
                Text(summary, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Text(valueLabel(draft), style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.primary)
        }
        Slider(
            value = draft,
            onValueChange = { draft = it },
            onValueChangeFinished = { onCommit(draft) },
            valueRange = range,
            steps = steps,
            modifier = Modifier.semantics { contentDescription = title },
        )
    }
}

@Composable
fun HomeDisplaySettings() {
    val context = LocalContext.current
    val preferences = remember(context) { context.getSharedPreferences("settings", Context.MODE_PRIVATE) }
    HomeLayoutSetting()
    data class Option(val key: String, val default: Boolean, val title: Int, val summary: Int)
    val options = listOf(
        Option("home_show_metrics", true, R.string.appearance_home_metrics, R.string.appearance_home_metrics_summary),
        Option("home_show_counts", true, R.string.appearance_home_counts, R.string.appearance_home_counts_summary),
        Option("home_show_device_details", false, R.string.appearance_home_details, R.string.appearance_home_details_summary),
    )
    options.forEachIndexed { index, option ->
        var enabled by remember(option.key) { mutableStateOf(preferences.getBoolean(option.key, option.default)) }
        SwitchSettingItem(
            icon = when (index) { 0 -> Icons.Default.MonitorHeart; 1 -> Icons.Default.Dashboard; else -> Icons.Default.Info },
            title = stringResource(option.title),
            summary = stringResource(option.summary),
            checked = enabled,
            onChange = { enabled = it; preferences.edit { putBoolean(option.key, it) } },
        )
    }
}

@Composable
fun BackgroundBlurSetting() {
    val context = LocalContext.current
    AppearanceSlider(
        title = stringResource(R.string.appearance_background_blur),
        summary = stringResource(R.string.appearance_background_blur_summary),
        value = CardConfig.backgroundBlur,
        range = 0f..32f,
        steps = 15,
        valueLabel = { stringResource(R.string.appearance_dp, it.roundToInt()) },
        onCommit = { CardConfig.updateBackgroundBlur(it); CardConfig.save(context) },
    )
}
