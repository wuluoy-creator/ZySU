package com.zying.zysu.ui.sumh

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.component.YukiPanel
import com.zying.zysu.ui.sumh.util.*
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as Controller
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import org.json.JSONObject

@Composable
internal fun SUMHRuntimeInventoryWorkspace(
    legacyRules: List<Controller.ActiveRule>, status: Controller.SUMHStatus, enabled: Boolean,
    onLegacyRefresh: () -> Unit, onLegacyClear: () -> Unit,
    read: suspend ((SUMHInventoryCollection) -> Unit) -> SUMHInventoryCollection = { SUMHManager.collectInventory(it) },
) {
    var legacy by rememberSaveable { mutableStateOf(false) }
    var collection by remember { mutableStateOf(SUMHInventoryCollection()) }
    var loading by remember { mutableStateOf(false) }
    var cancelling by remember { mutableStateOf(false) }
    var job by remember { mutableStateOf<Job?>(null) }
    val scope = rememberCoroutineScope()
    fun load() {
        if (loading || !enabled) return
        loading = true
        cancelling = false
        // Never keep a previously complete collection looking current during a new scan.
        collection = SUMHInventoryCollection(status = SUMHInventoryRead.Loading)
        job = scope.launch {
            try { collection = read { collection = it } }
            catch (error: CancellationException) {
                collection = collection.copy(status = SUMHInventoryRead.Cancelled, reason = "cancelled")
            } catch (error: Exception) {
                collection = collection.copy(status = SUMHInventoryRead.Failed, reason = error.message)
            } finally { loading = false; cancelling = false }
        }
    }
    if (legacy) Column(Modifier.fillMaxSize()) {
        TextButton(onClick = { legacy = false }, Modifier.padding(horizontal = 16.dp)) {
            Text(stringResource(R.string.sumh_inventory_back))
        }
        RulesTab(legacyRules, status, enabled, onLegacyRefresh, onLegacyClear, rulesIncomplete = true)
    } else SUMHInventoryView(collection, loading, cancelling, enabled, ::load,
        onCancel = { cancelling = true; job?.cancel() }, onLegacy = { legacy = true })
}

internal fun SUMHInventoryBytes.display(): String = utf8?.let { JSONObject.quote(it) } ?: hex

