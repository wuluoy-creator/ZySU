package com.zying.zysu.ui.screen

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Environment
import android.os.Parcelable
import android.util.Log
import androidx.activity.compose.BackHandler
import androidx.compose.animation.*
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Save
import androidx.compose.material.icons.outlined.Warning
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.activity.ComponentActivity
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.FlashScreenDestination
import com.ramcosta.composedestinations.generated.destinations.ModuleScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.ramcosta.composedestinations.navigation.EmptyDestinationsNavigator
import com.zying.zysu.R
import com.zying.zysu.data.repository.ModuleRepositoryProvider
import com.zying.zysu.superkey.SuperKeyHelper
import com.zying.zysu.ui.component.KeyEventBlocker
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.component.YukiPanel
import com.zying.zysu.ui.component.YukiTopAppBar
import com.zying.zysu.ui.component.YukiTopBarTitle
import com.zying.zysu.ui.theme.isExpressiveUi
import com.zying.zysu.ui.theme.getCardBorder
import com.zying.zysu.ui.util.*
import com.zying.zysu.ui.util.module.ModuleUtils
import com.zying.zysu.ui.viewmodel.ModuleViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.parcelize.Parcelize
import java.io.File
import java.text.SimpleDateFormat
import java.util.*

/**
 * @author ShirkNeko
 * @date 2025/5/31.
 */
enum class FlashingStatus {
    FLASHING,
    SUCCESS,
    FAILED
}

private var currentFlashingStatus = mutableStateOf(FlashingStatus.FLASHING)

// ??????????
data class ModuleInstallStatus(
    val totalModules: Int = 0,
    val currentModule: Int = 0,
    val currentModuleName: String = "",
    val failedModules: MutableList<String> = mutableListOf()
)

private var moduleInstallStatus = mutableStateOf(ModuleInstallStatus())

fun setFlashingStatus(status: FlashingStatus) {
    currentFlashingStatus.value = status
}

