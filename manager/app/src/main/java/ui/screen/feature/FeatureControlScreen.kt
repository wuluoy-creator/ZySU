package ui.screen.feature

import android.widget.Toast
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.res.vectorResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.zying.zysu.Natives
import com.zying.zysu.R
import com.zying.zysu.ui.component.KsuIsValid
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.component.YukiTopAppBar
import com.zying.zysu.ui.component.YukiTopBarTitle
import com.zying.zysu.ui.util.LocalSnackbarHost
import com.zying.zysu.ui.util.getFeatureStatus
import com.zying.zysu.ui.util.getFeatureValue
import com.zying.zysu.ui.util.restartAdbd
import com.zying.zysu.ui.util.setFeatureValue
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ui.screen.moreSettings.component.MoreSettingsItemPosition
import ui.screen.moreSettings.component.SettingsCard
import ui.screen.moreSettings.component.SwitchSettingItem

internal object FeatureControlState {
    var suLogEnabled by mutableStateOf<Boolean?>(null)
        private set

    fun refreshSuLog() {
        suLogEnabled = getFeatureValue(Natives.FEATURE_SULOG)
    }

    fun updateSuLog(enabled: Boolean) {
        suLogEnabled = enabled
    }
}

internal class FeatureToggleState(initialChecked: Boolean) {
    var checked by mutableStateOf(initialChecked)
    var saving by mutableStateOf(false)
}

@Composable
internal fun rememberFeatureToggleState(
    featureId: Int,
): FeatureToggleState = remember(featureId) {
    val enabled = getFeatureValue(featureId)
    FeatureToggleState(enabled)
}

