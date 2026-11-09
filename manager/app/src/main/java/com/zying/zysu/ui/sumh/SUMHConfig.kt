package com.zying.zysu.ui.sumh

import android.content.Context
import android.content.res.Configuration
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.listSaver
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.zying.zysu.ui.util.SnackbarController
import com.zying.zysu.ui.util.rememberSnackbarController
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.component.YukiPanel
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.component.YukiTopAppBar
import com.zying.zysu.ui.component.YukiTopBarTitle
import com.zying.zysu.ui.screen.WorkspaceOperationGuard
import com.zying.zysu.ui.screen.WorkspaceTabs
import com.zying.zysu.ui.theme.isExpressiveUi
import com.zying.zysu.ui.theme.UtilityPreviewTheme
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as SUMHManager
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter.SUMHStatus
import com.zying.zysu.ui.theme.getCardColors
import com.zying.zysu.ui.theme.getCardBorder
import com.zying.zysu.ui.theme.getCardElevation
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.SUMHConfigScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.launch
import ui.screen.moreSettings.component.MoreSettingsItemPosition
import ui.screen.moreSettings.component.SwitchSettingItem


@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun SUMHConfigScreen(navigator: DestinationsNavigator) {
    var selectedSection by rememberSaveable { mutableStateOf(SUMHSection.Status) }
    var diagnosticSection by rememberSaveable { mutableStateOf(SUMHSection.Runtime) }
    val mainSection = if (selectedSection == SUMHSection.Kernel) SUMHSection.Isolation
        else selectedSection.takeIf { it in SUMH_MAIN_SECTIONS } ?: SUMHSection.Diagnostics
    val diagnosticSections = listOf(SUMHSection.Runtime, SUMHSection.Logs, SUMHSection.Debug)
    var workspaceSaving by remember { mutableStateOf(false) }
    var backupBusy by remember { mutableStateOf(false) }
    var refreshKey by remember { mutableIntStateOf(0) }
    val saving = workspaceSaving || backupBusy
    WorkspaceOperationGuard(saving, SUMHConfigScreenDestination.route)
    Scaffold(
        topBar = {
            YukiTopAppBar(
                title = { YukiTopBarTitle(stringResource(R.string.sumh_title)) },
                navigationIcon = {
                    IconButton(onClick = { if (!saving) navigator.popBackStack() }, enabled = !saving) {
                        YukiIcon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.back))
                    }
                },
                actions = {
                    SUMHConfigBackupActions(
                        enabled = !saving,
                        onBusyChanged = { backupBusy = it },
                        onRestored = { refreshKey++ },
                    )
                },
            )
        }
    ) { padding ->
        Column(Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding)) {
            SUMHTabs(mainSection, enabled = !saving) { if (!saving) selectedSection = it }
            if (mainSection == SUMHSection.Diagnostics) {
                WorkspaceTabs(
                    labels = diagnosticSections.map { stringResource(it.displayNameRes) },
                    selected = diagnosticSections.indexOf(diagnosticSection).coerceAtLeast(0),
                    onSelected = { if (!saving) diagnosticSection = diagnosticSections[it] },
                    enabled = !saving,
                )
            }
            Box(Modifier.weight(1f)) {
                SUMHWorkspace(
                    if (mainSection == SUMHSection.Diagnostics) diagnosticSection else mainSection,
                    onSavingChanged = { workspaceSaving = it },
                    refreshKey = refreshKey,
                    externalSaving = backupBusy,
                )
            }
        }
    }
}

@Composable
internal fun StatusTab(
    sumhStatus: SUMHStatus,
    version: String,
    systemInfo: SUMHManager.SystemInfo,
    onRefresh: () -> Unit,
    scrollable: Boolean = true,
    contentPadding: PaddingValues = PaddingValues(horizontal = 18.dp, vertical = 16.dp),
    refreshEnabled: Boolean = true,
    loading: Boolean = false,
    loadError: String? = null,
) {
    Column(
        modifier = (if (scrollable) Modifier.fillMaxSize().verticalScroll(rememberScrollState()) else Modifier.fillMaxWidth())
            .padding(contentPadding),
    ) {
        SUMHStatusSummaryCard(
            modifier = Modifier.fillMaxWidth().height(IntrinsicSize.Min),
            status = sumhStatus,
            version = version,
            systemInfo = systemInfo,
            onRefresh = onRefresh,
            refreshEnabled = refreshEnabled,
            loading = loading,
            loadError = loadError,
            includeRuntimeDetails = true,
            includeKernel = scrollable,
        )
    }
}