fun updateModuleInstallStatus(
    totalModules: Int? = null,
    currentModule: Int? = null,
    currentModuleName: String? = null,
    failedModule: String? = null
) {
    val current = moduleInstallStatus.value
    moduleInstallStatus.value = current.copy(
        totalModules = totalModules ?: current.totalModules,
        currentModule = currentModule ?: current.currentModule,
        currentModuleName = currentModuleName ?: current.currentModuleName
    )

    if (failedModule != null) {
        val updatedFailedModules = current.failedModules.toMutableList()
        updatedFailedModules.add(failedModule)
        moduleInstallStatus.value = moduleInstallStatus.value.copy(
            failedModules = updatedFailedModules
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
@Destination<RootGraph>
fun FlashScreen(navigator: DestinationsNavigator, flashIt: FlashIt) {
    val context = LocalContext.current

    val isExternalInstall = remember {
        when (flashIt) {
            is FlashIt.FlashModule,
            is FlashIt.FlashModules,
            is FlashIt.FlashAk3 -> {
                (context as? ComponentActivity)?.intent?.let { intent ->
                    intent.action == Intent.ACTION_VIEW ||
                        intent.action == Intent.ACTION_SEND ||
                        intent.action == Intent.ACTION_SEND_MULTIPLE
                } ?: false
            }
            else -> false
        }
    }

    var text by rememberSaveable { mutableStateOf("") }
    var tempText: String
    val logContent = remember { StringBuilder() }
    var showFloatAction by rememberSaveable { mutableStateOf(false) }
    var hasFlashCompleted by rememberSaveable { mutableStateOf(false) }
    var hasExecuted by rememberSaveable { mutableStateOf(false) }
    var hasUpdateExecuted by rememberSaveable { mutableStateOf(false) }
    var hasUpdateCompleted by rememberSaveable { mutableStateOf(false) }

    val snackBarHost = rememberSnackbarController()
    val scope = rememberCoroutineScope()
    val scrollState = rememberScrollState()
    val topAppBarState = rememberTopAppBarState()
    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior(topAppBarState)
    val viewModel: ModuleViewModel = viewModel()

    val errorCodeString = stringResource(R.string.error_code)
    val checkLogString = stringResource(R.string.check_log)
    val logSavedString = stringResource(R.string.log_saved)
    val installingModuleString = stringResource(R.string.installing_module)

    // ????????
    val currentStatus = moduleInstallStatus.value

    // ????
    LaunchedEffect(flashIt) {
        when (flashIt) {
            is FlashIt.FlashModules -> {
                if (flashIt.currentIndex == 0) {
                    moduleInstallStatus.value = ModuleInstallStatus(
                        totalModules = flashIt.uris.size,
                        currentModule = 1
                    )
                    hasFlashCompleted = false
                    hasExecuted = false
                }
            }
            is FlashIt.FlashModuleUpdate -> {
                hasUpdateCompleted = false
                hasUpdateExecuted = false
            }
            else -> {
                hasFlashCompleted = false
                hasExecuted = false
            }
        }
    }

    // ????????
    LaunchedEffect(flashIt) {
        if (flashIt !is FlashIt.FlashModuleUpdate) return@LaunchedEffect
        if (hasUpdateExecuted || hasUpdateCompleted || text.isNotEmpty()) {
            return@LaunchedEffect
        }

        hasUpdateExecuted = true

        withContext(Dispatchers.IO) {
            setFlashingStatus(FlashingStatus.FLASHING)

            try {
                logContent.append(text).append("\n")
            } catch (_: Exception) {
                logContent.append(text).append("\n")
            }

            flashModuleUpdate(flashIt.uri, onFinish = { showReboot, code ->
                if (code != 0) {
                    text += "$errorCodeString $code.\n$checkLogString\n"
                    setFlashingStatus(FlashingStatus.FAILED)
                } else {
                    setFlashingStatus(FlashingStatus.SUCCESS)
                    viewModel.markNeedRefresh()
                    scope.launch(Dispatchers.IO) {
                        getModuleIdFromUri(context, flashIt.uri)?.let { moduleId ->
                            ModuleRepositoryProvider.get(context).clearInstalledBinding(moduleId)
                        }
                    }
                }
                if (showReboot) {
                    text += "\n\n\n"
                    showFloatAction = true

                    // ????????????????????
                    if (isExternalInstall) {
                        return@flashModuleUpdate
                    }
                }
                hasUpdateCompleted = true
            }, onStdout = {
                tempText = "$it\n"
                if (tempText.startsWith("[H[J")) { // clear command
                    text = tempText.substring(6)
                } else {
                    text += tempText
                }
                logContent.append(it).append("\n")
            }, onStderr = {
                text += "$it\n"
                logContent.append(it).append("\n")
            })
        }
    }

    // ?????????
    LaunchedEffect(flashIt) {
        if (flashIt is FlashIt.FlashModuleUpdate) return@LaunchedEffect
        if (hasExecuted || hasFlashCompleted || text.isNotEmpty()) {
            return@LaunchedEffect
        }

        hasExecuted = true

        withContext(Dispatchers.IO) {
            setFlashingStatus(FlashingStatus.FLASHING)

            if (flashIt is FlashIt.FlashModules) {
                try {
                    val currentUri = flashIt.uris[flashIt.currentIndex]
                    val moduleName = getModuleNameFromUri(context, currentUri)
                    updateModuleInstallStatus(
                        currentModuleName = moduleName
                    )
                    text = installingModuleString.format(flashIt.currentIndex + 1, flashIt.uris.size, moduleName)
                    logContent.append(text).append("\n")
                } catch (_: Exception) {
                    text = installingModuleString.format(flashIt.currentIndex + 1, flashIt.uris.size, "Module")
                    logContent.append(text).append("\n")
                }
            }

            flashIt(flashIt, onFinish = { showReboot, code ->
                if (code != 0) {
                    text += "$errorCodeString $code.\n$checkLogString\n"
                    setFlashingStatus(FlashingStatus.FAILED)

                    if (flashIt is FlashIt.FlashModules) {
                        updateModuleInstallStatus(
                            failedModule = moduleInstallStatus.value.currentModuleName
                        )
                    }
                } else {
                    when (flashIt) {
                        is FlashIt.FlashBoot ->
                            SuperKeyHelper.savePatchSuperKey(context, flashIt.superKey)
                        is FlashIt.DownloadBoot ->
                            SuperKeyHelper.savePatchSuperKey(context, flashIt.superKey)
                        else -> Unit
                    }
                    setFlashingStatus(FlashingStatus.SUCCESS)
                    viewModel.markNeedRefresh()
                    if (flashIt is FlashIt.FlashModule &&
                        flashIt.repositorySourceId != null &&
                        flashIt.repositoryModuleId != null &&
                        flashIt.repositoryVersionCode != null &&
                        flashIt.repositoryVersionName != null
                    ) {
                        scope.launch(Dispatchers.IO) {
                            val actualModuleId = getModuleIdFromUri(context, flashIt.uri)
                            if (actualModuleId == flashIt.repositoryModuleId) {
                                ModuleRepositoryProvider.get(context).recordInstalledBinding(
                                    moduleId = actualModuleId,
                                    sourceId = flashIt.repositorySourceId,
                                    version = flashIt.repositoryVersionName,
                                    versionCode = flashIt.repositoryVersionCode,
                                )
                            } else {
                                actualModuleId?.let {
                                    ModuleRepositoryProvider.get(context).clearInstalledBinding(it)
                                }
                                Log.w(
                                    "FlashScreen",
                                    "Repository module id ${flashIt.repositoryModuleId} does not match archive id $actualModuleId; source binding skipped"
                                )
                            }
                        }
                    } else {
                        val installedUri = when (flashIt) {
                            is FlashIt.FlashModule -> flashIt.uri
                            is FlashIt.FlashModules -> flashIt.uris.getOrNull(flashIt.currentIndex)
                            else -> null
                        }
                        if (installedUri != null) {
                            scope.launch(Dispatchers.IO) {
                                getModuleIdFromUri(context, installedUri)?.let { moduleId ->
                                    ModuleRepositoryProvider.get(context)
                                        .clearInstalledBinding(moduleId)
                                }
                            }
                        }
                    }
                }
                if (showReboot) {
                    text += "\n\n\n"
                    showFloatAction = true
                }

                hasFlashCompleted = true

                if (flashIt is FlashIt.FlashModules && flashIt.currentIndex < flashIt.uris.size - 1) {
                    val nextFlashIt = flashIt.copy(
                        currentIndex = flashIt.currentIndex + 1
                    )
                    scope.launch {
                        kotlinx.coroutines.delay(500)
                        navigator.navigate(FlashScreenDestination(nextFlashIt))
                    }
                }
            }, onStdout = {
                tempText = "$it\n"
                if (tempText.startsWith("[H[J")) { // clear command
                    text = tempText.substring(6)
                } else {
                    text += tempText
                }
                logContent.append(it).append("\n")
            }, onStderr = {
                text += "$it\n"
                logContent.append(it).append("\n")
            })
        }
    }

    val onBack: () -> Unit = {
        val canGoBack = when (flashIt) {
            is FlashIt.FlashModuleUpdate -> currentFlashingStatus.value != FlashingStatus.FLASHING
            else -> currentFlashingStatus.value != FlashingStatus.FLASHING
        }

        if (canGoBack) {
            if (isExternalInstall) {
                (context as? ComponentActivity)?.finish()
            } else {
                if (flashIt is FlashIt.FlashModules || flashIt is FlashIt.FlashModuleUpdate) {
                    viewModel.markNeedRefresh()
                    viewModel.fetchModuleList()
                    navigator.navigate(ModuleScreenDestination)
                } else {
                    viewModel.markNeedRefresh()
                    viewModel.fetchModuleList()
                    navigator.popBackStack()
                }
            }
        }
    }

    BackHandler(enabled = true) {
        onBack()
    }

    Scaffold(
        topBar = {
            TopBar(
                currentFlashingStatus.value,
                currentStatus,
                onBack = onBack,
                onSave = {
                    scope.launch {
                        val format = SimpleDateFormat("yyyy-MM-dd-HH-mm-ss", Locale.getDefault())
                        val date = format.format(Date())
                        val file = File(
                            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
                            "KernelSU_install_log_${date}.log"
                        )
                        file.writeText(logContent.toString())
                        snackBarHost.showSnackbar(logSavedString.format(file.absolutePath))
                    }
                },
                scrollBehavior = scrollBehavior
            )
        },
        floatingActionButton = {
            if (showFloatAction) {
                ExtendedFloatingActionButton(
                    onClick = {
                        scope.launch {
                            withContext(Dispatchers.IO) {
                                reboot()
                            }
                        }
                    },
                    icon = {
                        YukiIcon(
                            Icons.Filled.Refresh,
                            contentDescription = stringResource(
                                id = R.string.reboot
                            )
                        )
                    },
                    text = {
                        Text(
                            text = stringResource(
                                id = R.string.reboot
                            )
                        )
                    },
                    shape = androidx.compose.foundation.shape.RoundedCornerShape(percent = 50),
                    containerColor = MaterialTheme.colorScheme.primary,
                    contentColor = MaterialTheme.colorScheme.onPrimary,
                    expanded = true
                )
            }
        },
        snackbarHost = { SnackbarHost(hostState = snackBarHost.hostState) },
        contentWindowInsets = WindowInsets.safeDrawing,
        containerColor = MaterialTheme.colorScheme.background
    ) { innerPadding ->
        KeyEventBlocker {
            it.key == Key.VolumeDown || it.key == Key.VolumeUp
        }

        Column(
            modifier = Modifier
                .fillMaxSize(1f)
                .padding(innerPadding)
                .nestedScroll(scrollBehavior.nestedScrollConnection),
        ) {
            when (flashIt) {
                is FlashIt.FlashModules -> {
                    ModuleInstallProgressBar(
                        currentIndex = flashIt.currentIndex + 1,
                        totalCount = flashIt.uris.size,
                        currentModuleName = currentStatus.currentModuleName,
                        status = currentFlashingStatus.value,
                        failedModules = currentStatus.failedModules
                    )
                    Spacer(modifier = Modifier.height(8.dp))
                }

                is FlashIt.FlashAk3 -> {
                    val ak3ProgressTitle = when (currentFlashingStatus.value) {
                        FlashingStatus.FLASHING ->
                            stringResource(R.string.partition_flashing, "AnyKernel3")
                        FlashingStatus.SUCCESS -> stringResource(R.string.flash_success)
                        FlashingStatus.FAILED -> stringResource(R.string.flash_failed)
                    }
                    FlashOperationProgressBar(
                        title = ak3ProgressTitle,
                        status = currentFlashingStatus.value,
                    )
                    Spacer(modifier = Modifier.height(8.dp))
                }

                else -> Unit
            }

            YukiPanel(
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f)
                    .padding(horizontal = 16.dp, vertical = 8.dp)
            ) {
                Box(Modifier.fillMaxSize().verticalScroll(scrollState)) {
                    LaunchedEffect(text) {
                        scrollState.animateScrollTo(scrollState.maxValue)
                    }
                    Text(
                        modifier = Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp, bottom = 88.dp),
                        text = text,
                        style = MaterialTheme.typography.bodyMedium,
                        fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurface
                    )
                }
            }
        }
    }
}

@Composable
private fun FlashOperationProgressBar(
    title: String,
    status: FlashingStatus,
) {
    FlashProgressSurface {
        Text(
            text = title,
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.Bold,
        )
        Spacer(modifier = Modifier.height(8.dp))
        FlashStatusProgressIndicator(status)
    }
}

// ????????????
@Composable
fun ModuleInstallProgressBar(
    currentIndex: Int,
    totalCount: Int,
    currentModuleName: String,
    status: FlashingStatus,
    failedModules: List<String>
) {
    FlashProgressSurface {
            // ???????
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text(
                    text = currentModuleName.ifEmpty { stringResource(R.string.module) },
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold
                )

                Text(
                    text = "$currentIndex/$totalCount",
                    style = MaterialTheme.typography.titleMedium
                )
            }

            Spacer(modifier = Modifier.height(8.dp))

            FlashStatusProgressIndicator(status)

            Spacer(modifier = Modifier.height(8.dp))

            // ??????
            AnimatedVisibility(
                visible = failedModules.isNotEmpty(),
                enter = fadeIn() + expandVertically(),
                exit = fadeOut() + shrinkVertically()
            ) {
                Column {
                    Row(
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        YukiIcon(
                            imageVector = Icons.Outlined.Warning,
                            contentDescription = null,
                            tint = MaterialTheme.colorScheme.error,
                            modifier = Modifier.size(16.dp)
                        )

                        Spacer(modifier = Modifier.width(4.dp))

                        Text(
                            text = stringResource(R.string.module_failed_count, failedModules.size),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.error
                        )
                    }

                    Spacer(modifier = Modifier.height(4.dp))

                    // ??????
                    Column(
                        modifier = Modifier
                            .fillMaxWidth()
                            .background(
                                MaterialTheme.colorScheme.errorContainer.copy(alpha = 0.3f),
                                shape = MaterialTheme.shapes.small
                            )
                            .padding(8.dp)
                    ) {
                        failedModules.forEach { moduleName ->
                            Text(
                                text = "? $moduleName",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onErrorContainer
                            )
                        }
                    }
                }
            }
    }
}

