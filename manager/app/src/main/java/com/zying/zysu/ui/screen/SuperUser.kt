package com.zying.zysu.ui.screen

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material.icons.outlined.History
import androidx.compose.material.icons.outlined.Tune
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import coil.compose.AsyncImage
import coil.request.ImageRequest
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.AppProfileScreenDestination
import com.ramcosta.composedestinations.generated.destinations.AuthorizationPoliciesScreenDestination
import com.ramcosta.composedestinations.generated.destinations.AuthorizationRecordsScreenDestination
import com.ramcosta.composedestinations.generated.destinations.SuperUserScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.zying.zysu.Natives
import com.zying.zysu.R
import com.zying.zysu.ui.component.LocalBottomBarPadding
import com.zying.zysu.ui.component.SearchAppBar
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.component.YukiPageScaffold
import com.zying.zysu.ui.component.YukiPullToRefreshBox
import com.zying.zysu.ui.component.YukiTopAppBar
import com.zying.zysu.ui.component.YukiTopBarTitle
import com.zying.zysu.ui.theme.CardConfig
import com.zying.zysu.ui.theme.getCardBorder
import com.zying.zysu.ui.util.module.ModuleModify
import com.zying.zysu.ui.util.rememberSnackbarController
import com.zying.zysu.ui.viewmodel.SortType
import com.zying.zysu.ui.viewmodel.SuperUserViewModel
import com.zying.zysu.ui.viewmodel.sortAuthorizationApps
import kotlinx.coroutines.launch

private data class AuthorizationChange(
    val groups: List<SuperUserViewModel.AppGroup>,
    val allowSu: Boolean,
    val umountModules: Boolean? = null,
    val batch: Boolean = false,
)

