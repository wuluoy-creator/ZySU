package com.zying.zysu.ui.screen.home

import android.text.format.Formatter
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.NavigateNext
import androidx.compose.material.icons.outlined.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.theme.AppearancePreferences
import com.zying.zysu.ui.theme.getCardBorder
import com.zying.zysu.ui.theme.getCardColors
import com.zying.zysu.ui.theme.getCardElevation
import com.zying.zysu.ui.theme.getCardShape
import com.zying.zysu.ui.viewmodel.HomeCapacity
import com.zying.zysu.ui.viewmodel.HomeDeviceMetrics
import java.text.NumberFormat

/** Only the upper board changes layout. Services and the complete facts card have one owner. */
@Composable
internal fun HomeLayoutContent(
    style: HomeLayoutStyle,
    state: HomeLayoutState,
    onInstall: () -> Unit = {},
    onAuthenticate: () -> Unit = {},
    onSuperuser: () -> Unit = {},
    onModules: () -> Unit = {},
    services: @Composable () -> Unit = {},
    warnings: @Composable () -> Unit = {},
) {
    var detail by remember { mutableStateOf<HomeFact?>(null) }
    val showDetail: (HomeFact) -> Unit = { detail = it }
    val effectiveFont = LocalDensity.current.fontScale * AppearancePreferences.fontScale
    BoxWithConstraints(Modifier.fillMaxWidth().testTag("home-layout-${style.id}")) {
        val wide = maxWidth >= 640.dp && effectiveFont < 1.5f
        val pair = maxWidth >= 320.dp && effectiveFont < 1.3f
        Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
            Column(Modifier.fillMaxWidth().testTag("home-upper-${style.id}"), verticalArrangement = Arrangement.spacedBy(16.dp)) {
                when (style) {
                    HomeLayoutStyle.Circle -> {
                        CircleStatusCard(state.status, onInstall, onAuthenticate, showDetail)
                        if (state.showCounts) CountPair(state, onSuperuser, onModules, horizontal = pair)
                    }
                    HomeLayoutStyle.Grid -> {
                        if (pair && state.showCounts) {
                            Row(Modifier.fillMaxWidth().height(IntrinsicSize.Min), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                                GridStatusCard(state.status, onInstall, onAuthenticate, showDetail, Modifier.weight(1f).fillMaxHeight())
                                Column(Modifier.weight(1f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                                    CountTile(Icons.Outlined.Person, stringResource(R.string.home_authorized_apps), state.superuserCount, onSuperuser,
                                        Modifier.weight(1f).testTag("home-count-superuser"), valueEmphasis = true)
                                    CountTile(Icons.Outlined.Extension, stringResource(R.string.home_installed_modules), state.moduleCount, onModules,
                                        Modifier.weight(1f).testTag("home-count-modules"), valueEmphasis = true)
                                }
                            }
                        } else {
                            GridStatusCard(state.status, onInstall, onAuthenticate, showDetail)
                            if (state.showCounts) CountPair(state, onSuperuser, onModules, horizontal = false, valueEmphasis = true)
                        }
                    }
                    HomeLayoutStyle.Focus -> {
                        if (wide && state.showCounts) AdaptiveBoardRow(true,
                            first = { FocusStatusCard(state.status, onInstall, onAuthenticate, showDetail, it) },
                            second = { CountSummaryPanel(state, onSuperuser, onModules, it) })
                        else {
                            FocusStatusCard(state.status, onInstall, onAuthenticate, showDetail)
                            if (state.showCounts) CountSummaryPanel(state, onSuperuser, onModules)
                        }
                        if (state.showMetrics) AdaptiveBoardRow(wide,
                            first = { DevicePanel(state.metrics, pair, it) },
                            second = { StoragePanel(state.metrics, it) })
                    }
                    HomeLayoutStyle.Dashboard -> {
                        DashboardStatusCard(state.status, onInstall, onAuthenticate, showDetail)
                        if (state.showCounts) CountPair(state, onSuperuser, onModules, horizontal = pair, valueEmphasis = true)
                    }
                    HomeLayoutStyle.Stats -> {
                        StatsStatusCard(state.status, onInstall, onAuthenticate, showDetail, pair)
                        if (wide && state.showMetrics && state.showCounts) AdaptiveBoardRow(true,
                            first = { StatsMonitor(state.metrics, it) },
                            second = { StatsCounts(state, onSuperuser, onModules, pair, it) })
                        else {
                            if (state.showMetrics) StatsMonitor(state.metrics)
                            if (state.showCounts) StatsCounts(state, onSuperuser, onModules, pair)
                        }
                    }
                }
            }
            warnings()
            Box(Modifier.fillMaxWidth().testTag("home-services")) { services() }
            FactsPanel(state.facts, state.buildFacts, state.showBuildDetails, showDetail, Modifier.testTag("home-core-facts"))
        }
    }
    detail?.let { fact ->
        YukiAlertDialog(
            onDismissRequest = { detail = null },
            title = { Text(fact.label) },
            text = { SelectionContainer { Text(fact.value, Modifier.verticalScroll(rememberScrollState())) } },
            confirmButton = { TextButton(onClick = { detail = null }) { Text(stringResource(android.R.string.ok)) } },
        )
    }
}

