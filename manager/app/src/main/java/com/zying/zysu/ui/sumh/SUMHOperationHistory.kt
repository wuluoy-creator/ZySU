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
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.sumh.util.*
import kotlinx.coroutines.launch

/** Only identifiers enter saved UI state. Receipts stay in the bounded private file. */
@Composable internal fun SUMHOperationHistory(tracker: SUMHOperationTracker, enabled: Boolean) {
    val snapshot by tracker.snapshot.collectAsState()
    var open by rememberSaveable { mutableStateOf(false) }
    var selected by rememberSaveable { mutableStateOf<String?>(null) }
    var forget by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    LaunchedEffect(tracker) { tracker.load() }
    fun query(id: String, wait: Boolean = false) {
        if (busy || !enabled) return
        busy = true
        scope.launch { try {
            if (wait) tracker.observe(id, 5000) else tracker.recover(id)
        } finally { busy = false } }
    }
    Column(Modifier.padding(horizontal = 16.dp).semantics { liveRegion = LiveRegionMode.Polite }) {
        TextButton(onClick = { open = true }, modifier = Modifier.testTag("sumh_operation_history")) {
            Text(stringResource(R.string.sumh_operations_title, snapshot.records.size))
        }
        if (snapshot.error != null) Text(stringResource(R.string.sumh_operations_storage_error), color = MaterialTheme.colorScheme.error)
    }
    if (open) YukiAlertDialog(onDismissRequest = { open = false; selected = null },
        title = { Text(stringResource(R.string.sumh_operations_title, snapshot.records.size)) },
        text = { LazyColumn(Modifier.heightIn(max = 440.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            item { Text(stringResource(R.string.sumh_operations_hint), style = MaterialTheme.typography.bodySmall) }
            if (busy) item { LinearProgressIndicator(Modifier.fillMaxWidth()) }
            snapshot.error?.let { error -> item { Text(error, color = MaterialTheme.colorScheme.error) } }
            if (snapshot.loaded && snapshot.records.isEmpty()) item { Text(stringResource(R.string.sumh_operations_empty)) }
            items(snapshot.records.asReversed(), key = { it.id }) { entry ->
                Text(entry.name, style = MaterialTheme.typography.titleSmall)
                SelectionContainer { Text(entry.id, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall) }
                Text(stringResource(when (entry.stage) {
                    "prepared" -> R.string.sumh_operations_prepared
                    "dispatching" -> R.string.sumh_operations_dispatching
                    "received" -> R.string.sumh_operations_received
                    else -> R.string.sumh_operations_unknown
                }))
                FlowRow(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    TextButton(onClick = { selected = entry.id }, modifier = Modifier.testTag("sumh_operation_detail_${entry.id}")) {
                        Text(stringResource(R.string.sumh_operations_details))
                    }
                    TextButton(onClick = { query(entry.id) }, enabled = enabled && !busy,
                        modifier = Modifier.testTag("sumh_operation_query_${entry.id}")) { Text(stringResource(R.string.sumh_operations_query)) }
                    TextButton(onClick = { forget = entry.id }, enabled = enabled && !busy) { Text(stringResource(R.string.sumh_operations_forget)) }
                }
                HorizontalDivider()
            }
        } }, confirmButton = { TextButton(onClick = { open = false; selected = null }) { Text(stringResource(android.R.string.ok)) } },
        dismissButton = { TextButton(onClick = { scope.launch { tracker.load() } }, enabled = !busy) { Text(stringResource(R.string.sumh_rules_refresh)) } })

    val entry = snapshot.records.singleOrNull { it.id == selected }
    if (open && entry != null) {
        val history = remember(entry.observation) { entry.observation.takeIf { it.isNotEmpty() }?.let {
            SUMHOperationsParser.observation(SUMHQueryReply(it), entry.id, entry.requestRevision, entry.name, entry.bootId)
        } }
        val resource = remember(entry) { entry.resource() }
        val recovered = remember(entry) { entry.recovery() }
        YukiAlertDialog(onDismissRequest = { selected = null }, title = { Text(stringResource(R.string.sumh_operations_details)) },
            text = { LazyColumn(Modifier.heightIn(max = 440.dp).testTag("sumh_operation_detail_content"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                item {
                    SelectionContainer { Text(entry.id + "\n" + entry.name + "\n" + entry.bootId, fontFamily = FontFamily.Monospace) }
                    Text(stringResource(R.string.sumh_operations_historical), style = MaterialTheme.typography.bodySmall)
                }
                item {
                    Text(stringResource(R.string.sumh_operations_received_heading), style = MaterialTheme.typography.titleSmall)
                    if (resource != null) HideMutationSummary(resource) else Text(stringResource(R.string.sumh_operations_no_receipt))
                }
                item {
                    Text(stringResource(R.string.sumh_operations_summary_heading), style = MaterialTheme.typography.titleSmall)
                    Text(history?.let { observation -> observation.records.singleOrNull()?.let {
                        "${observation.status} · ${it.phase} · ${it.result}\nexit=${it.exitCode ?: "?"} · errno=${it.errno ?: "?"}"
                    } ?: observation.status } ?: stringResource(R.string.sumh_operations_not_queried))
                    if (history?.records?.singleOrNull()?.sameBoot == false) Text(stringResource(R.string.sumh_operations_previous_boot))
                }
                item {
                    Text(stringResource(R.string.sumh_operations_recovered_heading), style = MaterialTheme.typography.titleSmall)
                    if (recovered?.resource != null) HideMutationSummary(recovered.resource)
                    else Text(recovered?.let { "${it.status} · ${it.receiptStatus}\n${it.error}" } ?: stringResource(R.string.sumh_operations_not_queried))
                    if (recovered?.record?.sameBoot == false) Text(stringResource(R.string.sumh_operations_previous_boot))
                }
                items(recovered?.resource?.value?.items.orEmpty(), key = { it.id }) { item ->
                    Text("${item.id}\n${item.outcome} · errno=${item.errno}", style = MaterialTheme.typography.bodySmall)
                }
                if (entry.error.isNotEmpty()) item { Text(entry.error, color = MaterialTheme.colorScheme.error) }
                item { Text(stringResource(R.string.sumh_operations_current_hint), style = MaterialTheme.typography.bodySmall) }
                item { SelectionContainer { Text(entry.requestRevision + "\n" + entry.configRevision, style = MaterialTheme.typography.bodySmall) } }
            } }, confirmButton = { TextButton(onClick = { selected = null }) { Text(stringResource(android.R.string.ok)) } },
            dismissButton = { FlowRow {
                TextButton(onClick = { query(entry.id) }, enabled = enabled && !busy) { Text(stringResource(R.string.sumh_operations_query)) }
                TextButton(onClick = { query(entry.id, true) }, enabled = enabled && !busy) { Text(stringResource(R.string.sumh_operations_wait)) }
            } })
    }
    forget?.let { id -> YukiAlertDialog(onDismissRequest = { forget = null }, title = { Text(stringResource(R.string.sumh_operations_forget)) },
        text = { Text(stringResource(R.string.sumh_operations_forget_hint)) },
        confirmButton = { TextButton(onClick = {
            busy = true; forget = null
            scope.launch { try { if (tracker.acknowledge(id) && selected == id) selected = null } finally { busy = false } }
        }, enabled = enabled && !busy) { Text(stringResource(android.R.string.ok)) } },
        dismissButton = { TextButton(onClick = { forget = null }) { Text(stringResource(android.R.string.cancel)) } }) }
}