private fun CoroutineScope.persistFeature(
    state: FeatureToggleState,
    featureId: Int,
    featureName: String,
    kernelEnabled: Boolean,
    relatedStates: List<FeatureToggleState> = emptyList(),
    optimistic: Boolean = false,
    afterPersist: suspend () -> Boolean = { true },
    afterPersistenceFailure: suspend () -> Unit = {},
    afterPostRollback: suspend () -> Unit = {},
    onRuntimeSettled: (Boolean) -> Unit = {},
    onSuccess: suspend () -> Unit = {},
    onFailure: suspend () -> Unit = {}
) {
    val savingStates = relatedStates + state
    if (savingStates.any { it.saving }) return

    val previousKernelEnabled = getFeatureValue(featureId)
    if (optimistic) {
        state.checked = kernelEnabled
    }
    savingStates.forEach { it.saving = true }
    launch(start = CoroutineStart.UNDISPATCHED) {
        // Persistence and required runtime follow-ups must survive leaving this screen.
        val success = withContext(NonCancellable) {
            var persisted = setFeatureValue(featureName, kernelEnabled)
            if (persisted && !afterPersist()) {
                setFeatureValue(featureName, previousKernelEnabled)
                afterPostRollback()
                persisted = false
            } else if (!persisted) {
                afterPersistenceFailure()
            }
            val current = getFeatureValue(featureId)
            onRuntimeSettled(current)
            state.checked = current
            savingStates.forEach { it.saving = false }
            persisted
        }
        if (success) onSuccess() else onFailure()
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun FeatureControlScreen(navigator: DestinationsNavigator) {
    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior()
    Scaffold(
        topBar = {
            FeatureControlTopBar(onBack = { navigator.popBackStack() }, scrollBehavior = scrollBehavior)
        },
        contentWindowInsets = WindowInsets.safeDrawing,
    ) { padding ->
        Box(Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding)) {
            FeatureControlContent()
        }
    }
}

@Composable
fun FeatureControlContent(
    onSavingChanged: (Boolean) -> Unit = {},
    scrollable: Boolean = true,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val snackbarHost = LocalSnackbarHost.current

    val selinuxHide = rememberFeatureToggleState(Natives.FEATURE_SELINUX_HIDE)
    var selinuxHideStatus by remember { mutableLongStateOf(-1L) }
    LaunchedEffect(selinuxHide.checked, selinuxHide.saving) {
        selinuxHideStatus = withContext(Dispatchers.IO) {
            Natives.getFeature(Natives.FEATURE_SELINUX_HIDE_STATUS)
        }
    }
    val selinuxHideRuntimeSummary = stringResource(
        when (selinuxHideStatus) {
            0L -> R.string.selinux_hide_runtime_disabled
            1L -> R.string.selinux_hide_runtime_active
            2L -> R.string.selinux_hide_runtime_pending
            3L -> R.string.selinux_hide_runtime_failed
            else -> R.string.selinux_hide_runtime_unknown
        }
    )
    val webViewZygoteUmount = rememberFeatureToggleState(
        Natives.FEATURE_WEBVIEW_ZYGOTE_UMOUNT
    )
    val adbRoot = rememberFeatureToggleState(Natives.FEATURE_ADB_ROOT)
    val shellSuAdbOnly = rememberFeatureToggleState(Natives.FEATURE_SHELL_SU_ADB_ONLY)
    val hideBootloader = rememberFeatureToggleState(Natives.FEATURE_HIDE_BOOTLOADER)
    val unicodePathGuard = rememberFeatureToggleState(Natives.FEATURE_UNICODE_PATH_GUARD)
    var unicodePathGuardRuntime by remember { mutableStateOf(UnicodePathGuardRuntime.Loading) }
    var unicodePathGuardRefresh by remember { mutableIntStateOf(0) }
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    DisposableEffect(lifecycle) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) unicodePathGuardRefresh++
        }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer) }
    }
    LaunchedEffect(unicodePathGuard.saving, unicodePathGuardRefresh) {
        if (unicodePathGuard.saving) return@LaunchedEffect
        val (value, runtime) = withContext(Dispatchers.IO) {
            val value = Natives.getFeature(Natives.FEATURE_UNICODE_PATH_GUARD)
            value to UnicodePathGuardRuntime.fromKernel(
                featureSupported = value >= 0,
                status = Natives.getFeature(Natives.FEATURE_UNICODE_PATH_GUARD_STATUS),
            )
        }
        unicodePathGuard.checked = value > 0
        unicodePathGuardRuntime = runtime
    }

    val saving = listOf(
        selinuxHide, webViewZygoteUmount,
        adbRoot, hideBootloader, shellSuAdbOnly,
        unicodePathGuard,
    ).any { it.saving }
    val savingCallback by rememberUpdatedState(onSavingChanged)
    SideEffect { savingCallback(saving) }
    DisposableEffect(Unit) {
        onDispose { savingCallback(false) }
    }

    val savedRebootMessage = stringResource(R.string.setting_change_saved_reboot)
    val failedMessage = stringResource(R.string.setting_change_failed)
    val unicodePathGuardSavedMessage = stringResource(R.string.unicode_path_guard_saved)

    Column(
        modifier = (if (scrollable) {
            Modifier.fillMaxSize().verticalScroll(rememberScrollState())
        } else {
            Modifier.fillMaxWidth()
        })
            .padding(horizontal = 18.dp)
            .padding(top = 8.dp)
    ) {
        KsuIsValid {
            SettingsCard {
                FeatureSwitchItem(
                    featureId = Natives.FEATURE_SELINUX_HIDE,
                    icon = Icons.Filled.Security,
                    title = stringResource(R.string.settings_selinux_hide),
                    summary = stringResource(R.string.settings_selinux_hide_summary),
                    runtimeSummary = selinuxHideRuntimeSummary,
                    state = selinuxHide,
                    groupPosition = MoreSettingsItemPosition.First,
                    onChange = { enabled ->
                        scope.persistFeature(
                            state = selinuxHide,
                            featureId = Natives.FEATURE_SELINUX_HIDE,
                            featureName = "selinux_hide",
                            kernelEnabled = enabled,
                            onSuccess = { snackbarHost.showSnackbar(savedRebootMessage) },
                            onFailure = { snackbarHost.showSnackbar(failedMessage) }
                        )
                    }
                )

                FeatureSwitchItem(
                    featureId = Natives.FEATURE_WEBVIEW_ZYGOTE_UMOUNT,
                    icon = Icons.Filled.Language,
                    title = stringResource(R.string.settings_webview_zygote_umount),
                    summary = stringResource(R.string.settings_webview_zygote_umount_summary),
                    state = webViewZygoteUmount,
                    onChange = { enabled ->
                        scope.persistFeature(
                            state = webViewZygoteUmount,
                            featureId = Natives.FEATURE_WEBVIEW_ZYGOTE_UMOUNT,
                            featureName = "webview_zygote_umount",
                            kernelEnabled = enabled,
                            onSuccess = { snackbarHost.showSnackbar(savedRebootMessage) },
                            onFailure = { snackbarHost.showSnackbar(failedMessage) }
                        )
                    }
                )

                FeatureSwitchItem(
                    featureId = Natives.FEATURE_SHELL_SU_ADB_ONLY,
                    icon = Icons.Filled.Security,
                    title = stringResource(R.string.settings_shell_su_adb_only),
                    summary = stringResource(R.string.settings_shell_su_adb_only_summary),
                    state = shellSuAdbOnly,
                    onChange = { checked ->
                        scope.persistFeature(
                            state = shellSuAdbOnly,
                            featureId = Natives.FEATURE_SHELL_SU_ADB_ONLY,
                            featureName = "shell_su_adb_only",
                            kernelEnabled = checked,
                            onFailure = { snackbarHost.showSnackbar(failedMessage) },
                        )
                    },
                )

                FeatureSwitchItem(
                    featureId = Natives.FEATURE_ADB_ROOT,
                    icon = Icons.Filled.DeveloperMode,
                    title = stringResource(R.string.settings_adb_root),
                    summary = stringResource(R.string.settings_adb_root_summary),
                    state = adbRoot,
                    groupPosition = MoreSettingsItemPosition.Last,
                    onChange = { enabled ->
                        scope.persistFeature(
                            state = adbRoot,
                            featureId = Natives.FEATURE_ADB_ROOT,
                            featureName = "adb_root",
                            kernelEnabled = enabled,
                            afterPersist = {
                                withContext(Dispatchers.IO) { restartAdbd() }
                            },
                            afterPostRollback = {
                                withContext(Dispatchers.IO) { restartAdbd() }
                            },
                            onFailure = {
                                snackbarHost.showSnackbar(failedMessage)
                            }
                        )
                    }
                )
            }

            SettingsCard {
                FeatureSwitchItem(
                    featureId = Natives.FEATURE_HIDE_BOOTLOADER,
                    icon = Icons.Filled.Lock,
                    title = stringResource(R.string.hide_bl_title),
                    summary = if (hideBootloader.checked) {
                        stringResource(R.string.hide_bl_enabled)
                    } else {
                        stringResource(R.string.hide_bl_disabled)
                    },
                    state = hideBootloader,
                    groupPosition = MoreSettingsItemPosition.Only,
                    onChange = { enabled ->
                        scope.persistFeature(
                            state = hideBootloader,
                            featureId = Natives.FEATURE_HIDE_BOOTLOADER,
                            featureName = "hide_bootloader",
                            kernelEnabled = enabled,
                            onSuccess = {
                                Toast.makeText(
                                    context,
                                    if (enabled) {
                                        R.string.hide_bl_enabled_toast
                                    } else {
                                        R.string.hide_bl_disabled_toast
                                    },
                                    Toast.LENGTH_SHORT
                                ).show()
                            },
                            onFailure = {
                                Toast.makeText(
                                    context,
                                    R.string.hide_bl_change_failed,
                                    Toast.LENGTH_SHORT
                                ).show()
                            }
                        )
                    }
                )
            }

            UnicodePathGuardCard(
                state = unicodePathGuard,
                runtime = unicodePathGuardRuntime,
                enabled = !saving,
                onChange = { enabled ->
                    scope.persistFeature(
                        state = unicodePathGuard,
                        featureId = Natives.FEATURE_UNICODE_PATH_GUARD,
                        featureName = "unicode_path_guard",
                        kernelEnabled = enabled,
                        afterPersist = {
                            withContext(Dispatchers.IO) {
                                val runtime = UnicodePathGuardRuntime.fromKernel(
                                    Natives.isFeatureSupported(Natives.FEATURE_UNICODE_PATH_GUARD),
                                    Natives.getFeature(Natives.FEATURE_UNICODE_PATH_GUARD_STATUS),
                                )
                                val expected = if (enabled) UnicodePathGuardRuntime.Active
                                    else UnicodePathGuardRuntime.Disabled
                                runtime == expected
                            }
                        },
                        onRuntimeSettled = { unicodePathGuardRefresh++ },
                        onSuccess = { snackbarHost.showSnackbar(unicodePathGuardSavedMessage) },
                        onFailure = { snackbarHost.showSnackbar(failedMessage) },
                    )
                },
            )
        }

        if (scrollable) Spacer(modifier = Modifier.height(8.dp))
    }

}

