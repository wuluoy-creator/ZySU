package com.zying.zysu.ui.sumh

import android.content.Context
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.listSaver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.nestedscroll.NestedScrollConnection
import androidx.compose.ui.input.nestedscroll.NestedScrollSource
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.*
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.*
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.sumh.util.*
import com.zying.zysu.ui.theme.getCardBorder
import com.zying.zysu.ui.util.rememberSnackbarController
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import kotlin.coroutines.coroutineContext

/** Console tokens are shared by both themes; body text contrast is tested. */
internal object SUMHLogPalette {
    val surface = Color(0xFF1E1E1E)
    val plain = Color(0xFFE6E6E6)
    fun text(level: SUMHLogLevel?): Color = when (level) {
        SUMHLogLevel.VERBOSE -> Color(0xFFCE93D8)
        SUMHLogLevel.DEBUG -> Color(0xFFA5D6A7)
        SUMHLogLevel.INFO -> Color(0xFF90CAF9)
        SUMHLogLevel.WARN -> Color(0xFFFFCC80)
        SUMHLogLevel.ERROR -> Color(0xFFEF9A9A)
        null -> plain
    }
}

private val SUMHLogLevel.displayNameRes: Int get() = when (this) {
    SUMHLogLevel.VERBOSE -> R.string.sumh_logs_filter_verbose
    SUMHLogLevel.DEBUG -> R.string.sumh_logs_filter_debug
    SUMHLogLevel.INFO -> R.string.sumh_logs_filter_info
    SUMHLogLevel.WARN -> R.string.sumh_logs_filter_warn
    SUMHLogLevel.ERROR -> R.string.sumh_logs_filter_error
}

private data class LogRequest(
    val kernel: Boolean, val content: String, val levels: Set<SUMHLogLevel>, val search: String,
)
private data class RenderedLog(
    val request: LogRequest, val snapshot: SUMHLogSnapshot, val view: SUMHLogView,
)