@Composable
internal fun SUMHInventoryView(
    collection: SUMHInventoryCollection, loading: Boolean, cancelling: Boolean, enabled: Boolean,
    onLoad: () -> Unit, onCancel: () -> Unit, onLegacy: () -> Unit,
) {
    var query by rememberSaveable { mutableStateOf("") }
    var queryLimit by remember { mutableStateOf(false) }
    var filter by rememberSaveable { mutableStateOf<SUMHInventoryCategory?>(null) }
    var selected by remember { mutableStateOf<SUMHInventoryEntry?>(null) }
    var diagnostics by rememberSaveable { mutableStateOf(false) }
    var showRaw by remember { mutableStateOf(false) }
    val context = LocalContext.current
    LaunchedEffect(collection.identity, collection.status) { selected = null }
    val visible = remember(collection.entries, filter, query) { collection.entries.filter { entry ->
        (filter == null || entry.category == filter) && (query.isEmpty() ||
            entry.paths.values.any { it.utf8?.contains(query, true) == true || it.hex.contains(query, true) } ||
            entry.id.contains(query) || entry.category.wire.contains(query, true))
    } }
    selected?.let { InventoryDetails(it, onDismiss = { selected = null }) }
    LazyColumn(Modifier.fillMaxSize().testTag("inventory_list"), contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp)) {
        item("status") { YukiPanel { Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(stringResource(R.string.sumh_inventory_title), style = MaterialTheme.typography.titleMedium)
            Text(stringResource(when (collection.status) {
                SUMHInventoryRead.NotLoaded -> R.string.sumh_inventory_not_loaded
                SUMHInventoryRead.Loading -> if (cancelling) R.string.sumh_inventory_cancelling else R.string.sumh_inventory_loading
                SUMHInventoryRead.Complete -> R.string.sumh_inventory_complete
                SUMHInventoryRead.Partial -> R.string.sumh_inventory_partial
                SUMHInventoryRead.Cancelled -> R.string.sumh_inventory_cancelled
                SUMHInventoryRead.Stale -> R.string.sumh_inventory_stale
                SUMHInventoryRead.Unsupported -> R.string.sumh_inventory_unsupported
                SUMHInventoryRead.Unavailable -> R.string.sumh_inventory_unavailable
                SUMHInventoryRead.Failed -> R.string.sumh_inventory_failed
            }), modifier = Modifier.testTag("inventory_status"))
            Text(stringResource(R.string.sumh_inventory_scope), style = MaterialTheme.typography.bodySmall)
            Text(stringResource(R.string.sumh_inventory_budget), style = MaterialTheme.typography.bodySmall)
            if (collection.status != SUMHInventoryRead.NotLoaded) Text(stringResource(R.string.sumh_inventory_progress,
                collection.entries.size, collection.requests, collection.completedCategories.size), style = MaterialTheme.typography.bodySmall)
            collection.reason?.let { reason -> inventoryReason(reason)?.let { message ->
                Text(message, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            } }
            if (loading) LinearProgressIndicator(Modifier.fillMaxWidth())
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                OutlinedButton(onClick = onLoad, enabled = enabled && !loading, modifier = Modifier.testTag("inventory_load")) {
                    Text(stringResource(R.string.sumh_inventory_load))
                }
                if (loading) OutlinedButton(onClick = onCancel, enabled = !cancelling, modifier = Modifier.testTag("inventory_cancel")) {
                    Text(stringResource(R.string.sumh_rules_cancel))
                }
                TextButton(onClick = onLegacy, enabled = !loading, modifier = Modifier.testTag("inventory_legacy")) {
                    Text(stringResource(R.string.sumh_inventory_legacy))
                }
            }
            if (collection.unsupportedCategories.isNotEmpty()) Text(stringResource(R.string.sumh_inventory_unsupported_categories,
                collection.unsupportedCategories.joinToString { it.wire }), style = MaterialTheme.typography.bodySmall)
            if (collection.identity != null || collection.reason != null) {
                TextButton(onClick = { diagnostics = !diagnostics }) { Text(stringResource(R.string.sumh_state_diagnostics)) }
                if (diagnostics) Text(buildString {
                    collection.identity?.let { identity ->
                        append("boot: ${identity.bootId}\ninstance: ${identity.instance}\n")
                        append("generation: ${identity.generation}\nsupported mask: ${identity.supportedMask}\n")
                    }
                    append("response bytes: ${collection.responseBytes}")
                    collection.reason?.let { append("\nreason: $it") }
                }, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
            }
        } } }
        collection.legacy?.let { raw -> item("legacy_raw") { YukiPanel {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.sumh_inventory_legacy_raw))
                TextButton(onClick = { showRaw = !showRaw }) { Text(stringResource(R.string.sumh_runtime_details)) }
                if (showRaw) {
                    InventoryBytes(raw, preview = true)
                    Text(stringResource(R.string.sumh_inventory_raw_limit), style = MaterialTheme.typography.bodySmall)
                    TextButton(onClick = { copyInventory(context, raw.hex) }) { Text(stringResource(R.string.sumh_inventory_copy_hex)) }
                }
            }
        } } }
        collection.legacyErrno?.takeIf { it != 0 }?.let { errno -> item("legacy_error") {
            Text(stringResource(R.string.sumh_inventory_legacy_error, errno), color = MaterialTheme.colorScheme.error)
        } }
        if (collection.entries.isNotEmpty()) {
            item("search") { OutlinedTextField(query, { queryLimit = !sumhSmallDraftFits(it); if (!queryLimit) query = it },
                modifier = Modifier.fillMaxWidth(), singleLine = true, label = { Text(stringResource(R.string.sumh_inventory_search)) },
                supportingText = if (queryLimit) { { SUMHSmallDraftLimit() } } else null) }
            item("filters") { FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(filter == null, { filter = null }, label = { Text(stringResource(R.string.sumh_runtime_filter_all)) })
                SUMHInventoryCategory.entries.filter { it.readable }.forEach { category ->
                    FilterChip(filter == category, { filter = category }, label = { Text(inventoryCategoryTitle(category)) })
                }
            } }
            items(visible, key = { "${collection.identity}:${it.key}" }) { entry -> YukiPanel {
                Column(Modifier.fillMaxWidth().clickable { selected = entry }.padding(16.dp).testTag("inventory_${entry.key}"),
                    verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(inventoryCategoryTitle(entry.category), style = MaterialTheme.typography.titleSmall)
                    entry.paths.forEach { (label, bytes) ->
                        Text(inventoryPathTitle(label), style = MaterialTheme.typography.labelSmall)
                        InventoryBytes(bytes, preview = true)
                    }
                    Text(inventoryOwner(entry.owner), style = MaterialTheme.typography.bodySmall)
                    if (entry.category == SUMHInventoryCategory.UserHide) Text(stringResource(R.string.sumh_inventory_hide_scope),
                        style = MaterialTheme.typography.bodySmall)
                }
            } }
            if (visible.isEmpty()) item("filtered_empty") { Text(stringResource(R.string.sumh_runtime_no_matches)) }
        } else if (collection.status == SUMHInventoryRead.Complete) item("complete_empty") {
            Text(stringResource(R.string.sumh_inventory_empty), modifier = Modifier.testTag("inventory_empty"))
        }
    }
}

