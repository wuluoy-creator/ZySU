package com.zying.zysu.ui.component

import android.content.res.Configuration
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.HelpOutline
import androidx.compose.material.icons.outlined.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.integrity.KsudIntegrityStatus
import com.zying.zysu.ui.theme.UtilityPreviewTheme
import com.zying.zysu.ui.theme.getCardColors
import com.zying.zysu.ui.theme.getCardShape
import com.zying.zysu.ui.theme.getCardBorder
import com.zying.zysu.ui.theme.getCardElevation

/** Tap the card to sync; status and progress remain visible alongside the version. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun KsudStatusCard(
    bundledVersion: String?,
    installedVersion: String?,
    integrity: KsudIntegrityStatus,
    loading: Boolean,
    syncing: Boolean,
    syncResult: Boolean?,
    syncUnavailableReason: String?,
    onSync: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = MaterialTheme.colorScheme
    val verified = integrity == KsudIntegrityStatus.MATCH && installedVersion != null
    val mismatch = integrity == KsudIntegrityStatus.MISMATCH
    val syncFailed = syncResult == false
    val status = stringResource(when {
        syncing -> R.string.home_ksud_daemon_syncing
        loading -> R.string.home_ksud_checking
        syncFailed -> R.string.home_ksud_sync_failed
        mismatch -> R.string.home_ksud_mismatch
        verified && syncResult == true -> R.string.home_ksud_sync_success
        verified -> R.string.home_ksud_verified
        else -> R.string.home_ksud_unverified
    })
    val statusColor = when {
        syncing || loading -> colors.onSurfaceVariant
        syncFailed || mismatch -> colors.error
        verified -> colors.primary
        else -> colors.onSurfaceVariant
    }
    val titleDescription = stringResource(R.string.home_ksud_daemon_title)
    val version = (if (verified) installedVersion else null) ?: bundledVersion
    val versionDescription = stringResource(
        if (verified) R.string.home_ksud_daemon_installed_version else R.string.home_ksud_daemon_apk_version,
        version ?: stringResource(R.string.home_ksud_daemon_unknown),
    )
    val syncLabel = if (syncing) status else syncUnavailableReason
        ?: stringResource(R.string.home_ksud_daemon_sync)
    val canSync = !loading && !syncing && syncUnavailableReason == null
    val cardColors = getCardColors(colors.surfaceContainerLow)

    ElevatedCard(
        onClick = onSync,
        enabled = canSync,
        modifier = modifier.heightIn(min = 136.dp)
            .border(getCardBorder(), getCardShape())
            .semantics {
                onClick(label = syncLabel, action = null)
                stateDescription = if (loading || syncing) status else syncUnavailableReason ?: status
            },
        shape = getCardShape(),
        // Disabling the action must not fade the status information or wallpaper backing.
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
            HomeStatusCardHeader("KSUD", titleDescription, Icons.Outlined.Terminal, canSync)
            TooltipBox(
                positionProvider = TooltipDefaults.rememberTooltipPositionProvider(TooltipAnchorPosition.Above),
                tooltip = { PlainTooltip { Text(versionDescription) } },
                state = rememberTooltipState(),
            ) {
                Row(
                    modifier = Modifier.fillMaxWidth().clearAndSetSemantics { contentDescription = versionDescription },
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    verticalAlignment = Alignment.Top,
                ) {
                    // The package symbol identifies a bundled version, not an unknown installed binary.
                    if (!verified) {
                        YukiIcon(Icons.Outlined.Inventory2, null, Modifier.padding(top = 2.dp).size(16.dp), tint = colors.onSurfaceVariant)
                    }
                    Text(
                        stringResource(
                            R.string.home_ksud_version,
                            version ?: stringResource(R.string.home_ksud_daemon_unknown),
                        ),
                        modifier = Modifier.weight(1f),
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
            }
            Row(
                Modifier.fillMaxWidth().semantics { liveRegion = LiveRegionMode.Polite },
                horizontalArrangement = Arrangement.spacedBy(6.dp),
                verticalAlignment = Alignment.Top,
            ) {
                if (loading || syncing) {
                    CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                } else {
                    YukiIcon(
                        when {
                            syncFailed || mismatch -> Icons.Outlined.Warning
                            verified -> Icons.Outlined.CheckCircle
                            else -> Icons.AutoMirrored.Outlined.HelpOutline
                        },
                        null,
                        Modifier.size(16.dp),
                        tint = statusColor,
                    )
                }
                Text(status, style = MaterialTheme.typography.labelMedium, color = statusColor)
            }
            if (!loading && !syncing && syncUnavailableReason != null) {
                Text(syncUnavailableReason, style = MaterialTheme.typography.bodySmall, color = colors.onSurfaceVariant)
            }
        }
    }
}

@Preview(name = "ksud · states", widthDp = 360, heightDp = 800)
@Preview(name = "ksud · dark", widthDp = 360, heightDp = 800, uiMode = Configuration.UI_MODE_NIGHT_YES)
@Preview(name = "ksud · large text", widthDp = 320, heightDp = 1100, fontScale = 2f)
@Composable
private fun KsudCardPreview() {
    UtilityPreviewTheme {
        Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            listOf(KsudIntegrityStatus.MATCH, KsudIntegrityStatus.MISMATCH, KsudIntegrityStatus.UNAVAILABLE).forEach { status ->
                KsudStatusCard(
                    bundledVersion = "v1.3.0-94ca662a-nc", installedVersion = if (status == KsudIntegrityStatus.MATCH) "v1.3.0-94ca662a-nc" else null,
                    integrity = status, loading = false, syncing = false, syncResult = null,
                    syncUnavailableReason = null, onSync = {},
                )
            }
            KsudStatusCard("v1.3.0-94ca662a", null, KsudIntegrityStatus.MISMATCH, false, true, null, null, {})
            KsudStatusCard("v1.3.0-94ca662a", null, KsudIntegrityStatus.MISMATCH, false, false, false, null, {})
        }
    }
}