@Composable
internal fun LogsTab(
    showKernelLog: Boolean,
    onToggleLogType: () -> Unit,
    logContent: String,
    onRefreshLog: () -> Unit,
    onClearLog: () -> Unit,
    loading: Boolean = false,
    readError: String? = null,
) {
    val context = LocalContext.current
    val resources = LocalResources.current
    val snackbarHostState = rememberSnackbarController()
    val coroutineScope = rememberCoroutineScope()

    var selectedLogLevels by rememberSaveable(
        stateSaver = listSaver(
            save = { levels: Set<SUMHLogLevel> -> levels.map { it.name } },
            restore = { names -> names.map(SUMHLogLevel::valueOf).toSet() },
        )
    ) { mutableStateOf(emptySet<SUMHLogLevel>()) }
    var filterExpanded by remember { mutableStateOf(false) }
    var sourceExpanded by remember { mutableStateOf(false) }
    var searchExpanded by rememberSaveable { mutableStateOf(false) }
    var search by rememberSaveable { mutableStateOf("") }
    var searchLimit by remember { mutableStateOf(false) }
    var clearConfirm by remember { mutableStateOf(false) }
    var followLatest by rememberSaveable { mutableStateOf(false) }
    val searchFocus = remember { FocusRequester() }
    val focusManager = LocalFocusManager.current
    val keyboard = LocalSoftwareKeyboardController.current
    val largeText = LocalDensity.current.fontScale > 1.3f

    fun closeSearch() {
        search = ""
        searchExpanded = false
        focusManager.clearFocus()
        keyboard?.hide()
    }

    BackHandler(enabled = searchExpanded) { closeSearch() }
    LaunchedEffect(searchExpanded) {
        if (searchExpanded) searchFocus.requestFocus()
    }

    // A source change drops the previous source immediately. Search/filter keep
    // the visible snapshot until their cancellable replacement is ready.
    val request = LogRequest(showKernelLog, logContent, selectedLogLevels, search)
    var rendered by remember(showKernelLog) { mutableStateOf<RenderedLog?>(null) }
    LaunchedEffect(request) {
        val previous = rendered
        val next = withContext(Dispatchers.Default) {
            val active = coroutineContext
            val snapshot = if (previous?.request?.content == request.content) previous.snapshot
            else parseSUMHLog(request.content, previous?.snapshot) { active.ensureActive() }
            RenderedLog(request, snapshot,
                filterSUMHLog(snapshot, request.levels, request.search) { active.ensureActive() })
        }
        coroutineContext.ensureActive()
        rendered = next
    }
    val current = rendered?.takeIf { it.request == request }
    val processing = current == null
    val visibleLines = rendered?.view?.lines.orEmpty()

    if (clearConfirm) YukiAlertDialog(
        onDismissRequest = { clearConfirm = false },
        title = { Text(stringResource(R.string.sumh_logs_clear)) },
        text = { Text(stringResource(R.string.sumh_logs_clear_confirm)) },
        confirmButton = { TextButton(onClick = { clearConfirm = false; onClearLog() }) { Text(stringResource(android.R.string.ok)) } },
        dismissButton = { TextButton(onClick = { clearConfirm = false }) { Text(stringResource(android.R.string.cancel)) } },
    )

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp)
    ) {

        FlowRow(
            modifier = Modifier.fillMaxWidth().padding(bottom = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(4.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
            itemVerticalAlignment = Alignment.CenterVertically,
        ) {
            if (searchExpanded) {
                OutlinedTextField(
                    value = search,
                    onValueChange = { searchLimit = !sumhSmallDraftFits(it); if (!searchLimit) search = it },
                    supportingText = if (searchLimit) { { SUMHSmallDraftLimit() } } else null,
                    modifier = (if (largeText) Modifier.fillMaxWidth() else Modifier.weight(1f))
                        .focusRequester(searchFocus),
                    singleLine = true,
                    label = { Text(stringResource(R.string.sumh_logs_search)) },
                    placeholder = {
                        Text(stringResource(R.string.sumh_logs_search), maxLines = 1, overflow = TextOverflow.Ellipsis)
                    },
                    trailingIcon = {
                        IconButton(onClick = { closeSearch() }) {
                            YukiIcon(Icons.Filled.Close, stringResource(R.string.close))
                        }
                    },
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                    keyboardActions = KeyboardActions(onSearch = {
                        focusManager.clearFocus()
                        keyboard?.hide()
                    }),
                )
            } else {
                Box(if (largeText) Modifier.fillMaxWidth() else Modifier.weight(1f)) {
                    FilledTonalButton(onClick = { sourceExpanded = true }, modifier = Modifier.fillMaxWidth()) {
                        Text(
                            stringResource(if (showKernelLog) R.string.sumh_logs_kernel else R.string.sumh_logs_daemon),
                            modifier = Modifier.weight(1f),
                        )
                        YukiIcon(Icons.Filled.ArrowDropDown, null, Modifier.size(18.dp))
                    }
                    DropdownMenu(expanded = sourceExpanded, onDismissRequest = { sourceExpanded = false }) {
                        listOf(false, true).forEach { kernel ->
                            DropdownMenuItem(
                                text = { Text(stringResource(if (kernel) R.string.sumh_logs_kernel else R.string.sumh_logs_daemon)) },
                                trailingIcon = {
                                    if (showKernelLog == kernel) YukiIcon(Icons.Filled.Check, null)
                                },
                                onClick = {
                                    sourceExpanded = false
                                    if (showKernelLog != kernel) onToggleLogType()
                                },
                            )
                        }
                    }
                }
                IconButton(onClick = { searchExpanded = true }) {
                    YukiIcon(Icons.Filled.Search, stringResource(R.string.sumh_logs_search))
                }
            }
            IconButton(onClick = onRefreshLog, enabled = !loading) {
                YukiIcon(Icons.Filled.Refresh, stringResource(R.string.sumh_rules_refresh))
            }

            Box {
                IconButton(onClick = { filterExpanded = true }) {
                    YukiIcon(
                        Icons.Filled.MoreVert,
                        contentDescription = stringResource(R.string.sumh_logs_actions),
                        tint = if (selectedLogLevels.isEmpty()) LocalContentColor.current else MaterialTheme.colorScheme.primary,
                    )
                }
                DropdownMenu(
                    expanded = filterExpanded,
                    onDismissRequest = { filterExpanded = false },
                    modifier = Modifier.widthIn(min = 180.dp)
                ) {
                    DropdownMenuItem(
                        modifier = Modifier.testTag("sumh_logs_copy"),
                        text = { Text(stringResource(R.string.sumh_logs_copy)) },
                        leadingIcon = { YukiIcon(Icons.Filled.ContentCopy, null) },
                        enabled = current != null && !loading,
                        onClick = {
                            val copyText = current?.view?.copyText ?: return@DropdownMenuItem
                            filterExpanded = false
                            val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as android.content.ClipboardManager
                            val clip = android.content.ClipData.newPlainText("SUMH Log", copyText)
                            clipboard.setPrimaryClip(clip)
                            coroutineScope.launchUi(snackbarHostState) {
                                snackbarHostState.showSnackbar(resources.getString(R.string.sumh_logs_copy_success))
                            }
                        },
                    )
                    DropdownMenuItem(
                        text = { Text(stringResource(R.string.sumh_logs_clear)) },
                        leadingIcon = { YukiIcon(Icons.Filled.Delete, null) },
                        enabled = !showKernelLog && !loading,
                        onClick = { filterExpanded = false; clearConfirm = true },
                    )
                    HorizontalDivider()
                    Text(stringResource(R.string.sumh_logs_manual_refresh),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.widthIn(max = 280.dp).padding(12.dp))
                    Column(
                        modifier = Modifier.padding(vertical = 8.dp),
                        verticalArrangement = Arrangement.spacedBy(4.dp)
                    ) {
                        Text(
                            stringResource(R.string.sumh_logs_filter),
                            style = MaterialTheme.typography.labelLarge,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(horizontal = 12.dp),
                        )
                        FilterChip(
                            selected = selectedLogLevels.isEmpty(),
                            onClick = {
                                selectedLogLevels = emptySet()
                                filterExpanded = false
                            },
                            label = { Text(stringResource(R.string.sumh_logs_filter_all)) },
                            modifier = Modifier.padding(horizontal = 12.dp)
                        )
                        HorizontalDivider(modifier = Modifier.padding(vertical = 4.dp))
                        FlowRow(
                            modifier = Modifier.padding(horizontal = 12.dp),
                            horizontalArrangement = Arrangement.spacedBy(6.dp),
                            verticalArrangement = Arrangement.spacedBy(6.dp)
                        ) {
                            SUMHLogLevel.entries.forEach { level ->
                                FilterChip(
                                    selected = level in selectedLogLevels,
                                    onClick = {
                                        selectedLogLevels = if (level in selectedLogLevels) {
                                            selectedLogLevels - level
                                        } else {
                                            selectedLogLevels + level
                                        }
                                    },
                                    label = { Text(stringResource(level.displayNameRes)) },
                                    leadingIcon = {
                                        Box(
                                            modifier = Modifier
                                                .size(8.dp)
                                                .background(MaterialTheme.colorScheme.onSurfaceVariant, shape = RoundedCornerShape(4.dp))
                                        )
                                    }
                                )
                            }
                        }
                    }
                }
            }
        }

        Row(
            Modifier.fillMaxWidth().padding(vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Text(stringResource(R.string.sumh_logs_follow_latest),
                    style = MaterialTheme.typography.labelLarge)
                Text(stringResource(if (followLatest) R.string.sumh_logs_following else R.string.sumh_logs_paused),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            val followLabel = stringResource(R.string.sumh_logs_follow_latest)
            Switch(checked = followLatest, onCheckedChange = { followLatest = it },
                modifier = Modifier.testTag("sumh_logs_follow").semantics { contentDescription = followLabel })
        }
        when {
            loading -> Text(stringResource(R.string.sumh_logs_loading),
                Modifier.testTag("sumh_logs_loading"), style = MaterialTheme.typography.bodySmall)
            readError != null -> Text(stringResource(R.string.sumh_logs_read_failed, readError),
                Modifier.testTag("sumh_logs_read_error").semantics { liveRegion = LiveRegionMode.Polite },
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
        }
        if (processing) Text(stringResource(R.string.sumh_logs_processing),
            Modifier.testTag("sumh_logs_processing"), style = MaterialTheme.typography.bodySmall)
        else if ((!loading && readError == null) || visibleLines.isNotEmpty()) Text(stringResource(R.string.sumh_logs_line_count, visibleLines.size, current.view.totalCount),
            style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(bottom = 4.dp))

        Card(
            modifier = Modifier
                .fillMaxWidth()
                .weight(1f),
            shape = sumhCardShape(),
            border = getCardBorder(),
            colors = CardDefaults.cardColors(
                containerColor = SUMHLogPalette.surface
            )
        ) {
            Box(modifier = Modifier.fillMaxSize()) {
                key(showKernelLog) {
                    val listState = rememberLazyListState()
                    var followingScroll by remember { mutableStateOf(false) }
                    val pauseOnScroll = remember {
                        object : NestedScrollConnection {
                            override fun onPreScroll(available: Offset, source: NestedScrollSource): Offset {
                                if (source == NestedScrollSource.UserInput && available.y != 0f) followLatest = false
                                return Offset.Zero
                            }
                        }
                    }
                    LaunchedEffect(current, followLatest, loading) {
                        if (followLatest && current != null && !loading && current.view.lines.isNotEmpty()) {
                            followingScroll = true
                            try { listState.scrollToItem(current.view.lines.lastIndex) }
                            finally { followingScroll = false }
                        }
                    }
                    LaunchedEffect(listState) {
                        // Covers accessibility/keyboard scrolling as well as drag;
                        // our own instant follow jump must not pause following.
                        snapshotFlow { listState.isScrollInProgress && !followingScroll }.collect { userScroll ->
                            if (userScroll) followLatest = false
                        }
                    }
                    LazyColumn(
                        state = listState,
                        modifier = Modifier.fillMaxSize().testTag("sumh_logs_list").nestedScroll(pauseOnScroll),
                        contentPadding = PaddingValues(12.dp),
                    ) {
                        items(visibleLines, key = { it.key }, contentType = { "log-line" }) { line ->
                            val levelName = line.level?.let { stringResource(it.displayNameRes) }
                                ?: stringResource(R.string.sumh_logs_level_unknown)
                            Text(
                                text = line.text.ifEmpty { " " },
                                color = SUMHLogPalette.text(line.level),
                                modifier = Modifier.fillMaxWidth().testTag("sumh_log_line_${line.key}")
                                    .semantics { stateDescription = levelName },
                                style = MaterialTheme.typography.bodyMedium,
                                fontFamily = FontFamily.Monospace,
                                fontSize = 14.sp,
                                lineHeight = 20.sp,
                            )
                        }
                    }
                    if (!processing && !loading && readError == null && visibleLines.isEmpty()) {
                        Text(stringResource(if (rendered?.snapshot?.lines.isNullOrEmpty())
                            R.string.sumh_logs_empty else R.string.sumh_logs_no_matches),
                            Modifier.align(Alignment.Center).padding(16.dp).testTag("sumh_logs_empty"),
                            color = SUMHLogPalette.plain, style = MaterialTheme.typography.bodyMedium)
                    }
                }

                SnackbarHost(
                    hostState = snackbarHostState.hostState,
                    modifier = Modifier
                        .align(Alignment.BottomCenter)
                        .padding(16.dp)
                )
            }
        }
    }
}
