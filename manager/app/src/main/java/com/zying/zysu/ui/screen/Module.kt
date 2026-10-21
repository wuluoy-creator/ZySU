package com.zying.zysu.ui.screen

import android.annotation.SuppressLint
import android.app.Activity.*
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.net.Uri
import android.util.Log
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.core.tween
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.animateContentSize
import androidx.compose.animation.expandHorizontally
import androidx.compose.animation.shrinkHorizontally
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.outlined.ExpandLess
import androidx.compose.material.icons.outlined.ExpandMore
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.Wysiwyg
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.automirrored.filled.Undo
import androidx.compose.material.icons.outlined.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.core.content.edit
import androidx.core.net.toUri
import androidx.lifecycle.viewmodel.compose.viewModel
import com.zying.zysu.ui.webui.ModuleConfig
import com.zying.zysu.ui.webui.ModuleConfig.Companion.asModuleConfig
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.ExecuteModuleActionScreenDestination
import com.ramcosta.composedestinations.generated.destinations.FlashScreenDestination
import com.ramcosta.composedestinations.generated.destinations.ModuleRepositoryScreenDestination
import com.zying.zysu.ui.sumh.SUMHMountConfigDialog
import com.zying.zysu.ui.sumh.ConfigChoice
import com.zying.zysu.ui.sumh.util.SUMHManager
import kotlinx.coroutines.CancellationException
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.ramcosta.composedestinations.navigation.EmptyDestinationsNavigator
import com.zying.zysu.BuildConfig
import com.zying.zysu.Natives
import com.zying.zysu.R
import com.zying.zysu.ui.component.*
import com.zying.zysu.ui.theme.CardConfig
import com.zying.zysu.ui.theme.UtilityPreviewTheme
import com.zying.zysu.ui.theme.getCardElevation
import com.zying.zysu.ui.theme.getCardBorder
import com.zying.zysu.ui.theme.isExpressiveUi
import com.zying.zysu.ui.util.*
import com.zying.zysu.ui.util.module.ModuleUtils
import com.zying.zysu.ui.util.module.Shortcut
import com.zying.zysu.ui.viewmodel.ModuleViewModel
import com.zying.zysu.ui.webui.WebUIActivity
import com.zying.zysu.ui.webui.WebUIXActivity
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import java.util.concurrent.TimeUnit

private data class ModuleDownloadUiState(
    val moduleName: String,
    val fileName: String,
    val progress: DownloadProgress = DownloadProgress(),
)

private enum class ShortcutType {
    Action,
    WebUI
}

/**
 * @author ShirkNeko
 * @date 2025/9/29.
 */
