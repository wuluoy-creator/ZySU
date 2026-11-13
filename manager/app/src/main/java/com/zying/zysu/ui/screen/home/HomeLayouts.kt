package com.zying.zysu.ui.screen.home

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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.semantics
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

/** The status and two service slots form each layout; the complete facts card stays below them. */
@Composable
internal fun HomeLayoutContent(
    style: HomeLayoutStyle,
    state: HomeLayoutState,
    onInstall: () -> Unit = {},
    onAuthenticate: () -> Unit = {},
    daemon: @Composable (Modifier) -> Unit = {},
    kernel: @Composable (Modifier) -> Unit = {},
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
                        ServicePair(pair, daemon, kernel)
                    }
                    HomeLayoutStyle.Grid -> {
                        if (pair) {
                            Row(Modifier.fillMaxWidth().height(IntrinsicSize.Min), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                                GridStatusCard(state.status, onInstall, onAuthenticate, showDetail, Modifier.weight(0.9f).fillMaxHeight())
                                Column(Modifier.weight(1.1f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                                    ServiceSlot("home-service-ksud", Modifier.weight(1f).fillMaxWidth(), daemon)
                                    ServiceSlot("home-service-sumh", Modifier.weight(1f).fillMaxWidth(), kernel)
                                }
                            }
                        } else {
                            GridStatusCard(state.status, onInstall, onAuthenticate, showDetail)
                            ServicePair(false, daemon, kernel)
                        }
                    }
                    HomeLayoutStyle.Focus -> {
                        if (wide) {
                            Row(Modifier.fillMaxWidth().height(IntrinsicSize.Min), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                                FocusStatusCard(state.status, onInstall, onAuthenticate, showDetail, Modifier.weight(1f).fillMaxHeight())
                                ServiceSlot("home-service-ksud", Modifier.weight(1f).fillMaxHeight(), daemon)
                            }
                            ServiceSlot("home-service-sumh", Modifier.fillMaxWidth(), kernel)
                        } else {
                            FocusStatusCard(state.status, onInstall, onAuthenticate, showDetail)
                            Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(16.dp)) {
                                ServiceSlot("home-service-ksud", Modifier.fillMaxWidth(), daemon)
                                ServiceSlot("home-service-sumh", Modifier.fillMaxWidth(), kernel)
                            }
                        }
                    }
                    HomeLayoutStyle.Dashboard -> {
                        DashboardStatusCard(state.status, onInstall, onAuthenticate, showDetail)
                        ServicePair(pair, daemon, kernel)
                    }
                    HomeLayoutStyle.Stats -> {
                        StatsStatusCard(state.status, onInstall, onAuthenticate, showDetail, pair)
                        Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            ServiceSlot("home-service-ksud", Modifier.fillMaxWidth(), daemon)
                            ServiceSlot("home-service-sumh", Modifier.fillMaxWidth(), kernel)
                        }
                    }
                }
            }
            warnings()
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
private fun ServicePair(horizontal: Boolean, daemon: @Composable (Modifier) -> Unit, kernel: @Composable (Modifier) -> Unit) {
    if (horizontal) Row(Modifier.fillMaxWidth().height(IntrinsicSize.Min), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        ServiceSlot("home-service-ksud", Modifier.weight(1f).fillMaxHeight(), daemon)
        ServiceSlot("home-service-sumh", Modifier.weight(1f).fillMaxHeight(), kernel)
    } else Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        ServiceSlot("home-service-ksud", Modifier.fillMaxWidth(), daemon)
        ServiceSlot("home-service-sumh", Modifier.fillMaxWidth(), kernel)
    }
}

/** Preserve the assigned tile bounds through to the actual service card. */
@Composable
private fun ServiceSlot(tag: String, modifier: Modifier, content: @Composable (Modifier) -> Unit) {
    Box(modifier.testTag(tag), propagateMinConstraints = true) {
        content(Modifier.fillMaxWidth())
    }
}

@Composable
private fun StatusGlyph(status: HomeStatusPresentation, modifier: Modifier = Modifier) {
    val tint = if (!status.installed || status.safeMode) MaterialTheme.colorScheme.tertiary else MaterialTheme.colorScheme.primary
    val icon = when { !status.installed -> Icons.Outlined.Download; status.safeMode -> Icons.Outlined.Shield; else -> Icons.Outlined.CheckCircle }
    YukiIcon(icon, null, modifier, tint = tint)
}

/** Installed cards retain the familiar install/upgrade route without adding another visible button. */
@Composable
private fun Modifier.installedModuleAction(status: HomeStatusPresentation, install: () -> Unit): Modifier =
    if (status.installed && !status.needsAuthentication) {
        clip(getCardShape()).clickable(onClickLabel = stringResource(R.string.install), role = Role.Button, onClick = install)
    } else this

/** FinaSU's Circle layout uses a compact icon-and-summary row, not a giant status medallion. */
@Composable
private fun CircleStatusCard(status: HomeStatusPresentation, install: () -> Unit,
    authenticate: () -> Unit, detail: (HomeFact) -> Unit) {
    BoardCard(Modifier.testTag("home-status-card").installedModuleAction(status, install)) {
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

/** The corner icon and bottom-aligned summary balance the two service tiles beside them. */
@Composable
private fun GridStatusCard(status: HomeStatusPresentation, install: () -> Unit,
    authenticate: () -> Unit, detail: (HomeFact) -> Unit, modifier: Modifier = Modifier) {
    ElevatedCard(modifier = modifier.fillMaxWidth().testTag("home-status-card").installedModuleAction(status, install).border(getCardBorder(), getCardShape()),
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
    BoardCard(modifier.testTag("home-status-card").installedModuleAction(status, install)) {
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
    BoardCard(Modifier.testTag("home-status-card").installedModuleAction(status, install)) {
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

/** A compact running-status strip above the stacked service summaries. */
@Composable
private fun StatsStatusCard(status: HomeStatusPresentation, install: () -> Unit,
    authenticate: () -> Unit, detail: (HomeFact) -> Unit, horizontal: Boolean) {
    BoardCard(Modifier.testTag("home-status-card").installedModuleAction(status, install)) {
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
private fun TileHeading(title: String, icon: ImageVector) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        YukiIcon(icon, null, Modifier.size(20.dp), tint = MaterialTheme.colorScheme.primary)
        PanelHeading(title)
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