@Composable private fun InventoryBytes(bytes: SUMHInventoryBytes, preview: Boolean = false) {
    Text(pluralStringResource(if (bytes.utf8 == null) R.plurals.sumh_inventory_hex else R.plurals.sumh_inventory_utf8, bytes.size, bytes.size),
        style = MaterialTheme.typography.labelSmall)
    val display = bytes.display()
    SelectionContainer { Text(if (preview && display.length > 4096) display.take(4096) + "…" else display,
        fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall,
        maxLines = if (preview) 3 else Int.MAX_VALUE, overflow = TextOverflow.Ellipsis) }
}

private fun copyInventory(context: Context, value: String) =
    (context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager)
        .setPrimaryClip(ClipData.newPlainText("SUMH", value))

@Composable private fun InventoryDetails(entry: SUMHInventoryEntry, onDismiss: () -> Unit) {
    val context = LocalContext.current
    YukiAlertDialog(onDismissRequest = onDismiss, title = { Text(inventoryCategoryTitle(entry.category)) }, text = {
        Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(inventoryOwner(entry.owner))
            Text(stringResource(R.string.sumh_inventory_identity_scope))
            Text("ID: ${entry.id}\n${entry.idKind}\ndefinition generation: ${entry.definitionGeneration}",
                fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
            entry.paths.forEach { (label, bytes) ->
                Text(inventoryPathTitle(label))
                InventoryBytes(bytes)
                bytes.utf8?.let { path -> TextButton(onClick = { copyInventory(context, path) }) {
                    Text(stringResource(R.string.sumh_inventory_copy_utf8))
                } }
                TextButton(onClick = { copyInventory(context, bytes.hex) }) { Text(stringResource(R.string.sumh_inventory_copy_hex)) }
            }
            entry.numbers.forEach { (key, value) -> Text("$key: $value", fontFamily = FontFamily.Monospace) }
            entry.type?.let { Text("type: $it") }
            entry.storageManaged?.let { Text(stringResource(R.string.sumh_inventory_storage_flag, it.toString())) }
            if (entry.category == SUMHInventoryCategory.UserHide) Text(stringResource(R.string.sumh_inventory_hide_scope))
        }
    }, confirmButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.close)) } })
}

@Composable private fun inventoryCategoryTitle(category: SUMHInventoryCategory): String = stringResource(when (category) {
    SUMHInventoryCategory.Mapping -> R.string.sumh_runtime_mapping
    SUMHInventoryCategory.Merge -> R.string.sumh_runtime_merge
    SUMHInventoryCategory.LegacyHide -> R.string.sumh_inventory_legacy_hide
    SUMHInventoryCategory.ManagedMaps -> R.string.sumh_state_maps
    SUMHInventoryCategory.TemporaryMaps -> R.string.sumh_inventory_temporary_maps
    SUMHInventoryCategory.Inject -> R.string.sumh_inventory_inject
    SUMHInventoryCategory.UserHide -> R.string.sumh_state_hide
    else -> R.string.sumh_inventory_title
})
@Composable private fun inventoryOwner(owner: SUMHInventoryOwner) = stringResource(when (owner) {
    SUMHInventoryOwner.Unattributed -> R.string.sumh_inventory_unattributed
    SUMHInventoryOwner.UserHide -> R.string.sumh_inventory_owner_hide
    SUMHInventoryOwner.ManagedMaps -> R.string.sumh_inventory_owner_maps
})
@Composable private fun inventoryPathTitle(key: String) = stringResource(when (key) {
    "visible_path" -> R.string.sumh_runtime_visible_path
    "backing_path" -> R.string.sumh_runtime_source_path
    "spoofed_path" -> R.string.sumh_inventory_spoofed_path
    else -> R.string.sumh_rules_runtime_path
})
@Composable private fun inventoryReason(reason: String) = when (reason) {
    "record_budget", "byte_budget", "time_budget" -> stringResource(R.string.sumh_inventory_budget_reached)
    "controller_schema_not_advertised" -> stringResource(R.string.sumh_inventory_no_schema)
    "future_inventory_schema" -> stringResource(R.string.sumh_inventory_future_schema)
    "additional_categories_not_understood" -> stringResource(R.string.sumh_inventory_future_categories)
    "cancelled" -> stringResource(R.string.sumh_inventory_cancelled)
    else -> null
}
