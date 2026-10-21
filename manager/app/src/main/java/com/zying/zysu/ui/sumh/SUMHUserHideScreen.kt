package com.zying.zysu.ui.sumh

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.listSaver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.toggleableState
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.state.ToggleableState
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.ViewModel
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.viewmodel.compose.viewModel
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.sumh.util.*
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as Controller
import com.zying.zysu.ui.util.SnackbarController
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.concurrent.atomic.AtomicBoolean
import java.util.UUID
import java.io.File

internal enum class UserHideFilter { All, Effective, Attention }
internal fun userHideNeedsAttention(rule: Controller.ActiveRule): Boolean = rule.hideState !in listOf(1, 2)
internal fun userHideCanRetry(rule: Controller.ActiveRule): Boolean = rule.persisted
internal fun filterUserHideRules(rules: List<Controller.ActiveRule>, query: String, filter: UserHideFilter) = rules.filter {
    it.src.contains(query.trim(), ignoreCase = true) && when (filter) {
        UserHideFilter.All -> true
        UserHideFilter.Effective -> it.hideState == 2
        UserHideFilter.Attention -> userHideNeedsAttention(it)
    }
}
internal fun remainingUserHideDraft(draft: String, result: SUMHHideBatchResult): String =
    if (result.action != SUMHHideAction.Add) draft else {
        val completed = result.items.filter { it.succeeded }.mapTo(HashSet()) { it.path }
        draft.lineSequence().filter { it !in completed }.joinToString("\n")
    }

internal class UserHideOperationStore : ViewModel() {
    private var draftStore: UserHideDrafts? = null
    fun drafts(directory: File): UserHideDrafts = draftStore ?: UserHideDrafts.forDirectory(directory).also { draftStore = it }
    var working by mutableStateOf(false)
    var progress by mutableIntStateOf(0)
    var total by mutableIntStateOf(0)
    val stop = AtomicBoolean(false)
    var batch by mutableStateOf<SUMHHideBatchResult?>(null)
    var batchVersion by mutableStateOf<String?>(null)
    var selection by mutableStateOf(emptySet<String>())
    var result by mutableStateOf<UserHideOperationResult?>(null)
    var editError by mutableStateOf<String?>(null)
    var edited by mutableStateOf<String?>(null)
    var editedPath: String? = null
}

internal typealias UserHideChange = suspend (
    SUMHHideAction, List<String>, () -> Boolean, suspend (Int, Int) -> Unit,
) -> SUMHHideBatchResult

