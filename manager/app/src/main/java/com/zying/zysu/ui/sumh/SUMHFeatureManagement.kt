package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.FolderOff
import androidx.compose.material.icons.filled.Security
import androidx.compose.material.icons.filled.Storage
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.sumh.util.SUMHFeatureState
import com.zying.zysu.ui.sumh.util.SUMHFeatureSupport
import com.zying.zysu.ui.sumh.util.SUMHManagedFeature
import com.zying.zysu.ui.sumh.util.sumhFeatureStates
import com.zying.zysu.ui.sumh.util.unsupportedConfiguredFeatures
import ui.screen.moreSettings.component.MoreSettingsItemPosition
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as Controller

private val SUMHManagedFeature.titleRes: Int
    get() = when (this) {
        SUMHManagedFeature.Stealth -> R.string.sumh_stealth
        SUMHManagedFeature.MountHide -> R.string.sumh_mount_hide
        SUMHManagedFeature.OverlayXattr -> R.string.sumh_overlay_xattr
        SUMHManagedFeature.Statfs -> R.string.sumh_statfs_spoof
        SUMHManagedFeature.Maps -> R.string.sumh_maps_spoof
        SUMHManagedFeature.KernelBuild -> R.string.sumh_kernel_build_spoof
    }

private val SUMHManagedFeature.descriptionRes: Int
    get() = when (this) {
        SUMHManagedFeature.Stealth -> R.string.sumh_stealth_desc
        SUMHManagedFeature.MountHide -> R.string.sumh_mount_hide_desc
        SUMHManagedFeature.OverlayXattr -> R.string.sumh_overlay_xattr_desc
        SUMHManagedFeature.Statfs -> R.string.sumh_statfs_spoof_desc
        SUMHManagedFeature.Maps -> R.string.sumh_maps_spoof_desc
        SUMHManagedFeature.KernelBuild -> R.string.sumh_kernel_build_spoof_desc
    }

private val SUMHManagedFeature.icon: ImageVector
    get() = when (this) {
        SUMHManagedFeature.Stealth -> Icons.Filled.VisibilityOff
        SUMHManagedFeature.MountHide -> Icons.Filled.FolderOff
        SUMHManagedFeature.OverlayXattr -> Icons.Filled.Security
        SUMHManagedFeature.Statfs -> Icons.Filled.Storage
        SUMHManagedFeature.Maps, SUMHManagedFeature.KernelBuild -> Icons.Filled.Storage
    }

internal fun Controller.SUMHPConfig.configuredSUMHFeatures(): Set<SUMHManagedFeature> = buildSet {
    if (enableStealth) add(SUMHManagedFeature.Stealth)
    if (enableMountHide) add(SUMHManagedFeature.MountHide)
    if (enableOverlayXattrHide) add(SUMHManagedFeature.OverlayXattr)
    if (enableStatfsSpoof) add(SUMHManagedFeature.Statfs)
    if (enableMapsSpoof) add(SUMHManagedFeature.Maps)
    if (enableKernelBuildSpoof) add(SUMHManagedFeature.KernelBuild)
}

internal fun Controller.SUMHPConfig.disableSUMHFeatures(features: Set<SUMHManagedFeature>) = copy(
    enableStealth = enableStealth && SUMHManagedFeature.Stealth !in features,
    enableMountHide = enableMountHide && SUMHManagedFeature.MountHide !in features,
    enableOverlayXattrHide = enableOverlayXattrHide && SUMHManagedFeature.OverlayXattr !in features,
    enableStatfsSpoof = enableStatfsSpoof && SUMHManagedFeature.Statfs !in features,
    enableMapsSpoof = enableMapsSpoof && SUMHManagedFeature.Maps !in features,
    enableKernelBuildSpoof = enableKernelBuildSpoof && SUMHManagedFeature.KernelBuild !in features,
)

@Composable
internal fun sumhFeatureSummary(description: String, state: SUMHFeatureState): String {
    val support = if (state.support == SUMHFeatureSupport.Supported) null else stringResource(when (state.support) {
        SUMHFeatureSupport.Supported -> R.string.sumh_features_support_yes
        SUMHFeatureSupport.Unsupported -> if (state.configuredEnabled) R.string.sumh_disable_unsupported
            else R.string.sumh_features_support_no
        SUMHFeatureSupport.Unknown -> R.string.sumh_features_support_unknown
        SUMHFeatureSupport.KernelUnavailable -> R.string.sumh_features_kernel_unavailable
    })
    val runtime = when {
        state.runtimeEnabled != null && (state.configuredEnabled || state.runtimeEnabled) ->
            stringResource(if (state.runtimeEnabled) R.string.sumh_feature_running else R.string.sumh_feature_not_running)
        state.runtimeEnabled == null && state.configuredEnabled && state.support != SUMHFeatureSupport.KernelUnavailable ->
            stringResource(R.string.sumh_feature_runtime_unknown)
        else -> null
    }
    return listOfNotNull(description, support, runtime).joinToString("\n")
}

