package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.*
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as Controller

internal enum class SUMHApplyAdvice { Drift, Query, Verified, Legacy }

/** Only the runtime sample collected with the comparisons can justify a retry suggestion. */
internal fun SUMHControlState.applyAdvice(): SUMHApplyAdvice {
    if (capabilities.state == SUMHSchemaSupport.NotAdvertised) return SUMHApplyAdvice.Legacy
    val sample = state.value ?: return SUMHApplyAdvice.Query
    if (state.retained || state.error != null || sample.bootId.isNullOrEmpty() || sample.runtime.value?.status != SUMHRuntimeRead.Known ||
        sample.runtime.error != null || sample.history.status !in setOf(SUMHHistoryRead.Valid, SUMHHistoryRead.Missing) ||
        sample.history.last?.result == SUMHOperationResult.Unknown) return SUMHApplyAdvice.Query
    val controls = SUMHControlStateParser.controlNames.map { sample.features[it] }
    if (controls.any { it == null || it.desiredRevision == null || it.comparison == SUMHComparison.Unknown }) return SUMHApplyAdvice.Query
    return if (controls.any { it?.comparison == SUMHComparison.Drift }) SUMHApplyAdvice.Drift else SUMHApplyAdvice.Verified
}

private fun stateFeatureIds(section: SUMHSection) = when (section) {
    SUMHSection.Debug -> listOf("kernel_debug")
    SUMHSection.Mount -> listOf("modules", "overlay_xattr_hide")
    SUMHSection.Isolation -> SUMHControlStateParser.controlNames.drop(1) + listOf("maps_rules", "user_hide", "overlay_xattr_hide")
    else -> SUMHControlStateParser.controlNames + listOf("user_hide", "maps_rules", "modules", "overlay_xattr_hide")
}

private fun Controller.SUMHPConfig.savedFlag(id: String): Boolean? = when (id) {
    "kernel_debug" -> enableKernelDebug; "stealth" -> enableStealth; "mount_hide" -> enableMountHide
    "maps_spoof" -> enableMapsSpoof; "statfs_spoof" -> enableStatfsSpoof; "kernel_build_spoof" -> enableKernelBuildSpoof
    "overlay_xattr_hide" -> enableOverlayXattrHide; else -> null
}

