package com.zying.zysu.ui.component

import android.content.res.Configuration
import android.text.format.Formatter
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.AdminPanelSettings
import androidx.compose.material.icons.outlined.BatteryStd
import androidx.compose.material.icons.outlined.Extension
import androidx.compose.material.icons.outlined.Memory
import androidx.compose.material.icons.outlined.Storage
import androidx.compose.material.icons.outlined.Thermostat
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.theme.UtilityPreviewTheme
import com.zying.zysu.ui.theme.AppearancePreferences
import com.zying.zysu.ui.theme.getCardBorder
import com.zying.zysu.ui.theme.getCardColors
import com.zying.zysu.ui.theme.getCardShape
import com.zying.zysu.ui.theme.getCardElevation
import com.zying.zysu.ui.viewmodel.HomeCapacity
import com.zying.zysu.ui.viewmodel.HomeDeviceMetrics
import java.text.NumberFormat

@Composable
internal fun HomeSectionTitle(title: String, modifier: Modifier = Modifier) {
    Text(
        title,
        modifier = modifier.padding(horizontal = 4.dp).semantics { heading() },
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}

/** A shared public-device overview, independent of the kernel/root status above it. */
@Composable
internal fun HomeDeviceOverview(metrics: HomeDeviceMetrics?, listLayout: Boolean = false) {
    ElevatedCard(
        modifier = Modifier.fillMaxWidth().border(getCardBorder(), getCardShape()),
        shape = getCardShape(),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
        elevation = getCardElevation(),
    ) {
        Column(Modifier.fillMaxWidth().padding(20.dp), verticalArrangement = Arrangement.spacedBy(20.dp)) {
            Text(stringResource(R.string.home_device_overview), style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.semantics { heading() })
            if (metrics == null) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                    Text(stringResource(R.string.home_status_loading), style = MaterialTheme.typography.bodyMedium)
                }
            } else {
                HomeAdaptivePair(
                    listLayout = listLayout,
                    first = { CapacityMetric(Icons.Outlined.Memory, stringResource(R.string.home_memory_usage), metrics.memory, it) },
                    second = { CapacityMetric(Icons.Outlined.Storage, stringResource(R.string.home_storage_usage), metrics.storage, it) },
                )
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                HomeAdaptivePair(
                    listLayout = listLayout,
                    first = { modifier ->
                        DeviceMetric(
                            Icons.Outlined.BatteryStd,
                            stringResource(R.string.home_battery),
                            metrics.batteryPercent?.let { NumberFormat.getPercentInstance().format(it / 100.0) }
                                ?: stringResource(R.string.home_reading_unavailable),
                            listOfNotNull(
                                metrics.batteryTemperatureC?.let { temperature(it) },
                                metrics.charging?.let { stringResource(if (it) R.string.home_charging else R.string.home_on_battery) },
                            ).joinToString(" · "),
                            modifier,
                        )
                    },
                    second = { modifier ->
                        DeviceMetric(
                            Icons.Outlined.Thermostat,
                            stringResource(R.string.home_cpu_temperature),
                            metrics.cpuTemperatureC?.let { temperature(it) } ?: stringResource(R.string.home_reading_unavailable),
                            metrics.cpuFrequencyKHz?.let {
                                stringResource(R.string.home_cpu_frequency, NumberFormat.getNumberInstance().format(it / 1000))
                            }.orEmpty(),
                            modifier,
                        )
                    },
                )
                Text(
                    stringResource(R.string.home_uptime_value, metrics.uptimeMillis / 86_400_000L,
                        metrics.uptimeMillis / 3_600_000L % 24, metrics.uptimeMillis / 60_000L % 60),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
private fun temperature(value: Float): String = stringResource(
    R.string.home_temperature_value,
    NumberFormat.getNumberInstance().apply { maximumFractionDigits = 1 }.format(value),
)

@Composable
private fun CapacityMetric(icon: ImageVector, label: String, capacity: HomeCapacity?, modifier: Modifier) {
    val context = LocalContext.current
    Column(modifier, verticalArrangement = Arrangement.spacedBy(12.dp)) {
        DeviceMetric(
            icon, label,
            capacity?.let { NumberFormat.getPercentInstance().format(it.fraction.toDouble()) }
                ?: stringResource(R.string.home_reading_unavailable),
            capacity?.let {
                stringResource(R.string.home_capacity_value, Formatter.formatShortFileSize(context, it.usedBytes),
                    Formatter.formatShortFileSize(context, it.totalBytes))
            }.orEmpty(),
        )
        if (capacity != null) {
            LinearProgressIndicator(progress = { capacity.fraction }, modifier = Modifier.fillMaxWidth())
        }
    }
}

@Composable
private fun DeviceMetric(icon: ImageVector, label: String, value: String, detail: String, modifier: Modifier = Modifier) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            YukiIcon(icon, null, Modifier.size(20.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(label, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Text(value, style = MaterialTheme.typography.titleLarge)
        if (detail.isNotBlank()) Text(detail, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

/** Two useful destinations and the real counts returned by their respective providers. */
@Composable
internal fun HomeCountCards(superuserCount: Int?, moduleCount: Int?, onSuperuser: () -> Unit, onModules: () -> Unit, listLayout: Boolean = false) {
    HomeAdaptivePair(
        first = { HomeCountCard(stringResource(R.string.home_authorized_apps), superuserCount, Icons.Outlined.AdminPanelSettings, onSuperuser, it) },
        second = { HomeCountCard(stringResource(R.string.home_installed_modules), moduleCount, Icons.Outlined.Extension, onModules, it) },
        listLayout = listLayout,
    )
}

@Composable
private fun HomeCountCard(label: String, count: Int?, icon: ImageVector, onClick: () -> Unit, modifier: Modifier) {
    ElevatedCard(
        onClick = onClick,
        modifier = modifier.border(getCardBorder(), getCardShape()),
        shape = getCardShape(),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
        elevation = getCardElevation(),
    ) {
        Column(Modifier.fillMaxWidth().padding(18.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            YukiIcon(icon, null, Modifier.size(24.dp), tint = MaterialTheme.colorScheme.primary)
            Text(count?.let { NumberFormat.getIntegerInstance().format(it) } ?: stringResource(R.string.home_reading_unavailable),
                style = MaterialTheme.typography.headlineSmall)
            Text(label, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun HomeAdaptivePair(
    first: @Composable (Modifier) -> Unit,
    second: @Composable (Modifier) -> Unit,
    listLayout: Boolean = false,
) {
    val fontScale = LocalDensity.current.fontScale * AppearancePreferences.fontScale
    BoxWithConstraints(Modifier.fillMaxWidth()) {
        if (!listLayout && maxWidth >= 300.dp && fontScale < 1.3f) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                first(Modifier.weight(1f))
                second(Modifier.weight(1f))
            }
        } else {
            Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(20.dp)) {
                first(Modifier.fillMaxWidth())
                second(Modifier.fillMaxWidth())
            }
        }
    }
}

@Preview(name = "Device overview", widthDp = 380)
@Preview(name = "Device overview dark", widthDp = 380, uiMode = Configuration.UI_MODE_NIGHT_YES)
@Preview(name = "Device overview large text", widthDp = 320, fontScale = 2f)
@Composable
private fun HomeDeviceOverviewPreview() {
    UtilityPreviewTheme {
        HomeDeviceOverview(HomeDeviceMetrics(
            batteryPercent = 76, batteryTemperatureC = 32.5f, charging = true,
            cpuTemperatureC = 43f, cpuFrequencyKHz = 2_400_000L,
            memory = HomeCapacity.fromAvailable(8_000_000_000L, 3_000_000_000L),
            storage = HomeCapacity.fromAvailable(256_000_000_000L, 90_000_000_000L),
            uptimeMillis = 185_400_000L,
        ))
    }
}
