package com.zying.zysu.ui.screen

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.os.Build
import android.system.Os
import androidx.annotation.DrawableRes
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.StringRes
import androidx.compose.animation.*
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Engineering
import androidx.compose.material.icons.filled.*
import androidx.compose.material.icons.automirrored.filled.NavigateNext
import androidx.compose.material.icons.outlined.Block
import androidx.compose.material.icons.outlined.Download
import androidx.compose.material.icons.outlined.Shield
import androidx.compose.material.icons.outlined.TaskAlt
import androidx.compose.material.icons.outlined.Warning
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.core.content.edit
import androidx.core.content.pm.PackageInfoCompat
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.repeatOnLifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.InstallScreenDestination
import com.ramcosta.composedestinations.generated.destinations.ModuleScreenDestination
import com.ramcosta.composedestinations.generated.destinations.SuperUserScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.zying.zysu.KernelVersion
import com.zying.zysu.Natives
import com.zying.zysu.R
import com.zying.zysu.integrity.KsudIntegrity
import com.zying.zysu.integrity.KsudIntegrityStatus
import com.zying.zysu.superkey.SuperKeyHelper
import com.zying.zysu.ui.component.AppLogo
import com.zying.zysu.ui.component.KsuIsValid
import com.zying.zysu.ui.component.LocalBottomBarPadding
import com.zying.zysu.ui.sumh.HomeSUMHCard
import com.zying.zysu.ui.sumh.SUMHStatusSummaryCard
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter
import com.zying.zysu.ui.component.KsudStatusCard
import com.zying.zysu.ui.component.HomeCardIcon
import com.zying.zysu.ui.component.HomeDeviceOverview
import com.zying.zysu.ui.component.HomeCountCards
import com.zying.zysu.ui.component.HomeSectionTitle
import com.zying.zysu.ui.component.HomeStatusLayout
import com.zying.zysu.ui.component.rememberConfirmDialog
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.component.YukiTopAppBar
import com.zying.zysu.ui.component.YukiPageScaffold
import com.zying.zysu.ui.component.YukiTopBarTitle
import com.zying.zysu.ui.component.YukiPullToRefreshBox
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.component.clickHapticFeedback
import com.zying.zysu.ui.theme.CardConfig
import com.zying.zysu.ui.theme.getCardBorder
import com.zying.zysu.ui.theme.getCardColors
import com.zying.zysu.ui.theme.getCardShape
import com.zying.zysu.ui.theme.getCardElevation
import com.zying.zysu.ui.theme.isExpressiveUi
import com.zying.zysu.ui.theme.UtilityPreviewTheme
import com.zying.zysu.ui.util.LocalSnackbarHost
import com.zying.zysu.ui.util.KsuCli
import com.zying.zysu.ui.util.checkNewVersion
import com.zying.zysu.ui.util.module.LatestVersionInfo
import com.zying.zysu.ui.util.reboot
import com.zying.zysu.ui.viewmodel.HomeViewModel
import com.zying.zysu.ui.viewmodel.HomeDeviceMetrics
import com.zying.zysu.ui.viewmodel.HomeDeviceMetricsReader
import com.zying.zysu.ui.component.SuperKeyDialog
import com.zying.zysu.ui.component.rememberSuperKeyDialog
import com.zying.zysu.ui.component.SuperKeyAuthResult
import com.zying.zysu.ui.activity.util.AppData
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext

/**
 * @author ShirkNeko
 * @date 2025/9/29.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>(start = true)
@Composable
fun HomeScreen(navigator: DestinationsNavigator) {
    val context = LocalContext.current
    val viewModel = viewModel<HomeViewModel>()
    val coroutineScope = rememberCoroutineScope()
    val ksudIntegrityStatus by KsudIntegrity.status.collectAsState()
    val notificationPermissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted ->
        if (granted) {
            coroutineScope.launch {
                KsudIntegrity.refresh(context, notifyOnMismatch = false)
            }
        }
    }

    val lifecycleOwner = LocalLifecycleOwner.current
    LaunchedEffect(viewModel, lifecycleOwner) {
        lifecycleOwner.lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
            viewModel.loadUserSettings(context)
            viewModel.loadCoreData()?.join()
            viewModel.loadExtendedData(context)?.join()
            // A retained Home tab must reflect grants/revocations made on another tab.
            viewModel.autoRefreshIfNeeded(context, forceRefresh = true)
            while (isActive) {
                delay(15_000)
                viewModel.autoRefreshIfNeeded(context)
            }
        }
    }

    val homeRefreshKey by viewModel.dataRefreshTrigger.collectAsState()
    var hasLoadedCore by remember(viewModel) { mutableStateOf(false) }
    var hasLoadedExtended by remember(viewModel) { mutableStateOf(false) }
    LaunchedEffect(viewModel.isCoreDataLoaded, viewModel.isExtendedDataLoaded) {
        if (viewModel.isCoreDataLoaded) hasLoadedCore = true
        if (viewModel.isExtendedDataLoaded) hasLoadedExtended = true
    }
    val deviceMetrics by produceState<HomeDeviceMetrics?>(null, lifecycleOwner, viewModel.showDeviceMetrics && !viewModel.isSimpleMode, homeRefreshKey) {
        if (viewModel.showDeviceMetrics && !viewModel.isSimpleMode) {
            lifecycleOwner.lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                while (isActive) {
                    value = withContext(Dispatchers.IO) { HomeDeviceMetricsReader.read(context.applicationContext) }
                    delay(10_000)
                }
            }
        }
    }

    // SuperKey 对话框
    val superKeyDialog = rememberSuperKeyDialog()
    var superKeyAuthSuccess by remember { mutableStateOf(false) }
    val snackbarHostState = LocalSnackbarHost.current

    LaunchedEffect(viewModel.isCoreDataLoaded, superKeyAuthSuccess) {
        if (viewModel.isCoreDataLoaded) {
            KsudIntegrity.refresh(context, notifyOnMismatch = false)
        }
    }

    LaunchedEffect(viewModel.isCoreDataLoaded, viewModel.systemStatus.ksuVersion) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            viewModel.isCoreDataLoaded &&
            viewModel.systemStatus.ksuVersion != null &&
            context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            val preferences = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
            if (!preferences.getBoolean(
                    KsudIntegrity.NOTIFICATION_PERMISSION_REQUESTED,
                    false,
                )
            ) {
                preferences.edit {
                    putBoolean(KsudIntegrity.NOTIFICATION_PERMISSION_REQUESTED, true)
                }
                notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
            }
        }
    }

    // 检查内核是否配置了 SuperKey / 签名（异步，避免阻塞主线程）
    val isSuperKeyConfigured by produceState(initialValue = false) {
        value = withContext(Dispatchers.IO) { Natives.isSuperKeyConfigured() }
    }
    val isSignatureOk by produceState(initialValue = false) {
        value = withContext(Dispatchers.IO) { Natives.isSignatureOk() }
    }
    SuperKeyDialog(
        state = superKeyDialog,
        onAuthenticate = { superKey ->
            // 在 IO 线程执行 Native 调用，避免阻塞主线程
            try {
                kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
                    val success = Natives.authenticateSuperKey(superKey)
                    if (success) {
                        SuperKeyHelper.saveAuthenticatedSuperKey(context, superKey)
                    }
                    success
                }
            } catch (e: Exception) {
                android.util.Log.e("SuperKey", "Authentication error", e)
                false
            }
        },
        onResult = { result ->
            when (result) {
                is SuperKeyAuthResult.Success -> {
                    superKeyAuthSuccess = true
                    // 强制刷新状态 - 认证成功后需要重新创建 Shell
                    coroutineScope.launch {
                        // 等待内核状态更新
                        delay(100)
                        // 重新创建 Shell（之前的 Shell 没有 root 权限）
                        withContext(Dispatchers.IO) {
                            KsuCli.refreshShells()
                        }
                        // 强制刷新数据
                        viewModel.refreshData(context, forceRefresh = true)
                        withContext(Dispatchers.IO) {
                            AppData.DataRefreshManager.refreshData()
                        }
                    }
                }
                is SuperKeyAuthResult.Error -> {
                    coroutineScope.launch {
                        snackbarHostState.showSnackbar(result.message)
                    }
                }
                SuperKeyAuthResult.Canceled -> {}
            }
        }
    )

    // 自动尝试用保存的 SuperKey 认证
    LaunchedEffect(viewModel.isCoreDataLoaded) {
        if (viewModel.isCoreDataLoaded && !viewModel.systemStatus.isManager) {
            val savedKey = SuperKeyHelper.getSavedSuperKey(context)
            if (savedKey != null) {
                try {
                    // 在 IO 线程执行 Native 调用和 Shell 刷新
                    val success = withContext(Dispatchers.IO) {
                        val authSuccess = Natives.authenticateSuperKey(savedKey)
                        if (authSuccess) {
                            SuperKeyHelper.saveAuthenticatedSuperKey(context, savedKey)
                            // 重新创建 Shell（之前的 Shell 没有 root 权限）
                            KsuCli.refreshShells()
                        }
                        authSuccess
                    }
                    if (success) {
                        superKeyAuthSuccess = true
                        // 强制刷新数据
                        viewModel.refreshData(context, forceRefresh = true)
                        withContext(Dispatchers.IO) {
                            AppData.DataRefreshManager.refreshData()
                        }
                    }
                } catch (e: Exception) {
                    android.util.Log.e("SuperKey", "Auto-auth error", e)
                }
            }
        }
    }

    val scrollState = rememberScrollState()

    YukiPageScaffold(
        topBar = {
            TopBar(
                isDataLoaded = viewModel.isCoreDataLoaded
            )
        },
        contentWindowInsets = WindowInsets.safeDrawing.only(
            WindowInsetsSides.Top + WindowInsetsSides.Horizontal
        )
    ) { innerPadding ->
        YukiPullToRefreshBox(
            isRefreshing = viewModel.isRefreshing,
            onRefresh = { viewModel.onPullRefresh(context) },
            indicatorTopPadding = innerPadding.calculateTopPadding(),
            modifier = Modifier
                .fillMaxSize()
        ) {
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .verticalScroll(scrollState)
                    .padding(innerPadding)
                    .wrapContentWidth(Alignment.CenterHorizontally)
                    .widthIn(max = 1000.dp)
                    .padding(bottom = LocalBottomBarPadding.current)
                    .padding(top = 16.dp, start = 20.dp, end = 20.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp)
            ) {
                // 状态卡片
                if (viewModel.isCoreDataLoaded || hasLoadedCore) {
                    val isNotManager = !viewModel.systemStatus.isManager
                    val needsSuperKeyAuth = isNotManager && !superKeyAuthSuccess && viewModel.systemStatus.ksuVersion == null

                    HomeStatusLayout(
                        listLayout = viewModel.useListLayout,
                        status = { tileModifier ->
                            StatusCard(
                                modifier = tileModifier,
                                systemStatus = viewModel.systemStatus,
                                isSuperKeyMode = isSuperKeyConfigured || superKeyAuthSuccess,
                                needsSuperKeyAuth = needsSuperKeyAuth,
                                onClickInstall = { navigator.navigate(InstallScreenDestination) },
                                onSuperKeyAuth = { superKeyDialog.show() },
                                isSignatureOk = isSignatureOk,
                                hideVersion = viewModel.isHideVersion,
                                hideOtherInfo = viewModel.isHideOtherInfo,
                            )
                        },
                        daemon = { tileModifier ->
                            KsudSummaryCard(tileModifier, ksudIntegrityStatus, homeRefreshKey, viewModel)
                        },
                        kernel = { tileModifier ->
                            HomeSUMHCard(
                                modifier = tileModifier,
                                canReadStatus = viewModel.systemStatus.isManager && viewModel.systemStatus.ksuVersion != null,
                                refreshKey = homeRefreshKey,
                            )
                        },
                    )

                    if (ksudIntegrityStatus == KsudIntegrityStatus.MISMATCH) {
                        WarningCard(stringResource(R.string.ksud_integrity_warning))
                    }

                    if (viewModel.systemStatus.requireNewKernel) {
                        WarningCard(
                            message = stringResource(R.string.require_kernel_version),
                            onClick = { navigator.navigate(InstallScreenDestination) },
                        )
                    }
                    if (viewModel.systemStatus.requireNewManager) {
                        WarningCard(stringResource(R.string.require_manager_version))
                    }
                    if (viewModel.systemStatus.showLkmUpdate) {
                        WarningCard(
                            message = stringResource(R.string.home_lkm_update_available),
                            color = MaterialTheme.colorScheme.primary,
                            onClick = { navigator.navigate(InstallScreenDestination) },
                        )
                    }

                    if (viewModel.systemStatus.ksuVersion != null && !viewModel.systemStatus.isRootAvailable) {
                        WarningCard(
                            stringResource(id = R.string.grant_root_failed)
                        )
                    }

                }

                if (viewModel.informationLoadFailed) {
                    WarningCard(stringResource(R.string.home_load_retry), onClick = { viewModel.onPullRefresh(context) })
                }
                if (viewModel.showCounts && !viewModel.isSimpleMode && viewModel.systemStatus.isManager &&
                    (viewModel.isExtendedDataLoaded || hasLoadedExtended)) {
                    HomeCountCards(
                        superuserCount = viewModel.systemInfo.superuserCount,
                        moduleCount = viewModel.systemInfo.moduleCount,
                        onSuperuser = { navigator.navigate(SuperUserScreenDestination) },
                        onModules = { navigator.navigate(ModuleScreenDestination) },
                        listLayout = viewModel.useListLayout,
                    )
                }
                if (viewModel.showDeviceMetrics && !viewModel.isSimpleMode) {
                    HomeDeviceOverview(deviceMetrics, listLayout = viewModel.useListLayout)
                }

                if (viewModel.isExtendedDataLoaded || hasLoadedExtended) {
                    val checkUpdate = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
                        .getBoolean("check_update", true)
                    if (checkUpdate) {
                        UpdateCard()
                    }

                    // 信息卡片
                    InfoCard(
                        systemInfo = viewModel.systemInfo,
                        isSimpleMode = viewModel.isSimpleMode,
                        canReadHookType = viewModel.systemStatus.isManager &&
                            viewModel.systemStatus.ksuVersion != null,
                        isHideZygiskImplement = viewModel.isHideZygiskImplement,
                        isHideMetaModuleImplement = viewModel.isHideMetaModuleImplement,
                        isHideSeccompStatus = viewModel.isHideSeccompStatus,
                        showDeviceDetails = viewModel.showDeviceDetails,
                        managerUapiVersion = viewModel.systemStatus.managerUapiVersion,
                    )

                }

                if (viewModel.isExtendedDataLoaded || hasLoadedExtended) {
                    // 链接卡片
                    if (!viewModel.isSimpleMode && !viewModel.isHideLinkCard) {
                        ElevatedCard(
                            modifier = Modifier.border(getCardBorder(), getCardShape()),
                            shape = getCardShape(),
                            colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
                            elevation = getCardElevation(),
                        ) {
                            ContributionCard()
                            HorizontalDivider(
                                modifier = Modifier.padding(start = 56.dp, end = 16.dp),
                                color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.6f),
                            )
                            DonateCard()
                        }
                    }
                }

                if (!viewModel.isExtendedDataLoaded && !hasLoadedExtended) {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(24.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        CircularProgressIndicator()
                    }
                }

                Spacer(Modifier.height(16.dp))
            }
        }
    }
}

@Composable
fun UpdateCard() {
    val context = LocalContext.current
    val latestVersionInfo = LatestVersionInfo()
    val newVersion by produceState(initialValue = latestVersionInfo) {
        value = withContext(Dispatchers.IO) {
            checkNewVersion()
        }
    }

    val currentVersionCode = getManagerVersion(context).second
    val newVersionCode = newVersion.versionCode
    val newVersionUrl = newVersion.downloadUrl
    val changelog = newVersion.changelog

    val uriHandler = LocalUriHandler.current
    val title = stringResource(id = R.string.module_changelog)
    val updateText = stringResource(id = R.string.module_update)

    AnimatedVisibility(
        visible = newVersionCode > currentVersionCode,
        enter = fadeIn(tween(180)),
        exit = fadeOut(tween(120))
    ) {
        val updateDialog = rememberConfirmDialog(onConfirm = { uriHandler.openUri(newVersionUrl) })
        WarningCard(
            message = stringResource(id = R.string.new_version_available).format(newVersionCode),
            color = MaterialTheme.colorScheme.outlineVariant,
            onClick = {
                if (changelog.isEmpty()) {
                    uriHandler.openUri(newVersionUrl)
                } else {
                    updateDialog.showConfirm(
                        title = title,
                        content = changelog,
                        markdown = true,
                        confirm = updateText
                    )
                }
            }
        )
    }
}

private data class RebootMenuOption(
    @param:StringRes val label: Int,
    val reason: String = "",
)

@Composable
private fun RebootDropdownItem(
    option: RebootMenuOption,
    index: Int,
    count: Int,
    onSelected: () -> Unit,
) {
    if (isExpressiveUi) {
        val shape = MenuDefaults.itemShape(index, count).shape
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(vertical = ListItemDefaults.SegmentedGap / 2)
                .defaultMinSize(minHeight = 48.dp)
                .clip(shape)
                .background(
                    MaterialTheme.colorScheme.surfaceContainer.copy(
                        alpha = CardConfig.cardAlpha
                    )
                )
                .clickable {
                    onSelected()
                    reboot(option.reason)
                }
                .padding(horizontal = 12.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = stringResource(option.label),
                style = MaterialTheme.typography.labelLarge,
                fontWeight = FontWeight.Normal,
            )
        }
    } else {
        DropdownMenuItem(
            text = { Text(stringResource(option.label)) },
            onClick = {
                onSelected()
                reboot(option.reason)
            },
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TopBar(
    isDataLoaded: Boolean = false
) {
    YukiTopAppBar(
        title = {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                AppLogo(
                    modifier = Modifier.size(28.dp),
                )
                YukiTopBarTitle(
                    text = stringResource(R.string.app_name),
                )
            }
        },
        actions = {
            if (isDataLoaded) {
                var showDropdown by remember { mutableStateOf(false) }
                KsuIsValid {
                    IconButton(onClick = {
                        showDropdown = true
                    }) {
                        YukiIcon(
                            imageVector = Icons.Filled.PowerSettingsNew,
                            contentDescription = stringResource(id = R.string.reboot)
                        )

                        val rebootOptions = buildList {
                            add(RebootMenuOption(R.string.reboot))
                            add(RebootMenuOption(R.string.reboot_recovery, "recovery"))
                            add(RebootMenuOption(R.string.reboot_bootloader, "bootloader"))
                            add(RebootMenuOption(R.string.reboot_fastbootd, "fastboot"))
                            add(RebootMenuOption(R.string.reboot_download, "download"))
                            add(RebootMenuOption(R.string.reboot_edl, "edl"))
                        }

                        DropdownMenu(
                            expanded = showDropdown,
                            onDismissRequest = { showDropdown = false },
                            modifier = Modifier.clickHapticFeedback(),
                        ) {
                            rebootOptions.forEachIndexed { index, option ->
                                RebootDropdownItem(
                                    option = option,
                                    index = index,
                                    count = rebootOptions.size,
                                    onSelected = { showDropdown = false },
                                )
                            }
                        }
                    }
                }
            }
        },
    )
}

@Composable
private fun StatusCard(
    systemStatus: HomeViewModel.SystemStatus,
    modifier: Modifier = Modifier,
    isSuperKeyMode: Boolean = false,
    needsSuperKeyAuth: Boolean = false,
    isSignatureOk: Boolean = false,
    hideVersion: Boolean = false,
    hideOtherInfo: Boolean = false,
    onClickInstall: () -> Unit = {},
    onSuperKeyAuth: () -> Unit = {},
) {
    val installed = systemStatus.ksuVersion != null
    val safeMode = installed && Natives.isSafeMode
    val version = systemStatus.ksuFullVersion?.takeIf { installed && !hideVersion }
    val machine = if (installed) remember { Os.uname().machine } else null
    val labels = buildList {
        if (installed && isSignatureOk) add(stringResource(R.string.home_auth_signature_tag))
        if (installed && isSuperKeyMode) add(stringResource(R.string.home_auth_superkey_tag))
        if (machine != null && machine != "aarch64") add(machine)
        if (version != null && systemStatus.showCustomLkmBadge) add(stringResource(R.string.home_lkm_custom))
    }
    InstallationStatusCard(
        status = stringResource(when {
            !installed -> R.string.home_not_installed
            safeMode -> R.string.safe_mode
            else -> R.string.home_working
        }),
        installed = installed,
        safeMode = safeMode,
        onClickInstall = onClickInstall,
        modifier = modifier,
        version = version?.let { stringResource(R.string.home_working_version, it) },
        buildInfo = version?.let {
            if (systemStatus.kernelUapiVersion > 0) {
                stringResource(R.string.home_status_build_api, systemStatus.ksuVersion ?: 0, systemStatus.kernelUapiVersion)
            } else stringResource(R.string.home_status_build, systemStatus.ksuVersion ?: 0)
        },
        labels = if (hideOtherInfo) emptyList() else labels,
        needsSuperKeyAuth = !installed && needsSuperKeyAuth,
        onSuperKeyAuth = onSuperKeyAuth,
    )
}

/** Pure presentation shared by the live status and previews of every installation state. */
@Composable
private fun InstallationStatusCard(
    status: String,
    installed: Boolean,
    safeMode: Boolean,
    onClickInstall: () -> Unit,
    modifier: Modifier = Modifier,
    version: String? = null,
    buildInfo: String? = null,
    labels: List<String> = emptyList(),
    needsSuperKeyAuth: Boolean = false,
    onSuperKeyAuth: () -> Unit = {},
) {
    val statusColor = if (installed) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error
    ElevatedCard(
        onClick = onClickInstall,
        modifier = modifier
            .heightIn(min = 168.dp)
            .border(getCardBorder(statusColor.copy(alpha = 0.16f)), getCardShape()),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
        shape = getCardShape(),
        elevation = getCardElevation(),
    ) {
        Column(
            modifier = Modifier.fillMaxWidth().padding(20.dp),
            horizontalAlignment = Alignment.Start,
            verticalArrangement = Arrangement.SpaceBetween,
        ) {
            Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    HomeCardIcon(
                        icon = when {
                            !installed -> Icons.Outlined.Download
                            safeMode -> Icons.Outlined.Shield
                            else -> Icons.Outlined.TaskAlt
                        },
                        tint = statusColor,
                        containerColor = if (installed) MaterialTheme.colorScheme.secondaryContainer else MaterialTheme.colorScheme.errorContainer,
                    )
                    Text(
                        text = status,
                        modifier = Modifier.weight(1f),
                        style = MaterialTheme.typography.headlineSmall,
                        fontWeight = FontWeight.SemiBold,
                        color = statusColor,
                    )
                    if (needsSuperKeyAuth) {
                        IconButton(onClick = onSuperKeyAuth) {
                            YukiIcon(Icons.Default.Key, stringResource(R.string.superkey_auth_title), tint = MaterialTheme.colorScheme.tertiary)
                        }
                    }
            }
            Column(
                modifier = Modifier.fillMaxWidth().padding(top = 16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                if (installed) {
                    if (version != null) {
                        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text(version, style = MaterialTheme.typography.bodyMedium)
                            buildInfo?.let {
                                Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        }
                    }
                    if (labels.isNotEmpty()) {
                        FlowRow(
                            horizontalArrangement = Arrangement.spacedBy(4.dp),
                            verticalArrangement = Arrangement.spacedBy(4.dp),
                        ) {
                            labels.forEach { StatusBadge(it) }
                        }
                    }
                } else {
                    Text(
                        text = stringResource(R.string.home_click_to_install),
                        style = MaterialTheme.typography.labelLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
    }
}

@Composable
private fun StatusBadge(label: String) {
    Surface(shape = CircleShape, color = MaterialTheme.colorScheme.secondaryContainer) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelMedium,
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp),
            color = MaterialTheme.colorScheme.onSecondaryContainer,
        )
    }
}

@Composable
fun WarningCard(
    message: String,
    color: Color = MaterialTheme.colorScheme.error,
    onClick: (() -> Unit)? = null
) {
    ElevatedCard(
        modifier = Modifier.border(getCardBorder(), getCardShape()),
        shape = getCardShape(),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
        elevation = getCardElevation(),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .then(onClick?.let { Modifier.clickable { it() } } ?: Modifier)
                .padding(16.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            YukiIcon(
                imageVector = Icons.Outlined.Warning,
                contentDescription = null,
                tint = color,
                modifier = Modifier.size(24.dp),
            )
            Spacer(Modifier.width(12.dp))
            Text(
                text = message,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurface,
                modifier = Modifier.weight(1f),
            )
        }
    }
}

@Composable
private fun HomeLinkRow(
    title: String,
    content: String,
    icon: ImageVector,
    onClick: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .defaultMinSize(minHeight = 64.dp)
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        YukiIcon(
            imageVector = icon,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(24.dp),
        )
        Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(text = title, style = MaterialTheme.typography.titleSmall)
            Text(
                text = content,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        YukiIcon(
            imageVector = Icons.AutoMirrored.Filled.NavigateNext,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(20.dp),
        )
    }
}

@Composable
fun ContributionCard() {
    val uriHandler = LocalUriHandler.current
    HomeLinkRow(
        title = stringResource(R.string.home_ContributionCard_kernelsu),
        content = stringResource(R.string.home_click_to_ContributionCard_kernelsu),
        icon = Icons.Default.Code,
        onClick = { uriHandler.openUri("https://github.com/wuluoy-creator/ZySU") },
    )
}

@Composable
fun DonateCard() {
    val uriHandler = LocalUriHandler.current
    HomeLinkRow(
        title = stringResource(R.string.home_support_title),
        content = stringResource(R.string.home_support_content),
        icon = Icons.Default.FavoriteBorder,
        onClick = { uriHandler.openUri("https://patreon.com/weishu") },
    )
}

@Composable
private fun HomeInfoItem(
    label: String,
    content: String,
    icon: ImageVector? = null,
    @DrawableRes iconRes: Int? = null,
    contentColor: Color = Color.Unspecified,
    onClick: (() -> Unit)? = null,
    trailing: @Composable (() -> Unit)? = null,
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier
            .fillMaxWidth()
            .defaultMinSize(minHeight = 64.dp)
            .then(onClick?.let { Modifier.clickable(onClick = it) } ?: Modifier)
            .padding(horizontal = 16.dp, vertical = 12.dp),
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        if (icon != null || iconRes != null) {
            if (iconRes != null) {
                Icon(
                    painter = painterResource(iconRes),
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.size(24.dp),
                )
            } else {
                YukiIcon(
                    imageVector = icon!!,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.size(24.dp),
                )
            }
        }
        Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(
                text = label,
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            SelectionContainer {
                Text(
                    text = content.ifBlank { stringResource(R.string.home_reading_unavailable) },
                    style = MaterialTheme.typography.bodyMedium,
                    color = if (contentColor == Color.Unspecified) MaterialTheme.colorScheme.onSurface else contentColor,
                )
            }
        }
        trailing?.invoke()
        if (onClick != null && trailing == null) {
            YukiIcon(
                imageVector = Icons.AutoMirrored.Filled.NavigateNext,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(20.dp),
            )
        }
    }
}

