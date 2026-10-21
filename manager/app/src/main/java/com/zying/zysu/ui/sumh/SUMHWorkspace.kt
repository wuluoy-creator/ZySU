package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as Controller
import com.zying.zysu.ui.sumh.util.SUMHControlState
import com.zying.zysu.ui.util.rememberSnackbarController
import com.zying.zysu.ui.util.LocalSnackbarHost
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.withContext

enum class SUMHSection(val displayNameRes: Int) {
    Status(R.string.sumh_workspace_status),
    Mount(R.string.sumh_workspace_mount),
    Isolation(R.string.sumh_workspace_isolation),
    Rules(R.string.sumh_workspace_rules),
    Logs(R.string.sumh_workspace_logs),
    Debug(R.string.sumh_workspace_debug),
}

/** The host owns navigation and insets; status and debug may join a host's single scroll. */
@Composable
fun SUMHWorkspace(
    section: SUMHSection,
    scrollable: Boolean = true,
    onSavingChanged: (Boolean) -> Unit = {},
    settingsHeader: @Composable () -> Unit = {},
    contentPadding: PaddingValues = PaddingValues(horizontal = 18.dp, vertical = 16.dp),
    refreshKey: Int = 0,
    externalSaving: Boolean = false,
) {
    val resources = LocalResources.current
    val scope = rememberCoroutineScope()
    val inlineContent = !scrollable && (section == SUMHSection.Debug || section == SUMHSection.Status)
    val localSnackbar = rememberSnackbarController()
    val snackbar = if (inlineContent) LocalSnackbarHost.current else localSnackbar
    val layoutDirection = LocalLayoutDirection.current
    val horizontalPadding = PaddingValues(
        start = contentPadding.calculateStartPadding(layoutDirection),
        end = contentPadding.calculateEndPadding(layoutDirection),
    )
    val sectionState = rememberSaveableStateHolder()
    var isLoading by remember { mutableStateOf(false) }
    var configSaving by remember { mutableStateOf(false) }
    var userRulesSaving by remember { mutableStateOf(false) }
    var userRulesRefresh by remember { mutableIntStateOf(0) }
    val saving = configSaving || userRulesSaving || externalSaving
    var dataReady by remember { mutableStateOf(false) }
    var loadError by remember { mutableStateOf<String?>(null) }
    var runtimeApplyPending by rememberSaveable { mutableStateOf(false) }
    var status by remember { mutableStateOf(Controller.SUMHStatus.NOT_PRESENT) }
    var version by remember { mutableStateOf("Unknown") }
    var config by remember { mutableStateOf(Controller.SUMHPConfig()) }
    var rules by remember { mutableStateOf(emptyList<Controller.ActiveRule>()) }
    var rulesIncomplete by remember { mutableStateOf(false) }
    var system by remember { mutableStateOf(Controller.SystemInfo("", "", emptyList(), emptyList(), false, null)) }
    var features by remember { mutableStateOf<Controller.FeaturesResult?>(null) }
    var controlState by remember { mutableStateOf(SUMHControlState()) }
    var rulesRefreshing by remember { mutableStateOf(false) }
    var showKernelLog by rememberSaveable { mutableStateOf(false) }
    var logClearing by remember { mutableStateOf(false) }
    var logRefresh by remember { mutableIntStateOf(0) }
    var resumeRefresh by remember { mutableIntStateOf(0) }
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    val savingCallback by rememberUpdatedState(onSavingChanged)
    SideEffect { savingCallback(configSaving || userRulesSaving) }
    DisposableEffect(Unit) { onDispose { savingCallback(false) } }
    DisposableEffect(lifecycle) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) resumeRefresh++
        }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer) }
    }

    fun publish(loaded: Controller.UiState) {
        version = loaded.version
        status = loaded.status
        // Keep the complete config, including original JSON and supported fields.
        config = loaded.config
        system = loaded.system
        rules = loaded.rules
        rulesIncomplete = loaded.rulesIncomplete
        features = loaded.features
        controlState = loaded.controlState.retaining(controlState)
        dataReady = true
        loadError = null
    }

    suspend fun reload(): String? = try {
        publish(Controller.load())
        null
    } catch (error: Exception) {
        if (error is CancellationException) throw error
        resources.getString(R.string.sumh_toast_load_error, error.message ?: "unknown").also {
            // Keep the previous snapshot visible, but prevent edits based on stale data.
            loadError = it
            controlState = controlState.stale(it)
        }
    }

    fun loadData() {
        if (isLoading || saving) return
        isLoading = true
        scope.launchUi(snackbar) {
            val message = try {
                reload()
            } finally {
                isLoading = false
            }
            message?.let { snackbar.showSnackbar(it) }
        }
    }

    fun saveConfig(newConfig: Controller.SUMHPConfig) {
        if (saving || isLoading || !dataReady || loadError != null) return
        configSaving = true
        scope.launchUi(snackbar, start = CoroutineStart.UNDISPATCHED) {
            // Saving and applying are separate outcomes; a failed refresh must not hide them.
            val message = withContext(NonCancellable) {
                try {
                    val result = Controller.saveConfig(newConfig)
                    if (result.noChanges) return@withContext null
                    runtimeApplyPending = pendingSUMHApplyAfterSave(runtimeApplyPending, result)
                    val refreshError = reload()
                    val outcome = when {
                        result.error != null && result.persisted -> resources.getString(R.string.sumh_saved_apply_failed, result.error)
                        result.error != null -> result.error
                        result.runtimeRequired && !result.applied -> resources.getString(R.string.sumh_saved_runtime_pending)
                        result.applied && result.rebootRequired -> resources.getString(R.string.sumh_saved_applied_reboot)
                        result.rebootRequired -> resources.getString(R.string.sumh_saved_reboot)
                        result.applied -> resources.getString(R.string.sumh_saved_applied)
                        else -> resources.getString(R.string.sumh_saved_configuration)
                    }
                    listOfNotNull(outcome, refreshError).joinToString("\n")
                } finally {
                    configSaving = false
                }
            }
            if (message != null) snackbar.showSnackbar(message)
        }
    }

    fun retryApply() {
        if (saving || isLoading || !dataReady || loadError != null ||
            status != Controller.SUMHStatus.AVAILABLE) return
        configSaving = true
        scope.launchUi(snackbar, start = CoroutineStart.UNDISPATCHED) {
            val message = withContext(NonCancellable) {
                try {
                    runtimeApplyPending = !Controller.retryApply()
                    val refreshError = reload()
                    val outcome = resources.getString(if (runtimeApplyPending) R.string.sumh_apply_failed
                        else R.string.sumh_saved_applied)
                    listOfNotNull(outcome, refreshError).joinToString("\n")
                } finally {
                    configSaving = false
                }
            }
            snackbar.showSnackbar(message)
        }
    }

    fun refreshRules() {
        if (rulesRefreshing || saving || isLoading || !dataReady || loadError != null) return
        rulesRefreshing = true
        scope.launchUi(snackbar) {
            try {
                val refreshed = Controller.getActiveRules()
                rules = refreshed.rules
                rulesIncomplete = refreshed.incomplete
            } finally {
                rulesRefreshing = false
            }
        }
    }

    fun clearRules() {
        if (saving || rulesRefreshing || isLoading || !dataReady || loadError != null) return
        configSaving = true
        scope.launchUi(snackbar, start = CoroutineStart.UNDISPATCHED) {
            val message = withContext(NonCancellable) {
                try {
                    if (Controller.clearAllRules()) {
                        runtimeApplyPending = true
                        reload()
                        R.string.sumh_workspace_rules_cleared
                    } else {
                        R.string.sumh_workspace_rules_clear_failed
                    }
                } finally {
                    configSaving = false
                }
            }
            snackbar.showSnackbar(resources.getString(message))
        }
    }

    fun mutateMapRules(successMessage: Int, failureMessage: Int, clearing: Boolean = false, mutation: suspend () -> Boolean) {
        if (saving || isLoading || !dataReady || loadError != null) return
        if (!config.enableMapsSpoof || status != Controller.SUMHStatus.AVAILABLE ||
            features?.names?.contains("maps_spoof") != true) return
        configSaving = true
        scope.launchUi(snackbar, start = CoroutineStart.UNDISPATCHED) {
            val message = withContext(NonCancellable) {
                try {
                    if (mutation()) {
                        if (clearing && config.mapsRules.isNotEmpty()) runtimeApplyPending = true
                        successMessage
                    } else failureMessage
                } finally {
                    reload()
                    configSaving = false
                }
            }
            snackbar.showSnackbar(resources.getString(message))
        }
    }

    LaunchedEffect(refreshKey, section, resumeRefresh, userRulesRefresh) { loadData() }
    val logs = rememberSUMHLogRead(section == SUMHSection.Logs, showKernelLog, logRefresh) { kernel ->
        if (kernel) Controller.readKernelLog() else Controller.readLog()
    }

    Box(if (inlineContent) Modifier.fillMaxWidth() else Modifier.fillMaxSize()) {
        Column(if (inlineContent) Modifier.fillMaxWidth() else Modifier.fillMaxSize()) {
            if (loadError != null) {
                Row(
                    modifier = Modifier.fillMaxWidth().padding(horizontalPadding),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Text(
                        text = loadError.orEmpty(),
                        modifier = Modifier.weight(1f),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                    IconButton(onClick = ::loadData, enabled = !isLoading && !saving) {
                        YukiIcon(Icons.Filled.Refresh, stringResource(R.string.sumh_rules_refresh))
                    }
                }
            }
            if (isLoading || saving || rulesRefreshing || (section == SUMHSection.Logs && logs.loading) || logClearing) {
                LinearProgressIndicator(Modifier.fillMaxWidth())
            }
            Box(if (inlineContent) Modifier.fillMaxWidth() else Modifier.weight(1f).fillMaxWidth()) {
                sectionState.SaveableStateProvider(section.name) {
                    if (!dataReady && section != SUMHSection.Logs && section != SUMHSection.Rules) {
                        if (!inlineContent) Column(
                            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(contentPadding),
                            horizontalAlignment = Alignment.CenterHorizontally,
                        ) {
                            CompositionLocalProvider(LocalSnackbarHost provides snackbar) { settingsHeader() }
                            if (isLoading) CircularProgressIndicator(Modifier.padding(16.dp))
                        }
                    } else when (section) {
                        SUMHSection.Status -> StatusTab(
                            status, version, system, ::loadData,
                            scrollable = !inlineContent,
                            contentPadding = contentPadding,
                            refreshEnabled = !isLoading && !saving,
                            loading = isLoading,
                            loadError = loadError,
                            controlState = controlState,
                            config = config,
                        )
                        SUMHSection.Mount, SUMHSection.Isolation, SUMHSection.Debug -> SettingsTab(
                            config = config,
                            sumhStatus = status,
                            features = features,
                            controlsEnabled = dataReady && loadError == null && !saving && !isLoading,
                            runtimeApplyPending = controlState.applyAdvice() == SUMHApplyAdvice.Drift ||
                                controlState.applyAdvice() == SUMHApplyAdvice.Legacy && runtimeApplyPending,
                            onRetryApply = ::retryApply,
                            onConfigChanged = ::saveConfig,
                            controlState = controlState,
                            legacyApplyPending = runtimeApplyPending,
                            onRefreshState = ::loadData,
                            onPartitionsSaving = { configSaving = it },
                            onPartitionsSaved = { names ->
                                config = config.copy(partitions = names,
                                    original = org.json.JSONObject(config.original.toString()).put("partitions", org.json.JSONArray(names)))
                                // Save receipt confirms only persistent configuration. A later
                                // explicit refresh establishes the remaining controller state.
                                controlState = controlState.stale(resources.getString(R.string.sumh_partitions_saved))
                            },
                            onClearMapRules = {
                                mutateMapRules(R.string.sumh_maps_cleared, R.string.sumh_maps_clear_failed, clearing = true) {
                                    Controller.clearMapsRules()
                                }
                            },
                            onAddMapRule = { ti, td, si, sd, path ->
                                mutateMapRules(R.string.sumh_maps_add_success, R.string.sumh_maps_add_failed) {
                                    Controller.addMapsRule(ti, td, si, sd, path)
                                }
                            },
                            section = section,
                            scrollable = !inlineContent,
                            header = { CompositionLocalProvider(LocalSnackbarHost provides snackbar) { settingsHeader() } },
                            contentPadding = contentPadding,
                            headingActions = {
                                IconButton(onClick = ::loadData, enabled = !isLoading && !saving) {
                                    YukiIcon(Icons.Filled.Refresh, stringResource(R.string.sumh_rules_refresh))
                                }
                            },
                        )
                        SUMHSection.Rules -> SUMHRulesWorkspace(
                            rules, status, !configSaving && !externalSaving && !rulesRefreshing,
                            dataReady && loadError == null && !saving && !isLoading && !rulesRefreshing,
                            saving, snackbar,
                            onSavingChanged = { working ->
                                if (userRulesSaving && !working) userRulesRefresh++
                                userRulesSaving = working
                            }, onRefresh = ::refreshRules, onClearAll = ::clearRules,
                            rulesIncomplete = rulesIncomplete,
                        )
                        SUMHSection.Logs -> LogsTab(
                            showKernelLog = showKernelLog,
                            onToggleLogType = { showKernelLog = !showKernelLog },
                            logContent = logs.content,
                            loading = logs.loading,
                            readError = logs.error,
                            onRefreshLog = { logRefresh++ },
                            onClearLog = {
                                if (!logClearing && !showKernelLog) {
                                    logClearing = true
                                    scope.launchUi(snackbar) {
                                        try {
                                            if (Controller.clearLog()) logRefresh++
                                            else snackbar.showSnackbar(resources.getString(R.string.operation_failed))
                                        } finally { logClearing = false }
                                    }
                                }
                            },
                        )
                    }
                }
            }
        }
        if (!inlineContent) SnackbarHost(snackbar.hostState, Modifier.align(Alignment.BottomCenter))
    }
}