@Destination<RootGraph>
@Composable
fun SuperUserScreen(navigator: DestinationsNavigator) {
    AuthorizationWorkspace(navigator)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun SuperUserPage(navigator: DestinationsNavigator) {
    val viewModel = viewModel<SuperUserViewModel>()
    val scope = rememberCoroutineScope()
    val context = LocalContext.current
    val resources = LocalResources.current
    val snackbar = rememberSnackbarController()
    val listState = rememberLazyListState()
    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior()
    var showOptions by rememberSaveable { mutableStateOf(false) }
    var managedUid by rememberSaveable { mutableStateOf<Int?>(null) }
    var pendingChange by remember { mutableStateOf<AuthorizationChange?>(null) }
    val busy = viewModel.isUpdatingPermissions
    val visibleGroups = viewModel.appGroupList
    val displayedGroups = remember(visibleGroups, viewModel.currentSortType) {
        sortAuthorizationApps(visibleGroups, viewModel.currentSortType)
    }
    val backupLauncher = ModuleModify.rememberAllowlistBackupLauncher(context, snackbar, viewModel)
    val restoreLauncher = ModuleModify.rememberAllowlistRestoreLauncher(context, snackbar, viewModel)
    val selectedGroups = SuperUserViewModel.appGroups.filter { it.uid in viewModel.selectedUids }

    LaunchedEffect(Unit) { if (!viewModel.hasLoaded) viewModel.fetchAppList() }
    WorkspaceOperationGuard(busy, SuperUserScreenDestination.route)
    BackHandler(viewModel.showBatchActions || busy) { if (!busy) viewModel.exitBatchMode() }

    fun applyChange(change: AuthorizationChange) {
        managedUid = null
        scope.launch {
            if (change.batch) {
                val result = viewModel.updateBatchPermissions(change.allowSu, change.umountModules, change.groups)
                snackbar.showSnackbar(resources.getString(
                    if (result.failureCount == 0) R.string.authorization_batch_success else R.string.authorization_batch_partial,
                    result.successCount, result.failureCount,
                ))
            } else {
                val group = change.groups.single()
                val success = viewModel.updateGroupPermission(group, change.allowSu, change.umountModules)
                snackbar.showSnackbar(resources.getString(
                    if (success) R.string.authorization_updated else R.string.authorization_update_failed,
                    group.mainApp.label,
                ))
            }
        }
    }

    fun requestChange(change: AuthorizationChange) {
        if (busy || change.groups.isEmpty()) return
        managedUid = null
        if (change.allowSu || change.batch || change.groups.any { it.apps.size > 1 } ||
            (change.umountModules != null && change.groups.any { it.allowSu })) pendingChange = change
        else applyChange(change)
    }

    YukiPageScaffold(
        topBar = {
            Column {
                if (viewModel.showBatchActions) {
                    YukiTopAppBar(
                        title = { YukiTopBarTitle(stringResource(R.string.authorization_selected_count, selectedGroups.size)) },
                        navigationIcon = {
                            IconButton(onClick = viewModel::exitBatchMode, enabled = !busy) {
                                YukiIcon(Icons.Filled.Close, stringResource(R.string.cancel))
                            }
                        },
                        actions = {
                            TextButton(onClick = { viewModel.selectGroups(displayedGroups) }, enabled = !busy && displayedGroups.isNotEmpty()) {
                                Text(stringResource(R.string.authorization_select_visible))
                            }
                        },
                    )
                    AuthorizationBatchBar(
                        enabled = !busy && selectedGroups.isNotEmpty(),
                        onChange = { allow, unmount -> requestChange(AuthorizationChange(selectedGroups, allow, unmount, batch = true)) },
                        onClear = viewModel::clearSelection,
                    )
                } else {
                    SearchAppBar(
                        title = { YukiTopBarTitle(stringResource(R.string.nav_authorization)) },
                        searchText = viewModel.search,
                        onSearchTextChange = { viewModel.search = it },
                        onClearClick = { viewModel.search = "" },
                        placeholder = { Text(stringResource(R.string.authorization_search_hint)) },
                        navigationActions = {
                            AuthorizationShortcut(Icons.Outlined.Tune, stringResource(R.string.nav_policies), enabled = !busy) {
                                navigator.navigate(AuthorizationPoliciesScreenDestination)
                            }
                            AuthorizationShortcut(Icons.Outlined.History, stringResource(R.string.nav_records), enabled = !busy) {
                                navigator.navigate(AuthorizationRecordsScreenDestination)
                            }
                        },
                        dropdownContent = {
                            IconButton(onClick = { showOptions = true }, enabled = !busy) {
                                YukiIcon(Icons.Filled.MoreVert, stringResource(R.string.authorization_options))
                            }
                        },
                        scrollBehavior = scrollBehavior,
                    )
                }
                if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
            }
        },
        snackbarHost = { SnackbarHost(snackbar.hostState, Modifier.padding(bottom = LocalBottomBarPadding.current)) },
        contentWindowInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal),
    ) { innerPadding ->
        val layoutDirection = LocalLayoutDirection.current
        YukiPullToRefreshBox(
            modifier = Modifier.fillMaxSize().imePadding(),
            indicatorTopPadding = innerPadding.calculateTopPadding(),
            isRefreshing = viewModel.isRefreshing,
            onRefresh = { if (!busy) scope.launch { viewModel.fetchAppList(forceRefresh = true) } },
        ) {
            LazyColumn(
                state = listState,
                modifier = Modifier.fillMaxSize().nestedScroll(scrollBehavior.nestedScrollConnection),
                contentPadding = PaddingValues(
                    start = innerPadding.calculateStartPadding(layoutDirection) + 16.dp,
                    end = innerPadding.calculateEndPadding(layoutDirection) + 16.dp,
                    top = innerPadding.calculateTopPadding() + 12.dp,
                    bottom = innerPadding.calculateBottomPadding() + LocalBottomBarPadding.current + 24.dp,
                ),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                if (viewModel.loadError != null) {
                    item(key = "load-error") {
                        AuthorizationLoadError(viewModel.isRefreshing || busy) { scope.launch { viewModel.fetchAppList(forceRefresh = true) } }
                    }
                }
                items(displayedGroups, key = { it.uid }) { group ->
                    AuthorizationAppRow(
                        group = group,
                        selecting = viewModel.showBatchActions,
                        selected = group.uid in viewModel.selectedUids,
                        enabled = !busy,
                        onClick = { if (viewModel.showBatchActions) viewModel.toggleGroupSelection(group) else managedUid = group.uid },
                        onLongClick = {
                            if (!viewModel.showBatchActions) viewModel.toggleBatchMode()
                            viewModel.toggleGroupSelection(group)
                        },
                        onToggleRoot = { allow -> requestChange(AuthorizationChange(listOf(group), allow)) },
                    )
                }
                if (displayedGroups.isEmpty() && viewModel.loadError == null) {
                    item(key = "empty") {
                        AuthorizationEmptyState(
                            loading = viewModel.isRefreshing || !viewModel.hasLoaded,
                            filtered = viewModel.search.isNotBlank(),
                            onReset = { viewModel.search = "" },
                            onRefresh = { scope.launch { viewModel.fetchAppList(forceRefresh = true) } },
                        )
                    }
                }
            }
        }
    }
    if (showOptions) {
        AuthorizationOptionsSheet(
            viewModel = viewModel,
            onDismiss = { showOptions = false },
            onRefresh = { showOptions = false; scope.launch { viewModel.fetchAppList(forceRefresh = true) } },
            onBackup = { showOptions = false; backupLauncher.launch(ModuleModify.createAllowlistBackupIntent()) },
            onRestore = { showOptions = false; restoreLauncher.launch(ModuleModify.createAllowlistRestoreIntent()) },
        )
    }
    SuperUserViewModel.appGroups.firstOrNull { it.uid == managedUid }?.let { group ->
        AuthorizationAppSheet(
            group = group, busy = busy,
            onDismiss = { managedUid = null },
            onEditProfile = { app -> managedUid = null; navigator.navigate(AppProfileScreenDestination(app)) },
            onPermissionChange = { allow, unmount -> requestChange(AuthorizationChange(listOf(group), allow, unmount)) },
        )
    }
    pendingChange?.let { change ->
        AuthorizationConfirmDialog(change, onDismiss = { pendingChange = null }, onConfirm = { pendingChange = null; applyChange(change) })
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AuthorizationShortcut(icon: ImageVector, label: String, enabled: Boolean = true, onClick: () -> Unit) {
    TooltipBox(
        positionProvider = TooltipDefaults.rememberTooltipPositionProvider(),
        tooltip = { PlainTooltip { Text(label) } }, state = rememberTooltipState(),
    ) { IconButton(onClick = onClick, enabled = enabled) { YukiIcon(icon, label) } }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun AuthorizationAppRow(
    group: SuperUserViewModel.AppGroup,
    selecting: Boolean,
    selected: Boolean,
    enabled: Boolean,
    onClick: () -> Unit,
    onLongClick: () -> Unit,
    onToggleRoot: (Boolean) -> Unit,
) {
    val selectLabel = stringResource(R.string.authorization_select_app, group.mainApp.label)
    val rootLabel = stringResource(R.string.authorization_root_for_app, group.mainApp.label)
    val modulesUnmounted = remember(group.uid, group.profile) {
        !group.allowSu && Natives.uidShouldUmount(group.uid)
    }
    Surface(
        modifier = Modifier.fillMaxWidth(), shape = RoundedCornerShape(24.dp), border = getCardBorder(),
        color = (if (selected) MaterialTheme.colorScheme.secondaryContainer else MaterialTheme.colorScheme.surfaceContainerLow).copy(alpha = CardConfig.cardAlpha),
    ) {
        ListItem(
            modifier = Modifier.clip(RoundedCornerShape(24.dp)).combinedClickable(
                enabled = enabled, onClick = onClick,
                onLongClickLabel = stringResource(R.string.authorization_select), onLongClick = onLongClick,
            ),
            colors = ListItemDefaults.colors(containerColor = Color.Transparent),
            leadingContent = {
                AsyncImage(
                    model = ImageRequest.Builder(LocalContext.current).data(group.mainApp.packageInfo).build(),
                    contentDescription = null, modifier = Modifier.size(44.dp),
                )
            },
            content = { Text(group.mainApp.label, maxLines = 2, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.titleMedium) },
            supportingContent = {
                Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(group.mainApp.packageName, style = MaterialTheme.typography.bodySmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text(
                        if (group.apps.size > 1) stringResource(R.string.authorization_shared_uid_count, group.uid, group.apps.size) else "UID ${group.uid}",
                        style = MaterialTheme.typography.labelSmall,
                    )
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        AuthorizationBadge(stringResource(if (group.allowSu) R.string.authorization_granted else R.string.authorization_not_granted), group.allowSu)
                        if (modulesUnmounted) AuthorizationBadge(stringResource(R.string.authorization_unmount_enable))
                        if (group.hasCustomProfile) AuthorizationBadge(stringResource(R.string.category_custom_apps))
                    }
                }
            },
            trailingContent = {
                if (selecting) {
                    Checkbox(checked = selected, onCheckedChange = { onClick() }, enabled = enabled, modifier = Modifier.semantics { contentDescription = selectLabel })
                } else {
                    Switch(checked = group.allowSu, onCheckedChange = onToggleRoot, enabled = enabled, modifier = Modifier.semantics { contentDescription = rootLabel })
                }
            },
        )
    }
}

@Composable
private fun AuthorizationBadge(label: String, emphasized: Boolean = false) {
    Surface(
        shape = RoundedCornerShape(6.dp),
        color = if (emphasized) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceContainerHighest,
        contentColor = if (emphasized) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
    ) { Text(label, Modifier.padding(horizontal = 6.dp, vertical = 3.dp), style = MaterialTheme.typography.labelSmall) }
}

@Composable
private fun AuthorizationBatchBar(enabled: Boolean, onChange: (Boolean, Boolean?) -> Unit, onClear: () -> Unit) {
    var showMore by remember { mutableStateOf(false) }
    Row(
        Modifier.fillMaxWidth().windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)).padding(horizontal = 16.dp, vertical = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically,
    ) {
        FilledTonalButton(onClick = { onChange(true, null) }, enabled = enabled, modifier = Modifier.weight(1f)) { Text(stringResource(R.string.authorization_grant)) }
        OutlinedButton(onClick = { onChange(false, null) }, enabled = enabled, modifier = Modifier.weight(1f)) { Text(stringResource(R.string.authorization_revoke)) }
        Box {
            IconButton(onClick = { showMore = true }, enabled = enabled) { YukiIcon(Icons.Filled.MoreVert, stringResource(R.string.authorization_batch_more)) }
            DropdownMenu(expanded = showMore, onDismissRequest = { showMore = false }) {
                DropdownMenuItem(text = { Text(stringResource(R.string.authorization_unmount_enable)) }, onClick = { showMore = false; onChange(false, true) })
                DropdownMenuItem(text = { Text(stringResource(R.string.authorization_unmount_disable)) }, onClick = { showMore = false; onChange(false, false) })
                DropdownMenuItem(text = { Text(stringResource(R.string.authorization_clear_selection)) }, onClick = { showMore = false; onClear() })
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AuthorizationOptionsSheet(viewModel: SuperUserViewModel, onDismiss: () -> Unit, onRefresh: () -> Unit, onBackup: () -> Unit, onRestore: () -> Unit) {
    val systemLabel = stringResource(R.string.show_system_apps)
    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)) {
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(bottom = 24.dp)) {
            Text(stringResource(R.string.authorization_options), Modifier.padding(horizontal = 24.dp, vertical = 12.dp), style = MaterialTheme.typography.titleLarge)
            ListItem(
                content = { Text(systemLabel) },
                supportingContent = { Text(stringResource(R.string.authorization_system_hint)) },
                trailingContent = { Switch(checked = viewModel.showSystemApps, onCheckedChange = viewModel::updateShowSystemApps, modifier = Modifier.semantics { contentDescription = systemLabel }) },
            )
            Text(stringResource(R.string.sort_options), Modifier.padding(horizontal = 24.dp, vertical = 12.dp), style = MaterialTheme.typography.titleSmall)
            SortType.entries.forEach { sort ->
                Row(
                    Modifier.fillMaxWidth().selectable(selected = viewModel.currentSortType == sort, role = Role.RadioButton, onClick = { viewModel.updateCurrentSortType(sort) })
                        .padding(horizontal = 24.dp).heightIn(min = 48.dp),
                    verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp),
                ) {
                    RadioButton(selected = viewModel.currentSortType == sort, onClick = null)
                    Text(stringResource(sort.displayNameRes), style = MaterialTheme.typography.bodyMedium)
                }
            }
            HorizontalDivider(Modifier.padding(vertical = 12.dp, horizontal = 24.dp))
            AuthorizationOption(Icons.Filled.Refresh, stringResource(R.string.refresh), onRefresh)
            AuthorizationOption(Icons.Filled.Save, stringResource(R.string.backup_allowlist), onBackup)
            AuthorizationOption(Icons.Filled.Restore, stringResource(R.string.restore_allowlist), onRestore)
        }
    }
}

@Composable
private fun AuthorizationOption(icon: ImageVector, label: String, onClick: () -> Unit) {
    TextButton(onClick = onClick, modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp), contentPadding = PaddingValues(horizontal = 24.dp, vertical = 12.dp)) {
        YukiIcon(icon, null)
        Spacer(Modifier.width(16.dp))
        Text(label, Modifier.weight(1f), textAlign = TextAlign.Start)
    }
}