@Composable
private fun UnicodePathGuardCard(
    state: FeatureToggleState,
    runtime: UnicodePathGuardRuntime,
    enabled: Boolean,
    onChange: (Boolean) -> Unit,
) {
    val forced = remember { Natives.isFeatureForced(Natives.FEATURE_UNICODE_PATH_GUARD) }
    val runtimeSummary = stringResource(
        if (state.saving) R.string.unicode_path_guard_saving else when (runtime) {
            UnicodePathGuardRuntime.Loading -> R.string.unicode_path_guard_loading
            UnicodePathGuardRuntime.Disabled -> R.string.unicode_path_guard_disabled
            UnicodePathGuardRuntime.Active -> R.string.unicode_path_guard_active
            UnicodePathGuardRuntime.Unsupported -> R.string.unicode_path_guard_unsupported
            UnicodePathGuardRuntime.Unknown -> R.string.unicode_path_guard_unknown
        }
    )
    SettingsCard(title = stringResource(R.string.unicode_path_guard_cve)) {
        SwitchSettingItem(
            icon = Icons.Filled.Security,
            title = stringResource(R.string.unicode_path_guard_title),
            summary = if (forced) {
                stringResource(R.string.feature_status_forced_summary) + "\n" + runtimeSummary
            } else runtimeSummary,
            checked = state.checked,
            enabled = enabled && !forced && runtime.configurable && !state.saving,
            groupPosition = MoreSettingsItemPosition.Only,
            onChange = onChange,
        )
        if (state.saving) LinearProgressIndicator(Modifier.fillMaxWidth())
        Column(
            Modifier.padding(horizontal = 16.dp).padding(bottom = 12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(
                stringResource(R.string.unicode_path_guard_scope),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                stringResource(R.string.unicode_path_guard_notice),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

/** Native isolation controls embedded beside the shared SUMH settings. */
@Composable
internal fun IsolationKernelSettings(
    enabled: Boolean = true,
    onSavingChanged: (Boolean) -> Unit = {},
) {
    val scope = rememberCoroutineScope()
    val snackbarHost = LocalSnackbarHost.current
    val unshareMnt = rememberFeatureToggleState(Natives.FEATURE_UNSHARE_MNT)
    val ksmSupported = remember { getFeatureStatus(Natives.FEATURE_SUMH_SUCOMPAT) == "supported" }
    var suPathSaving by remember { mutableStateOf(false) }
    val saving = unshareMnt.saving || suPathSaving
    val savingCallback by rememberUpdatedState(onSavingChanged)
    SideEffect { savingCallback(saving) }
    DisposableEffect(Unit) { onDispose { savingCallback(false) } }
    val failedMessage = stringResource(R.string.setting_change_failed)

    FeatureSwitchItem(
        featureId = Natives.FEATURE_UNSHARE_MNT,
        icon = ImageVector.vectorResource(R.drawable.ic_mount_view_cleanup),
        title = stringResource(R.string.settings_unshare_mnt),
        summary = stringResource(R.string.settings_unshare_mnt_summary),
        state = unshareMnt,
        groupPosition = if (ksmSupported) MoreSettingsItemPosition.First else MoreSettingsItemPosition.Only,
        enabled = enabled && !saving,
        onChange = { checked ->
            if (enabled && !unshareMnt.saving && !suPathSaving) {
                scope.persistFeature(
                    state = unshareMnt,
                    featureId = Natives.FEATURE_UNSHARE_MNT,
                    featureName = "unshare_mnt",
                    kernelEnabled = checked,
                    onFailure = { snackbarHost.showSnackbar(failedMessage) },
                )
            }
        },
    )
    if (ksmSupported) {
        SuPathSetting(
            enabled = enabled && !saving,
            onSavingChange = { suPathSaving = it },
        )
    }
}

@Composable
internal fun SuperuserLogSetting(
    enabled: Boolean = true,
    groupPosition: MoreSettingsItemPosition = MoreSettingsItemPosition.Last,
    onSavingChanged: (Boolean) -> Unit = {},
) {
    val scope = rememberCoroutineScope()
    val suLog = rememberFeatureToggleState(Natives.FEATURE_SULOG)
    val savingCallback by rememberUpdatedState(onSavingChanged)
    SideEffect { savingCallback(suLog.saving) }
    DisposableEffect(Unit) { onDispose { savingCallback(false) } }

    FeatureSwitchItem(
        featureId = Natives.FEATURE_SULOG,
        icon = Icons.Filled.Visibility,
        title = stringResource(R.string.settings_disable_sulog),
        summary = stringResource(R.string.settings_disable_sulog_summary),
        state = suLog,
        groupPosition = groupPosition,
        enabled = enabled,
        onChange = { checked ->
            if (enabled && !suLog.saving) {
                scope.persistFeature(
                    state = suLog,
                    featureId = Natives.FEATURE_SULOG,
                    featureName = "sulog",
                    kernelEnabled = checked,
                    onRuntimeSettled = FeatureControlState::updateSuLog,
                )
            }
        },
    )
}

@Composable
private fun FeatureSwitchItem(
    featureId: Int,
    icon: ImageVector,
    title: String,
    summary: String,
    state: FeatureToggleState,
    groupPosition: MoreSettingsItemPosition = MoreSettingsItemPosition.Middle,
    enabled: Boolean = true,
    runtimeSummary: String? = null,
    onChange: (Boolean) -> Unit
) {
    val status = remember(featureId) { getFeatureStatus(featureId) }
    val baseSummary = when (status) {
        "unsupported" -> stringResource(R.string.feature_status_unsupported_summary)
        "managed" -> stringResource(R.string.feature_status_managed_summary)
        "forced" -> stringResource(R.string.feature_status_forced_summary)
        else -> summary
    }
    val renderedSummary = if (runtimeSummary == null) baseSummary else "$baseSummary\n$runtimeSummary"

    SwitchSettingItem(
        icon = icon,
        title = title,
        summary = renderedSummary,
        checked = state.checked,
        enabled = enabled && status == "supported" && !state.saving,
        groupPosition = groupPosition,
        onChange = onChange
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun FeatureControlTopBar(
    onBack: () -> Unit,
    scrollBehavior: TopAppBarScrollBehavior
) {
    val title: @Composable () -> Unit = {
        YukiTopBarTitle(text = stringResource(R.string.feature_control))
    }
    val navigationIcon: @Composable () -> Unit = {
        IconButton(onClick = onBack) {
            YukiIcon(
                imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                contentDescription = stringResource(R.string.back)
            )
        }
    }
    YukiTopAppBar(
        title = title,
        navigationIcon = navigationIcon,
        scrollBehavior = scrollBehavior,
    )
}
