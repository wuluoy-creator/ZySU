package com.zying.zysu.ui.screen

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.NavigateNext
import androidx.compose.material.icons.automirrored.filled.Undo
import androidx.compose.material.icons.filled.*
import androidx.compose.material.icons.rounded.FolderDelete
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import androidx.core.content.edit
import com.maxkeppeker.sheets.core.models.base.IconSource
import com.maxkeppeler.sheets.list.models.ListOption
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.AppProfileTemplateScreenDestination
import com.ramcosta.composedestinations.generated.destinations.FlashScreenDestination
import com.ramcosta.composedestinations.generated.destinations.KernelPolicyScreenDestination
import com.ramcosta.composedestinations.generated.destinations.MountIsolationScreenDestination
import com.ramcosta.composedestinations.generated.destinations.DiagnosticsScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.zying.zysu.BuildConfig
import com.zying.zysu.Natives
import com.zying.zysu.R
import com.zying.zysu.ui.component.*
import com.zying.zysu.ui.util.*
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.time.LocalDateTime
import java.time.format.DateTimeFormatter
import ui.screen.moreSettings.MoreSettingsContent
import ui.screen.moreSettings.PreferenceCategory
import ui.screen.moreSettings.component.SettingsCard
import ui.screen.moreSettings.component.SettingsIconBadge

/**
 * @author ShirkNeko
 * @date 2025/9/29.
 */
private val SPACING_MEDIUM = 8.dp
private val SPACING_LARGE = 16.dp

enum class SettingsItemPosition(val index: Int, val count: Int) {
    First(0, 3),
    Middle(1, 3),
    Last(2, 3),
    Only(0, 1)
}

private enum class SettingsWorkspaceCategory(val titleRes: Int) {
    Appearance(R.string.settings_category_appearance),
    Display(R.string.settings_category_display),
    App(R.string.settings_category_app),
}

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun SettingScreen(navigator: DestinationsNavigator) {
    var category by rememberSaveable { mutableStateOf<SettingsWorkspaceCategory?>(null) }
    val savedState = rememberSaveableStateHolder()
    BackHandler(enabled = category != null) { category = null }
    YukiPageScaffold(
        topBar = {
            YukiTopAppBar(
                title = {
                    YukiTopBarTitle(
                        text = stringResource(category?.titleRes ?: R.string.settings),
                    )
                },
                navigationIcon = {
                    if (category != null) {
                        IconButton(onClick = { category = null }) {
                            YukiIcon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.back))
                        }
                    }
                },
            )
        },
        contentWindowInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal),
    ) { padding ->
        Box(Modifier.fillMaxSize()) {
            savedState.SaveableStateProvider(category?.name ?: "index") {
                when (category) {
                    SettingsWorkspaceCategory.Appearance -> MoreSettingsContent(PreferenceCategory.Appearance, contentPadding = padding)
                    SettingsWorkspaceCategory.Display -> MoreSettingsContent(PreferenceCategory.Display, contentPadding = padding)
                    SettingsWorkspaceCategory.App -> UpdateUninstallSettingsContent(navigator, padding)
                    null -> SettingsWorkspaceIndex(navigator, padding) { category = it }
                }
            }
        }
    }
}

