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

/** Five structural layouts. All facts are supplied once, and each branch assigns them one owner. */
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
            when (style) {
                HomeLayoutStyle.Circle -> {
                    StatusPanel(state.status, style, onInstall, onAuthenticate, showDetail)
                    warnings()
                    if (state.showCounts) CountPair(state, onSuperuser, onModules, horizontal = pair)
                    FactsPanel(state.facts, state.buildFacts, state.showBuildDetails, showDetail)
                }
                HomeLayoutStyle.Grid -> {
                    if (pair && state.showCounts) {
                        Row(Modifier.fillMaxWidth().height(IntrinsicSize.Min), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                            StatusPanel(state.status, style, onInstall, onAuthenticate, showDetail,
                                Modifier.weight(1.1f).fillMaxHeight())
                            Column(Modifier.weight(1f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                                CountTile(stringResource(R.string.home_authorized_apps), state.superuserCount, onSuperuser, Modifier.weight(1f))
                                CountTile(stringResource(R.string.home_installed_modules), state.moduleCount, onModules, Modifier.weight(1f))
                            }
                        }
                    } else {
                        StatusPanel(state.status, style, onInstall, onAuthenticate, showDetail)
                        if (state.showCounts) CountPair(state, onSuperuser, onModules, horizontal = false)
                    }
                    warnings()
                    FactsPanel(state.facts, state.buildFacts, state.showBuildDetails, showDetail)
                }
                HomeLayoutStyle.Focus -> {
                    AdaptiveBoardRow(wide, first = { modifier ->
                        Column(modifier, verticalArrangement = Arrangement.spacedBy(12.dp)) {
                            StatusPanel(state.status, style, onInstall, onAuthenticate, showDetail)
                            if (state.showCounts) CountPair(state, onSuperuser, onModules, horizontal = pair)
                        }
                    }, second = { modifier ->
                        FactsPanel(state.facts, state.buildFacts, state.showBuildDetails, showDetail, modifier)
                    })
                    warnings()
                    if (state.showMetrics) AdaptiveBoardRow(wide,
                        first = { DevicePanel(state.metrics, it) },
                        second = { StoragePanel(state.metrics, it) })
                }
                HomeLayoutStyle.Dashboard -> {
                    BoardCard {
                        StatusBody(state.status, style, onInstall, onAuthenticate, showDetail)
                        if (state.facts.isNotEmpty()) {
                            HorizontalDivider(Modifier.padding(vertical = 8.dp))
                            if (pair) {
                                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                                    state.facts.take(3).forEach { FactCell(it, showDetail, Modifier.weight(1f)) }
                                }
                            } else state.facts.take(3).forEach { FactRow(it, showDetail) }
                        }
                    }
                    warnings()
                    if (state.showCounts) CountPair(state, onSuperuser, onModules, horizontal = pair)
                    FactsPanel(state.facts.drop(3), state.buildFacts, state.showBuildDetails, showDetail)
                }
                HomeLayoutStyle.Stats -> {
                    StatusPanel(state.status, style, onInstall, onAuthenticate, showDetail)
                    warnings()
                    if (!state.showCounts && !state.showMetrics) {
                        FactsPanel(state.facts, state.buildFacts, state.showBuildDetails, showDetail)
                    } else AdaptiveBoardRow(wide, first = { modifier ->
                        Column(modifier, verticalArrangement = Arrangement.spacedBy(16.dp)) {
                            if (state.showCounts) {
                                BoardCard {
                                    PanelHeading(stringResource(R.string.home_layout_activity))
                                    AdaptiveBoardRow(pair,
                                        first = { CountFigure(stringResource(R.string.home_authorized_apps), state.superuserCount, onSuperuser, it) },
                                        second = { CountFigure(stringResource(R.string.home_installed_modules), state.moduleCount, onModules, it) })
                                }
                            }
                            if (state.showMetrics) StatsMonitor(state.metrics)
                        }
                    }, second = { modifier ->
                        FactsPanel(state.facts, state.buildFacts, state.showBuildDetails, showDetail, modifier)
                    })
                }
            }
            services()
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
private fun StatusPanel(status: HomeStatusPresentation, style: HomeLayoutStyle, install: () -> Unit,
    authenticate: () -> Unit, detail: (HomeFact) -> Unit, modifier: Modifier = Modifier) {
    BoardCard(modifier) { StatusBody(status, style, install, authenticate, detail) }
}

