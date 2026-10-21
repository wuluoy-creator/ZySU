package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.*

/** Historical receipts are deliberately separate from the current inventory and its selection. */
internal fun LazyListScope.temporaryMapsHistory(operations: SUMHTemporaryMapsOperations, enabled: Boolean,
    tracking: SUMHTemporaryMapsTrackingService, onForget: (String) -> Unit) {
    item("history-hint") {
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(stringResource(R.string.sumh_temporary_maps_history_hint), style = MaterialTheme.typography.bodySmall)
            OutlinedButton(onClick = { operations.loadHistory(tracking) }, enabled = enabled && !operations.busy,
                modifier = Modifier.heightIn(min = 48.dp).testTag("temporary-maps-history-load")) {
                Text(stringResource(R.string.sumh_temporary_maps_history_load))
            }
            if (operations.history.loaded && operations.history.records.isEmpty())
                Text(stringResource(R.string.sumh_temporary_maps_history_empty))
        }
    }
    items(operations.history.records.asReversed(), key = { "history:${it.id}" }) { entry ->
        val received = remember(entry) { runCatching { entry.resource() }.getOrNull() }
        val recovered = remember(entry) { runCatching { entry.recovery() }.getOrNull() }
        val observation = remember(entry) { runCatching { entry.history() }.getOrNull() }
        OutlinedCard(Modifier.fillMaxWidth().testTag("temporary-maps-history:${entry.id}")) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.sumh_temporary_maps_operation_id),
                    style = MaterialTheme.typography.titleSmall, modifier = Modifier.semantics { heading() })
                SelectionContainer { Text(entry.id, fontFamily = FontFamily.Monospace) }
                Text(stringResource(when (entry.stage) {
                    "prepared" -> R.string.sumh_operations_prepared
                    "dispatching" -> R.string.sumh_operations_dispatching
                    "received" -> R.string.sumh_operations_received
                    else -> R.string.sumh_operations_unknown
                }))
                SelectionContainer { Text(stringResource(R.string.sumh_temporary_maps_tuple,
                    entry.key.targetIno, entry.key.targetDev), style = MaterialTheme.typography.bodySmall) }
                Text(stringResource(R.string.sumh_temporary_maps_received_heading), style = MaterialTheme.typography.labelLarge)
                HistoricalMapsReceipt(received)
                Text(stringResource(R.string.sumh_temporary_maps_recovered_heading), style = MaterialTheme.typography.labelLarge)
                Text(stringResource(when (observation?.status) {
                    "observed" -> when (observation.records.singleOrNull()?.phase) {
                        "prepared" -> R.string.sumh_operations_prepared
                        "started" -> R.string.sumh_operations_dispatching
                        "finished" -> R.string.sumh_temporary_maps_command_finished
                        else -> R.string.sumh_temporary_maps_query_unavailable
                    }
                    "not_retained" -> R.string.sumh_temporary_maps_query_not_retained
                    "wait_timeout" -> R.string.sumh_temporary_maps_query_wait_timeout
                    null -> R.string.sumh_operations_not_queried
                    else -> R.string.sumh_temporary_maps_query_unavailable
                }))
                Text(stringResource(when (recovered?.status) {
                    "observed" -> R.string.sumh_temporary_maps_query_receipt
                    "not_finished" -> R.string.sumh_temporary_maps_query_not_finished
                    "not_retained" -> R.string.sumh_temporary_maps_query_not_retained
                    "resource_unavailable" -> R.string.sumh_temporary_maps_query_no_receipt
                    null -> R.string.sumh_operations_not_queried
                    else -> R.string.sumh_temporary_maps_query_unavailable
                }))
                HistoricalMapsReceipt(recovered?.resource)
                (recovered?.bootId ?: observation?.bootId)?.let { boot ->
                    SelectionContainer { Text(stringResource(R.string.sumh_temporary_maps_query_boot, boot),
                        style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace) }
                }
                if (observation?.records?.singleOrNull()?.sameBoot == false || recovered?.record?.sameBoot == false)
                    Text(stringResource(R.string.sumh_operations_previous_boot), style = MaterialTheme.typography.bodySmall)
                if (entry.error.isNotEmpty() || recovered?.error?.isNotEmpty() == true)
                    Text(stringResource(R.string.sumh_temporary_maps_history_entry_error), color = MaterialTheme.colorScheme.error)
                SelectionContainer {
                    Text(stringResource(R.string.sumh_temporary_maps_binding, entry.bootId,
                        entry.key.instance, entry.key.generation, entry.requestRevision),
                        style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
                }
                FlowRow(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    TextButton(onClick = { operations.recover(entry.id, tracking = tracking) }, enabled = enabled && !operations.busy,
                        modifier = Modifier.heightIn(min = 48.dp).testTag("temporary-maps-query:${entry.id}")) {
                        Text(stringResource(R.string.sumh_temporary_maps_history_query))
                    }
                    TextButton(onClick = { operations.recover(entry.id, 5000, tracking) }, enabled = enabled && !operations.busy,
                        modifier = Modifier.heightIn(min = 48.dp).testTag("temporary-maps-wait:${entry.id}")) {
                        Text(stringResource(R.string.sumh_temporary_maps_history_wait))
                    }
                    TextButton(onClick = { onForget(entry.id) }, enabled = enabled && !operations.busy,
                        modifier = Modifier.heightIn(min = 48.dp).testTag("temporary-maps-forget:${entry.id}")) {
                        Text(stringResource(R.string.sumh_temporary_maps_forget))
                    }
                }
            }
        }
    }
}

@Composable
private fun HistoricalMapsReceipt(receipt: SUMHTemporaryMapsRemoveResult?) {
    Text(stringResource(when (receipt?.status) {
        SUMHTemporaryMapsRemoveStatus.Deleted -> R.string.sumh_temporary_maps_history_deleted
        SUMHTemporaryMapsRemoveStatus.NotApplied -> R.string.sumh_temporary_maps_history_not_applied
        else -> R.string.sumh_temporary_maps_history_unconfirmed
    }))
}