@Composable
private fun SettingsWorkspaceIndex(
    navigator: DestinationsNavigator,
    contentPadding: PaddingValues,
    onCategory: (SettingsWorkspaceCategory) -> Unit,
) {
    val aboutDialog = rememberCustomDialog { AboutDialog(it) }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(contentPadding).padding(top = 4.dp, bottom = 16.dp + LocalBottomBarPadding.current)) {
        SettingsGroupCard(stringResource(R.string.settings_workspace_personalization)) {
            SettingItem(
                icon = Icons.Filled.Palette,
                title = stringResource(R.string.settings_category_appearance),
                summary = stringResource(R.string.settings_category_appearance_summary),
                groupPosition = SettingsItemPosition.First,
                onClick = { onCategory(SettingsWorkspaceCategory.Appearance) },
            )
            SettingItem(
                icon = Icons.Filled.ViewList,
                title = stringResource(R.string.settings_category_display),
                summary = stringResource(R.string.settings_category_display_summary),
                groupPosition = SettingsItemPosition.Last,
                onClick = { onCategory(SettingsWorkspaceCategory.Display) },
            )
        }
        SettingsGroupCard(stringResource(R.string.settings_workspace_system)) {
            SettingItem(
                icon = Icons.Filled.SystemUpdate,
                title = stringResource(R.string.settings_category_app),
                summary = stringResource(R.string.settings_category_app_summary),
                groupPosition = SettingsItemPosition.First,
                onClick = { onCategory(SettingsWorkspaceCategory.App) },
            )
            KsuIsValid {
                SettingItem(
                    icon = Icons.Filled.Folder,
                    title = stringResource(R.string.settings_category_mount_isolation),
                    summary = stringResource(R.string.settings_category_mount_isolation_summary),
                    onClick = { navigator.navigate(MountIsolationScreenDestination()) },
                )
                SettingItem(
                    icon = Icons.Filled.Security,
                    title = stringResource(R.string.settings_category_kernel),
                    summary = stringResource(R.string.settings_category_kernel_summary),
                    onClick = { navigator.navigate(KernelPolicyScreenDestination()) },
                )
            }
            SettingItem(
                icon = Icons.Filled.BugReport,
                title = stringResource(R.string.settings_category_diagnostics),
                summary = stringResource(R.string.settings_category_diagnostics_summary),
                onClick = { navigator.navigate(DiagnosticsScreenDestination) },
            )
            SettingItem(
                icon = Icons.Filled.Info,
                title = stringResource(R.string.about),
                groupPosition = SettingsItemPosition.Last,
                onClick = { aboutDialog.show() },
            )
        }
    }
}

/** Embedded by the authorization workspace; owns its existing policies and dialogs. */
@Composable
fun AuthorizationSettingsContent(navigator: DestinationsNavigator) {
    val scope = rememberCoroutineScope()
    val resources = LocalResources.current
    val snackBarHost = LocalSnackbarHost.current
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(bottom = 16.dp)) {
        KsuIsValid {
            SettingsGroupCard(stringResource(R.string.settings_authorization_defaults)) {
                SettingItem(
                    icon = Icons.Filled.Fence,
                    title = stringResource(R.string.settings_profile_template),
                    summary = stringResource(R.string.settings_profile_template_summary),
                    groupPosition = SettingsItemPosition.First,
                    onClick = { navigator.navigate(AppProfileTemplateScreenDestination) },
                )
                val superKeyDialog = rememberCustomDialog { dismiss ->
                    SuperKeySettingsDialog(
                        onDismiss = dismiss,
                        onKeyCleared = {
                            scope.launch {
                                snackBarHost.showSnackbar(resources.getString(R.string.clear_super_key) + " ✓")
                            }
                        },
                    )
                }
                SettingItem(
                    icon = Icons.Filled.Key,
                    title = stringResource(R.string.settings_superkey_management),
                    summary = stringResource(R.string.settings_superkey_management_summary),
                    onClick = { superKeyDialog.show() },
                )
                var umountChecked by rememberSaveable { mutableStateOf(Natives.isDefaultUmountModules()) }
                SwitchItem(
                    icon = Icons.Rounded.FolderDelete,
                    title = stringResource(R.string.settings_umount_modules_default),
                    summary = stringResource(R.string.settings_umount_modules_default_summary),
                    checked = umountChecked,
                    groupPosition = SettingsItemPosition.Last,
                    onCheckedChange = {
                        if (Natives.setDefaultUmountModules(it)) umountChecked = it
                    },
                )
            }
        }
    }
}