@Composable
internal fun UserHideRulesScreen(
    enabled: Boolean, snackbar: SnackbarController, onSavingChanged: (Boolean) -> Unit, kernelAvailable: Boolean,
    loadRules: suspend () -> List<Controller.ActiveRule> = Controller::userHideStates,
    changeRules: UserHideChange = Controller::changeUserHideRules,
) {
    val context = LocalContext.current
    val resources = LocalResources.current
    val scope = rememberCoroutineScope()
    val operation: UserHideOperationStore = viewModel()
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    val savingCallback by rememberUpdatedState(onSavingChanged)
    val revision by SUMHManager.revision.collectAsState()
    var refresh by remember { mutableIntStateOf(0) }
    var rules by remember { mutableStateOf(emptyList<Controller.ActiveRule>()) }
    var loading by remember { mutableStateOf(true) }
    var loaded by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var savedResult by rememberSaveable(stateSaver = listSaver<UserHideOperationResult?, String>(save = { saveUserHideResult(it) }, restore = ::restoreUserHideResult)) {
        mutableStateOf<UserHideOperationResult?>(null)
    }
    val result = operation.result ?: savedResult
    var details by rememberSaveable { mutableStateOf(false) }
    var selecting by rememberSaveable { mutableStateOf(false) }
    var selection by operation::selection
    var query by rememberSaveable { mutableStateOf("") }
    var filter by rememberSaveable { mutableStateOf(UserHideFilter.All) }
    var menu by remember { mutableStateOf(false) }
    var filterMenu by remember { mutableStateOf(false) }
    var editor by rememberSaveable { mutableStateOf<String?>(null) }
    var original by rememberSaveable { mutableStateOf<String?>(null) }
    val drafts = remember(operation) { operation.drafts(File(context.noBackupFilesDir, "sumh-drafts")) }
    val draft = when (editor) { "edit" -> drafts.edit(requireNotNull(original)); "import" -> drafts.bulk; else -> drafts.add }
    fun updateDraft(value: String) {
        when (editor) { "edit" -> drafts.update(requireNotNull(original), value); "import" -> drafts.updateBulk(value); else -> drafts.updateAdd(value) }
    }
    val importRetryPaths = drafts.retryPaths
    var fileChunks by remember { mutableStateOf<SUMHHideFileChunks?>(null) }
    var pendingChunk by remember { mutableStateOf<String?>(null) }
    var readingChunk by remember { mutableStateOf(false) }
    var fileError by remember { mutableStateOf<String?>(null) }
    fun readChunk() {
        val chunks = fileChunks ?: return
        if (readingChunk || drafts.bulk.isNotEmpty()) return
        readingChunk = true
        scope.launch {
            try {
                val next = pendingChunk ?: withContext(Dispatchers.IO) { chunks.next() }
                pendingChunk = next
                if (drafts.updateBulk(next)) pendingChunk = null
                fileError = null
            } catch (e: Exception) {
                if (e is CancellationException) throw e
                fileError = resources.getString(R.string.sumh_draft_file_failed, e.localizedMessage.orEmpty())
                withContext(Dispatchers.IO) { runCatching { chunks.close() } }
                fileChunks = null
            } finally { readingChunk = false }
        }
    }
    val chooseFile = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        if (uri != null && drafts.bulk.isEmpty() && !readingChunk) {
            readingChunk = true
            scope.launch {
                var opened: SUMHHideFileChunks? = null
                try {
                    withContext(Dispatchers.IO) {
                        fileChunks?.close()
                        opened = SUMHHideFileChunks(requireNotNull(context.contentResolver.openInputStream(uri)))
                    }
                    fileChunks = opened
                    pendingChunk = null
                    readingChunk = false
                    readChunk()
                } catch (e: Exception) {
                    withContext(NonCancellable + Dispatchers.IO) { runCatching { opened?.close() } }
                    if (e is CancellationException) throw e
                    fileError = resources.getString(R.string.sumh_draft_file_failed, e.localizedMessage.orEmpty())
                    fileChunks = null
                    readingChunk = false
                }
            }
        }
    }
    DisposableEffect(fileChunks) {
        val chunks = fileChunks
        onDispose { scope.launch(NonCancellable + Dispatchers.IO) { runCatching { chunks?.close() } } }
    }
    var dismissedResult by rememberSaveable { mutableStateOf(false) }
    var remove by remember { mutableStateOf<List<String>?>(null) }
    val working = operation.working
    val canEdit = enabled && loaded && drafts.loaded && !loading && !working && error == null
    val visible = filterUserHideRules(rules, query, filter)
    val selected = rules.filter { it.src in selection }
    val visiblePaths = visible.mapTo(HashSet()) { it.src }
    val listState = rememberLazyListState()
    var lastQuery by rememberSaveable { mutableStateOf(query) }
    var lastFilter by rememberSaveable { mutableStateOf(filter) }
    LaunchedEffect(query, filter) {
        if (query != lastQuery || filter != lastFilter) listState.scrollToItem(0)
        lastQuery = query; lastFilter = filter
    }
    BackHandler(enabled = selecting && !working && editor == null && remove == null && !details && !menu && !filterMenu) {
        selecting = false; selection = emptySet()
    }

    SideEffect { savingCallback(working); operation.result?.let { savedResult = it } }
    DisposableEffect(lifecycle) {
        val observer = LifecycleEventObserver { _, event -> if (event == Lifecycle.Event.ON_RESUME) refresh++ }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer) }
    }
    LaunchedEffect(revision, refresh, working) {
        if (!working) {
            loading = true
            try {
                rules = loadRules()
                selection = selection.intersect(rules.mapTo(HashSet()) { it.src })
                loaded = true; error = null
            } catch (e: Exception) {
                if (e is CancellationException) throw e
                error = e.message ?: resources.getString(R.string.operation_failed)
            } finally { loading = false }
        }
    }
    var consumedBatch by rememberSaveable { mutableStateOf<String?>(null) }
    LaunchedEffect(operation.batchVersion) {
        operation.batch?.takeIf { operation.batchVersion != consumedBatch }?.let {
            dismissedResult = false
            if (editor == "add" || editor == "import") {
                val remaining = remainingUserHideDraft(draft, it)
                val complete = it.items.all { item -> item.succeeded } &&
                    (editor == "add" || previewSUMHHideImport(draft, emptySet()).lines.none { line -> line.error != null })
                if (editor == "import") {
                    if (!drafts.finishBulk(if (complete) "" else remaining,
                            it.items.filterNot { item -> item.succeeded }.map { item -> item.path })) return@LaunchedEffect
                }
                else updateDraft(if (complete) "" else remaining)
                if (complete && (editor != "import" || (fileChunks?.hasMore != true && pendingChunk == null))) editor = null
            }
            consumedBatch = operation.batchVersion
        }
    }
    var consumedEdit by rememberSaveable { mutableStateOf<String?>(null) }
    LaunchedEffect(operation.edited) {
        if (operation.edited != null && operation.edited != consumedEdit) {
            operation.editedPath?.let(drafts::forget)
            editor = null; consumedEdit = operation.edited
        }
    }
    fun copy(paths: List<String>) {
        if (paths.isEmpty()) return
        (context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager).setPrimaryClip(
            ClipData.newPlainText(resources.getString(R.string.sumh_user_hide_title), paths.joinToString("\n")))
        scope.launch { snackbar.showSnackbar(resources.getString(R.string.sumh_user_rules_copied, paths.size)) }
    }
    fun mutate(action: SUMHHideAction, paths: List<String>) {
        if (!canEdit || paths.isEmpty() || (action == SUMHHideAction.Retry && !kernelAvailable)) return
        operation.working = true; operation.stop.set(false); operation.progress = 0; operation.total = paths.distinct().size
        operation.editError = null
        savingCallback(true)
        scope.launch(start = CoroutineStart.UNDISPATCHED) {
            withContext(NonCancellable) {
                try {
                    val outcome = try {
                        changeRules(action, paths, { !operation.stop.get() },
                            { done, total -> withContext(Dispatchers.Main) { operation.progress = done; operation.total = total } })
                    } catch (e: Exception) {
                        if (e is CancellationException) throw e
                        SUMHHideBatchResult(action, paths.distinct().map { SUMHHideItemResult(it, e.message ?: "Operation failed") })
                    }
                    operation.batch = outcome; operation.batchVersion = UUID.randomUUID().toString(); operation.result = UserHideOperationResult.from(outcome)
                    operation.editError = outcome.items.firstOrNull { !it.succeeded }?.error
                } finally { operation.working = false; savingCallback(false) }
            }
        }
    }
    fun edit(old: String, path: String) {
        if (!canEdit) return
        operation.working = true; operation.total = 0; operation.editError = null; savingCallback(true)
        scope.launch(start = CoroutineStart.UNDISPATCHED) {
            withContext(NonCancellable) {
                try {
                    val outcome = Controller.editUserHideRule(old, path)
                    if (outcome.error == null) {
                        operation.editedPath = old
                        operation.edited = UUID.randomUUID().toString()
                    }
                    else operation.editError = resources.getString(R.string.sumh_rules_edit_failed, outcome.error)
                } catch (e: Exception) {
                    if (e is CancellationException) throw e
                    operation.editError = resources.getString(R.string.sumh_rules_edit_failed, e.message.orEmpty())
                } finally { operation.working = false; savingCallback(false) }
            }
        }
    }
    fun openEditor(path: String? = null, bulk: Boolean = false) {
        original = path
        operation.editError = null
        editor = if (path != null) "edit" else if (bulk) "import" else "add"
    }

    Column(Modifier.fillMaxSize()) {
        FlowRow(Modifier.fillMaxWidth().padding(horizontal = 16.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            Box {
                TextButton(onClick = { filterMenu = true }) { Text(stringResource(filter.title, visible.size)); YukiIcon(Icons.Filled.ArrowDropDown, null) }
                DropdownMenu(filterMenu, { filterMenu = false }) {
                    UserHideFilter.entries.forEach { choice ->
                        DropdownMenuItem(text = { Text(stringResource(choice.title, filterUserHideRules(rules, query, choice).size)) },
                            onClick = { filter = choice; filterMenu = false })
                    }
                }
            }
            TextButton(onClick = { selecting = !selecting; selection = emptySet() }, enabled = !working && rules.isNotEmpty()) {
                Text(stringResource(if (selecting) R.string.sumh_rules_done else R.string.sumh_rules_select))
            }
            Box {
                IconButton(onClick = { menu = true }) { YukiIcon(Icons.Filled.MoreVert, stringResource(R.string.sumh_rules_more)) }
                DropdownMenu(menu, { menu = false }) {
                    DropdownMenuItem(text = { Text(stringResource(R.string.sumh_rules_import)) }, enabled = !working && drafts.loaded,
                        onClick = { menu = false; openEditor(bulk = true) })
                    DropdownMenuItem(text = { Text(stringResource(R.string.sumh_user_rules_copy_visible)) }, enabled = visible.isNotEmpty(),
                        onClick = { menu = false; copy(visible.filter { it.persisted }.map { it.src }) })
                    DropdownMenuItem(text = { Text(stringResource(R.string.sumh_rules_refresh)) }, enabled = !working && !loading,
                        onClick = { menu = false; refresh++ })
                    if (result != null) DropdownMenuItem(text = { Text(stringResource(R.string.sumh_rules_last_result)) },
                        onClick = { menu = false; details = true })
                }
            }
            FilledTonalButton(onClick = { openEditor() }, enabled = !working && drafts.loaded) {
                YukiIcon(Icons.Filled.Add, null); Spacer(Modifier.width(4.dp)); Text(stringResource(R.string.sumh_user_hide_add))
            }
        }
        OutlinedTextField(query, { if (it.toByteArray(Charsets.UTF_8).size < 4096) query = it }, Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
            singleLine = true, label = { Text(stringResource(R.string.sumh_user_rules_search)) },
            leadingIcon = { YukiIcon(Icons.Filled.Search, null) }, trailingIcon = {
                if (query.isNotEmpty()) IconButton(onClick = { query = "" }) { YukiIcon(Icons.Filled.Close, stringResource(R.string.sumh_user_rules_clear_search)) }
            })
        if (loading || working) LinearProgressIndicator(Modifier.fillMaxWidth())
        if (editor == null) UserHideDraftStatus(drafts, Modifier.padding(horizontal = 16.dp))
        if (working && operation.total > 0) Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.sumh_rules_progress, operation.progress, operation.total), Modifier.weight(1f), style = MaterialTheme.typography.bodySmall)
            TextButton(onClick = { operation.stop.set(true) }) { Text(stringResource(R.string.sumh_rules_stop)) }
        }
        error?.let {
            Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(it, Modifier.weight(1f), color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
                TextButton(onClick = { refresh++ }, enabled = !loading && !working) { Text(stringResource(R.string.retry)) }
            }
        }
        if (!kernelAvailable) Text(stringResource(R.string.sumh_user_rules_kernel_unavailable), Modifier.padding(horizontal = 16.dp),
            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        result?.takeUnless { dismissedResult }?.let {
            Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                TextButton(onClick = { details = true }, modifier = Modifier.weight(1f)) {
                    Text(stringResource(R.string.sumh_user_rules_result, it.completedCount, it.failedCount))
                    YukiIcon(Icons.Filled.ChevronRight, null)
                }
                IconButton(onClick = { dismissedResult = true }) { YukiIcon(Icons.Filled.Close, stringResource(R.string.sumh_rules_dismiss_result)) }
            }
        }
        if (selecting) FlowRow(Modifier.fillMaxWidth().padding(horizontal = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp), itemVerticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.sumh_user_rules_selected, selection.size, selected.count { it in visible }), style = MaterialTheme.typography.labelMedium)
            TextButton(onClick = { selection = toggleVisibleUserHideSelection(selection, visiblePaths) }, enabled = !working && visiblePaths.isNotEmpty()) {
                Text(stringResource(if (visiblePaths.isNotEmpty() && selection.containsAll(visiblePaths)) R.string.sumh_rules_deselect_visible else R.string.sumh_user_rules_select_visible))
            }
            TextButton(onClick = { selection = invertVisibleUserHideSelection(selection, visiblePaths) }, enabled = !working && visiblePaths.isNotEmpty()) {
                Text(stringResource(R.string.sumh_rules_invert_visible))
            }
        }
        UserHideRuleList(visible, selecting, selection, !working && drafts.loaded, Modifier.weight(1f),
            onSelect = { path -> selection = if (path in selection) selection - path else selection + path },
            onEdit = { openEditor(it.src) }, onRemove = { remove = listOf(it.src) },
            onRetry = { mutate(SUMHHideAction.Retry, listOf(it.src)) }, onCopy = { copy(listOf(it.src)) }, retryEnabled = canEdit && kernelAvailable,
            onLongSelect = { selecting = true; selection = selection + it.src }, state = listState,
            showEmpty = !loading && error == null, filtered = query.isNotEmpty() || filter != UserHideFilter.All,
            onClearFilter = { query = ""; filter = UserHideFilter.All })
        if (selecting && selection.isNotEmpty()) Surface(tonalElevation = 3.dp) {
            FlowRow(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TextButton(onClick = { copy(selected.map { it.src }) }) { Text(stringResource(R.string.sumh_rules_copy)) }
                TextButton(onClick = { mutate(SUMHHideAction.Retry, selected.filter { it.persisted }.map { it.src }) },
                    enabled = canEdit && kernelAvailable && selected.any { it.persisted }) { Text(stringResource(R.string.retry)) }
                TextButton(onClick = { remove = selected.map { it.src } }, enabled = canEdit,
                    colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error)) { Text(stringResource(R.string.delete)) }
                TextButton(onClick = { selection = emptySet() }, enabled = !working) { Text(stringResource(R.string.sumh_user_rules_clear_selection)) }
            }
        }
    }
    editor?.let { mode ->
        UserHideRuleEditor(original, draft, mode == "import", working, canEdit,
            rules.filter { it.persisted }.mapTo(HashSet()) { it.src }, operation.editError ?: error,
            progress = operation.progress, total = operation.total,
            retryPaths = importRetryPaths.toSet(),
            inputEnabled = drafts.loaded && !readingChunk,
            draftStatus = {
                UserHideDraftStatus(drafts)
                if (mode == "import") {
                    Text(stringResource(R.string.sumh_draft_file_hint), style = MaterialTheme.typography.bodySmall)
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        TextButton(enabled = drafts.loaded && !working && !readingChunk && drafts.bulk.isEmpty(),
                            onClick = { chooseFile.launch("*/*") }) { Text(stringResource(R.string.sumh_draft_import_file)) }
                        if (fileChunks?.hasMore == true || pendingChunk != null) TextButton(
                            enabled = drafts.loaded && !working && !readingChunk && drafts.bulk.isEmpty(), onClick = ::readChunk) {
                            Text(stringResource(R.string.sumh_draft_next_chunk))
                        }
                    }
                    if (readingChunk) LinearProgressIndicator(Modifier.fillMaxWidth())
                    fileError?.let { Text(it, color = MaterialTheme.colorScheme.error, modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite }) }
                }
            },
            onDraftChange = { updateDraft(it); operation.editError = null }, onDismiss = { editor = null }, onStop = { operation.stop.set(true) }, onSubmit = { paths ->
                if (mode == "edit") edit(requireNotNull(original), paths.single()) else mutate(SUMHHideAction.Add, paths)
            })
    }
    remove?.let { paths ->
        YukiAlertDialog(onDismissRequest = { remove = null }, title = { Text(stringResource(R.string.sumh_user_rules_remove_count, paths.size)) },
            text = { LazyColumn(Modifier.heightIn(max = 320.dp)) { items(paths) { Text(it, Modifier.padding(vertical = 4.dp), fontFamily = FontFamily.Monospace) } } },
            confirmButton = { TextButton(enabled = canEdit, onClick = { remove = null; mutate(SUMHHideAction.Remove, paths) }) { Text(stringResource(R.string.sumh_user_hide_remove)) } },
            dismissButton = { TextButton(onClick = { remove = null }) { Text(stringResource(R.string.sumh_rules_cancel)) } })
    }
    if (details && result != null) YukiAlertDialog(onDismissRequest = { details = false },
        title = { Text(stringResource(R.string.sumh_user_rules_result, result.completedCount, result.failedCount)) },
        text = { LazyColumn(Modifier.heightIn(max = 400.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            item { Text(stringResource(R.string.sumh_user_rules_result_note)) }
            if (result.omittedFailures > 0) item { Text(stringResource(R.string.sumh_user_rules_result_omitted, result.omittedFailures)) }
            if (result.shortenedErrors) item { Text(stringResource(R.string.sumh_user_rules_result_shortened)) }
            items(result.failures) { Text("${it.path}\n${it.error}", style = MaterialTheme.typography.bodySmall) }
        } }, confirmButton = { TextButton(onClick = { details = false }) { Text(stringResource(R.string.sumh_rules_done)) } },
        dismissButton = { if (result.failures.isNotEmpty()) TextButton(enabled = canEdit && (result.action != SUMHHideAction.Retry || kernelAvailable), onClick = {
            details = false
            if (result.action == SUMHHideAction.Remove) remove = result.failures.map { it.path } else mutate(result.action, result.failures.map { it.path })
        }) { Text(stringResource(R.string.sumh_user_rules_retry_failed, result.failures.size)) } })
}