@Composable
private fun StatusBody(status: HomeStatusPresentation, style: HomeLayoutStyle, install: () -> Unit,
    authenticate: () -> Unit, detail: (HomeFact) -> Unit) {
    val tint = if (!status.installed || status.safeMode) MaterialTheme.colorScheme.tertiary else MaterialTheme.colorScheme.primary
    val icon = when { !status.installed -> Icons.Outlined.Download; status.safeMode -> Icons.Outlined.Shield; else -> Icons.Outlined.CheckCircle }
    if (style == HomeLayoutStyle.Circle) {
        Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Surface(Modifier.size(104.dp).border(2.dp, tint.copy(alpha = 0.35f), CircleShape), CircleShape,
                color = tint.copy(alpha = 0.08f)) {
                Box(contentAlignment = Alignment.Center) { YukiIcon(icon, null, Modifier.size(48.dp), tint = tint) }
            }
            Text(status.title, style = MaterialTheme.typography.headlineSmall, textAlign = TextAlign.Center)
            StatusVersion(status, detail)
            StatusBadges(status.badges)
            StatusActions(status, install, authenticate)
        }
    } else if (style == HomeLayoutStyle.Grid) {
        Column(Modifier.fillMaxWidth().heightIn(min = 240.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            YukiIcon(icon, null, Modifier.size(40.dp), tint = tint)
            Spacer(Modifier.height(12.dp))
            Text(status.title, style = MaterialTheme.typography.headlineSmall)
            StatusVersion(status, detail)
            StatusBadges(status.badges)
            StatusActions(status, install, authenticate)
        }
    } else {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(16.dp)) {
            YukiIcon(icon, null, Modifier.size(if (style == HomeLayoutStyle.Dashboard) 48.dp else 28.dp), tint = tint)
            Text(status.title, style = if (style == HomeLayoutStyle.Stats) MaterialTheme.typography.titleLarge else MaterialTheme.typography.headlineSmall,
                modifier = Modifier.weight(1f))
        }
        StatusVersion(status, detail)
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
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        TextButton(onClick = install) {
            Text(stringResource(if (status.installed) R.string.home_layout_manage_installation else R.string.install))
        }
        if (status.needsAuthentication) FilledTonalButton(onClick = authenticate) {
            Text(stringResource(R.string.superkey_auth_title))
        }
    }
}

@Composable
private fun CountPair(state: HomeLayoutState, superuser: () -> Unit, modules: () -> Unit, horizontal: Boolean) {
    AdaptiveBoardRow(horizontal,
        first = { CountTile(stringResource(R.string.home_authorized_apps), state.superuserCount, superuser, it) },
        second = { CountTile(stringResource(R.string.home_installed_modules), state.moduleCount, modules, it) })
}

@Composable
private fun CountTile(label: String, count: Int?, onClick: () -> Unit, modifier: Modifier = Modifier) {
    BoardCard(modifier) { CountFigure(label, count, onClick) }
}

@Composable
private fun CountFigure(label: String, count: Int?, onClick: () -> Unit, modifier: Modifier = Modifier) {
    Column(modifier.fillMaxWidth().heightIn(min = 72.dp).clickable(onClick = onClick).padding(vertical = 4.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(count?.let { NumberFormat.getIntegerInstance().format(it) } ?: "—", style = MaterialTheme.typography.headlineMedium,
            fontWeight = FontWeight.SemiBold)
        Text(label, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        if (count == null) Text(stringResource(R.string.home_reading_unavailable), style = MaterialTheme.typography.labelSmall)
    }
}

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
    Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).clickable(onClickLabel = stringResource(R.string.home_layout_detail_action, fact.label)) {
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
private fun FactCell(fact: HomeFact, detail: (HomeFact) -> Unit, modifier: Modifier) {
    Column(modifier.heightIn(min = 56.dp).clickable(onClickLabel = stringResource(R.string.home_layout_detail_action, fact.label)) { detail(fact) },
        verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(fact.label, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(fact.summary, style = MaterialTheme.typography.bodyMedium, maxLines = 2, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun DevicePanel(metrics: HomeDeviceMetrics?, modifier: Modifier = Modifier) {
    BoardCard(modifier) {
        PanelHeading(stringResource(R.string.home_layout_device))
        MetricReading(stringResource(R.string.home_battery), metrics?.batteryPercent?.let { NumberFormat.getPercentInstance().format(it / 100.0) },
            listOfNotNull(metrics?.batteryTemperatureC?.let(::temperature), metrics?.charging?.let {
                stringResource(if (it) R.string.home_charging else R.string.home_on_battery)
            }).joinToString(" · "))
        MetricReading(stringResource(R.string.home_cpu_temperature), metrics?.cpuTemperatureC?.let(::temperature),
            metrics?.cpuFrequencyKHz?.let { stringResource(R.string.home_cpu_frequency, number(it / 1000)) }.orEmpty())
        Uptime(metrics)
    }
}

@Composable
private fun StoragePanel(metrics: HomeDeviceMetrics?, modifier: Modifier = Modifier) {
    BoardCard(modifier) {
        PanelHeading(stringResource(R.string.home_layout_storage))
        CapacityReading(stringResource(R.string.home_memory_usage), metrics?.memory)
        CapacityReading(stringResource(R.string.home_storage_usage), metrics?.storage)
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
private fun StatsMonitor(metrics: HomeDeviceMetrics?) {
    BoardCard {
        PanelHeading(stringResource(R.string.home_layout_monitor))
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
        CapacityReading(stringResource(R.string.home_storage_usage), metrics?.storage)
        metrics?.cpuFrequencyKHz?.let { Text(stringResource(R.string.home_cpu_frequency, number(it / 1000)), style = MaterialTheme.typography.bodySmall) }
        Uptime(metrics)
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