@SuppressLint("ResourceType", "AutoboxingStateCreation")
@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun ModuleScreen(navigator: DestinationsNavigator) {
    ModulePage(navigator)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun ModulePage(navigator: DestinationsNavigator) {
    val viewModel = viewModel<ModuleViewModel>()
    val context = LocalContext.current
    val resources = LocalResources.current
    val prefs = context.getSharedPreferences("settings", MODE_PRIVATE)
    val snackBarHost = rememberSnackbarController()
    val scope = rememberCoroutineScope()
    val confirmDialog = rememberConfirmDialog()
    var lastClickTime by remember { mutableLongStateOf(0L) }

    LaunchedEffect(Unit) {
        viewModel.initializeCache(context)
    }

    val bottomSheetState = rememberBottomSheetState(
        initialValue = SheetValue.Hidden,
        enabledValues = setOf(SheetValue.Hidden, SheetValue.Expanded)
    )
    var showBottomSheet by remember { mutableStateOf(false) }
    val listState = rememberLazyListState()
    val fabVisible by rememberFabVisibilityState(listState)

    // 快捷方式相关状态
    var shortcutModuleId by rememberSaveable { mutableStateOf<String?>(null) }
    var shortcutName by rememberSaveable { mutableStateOf("") }
    var shortcutIconUri by rememberSaveable { mutableStateOf<String?>(null) }
    var defaultShortcutIconUri by rememberSaveable { mutableStateOf<String?>(null) }
    var defaultActionShortcutIconUri by rememberSaveable { mutableStateOf<String?>(null) }
    var defaultWebUiShortcutIconUri by rememberSaveable { mutableStateOf<String?>(null) }
    var selectedShortcutType by rememberSaveable { mutableStateOf<ShortcutType?>(null) }
    var showShortcutDialog by remember { mutableStateOf(false) }
    var showShortcutTypeDialog by remember { mutableStateOf(false) }
    var mountState by remember { mutableStateOf<SUMHManager.MountState?>(null) }
    var mountDialog by rememberSaveable { mutableStateOf<Pair<String, String>?>(null) }
    var mountRefresh by remember { mutableIntStateOf(0) }
    var mountFilter by rememberSaveable { mutableStateOf("all") }
    var mountConflicts by remember { mutableStateOf<List<SUMHManager.MountConflict>?>(null) }
    var checkingConflicts by remember { mutableStateOf(false) }
    val mountRevision by SUMHManager.revision.collectAsState()
    LaunchedEffect(mountRefresh, mountRevision, viewModel.moduleList) {
        try { mountState = SUMHManager.getMountState() }
        catch (e: Exception) {
            if (e is CancellationException) throw e
            mountState = null
            Log.w("ModuleScreen", "SUMHP mount controls unavailable", e)
        }
    }
    mountDialog?.let { (id, name) ->
        val state = mountState
        if (state != null) SUMHMountConfigDialog(
            moduleId = id,
            moduleName = name,
            initialInfo = state.modules[id],
            sumhAvailable = state.available,
            globalMode = state.globalMode,
            onDismiss = { mountDialog = null },
            onSaved = {
                mountDialog = null
                mountRefresh++
            },
        )
    }
    mountConflicts?.let { conflicts ->
        YukiAlertDialog(
            onDismissRequest = { mountConflicts = null },
            title = { Text(stringResource(R.string.sumh_check_conflicts)) },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    if (conflicts.isEmpty()) Text(stringResource(R.string.sumh_no_conflicts))
                    conflicts.forEach { conflict ->
                        Text(conflict.path, style = MaterialTheme.typography.bodyMedium)
                        Text(conflict.modules.joinToString(", "), style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Spacer(Modifier.height(12.dp))
                    }
                }
            },
            confirmButton = { TextButton(onClick = { mountConflicts = null }) { Text(stringResource(android.R.string.ok)) } },
        )
    }
    val selectZipLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.StartActivityForResult()
    ) {
        if (it.resultCode != RESULT_OK) {
            return@rememberLauncherForActivityResult
        }
        val data = it.data ?: return@rememberLauncherForActivityResult

        scope.launch {
            val clipData = data.clipData
            if (clipData != null) {
                val selectedModules = mutableListOf<Uri>()
                val selectedModuleNames = mutableMapOf<Uri, String>()

                fun processUri(uri: Uri) {
                    try {
                        if (!ModuleUtils.isUriAccessible(context, uri)) {
                            return
                        }
                        ModuleUtils.takePersistableUriPermission(context, uri)
                        val moduleName = ModuleUtils.extractModuleName(context, uri)
                        selectedModules.add(uri)
                        selectedModuleNames[uri] = moduleName
                    } catch (e: Exception) {
                        Log.e("ModuleScreen", "Error while processing URI: $uri, Error: ${e.message}")
                    }
                }

                for (i in 0 until clipData.itemCount) {
                    val uri = clipData.getItemAt(i).uri
                    processUri(uri)
                }

                if (selectedModules.isEmpty()) {
                    snackBarHost.showSnackbar("Unable to access selected module files")
                    return@launch
                }

                val modulesList = selectedModuleNames.values.joinToString("\n• ", "• ")
                val confirmResult = confirmDialog.awaitConfirm(
                    title = resources.getString(R.string.module_install),
                    content = resources.getString(R.string.module_install_multiple_confirm_with_names, selectedModules.size, modulesList),
                    confirm = resources.getString(R.string.install),
                    dismiss = resources.getString(R.string.cancel)
                )

                if (confirmResult == ConfirmResult.Confirmed) {
                    // 直接安装所有模块
                    try {
                        navigator.navigate(FlashScreenDestination(FlashIt.FlashModules(selectedModules)))
                        viewModel.markNeedRefresh()
                    } catch (e: Exception) {
                        Log.e("ModuleScreen", "Error navigating to FlashScreen: ${e.message}")
                        snackBarHost.showSnackbar("Error while installing module: ${e.message}")
                    }
                }
            } else {
                val uri = data.data ?: return@launch
                // 单个安装模块
                try {
                    if (!ModuleUtils.isUriAccessible(context, uri)) {
                        snackBarHost.showSnackbar("Unable to access selected module files")
                        return@launch
                    }

                    ModuleUtils.takePersistableUriPermission(context, uri)

                    val moduleName = ModuleUtils.extractModuleName(context, uri)

                    val confirmResult = confirmDialog.awaitConfirm(
                        title = resources.getString(R.string.module_install),
                        content = resources.getString(R.string.module_install_confirm, moduleName),
                        confirm = resources.getString(R.string.install),
                        dismiss = resources.getString(R.string.cancel)
                    )

                    if (confirmResult == ConfirmResult.Confirmed) {
                        navigator.navigate(FlashScreenDestination(FlashIt.FlashModule(uri)))
                        viewModel.markNeedRefresh()
                    }
                } catch (e: Exception) {
                    Log.e("ModuleScreen", "Error processing a single URI: $uri, Error: ${e.message}")
                    snackBarHost.showSnackbar("Error processing module file: ${e.message}")
                }
            }
        }
    }

    // 快捷方式图片选择器
    val pickShortcutIconLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.GetContent()
    ) { uri ->
        shortcutIconUri = uri?.toString()
    }

    val shortcutPreviewIcon = remember { mutableStateOf<ImageBitmap?>(null) }
    LaunchedEffect(shortcutIconUri) {
        val uriStr = shortcutIconUri
        if (uriStr.isNullOrBlank()) {
            shortcutPreviewIcon.value = null
            return@LaunchedEffect
        }
        val bitmap = withContext(Dispatchers.IO) {
            Shortcut.loadShortcutBitmap(context, uriStr)
        }
        shortcutPreviewIcon.value = bitmap?.asImageBitmap()
    }

    var hasExistingShortcut by rememberSaveable { mutableStateOf(false) }
    LaunchedEffect(shortcutModuleId, selectedShortcutType, showShortcutDialog) {
        val moduleId = shortcutModuleId
        val type = selectedShortcutType
        if (!showShortcutDialog || moduleId.isNullOrBlank() || type == null) {
            hasExistingShortcut = false
            return@LaunchedEffect
        }
        val exists = withContext(Dispatchers.IO) {
            when (type) {
                ShortcutType.Action -> Shortcut.hasModuleActionShortcut(context, moduleId)
                ShortcutType.WebUI -> Shortcut.hasModuleWebUiShortcut(context, moduleId)
            }
        }
        hasExistingShortcut = exists
    }

    // 快捷方式辅助函数
    fun openShortcutDialogForType(type: ShortcutType) {
        selectedShortcutType = type
        val defaultIcon = when (type) {
            ShortcutType.Action -> defaultActionShortcutIconUri ?: defaultWebUiShortcutIconUri
            ShortcutType.WebUI -> defaultWebUiShortcutIconUri ?: defaultActionShortcutIconUri
        }
        defaultShortcutIconUri = defaultIcon
        shortcutIconUri = defaultIcon
        showShortcutDialog = true
    }

    fun onModuleAddShortcut(module: ModuleViewModel.ModuleInfo) {
        shortcutModuleId = module.dirId
        shortcutName = module.name
        shortcutIconUri = null
        defaultShortcutIconUri = null
        defaultActionShortcutIconUri = module.actionIconPath
            ?.takeIf { it.isNotBlank() }
            ?.let { Shortcut.moduleFileUri(module.dirId, it) }
        defaultWebUiShortcutIconUri = module.webUiIconPath
            ?.takeIf { it.isNotBlank() }
            ?.let { Shortcut.moduleFileUri(module.dirId, it) }
        if (module.hasActionScript && module.hasWebUi) {
            selectedShortcutType = null
            showShortcutTypeDialog = true
        } else if (module.hasActionScript) {
            openShortcutDialogForType(ShortcutType.Action)
        } else if (module.hasWebUi) {
            openShortcutDialogForType(ShortcutType.WebUI)
        }
    }

    LaunchedEffect(Unit) {
        if (viewModel.moduleList.isEmpty() || viewModel.isNeedRefresh) {
            viewModel.sortEnabledFirst = prefs.getBoolean("module_sort_enabled_first", false)
            viewModel.sortActionFirst = prefs.getBoolean("module_sort_action_first", false)
            viewModel.fetchModuleList()
        }
    }

    // Both checks were re-running on every recomposition; cache them.
    // hasMagisk() shells out, so push it to IO via produceState.
    val isSafeMode = remember { Natives.isSafeMode }
    val hasMagisk by produceState(initialValue = false) {
        value = withContext(Dispatchers.IO) { hasMagisk() }
    }
    val hideInstallButton = isSafeMode || hasMagisk
    val fabTransition = rememberFabTransition(!hideInstallButton && fabVisible)
    val fabPresent = fabTransition.currentState || fabTransition.targetState || !fabTransition.isIdle

    val topAppBarState = rememberTopAppBarState()
    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior(topAppBarState)

    val webUILauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.StartActivityForResult()
    ) { viewModel.fetchModuleList() }

    YukiPageScaffold(
        topBar = {
            SearchAppBar(
                title = {
                    YukiTopBarTitle(
                        text = stringResource(R.string.module),
                    )
                },
                searchText = viewModel.search,
                onSearchTextChange = { viewModel.search = it },
                onClearClick = { viewModel.search = "" },
                dropdownContent = {
                    IconButton(
                        onClick = { navigator.navigate(ModuleRepositoryScreenDestination) },
                    ) {
                        YukiIcon(
                            imageVector = Icons.Outlined.Inventory2,
                            contentDescription = stringResource(R.string.module_repositories),
                        )
                    }
                    IconButton(
                        onClick = { showBottomSheet = true },
                    ) {
                        YukiIcon(
                            imageVector = Icons.Filled.MoreVert,
                            contentDescription = stringResource(id = R.string.settings),
                        )
                    }
                },
                scrollBehavior = scrollBehavior,
            )
        },
        floatingActionButton = {
            AnimatedFab(visibilityState = fabTransition) {
                ExtendedFloatingActionButton(
                    modifier = Modifier.padding(bottom = LocalBottomBarPadding.current),
                    shape = if (isExpressiveUi) {
                        CircleShape
                    } else {
                        FloatingActionButtonDefaults.shape
                    },
                    contentColor = MaterialTheme.colorScheme.onPrimary,
                    containerColor = MaterialTheme.colorScheme.primary,
                    onClick = {
                        selectZipLauncher.launch(
                            Intent(Intent.ACTION_GET_CONTENT).apply {
                                type = "application/zip"
                                putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
                            }
                        )
                    },
                    icon = { YukiIcon(Icons.Outlined.Add, contentDescription = null) },
                    text = { Text(stringResource(R.string.module_install)) },
                )
            }
        },
        contentWindowInsets = WindowInsets.safeDrawing.only(
            WindowInsetsSides.Top + WindowInsetsSides.Horizontal
        ),
        snackbarHost = {
            SnackbarHost(
                hostState = snackBarHost.hostState,
                modifier = Modifier.padding(
                    bottom = if (fabPresent) 0.dp else LocalBottomBarPadding.current,
                ),
            )
        }
    ) { innerPadding ->
        when {
            hasMagisk -> {
                Box(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(innerPadding)
                        .consumeWindowInsets(innerPadding)
                        .padding(24.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Column(
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.Center
                    ) {
                        YukiIcon(
                            imageVector = Icons.Outlined.Warning,
                            contentDescription = null,
                            modifier = Modifier
                                .size(64.dp)
                                .padding(bottom = 16.dp)
                        )
                        Text(
                            stringResource(R.string.module_magisk_conflict),
                            textAlign = TextAlign.Center,
                            style = MaterialTheme.typography.bodyLarge,
                        )
                    }
                }
            }
            else -> {
                ModuleList(
                    navigator = navigator,
                    viewModel = viewModel,
                    listState = listState,
                    mountState = mountState,
                    mountFilter = mountFilter,
                    onMountConfig = { id, name -> mountDialog = id to name },
                    onRefreshMounts = { mountRefresh++ },
                    modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
                    contentPadding = innerPadding,
                    onInstallModule = {
                        navigator.navigate(FlashScreenDestination(FlashIt.FlashModule(it)))
                    },
                    onUpdateModule = {
                        navigator.navigate(FlashScreenDestination(FlashIt.FlashModuleUpdate(it)))
                    },
                    onClickModule = { id, name, hasWebUi ->
                        val currentTime = System.currentTimeMillis()
                        if (currentTime - lastClickTime < 600) {
                            Log.d("ModuleScreen", "Click too fast, ignoring")
                            return@ModuleList
                        }
                        lastClickTime = currentTime

                        if (hasWebUi) {
                            scope.launch {
                                try {
                                    val wxEngine = Intent(context, WebUIXActivity::class.java)
                                        .setData("kernelsu://webuix/$id".toUri())
                                        .putExtra("id", id)
                                        .putExtra("name", name)

                                    val ksuEngine = Intent(context, WebUIActivity::class.java)
                                        .setData("kernelsu://webui/$id".toUri())
                                        .putExtra("id", id)
                                        .putExtra("name", name)

                                    val config = try {
                                        withContext(Dispatchers.IO) { id.asModuleConfig }
                                    } catch (e: CancellationException) {
                                        throw e
                                    } catch (e: Exception) {
                                        Log.e("ModuleScreen", "Failed to get config from id: $id", e)
                                        null
                                    }

                                    val globalEngine = prefs.getString("webui_engine", "default") ?: "default"
                                    val moduleEngine = config?.getWebuiEngine(context)
                                    val selectedEngine = when (globalEngine) {
                                        "wx" -> wxEngine
                                        "ksu" -> ksuEngine
                                        // Automatic mode requires an explicit WebUI X preference.
                                        "default" -> if (moduleEngine == "wx") wxEngine else ksuEngine
                                        else -> ksuEngine
                                    }
                                    webUILauncher.launch(selectedEngine)
                                } catch (e: CancellationException) {
                                    throw e
                                } catch (e: Exception) {
                                    Log.e("ModuleScreen", "Error launching WebUI: ${e.message}", e)
                                    scope.launch {
                                        snackBarHost.showSnackbar("Error launching WebUI: ${e.message}")
                                    }
                                }
                            }
                            return@ModuleList
                        }
                    },
                    onAddShortcut = { onModuleAddShortcut(it) },
                    context = context,
                    snackBarHost = snackBarHost
                )
            }
        }

        if (showBottomSheet) {
            ModalBottomSheet(
                onDismissRequest = {
                    showBottomSheet = false
                },
                modifier = Modifier.clickHapticFeedback(),
                sheetState = bottomSheetState,
                dragHandle = {
                    Surface(
                        modifier = Modifier.padding(vertical = 11.dp),
                        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f),
                        shape = RoundedCornerShape(16.dp)
                    ) {
                        Box(
                            Modifier.size(
                                width = 32.dp,
                                height = 4.dp
                            )
                        )
                    }
                }
            ) {
                ModuleBottomSheetContent(
                    viewModel = viewModel,
                    prefs = prefs,
                    scope = scope,
                    bottomSheetState = bottomSheetState,
                    mountState = mountState,
                    mountFilter = mountFilter,
                    onMountFilter = { mountFilter = it },
                    checkingConflicts = checkingConflicts,
                    onCheckConflicts = {
                        if (!checkingConflicts) {
                            checkingConflicts = true
                            scope.launch {
                                try {
                                    bottomSheetState.hide()
                                    showBottomSheet = false
                                    mountConflicts = SUMHManager.checkConflicts()
                                } catch (error: Exception) {
                                    if (error is CancellationException) throw error
                                    snackBarHost.showSnackbar(error.message ?: resources.getString(R.string.operation_failed))
                                } finally { checkingConflicts = false }
                            }
                        }
                    },
                    onDismiss = { showBottomSheet = false }
                )
            }
        }

    // 快捷方式类型选择对话框
    if (showShortcutTypeDialog) {
        YukiAlertDialog(
            onDismissRequest = { showShortcutTypeDialog = false },
            title = { Text(stringResource(R.string.module_shortcut_type_title)) },
            text = {
                Column(
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                    modifier = Modifier.padding(vertical = 8.dp)
                ) {
                    FilledTonalButton(
                        onClick = {
                            showShortcutTypeDialog = false
                            openShortcutDialogForType(ShortcutType.Action)
                        },
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text("Action")
                    }
                    FilledTonalButton(
                        onClick = {
                            showShortcutTypeDialog = false
                            openShortcutDialogForType(ShortcutType.WebUI)
                        },
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text("WebUI")
                    }
                }
            },
            confirmButton = {},
            dismissButton = {
                TextButton(onClick = { showShortcutTypeDialog = false }) {
                    Text(stringResource(android.R.string.cancel))
                }
            }
        )
    }

    // 快捷方式创建/编辑对话框
    if (showShortcutDialog) {
        YukiAlertDialog(
            onDismissRequest = { showShortcutDialog = false },
            title = { Text(stringResource(R.string.module_shortcut_title)) },
            text = {
                Column(
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    // 图标预览
                    Box(
                        contentAlignment = Alignment.Center,
                        modifier = Modifier
                            .padding(vertical = 16.dp)
                            .size(100.dp)
                            .clip(RoundedCornerShape(25.dp))
                    ) {
                        val preview = shortcutPreviewIcon.value
                        if (preview != null) {
                            Image(
                                bitmap = preview,
                                modifier = Modifier.size(100.dp),
                                contentDescription = null,
                            )
                        } else {
                            AppLogo(
                                modifier = Modifier.size(100.dp),
                                contentDescription = stringResource(R.string.app_name),
                            )
                        }
                    }

                    // 选择图标按钮和撤销按钮
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        FilledTonalButton(
                            modifier = Modifier.weight(1f),
                            onClick = { pickShortcutIconLauncher.launch("image/*") },
                        ) {
                            Text(stringResource(id = R.string.module_shortcut_icon_pick))
                        }
                        androidx.compose.animation.AnimatedVisibility(
                            visible = shortcutIconUri != defaultShortcutIconUri,
                            enter = expandHorizontally() + slideInHorizontally(initialOffsetX = { it }),
                            exit = shrinkHorizontally() + slideOutHorizontally(targetOffsetX = { it }),
                        ) {
                            IconButton(
                                onClick = { shortcutIconUri = defaultShortcutIconUri }
                            ) {
                                YukiIcon(
                                    imageVector = Icons.AutoMirrored.Filled.Undo,
                                    contentDescription = null,
                                    modifier = Modifier.size(28.dp),
                                )
                            }
                        }
                    }

                    // 名称输入框
                    OutlinedTextField(
                        value = shortcutName,
                        onValueChange = { shortcutName = it },
                        label = { Text(stringResource(id = R.string.module_shortcut_name_label)) },
                        modifier = Modifier.fillMaxWidth(),
                        singleLine = true
                    )

                    if (hasExistingShortcut) {
                        FilledTonalButton(
                            onClick = {
                                val moduleId = shortcutModuleId
                                val type = selectedShortcutType
                                if (!moduleId.isNullOrBlank() && type != null) {
                                    when (type) {
                                        ShortcutType.Action -> Shortcut.deleteModuleActionShortcut(context, moduleId)
                                        ShortcutType.WebUI -> Shortcut.deleteModuleWebUiShortcut(context, moduleId)
                                    }
                                }
                                showShortcutDialog = false
                            },
                            modifier = Modifier.fillMaxWidth(),
                        ) {
                            Text(stringResource(id = R.string.module_shortcut_delete))
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        val moduleId = shortcutModuleId
                        val type = selectedShortcutType
                        if (!moduleId.isNullOrBlank() && shortcutName.isNotBlank() && type != null) {
                            when (type) {
                                ShortcutType.Action -> {
                                    Shortcut.createModuleActionShortcut(
                                        context = context,
                                        moduleId = moduleId,
                                        name = shortcutName,
                                        iconUri = shortcutIconUri
                                    )
                                }
                                ShortcutType.WebUI -> {
                                    Shortcut.createModuleWebUiShortcut(
                                        context = context,
                                        moduleId = moduleId,
                                        name = shortcutName,
                                        iconUri = shortcutIconUri
                                    )
                                }
                            }
                        }
                        showShortcutDialog = false
                    }
                ) {
                    Text(
                        if (hasExistingShortcut) {
                            stringResource(id = R.string.module_update)
                        } else {
                            stringResource(id = android.R.string.ok)
                        }
                    )
                }
            },
            dismissButton = {
                TextButton(onClick = { showShortcutDialog = false }) {
                    Text(stringResource(id = android.R.string.cancel))
                }
            }
        )
    }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ModuleBottomSheetContent(
    viewModel: ModuleViewModel,
    prefs: android.content.SharedPreferences,
    scope: kotlinx.coroutines.CoroutineScope,
    bottomSheetState: SheetState,
    mountState: SUMHManager.MountState?,
    mountFilter: String,
    onMountFilter: (String) -> Unit,
    checkingConflicts: Boolean,
    onCheckConflicts: () -> Unit,
    onDismiss: () -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState())
            .padding(bottom = 24.dp)
    ) {
        Text(
            text = stringResource(R.string.sort_options),
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(horizontal = 24.dp, vertical = 16.dp)
        )

        Column(
            modifier = Modifier.padding(horizontal = 24.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = stringResource(R.string.module_sort_action_first),
                    style = MaterialTheme.typography.bodyMedium
                )
                YukiSwitch(
                    checked = viewModel.sortActionFirst,
                    onCheckedChange = { checked ->
                        viewModel.sortActionFirst = checked
                        prefs.edit {
                            putBoolean("module_sort_action_first", checked)
                        }
                        scope.launch {
                            viewModel.fetchModuleList()
                            bottomSheetState.hide()
                            onDismiss()
                        }
                    }
                )
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = stringResource(R.string.module_sort_enabled_first),
                    style = MaterialTheme.typography.bodyMedium
                )
                YukiSwitch(
                    checked = viewModel.sortEnabledFirst,
                    onCheckedChange = { checked ->
                        viewModel.sortEnabledFirst = checked
                        prefs.edit {
                            putBoolean("module_sort_enabled_first", checked)
                        }
                        scope.launch {
                            viewModel.fetchModuleList()
                            bottomSheetState.hide()
                            onDismiss()
                        }
                    }
                )
            }
            HorizontalDivider(Modifier.padding(vertical = 8.dp))
            ConfigChoice(stringResource(R.string.sumh_mount_filter), mountFilter,
                listOf("all", "auto", "sumh", "overlay", "magic", "none", "sumh-active"),
                mountState != null) { value ->
                onMountFilter(value)
                scope.launch { bottomSheetState.hide(); onDismiss() }
            }
            OutlinedButton(onClick = onCheckConflicts, enabled = mountState != null && !checkingConflicts,
                modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.sumh_check_conflicts)) }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ModuleList(
    navigator: DestinationsNavigator,
    viewModel: ModuleViewModel,
    listState: LazyListState,
    mountState: SUMHManager.MountState?,
    mountFilter: String,
    onMountConfig: (String, String) -> Unit,
    onRefreshMounts: () -> Unit,
    modifier: Modifier = Modifier,
    contentPadding: PaddingValues = PaddingValues(0.dp),
    onInstallModule: (Uri) -> Unit,
    onUpdateModule: (Uri) -> Unit,
    onClickModule: (id: String, name: String, hasWebUi: Boolean) -> Unit,
    onAddShortcut: (ModuleViewModel.ModuleInfo) -> Unit,
    context: Context,
    snackBarHost: SnackbarController
) {
    val layoutDirection = LocalLayoutDirection.current
    val failedEnable = stringResource(R.string.module_failed_to_enable)
    val failedDisable = stringResource(R.string.module_failed_to_disable)
    val failedUninstall = stringResource(R.string.module_uninstall_failed)
    val successUninstall = stringResource(R.string.module_uninstall_success)
    val reboot = stringResource(R.string.reboot)
    val rebootToApply = stringResource(R.string.reboot_to_apply)
    val moduleStr = stringResource(R.string.module)
    val uninstall = stringResource(R.string.uninstall)
    val cancel = stringResource(android.R.string.cancel)
    val moduleUninstallConfirm = stringResource(R.string.module_uninstall_confirm)
    val metaModuleUninstallConfirm = stringResource(R.string.metamodule_uninstall_confirm)
    val updateText = stringResource(R.string.module_update)
    val changelogText = stringResource(R.string.module_changelog)
    val downloadingText = stringResource(R.string.module_downloading)
    val startDownloadingText = stringResource(R.string.module_start_downloading)
    val fetchChangeLogFailed = stringResource(R.string.module_changelog_failed)
    val downloadErrorText = stringResource(R.string.module_download_error)

    val loadingDialog = rememberLoadingDialog()
    val confirmDialog = rememberConfirmDialog()
    var activeDownload by remember { mutableStateOf<ModuleDownloadUiState?>(null) }
    var downloadHandle by remember { mutableStateOf<DownloadHandle?>(null) }

    suspend fun onModuleUpdate(
        module: ModuleViewModel.ModuleInfo,
        changelogUrl: String,
        downloadUrl: String,
        fileName: String
    ) {
        val client = OkHttpClient.Builder()
            .connectTimeout(15, TimeUnit.SECONDS)
            .readTimeout(30, TimeUnit.SECONDS)
            .build()

        val request = okhttp3.Request.Builder()
            .url(changelogUrl)
            .header("User-Agent", "ZySU/${BuildConfig.VERSION_NAME}")
            .build()

        val changelogResult = loadingDialog.withLoading {
            withContext(Dispatchers.IO) {
                runCatching {
                    client.newCall(request).execute().body!!.string()
                }
            }
        }

        val showToast: suspend (String) -> Unit = { msg ->
            withContext(Dispatchers.Main) {
                Toast.makeText(
                    context,
                    msg,
                    Toast.LENGTH_SHORT
                ).show()
            }
        }

        val changelog = changelogResult.getOrElse {
            showToast(fetchChangeLogFailed.format(it.message))
            return
        }.ifBlank {
            showToast(fetchChangeLogFailed.format(module.name))
            return
        }

        val confirmResult = confirmDialog.awaitConfirm(
            changelogText,
            content = changelog,
            markdown = true,
            confirm = updateText,
        )

        if (confirmResult != ConfirmResult.Confirmed) {
            return
        }

        val downloading = downloadingText.format(module.name)
        activeDownload = ModuleDownloadUiState(module.name, fileName)
        downloadHandle = download(
            context,
            downloadUrl,
            fileName,
            downloading,
            onDownloaded = { uri ->
                activeDownload = null
                downloadHandle = null
                onUpdateModule(uri)
            },
            onProgress = { progress ->
                activeDownload = activeDownload?.copy(progress = progress)
            },
            onError = { errorMsg ->
                activeDownload = null
                downloadHandle = null
                Toast.makeText(context, "$downloadErrorText: $errorMsg", Toast.LENGTH_LONG).show()
            },
        )
    }

    suspend fun onModuleUninstallClicked(module: ModuleViewModel.ModuleInfo) {
        val isUninstall = !module.remove
        if (isUninstall) {
            val formatter = if (module.metamodule) metaModuleUninstallConfirm else moduleUninstallConfirm
            val confirmResult = confirmDialog.awaitConfirm(
                moduleStr,
                content = formatter.format(module.name),
                confirm = uninstall,
                dismiss = cancel
            )
            if (confirmResult != ConfirmResult.Confirmed) {
                return
            }
        }

        val success = loadingDialog.withLoading {
            withContext(Dispatchers.IO) {
                if (isUninstall) {
                    Shortcut.deleteModuleActionShortcut(context, module.dirId)
                    Shortcut.deleteModuleWebUiShortcut(context, module.dirId)
                    uninstallModule(module.dirId)
                } else {
                    undoUninstallModule(module.dirId)
                }
            }
        }

        if (success) {
            viewModel.fetchModuleList()
            viewModel.markNeedRefresh()
        }
        if (!isUninstall) return
        val message = if (success) {
            successUninstall.format(module.name)
        } else {
            failedUninstall.format(module.name)
        }
        val actionLabel = if (success) {
            reboot
        } else {
            null
        }
        snackBarHost.showSnackbar(
            message = message,
            actionLabel = actionLabel,
            duration = SnackbarDuration.Long,
            onAction = { reboot() },
        )
    }

    YukiPullToRefreshBox(
        modifier = Modifier.fillMaxSize(),
        indicatorTopPadding = contentPadding.calculateTopPadding(),
        onRefresh = {
            viewModel.fetchModuleList()
            onRefreshMounts()
        },
        isRefreshing = viewModel.isRefreshing
    ) {
        val visibleModules = viewModel.moduleList.filter { module ->
            when (mountFilter) {
                "all" -> true
                "sumh-active" -> mountState?.activeSUMHIds?.contains(module.dirId) == true
                else -> mountState?.modules?.get(module.dirId)?.mode == mountFilter
            }
        }
        LazyColumn(
            state = listState,
            modifier = modifier.fillMaxSize(),
            verticalArrangement = Arrangement.spacedBy(12.dp),
            contentPadding = PaddingValues(
                start = contentPadding.calculateStartPadding(layoutDirection) + 20.dp,
                top = contentPadding.calculateTopPadding() + 16.dp,
                end = contentPadding.calculateEndPadding(layoutDirection) + 20.dp,
                // Keep the last module above the dock, FAB, and snackbar.
                bottom = contentPadding.calculateBottomPadding() + LocalBottomBarPadding.current + 16.dp + 56.dp + 16.dp + 48.dp + 6.dp,
            ),
        ) {
            when {
                visibleModules.isEmpty() -> {
                    item {
                        Box(
                            modifier = Modifier.fillParentMaxSize(),
                            contentAlignment = Alignment.Center
                        ) {
                            Column(
                                horizontalAlignment = Alignment.CenterHorizontally,
                                verticalArrangement = Arrangement.Center
                            ) {
                                YukiIcon(
                                    imageVector = Icons.Outlined.Extension,
                                    contentDescription = null,
                                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                                    modifier = Modifier
                                        .padding(bottom = 16.dp)
                                        .size(48.dp)
                                )
                                Text(
                                    text = stringResource(R.string.module_empty),
                                    textAlign = TextAlign.Center,
                                    style = MaterialTheme.typography.bodyLarge,
                                )
                            }
                        }
                    }
                }

                else -> {
                    items(
                        items = visibleModules,
                        key = { it.dirId }
                    ) { module ->
                        val scope = rememberCoroutineScope()
                        val updatedModule by produceState(initialValue = Triple("", "", "")) {
                            scope.launch(Dispatchers.IO) {
                                value = viewModel.checkUpdate(module)
                            }
                        }

                        ModuleItem(
                            navigator = navigator,
                            module = module,
                            updateUrl = updatedModule.first,
                            mountInfo = mountState?.modules?.get(module.dirId),
                            mountConfigEnabled = mountState?.externalOwner?.isEmpty() == true,
                            onMountConfig = { onMountConfig(module.dirId, module.name) },
                            onUninstallClicked = {
                                scope.launch { onModuleUninstallClicked(module) }
                            },
                            onCheckChanged = { newChecked ->
                                val success = withContext(Dispatchers.IO) {
                                    toggleModule(module.dirId, newChecked)
                                }
                                if (success) {
                                    viewModel.fetchModuleList()
                                    snackBarHost.showSnackbar(
                                        message = rebootToApply,
                                        actionLabel = reboot,
                                        duration = SnackbarDuration.Long,
                                        onAction = { reboot() },
                                    )
                                } else {
                                    val message = if (newChecked) failedEnable else failedDisable
                                    snackBarHost.showSnackbar(message.format(module.name))
                                }
                                success
                            },
                            onUpdate = {
                                scope.launch {
                                    onModuleUpdate(
                                        module,
                                        updatedModule.third,
                                        updatedModule.first,
                                        "${module.name}-${updatedModule.second}.zip"
                                    )
                                }
                            },
                            onClick = { clickedModule: ModuleViewModel.ModuleInfo ->
                                onClickModule(clickedModule.dirId, clickedModule.name, clickedModule.hasWebUi)
                            },
                            onAddShortcut = {
                                onAddShortcut(module)
                            }
                        )

                    }
                }
            }
        }
        }

        activeDownload?.let { state ->
            DownloadProgressDialog(
                title = downloadingText.format(state.moduleName),
                message = startDownloadingText.format(state.fileName),
                progress = state.progress,
                onCancel = {
                    downloadHandle?.cancel()
                    downloadHandle = null
                    activeDownload = null
                },
            )
        }
    }

