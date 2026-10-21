package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.sumh.util.*

@Composable
internal fun TemporaryMapsControls(
    enabled: Boolean,
    operations: SUMHTemporaryMapsOperations = rememberSUMHTemporaryMapsOperations(),
    service: SUMHTemporaryMapsService? = null,
    tracking: SUMHTemporaryMapsTrackingService = SUMHTemporaryMapsTracking.service,
) {
    var open by rememberSaveable { mutableStateOf(false) }
    SideEffect { operations.observeParent(enabled) }
    OutlinedButton(onClick = { open = true }, enabled = enabled,
        modifier = Modifier.padding(top = 8.dp).heightIn(min = 48.dp), shape = MaterialTheme.shapes.small) {
        Text(stringResource(R.string.sumh_temporary_maps_open))
    }
    if (open) TemporaryMapsDialog(operations, enabled, service, tracking, onDismiss = { open = false })
}

@Composable
internal fun TemporaryMapsDialog(
    operations: SUMHTemporaryMapsOperations,
    enabled: Boolean,
    service: SUMHTemporaryMapsService? = null,
    tracking: SUMHTemporaryMapsTrackingService = SUMHTemporaryMapsTracking.service,
    onDismiss: () -> Unit,
) {
    fun refresh() { if (service == null) operations.refresh(tracking = tracking) else operations.refresh(service, tracking) }
    fun next() { if (service == null) operations.next() else operations.next(service) }
    fun remove() { operations.remove(tracking) }
    var showHistory by rememberSaveable { mutableStateOf(false) }
    val historyState = stringResource(if (showHistory) R.string.sumh_temporary_maps_history_expanded
        else R.string.sumh_temporary_maps_history_collapsed)
    var forgetId by rememberSaveable { mutableStateOf<String?>(null) }
    LaunchedEffect(tracking) { if (!operations.history.loaded) operations.loadHistory(tracking) }
    val selection = operations.selected
    YukiAlertDialog(
        onDismissRequest = onDismiss,
        modifier = Modifier.padding(horizontal = 24.dp).widthIn(max = 640.dp).fillMaxWidth()
            .testTag("temporary-maps-dialog"),
        properties = DialogProperties(usePlatformDefaultWidth = false),
        title = { Text(stringResource(if (selection == null) R.string.sumh_temporary_maps_title
            else R.string.sumh_temporary_maps_confirm_title)) },
        text = {
            LazyColumn(Modifier.fillMaxWidth().heightIn(max = 560.dp).testTag("temporary-maps-list"),
                verticalArrangement = Arrangement.spacedBy(12.dp)) {
                item("status") {
                    TemporaryMapsStatus(operations)
                }
                item("refresh") {
                    OutlinedButton(onClick = ::refresh, enabled = enabled && !operations.busy,
                        modifier = Modifier.heightIn(min = 48.dp).testTag("temporary-maps-refresh")) {
                        Text(stringResource(R.string.sumh_temporary_maps_refresh))
                    }
                }
                item("history-toggle") {
                    TextButton(onClick = { showHistory = !showHistory },
                        modifier = Modifier.heightIn(min = 48.dp).testTag("temporary-maps-history-toggle")
                            .semantics { stateDescription = historyState }) {
                        Text(stringResource(R.string.sumh_temporary_maps_history_title, operations.history.records.size))
                    }
                    if (operations.history.error != null || operations.historyError)
                        Text(stringResource(R.string.sumh_temporary_maps_history_error), color = MaterialTheme.colorScheme.error,
                            modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite })
                }
                if (showHistory) temporaryMapsHistory(operations, enabled, tracking) { forgetId = it }
                if (selection != null) {
                    item("selection") {
                        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text(stringResource(R.string.sumh_temporary_maps_confirm))
                            TemporaryMapsIdentity(selection)
                            operations.operationId?.let { id ->
                                Text(stringResource(R.string.sumh_temporary_maps_operation_id))
                                SelectionContainer { Text(id, fontFamily = FontFamily.Monospace,
                                    modifier = Modifier.testTag("temporary-maps-operation-id")) }
                            }
                            operations.result?.let { receipt ->
                                Text(stringResource(R.string.sumh_temporary_maps_received_heading), style = MaterialTheme.typography.labelLarge)
                                Text(stringResource(temporaryMapsRemoveMessage(receipt)),
                                    modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite }
                                    .testTag("temporary-maps-result"),
                                    color = if (receipt.status == SUMHTemporaryMapsRemoveStatus.Deleted)
                                        MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.error)
                            }
                            TextButton(onClick = operations::dismissSelection,
                                modifier = Modifier.heightIn(min = 48.dp).testTag("temporary-maps-back")) {
                                Text(stringResource(R.string.sumh_temporary_maps_back))
                            }
                        }
                    }
                } else {
                    item("scope") {
                        Text(stringResource(R.string.sumh_temporary_maps_description),
                            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    item("capabilities") {
                        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text(stringResource(R.string.sumh_temporary_maps_inventory_support,
                                temporaryMapsSupport(operations.capabilities.inventory)))
                            Text(stringResource(R.string.sumh_temporary_maps_delete_support,
                                temporaryMapsSupport(operations.capabilities.delete)))
                            Text(stringResource(R.string.sumh_temporary_maps_tracking_support,
                                temporaryMapsSupport(operations.trackingSupport)))
                            if (operations.trackingSupport != SUMHTemporaryMapsSupport.Supported)
                                Text(stringResource(if (operations.trackingSupport == SUMHTemporaryMapsSupport.Unsupported)
                                    R.string.sumh_temporary_maps_tracking_update else R.string.sumh_temporary_maps_tracking_unknown),
                                    style = MaterialTheme.typography.bodySmall)
                        }
                    }
                    item("count") {
                        operations.snapshot?.let { snapshot ->
                            Text(pluralStringResource(if (snapshot.complete) R.plurals.sumh_temporary_maps_complete
                                else R.plurals.sumh_temporary_maps_partial, snapshot.rows.size, snapshot.rows.size),
                                modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite })
                            if (snapshot.complete && snapshot.rows.isEmpty() && operations.fresh)
                                Text(stringResource(R.string.sumh_temporary_maps_empty))
                        }
                    }
                    items(operations.snapshot?.rows.orEmpty(), key = { "rule:${it.key}" }) { row ->
                        OutlinedCard(Modifier.fillMaxWidth()) {
                            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                TemporaryMapsIdentity(row)
                                OutlinedButton(onClick = { operations.select(row) },
                                    enabled = enabled && operations.canSelect,
                                    modifier = Modifier.heightIn(min = 48.dp).testTag("temporary-maps-select:${row.key}")) {
                                    Text(stringResource(R.string.sumh_temporary_maps_review, row.targetIno, row.targetDev))
                                }
                            }
                        }
                    }
                    if (operations.snapshot?.continuation != null) item("next") {
                        OutlinedButton(onClick = ::next, enabled = enabled && operations.canNext,
                            modifier = Modifier.heightIn(min = 48.dp).testTag("temporary-maps-next")) {
                            Text(stringResource(R.string.sumh_temporary_maps_next))
                        }
                    }
                }
            }
        },
        confirmButton = {
            if (selection != null && operations.result == null) TextButton(onClick = ::remove,
                enabled = enabled && operations.canRemove,
                colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error),
                modifier = Modifier.heightIn(min = 48.dp).testTag("temporary-maps-remove")) {
                Text(stringResource(R.string.sumh_temporary_maps_delete))
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss, modifier = Modifier.heightIn(min = 48.dp).testTag("temporary-maps-close")) {
                Text(stringResource(R.string.sumh_temporary_maps_close))
            }
        },
    )
    forgetId?.let { id ->
        YukiAlertDialog(onDismissRequest = { forgetId = null },
            title = { Text(stringResource(R.string.sumh_temporary_maps_forget)) },
            text = { Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.sumh_temporary_maps_forget_hint))
                SelectionContainer { Text(id, fontFamily = FontFamily.Monospace) }
            } },
            confirmButton = { TextButton(onClick = {
                if (operations.forget(id, tracking)) forgetId = null
            }, enabled = enabled && !operations.busy,
                modifier = Modifier.heightIn(min = 48.dp).testTag("temporary-maps-forget-confirm")) {
                Text(stringResource(R.string.sumh_temporary_maps_forget))
            } },
            dismissButton = { TextButton(onClick = { forgetId = null }, modifier = Modifier.heightIn(min = 48.dp)) {
                Text(stringResource(android.R.string.cancel))
            } })
    }
}