@Composable
private fun UpdateUninstallSettingsContent(navigator: DestinationsNavigator, contentPadding: PaddingValues) {
    val context = LocalContext.current
    val prefs = remember { context.getSharedPreferences("settings", Context.MODE_PRIVATE) }
    val loadingDialog = rememberLoadingDialog()
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(contentPadding).padding(bottom = 16.dp + LocalBottomBarPadding.current)) {
        SettingsGroupCard(stringResource(R.string.settings_category_app)) {
            var checkUpdate by rememberSaveable { mutableStateOf(prefs.getBoolean("check_update", true)) }
            SwitchItem(
                icon = Icons.Filled.Update,
                title = stringResource(R.string.settings_check_update),
                summary = stringResource(R.string.settings_check_update_summary),
                checked = checkUpdate,
                groupPosition = SettingsItemPosition.First,
                onCheckedChange = { enabled ->
                    prefs.edit { putBoolean("check_update", enabled) }
                    checkUpdate = enabled
                },
            )
            var autoUpdateKsud by rememberSaveable { mutableStateOf(prefs.getBoolean("auto_update_ksud", false)) }
            SwitchItem(
                icon = Icons.Filled.Sync,
                title = stringResource(R.string.settings_auto_update_ksud),
                summary = stringResource(R.string.settings_auto_update_ksud_summary),
                checked = autoUpdateKsud,
                groupPosition = SettingsItemPosition.Last,
                onCheckedChange = { enabled ->
                    prefs.edit { putBoolean("auto_update_ksud", enabled) }
                    autoUpdateKsud = enabled
                },
            )
        }
        KsuIsValid {
            SettingsGroupCard(stringResource(R.string.settings_uninstall)) {
                UninstallItem(navigator, groupPosition = SettingsItemPosition.Only) {
                    loadingDialog.withLoading(it)
                }
            }
        }
    }
}

/** Non-scrolling report-export group for the shared diagnostics page. */
@Composable
fun DiagnosticExportActions(additionalActions: (@Composable () -> Unit)? = null) {
    val context = LocalContext.current
    val resources = LocalResources.current
    val scope = rememberCoroutineScope()
    val snackBarHost = LocalSnackbarHost.current
    val loadingDialog = rememberLoadingDialog()
    var showBottomsheet by remember { mutableStateOf(false) }
    val exportBugreportLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/gzip")
    ) { uri: Uri? ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch(start = CoroutineStart.UNDISPATCHED) {
            val saved = loadingDialog.withLoading {
                runCatching {
                    withContext(NonCancellable + Dispatchers.IO) {
                        val output = checkNotNull(
                            context.contentResolver.openOutputStream(uri)
                        )
                        output.use { outputStream ->
                            getBugreportFile(context).inputStream().use { input ->
                                input.copyTo(outputStream)
                            }
                        }
                    }
                }.onFailure {
                    if (it is CancellationException) throw it
                }.isSuccess
            }
            val message = if (saved) R.string.log_saved else R.string.operation_failed
            snackBarHost.showSnackbar(resources.getString(message))
        }
    }
    SettingsGroupCard(stringResource(R.string.nav_diagnostic_options)) {
        SettingItem(
            icon = Icons.Filled.BugReport,
            title = stringResource(R.string.send_log),
            groupPosition = if (additionalActions == null) SettingsItemPosition.Only else SettingsItemPosition.First,
            onClick = { showBottomsheet = true },
        )
        additionalActions?.invoke()
    }
    if (showBottomsheet) {
        LogBottomSheet(
            onDismiss = { showBottomsheet = false },
            onSaveLog = {
                val formatter = DateTimeFormatter.ofPattern("yyyy-MM-dd_HH_mm")
                val current = LocalDateTime.now().format(formatter)
                exportBugreportLauncher.launch("ZySU_bugreport_${current}.tar.gz")
                showBottomsheet = false
            },
            onShareLog = {
                scope.launch {
                    val bugreport = loadingDialog.withLoading {
                        runCatching {
                            withContext(Dispatchers.IO) {
                                getBugreportFile(context)
                            }
                        }.onFailure {
                            if (it is CancellationException) throw it
                        }
                    }.getOrElse {
                        snackBarHost.showSnackbar(
                            resources.getString(R.string.operation_failed)
                        )
                        return@launch
                    }

                    val shared = runCatching {
                        val uri = FileProvider.getUriForFile(
                            context,
                            "${BuildConfig.APPLICATION_ID}.fileprovider",
                            bugreport
                        )

                        val shareIntent = Intent(Intent.ACTION_SEND).apply {
                            putExtra(Intent.EXTRA_STREAM, uri)
                            setDataAndType(uri, "application/gzip")
                            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                        }

                        context.startActivity(
                            Intent.createChooser(
                                shareIntent,
                                resources.getString(R.string.send_log)
                            )
                        )
                    }.isSuccess
                    if (shared) {
                        showBottomsheet = false
                    } else {
                        snackBarHost.showSnackbar(
                            resources.getString(R.string.operation_failed)
                        )
                    }
                }
            }
        )
    }
}