@Composable
internal fun SettingSwitch(
    title: String,
    subtitle: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    enabled: Boolean = true,
    icon: ImageVector = Icons.Filled.Settings,
    position: MoreSettingsItemPosition = MoreSettingsItemPosition.Middle,
) {
    SwitchSettingItem(
        icon = icon,
        title = title,
        summary = subtitle,
        checked = checked,
        enabled = enabled,
        groupPosition = position,
        onChange = onCheckedChange,
    )
}

@Composable
internal fun SettingTextField(
    title: String,
    subtitle: String,
    value: String,
    onValueChange: (String) -> Unit,
    onConfirm: () -> Unit,
    enabled: Boolean = true,
    placeholder: String = "",
    position: MoreSettingsItemPosition = MoreSettingsItemPosition.Middle,
) {
    var isEditing by remember { mutableStateOf(false) }
    SUMHControlGroup(position) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = if (enabled) 1f else 0.5f),
                )
                Text(
                    text = subtitle,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = if (enabled) 1f else 0.5f),
                )
            }
            IconButton(onClick = { isEditing = !isEditing }, enabled = enabled) {
                YukiIcon(
                    imageVector = if (isEditing) Icons.Filled.Close else Icons.Filled.Edit,
                    contentDescription = if (isEditing) stringResource(R.string.close) else title,
                )
            }
        }
        if (isEditing) {
            OutlinedTextField(
                value = value,
                onValueChange = onValueChange,
                modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
                label = { Text(title) },
                placeholder = { if (placeholder.isNotEmpty()) Text(placeholder) },
                shape = MaterialTheme.shapes.small,
                singleLine = true,
                enabled = enabled,
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = {
                    onConfirm()
                    isEditing = false
                }),
                trailingIcon = {
                    IconButton(onClick = { onConfirm(); isEditing = false }, enabled = enabled) {
                        YukiIcon(Icons.Filled.Done, contentDescription = stringResource(R.string.sumh_rule_save))
                    }
                },
            )
        } else if (value.isNotEmpty()) {
            Text(
                text = value,
                modifier = Modifier.padding(top = 8.dp),
                style = MaterialTheme.typography.bodyMedium,
                fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

enum class LogLevel(val displayNameRes: Int, val color: Color, val tag: String) {
    VERBOSE(R.string.sumh_logs_filter_verbose, Color(0xFF9C27B0), "VERBOSE"),
    DEBUG(R.string.sumh_logs_filter_debug, Color(0xFF4CAF50), "DEBUG"),
    INFO(R.string.sumh_logs_filter_info, Color(0xFF2196F3), "INFO"),
    WARN(R.string.sumh_logs_filter_warn, Color(0xFFFF9800), "WARN"),
    ERROR(R.string.sumh_logs_filter_error, Color(0xFFF44336), "ERROR")
}

private val taggedLogLevel = Regex("\\[(VERBOSE|DEBUG|INFO|WARN(?:ING)?|ERROR)]", RegexOption.IGNORE_CASE)

private fun logLevelOf(line: String): LogLevel? {
    val tag = taggedLogLevel.find(line)?.groupValues?.get(1)?.uppercase()
    if (tag != null) return LogLevel.entries.firstOrNull { it.tag == if (tag == "WARNING") "WARN" else tag }
    return LogLevel.entries.asReversed().firstOrNull { line.contains(it.tag, ignoreCase = true) }
}

@Composable
internal fun LogsTab(
    showKernelLog: Boolean,
    onToggleLogType: () -> Unit,
    logContent: String,
    onRefreshLog: () -> Unit,
    onClearLog: () -> Unit,
) {
    val context = LocalContext.current
    val resources = LocalResources.current
    val snackbarHostState = rememberSnackbarController()
    val coroutineScope = rememberCoroutineScope()

    var selectedLogLevels by rememberSaveable(
        stateSaver = listSaver(
            save = { levels: Set<LogLevel> -> levels.map { it.name } },
            restore = { names -> names.map(LogLevel::valueOf).toSet() },
        )
    ) { mutableStateOf(emptySet<LogLevel>()) }
    var filterExpanded by remember { mutableStateOf(false) }
    var sourceExpanded by remember { mutableStateOf(false) }
    var searchExpanded by rememberSaveable { mutableStateOf(false) }
    var search by rememberSaveable { mutableStateOf("") }
    var clearConfirm by remember { mutableStateOf(false) }
    val searchFocus = remember { FocusRequester() }
    val focusManager = LocalFocusManager.current
    val keyboard = LocalSoftwareKeyboardController.current

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

    val filteredLogContent = remember(logContent, selectedLogLevels, search) {
        logContent.lineSequence().filter { line ->
            (search.isEmpty() || line.contains(search, ignoreCase = true)) &&
                (selectedLogLevels.isEmpty() || logLevelOf(line) in selectedLogLevels)
        }.joinToString("\n")
    }

    if (clearConfirm) YukiAlertDialog(
        onDismissRequest = { clearConfirm = false },
        title = { Text(stringResource(R.string.sumh_logs_clear)) },
        text = { Text(stringResource(R.string.sumh_logs_clear_confirm)) },
        confirmButton = { TextButton(onClick = { clearConfirm = false; onClearLog() }) { Text(stringResource(android.R.string.ok)) } },
        dismissButton = { TextButton(onClick = { clearConfirm = false }) { Text(stringResource(android.R.string.cancel)) } },
    )

    val annotatedLogContent = remember(filteredLogContent) {
        buildAnnotatedString {
            filteredLogContent.lines().forEach { line ->
                val color = logLevelOf(line)?.color ?: Color.White
                withStyle(style = SpanStyle(color = color)) {
                    append(line)
                }
                append("\n")
            }
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp)
    ) {

        SUMHLogActions {
            if (searchExpanded) {
                OutlinedTextField(
                    value = search,
                    onValueChange = { search = it },
                    modifier = Modifier.weight(1f).focusRequester(searchFocus),
                    singleLine = true,
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
                Box(Modifier.weight(1f)) {
                    FilledTonalButton(onClick = { sourceExpanded = true }, modifier = Modifier.fillMaxWidth()) {
                        Text(
                            stringResource(if (showKernelLog) R.string.sumh_logs_kernel else R.string.sumh_logs_daemon),
                            modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis,
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
            IconButton(onClick = onRefreshLog) {
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
                        text = { Text(stringResource(R.string.sumh_logs_copy)) },
                        leadingIcon = { YukiIcon(Icons.Filled.ContentCopy, null) },
                        onClick = {
                            filterExpanded = false
                            val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as android.content.ClipboardManager
                            val clip = android.content.ClipData.newPlainText("SUMH Log", filteredLogContent)
                            clipboard.setPrimaryClip(clip)
                            coroutineScope.launchUi(snackbarHostState) {
                                snackbarHostState.showSnackbar(resources.getString(R.string.sumh_logs_copy_success))
                            }
                        },
                    )
                    DropdownMenuItem(
                        text = { Text(stringResource(R.string.sumh_logs_clear)) },
                        leadingIcon = { YukiIcon(Icons.Filled.Delete, null) },
                        enabled = !showKernelLog,
                        onClick = { filterExpanded = false; clearConfirm = true },
                    )
                    HorizontalDivider()
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
                            LogLevel.entries.forEach { level ->
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
                                                .background(level.color, shape = RoundedCornerShape(4.dp))
                                        )
                                    }
                                )
                            }
                        }
                    }
                }
            }
        }

        Card(
            modifier = Modifier
                .fillMaxWidth()
                .weight(1f),
            shape = sumhCardShape(),
            border = getCardBorder(),
            colors = CardDefaults.cardColors(
                containerColor = Color(0xFF1E1E1E)
            )
        ) {
            Box(modifier = Modifier.fillMaxSize()) {
                val scrollState = rememberScrollState()

                Text(
                    text = annotatedLogContent,
                    modifier = Modifier
                        .fillMaxSize()
                        .verticalScroll(scrollState)
                        .padding(12.dp),
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                    fontSize = if (isExpressiveUi) 12.sp else 11.sp
                )

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

internal fun CoroutineScope.launchUi(
    snackbar: SnackbarController,
    start: CoroutineStart = CoroutineStart.DEFAULT,
    action: suspend () -> Unit,
) = launch(start = start) {
    try { action() }
    catch (error: Exception) {
        if (error is CancellationException) throw error
        snackbar.showSnackbar(error.message ?: "Operation failed")
    }
}

@Preview(name = "SUMH status - light", widthDp = 360, heightDp = 800, locale = "zh-rCN")
@Preview(name = "SUMH status - dark", widthDp = 360, heightDp = 800, locale = "zh-rCN", uiMode = Configuration.UI_MODE_NIGHT_YES)
@Preview(name = "SUMH status - large text", widthDp = 320, heightDp = 800, locale = "zh-rCN", fontScale = 2f)
@Preview(name = "SUMH status - landscape", widthDp = 800, heightDp = 360)
@Composable
private fun SUMHStatusPreview() {
    UtilityPreviewTheme {
        Surface(color = MaterialTheme.colorScheme.background) {
            StatusTab(
                sumhStatus = SUMHStatus.AVAILABLE,
                version = "1",
                systemInfo = SUMHManager.SystemInfo(
                    kernel = "6.12.52-android16-5-g2cf44e703c2c-ab14552068-4k",
                    mountBase = "none",
                    activeMounts = emptyList(),
                    sumhModuleIds = emptyList(),
                    sumhMismatch = false,
                    mismatchMessage = null,
                    hooks = "vfs_getattr\nshow_vfsmnt",
                    viewsEnabled = true,
                ),
                onRefresh = {},
            )
        }
    }
}