@Composable
private fun AuthorizationConfirmDialog(change: AuthorizationChange, onDismiss: () -> Unit, onConfirm: () -> Unit) {
    val title = when {
        change.umountModules == true -> R.string.authorization_unmount_enable
        change.umountModules == false -> R.string.authorization_unmount_disable
        change.allowSu -> R.string.authorization_grant
        else -> R.string.authorization_revoke
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        icon = { YukiIcon(Icons.Filled.Security, null) }, title = { Text(stringResource(title)) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(stringResource(if (change.allowSu) R.string.authorization_grant_warning else R.string.authorization_change_warning))
                if (change.umountModules != null) Text(stringResource(R.string.authorization_unmount_warning))
                if (change.groups.any { it.apps.size > 1 }) Text(stringResource(R.string.authorization_shared_warning))
                Text(stringResource(R.string.authorization_affected_count, change.groups.size), fontWeight = FontWeight.SemiBold)
                change.groups.forEach { group ->
                    Column {
                        Text(group.mainApp.label, style = MaterialTheme.typography.bodyMedium)
                        Text("UID ${group.uid} · ${group.packageNames.joinToString()}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = onConfirm) { Text(stringResource(title)) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) } },
    )
}

@Composable
private fun AuthorizationLoadError(refreshing: Boolean, onRetry: () -> Unit) {
    Surface(color = MaterialTheme.colorScheme.errorContainer, shape = RoundedCornerShape(20.dp)) {
        Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(stringResource(R.string.authorization_load_failed), style = MaterialTheme.typography.bodyMedium)
            TextButton(onClick = onRetry, enabled = !refreshing) { Text(stringResource(R.string.authorization_retry)) }
        }
    }
}

@Composable
private fun AuthorizationEmptyState(loading: Boolean, filtered: Boolean, onReset: () -> Unit, onRefresh: () -> Unit) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 24.dp, vertical = 40.dp), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(16.dp)) {
        if (loading) {
            CircularProgressIndicator()
            Text(stringResource(R.string.authorization_loading), style = MaterialTheme.typography.bodyMedium)
        } else {
            YukiIcon(Icons.Filled.SearchOff, null, modifier = Modifier.size(40.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(stringResource(R.string.no_apps_found), style = MaterialTheme.typography.titleMedium)
            Text(stringResource(if (filtered) R.string.authorization_empty_filtered else R.string.authorization_empty), style = MaterialTheme.typography.bodyMedium, textAlign = TextAlign.Center, color = MaterialTheme.colorScheme.onSurfaceVariant)
            TextButton(onClick = if (filtered) onReset else onRefresh) { Text(stringResource(if (filtered) R.string.authorization_reset_filters else R.string.refresh)) }
        }
    }
}