@Composable
fun ModuleItem(
    navigator: DestinationsNavigator,
    module: ModuleViewModel.ModuleInfo,
    updateUrl: String,
    mountInfo: SUMHManager.ModuleInfo? = null,
    mountConfigEnabled: Boolean = false,
    onMountConfig: () -> Unit = {},
    onUninstallClicked: (ModuleViewModel.ModuleInfo) -> Unit,
    onCheckChanged: suspend (Boolean) -> Boolean,
    onUpdate: (ModuleViewModel.ModuleInfo) -> Unit,
    onClick: (ModuleViewModel.ModuleInfo) -> Unit,
    onAddShortcut: () -> Unit = {}
) {
    val context = LocalContext.current
    val resources = LocalResources.current
    val moduleDescriptionMaxLines = context.getSharedPreferences("settings", MODE_PRIVATE)
        .getInt("module_description_max_lines", 4)
        .coerceIn(1, 5)
    val (isHideTagRow, showMoreModuleInfo) = remember {
        val p = context.getSharedPreferences("settings", MODE_PRIVATE)
        Pair(p.getBoolean("is_hide_tag_row", false), p.getBoolean("show_more_module_info", false))
    }

    // 剪贴板管理器和触觉反馈
    val clipboardManager = context.getSystemService(CLIPBOARD_SERVICE) as ClipboardManager
    val hapticFeedback = LocalHapticFeedback.current

    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainerLow.copy(alpha = CardConfig.cardAlpha),
            contentColor = MaterialTheme.colorScheme.onSurface,
        ),
        elevation = getCardElevation(),
        shape = RoundedCornerShape(28.dp),
        border = getCardBorder(),
    ) {
        val textDecoration = if (!module.remove) null else TextDecoration.LineThrough
        val viewModel = viewModel<ModuleViewModel>()
        
        var localEnabled by remember(module.enabled) { mutableStateOf(module.enabled) }
        val scope = rememberCoroutineScope()

        val sizeStr = remember(module.dirId) {
            viewModel.getModuleSize(module.dirId)
        }

        Column(
            modifier = Modifier.padding(20.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                HomeCardIcon(
                    icon = Icons.Outlined.Extension,
                    containerColor = if (localEnabled) MaterialTheme.colorScheme.secondaryContainer
                    else MaterialTheme.colorScheme.surfaceContainerHighest,
                    tint = if (localEnabled) MaterialTheme.colorScheme.primary
                    else MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(
                    text = module.name,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    textDecoration = textDecoration,
                    modifier = Modifier.weight(1f),
                )
                YukiSwitch(
                    modifier = Modifier.semantics { contentDescription = module.name },
                    enabled = !module.update,
                    checked = localEnabled,
                    onCheckedChange = { newChecked ->
                        localEnabled = newChecked
                        scope.launch {
                            val success = onCheckChanged(newChecked)
                            if (!success) localEnabled = !newChecked
                        }
                    },
                )
            }

            Spacer(Modifier.height(12.dp))
            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(16.dp),
                verticalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                Text(
                    text = "${stringResource(R.string.module_version)}: ${module.version}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textDecoration = textDecoration,
                )
                Text(
                    text = "${stringResource(R.string.module_author)}: ${module.author}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textDecoration = textDecoration,
                )
            }

            if (showMoreModuleInfo && module.updateJson.isNotEmpty()) {
                Spacer(Modifier.height(8.dp))
                val updateJsonLabel = stringResource(R.string.module_update_json)
                Text(
                    text = "$updateJsonLabel: ${module.updateJson}",
                    style = MaterialTheme.typography.bodySmall,
                    textDecoration = textDecoration,
                    color = MaterialTheme.colorScheme.primary,
                    maxLines = 5,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.fillMaxWidth().combinedClickable(
                        onClick = {},
                        onLongClick = {
                            clipboardManager.setPrimaryClip(ClipData.newPlainText("Update JSON URL", module.updateJson))
                            hapticFeedback.performHapticFeedback(HapticFeedbackType.LongPress)
                            Toast.makeText(
                                context,
                                resources.getString(R.string.module_update_json_copied),
                                Toast.LENGTH_SHORT,
                            ).show()
                        },
                    ),
                )
            }

            Spacer(Modifier.height(12.dp))
            Text(
                text = module.description,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                overflow = TextOverflow.Ellipsis,
                maxLines = moduleDescriptionMaxLines,
                textDecoration = textDecoration,
            )

            if (!isHideTagRow) {
                val showMountStrategy = mountInfo != null && mountConfigEnabled && !module.metamodule
                Spacer(Modifier.height(8.dp))
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (module.metamodule || showMountStrategy) {
                        FlowRow(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                            verticalArrangement = Arrangement.spacedBy(8.dp),
                        ) {
                            if (module.metamodule) ModuleTag("META")
                            if (showMountStrategy) {
                                val strategy = if (mountInfo.mode == "none") "none" else mountInfo.strategy
                                ModuleTag(stringResource(when (strategy) {
                                    "sumh" -> R.string.sumh_mount_mode_sumh
                                    "overlay" -> R.string.sumh_mount_mode_overlay
                                    "magic" -> R.string.sumh_strategy_magic_mount
                                    else -> R.string.sumh_strategy_not_mounted
                                }))
                            }
                        }
                    }
                    FlowRow(
                        horizontalArrangement = Arrangement.spacedBy(12.dp),
                        verticalArrangement = Arrangement.spacedBy(4.dp),
                    ) {
                        Text(module.dirId, style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Text(sizeStr, style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            }

            Spacer(modifier = Modifier.height(12.dp))

            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.55f))

            Spacer(modifier = Modifier.height(8.dp))

            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(4.dp),
                modifier = Modifier.fillMaxWidth()
            ) {
                if (mountInfo != null && mountConfigEnabled && !module.metamodule) {
                    ModuleActionButton(
                        enabled = !module.remove && localEnabled,
                        onClick = onMountConfig,
                        imageVector = Icons.Outlined.Folder,
                        contentDescription = stringResource(R.string.sumh_mount_config)
                    )
                }
                if (module.hasActionScript) {
                    ModuleActionButton(
                        enabled = !module.remove && localEnabled,
                        onClick = {
                            navigator.navigate(ExecuteModuleActionScreenDestination(module.dirId))
                            viewModel.markNeedRefresh()
                        },
                        imageVector = Icons.Outlined.PlayArrow,
                        contentDescription = stringResource(R.string.action)
                    )
                }

                if (module.hasWebUi) {
                    ModuleActionButton(
                        enabled = !module.remove && localEnabled,
                        onClick = { onClick(module) },
                        imageVector = Icons.AutoMirrored.Outlined.Wysiwyg,
                        contentDescription = "WebUI"
                    )
                }

                if (module.hasActionScript || module.hasWebUi) {
                    ModuleActionButton(
                        enabled = !module.remove,
                        onClick = onAddShortcut,
                        imageVector = Icons.Outlined.AddCircle,
                        contentDescription = stringResource(R.string.module_shortcut_add),
                        label = stringResource(R.string.ui_module_shortcut_action),
                    )
                }

                if (updateUrl.isNotEmpty()) {
                    ModuleActionButton(
                        enabled = !module.remove,
                        onClick = { onUpdate(module) },
                        imageVector = Icons.Outlined.Download,
                        contentDescription = stringResource(R.string.module_update),
                        prominent = true
                    )
                }

                ModuleActionButton(
                    onClick = { onUninstallClicked(module) },
                    imageVector = if (!module.remove) Icons.Outlined.Delete else Icons.Outlined.Refresh,
                    modifier = if (!module.remove) Modifier else Modifier.rotate(180f),
                    contentDescription = stringResource(if (!module.remove) R.string.uninstall else R.string.cancel),
                    destructive = !module.remove,
                )
            }
        }
    }
}