private val UserHideFilter.title: Int get() = when (this) {
    UserHideFilter.All -> R.string.sumh_user_rules_filter_all
    UserHideFilter.Effective -> R.string.sumh_user_rules_filter_effective
    UserHideFilter.Attention -> R.string.sumh_user_rules_filter_attention
}

@Composable
internal fun UserHideRuleList(
    rules: List<Controller.ActiveRule>, selecting: Boolean, selection: Set<String>, enabled: Boolean, modifier: Modifier = Modifier,
    onSelect: (String) -> Unit, onEdit: (Controller.ActiveRule) -> Unit, onRemove: (Controller.ActiveRule) -> Unit,
    onRetry: (Controller.ActiveRule) -> Unit, onCopy: (Controller.ActiveRule) -> Unit, retryEnabled: Boolean,
    onLongSelect: (Controller.ActiveRule) -> Unit = {}, state: LazyListState = rememberLazyListState(),
    showEmpty: Boolean = true, filtered: Boolean = false, onClearFilter: () -> Unit = {},
    keyOf: (Controller.ActiveRule) -> String = { it.src },
    headline: (@Composable (Controller.ActiveRule) -> Unit)? = null,
    supporting: (@Composable (Controller.ActiveRule) -> Unit)? = null,
    canRetry: (Controller.ActiveRule) -> Boolean = { it.persisted },
    canRemove: (Controller.ActiveRule) -> Boolean = { true },
    extraMenu: @Composable (Controller.ActiveRule, () -> Unit) -> Unit = { _, _ -> },
) {
    LazyColumn(modifier.fillMaxWidth(), state = state, contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp)) {
        if (rules.isEmpty() && showEmpty) item {
            Column(Modifier.padding(vertical = 24.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(if (filtered) R.string.sumh_rules_no_match else R.string.sumh_user_rules_empty), color = MaterialTheme.colorScheme.onSurfaceVariant)
                if (filtered) TextButton(onClick = onClearFilter) { Text(stringResource(R.string.sumh_rules_clear_filter)) }
            }
        }
        items(rules, key = keyOf) { rule ->
            var menu by remember { mutableStateOf(false) }
            val selectionKey = keyOf(rule)
            Row(Modifier.fillMaxWidth().heightIn(min = 72.dp)
                .semantics { if (selecting) toggleableState = if (selectionKey in selection) ToggleableState.On else ToggleableState.Off }
                .combinedClickable(enabled = enabled, role = if (selecting) Role.Checkbox else Role.Button,
                    onClickLabel = stringResource(if (selecting) R.string.sumh_rules_select else if (rule.persisted) R.string.sumh_rules_edit else R.string.sumh_rules_more),
                    onLongClickLabel = stringResource(R.string.sumh_rules_select),
                    onLongClick = { if (selecting) onSelect(selectionKey) else onLongSelect(rule) },
                    onClick = { if (selecting) onSelect(selectionKey) else if (rule.persisted) onEdit(rule) else menu = true }),
                verticalAlignment = Alignment.CenterVertically) {
                if (selecting) Checkbox(selectionKey in selection, null, enabled = enabled, modifier = Modifier.padding(end = 12.dp))
                Column(Modifier.weight(1f).padding(vertical = 12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    if (headline != null) headline(rule) else Text(rule.src, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodyMedium)
                    if (supporting != null) supporting(rule) else Text(if (!rule.persisted) stringResource(R.string.sumh_user_rules_runtime_only)
                        else if (rule.hideState == null) stringResource(R.string.sumh_user_rules_unknown) else userHideStatus(rule),
                        style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                if (!selecting) Box {
                    IconButton(onClick = { menu = true }, enabled = enabled) { YukiIcon(Icons.Filled.MoreVert, stringResource(R.string.sumh_rules_more)) }
                    DropdownMenu(menu, { menu = false }) {
                        if (rule.persisted) DropdownMenuItem(text = { Text(stringResource(R.string.sumh_rules_edit)) }, onClick = { menu = false; onEdit(rule) })
                        DropdownMenuItem(text = { Text(stringResource(R.string.sumh_rules_copy)) }, onClick = { menu = false; onCopy(rule) })
                        DropdownMenuItem(text = { Text(stringResource(R.string.sumh_hide_retry)) }, enabled = retryEnabled && canRetry(rule), onClick = { menu = false; onRetry(rule) })
                        extraMenu(rule) { menu = false }
                        DropdownMenuItem(text = { Text(stringResource(R.string.sumh_user_hide_remove)) }, enabled = canRemove(rule), onClick = { menu = false; onRemove(rule) })
                    }
                }
            }
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        }
    }
}

@Composable
internal fun UserHideRuleEditor(
    original: String?, draft: String, bulk: Boolean, busy: Boolean, enabled: Boolean, existing: Set<String>, error: String?,
    progress: Int = 0, total: Int = 0,
    retryPaths: Set<String> = emptySet(),
    draftStatus: @Composable () -> Unit = {},
    inputEnabled: Boolean = true,
    onDraftChange: (String) -> Unit, onDismiss: () -> Unit, onStop: () -> Unit = {}, onSubmit: (List<String>) -> Unit,
) {
    // A failed add can already be persisted. Keep that exact path actionable until explicitly retried.
    val preview = remember(draft, existing, retryPaths) { previewSUMHHideImport(draft, existing - retryPaths) }
    val pathError = remember(draft) { validateSUMHHidePath(draft) }
    YukiAlertDialog(onDismissRequest = { if (!busy) onDismiss() },
        // Bound the editor by the available window instead of the platform's wrap-content width.
        modifier = Modifier.padding(horizontal = 24.dp).widthIn(max = 560.dp).fillMaxWidth(),
        properties = DialogProperties(usePlatformDefaultWidth = false),
        title = { Text(stringResource(if (original != null) R.string.sumh_rules_edit else if (bulk) R.string.sumh_rules_import else R.string.sumh_user_hide_add)) },
        text = { Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            OutlinedTextField(draft, onDraftChange, Modifier.fillMaxWidth(), enabled = !busy && inputEnabled,
                keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false),
                label = { Text(stringResource(R.string.sumh_user_rules_add_label)) }, singleLine = !bulk,
                minLines = if (bulk) 5 else 1, maxLines = if (bulk) 10 else 1, isError = !bulk && draft.isNotEmpty() && pathError != null)
            draftStatus()
            if (bulk) {
                Text(stringResource(R.string.sumh_user_rules_preview_count, preview.paths.size, preview.lines.count { it.duplicate }, preview.lines.count { it.error != null }))
                if (preview.paths.any { it in retryPaths }) Text(stringResource(R.string.sumh_rules_import_retry_hint), style = MaterialTheme.typography.bodySmall)
                Text(stringResource(R.string.sumh_rules_import_hint), style = MaterialTheme.typography.bodySmall)
                preview.lines.filter { it.error != null }.take(5).forEach {
                    Text(stringResource(R.string.sumh_user_rules_preview_line, it.number, it.path) + "\n" + userHidePathError(requireNotNull(it.error)),
                        color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
                }
            } else if (draft.isNotEmpty() && pathError != null) Text(userHidePathError(pathError), color = MaterialTheme.colorScheme.error)
            if (original != null) Text(stringResource(R.string.sumh_rules_edit_hint), style = MaterialTheme.typography.bodySmall)
            if (original != null && draft != original) TextButton(enabled = !busy, onClick = { onDraftChange(original) }) {
                Text(stringResource(R.string.sumh_rules_reset_draft))
            }
            error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
            if (busy) {
                if (bulk) Text(stringResource(R.string.sumh_rules_progress, progress, total), style = MaterialTheme.typography.bodySmall)
                LinearProgressIndicator(Modifier.fillMaxWidth())
            }
        } }, confirmButton = { TextButton(enabled = enabled && !busy && (if (bulk) preview.canApply else pathError == null),
            onClick = { onSubmit(if (bulk) preview.paths else listOf(draft)) }) {
            Text(stringResource(if (original != null) R.string.sumh_rules_save
                else if (bulk && preview.paths.any { it in retryPaths }) R.string.sumh_rules_continue_import
                else if (bulk) R.string.sumh_rules_add_valid else R.string.sumh_user_hide_add))
        } }, dismissButton = {
            if (busy && bulk) TextButton(onClick = onStop) { Text(stringResource(R.string.sumh_rules_stop)) }
            else TextButton(enabled = !busy, onClick = onDismiss) { Text(stringResource(R.string.sumh_rules_cancel)) }
        })
}

@Composable
private fun userHidePathError(error: SUMHHidePathError): String = stringResource(when (error) {
    SUMHHidePathError.NotAbsolute -> R.string.sumh_user_rules_invalid_absolute
    SUMHHidePathError.NulCharacter -> R.string.sumh_user_rules_invalid_control
    SUMHHidePathError.TooLong -> R.string.sumh_user_rules_invalid_length
})

@Composable
private fun UserHideDraftStatus(drafts: UserHideDrafts, modifier: Modifier = Modifier) {
    Column(modifier.semantics { liveRegion = LiveRegionMode.Polite }) {
        when {
            drafts.storageError -> Row(verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(if (drafts.loaded) R.string.sumh_draft_save_failed else R.string.sumh_draft_read_failed),
                    Modifier.weight(1f), color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
                TextButton(onClick = drafts::retry) { Text(stringResource(R.string.retry)) }
            }
            !drafts.loaded -> Text(stringResource(R.string.sumh_draft_loading), style = MaterialTheme.typography.bodySmall)
            drafts.saving -> Text(stringResource(R.string.sumh_draft_saving), style = MaterialTheme.typography.bodySmall)
        }
        drafts.limit?.let { limit ->
            Text(stringResource(when (limit) {
                UserHideDraftLimit.Line -> R.string.sumh_draft_line_limit
                UserHideDraftLimit.Bulk -> R.string.sumh_draft_bulk_limit
                UserHideDraftLimit.Total -> R.string.sumh_draft_total_limit
            }), color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
        }
    }
}
