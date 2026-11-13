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
import com.zying.zysu.ui.component.HomeMainCardArtwork
import com.zying.zysu.ui.component.HomeServiceCardContent
import com.zying.zysu.ui.component.HomeServiceStatusRow
import com.zying.zysu.ui.component.HomeServiceValueRow
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
                        HomeMainStatusCard(style, state.status, onInstall = onInstall, onAuthenticate = onAuthenticate, onDetails = showDetail)
                        ServicePair(pair, daemon, kernel)
                    }
                    HomeLayoutStyle.Grid -> {
                        if (pair) {
                            Row(Modifier.fillMaxWidth().height(IntrinsicSize.Min), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                                HomeMainStatusCard(style, state.status, Modifier.weight(0.9f).fillMaxHeight(), onInstall, onAuthenticate, showDetail,
                                    fillTileHeight = true)
                                Column(Modifier.weight(1.1f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                                    ServiceSlot("home-service-ksud", Modifier.weight(1f).fillMaxWidth(), daemon)
                                    ServiceSlot("home-service-sumh", Modifier.weight(1f).fillMaxWidth(), kernel)
                                }
                            }
                        } else {
                            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                HomeMainStatusCard(style, state.status, onInstall = onInstall, onAuthenticate = onAuthenticate, onDetails = showDetail)
                                ServiceSlot("home-service-ksud", Modifier.fillMaxWidth(), daemon)
                                ServiceSlot("home-service-sumh", Modifier.fillMaxWidth(), kernel)
                            }
                        }
                    }
                    HomeLayoutStyle.Focus -> {
                        if (wide) {
                            Row(Modifier.fillMaxWidth().height(IntrinsicSize.Min), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                                HomeMainStatusCard(style, state.status, Modifier.weight(1f).fillMaxHeight(), onInstall, onAuthenticate, showDetail)
                                ServiceSlot("home-service-ksud", Modifier.weight(1f).fillMaxHeight(), daemon)
                            }
                            ServiceSlot("home-service-sumh", Modifier.fillMaxWidth(), kernel)
                        } else {
                            HomeMainStatusCard(style, state.status, onInstall = onInstall, onAuthenticate = onAuthenticate, onDetails = showDetail)
                            Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(16.dp)) {
                                ServiceSlot("home-service-ksud", Modifier.fillMaxWidth(), daemon)
                                ServiceSlot("home-service-sumh", Modifier.fillMaxWidth(), kernel)
                            }
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

/**
 * Shared production/preview main card. Its rows use the same 16/8/32/48 dp rhythm as KSUD/SUMH.
 * Artwork is decorative and never participates in hit testing or the content's measurement.
 */
@Composable
internal fun HomeMainStatusCard(
    style: HomeLayoutStyle,
    status: HomeStatusPresentation,
    modifier: Modifier = Modifier,
    onInstall: () -> Unit = {},
    onAuthenticate: () -> Unit = {},
    onDetails: (HomeFact) -> Unit = {},
    fillTileHeight: Boolean = false,
) {
    val shape = getCardShape()
    val color = if (!status.installed || status.safeMode) MaterialTheme.colorScheme.tertiary else MaterialTheme.colorScheme.primary
    val statusIcon = when {
        !status.installed -> Icons.Outlined.Download
        status.safeMode -> Icons.Outlined.Shield
        else -> Icons.Outlined.CheckCircle
    }
    val actionModifier = if (status.installed && !status.needsAuthentication) {
        Modifier.clickable(onClickLabel = stringResource(R.string.install), role = Role.Button, onClick = onInstall)
    } else Modifier
    ElevatedCard(
        modifier = modifier.fillMaxWidth().testTag("home-status-card").clip(shape).then(actionModifier).border(getCardBorder(), shape),
        shape = shape, colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow), elevation = getCardElevation(),
    ) {
        Box(Modifier.fillMaxWidth().then(if (fillTileHeight) Modifier.fillMaxHeight() else Modifier)
            .testTag("home-status-${style.id}"), propagateMinConstraints = true) {
            HomeMainCardArtwork(Modifier.matchParentSize())
            HomeServiceCardContent(
                title = stringResource(R.string.app_name),
                description = stringResource(R.string.home_layout_kernel_module),
                icon = Icons.Outlined.Security,
                enabled = status.installed,
                tag = "main",
                value = {
                    status.version?.let { fact ->
                        HomeServiceValueRow(
                            label = stringResource(R.string.service_card_version),
                            value = fact.summary,
                            description = stringResource(R.string.home_layout_detail_action, fact.label),
                            tag = "main", onDetails = { onDetails(fact) },
                        )
                    }
                },
                status = {
                    HomeServiceStatusRow(text = status.title, color = color, tag = "main") {
                        YukiIcon(statusIcon, null, Modifier.size(16.dp), tint = color)
                    }
                },
                supporting = {
                    StatusBadges(status.badges)
                    StatusActions(status, onInstall, onAuthenticate)
                },
            )
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
