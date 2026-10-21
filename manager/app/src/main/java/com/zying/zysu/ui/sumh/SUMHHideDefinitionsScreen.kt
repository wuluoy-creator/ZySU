package com.zying.zysu.ui.sumh

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.ViewModel
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.compose.viewModel
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.sumh.util.*
import com.zying.zysu.ui.util.SnackbarController
import kotlinx.coroutines.*
import java.util.concurrent.atomic.AtomicBoolean

internal typealias HideDefinitionRead = suspend () -> HideDefinitionCollection
internal typealias HideDefinitionChange = suspend (List<String>, String, String) -> HideMutationResult
internal enum class HideDefinitionFilter { All, Enabled, Disabled, Attention }
internal data class HideDefinitionConfirmation(val action: String, val ids: List<String>, val sample: HideDefinitionCollection,
    val group: String? = null) {
    fun isCurrent(current: HideDefinitionCollection) = current.canMutate && current.revision == sample.revision && current.bootId == sample.bootId
}
internal fun filterHideDefinitions(rows: List<HideDefinition>, query: String, group: String?, filter: HideDefinitionFilter) = rows.filter { row ->
    (group == null || row.group == group) && listOf(row.path, row.name, row.note, row.group).any { it.contains(query, ignoreCase = true) } && when (filter) {
        HideDefinitionFilter.All -> true; HideDefinitionFilter.Enabled -> row.enabled; HideDefinitionFilter.Disabled -> !row.enabled
        HideDefinitionFilter.Attention -> row.state !in setOf("disabled", "bound", "pending")
    }
}

internal class HideDefinitionOperations : ViewModel() {
    var sample by mutableStateOf(HideDefinitionCollection()); private set
    var loading by mutableStateOf(false); private set
    var working by mutableStateOf(false); private set
    var result by mutableStateOf<HideMutationResult?>(null); private set
    var batch by mutableStateOf<HideDefinitionBatch?>(null); private set
    var progress by mutableIntStateOf(0); private set
    var total by mutableIntStateOf(0); private set
    var selection by mutableStateOf(emptySet<String>())
    val stop = AtomicBoolean(false)
    fun refresh(read: HideDefinitionRead) {
        if (loading || working) return
        loading = true
        viewModelScope.launch {
            try {
                val next = read()
                sample = if (!next.complete && next.rows.isEmpty() && sample.rows.isNotEmpty())
                    sample.copy(complete = false, error = next.error ?: "hide_collection_unknown") else next
                if (sample.complete) selection = selection.intersect(sample.rows.mapTo(HashSet()) { it.id })
            } catch (error: Exception) {
                if (error is CancellationException) throw error
                sample = sample.copy(complete = false, error = error.message ?: "hide_collection_unknown")
            } finally { loading = false }
        }
    }
    fun single(args: List<String>, revision: String, boot: String, change: HideDefinitionChange, read: HideDefinitionRead,
        confirmed: () -> Unit = {}) {
        if (working || loading) return
        working = true; result = null; batch = null; total = 0
        viewModelScope.launch {
            withContext(NonCancellable) {
                try {
                    result = try { change(args, revision, boot) } catch (error: Exception) { HideMutationResult(error = error.message ?: "hide_result_unknown") }
                    if (result?.succeeded == true) confirmed()
                } finally { working = false; refresh(read) }
            }
        }
    }
    fun selected(action: String, ids: List<String>, change: HideDefinitionChange, read: HideDefinitionRead,
        initial: HideDefinitionCollection = sample) {
        if (working || loading || !sample.canMutate || sample.managed != true || ids.isEmpty()) return
        if (initial.revision != sample.revision || initial.bootId != sample.bootId) return
        working = true; result = null; batch = null; stop.set(false); progress = 0; total = ids.distinct().size
        viewModelScope.launch {
            withContext(NonCancellable) {
                try {
                    batch = changeHideDefinitionSelection(action, ids, initial, change, { !stop.get() }, { done, count -> progress = done; total = count })
                    result = batch?.attempted?.lastOrNull()?.second
                } catch (error: Exception) { result = HideMutationResult(error = error.message ?: "hide_result_unknown") }
                finally { working = false; refresh(read) }
            }
        }
    }
}