@Composable
private fun BoardCard(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    ElevatedCard(
        modifier = modifier.fillMaxWidth().border(getCardBorder(), getCardShape()), shape = getCardShape(),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow), elevation = getCardElevation(),
    ) {
        Column(Modifier.fillMaxWidth().padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp), content = content)
    }
}

@Composable
private fun PanelHeading(title: String) {
    Text(title, style = MaterialTheme.typography.titleMedium, modifier = Modifier.semantics { heading() })
}

@Composable
private fun AdaptiveBoardRow(wide: Boolean, first: @Composable (Modifier) -> Unit, second: @Composable (Modifier) -> Unit) {
    if (wide) Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
        first(Modifier.weight(1f)); second(Modifier.weight(1f))
    } else Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        first(Modifier.fillMaxWidth()); second(Modifier.fillMaxWidth())
    }
}

@Composable
private fun StatusGlyph(status: HomeStatusPresentation, modifier: Modifier = Modifier) {
    val tint = if (!status.installed || status.safeMode) MaterialTheme.colorScheme.tertiary else MaterialTheme.colorScheme.primary
    val icon = when { !status.installed -> Icons.Outlined.Download; status.safeMode -> Icons.Outlined.Shield; else -> Icons.Outlined.CheckCircle }
    YukiIcon(icon, null, modifier, tint = tint)
}

/** FinaSU's Circle layout uses a compact icon-and-summary row, not a giant status medallion. */
@Composable
private fun CircleStatusCard(status: HomeStatusPresentation, install: () -> Unit,
    authenticate: () -> Unit, detail: (HomeFact) -> Unit) {
    BoardCard(Modifier.testTag("home-status-card")) {
        Row(Modifier.fillMaxWidth().testTag("home-status-circle"), verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(20.dp)) {
            StatusGlyph(status, Modifier.size(28.dp))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(status.title, style = MaterialTheme.typography.titleLarge)
                StatusVersion(status, detail)
                StatusBadges(status.badges)
            }
        }
        StatusActions(status, install, authenticate)
    }
}

/** The corner icon and bottom-aligned summary retain the tile geometry when counters are hidden. */
@Composable
private fun GridStatusCard(status: HomeStatusPresentation, install: () -> Unit,
    authenticate: () -> Unit, detail: (HomeFact) -> Unit, modifier: Modifier = Modifier) {
    ElevatedCard(modifier = modifier.fillMaxWidth().testTag("home-status-card").border(getCardBorder(), getCardShape()),
        shape = getCardShape(), colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow), elevation = getCardElevation()) {
        Box(Modifier.fillMaxWidth().heightIn(min = 220.dp).fillMaxHeight().padding(20.dp).testTag("home-status-grid")) {
            StatusGlyph(status, Modifier.align(Alignment.TopEnd).size(48.dp))
            Column(Modifier.fillMaxWidth().align(Alignment.BottomStart).padding(top = 64.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(status.title, style = MaterialTheme.typography.headlineSmall)
                StatusVersion(status, detail)
                StatusBadges(status.badges)
                StatusActions(status, install, authenticate)
            }
        }
    }
}