private data class HomeInfoEntry(
    val label: String,
    val content: String,
    val icon: ImageVector? = null,
    @DrawableRes val iconRes: Int? = null,
    val contentColor: Color = Color.Unspecified,
    val onClick: (() -> Unit)? = null,
    val trailing: (@Composable () -> Unit)? = null,
)

@Composable
private fun InfoCard(
    systemInfo: HomeViewModel.SystemInfo,
    isSimpleMode: Boolean,
    canReadHookType: Boolean,
    isHideZygiskImplement: Boolean,
    isHideMetaModuleImplement: Boolean,
    isHideSeccompStatus: Boolean = false,
    showDeviceDetails: Boolean = false,
    managerUapiVersion: Int = 0,
) {
    val unknown = stringResource(R.string.home_reading_unavailable)
    fun String.displayValue() = takeUnless { it.isBlank() || it == "Unknown" } ?: unknown
    val systemEntries = buildList {
        add(HomeInfoEntry(stringResource(R.string.home_device_model), systemInfo.deviceModel.displayValue(), Icons.Default.PhoneAndroid))
        if (!isSimpleMode) {
            add(HomeInfoEntry(stringResource(R.string.home_android_version),
                stringResource(R.string.home_android_api, systemInfo.androidVersion.displayValue(), Build.VERSION.SDK_INT), Icons.Default.Android))
        }
        add(HomeInfoEntry(stringResource(R.string.home_kernel), systemInfo.kernelRelease.displayValue(), Icons.Default.Memory))
        val managerVersion = buildString {
            append(systemInfo.managerVersion.first.displayValue())
            if (systemInfo.managerVersion.second > 0) append(" (${systemInfo.managerVersion.second})")
            if (managerUapiVersion > 0) append(" · UAPI $managerUapiVersion")
        }
        add(HomeInfoEntry(stringResource(R.string.home_manager_version), managerVersion, Icons.Default.SettingsSuggest))
    }
    val runtimeEntries = buildList {
        if (!isSimpleMode && canReadHookType && systemInfo.workingMode != null) {
            add(HomeInfoEntry(stringResource(R.string.home_working_mode), systemInfo.workingMode, Icons.Default.Link))
        }
        add(HomeInfoEntry(stringResource(R.string.home_selinux_status), systemInfo.seLinuxStatus.displayValue(), Icons.Default.Security))
        if (!isHideSeccompStatus) {
            val seccompText = stringResource(when (systemInfo.seccompStatus) {
                -1 -> R.string.seccomp_status_not_supported
                0 -> R.string.seccomp_status_disabled
                1 -> R.string.seccomp_status_strict
                2 -> R.string.seccomp_status_filter
                else -> R.string.seccomp_status_unknown
            })
            add(HomeInfoEntry(stringResource(R.string.home_seccomp_status), seccompText, Icons.Default.LocalPolice))
        }
        if (!isHideZygiskImplement && !isSimpleMode && systemInfo.zygiskImplement.isNotBlank() && systemInfo.zygiskImplement != "None") {
            add(HomeInfoEntry(stringResource(R.string.home_zygisk_implement), systemInfo.zygiskImplement, iconRes = R.drawable.ms_syringe))
        }
        if (!isHideMetaModuleImplement && !isSimpleMode && systemInfo.metaModuleImplement.isNotBlank() && systemInfo.metaModuleImplement != "None") {
            add(HomeInfoEntry(stringResource(R.string.home_meta_module_implement), systemInfo.metaModuleImplement, Icons.Default.Extension))
        }
    }
    Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
        HomeInfoGroup(stringResource(R.string.home_system_section), systemEntries)
        HomeInfoGroup(stringResource(R.string.home_runtime_section), runtimeEntries)
        if (!isSimpleMode) {
            var detailsExpanded by rememberSaveable(showDeviceDetails) { mutableStateOf(showDeviceDetails) }
            TextButton(onClick = { detailsExpanded = !detailsExpanded }, modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp)) {
                YukiIcon(if (detailsExpanded) Icons.Default.ExpandLess else Icons.Default.ExpandMore, null)
                Spacer(Modifier.width(8.dp))
                Text(stringResource(if (detailsExpanded) R.string.home_collapse_details else R.string.home_expand_details))
            }
            if (detailsExpanded) {
                HomeInfoGroup(stringResource(R.string.home_build_section), listOf(
                    HomeInfoEntry(stringResource(R.string.home_security_patch), systemInfo.securityPatch.displayValue(), Icons.Default.Security),
                    HomeInfoEntry(stringResource(R.string.home_architecture), systemInfo.architecture.displayValue(), Icons.Default.DeveloperBoard),
                    HomeInfoEntry(stringResource(R.string.home_build_id), systemInfo.buildId.displayValue(), Icons.Default.Info),
                    HomeInfoEntry(stringResource(R.string.home_fingerprint), systemInfo.fingerprint.displayValue(), Icons.Default.Fingerprint),
                ))
            }
        }
    }
}

