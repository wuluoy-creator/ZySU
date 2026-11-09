package com.zying.zysu.ui.screen

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.ArrowBack
import androidx.compose.material.icons.outlined.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.zying.zysu.BuildConfig
import com.zying.zysu.Natives
import com.zying.zysu.R
import com.zying.zysu.ui.component.KsuIsValid
import com.zying.zysu.ui.component.LiquidGlassSurface
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.component.YukiTopAppBar
import com.zying.zysu.ui.component.YukiTopBarTitle
import com.zying.zysu.ui.component.YukiPanel
import com.zying.zysu.ui.component.YukiIconBadge
import com.zying.zysu.ui.sumh.SUMHSection
import com.zying.zysu.ui.sumh.SUMHWorkspace
import com.zying.zysu.ui.sumh.SUMHTabs
import com.zying.zysu.ui.sumh.ConfigSection
import com.zying.zysu.ui.util.LocalNavigationLeaveGuard
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.*
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import ui.screen.feature.FeatureControlContent
import ui.screen.feature.FeatureControlState
import ui.screen.feature.IsolationKernelSettings
import ui.screen.feature.SuperuserLogSetting
import ui.screen.moreSettings.MoreSettingsContent
import ui.screen.moreSettings.PreferenceCategory
import ui.screen.moreSettings.component.MoreSettingsItemPosition
import ui.screen.moreSettings.component.SettingsCard