/** New definition view shares the existing path/batch workspace and list renderer. */
@Composable internal fun SUMHUserHideWorkspace(enabled: Boolean, snackbar: SnackbarController,
    onSavingChanged: (Boolean) -> Unit, kernelAvailable: Boolean) {
    var legacy by rememberSaveable { mutableStateOf(false) }
    var working by remember { mutableStateOf(false) }
    val report: (Boolean) -> Unit = { working = it; onSavingChanged(it) }
    Column(Modifier.fillMaxSize()) {
        TextButton(onClick = { legacy = !legacy }, enabled = !working, modifier = Modifier.padding(horizontal = 12.dp)) {
            Text(stringResource(if (legacy) R.string.sumh_definitions_return else R.string.sumh_definitions_legacy))
        }
        if (legacy) {
            Text(stringResource(R.string.sumh_definitions_legacy_hint), Modifier.padding(horizontal = 16.dp), style = MaterialTheme.typography.bodySmall)
            UserHideRulesScreen(enabled, snackbar, report, kernelAvailable)
        } else HideDefinitionsScreen(enabled, report, tracker = SUMHManager.operationTracker)
    }
}

@Composable internal fun HideDefinitionsScreen(enabled: Boolean, onSaving: (Boolean) -> Unit,
    read: HideDefinitionRead = { SUMHManager.hideDefinitions() },
    change: HideDefinitionChange = SUMHManager::changeHideDefinition,
    operation: HideDefinitionOperations = viewModel(), tracker: SUMHOperationTracker? = null) {
    val context = LocalContext.current
    val drafts = rememberSUMHEditorDrafts()
    var transferring by remember { mutableStateOf(false) }
    val sample = operation.sample
    val ready = enabled && !transferring && !operation.loading && !operation.working && sample.canMutate
    val canRuntime = ready && sample.managed == true
    var query by rememberSaveable { mutableStateOf("") }
    var filter by rememberSaveable { mutableStateOf(HideDefinitionFilter.All) }
    var group by rememberSaveable { mutableStateOf<String?>(null) }
    var groupsOpen by remember { mutableStateOf(false) }
    var filterOpen by remember { mutableStateOf(false) }
    var selecting by rememberSaveable { mutableStateOf(false) }
    var editor by rememberSaveable { mutableStateOf<String?>(null) }
    var confirm by remember { mutableStateOf<HideDefinitionConfirmation?>(null) }
    var groupAction by remember { mutableStateOf<HideDefinitionConfirmation?>(null) }
    var editorAttempt by rememberSaveable { mutableStateOf<String?>(null) }
    var resultOpen by rememberSaveable { mutableStateOf(false) }
    val visible = remember(sample.rows, query, group, filter) { filterHideDefinitions(sample.rows, query, group, filter) }
    val byPath = sample.rows.associateBy { it.path }; val byId = sample.rows.associateBy { it.id }
    val selected = sample.rows.filter { it.id in operation.selection }
    val revision by SUMHManager.revision.collectAsState()
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    val latestRead by rememberUpdatedState(read)
    val latestSaving by rememberUpdatedState(onSaving)
    SideEffect { latestSaving(operation.working || transferring) }
    LaunchedEffect(revision) { operation.refresh(latestRead) }
    // The active composition consumes completion even after Activity recreation. A newer draft survives compare-and-delete.
    LaunchedEffect(editor, drafts.loaded, editor?.let { drafts.snapshot[it] }) {
        if (drafts.loaded && editor != null && drafts.snapshot[editor] == null) editor = null
    }
    DisposableEffect(lifecycle) {
        val observer = LifecycleEventObserver { _, event -> if (event == Lifecycle.Event.ON_RESUME) operation.refresh(latestRead) }
        lifecycle.addObserver(observer); onDispose { lifecycle.removeObserver(observer) }
    }
    BackHandler(selecting && !operation.working && editor == null && !resultOpen && confirm == null && groupAction == null) {
        selecting = false; operation.selection = emptySet()
    }
    fun open(row: HideDefinition?) {
        if (!ready || !drafts.loaded) return
        val initial = HideDefinitionDraft.initial(row, requireNotNull(sample.revision))
        if (drafts.ensure(initial.key, initial.record())) { editorAttempt = null; editor = initial.key }
    }
    fun copy(rows: List<HideDefinition>) {
        (context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager).setPrimaryClip(
            ClipData.newPlainText(context.getString(R.string.sumh_user_hide_title), rows.joinToString("\n") { it.path }))
    }
    Column(Modifier.fillMaxSize()) {
        if (tracker != null) SUMHOperationHistory(tracker, !operation.working && !transferring)
        FlowRow(Modifier.fillMaxWidth().padding(horizontal = 16.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            HideTransferControls(operation.selection, enabled && !operation.working && !operation.loading,
                onBusy = { transferring = it }, onApplied = { operation.refresh(read) }, runtime = read, change = change)
            Box {
                TextButton(onClick = { filterOpen = true }) { Text(stringResource(filter.label)); Text(" (${visible.size})") }
                DropdownMenu(filterOpen, { filterOpen = false }) { HideDefinitionFilter.entries.forEach { choice ->
                    DropdownMenuItem(text = { Text(stringResource(choice.label)) }, onClick = { filter = choice; filterOpen = false })
                } }
            }
            TextButton(onClick = { groupsOpen = true }) { Text(group?.ifEmpty { stringResource(R.string.sumh_definitions_ungrouped) } ?: stringResource(R.string.sumh_definitions_groups)) }
            TextButton(onClick = { selecting = !selecting; operation.selection = emptySet() }, enabled = !operation.working) {
                Text(stringResource(if (selecting) R.string.sumh_rules_done else R.string.sumh_rules_select))
            }
            TextButton(onClick = { operation.refresh(read) }, enabled = !operation.working && !operation.loading,
                modifier = Modifier.testTag("hide_definitions_refresh")) { Text(stringResource(R.string.sumh_rules_refresh)) }
            FilledTonalButton(onClick = { open(null) }, enabled = canRuntime && drafts.loaded,
                modifier = Modifier.testTag("hide_definitions_add")) { Text(stringResource(R.string.sumh_user_hide_add)) }
        }
        OutlinedTextField(query, { if (validHideDefinitionText(it, 4095)) query = it }, Modifier.fillMaxWidth().padding(horizontal = 16.dp),
            label = { Text(stringResource(R.string.sumh_definitions_search)) }, singleLine = true)
        if (operation.loading || operation.working) LinearProgressIndicator(Modifier.fillMaxWidth().padding(top = 8.dp))
        Column(Modifier.padding(horizontal = 16.dp).semantics { liveRegion = LiveRegionMode.Polite }) {
            SUMHEditorDraftStatus(drafts)
            if (!sample.complete || sample.error != null) Text(stringResource(R.string.sumh_definitions_incomplete), color = MaterialTheme.colorScheme.error,
                modifier = Modifier.testTag("hide_definitions_incomplete"))
            sample.error?.let { Text(it.take(1500), style = MaterialTheme.typography.bodySmall) }
            if (sample.complete && sample.managed != true) Text(stringResource(R.string.sumh_definitions_no_managed), style = MaterialTheme.typography.bodySmall)
            if (operation.working && operation.total > 0) Row(verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(R.string.sumh_rules_progress, operation.progress, operation.total), Modifier.weight(1f))
                TextButton(onClick = { operation.stop.set(true) }) { Text(stringResource(R.string.sumh_rules_stop)) }
            }
            if (operation.result != null || operation.batch != null) TextButton(onClick = { resultOpen = true }, modifier = Modifier.testTag("hide_definitions_result")) {
                Text(stringResource(R.string.sumh_rules_last_result))
            }
            if (group != null) FlowRow {
                TextButton(onClick = { groupAction = HideDefinitionConfirmation("enable-group", sample.rows.filter { it.group == group }.map { it.id }, sample, group) }, enabled = canRuntime && visible.isNotEmpty()) { Text(stringResource(R.string.sumh_definitions_enable_group)) }
                TextButton(onClick = { groupAction = HideDefinitionConfirmation("disable-group", sample.rows.filter { it.group == group }.map { it.id }, sample, group) }, enabled = canRuntime && visible.isNotEmpty()) { Text(stringResource(R.string.sumh_definitions_disable_group)) }
            }
            if (selecting) FlowRow {
                Text(stringResource(R.string.sumh_user_rules_selected, operation.selection.size, visible.count { it.id in operation.selection }))
                TextButton(onClick = { operation.selection = toggleVisibleUserHideSelection(operation.selection, visible.mapTo(HashSet()) { it.id }) }, enabled = !operation.working) { Text(stringResource(R.string.sumh_user_rules_select_visible)) }
                TextButton(onClick = { operation.selection = invertVisibleUserHideSelection(operation.selection, visible.mapTo(HashSet()) { it.id }) }, enabled = !operation.working) { Text(stringResource(R.string.sumh_rules_invert_visible)) }
            }
        }
        val rendered = visible.map { SUMHUiAdapter.ActiveRule("hide", it.path, isUserDefined = true, hideState = it.runtime?.binding, persisted = true) }
        UserHideRuleList(rendered, selecting, operation.selection, !operation.working && drafts.loaded, Modifier.weight(1f),
            onSelect = { operation.selection = if (it in operation.selection) operation.selection - it else operation.selection + it },
            onEdit = { open(byPath.getValue(it.src)) }, onRemove = { confirm = HideDefinitionConfirmation("remove-id", listOf(byPath.getValue(it.src).id), sample) },
            onRetry = { operation.selected("retry", listOf(byPath.getValue(it.src).id), change, read) }, onCopy = { copy(listOf(byPath.getValue(it.src))) },
            retryEnabled = canRuntime, onLongSelect = { selecting = true; operation.selection = operation.selection + byPath.getValue(it.src).id },
            showEmpty = sample.complete && !operation.loading, filtered = query.isNotEmpty() || group != null || filter != HideDefinitionFilter.All,
            onClearFilter = { query = ""; group = null; filter = HideDefinitionFilter.All }, keyOf = { byPath.getValue(it.src).id },
            headline = { rule -> val row = byPath.getValue(rule.src)
                if (row.name.isNotEmpty()) Text(row.name, style = MaterialTheme.typography.titleSmall)
                Text(row.path, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodyMedium)
            }, supporting = { rule -> val row = byPath.getValue(rule.src)
                Text(stringResource(if (row.enabled) R.string.sumh_definitions_enabled else R.string.sumh_definitions_disabled) + " · " + hideDefinitionStatus(row), style = MaterialTheme.typography.bodySmall)
                if (row.group.isNotEmpty()) Text(row.group, style = MaterialTheme.typography.labelMedium)
            }, canRetry = { byPath.getValue(it.src).enabled }, canRemove = { canRuntime }, extraMenu = { rule, close ->
                val row = byPath.getValue(rule.src)
                DropdownMenuItem(text = { Text(stringResource(if (row.enabled) R.string.sumh_definitions_disable else R.string.sumh_definitions_enable)) },
                    enabled = canRuntime, onClick = { close(); operation.selected(if (row.enabled) "disable" else "enable", listOf(row.id), change, read) })
            })
        if (selecting && operation.selection.isNotEmpty()) Surface(tonalElevation = 3.dp) {
            FlowRow(Modifier.fillMaxWidth().padding(horizontal = 16.dp)) {
                listOf("enable", "disable", "retry", "remove-id").forEach { action ->
                    TextButton(onClick = { if (action == "remove-id") confirm = HideDefinitionConfirmation(action, selected.map { it.id }, sample)
                        else operation.selected(action, selected.map { it.id }, change, read) },
                        enabled = canRuntime && selected.isNotEmpty() && (action != "retry" || selected.all { it.enabled })) { Text(stringResource(hideActionLabel(action))) }
                }
                TextButton(onClick = { copy(selected) }) { Text(stringResource(R.string.sumh_rules_copy)) }
            }
        }
    }
    if (groupsOpen) YukiAlertDialog(onDismissRequest = { groupsOpen = false }, title = { Text(stringResource(R.string.sumh_definitions_groups)) },
        text = { LazyColumn(Modifier.heightIn(max = 360.dp)) {
            item { TextButton(onClick = { group = null; groupsOpen = false }) { Text(stringResource(R.string.sumh_definitions_all)) } }
            items(sample.rows.map { it.group }.distinct().sorted()) { name -> TextButton(onClick = { group = name; groupsOpen = false }) {
                Text(name.ifEmpty { stringResource(R.string.sumh_definitions_ungrouped) })
            } }
        } }, confirmButton = { TextButton(onClick = { groupsOpen = false }) { Text(stringResource(android.R.string.cancel)) } })
    confirm?.let { reviewed -> YukiAlertDialog(onDismissRequest = { confirm = null }, title = { Text(stringResource(R.string.sumh_user_rules_remove_count, reviewed.ids.size)) },
        text = { Column {
            if (!reviewed.isCurrent(sample)) Text(stringResource(R.string.sumh_definitions_confirmation_changed), color = MaterialTheme.colorScheme.error)
            LazyColumn(Modifier.heightIn(max = 320.dp)) { items(reviewed.ids) { id -> Text(reviewed.sample.rows.find { it.id == id }?.path ?: id) } }
        } },
        confirmButton = { TextButton(enabled = canRuntime && reviewed.isCurrent(sample), onClick = { confirm = null; operation.selected(reviewed.action, reviewed.ids, change, read, reviewed.sample) }) { Text(stringResource(R.string.delete)) } },
        dismissButton = { TextButton(onClick = { confirm = null }) { Text(stringResource(android.R.string.cancel)) } }) }
    groupAction?.let { reviewed ->
        val members = reviewed.sample.rows.filter { it.id in reviewed.ids }
        YukiAlertDialog(onDismissRequest = { groupAction = null }, title = { Text(stringResource(hideActionLabel(reviewed.action))) },
            text = { Column { Text(stringResource(R.string.sumh_definitions_group_scope, members.size));
                if (!reviewed.isCurrent(sample)) Text(stringResource(R.string.sumh_definitions_confirmation_changed), color = MaterialTheme.colorScheme.error)
                LazyColumn(Modifier.heightIn(max = 280.dp)) { items(members, key = { it.id }) { Text(it.path) } } } },
            confirmButton = { TextButton(enabled = canRuntime && reviewed.isCurrent(sample) && members.isNotEmpty(), onClick = {
                groupAction = null
                val checkedChange: HideDefinitionChange = { args, revision, boot ->
                    val result = change(args, revision, boot)
                    if (result.succeeded && (result.value?.affected != members.size ||
                            result.value.items.any { item -> item.id !in reviewed.ids })) result.copy(error = "hide_group_membership_unknown") else result
                }
                operation.single(listOf("hide", reviewed.action, requireNotNull(reviewed.group), requireNotNull(reviewed.sample.revision)), requireNotNull(reviewed.sample.revision), requireNotNull(reviewed.sample.bootId), checkedChange, read)
            }) { Text(stringResource(android.R.string.ok)) } }, dismissButton = { TextButton(onClick = { groupAction = null }) { Text(stringResource(android.R.string.cancel)) } })
    }
    editor?.let { key -> HideDefinitionEditor(key, drafts, sample, ready, operation.working, operation.result.takeIf { editorAttempt == key }, onDismiss = { editor = null }, onSubmit = { submitted ->
        editorAttempt = key
        operation.single(submitted.arguments(), submitted.revision, requireNotNull(sample.bootId), change, read) {
            drafts.forget(submitted.key, submitted.record())
        }
    }) }
    if (resultOpen) HideDefinitionResultDialog(operation.result, operation.batch) { resultOpen = false }
}