@Composable
private fun FlashStatusProgressIndicator(status: FlashingStatus) {
    val progressColor = when (status) {
        FlashingStatus.FLASHING -> MaterialTheme.colorScheme.primary
        FlashingStatus.SUCCESS -> MaterialTheme.colorScheme.tertiary
        FlashingStatus.FAILED -> MaterialTheme.colorScheme.error
    }

    if (isExpressiveUi) {
        if (status == FlashingStatus.FLASHING) {
            LinearWavyProgressIndicator(
                modifier = Modifier.fillMaxWidth(),
                color = progressColor,
                trackColor = MaterialTheme.colorScheme.surfaceVariant,
            )
        } else {
            LinearWavyProgressIndicator(
                progress = { 1f },
                modifier = Modifier.fillMaxWidth(),
                color = progressColor,
                trackColor = MaterialTheme.colorScheme.surfaceVariant,
            )
        }
    } else if (status == FlashingStatus.FLASHING) {
        LinearProgressIndicator(
            modifier = Modifier
                .fillMaxWidth()
                .height(8.dp),
            color = progressColor,
            trackColor = MaterialTheme.colorScheme.surfaceVariant,
        )
    } else {
        LinearProgressIndicator(
            progress = { 1f },
            modifier = Modifier
                .fillMaxWidth()
                .height(8.dp),
            color = progressColor,
            trackColor = MaterialTheme.colorScheme.surfaceVariant,
        )
    }
}