/** Tabs share the page header's wallpaper-aware material and grow with wrapped labels. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun WorkspaceTabs(
    labels: List<String>,
    selected: Int,
    onSelected: (Int) -> Unit,
    enabled: Boolean = true,
) {
    val focusManager = LocalFocusManager.current
    LiquidGlassSurface(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp).padding(bottom = 12.dp),
        samplePageBackdrop = false,
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().height(IntrinsicSize.Min).padding(4.dp).selectableGroup(),
            horizontalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            labels.forEachIndexed { index, label ->
                val isSelected = index == selected
                Box(
                    modifier = Modifier.weight(1f).fillMaxHeight().heightIn(min = 48.dp)
                        .clip(RoundedCornerShape(24.dp))
                        .background(if (isSelected) MaterialTheme.colorScheme.primaryContainer else androidx.compose.ui.graphics.Color.Transparent)
                        .selectable(selected = isSelected, enabled = enabled, role = Role.Tab) {
                            focusManager.clearFocus()
                            onSelected(index)
                        }
                        .padding(horizontal = 8.dp, vertical = 12.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        label,
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = if (isSelected) FontWeight.SemiBold else FontWeight.Medium,
                        color = (if (isSelected) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant)
                            .copy(alpha = if (enabled) 1f else 0.5f),
                        textAlign = TextAlign.Center,
                    )
                }
            }
        }
    }
}

/** A single inset owner for embedded pages; content always receives a bounded viewport. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun WorkspaceScaffold(
    title: String,
    navigator: DestinationsNavigator,
    showBack: Boolean = true,
    navigationEnabled: Boolean = true,
    tabs: @Composable () -> Unit = {},
    content: @Composable () -> Unit,
) {
    Scaffold(
        topBar = {
            Column {
                YukiTopAppBar(
                    title = { YukiTopBarTitle(title) },
                    navigationIcon = {
                        if (showBack) IconButton(enabled = navigationEnabled, onClick = { navigator.navigateUp() }) {
                            YukiIcon(Icons.AutoMirrored.Outlined.ArrowBack, stringResource(R.string.back))
                        }
                    },
                )
                tabs()
            }
        },
        contentWindowInsets = WindowInsets.safeDrawing,
    ) { padding ->
        Box(Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding)) { content() }
    }
}

@Composable
internal fun AuthorizationWorkspace(navigator: DestinationsNavigator) {
    SuperUserPage(navigator)
}

@Destination<RootGraph>
@Composable
fun AuthorizationPoliciesScreen(navigator: DestinationsNavigator) {
    WorkspaceScaffold(stringResource(R.string.nav_policies), navigator) {
        AuthorizationSettingsContent(navigator)
    }
}

@Destination<RootGraph>
@Composable
fun AuthorizationRecordsScreen(navigator: DestinationsNavigator) {
    KsuIsValid {
        if (rememberAuthorizationLogsEnabled()) {
            LogViewerPage(navigator)
        } else {
            WorkspaceScaffold(stringResource(R.string.nav_records), navigator) {
                Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp)) {
                    YukiPanel {
                        Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
                            YukiIconBadge(Icons.Outlined.History)
                            Text(stringResource(R.string.nav_records_disabled), style = MaterialTheme.typography.bodyLarge)
                            OutlinedButton(onClick = { navigator.navigate(DiagnosticsScreenDestination) }) {
                                Text(stringResource(R.string.settings_category_diagnostics))
                            }
                        }
                    }
                }
            }
        }
    }
}

@Destination<RootGraph>
@Composable
@Suppress("UNUSED_PARAMETER") // Keep the old navigation argument for restored back stacks.
fun KernelPolicyScreen(navigator: DestinationsNavigator, initialPage: Int = 0) {
    var saving by remember { mutableStateOf(false) }
    WorkspaceOperationGuard(saving, KernelPolicyScreenDestination.route)
    WorkspaceScaffold(stringResource(R.string.nav_kernel_policy), navigator, navigationEnabled = !saving) {
        KsuIsValid {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                FeatureControlContent(navigator, onSavingChanged = { saving = it }, scrollable = false)
                MoreSettingsContent(PreferenceCategory.Advanced, scrollable = false)
                Box(Modifier.padding(horizontal = 16.dp)) {
                    SettingsCard { WebUIEngineSelector() }
                }
            }
        }
    }
}

@Destination<RootGraph>
@Composable
fun MountIsolationScreen(navigator: DestinationsNavigator) {
    var selected by rememberSaveable { mutableIntStateOf(0) }
    val sections = listOf(SUMHSection.Mount, SUMHSection.Isolation, SUMHSection.Rules,
        SUMHSection.Kstat, SUMHSection.Maps)
    // Restore the former kernel-info tab into its new home under Standard.
    val section = if (selected == 5) SUMHSection.Isolation
        else sections.getOrElse(selected) { SUMHSection.Mount }
    var sumhSaving by remember { mutableStateOf(false) }
    var isolationSaving by remember { mutableStateOf(false) }
    val saving = sumhSaving || isolationSaving
    // Keep the path dialog alive when SUMH finishes loading and moves its header.
    val isolationHeader = remember(navigator) {
        movableContentOf<Boolean> { controlsEnabled ->
            ConfigSection(title = stringResource(R.string.sumh_group_process_isolation), segmented = true) {
                IsolationKernelSettings(
                    enabled = controlsEnabled,
                    onSavingChanged = { isolationSaving = it },
                )
            }
        }
    }
    WorkspaceOperationGuard(saving, MountIsolationScreenDestination.route)
    WorkspaceScaffold(stringResource(R.string.settings_category_mount_isolation), navigator, navigationEnabled = !saving, tabs = {
        SUMHTabs(section, enabled = !saving, sections = sections) { selected = sections.indexOf(it) }
    }) {
        KsuIsValid {
            SUMHWorkspace(
                section = section,
                onSavingChanged = { sumhSaving = it },
                externalSaving = isolationSaving,
                settingsHeader = {
                    if (section == SUMHSection.Isolation) isolationHeader(!sumhSaving)
                },
            )
        }
    }
}

@Destination<RootGraph>
@Composable
fun DiagnosticsScreen(navigator: DestinationsNavigator) {
    var selected by rememberSaveable { mutableIntStateOf(0) }
    var sumhSaving by remember { mutableStateOf(false) }
    var suLogSaving by remember { mutableStateOf(false) }
    val saving = sumhSaving || suLogSaving
    val pages = rememberSaveableStateHolder()
    WorkspaceOperationGuard(saving, DiagnosticsScreenDestination.route)
    val rootAvailable = remember { Natives.isManager && Natives.version != null }
    val currentPage = if (rootAvailable) selected else 0
    val labels = buildList {
        add(stringResource(R.string.nav_diagnostic_options))
        if (rootAvailable) {
            add(stringResource(R.string.nav_kernel_logs))
            if (BuildConfig.DEBUG) add(stringResource(R.string.nav_webui_debug))
        }
    }
    WorkspaceScaffold(stringResource(R.string.nav_diagnostics), navigator, navigationEnabled = !saving, tabs = {
        if (labels.size > 1) WorkspaceTabs(labels, currentPage.coerceAtMost(labels.lastIndex), { selected = it }, enabled = !saving)
    }) {
        pages.SaveableStateProvider(currentPage) {
        when (currentPage) {
            0 -> Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(bottom = 16.dp)) {
                val logsEnabled = if (rootAvailable) rememberAuthorizationLogsEnabled() else false
                DiagnosticExportActions(additionalActions = if (rootAvailable) {
                    {
                        SuperuserLogSetting(
                            enabled = !saving,
                            groupPosition = if (logsEnabled) MoreSettingsItemPosition.Middle else MoreSettingsItemPosition.Last,
                            onSavingChanged = { suLogSaving = it },
                        )
                        if (logsEnabled) {
                            SettingItem(
                                icon = Icons.Outlined.History,
                                title = stringResource(R.string.nav_records),
                                summary = stringResource(R.string.nav_records_summary),
                                enabled = !saving,
                                groupPosition = SettingsItemPosition.Last,
                                onClick = { navigator.navigate(AuthorizationRecordsScreenDestination) },
                            )
                        }
                    }
                } else null)
                KsuIsValid {
                    SUMHWorkspace(
                        SUMHSection.Debug,
                        scrollable = false,
                        onSavingChanged = { sumhSaving = it },
                        externalSaving = suLogSaving,
                    )
                }
            }
            1 -> KsuIsValid { SUMHWorkspace(SUMHSection.Logs) }
            else -> MoreSettingsContent(PreferenceCategory.WebUI)
        }
        }
    }
}

/** Refresh availability when returning from diagnostics or another app. */
@Composable
private fun rememberAuthorizationLogsEnabled(): Boolean {
    val lifecycleOwner = LocalLifecycleOwner.current
    var nativeEnabled by remember { mutableStateOf(Natives.isSuLogEnabled()) }
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) {
                FeatureControlState.refreshSuLog()
                nativeEnabled = Natives.isSuLogEnabled()
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }
    return FeatureControlState.suLogEnabled ?: nativeEnabled
}

/** Prevent navigation from disposing an operation while its progress is visible. */
@Composable
internal fun WorkspaceOperationGuard(busy: Boolean, route: String) {
    val guard = LocalNavigationLeaveGuard.current
    val owner = remember { Any() }
    DisposableEffect(busy, route, guard) {
        if (busy) guard.register(owner, route) { _, onIntercepted -> onIntercepted() }
        onDispose { guard.unregister(owner) }
    }
    BackHandler(busy) {}
}