@Composable
private fun ModuleTag(text: String, emphasized: Boolean = false) {
    Surface(
        shape = MaterialTheme.shapes.extraSmall,
        color = if (emphasized) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceContainerHigh,
        contentColor = if (emphasized) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
    ) {
        Text(text, style = MaterialTheme.typography.labelMedium,
            modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp))
    }
}

@Composable
private fun ModuleActionButton(
    imageVector: ImageVector,
    contentDescription: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    prominent: Boolean = false,
    destructive: Boolean = false,
    label: String = contentDescription,
) {
    val content: @Composable RowScope.() -> Unit = {
        YukiIcon(
            modifier = modifier.size(20.dp),
            imageVector = imageVector,
            contentDescription = null,
        )
        Spacer(Modifier.width(8.dp))
        Text(label, modifier = Modifier.weight(1f, fill = false).clearAndSetSemantics {})
    }
    val buttonModifier = Modifier.heightIn(min = 48.dp)
        .semantics { this.contentDescription = contentDescription }

    if (prominent) {
        FilledTonalButton(
            modifier = buttonModifier,
            enabled = enabled,
            onClick = onClick,
            contentPadding = PaddingValues(horizontal = 12.dp, vertical = 8.dp),
            content = content,
        )
    } else {
        TextButton(
            modifier = buttonModifier,
            enabled = enabled,
            onClick = onClick,
            colors = ButtonDefaults.textButtonColors(
                contentColor = if (destructive) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.primary,
            ),
            contentPadding = PaddingValues(horizontal = 12.dp, vertical = 8.dp),
            content = content,
        )
    }
}