@Composable
private fun HomeInfoGroup(title: String, entries: List<HomeInfoEntry>) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        HomeSectionTitle(title)
        ElevatedCard(
            modifier = Modifier.fillMaxWidth().border(getCardBorder(), getCardShape()),
            shape = getCardShape(),
            colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
            elevation = getCardElevation(),
        ) {
            entries.forEachIndexed { index, entry ->
                HomeInfoItem(entry.label, entry.content, entry.icon, entry.iconRes, entry.contentColor, entry.onClick, entry.trailing)
                if (index < entries.lastIndex) {
                    HorizontalDivider(Modifier.padding(start = 56.dp, end = 16.dp), color = MaterialTheme.colorScheme.outlineVariant)
                }
            }
        }
    }
}

@Composable
private fun KsudSummaryCard(
    modifier: Modifier,
    ksudIntegrityStatus: KsudIntegrityStatus,
    refreshKey: Long,
    viewModel: HomeViewModel,
) {
    val state = viewModel.ksudCardState
    val system = viewModel.systemStatus
    LaunchedEffect(ksudIntegrityStatus, refreshKey) { viewModel.refreshKsudCard() }
    val unavailableReason = when {
        !system.isManager -> stringResource(R.string.home_ksud_requires_manager)
        system.kernelUapiVersion <= 0 || system.kernelUapiVersion != system.managerUapiVersion ->
            stringResource(R.string.home_ksud_incompatible)
        !state.loading && !state.hasBundledDaemon -> stringResource(R.string.home_ksud_not_bundled)
        else -> null
    }
    KsudStatusCard(
        bundledVersion = state.bundledVersion,
        installedVersion = state.installedVersion,
        integrity = ksudIntegrityStatus,
        loading = state.loading,
        syncing = state.syncing,
        syncResult = state.syncResult,
        syncUnavailableReason = unavailableReason,
        onSync = viewModel::syncKsudCard,
        modifier = modifier,
    )
}