@Composable
internal fun SUMHControlStatePanel(
    control: SUMHControlState,
    config: Controller.SUMHPConfig,
    section: SUMHSection,
    onRefresh: () -> Unit,
    onApply: (() -> Unit)? = null,
    enabled: Boolean = true,
    legacyPending: Boolean = false,
) {
    var expanded by rememberSaveable { mutableStateOf(false) }
    var diagnostic by rememberSaveable { mutableStateOf(false) }
    val sample = control.state.value
    val advice = control.applyAdvice()
    val stale = control.state.retained || control.runtime.retained
    ConfigSection(stringResource(R.string.sumh_state_title)) {
        Text(stringResource(when {
            stale -> R.string.sumh_state_stale
            advice == SUMHApplyAdvice.Legacy -> R.string.sumh_state_legacy
            advice == SUMHApplyAdvice.Query -> R.string.sumh_state_query_needed
            advice == SUMHApplyAdvice.Drift -> R.string.sumh_state_drift_summary
            else -> R.string.sumh_state_sampled_summary
        }), style = MaterialTheme.typography.bodyMedium)
        Text(stringResource(R.string.sumh_state_scope), style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        val failures = listOfNotNull(control.state.error, sample?.runtime?.error, control.runtime.error,
            sample?.history?.error).distinct()
        failures.forEach { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
        control.runtime.value?.takeIf { it.status != SUMHRuntimeRead.Known }?.let {
            Text(stringResource(when (it.status) {
                SUMHRuntimeRead.Unsupported -> R.string.sumh_state_runtime_unsupported
                SUMHRuntimeRead.Unavailable -> R.string.sumh_state_runtime_unavailable
                else -> R.string.sumh_state_runtime_error
            }), style = MaterialTheme.typography.bodySmall)
        }
        sample?.history?.let { history ->
            val last = history.last
            Text(stringResource(if (last == null) when (history.status) {
                SUMHHistoryRead.Missing -> R.string.sumh_state_history_missing
                else -> R.string.sumh_state_history_unavailable
            } else when (last.result) {
                SUMHOperationResult.Completed -> R.string.sumh_state_operation_completed
                SUMHOperationResult.FailedOrPartial -> R.string.sumh_state_operation_failed
                SUMHOperationResult.Unknown -> R.string.sumh_state_operation_unknown
            }), style = MaterialTheme.typography.bodySmall)
            if (last != null) {
                Text(last.name, style = MaterialTheme.typography.bodySmall)
                if (last.bootId != sample.bootId || stale) Text(stringResource(R.string.sumh_state_history_old_boot),
                    style = MaterialTheme.typography.bodySmall)
            }
        }
        if (advice == SUMHApplyAdvice.Legacy && legacyPending) Text(stringResource(R.string.sumh_state_legacy_pending))
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            OutlinedButton(onClick = onRefresh, enabled = enabled, modifier = Modifier.testTag("state_refresh")) {
                Text(stringResource(R.string.sumh_state_refresh))
            }
            if (onApply != null && (advice == SUMHApplyAdvice.Drift || advice == SUMHApplyAdvice.Legacy && legacyPending)) {
                OutlinedButton(onClick = onApply, enabled = enabled, modifier = Modifier.testTag("state_apply")) {
                    Text(stringResource(R.string.sumh_state_apply))
                }
            }
            TextButton(onClick = { expanded = !expanded }, modifier = Modifier.testTag("state_expand")) {
                Text(stringResource(if (expanded) R.string.sumh_state_less else R.string.sumh_state_more))
            }
        }
        if (advice == SUMHApplyAdvice.Drift && onApply != null) Text(stringResource(R.string.sumh_state_apply_scope),
            style = MaterialTheme.typography.bodySmall)
        if (expanded) {
            if (control.capabilities.state != SUMHSchemaSupport.Supported) Text(stringResource(when (control.capabilities.state) {
                SUMHSchemaSupport.NotAdvertised -> R.string.sumh_state_schema_legacy
                SUMHSchemaSupport.Future -> R.string.sumh_state_schema_future
                else -> R.string.sumh_state_schema_unknown
            }))
            stateFeatureIds(section).forEach { id ->
                val observation = sample?.features?.get(id)
                val runtime = if (!control.state.retained) sample?.runtime?.value ?: control.runtime.value else control.runtime.value
                val live = runtime?.features?.get(id)
                val current = !stale && runtime?.status == SUMHRuntimeRead.Known
                val comparisonCurrent = !stale && sample?.runtime?.value?.status == SUMHRuntimeRead.Known
                HorizontalDivider()
                Column(Modifier.fillMaxWidth().testTag("state_feature_$id"), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(stateFeatureTitle(id), style = MaterialTheme.typography.titleSmall)
                    Text(stringResource(when {
                        stale -> R.string.sumh_state_stale
                        observation == null || !comparisonCurrent -> R.string.sumh_state_unknown
                        else -> when (observation.comparison) {
                            SUMHComparison.Matches -> if (id == "user_hide") R.string.sumh_state_registered else R.string.sumh_state_matches
                            SUMHComparison.Drift -> R.string.sumh_state_drift
                            SUMHComparison.Unknown -> R.string.sumh_state_unknown
                            SUMHComparison.Unsupported -> R.string.sumh_state_unsupported
                            SUMHComparison.Deferred -> R.string.sumh_state_deferred
                        }
                    }), style = MaterialTheme.typography.bodyMedium)
                    config.savedFlag(id)?.let { saved ->
                        Text(stringResource(R.string.sumh_state_saved, stateFlag(saved)), style = MaterialTheme.typography.bodySmall)
                        Text(stringResource(R.string.sumh_state_runtime_flags, stateFlag(live?.supported.takeIf { current }),
                            stateFlag(live?.configured.takeIf { current }), stateFlag(live?.active.takeIf { current })),
                            style = MaterialTheme.typography.bodySmall)
                    }
                    if (live?.degraded == true && current) Text(stringResource(R.string.sumh_state_configured_inactive),
                        style = MaterialTheme.typography.bodySmall)
                    observation?.let { entry ->
                        Text(if (comparisonCurrent) stateReason(entry.reason) else stringResource(R.string.sumh_state_query_needed), style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        if (id == "user_hide") {
                            Text(stringResource(R.string.sumh_state_hide_scope), style = MaterialTheme.typography.bodySmall)
                            if (entry.pendingCount != null && entry.errorCount != null && entry.suspendedCount != null)
                                Text(stringResource(R.string.sumh_state_hide_counts, entry.pendingCount, entry.errorCount, entry.suspendedCount),
                                    style = MaterialTheme.typography.bodySmall)
                        }
                        if (entry.lastVerified != null) Text(stringResource(if (!stale && entry.appliedRevision != null)
                            R.string.sumh_state_history_current else R.string.sumh_state_history_only), style = MaterialTheme.typography.bodySmall)
                    }
                    if (current && live != null && live.lastSetErrno != 0) Text(
                        stringResource(R.string.sumh_state_setter_error, live.lastSetErrno), style = MaterialTheme.typography.bodySmall)
                }
            }
            TextButton(onClick = { diagnostic = !diagnostic }, modifier = Modifier.testTag("state_diagnostics")) {
                Text(stringResource(R.string.sumh_state_diagnostics))
            }
            if (diagnostic) {
                Text(stringResource(R.string.sumh_state_digest_scope), style = MaterialTheme.typography.bodySmall)
                val live = sample?.runtime?.value ?: control.runtime.value
                val lines = buildList {
                    add("boot: ${sample?.bootId ?: live?.bootId ?: "?"}")
                    add("instance: ${live?.instanceId ?: "?"}; generation: ${live?.generation ?: "?"}")
                    add("sample monotonic ms: ${live?.startedMs ?: "?"}–${live?.finishedMs ?: "?"}")
                    live?.let { add("runtime: ${it.reason}; errno=${it.errno}") }
                    sample?.history?.last?.let { add("operation: ${it.id}; exit=${it.exitCode}; errno=${it.errno}\n${it.detail}") }
                    stateFeatureIds(section).forEach { id -> sample?.features?.get(id)?.let {
                        add("$id\ndesired: ${it.desiredRevision ?: "?"}\napplied: ${it.appliedRevision ?: "?"}")
                    } }
                }
                Text(lines.joinToString("\n\n"), style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
            }
        }
    }
}

@Composable private fun stateFlag(value: Boolean?) = stringResource(when (value) {
    true -> R.string.sumh_state_yes; false -> R.string.sumh_state_no; null -> R.string.sumh_state_unknown
})
@Composable private fun stateFeatureTitle(id: String) = stringResource(when (id) {
    "kernel_debug" -> R.string.sumh_kernel_debug; "stealth" -> R.string.sumh_stealth
    "mount_hide" -> R.string.sumh_mount_hide; "maps_spoof" -> R.string.sumh_maps_spoof
    "statfs_spoof" -> R.string.sumh_statfs_spoof; "kernel_build_spoof" -> R.string.sumh_state_build
    "user_hide" -> R.string.sumh_state_hide; "maps_rules" -> R.string.sumh_state_maps
    "modules" -> R.string.sumh_state_modules; else -> R.string.sumh_state_xattrs
})
@Composable private fun stateReason(reason: String): String = when (reason) {
    "setting_verified", "both_strings_verified" -> stringResource(R.string.sumh_state_reason_verified)
    "configured_but_inactive" -> stringResource(R.string.sumh_state_configured_inactive)
    "saved_setting_differs_from_runtime", "saved_build_differs_from_runtime" -> stringResource(R.string.sumh_state_drift_summary)
    "waiting_for_boot_stage" -> stringResource(R.string.sumh_state_deferred)
    "managed_identity_query_not_available", "runtime_inventory_not_available", "complete_runtime_mapping_and_source_inventory_required" ->
        stringResource(R.string.sumh_state_inventory_unknown)
    "registered_definitions_verified_globally_suspended" -> stringResource(R.string.sumh_state_hide_suspended)
    "registered_definitions_verified" -> stringResource(R.string.sumh_state_registered)
    "definitions_registered_binding_pending" -> stringResource(R.string.sumh_state_hide_pending)
    "binding_errors", "registered_definitions_differ" -> stringResource(R.string.sumh_state_hide_drift)
    "required_runtime_machinery_unavailable", "kernel_build_unavailable" -> stringResource(R.string.sumh_state_unsupported)
    "definitions_changed_during_sample", "runtime_changed_during_sample" -> stringResource(R.string.sumh_state_sample_changed)
    else -> reason
}