@Composable
internal fun IsolationFeatureSettings(
    config: Controller.SUMHPConfig,
    kernelAvailable: Boolean,
    features: Controller.FeaturesResult?,
    controlsEnabled: Boolean,
    onConfigChanged: (Controller.SUMHPConfig) -> Unit,
    headingActions: (@Composable RowScope.() -> Unit)?,
    runtimeRules: List<Controller.ActiveRule>? = null,
) {
    val states = sumhFeatureStates(config.configuredSUMHFeatures(), kernelAvailable, features?.names?.toSet(),
        runtimeRules?.mapTo(hashSetOf()) { it.type })
    val standard = setOf(SUMHManagedFeature.Stealth, SUMHManagedFeature.MountHide,
        SUMHManagedFeature.OverlayXattr, SUMHManagedFeature.Statfs)
    val cleanup = unsupportedConfiguredFeatures(states).intersect(standard)
    var cleanupTargets by remember { mutableStateOf<Set<SUMHManagedFeature>?>(null) }

    val groups = listOf(
        R.string.sumh_group_mount_visibility to setOf(SUMHManagedFeature.Stealth, SUMHManagedFeature.MountHide),
        R.string.sumh_group_filesystem to setOf(SUMHManagedFeature.OverlayXattr, SUMHManagedFeature.Statfs),
    )
    groups.forEachIndexed { groupIndex, (title, members) ->
        val group = states.filter { it.feature in members }
        if (group.isNotEmpty()) {
            ConfigSection(stringResource(title), segmented = true,
                actions = if (groupIndex == 0) headingActions else null) {
                group.forEachIndexed { index, state ->
                    val position = if (index == 0) MoreSettingsItemPosition.First else MoreSettingsItemPosition.Last
                    val requiresMountHide = state.feature == SUMHManagedFeature.Statfs && !config.enableMountHide
                    SettingSwitch(
                        title = stringResource(state.feature.titleRes),
                        subtitle = sumhFeatureSummary(stringResource(state.feature.descriptionRes), state) +
                            if (requiresMountHide) "\n" + stringResource(R.string.sumh_statfs_requires_mount_hide) else "",
                        checked = state.configuredEnabled,
                        onCheckedChange = { checked ->
                            val next = when (state.feature) {
                                SUMHManagedFeature.Stealth -> config.copy(enableStealth = checked)
                                SUMHManagedFeature.MountHide -> config.copy(enableMountHide = checked)
                                SUMHManagedFeature.OverlayXattr -> config.copy(enableOverlayXattrHide = checked)
                                SUMHManagedFeature.Statfs -> config.copy(enableStatfsSpoof = checked)
                                else -> config
                            }
                            onConfigChanged(next)
                        },
                        enabled = controlsEnabled && state.canChange && (!requiresMountHide || state.configuredEnabled),
                        icon = state.feature.icon,
                        position = position,
                    )
                }
                if (groupIndex == 1 && cleanup.isNotEmpty()) TextButton(
                    onClick = { cleanupTargets = cleanup }, enabled = controlsEnabled,
                    modifier = Modifier.heightIn(min = 48.dp)) {
                    Text(stringResource(R.string.sumh_features_cleanup))
                }
            }
        }
    }

    cleanupTargets?.let { targets ->
        YukiAlertDialog(
            onDismissRequest = { cleanupTargets = null },
            title = { Text(stringResource(R.string.sumh_features_cleanup)) },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(stringResource(R.string.sumh_features_cleanup_confirm))
                    targets.forEach { Text("• " + stringResource(it.titleRes)) }
                }
            },
            confirmButton = {
                TextButton(
                    enabled = controlsEnabled && targets.isNotEmpty() && targets.all { it in cleanup },
                    modifier = Modifier.heightIn(min = 48.dp),
                    onClick = {
                        if (controlsEnabled && targets.all { it in cleanup }) {
                            cleanupTargets = null
                            onConfigChanged(config.disableSUMHFeatures(targets))
                        }
                    },
                ) { Text(stringResource(R.string.sumh_features_cleanup_confirm_action)) }
            },
            dismissButton = {
                TextButton(onClick = { cleanupTargets = null }, modifier = Modifier.heightIn(min = 48.dp)) {
                    Text(stringResource(R.string.sumh_rules_cancel))
                }
            },
        )
    }
}