fun getManagerVersion(context: Context): Pair<String, Long> {
    val packageInfo = context.packageManager.getPackageInfo(context.packageName, 0)!!
    val versionCode = PackageInfoCompat.getLongVersionCode(packageInfo)
    return Pair(packageInfo.versionName!!, versionCode)
}

@Preview(name = "Home cards · light", widthDp = 360, heightDp = 640)
@Preview(name = "Home cards · compact", widthDp = 320, heightDp = 800, locale = "zh-rCN")
@Preview(name = "Home cards · dark", widthDp = 360, heightDp = 640, uiMode = Configuration.UI_MODE_NIGHT_YES)
@Preview(name = "Home cards · large text", widthDp = 320, heightDp = 900, fontScale = 2f)
@Preview(name = "Home cards · landscape", widthDp = 800, heightDp = 360)
@Composable
private fun HomeStatusLayoutPreview() {
    HomeStatusPreviewContent(installed = true)
}

@Preview(name = "Home cards · uninstalled", widthDp = 320, heightDp = 640)
@Composable
private fun UninstalledStatusPreview() {
    HomeStatusPreviewContent(installed = false)
}

@Preview(name = "Home cards · safe mode and mismatch", widthDp = 360, heightDp = 740)
@Composable
private fun SafeModeStatusPreview() {
    HomeStatusPreviewContent(installed = true, safeMode = true)
}