/** Focus follows the reference's titled tile and labelled status/version rows. */
@Composable
private fun FocusStatusCard(status: HomeStatusPresentation, install: () -> Unit,
    authenticate: () -> Unit, detail: (HomeFact) -> Unit, modifier: Modifier = Modifier) {
    BoardCard(modifier.testTag("home-status-card")) {
        Column(Modifier.fillMaxWidth().testTag("home-status-focus"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            TileHeading(stringResource(R.string.home_layout_status), Icons.Outlined.AdminPanelSettings)
            LabeledStatusValue(stringResource(R.string.home_layout_running_status), status.title)
            status.version?.let { fact ->
                LabeledStatusValue(fact.label, fact.summary, Modifier.clickable(
                    onClickLabel = stringResource(R.string.home_layout_detail_action, fact.label)) { detail(fact) })
            }
            StatusBadges(status.badges)
            StatusActions(status, install, authenticate)
        }
    }
}

@Composable
private fun LabeledStatusValue(label: String, value: String, modifier: Modifier = Modifier) {
    val stack = LocalDensity.current.fontScale * AppearancePreferences.fontScale >= 1.3f
    if (stack) Column(modifier.fillMaxWidth().heightIn(min = 48.dp).padding(vertical = 6.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(label, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, style = MaterialTheme.typography.bodyLarge)
    } else Row(modifier.fillMaxWidth().heightIn(min = 48.dp).padding(vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
        Text(label, modifier = Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, modifier = Modifier.weight(1f), style = MaterialTheme.typography.bodyLarge, textAlign = TextAlign.End)
    }
}

/** A wide hero with a dedicated module-version band; kernel/device/policy facts remain below services. */
@Composable
private fun DashboardStatusCard(status: HomeStatusPresentation, install: () -> Unit,
    authenticate: () -> Unit, detail: (HomeFact) -> Unit) {
    BoardCard(Modifier.testTag("home-status-card")) {
        Column(Modifier.fillMaxWidth().testTag("home-status-dashboard_ui").padding(vertical = 4.dp),
            verticalArrangement = Arrangement.spacedBy(20.dp)) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                StatusGlyph(status, Modifier.size(36.dp))
                Text(status.title, style = MaterialTheme.typography.headlineSmall, modifier = Modifier.weight(1f))
            }
            StatusBadges(status.badges)
            status.version?.let { fact ->
                HorizontalDivider()
                Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text(fact.label, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    StatusVersion(status, detail)
                }
            }
            StatusActions(status, install, authenticate)
        }
    }
}

/** A dense monitoring strip, followed by live waveforms and the count distribution. */
@Composable
private fun StatsStatusCard(status: HomeStatusPresentation, install: () -> Unit,
    authenticate: () -> Unit, detail: (HomeFact) -> Unit, horizontal: Boolean) {
    BoardCard(Modifier.testTag("home-status-card")) {
        Row(Modifier.fillMaxWidth().testTag("home-status-stats"), verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(16.dp)) {
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(stringResource(R.string.home_layout_running_status), style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                Text(status.title, style = MaterialTheme.typography.titleLarge)
            }
            StatusGlyph(status, Modifier.size(24.dp))
        }
        status.version?.let { fact ->
            HorizontalDivider()
            if (horizontal) Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                Text(fact.label, Modifier.weight(1f), style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Box(Modifier.weight(1f)) { StatusVersion(status, detail) }
            } else StatusVersion(status, detail)
        }
        StatusBadges(status.badges)
        StatusActions(status, install, authenticate)
    }
}

@Composable
private fun StatusVersion(status: HomeStatusPresentation, detail: (HomeFact) -> Unit) {
    status.version?.let { fact ->
        TextButton(onClick = { detail(fact) }, contentPadding = PaddingValues(horizontal = 0.dp, vertical = 4.dp)) {
            Text(fact.summary, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
            Spacer(Modifier.width(6.dp))
            YukiIcon(Icons.Outlined.Info, stringResource(R.string.home_layout_detail_action, fact.label), Modifier.size(16.dp))
        }
    }
}

@Composable
private fun StatusBadges(badges: List<String>) {
    if (badges.isNotEmpty()) FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        badges.distinct().forEach { badge ->
            Surface(shape = CircleShape, color = MaterialTheme.colorScheme.secondaryContainer) {
                Text(badge, Modifier.padding(horizontal = 10.dp, vertical = 4.dp), style = MaterialTheme.typography.labelMedium)
            }
        }
    }
}

@Composable
private fun StatusActions(status: HomeStatusPresentation, install: () -> Unit, authenticate: () -> Unit) {
    if (status.installed && !status.needsAuthentication) return
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        if (!status.installed) FilledTonalButton(onClick = install, modifier = Modifier.testTag("home-install-action")) {
            Text(stringResource(R.string.install))
        }
        if (status.needsAuthentication) FilledTonalButton(onClick = authenticate, modifier = Modifier.testTag("home-authenticate-action")) {
            Text(stringResource(R.string.superkey_auth_title))
        }
    }
}