@Composable
private fun SettingsGroupCard(
    title: String,
    content: @Composable ColumnScope.() -> Unit
) {
    Box(Modifier.fillMaxWidth().padding(horizontal = 18.dp)) {
        SettingsCard(title = title) {
            Column(content = content)
        }
    }
}

@Composable
internal fun WebUIEngineSelector() {
    val context = LocalContext.current
    val prefs = remember { context.getSharedPreferences("settings", Context.MODE_PRIVATE) }
    var selectedEngine by rememberSaveable { mutableStateOf(prefs.getString("webui_engine", "default") ?: "default") }
    var showDialog by remember { mutableStateOf(false) }
    val engineOptions = listOf(
        "default" to stringResource(R.string.engine_auto_select),
        "wx" to stringResource(R.string.engine_force_webuix),
        "ksu" to stringResource(R.string.engine_force_ksu)
    )

    SettingItem(
        icon = Icons.Filled.WebAsset,
        title = stringResource(R.string.use_webuix),
        summary = engineOptions.find { it.first == selectedEngine }?.second
            ?: stringResource(R.string.engine_auto_select),
        groupPosition = SettingsItemPosition.Only,
        onClick = { showDialog = true }
    )

    if (showDialog) {
        YukiAlertDialog(
            onDismissRequest = { showDialog = false },
            title = { Text(stringResource(R.string.use_webuix)) },
            text = {
                Column {
                    engineOptions.forEach { (value, label) ->
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable {
                                    selectedEngine = value
                                    prefs.edit { putString("webui_engine", value) }
                                    showDialog = false
                                }
                                .padding(vertical = 12.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            RadioButton(
                                selected = selectedEngine == value,
                                onClick = null
                            )
                            Spacer(modifier = Modifier.width(SPACING_MEDIUM))
                            Text(text = label)
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showDialog = false }) {
                    Text(stringResource(R.string.cancel))
                }
            }
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LogBottomSheet(
    onDismiss: () -> Unit,
    onSaveLog: () -> Unit,
    onShareLog: () -> Unit
) {
    ModalBottomSheet(
        onDismissRequest = onDismiss,
        modifier = Modifier.clickHapticFeedback(),
        containerColor = MaterialTheme.colorScheme.surfaceContainerHigh,
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(SPACING_LARGE),
            horizontalArrangement = Arrangement.SpaceEvenly
        ) {
            LogActionButton(
                icon = Icons.Filled.Save,
                text = stringResource(R.string.save_log),
                onClick = onSaveLog
            )

            LogActionButton(
                icon = Icons.Filled.Share,
                text = stringResource(R.string.send_log),
                onClick = onShareLog
            )
        }
        Spacer(modifier = Modifier.height(SPACING_LARGE))
    }
}

@Composable
fun LogActionButton(
    icon: ImageVector,
    text: String,
    onClick: () -> Unit
) {
    Column(
        horizontalAlignment = Alignment.CenterHorizontally,
        modifier = Modifier
            .clickable(onClick = onClick)
            .padding(SPACING_MEDIUM)
    ) {
        Box(
            contentAlignment = Alignment.Center,
            modifier = Modifier
                .size(56.dp)
                .clip(CircleShape)
                .background(MaterialTheme.colorScheme.primaryContainer)
        ) {
            YukiIcon(
                imageVector = icon,
                contentDescription = text,
                tint = MaterialTheme.colorScheme.onPrimaryContainer,
                modifier = Modifier.size(24.dp)
            )
        }
        Spacer(modifier = Modifier.height(SPACING_MEDIUM))
        Text(
            text = text,
            style = MaterialTheme.typography.bodyMedium
        )
    }
}

@Composable
fun SettingItem(
    icon: ImageVector,
    title: String,
    summary: String? = null,
    groupPosition: SettingsItemPosition = SettingsItemPosition.Middle,
    enabled: Boolean = true,
    onClick: () -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 76.dp)
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick)
            .alpha(if (enabled) 1f else 0.5f)
            .padding(horizontal = SPACING_LARGE, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        SettingsLeadingIcon(icon = icon)
        Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(text = title, style = MaterialTheme.typography.bodyLarge, fontWeight = FontWeight.Medium)
            if (summary != null) {
                Text(
                    text = summary,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        YukiIcon(
            imageVector = Icons.AutoMirrored.Filled.NavigateNext,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(20.dp),
        )
    }
    SettingsRowDivider(groupPosition)
}

@Composable
fun SwitchItem(
    icon: ImageVector,
    title: String,
    summary: String? = null,
    checked: Boolean,
    enabled: Boolean = true,
    groupPosition: SettingsItemPosition = SettingsItemPosition.Middle,
    onCheckedChange: (Boolean) -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 76.dp)
            .toggleable(
                value = checked,
                enabled = enabled,
                role = Role.Switch,
                onValueChange = onCheckedChange,
            )
            .alpha(if (enabled) 1f else 0.5f)
            .padding(horizontal = SPACING_LARGE, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        SettingsIconBadge(icon = icon, selected = checked)
        Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(text = title, style = MaterialTheme.typography.bodyLarge, fontWeight = FontWeight.Medium)
            if (summary != null) {
                Text(
                    text = summary,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        YukiSwitch(checked = checked, enabled = enabled, onCheckedChange = null)
    }
    SettingsRowDivider(groupPosition)
}

@Composable
private fun SettingsRowDivider(groupPosition: SettingsItemPosition) {
    if (groupPosition != SettingsItemPosition.Last && groupPosition != SettingsItemPosition.Only) {
        HorizontalDivider(
            modifier = Modifier.padding(start = 74.dp, end = 16.dp),
            color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.45f),
        )
    }
}

@Composable
private fun SettingsLeadingIcon(icon: ImageVector) {
    SettingsIconBadge(icon = icon)
}

@Composable
fun UninstallItem(
    navigator: DestinationsNavigator,
    groupPosition: SettingsItemPosition = SettingsItemPosition.Middle,
    withLoading: suspend (suspend () -> Unit) -> Unit
) {
    val context = LocalContext.current
    val resources = LocalResources.current
    val scope = rememberCoroutineScope()
    val uninstallConfirmDialog = rememberConfirmDialog()
    val uninstallDialog = rememberUninstallDialog { uninstallType ->
        scope.launch {
            val result = uninstallConfirmDialog.awaitConfirm(
                title = resources.getString(uninstallType.title),
                content = resources.getString(uninstallType.message)
            )
            if (result == ConfirmResult.Confirmed) {
                withLoading {
                    when (uninstallType) {
                        UninstallType.PERMANENT -> navigator.navigate(
                            FlashScreenDestination(FlashIt.FlashUninstall)
                        )
                        UninstallType.RESTORE_STOCK_IMAGE -> navigator.navigate(
                            FlashScreenDestination(FlashIt.FlashRestore)
                        )
                        UninstallType.NONE -> Unit
                    }
                }
            }
        }
    }

    SettingItem(
        icon = Icons.Filled.Delete,
        title = stringResource(id = R.string.settings_uninstall),
        groupPosition = groupPosition,
        onClick = {
            uninstallDialog.show()
        }
    )
}

enum class UninstallType(val title: Int, val message: Int, val icon: ImageVector) {
    PERMANENT(
        R.string.settings_uninstall_permanent,
        R.string.settings_uninstall_permanent_message,
        Icons.Filled.DeleteForever
    ),
    RESTORE_STOCK_IMAGE(
        R.string.settings_restore_stock_image,
        R.string.settings_restore_stock_image_message,
        Icons.AutoMirrored.Filled.Undo
    ),
    NONE(0, 0, Icons.Filled.Delete)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun rememberUninstallDialog(onSelected: (UninstallType) -> Unit): DialogHandle {
    return rememberCustomDialog { dismiss ->
        val options = listOf(
            UninstallType.PERMANENT,
            UninstallType.RESTORE_STOCK_IMAGE
        )
        val listOptions = options.map {
            ListOption(
                titleText = stringResource(it.title),
                subtitleText = if (it.message != 0) stringResource(it.message) else null,
                icon = IconSource(it.icon)
            )
        }

        var selectedOption by remember { mutableStateOf<UninstallType?>(null) }

        MaterialTheme(
            colorScheme = MaterialTheme.colorScheme.copy(
                surface = MaterialTheme.colorScheme.surfaceContainerHigh
            )
        ) {
            YukiAlertDialog(
                onDismissRequest = {
                    dismiss()
                },
                title = {
                    Text(
                        text = stringResource(R.string.settings_uninstall),
                    )
                },
                text = {
                    Column(
                        modifier = Modifier.padding(vertical = 8.dp),
                        verticalArrangement = Arrangement.spacedBy(16.dp)
                    ) {
                        options.forEachIndexed { index, option ->
                            val isSelected = selectedOption == option
                            val backgroundColor = if (isSelected)
                                MaterialTheme.colorScheme.primaryContainer
                            else
                                Color.Transparent
                            val contentColor = if (isSelected)
                                MaterialTheme.colorScheme.onPrimaryContainer
                            else
                                MaterialTheme.colorScheme.onSurface

                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .clip(MaterialTheme.shapes.medium)
                                    .background(backgroundColor)
                                    .clickable {
                                        selectedOption = option
                                    }
                                    .padding(vertical = 12.dp, horizontal = 8.dp),
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                YukiIcon(
                                    imageVector = option.icon,
                                    contentDescription = null,
                                    tint = MaterialTheme.colorScheme.primary,
                                    modifier = Modifier
                                        .padding(end = 16.dp)
                                        .size(24.dp)
                                )
                                Column(
                                    modifier = Modifier.weight(1f)
                                ) {
                                    Text(
                                        text = listOptions[index].titleText,
                                        style = MaterialTheme.typography.titleMedium,
                                    )
                                    listOptions[index].subtitleText?.let {
                                        Text(
                                            text = it,
                                            style = MaterialTheme.typography.bodyMedium,
                                            color = if (isSelected)
                                                contentColor.copy(alpha = 0.8f)
                                            else
                                                MaterialTheme.colorScheme.onSurfaceVariant
                                        )
                                    }
                                }
                                if (isSelected) {
                                    YukiIcon(
                                        imageVector = Icons.Default.RadioButtonChecked,
                                        contentDescription = null,
                                        tint = MaterialTheme.colorScheme.primary,
                                        modifier = Modifier.size(24.dp)
                                    )
                                } else {
                                    YukiIcon(
                                        imageVector = Icons.Default.RadioButtonUnchecked,
                                        contentDescription = null,
                                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                                        modifier = Modifier.size(24.dp)
                                    )
                                }
                            }
                        }
                    }
                },
                confirmButton = {
                    Button(
                        onClick = {
                            selectedOption?.let { onSelected(it) }
                            dismiss()
                        },
                        enabled = selectedOption != null,
                    ) {
                        Text(
                            text = stringResource(android.R.string.ok)
                        )
                    }
                },
                dismissButton = {
                    TextButton(
                        onClick = {
                            dismiss()
                        }
                    ) {
                        Text(
                            text = stringResource(android.R.string.cancel),
                        )
                    }
                },
                shape = MaterialTheme.shapes.extraLarge,
                tonalElevation = 4.dp
            )
        }
    }
}
