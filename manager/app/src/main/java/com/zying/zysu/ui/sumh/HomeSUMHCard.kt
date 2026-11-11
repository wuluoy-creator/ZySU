package com.zying.zysu.ui.sumh

import androidx.compose.foundation.border
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.HelpOutline
import androidx.compose.material.icons.outlined.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.*
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.component.HomeStatusCardHeader
import com.zying.zysu.ui.theme.getCardBorder
import com.zying.zysu.ui.theme.getCardColors
import com.zying.zysu.ui.theme.getCardShape
import com.zying.zysu.ui.theme.getCardElevation
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as Controller
import kotlinx.coroutines.CancellationException

/** Home owns the manager/version guard and its existing pull-to-refresh trigger. */
@Composable
internal fun HomeSUMHCard(
    modifier: Modifier = Modifier,
    canReadStatus: Boolean,
    refreshKey: Long,
) {
    val resources = LocalResources.current
    var loaded by remember { mutableStateOf<Controller.UiState?>(null) }
    var loading by remember { mutableStateOf(canReadStatus) }
    var loadError by remember { mutableStateOf<String?>(null) }
    var retry by remember { mutableIntStateOf(0) }

    LaunchedEffect(canReadStatus, refreshKey, retry) {
        if (!canReadStatus) {
            loaded = null
            loadError = null
            loading = false
            return@LaunchedEffect
        }
        loading = true
        loadError = null
        try {
            // Preserve the protocol value while the latest runtime state is being read.
            loaded = Controller.load()
            loadError = null
        } catch (error: Exception) {
            if (error is CancellationException) throw error
            loadError = resources.getString(
                R.string.sumh_toast_load_error,
                error.message ?: resources.getString(R.string.home_ksud_daemon_unknown),
            )
        } finally {
            loading = false
        }
    }

    SUMHStatusSummaryCard(
        modifier = modifier,
        status = loaded?.status.takeIf { canReadStatus },
        version = loaded?.version.takeIf { canReadStatus },
        systemInfo = loaded?.system.takeIf { canReadStatus },
        loading = loading,
        loadError = loadError.takeIf { canReadStatus },
        unavailableReason = if (canReadStatus) null else stringResource(R.string.home_sumh_requires_manager),
        refreshEnabled = canReadStatus && !loading,
        onRefresh = {
            if (canReadStatus && !loading) {
                loading = true
                retry++
            }
        },
    )
}