@Composable
private fun CountPair(state: HomeLayoutState, superuser: () -> Unit, modules: () -> Unit, horizontal: Boolean, valueEmphasis: Boolean = false) {
    AdaptiveBoardRow(horizontal,
        first = { CountTile(Icons.Outlined.Person, stringResource(R.string.home_authorized_apps), state.superuserCount, superuser, it.testTag("home-count-superuser"), valueEmphasis) },
        second = { CountTile(Icons.Outlined.Extension, stringResource(R.string.home_installed_modules), state.moduleCount, modules, it.testTag("home-count-modules"), valueEmphasis) })
}

@Composable
private fun CountTile(icon: ImageVector, label: String, count: Int?, onClick: () -> Unit, modifier: Modifier = Modifier, valueEmphasis: Boolean = false) {
    BoardCard(modifier.clickable(onClick = onClick)) {
        Row(Modifier.fillMaxWidth().heightIn(min = 72.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            YukiIcon(icon, null, Modifier.size(20.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(label, style = if (valueEmphasis) MaterialTheme.typography.labelMedium else MaterialTheme.typography.bodyMedium,
                    color = if (valueEmphasis) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.onSurface)
                Text(countText(count), style = if (valueEmphasis) MaterialTheme.typography.headlineMedium else MaterialTheme.typography.titleLarge,
                    color = if (valueEmphasis) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurfaceVariant, fontWeight = FontWeight.Medium)
                if (count == null) Text(stringResource(R.string.home_reading_unavailable), style = MaterialTheme.typography.labelSmall)
            }
        }
    }
}

@Composable
private fun CountSummaryPanel(state: HomeLayoutState, superuser: () -> Unit, modules: () -> Unit, modifier: Modifier = Modifier) {
    BoardCard(modifier) {
        TileHeading(stringResource(R.string.home_layout_activity), Icons.Outlined.Extension)
        LabeledStatusValue(stringResource(R.string.home_authorized_apps), countText(state.superuserCount),
            Modifier.testTag("home-count-superuser").clickable(onClick = superuser))
        LabeledStatusValue(stringResource(R.string.home_installed_modules), countText(state.moduleCount),
            Modifier.testTag("home-count-modules").clickable(onClick = modules))
    }
}

@Composable
private fun TileHeading(title: String, icon: ImageVector) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        YukiIcon(icon, null, Modifier.size(20.dp), tint = MaterialTheme.colorScheme.primary)
        PanelHeading(title)
    }
}