@Preview(name = "Module · light", widthDp = 390)
@Preview(name = "Module · dark", widthDp = 390, uiMode = Configuration.UI_MODE_NIGHT_YES)
@Preview(name = "Module · large text", widthDp = 320, fontScale = 2f)
@Preview(name = "Module · landscape", widthDp = 800)
@Composable
fun ModuleItemPreview() {
    val module = ModuleViewModel.ModuleInfo(
        id = "zygisk_lsposed",
        name = "LSPosed",
        version = "v2.2.0 (7854)",
        versionCode = 1,
        author = "LSPosed Developers",
        description = "Another enhanced implementation of Xposed Framework. Supports Android 9 ~ 17. Requires Zygisk enabled.",
        enabled = true,
        update = false,
        remove = false,
        updateJson = "",
        hasWebUi = true,
        hasActionScript = true,
        metamodule = false,
        dirId = "zygisk_lsposed",
        config = ModuleConfig()
    )
    UtilityPreviewTheme {
        Surface(color = MaterialTheme.colorScheme.background) {
            Column(Modifier.padding(16.dp)) {
                ModuleItem(
                    navigator = EmptyDestinationsNavigator,
                    module = module,
                    updateUrl = "",
                    onUninstallClicked = {},
                    onCheckChanged = { true },
                    onUpdate = {},
                    onClick = {},
                )
            }
        }
    }
}