@Composable
private fun HomeStatusPreviewContent(installed: Boolean, safeMode: Boolean = false) {
    UtilityPreviewTheme {
        Surface(color = MaterialTheme.colorScheme.background) {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp)) {
                HomeStatusLayout(
                    status = { modifier ->
                        InstallationStatusCard(
                            status = stringResource(when {
                                !installed -> R.string.home_not_installed
                                safeMode -> R.string.safe_mode
                                else -> R.string.home_working
                            }),
                            installed = installed,
                            safeMode = safeMode,
                            onClickInstall = {},
                            modifier = modifier,
                            version = if (installed) stringResource(R.string.home_working_version, "1.0.0-android16-release") else null,
                            buildInfo = if (installed) stringResource(R.string.home_status_build_api, 12345, 5) else null,
                            labels = if (installed) listOf("Signature", "SuperKey") else emptyList(),
                            needsSuperKeyAuth = !installed,
                        )
                    },
                    daemon = { modifier ->
                        KsudStatusCard(
                            bundledVersion = "v1.3.0-94ca662a-nc",
                            installedVersion = if (installed && !safeMode) "v1.3.0-94ca662a-nc" else null,
                            integrity = when {
                                !installed -> KsudIntegrityStatus.UNAVAILABLE
                                safeMode -> KsudIntegrityStatus.MISMATCH
                                else -> KsudIntegrityStatus.MATCH
                            },
                            loading = false, syncing = false, syncResult = null,
                            syncUnavailableReason = if (installed) null else stringResource(R.string.home_ksud_requires_manager),
                            modifier = modifier, onSync = {},
                        )
                    },
                    kernel = { modifier ->
                        SUMHStatusSummaryCard(
                            status = if (installed) SUMHUiAdapter.SUMHStatus.AVAILABLE else null,
                            version = if (installed) "1" else null,
                            systemInfo = if (installed) SUMHUiAdapter.SystemInfo(
                                kernel = "6.12.52-android16", mountBase = "none",
                                activeMounts = emptyList(), sumhModuleIds = emptyList(),
                                sumhMismatch = safeMode, mismatchMessage = null,
                                viewsEnabled = !safeMode,
                            ) else null,
                            unavailableReason = if (installed) null else stringResource(R.string.home_sumh_requires_manager),
                            modifier = modifier, onRefresh = {},
                        )
                    },
                )
            }
        }
    }
}

@Composable
private fun IncompatibleKernelCard() {
    val currentKver = remember { Natives.version }
    val threshold   = Natives.MINIMAL_NEW_IOCTL_KERNEL

    val msg = stringResource(
        id = R.string.incompatible_kernel_msg,
        currentKver,
        threshold
    )

    WarningCard(
        message = msg,
        color = MaterialTheme.colorScheme.error
    )
}

@Preview
@Composable
private fun WarningCardPreview() {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        WarningCard(message = "Warning message")
        WarningCard(
            message = "Warning message ",
            MaterialTheme.colorScheme.outlineVariant,
            onClick = {})
    }
}
