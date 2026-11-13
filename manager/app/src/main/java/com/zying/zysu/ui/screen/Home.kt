package com.zying.zysu.ui.screen

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.os.Build
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
import androidx.compose.foundation.verticalScroll
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
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.zying.zysu.KernelVersion
import com.zying.zysu.Natives
import com.zying.zysu.R
import com.zying.zysu.integrity.KsudIntegrity
import com.zying.zysu.integrity.KsudIntegrityStatus
import com.zying.zysu.superkey.SuperKeyHelper
import com.zying.zysu.ui.component.AppLogo
import com.zying.zysu.ui.component.LocalBottomBarPadding
import com.zying.zysu.ui.sumh.HomeSUMHCard
import com.zying.zysu.ui.component.KsudStatusCard
import com.zying.zysu.ui.component.rememberConfirmDialog
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.component.YukiTopAppBar
import com.zying.zysu.ui.component.YukiPageScaffold
import com.zying.zysu.ui.component.YukiTopBarTitle
import com.zying.zysu.ui.component.YukiPullToRefreshBox
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
import com.zying.zysu.ui.screen.home.*
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
    LaunchedEffect(viewModel.homeLayout) {
        scrollState.scrollTo(0)
    }

    YukiPageScaffold(
        topBar = {
            TopBar(showReboot = viewModel.systemStatus.isManager && viewModel.systemStatus.ksuVersion != null)
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
                if (viewModel.isCoreDataLoaded || hasLoadedCore) {
                    val needsSuperKeyAuth = !viewModel.systemStatus.isManager && !superKeyAuthSuccess && viewModel.systemStatus.ksuVersion == null
                    val layoutState = homeLayoutState(
                        viewModel,
                        isSignatureOk = isSignatureOk,
                        isSuperKeyMode = isSuperKeyConfigured || superKeyAuthSuccess,
                        needsAuthentication = needsSuperKeyAuth,
                    )
                    HomeLayoutContent(
                        style = viewModel.homeLayout,
                        state = layoutState,
                        onInstall = { navigator.navigate(InstallScreenDestination) },
                        onAuthenticate = { superKeyDialog.show() },
                        daemon = { KsudSummaryCard(it, ksudIntegrityStatus, homeRefreshKey, viewModel) },
                        kernel = { HomeSUMHCard(it,
                            canReadStatus = viewModel.systemStatus.isManager && viewModel.systemStatus.ksuVersion != null,
                            refreshKey = homeRefreshKey) },
                        warnings = {
                            if (ksudIntegrityStatus == KsudIntegrityStatus.MISMATCH) WarningCard(stringResource(R.string.ksud_integrity_warning))
                            if (viewModel.systemStatus.requireNewKernel) WarningCard(stringResource(R.string.require_kernel_version),
                                onClick = { navigator.navigate(InstallScreenDestination) })
                            if (viewModel.systemStatus.requireNewManager) WarningCard(stringResource(R.string.require_manager_version))
                            if (viewModel.systemStatus.showLkmUpdate) WarningCard(stringResource(R.string.home_lkm_update_available),
                                color = MaterialTheme.colorScheme.primary, onClick = { navigator.navigate(InstallScreenDestination) })
                            if (viewModel.systemStatus.ksuVersion != null && !viewModel.systemStatus.isRootAvailable) WarningCard(stringResource(R.string.grant_root_failed))
                            if (viewModel.informationLoadFailed) WarningCard(stringResource(R.string.home_load_retry), onClick = { viewModel.onPullRefresh(context) })
                        },
                    )
                }
                if ((viewModel.isExtendedDataLoaded || hasLoadedExtended) &&
                    context.getSharedPreferences("settings", Context.MODE_PRIVATE).getBoolean("check_update", true)) {
                    UpdateCard()
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
internal fun TopBar(showReboot: Boolean = false) {
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
            if (showReboot) {
                var showDropdown by remember { mutableStateOf(false) }
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
        },
    )
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
private fun homeLayoutState(
    viewModel: HomeViewModel,
    isSignatureOk: Boolean,
    isSuperKeyMode: Boolean,
    needsAuthentication: Boolean,
): HomeLayoutState {
    val system = viewModel.systemStatus
    val info = viewModel.systemInfo
    val installed = system.ksuVersion != null
    val unknown = stringResource(R.string.home_reading_unavailable)
    fun String.readable() = takeUnless { it.isBlank() || it == "Unknown" } ?: unknown
    val version = system.ksuFullVersion?.takeIf { installed && !viewModel.isHideVersion }?.let {
        HomeFact("kernel-module", stringResource(R.string.home_layout_kernel_module), buildString {
            append(it)
            append("\n")
            append(stringResource(R.string.home_status_build_api, system.ksuVersion ?: 0, system.kernelUapiVersion))
        }, homeReleaseLabel(it))
    }
    val badges = if (viewModel.isHideOtherInfo) emptyList() else buildList {
        if (installed && isSignatureOk) add(stringResource(R.string.home_auth_signature_tag))
        if (installed && isSuperKeyMode) add(stringResource(R.string.home_auth_superkey_tag))
        if (installed && system.showCustomLkmBadge) add(stringResource(R.string.home_lkm_custom))
    }
    val facts = buildList {
        add(HomeFact("device", stringResource(R.string.home_device_model), info.deviceModel.readable()))
        add(HomeFact("kernel", stringResource(R.string.home_kernel), info.kernelRelease.readable(), homeReleaseLabel(info.kernelRelease.readable())))
        add(HomeFact("selinux", stringResource(R.string.home_selinux_status), info.seLinuxStatus.readable()))
        val managerVersion = info.managerVersion.first.readable()
        add(HomeFact("manager", stringResource(R.string.home_manager_version), buildString {
            append(managerVersion)
            if (info.managerVersion.second > 0) {
                append("\n")
                append(stringResource(R.string.home_status_build_api, info.managerVersion.second.toInt(), system.managerUapiVersion))
            }
        }, homeReleaseLabel(managerVersion)))
        add(HomeFact("android", stringResource(R.string.home_android_version),
            stringResource(R.string.home_android_api, info.androidVersion.readable(), Build.VERSION.SDK_INT)))
        info.workingMode?.takeIf { system.isManager }?.let { add(HomeFact("mode", stringResource(R.string.home_working_mode), it)) }
        add(HomeFact("seccomp", stringResource(R.string.home_seccomp_status), stringResource(when (info.seccompStatus) {
            -1 -> R.string.seccomp_status_not_supported
            0 -> R.string.seccomp_status_disabled
            1 -> R.string.seccomp_status_strict
            2 -> R.string.seccomp_status_filter
            else -> R.string.seccomp_status_unknown
        })))
        if (info.zygiskImplement.isNotBlank() && info.zygiskImplement != "None") {
            add(HomeFact("zygisk", stringResource(R.string.home_zygisk_implement), info.zygiskImplement))
        }
        if (info.metaModuleImplement.isNotBlank() && info.metaModuleImplement != "None") {
            add(HomeFact("mount", stringResource(R.string.home_meta_module_implement), info.metaModuleImplement))
        }
    }
    val buildFacts = listOf(
        HomeFact("security", stringResource(R.string.home_security_patch), info.securityPatch.readable()),
        HomeFact("architecture", stringResource(R.string.home_architecture), info.architecture.readable()),
        HomeFact("build", stringResource(R.string.home_build_id), info.buildId.readable()),
        HomeFact("fingerprint", stringResource(R.string.home_fingerprint), info.fingerprint.readable()),
    )
    return HomeLayoutState(
        status = HomeStatusPresentation(
            title = stringResource(when { !installed -> R.string.home_not_installed; system.safeMode -> R.string.safe_mode; else -> R.string.home_working }),
            installed = installed, safeMode = system.safeMode, version = version, badges = badges,
            needsAuthentication = needsAuthentication,
        ),
        facts = visibleHomeFacts(facts, viewModel.visibleHomeFactIds),
        buildFacts = visibleHomeFacts(buildFacts, viewModel.visibleHomeFactIds),
        showBuildDetails = viewModel.showDeviceDetails,
    )
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
                HomeLayoutContent(HomeLayoutStyle.Circle, HomeLayoutState(
                    status = HomeStatusPresentation(
                        title = stringResource(when { !installed -> R.string.home_not_installed; safeMode -> R.string.safe_mode; else -> R.string.home_working }),
                        installed = installed, safeMode = safeMode,
                        version = if (installed) HomeFact("module", stringResource(R.string.home_layout_kernel_module), "v1.3.0-94ca662a-nc", "v1.3.0") else null,
                        needsAuthentication = !installed,
                    ),
                    facts = listOf(HomeFact("device", stringResource(R.string.home_device_model), "Android device")),
                ))
            }
        }
    }
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