private val HideDefinitionFilter.label get() = when (this) {
    HideDefinitionFilter.All -> R.string.sumh_definitions_all; HideDefinitionFilter.Enabled -> R.string.sumh_definitions_enabled
    HideDefinitionFilter.Disabled -> R.string.sumh_definitions_disabled; HideDefinitionFilter.Attention -> R.string.sumh_definitions_attention
}
internal fun hideActionLabel(action: String) = when (action) {
    "enable" -> R.string.sumh_definitions_enable; "disable" -> R.string.sumh_definitions_disable
    "enable-group" -> R.string.sumh_definitions_enable_group; "disable-group" -> R.string.sumh_definitions_disable_group
    "retry" -> R.string.sumh_hide_retry; else -> R.string.delete
}
@Composable private fun hideDefinitionStatus(row: HideDefinition) = stringResource(when (row.state) {
    "bound" -> R.string.sumh_definitions_bound; "pending" -> R.string.sumh_definitions_pending
    "binding_error" -> R.string.sumh_definitions_binding_error; "globally_suspended" -> R.string.sumh_definitions_global_pause
    "disabled" -> R.string.sumh_definitions_absent; "disabled_pending_removal" -> R.string.sumh_definitions_residue
    "unregistered" -> R.string.sumh_definitions_unregistered; else -> R.string.sumh_definitions_unknown
})