/** Compact home status; the dedicated status page can also show inline runtime details. */
@Composable
internal fun SUMHStatusSummaryCard(
    status: Controller.SUMHStatus?,
    version: String?,
    systemInfo: Controller.SystemInfo?,
    onRefresh: () -> Unit,
    modifier: Modifier = Modifier,
    loading: Boolean = false,
    loadError: String? = null,
    unavailableReason: String? = null,
    refreshEnabled: Boolean = true,
    includeRuntimeDetails: Boolean = false,
    includeKernel: Boolean = false,
) {
    val colors = MaterialTheme.colorScheme
    val warning = if (systemInfo?.sumhMismatch == true) {
        systemInfo.mismatchMessage ?: stringResource(R.string.sumh_mismatch_default)
    } else null
    val hasWarning = loadError != null || warning != null || status in setOf(
        Controller.SUMHStatus.KERNEL_TOO_OLD,
        Controller.SUMHStatus.MODULE_TOO_OLD,
    )
    val available = status == Controller.SUMHStatus.AVAILABLE && !hasWarning && unavailableReason == null
    val statusText = when {
        unavailableReason != null -> unavailableReason
        loading -> stringResource(R.string.home_status_loading)
        loadError != null -> stringResource(R.string.home_status_load_failed)
        status == Controller.SUMHStatus.KERNEL_TOO_OLD -> stringResource(R.string.sumh_status_kernel_too_old)
        status == Controller.SUMHStatus.MODULE_TOO_OLD -> stringResource(R.string.sumh_status_module_too_old)
        warning != null -> stringResource(R.string.home_status_protocol_mismatch)
        status == Controller.SUMHStatus.NOT_PRESENT -> stringResource(R.string.sumh_status_not_present)
        available -> stringResource(when (systemInfo?.viewsEnabled) {
            true -> R.string.home_sumh_views_on
            false -> R.string.home_sumh_views_off
            null -> R.string.home_sumh_views_unknown
        })
        else -> stringResource(R.string.home_ksud_daemon_unknown)
    }
    val statusColor = when {
        loading || unavailableReason != null -> colors.onSurfaceVariant
        hasWarning -> colors.error
        available && systemInfo?.viewsEnabled == true -> colors.primary
        else -> colors.onSurfaceVariant
    }
    // The source returns the SUMH kernel protocol, not a release or daemon version.
    val protocol = version?.takeUnless {
        it.isBlank() || it == "0" || it == "-" || it == "—" || it.equals("unknown", ignoreCase = true)
    }
    val protocolText = stringResource(R.string.home_sumh_protocol, protocol ?: "—")
    val protocolDescription = stringResource(
        R.string.home_sumh_protocol_description,
        protocol ?: stringResource(R.string.home_ksud_daemon_unknown),
    )
    val refreshLabel = when {
        loading -> stringResource(R.string.home_status_loading)
        unavailableReason != null -> unavailableReason
        else -> stringResource(R.string.home_sumh_refresh)
    }
    val titleDescription = stringResource(R.string.sumh_kernel_title)
    val failureDetails = if (status == Controller.SUMHStatus.KERNEL_TOO_OLD ||
        status == Controller.SUMHStatus.MODULE_TOO_OLD
    ) loadError else loadError ?: warning
    var showFailureDetails by rememberSaveable(failureDetails) { mutableStateOf(false) }
    val canRefresh = refreshEnabled && !loading && unavailableReason == null
    val cardColors = getCardColors(colors.surfaceContainerLow)

    ElevatedCard(
        onClick = onRefresh,
        enabled = canRefresh,
        modifier = modifier.heightIn(min = 136.dp)
            .border(getCardBorder(), getCardShape())
            .semantics {
                onClick(label = refreshLabel, action = null)
                stateDescription = statusText
            },
        shape = getCardShape(),
        colors = cardColors.copy(
            disabledContainerColor = cardColors.containerColor,
            disabledContentColor = cardColors.contentColor,
        ),
        elevation = getCardElevation(),
    ) {
        Column(
            Modifier.fillMaxWidth().padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            HomeStatusCardHeader("SUMH", titleDescription, Icons.Outlined.Memory, canRefresh)
            Text(
                protocolText,
                Modifier.fillMaxWidth()
                    .clearAndSetSemantics { contentDescription = protocolDescription },
                style = MaterialTheme.typography.bodyMedium,
            )
            Row(
                Modifier.fillMaxWidth()
                    .semantics(mergeDescendants = true) { liveRegion = LiveRegionMode.Polite },
                horizontalArrangement = Arrangement.spacedBy(6.dp),
                verticalAlignment = Alignment.Top,
            ) {
                if (loading) {
                    CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                } else YukiIcon(
                    when {
                        unavailableReason != null -> Icons.Outlined.Lock
                        hasWarning -> Icons.Outlined.Warning
                        available && systemInfo?.viewsEnabled == true -> Icons.Outlined.Visibility
                        available && systemInfo?.viewsEnabled == false -> Icons.Outlined.VisibilityOff
                        available -> Icons.Outlined.Layers
                        else -> Icons.AutoMirrored.Outlined.HelpOutline
                    },
                    null,
                    Modifier.size(16.dp),
                    tint = statusColor,
                )
                Text(statusText, style = MaterialTheme.typography.labelMedium, color = statusColor)
            }
            if (!loading && unavailableReason == null && failureDetails != null) {
                Column {
                    TextButton(
                        onClick = { showFailureDetails = !showFailureDetails },
                        modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                        contentPadding = PaddingValues(vertical = 6.dp),
                    ) {
                        Text(
                            stringResource(if (showFailureDetails) R.string.home_sumh_hide_error else R.string.home_sumh_show_error),
                            Modifier.weight(1f),
                            style = MaterialTheme.typography.labelMedium,
                        )
                        YukiIcon(
                            if (showFailureDetails) Icons.Outlined.KeyboardArrowUp else Icons.Outlined.KeyboardArrowDown,
                            null,
                            Modifier.size(20.dp),
                        )
                    }
                    if (showFailureDetails) {
                        Text(failureDetails, style = MaterialTheme.typography.bodySmall, color = colors.error)
                    }
                }
            }
            if (includeRuntimeDetails && !loading && loadError == null &&
                unavailableReason == null && systemInfo != null
            ) {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    HorizontalDivider(color = colors.outlineVariant)
                    SUMHRuntimeDetails(systemInfo, includeKernel)
                }
            }
        }
    }
}

@Composable
private fun SUMHRuntimeDetails(systemInfo: Controller.SystemInfo, includeKernel: Boolean) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        if (includeKernel) {
            RuntimeInfo(stringResource(R.string.sumh_info_kernel), systemValueText(systemInfo.kernel), monospace = true)
        }
        RuntimeInfo(
            stringResource(R.string.sumh_mirror_path),
            systemValueText(systemInfo.mountBase.ifBlank { "none" }),
            monospace = true,
        )
        if (systemInfo.activeMounts.isNotEmpty()) {
            RuntimeInfo(
                stringResource(R.string.sumh_info_active_mounts),
                systemInfo.activeMounts.joinToString("\n"),
                monospace = true,
            )
        }
        if (systemInfo.hooks.isNotBlank()) {
            RuntimeInfo(stringResource(R.string.sumh_kernel_hooks), systemInfo.hooks, monospace = true)
        }
        systemInfo.mountStats?.let {
            RuntimeInfo(stringResource(R.string.sumh_mount_total), it.totalMounts.toString())
            RuntimeInfo("OverlayFS", it.overlayfsMounts.toString())
        }
    }
}

@Composable
private fun RuntimeInfo(label: String, value: String, monospace: Boolean = false) {
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(label, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(
            value,
            style = MaterialTheme.typography.bodyMedium,
            fontFamily = if (monospace) FontFamily.Monospace else FontFamily.Default,
        )
    }
}

@Composable
private fun systemValueText(value: String): String = when {
    value.equals("none", ignoreCase = true) -> stringResource(R.string.sumh_mount_mode_none)
    value.isBlank() || value.equals("unknown", ignoreCase = true) || value == "-" || value == "—" -> stringResource(R.string.home_ksud_daemon_unknown)
    else -> value
}