@Composable
private fun FlashProgressSurface(content: @Composable ColumnScope.() -> Unit) {
    if (isExpressiveUi) {
        YukiPanel(
            modifier = Modifier
                .fillMaxWidth()
                .padding(
                    horizontal = 16.dp,
                    vertical = 8.dp,
                )
        ) { Column(Modifier.padding(20.dp), content = content) }
    } else {
        Card(
            border = getCardBorder(),
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            colors = CardDefaults.cardColors(
                containerColor = MaterialTheme.colorScheme.surfaceVariant
            )
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(16.dp),
                content = content,
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TopBar(
    status: FlashingStatus,
    moduleStatus: ModuleInstallStatus = ModuleInstallStatus(),
    onBack: () -> Unit,
    onSave: () -> Unit = {},
    scrollBehavior: TopAppBarScrollBehavior? = null
) {
    val statusColor = when(status) {
        FlashingStatus.FLASHING -> MaterialTheme.colorScheme.primary
        FlashingStatus.SUCCESS -> MaterialTheme.colorScheme.tertiary
        FlashingStatus.FAILED -> MaterialTheme.colorScheme.error
    }

    val title: @Composable () -> Unit = {
        Column {
            YukiTopBarTitle(
                text = stringResource(
                    when (status) {
                        FlashingStatus.FLASHING -> R.string.flashing
                        FlashingStatus.SUCCESS -> R.string.flash_success
                        FlashingStatus.FAILED -> R.string.flash_failed
                    }
                ),
                color = statusColor
            )

            if (moduleStatus.failedModules.isNotEmpty()) {
                Text(
                    text = stringResource(
                        R.string.module_failed_count,
                        moduleStatus.failedModules.size,
                    ),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error
                )
            }
        }
    }
    val navigationIcon: @Composable () -> Unit = {
        IconButton(onClick = onBack) {
            YukiIcon(
                imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                contentDescription = stringResource(R.string.back),
            )
        }
    }
    val actions: @Composable RowScope.() -> Unit = {
        IconButton(onClick = onSave) {
            YukiIcon(
                imageVector = Icons.Filled.Save,
                contentDescription = stringResource(id = R.string.save_log),
            )
        }
    }
    YukiTopAppBar(
        title = title,
        navigationIcon = navigationIcon,
        actions = actions,
        scrollBehavior = scrollBehavior,
    )
}

suspend fun getModuleNameFromUri(context: Context, uri: Uri): String {
    return withContext(Dispatchers.IO) {
        try {
            if (uri == Uri.EMPTY) {
                return@withContext context.getString(R.string.unknown_module)
            }
            if (!ModuleUtils.isUriAccessible(context, uri)) {
                return@withContext context.getString(R.string.unknown_module)
            }
            ModuleUtils.extractModuleName(context, uri)
        } catch (_: Exception) {
            context.getString(R.string.unknown_module)
        }
    }
}

suspend fun getModuleIdFromUri(context: Context, uri: Uri): String? {
    return withContext(Dispatchers.IO) {
        try {
            if (uri == Uri.EMPTY) {
                return@withContext null
            }
            if (!ModuleUtils.isUriAccessible(context, uri)) {
                return@withContext null
            }
            ModuleUtils.extractModuleId(context, uri)
        } catch (_: Exception) {
            null
        }
    }
}

@Parcelize
sealed class FlashIt : Parcelable {
    data class FlashBoot(
        val boot: Uri? = null,
        val lkm: LkmSelection,
        val targetKmi: String,
        val ota: Boolean,
        val partition: String? = null,
        val blockModules: String = "",
        val allowShell: Boolean = false,
        val enableAdb: Boolean = false,
        val backup: Boolean = false,
        val superKey: String? = null,
        val signatureBypass: Boolean = false,
        val embedLkmInBoot: Boolean = false,
    ) : FlashIt()
    data class DownloadBoot(
        val url: String,
        val partition: String,
        val targetKmi: String,
        val lkm: LkmSelection,
        val blockModules: String = "",
        val allowShell: Boolean = false,
        val enableAdb: Boolean = false,
        val backup: Boolean = false,
        val superKey: String? = null,
        val signatureBypass: Boolean = false,
    ) : FlashIt()
    data class FlashModule(
        val uri: Uri,
        val repositorySourceId: String? = null,
        val repositoryModuleId: String? = null,
        val repositoryVersionCode: Long? = null,
        val repositoryVersionName: String? = null,
    ) : FlashIt()
    data class FlashModules(val uris: List<Uri>, val currentIndex: Int = 0) : FlashIt()
    data class FlashModuleUpdate(val uri: Uri) : FlashIt() // ????
    data class FlashAk3(
        val zipPath: String,
        val targetSlot: String?,
        val useMkbootfs: Boolean,
    ) : FlashIt()
    data object FlashRestore : FlashIt()
    data object FlashUninstall : FlashIt()
}

// ??????
fun flashModuleUpdate(
    uri: Uri,
    onFinish: (Boolean, Int) -> Unit,
    onStdout: (String) -> Unit,
    onStderr: (String) -> Unit
) {
    flashModule(uri, onFinish, onStdout, onStderr)
}

fun flashIt(
    flashIt: FlashIt,
    onFinish: (Boolean, Int) -> Unit,
    onStdout: (String) -> Unit,
    onStderr: (String) -> Unit
) {
    when (flashIt) {
        is FlashIt.FlashBoot -> installBoot(
            flashIt.boot,
            flashIt.lkm,
            flashIt.targetKmi,
            flashIt.ota,
            flashIt.partition,
            flashIt.allowShell,
            flashIt.enableAdb,
            flashIt.backup,
            flashIt.superKey,
            flashIt.signatureBypass,
            flashIt.embedLkmInBoot,
            onFinish,
            onStdout,
            onStderr,
            blockModules = flashIt.blockModules,
        )
        is FlashIt.DownloadBoot -> downloadBoot(
            flashIt.url,
            flashIt.partition,
            flashIt.targetKmi,
            flashIt.lkm,
            flashIt.allowShell,
            flashIt.enableAdb,
            flashIt.backup,
            flashIt.superKey,
            flashIt.signatureBypass,
            onFinish,
            onStdout,
            onStderr,
            blockModules = flashIt.blockModules,
        )
        is FlashIt.FlashModule -> flashModule(flashIt.uri, onFinish, onStdout, onStderr)
        is FlashIt.FlashModules -> {
            if (flashIt.uris.isEmpty() || flashIt.currentIndex >= flashIt.uris.size) {
                onFinish(false, 0)
                return
            }

            val currentUri = flashIt.uris[flashIt.currentIndex]
            onStdout("\n")

            flashModule(currentUri, onFinish, onStdout, onStderr)
        }
        is FlashIt.FlashModuleUpdate -> {
            onFinish(false, 0)
        }
        is FlashIt.FlashAk3 -> flashAnyKernel3(
            flashIt.zipPath,
            flashIt.targetSlot,
            flashIt.useMkbootfs,
            onFinish,
            onStdout,
            onStderr,
        )
        FlashIt.FlashRestore -> restoreBoot(onFinish, onStdout, onStderr)
        FlashIt.FlashUninstall -> uninstallPermanently(onFinish, onStdout, onStderr)
    }
}

@Preview
@Composable
fun FlashScreenPreview() {
    FlashScreen(EmptyDestinationsNavigator, FlashIt.FlashUninstall)
}