@Composable
private fun TemporaryMapsStatus(operations: SUMHTemporaryMapsOperations) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        val status = when (operations.action) {
            SUMHTemporaryMapsAction.Refresh -> R.string.sumh_temporary_maps_reading
            SUMHTemporaryMapsAction.Next -> R.string.sumh_temporary_maps_reading_next
            SUMHTemporaryMapsAction.Remove -> R.string.sumh_temporary_maps_removing
            SUMHTemporaryMapsAction.LoadHistory -> R.string.sumh_temporary_maps_history_loading
            SUMHTemporaryMapsAction.Recover -> R.string.sumh_temporary_maps_history_querying
            SUMHTemporaryMapsAction.Forget -> R.string.sumh_temporary_maps_history_forgetting
            null -> when {
                operations.problem == SUMHTemporaryMapsProblem.Unsupported -> R.string.sumh_temporary_maps_unsupported
                operations.problem == SUMHTemporaryMapsProblem.Stale -> R.string.sumh_temporary_maps_stale
                operations.problem == SUMHTemporaryMapsProblem.Unknown -> R.string.sumh_temporary_maps_support_unknown
                operations.problem == SUMHTemporaryMapsProblem.Failed -> R.string.sumh_temporary_maps_read_failed
                operations.problem == SUMHTemporaryMapsProblem.Changed -> R.string.sumh_temporary_maps_changed
                operations.queryRequired -> R.string.sumh_temporary_maps_query_required
                operations.snapshot == null -> R.string.sumh_temporary_maps_start
                else -> R.string.sumh_temporary_maps_observed
            }
        }
        Text(stringResource(status), Modifier.semantics { liveRegion = LiveRegionMode.Polite }
            .testTag("temporary-maps-status"), color = if (operations.problem == null)
                MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.error)
        if (operations.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
        if (operations.snapshot != null && !operations.fresh)
            Text(stringResource(R.string.sumh_temporary_maps_historical),
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun TemporaryMapsIdentity(row: SUMHTemporaryMapsRow) {
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(stringResource(R.string.sumh_temporary_maps_target), style = MaterialTheme.typography.labelLarge,
            modifier = Modifier.semantics { heading() })
        SelectionContainer { Text(stringResource(R.string.sumh_temporary_maps_tuple, row.targetIno, row.targetDev),
            fontFamily = FontFamily.Monospace) }
        Text(stringResource(R.string.sumh_temporary_maps_replacement), style = MaterialTheme.typography.labelLarge)
        SelectionContainer { Text(stringResource(R.string.sumh_temporary_maps_tuple, row.spoofedIno, row.spoofedDev),
            fontFamily = FontFamily.Monospace) }
        Text(stringResource(if (row.spoofedPath.utf8 == null) R.string.sumh_temporary_maps_hex
            else R.string.sumh_temporary_maps_path), style = MaterialTheme.typography.labelLarge)
        SelectionContainer { Text(row.spoofedPath.utf8 ?: row.spoofedPath.hex, fontFamily = FontFamily.Monospace,
            modifier = Modifier.testTag("temporary-maps-path:${row.key}")) }
    }
}

@Composable
private fun temporaryMapsSupport(value: SUMHTemporaryMapsSupport) = stringResource(when (value) {
    SUMHTemporaryMapsSupport.Supported -> R.string.sumh_temporary_maps_supported
    SUMHTemporaryMapsSupport.Unsupported -> R.string.sumh_temporary_maps_not_supported
    SUMHTemporaryMapsSupport.Unknown -> R.string.sumh_temporary_maps_not_known
})

internal fun temporaryMapsRemoveMessage(result: SUMHTemporaryMapsRemoveResult): Int = when (result.status) {
    SUMHTemporaryMapsRemoveStatus.Deleted -> R.string.sumh_temporary_maps_deleted
    SUMHTemporaryMapsRemoveStatus.Unknown -> R.string.sumh_temporary_maps_unknown
    SUMHTemporaryMapsRemoveStatus.NotApplied -> when (result.errorCode) {
        "snapshot_changed" -> R.string.sumh_temporary_maps_remove_stale
        "temporary_key_not_found" -> R.string.sumh_temporary_maps_remove_absent
        "unsupported" -> R.string.sumh_temporary_maps_remove_unsupported
        "permission_denied" -> R.string.sumh_temporary_maps_remove_permission
        "insufficient_memory" -> R.string.sumh_temporary_maps_remove_memory
        "runtime_stopped" -> R.string.sumh_temporary_maps_remove_stopped
        else -> R.string.sumh_temporary_maps_not_applied
    }
}