private fun countText(value: Int?): String = value?.let { NumberFormat.getIntegerInstance().format(it) } ?: "—"

@Composable
private fun FactsPanel(facts: List<HomeFact>, buildFacts: List<HomeFact>, showBuild: Boolean,
    detail: (HomeFact) -> Unit, modifier: Modifier = Modifier) {
    if (facts.isEmpty() && buildFacts.isEmpty()) return
    var expanded by remember(showBuild) { mutableStateOf(showBuild) }
    BoardCard(modifier) {
        PanelHeading(stringResource(R.string.home_layout_essential))
        facts.forEach { FactRow(it, detail) }
        if (buildFacts.isNotEmpty()) {
            TextButton(onClick = { expanded = !expanded }, modifier = Modifier.fillMaxWidth()) {
                Text(stringResource(R.string.home_layout_build_details), modifier = Modifier.weight(1f), textAlign = TextAlign.Start)
                YukiIcon(if (expanded) Icons.Outlined.ExpandLess else Icons.Outlined.ExpandMore, null)
            }
            if (expanded) buildFacts.forEach { FactRow(it, detail) }
        }
    }
}

@Composable
private fun FactRow(fact: HomeFact, detail: (HomeFact) -> Unit) {
    Row(Modifier.fillMaxWidth().testTag("home-fact-${fact.id}").heightIn(min = 48.dp).clickable(onClickLabel = stringResource(R.string.home_layout_detail_action, fact.label)) {
        detail(fact)
    }.padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(fact.label, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(fact.summary, style = MaterialTheme.typography.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        YukiIcon(Icons.AutoMirrored.Filled.NavigateNext, null, Modifier.size(18.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun DevicePanel(metrics: HomeDeviceMetrics?, horizontal: Boolean, modifier: Modifier = Modifier) {
    BoardCard(modifier.testTag("home-focus-device")) {
        TileHeading(stringResource(R.string.home_layout_device), Icons.Outlined.Memory)
        val colors = MaterialTheme.colorScheme
        val readings: @Composable (Modifier) -> Unit = { itemModifier ->
            MetricCircle(metrics?.batteryTemperatureC?.let(::temperature), stringResource(R.string.home_layout_battery_temperature),
                metrics?.batteryTemperatureC?.div(50f), colors.primary, horizontal, itemModifier)
            MetricCircle(metrics?.cpuTemperatureC?.let(::temperature), stringResource(R.string.home_cpu_temperature),
                metrics?.cpuTemperatureC?.div(80f), colors.secondary, horizontal, itemModifier)
            MetricCircle(metrics?.batteryPercent?.let { NumberFormat.getPercentInstance().format(it / 100.0) }, stringResource(R.string.home_battery),
                metrics?.batteryPercent?.div(100f), colors.tertiary, horizontal, itemModifier)
        }
        if (horizontal) Row(Modifier.fillMaxWidth().padding(vertical = 8.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            readings(Modifier.weight(1f))
        } else Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(16.dp)) { readings(Modifier.fillMaxWidth()) }
        metrics?.charging?.let { Text(stringResource(if (it) R.string.home_charging else R.string.home_on_battery),
            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant) }
        metrics?.cpuFrequencyKHz?.let { Text(stringResource(R.string.home_cpu_frequency, number(it / 1000)), style = MaterialTheme.typography.bodySmall) }
        Uptime(metrics)
    }
}

/** Temperature rings use FinaSU's 50°C/80°C display ranges; these are scales, not health thresholds. */
@Composable
private fun MetricCircle(value: String?, label: String, progress: Float?, color: Color, compact: Boolean, modifier: Modifier) {
    val ring: @Composable () -> Unit = {
        Box(Modifier.size(if (compact) 80.dp else 96.dp), contentAlignment = Alignment.Center) {
            CircularProgressIndicator(progress = { (progress ?: 0f).coerceIn(0f, 1f) }, modifier = Modifier.fillMaxSize(),
                color = color, trackColor = color.copy(alpha = 0.16f), strokeWidth = 4.dp)
            if (compact) Text(value ?: "—", style = MaterialTheme.typography.titleMedium, textAlign = TextAlign.Center)
        }
    }
    if (compact) Column(modifier, horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(8.dp)) {
        ring()
        Text(label, style = MaterialTheme.typography.labelMedium, textAlign = TextAlign.Center, color = MaterialTheme.colorScheme.onSurfaceVariant)
        if (value == null) Text(stringResource(R.string.home_reading_unavailable), style = MaterialTheme.typography.labelSmall, textAlign = TextAlign.Center)
    } else Row(modifier, verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
        ring()
        Column(Modifier.weight(1f)) {
            Text(label, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(value ?: stringResource(R.string.home_reading_unavailable), style = MaterialTheme.typography.titleLarge)
        }
    }
}

@Composable
private fun StoragePanel(metrics: HomeDeviceMetrics?, modifier: Modifier = Modifier) {
    BoardCard(modifier.testTag("home-focus-storage")) {
        TileHeading(stringResource(R.string.home_layout_storage), Icons.Outlined.SdStorage)
        CapacityReading(stringResource(R.string.home_storage_usage), metrics?.storage)
        CapacityReading(stringResource(R.string.home_memory_usage), metrics?.memory)
    }
}

@Composable
private fun MetricReading(label: String, value: String?, supporting: String = "") {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(label, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value ?: stringResource(R.string.home_reading_unavailable), style = MaterialTheme.typography.titleLarge)
        if (supporting.isNotBlank()) Text(supporting, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun CapacityReading(label: String, capacity: HomeCapacity?) {
    val context = LocalContext.current
    MetricReading(label, capacity?.let { NumberFormat.getPercentInstance().format(it.fraction.toDouble()) }, capacity?.let {
        stringResource(R.string.home_capacity_value, Formatter.formatShortFileSize(context, it.usedBytes), Formatter.formatShortFileSize(context, it.totalBytes))
    }.orEmpty())
    capacity?.let { LinearProgressIndicator(progress = { it.fraction }, modifier = Modifier.fillMaxWidth()) }
}

@Composable
private fun Uptime(metrics: HomeDeviceMetrics?) {
    metrics?.let {
        Text(stringResource(R.string.home_uptime_value, it.uptimeMillis / 86_400_000L,
            it.uptimeMillis / 3_600_000L % 24, it.uptimeMillis / 60_000L % 60),
            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun StatsMonitor(metrics: HomeDeviceMetrics?, modifier: Modifier = Modifier) {
    BoardCard(modifier.testTag("home-stats-monitor")) {
        TileHeading(stringResource(R.string.home_layout_monitor), Icons.Outlined.Speed)
        MetricReading(stringResource(R.string.home_cpu_temperature), metrics?.cpuTemperatureC?.let(::temperature))
        TrendLine(metrics?.cpuTemperatureHistory.orEmpty(), MaterialTheme.colorScheme.tertiary)
        CapacityReading(stringResource(R.string.home_memory_usage), metrics?.memory)
        // Plot percentage points so a one-point change has the same minimum scale as temperature.
        TrendLine(metrics?.memoryHistory.orEmpty().map { it?.times(100f) }, MaterialTheme.colorScheme.primary)
        Text(stringResource(R.string.home_layout_trend_window), style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        HorizontalDivider()
        MetricReading(stringResource(R.string.home_battery), metrics?.batteryPercent?.let { NumberFormat.getPercentInstance().format(it / 100.0) },
            metrics?.batteryTemperatureC?.let(::temperature).orEmpty())
        metrics?.batteryPercent?.let { LinearProgressIndicator(progress = { it / 100f }, modifier = Modifier.fillMaxWidth()) }
        CapacityReading(stringResource(R.string.home_storage_usage), metrics?.storage)
        metrics?.cpuFrequencyKHz?.let { Text(stringResource(R.string.home_cpu_frequency, number(it / 1000)), style = MaterialTheme.typography.bodySmall) }
        Uptime(metrics)
    }
}

/** FinaSU's donut/legend geometry, using only the two real record categories available in ZySU. */
@Composable
private fun StatsCounts(state: HomeLayoutState, superuser: () -> Unit, modules: () -> Unit, horizontal: Boolean, modifier: Modifier = Modifier) {
    val distribution = HomeCountDistribution.fromCounts(state.superuserCount, state.moduleCount)
    val primary = MaterialTheme.colorScheme.primary
    val tertiary = MaterialTheme.colorScheme.tertiary
    val track = MaterialTheme.colorScheme.surfaceContainerHighest
    val chart: @Composable () -> Unit = {
        Box(Modifier.size(128.dp).testTag("home-count-chart"), contentAlignment = Alignment.Center) {
            Canvas(Modifier.fillMaxSize().padding(8.dp)) {
                val stroke = Stroke(18.dp.toPx())
                drawArc(track, -90f, 360f, false, style = stroke)
                if (distribution != null && distribution.total > 0) {
                    val modulesSweep = distribution.moduleFraction * 360f
                    drawArc(primary, -90f, modulesSweep, false, style = stroke)
                    drawArc(tertiary, -90f + modulesSweep, 360f - modulesSweep, false, style = stroke)
                }
            }
            YukiIcon(Icons.Outlined.PieChart, null, Modifier.size(28.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
    val legend: @Composable (Modifier) -> Unit = { legendModifier ->
        Column(legendModifier, verticalArrangement = Arrangement.spacedBy(12.dp)) {
            CountLegend(stringResource(R.string.home_installed_modules), state.moduleCount, primary, modules, "home-count-modules")
            CountLegend(stringResource(R.string.home_authorized_apps), state.superuserCount, tertiary, superuser, "home-count-superuser")
        }
    }
    BoardCard(modifier.testTag("home-stats-counts")) {
        TileHeading(stringResource(R.string.home_layout_activity), Icons.Outlined.PieChart)
        if (horizontal) Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
            chart()
            legend(Modifier.weight(1f))
        } else Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(16.dp)) {
            chart()
            legend(Modifier.fillMaxWidth())
        }
    }
}

@Composable
private fun CountLegend(label: String, count: Int?, color: Color, onClick: () -> Unit, tag: String) {
    Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag(tag).clickable(onClick = onClick),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Surface(Modifier.size(10.dp), shape = CircleShape, color = color) {}
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(label, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(countText(count), style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Medium)
            if (count == null) Text(stringResource(R.string.home_reading_unavailable), style = MaterialTheme.typography.labelSmall)
        }
    }
}

/** Missing readings create gaps, and no historical values are fabricated before collection. */
@Composable
private fun TrendLine(samples: List<Float?>, color: Color) {
    if (samples.none { it != null }) return
    if (samples.count { it != null } < 2) {
        Text(stringResource(R.string.home_layout_trend_waiting), style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        return
    }
    Canvas(Modifier.fillMaxWidth().height(48.dp)) {
        val readings = samples.filterNotNull()
        val low = readings.minOrNull() ?: 0f
        val range = ((readings.maxOrNull() ?: 1f) - low).coerceAtLeast(1f)
        val path = Path()
        var connected = false
        samples.forEachIndexed { index, value ->
            if (value == null) connected = false else {
                val point = Offset(index * size.width / (samples.size - 1), size.height - 4.dp.toPx() - (value - low) / range * (size.height - 8.dp.toPx()))
                if (connected) path.lineTo(point.x, point.y) else path.moveTo(point.x, point.y)
                connected = true
            }
        }
        drawPath(path, color, style = Stroke(width = 2.dp.toPx()))
    }
}

private fun number(value: Number): String = NumberFormat.getNumberInstance().apply { maximumFractionDigits = 1 }.format(value)
private fun temperature(value: Float): String = "${number(value)} °C"
