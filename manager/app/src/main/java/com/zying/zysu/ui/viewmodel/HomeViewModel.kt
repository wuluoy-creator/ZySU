package com.zying.zysu.ui.viewmodel

import android.content.Context
import android.content.SharedPreferences
import android.os.Build
import android.system.Os
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.zying.zysu.BuildConfig
import com.zying.zysu.KernelVersion
import com.zying.zysu.Natives
import com.zying.zysu.getKernelVersion
import com.zying.zysu.ksuApp
import com.zying.zysu.integrity.KsudIntegrity
import com.zying.zysu.integrity.KsudIntegrityStatus
import com.zying.zysu.ui.util.*
import com.zying.zysu.ui.screen.home.HomeLayoutStyle
import com.zying.zysu.ui.screen.home.HomeFactField
import com.zying.zysu.ui.screen.home.HomeFactVisibility
import com.zying.zysu.ui.util.module.LatestVersionInfo
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.io.File
import org.json.JSONArray

class HomeViewModel : ViewModel() {

    data class KsudCardState(
        val bundledVersion: String? = null,
        val installedVersion: String? = null,
        val hasBundledDaemon: Boolean = false,
        val loading: Boolean = true,
        val syncing: Boolean = false,
        val syncResult: Boolean? = null,
    )

    var ksudCardState by mutableStateOf(KsudCardState())
        private set
    private var ksudRefreshJob: Job? = null

    private suspend fun readKsudCard(): KsudCardState = withContext(Dispatchers.IO) {
        val (bundled, installed) = KsuCli.getKsudVersionsForUi()
        KsudCardState(
            bundledVersion = bundled,
            installedVersion = installed,
            hasBundledDaemon = File(ksuApp.applicationInfo.nativeLibraryDir, BuildConfig.KSUD_LIBRARY_NAME).isFile,
            loading = false,
        )
    }

    fun refreshKsudCard() {
        if (ksudCardState.syncing) return
        ksudRefreshJob?.cancel()
        ksudRefreshJob = viewModelScope.launch {
            try {
                val refreshed = readKsudCard()
                ksudCardState = refreshed
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                Log.e("HomeViewModel", "Unable to read ksud card", error)
                ksudCardState = ksudCardState.copy(loading = false, installedVersion = null, syncResult = null)
            }
        }
    }

    fun syncKsudCard() {
        val status = systemStatus
        if (ksudCardState.loading || ksudCardState.syncing || !ksudCardState.hasBundledDaemon ||
            !status.isManager || status.kernelUapiVersion <= 0 ||
            status.kernelUapiVersion != status.managerUapiVersion
        ) return
        ksudRefreshJob?.cancel()
        ksudCardState = ksudCardState.copy(syncing = true, syncResult = null)
        // Retain progress across navigation/rotation; finish the atomic install and required
        // shell refresh + integrity readback even if this destination's ViewModel is cleared.
        viewModelScope.launch(start = CoroutineStart.UNDISPATCHED) {
            withContext(NonCancellable) {
                var updated = false
                var verified = false
                try {
                    updated = KsuCli.updateKsudDaemonForUi()
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Exception) {
                    Log.e("HomeViewModel", "Unable to sync ksud", error)
                } finally {
                    try {
                        val integrityMatches = KsudIntegrity.refresh(ksuApp, notifyOnMismatch = false) == KsudIntegrityStatus.MATCH
                        val refreshed = readKsudCard()
                        ksudCardState = refreshed.copy(syncing = true)
                        verified = integrityMatches && refreshed.installedVersion != null
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (error: Exception) {
                        Log.e("HomeViewModel", "Unable to verify ksud after sync", error)
                    } finally {
                        ksudCardState = ksudCardState.copy(loading = false, syncing = false, syncResult = updated && verified)
                    }
                }
            }
        }
    }

    data class SystemStatus(
        val isManager: Boolean = false,
        val ksuVersion: Int? = null,
        val ksuFullVersion : String? = null,
        val kernelVersion: KernelVersion = getKernelVersion(),
        val isRootAvailable: Boolean = false,
        val isLkmBundled: Boolean = false,
        val kernelUapiVersion: Int = 0,
        val managerUapiVersion: Int = 0,
        val managerVersionCode: Long = BuildConfig.VERSION_CODE.toLong(),
        val safeMode: Boolean = false,
    ) {
        val requireNewKernel: Boolean
            get() = isManager && kernelUapiVersion < managerUapiVersion
        val requireNewManager: Boolean
            get() = isManager && kernelUapiVersion > managerUapiVersion
        val showLkmUpdate: Boolean
            get() = isManager && isLkmBundled && ksuVersion != null &&
                ksuVersion.toLong() != managerVersionCode &&
                kernelUapiVersion == managerUapiVersion
        val showCustomLkmBadge: Boolean
            get() = ksuVersion != null && !isLkmBundled
    }

    data class SystemInfo(
        val kernelRelease: String = "",
        val androidVersion: String = "",
        val deviceModel: String = "",
        val managerVersion: Pair<String, Long> = Pair("", 0L),
        val seLinuxStatus: String = "",
        val superuserCount: Int? = null,
        val moduleCount: Int? = null,
        val zygiskImplement: String = "",
        val metaModuleImplement: String = "",
        // Manager process seccomp mode: -1 unsupported, 0 disabled, 1 strict,
        // 2 filter (mirrors upstream's prctl(PR_GET_SECCOMP)).
        val seccompStatus: Int = -1,
        val workingMode: String? = null,
        val architecture: String = "",
        val securityPatch: String = "",
        val buildId: String = "",
        val fingerprint: String = ""
    )

    var systemStatus by mutableStateOf(SystemStatus())
        private set

    var systemInfo by mutableStateOf(SystemInfo())
        private set

    var latestVersionInfo by mutableStateOf(LatestVersionInfo())
        private set

    var isSimpleMode by mutableStateOf(false)
        private set
    var isKernelSimpleMode by mutableStateOf(false)
        private set
    var isHideVersion by mutableStateOf(false)
        private set
    var isHideOtherInfo by mutableStateOf(false)
        private set
    var isHideZygiskImplement by mutableStateOf(false)
        private set
    var isHideSeccompStatus by mutableStateOf(false)
        private set
    var isHideMetaModuleImplement by mutableStateOf(false)
        private set
    var isHideLinkCard by mutableStateOf(false)
        private set


    var showDeviceDetails by mutableStateOf(false)
        private set
    var homeLayout by mutableStateOf(HomeLayoutStyle.Circle)
        private set
    internal var visibleHomeFactIds by mutableStateOf<Set<String>>(HomeFactField.entries.mapTo(linkedSetOf()) { it.id })
        private set
    var informationLoadFailed by mutableStateOf(false)
        private set
    private var settingsPreferences: SharedPreferences? = null
    private var settingsListener: SharedPreferences.OnSharedPreferenceChangeListener? = null
    private var refreshJob: Job? = null

    var isCoreDataLoaded by mutableStateOf(false)
        private set
    var isExtendedDataLoaded by mutableStateOf(false)
        private set
    var isRefreshing by mutableStateOf(false)
        private set

    private val _dataRefreshTrigger = MutableStateFlow(0L)
    val dataRefreshTrigger: StateFlow<Long> = _dataRefreshTrigger

    private var loadingJobs = mutableListOf<Job>()
    private var lastRefreshTime = 0L
    private val refreshCooldown = 2000L

    fun loadUserSettings(context: Context) {
        val appContext = context.applicationContext
        val prefs = appContext.getSharedPreferences("settings", Context.MODE_PRIVATE)
        fun read() {
            isSimpleMode = prefs.getBoolean("is_simple_mode", false)
            isKernelSimpleMode = prefs.getBoolean("is_kernel_simple_mode", false)
            isHideVersion = prefs.getBoolean("is_hide_version", false)
            isHideOtherInfo = prefs.getBoolean("is_hide_other_info", false)
            isHideLinkCard = prefs.getBoolean("is_hide_link_card", false)
            isHideZygiskImplement = prefs.getBoolean("is_hide_zygisk_Implement", false)
            isHideSeccompStatus = prefs.getBoolean("is_hide_seccomp_status", false)
            isHideMetaModuleImplement = prefs.getBoolean("is_hide_meta_module_Implement", false)
            showDeviceDetails = prefs.getBoolean("home_show_device_details", false)
            homeLayout = HomeLayoutStyle.fromValue(prefs.getString("home_layout", null))
            visibleHomeFactIds = HomeFactVisibility.read(prefs)
        }
        read()
        if (settingsPreferences === prefs) return
        settingsListener?.let { settingsPreferences?.unregisterOnSharedPreferenceChangeListener(it) }
        val listener = SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
            read()
            if (key == "is_kernel_simple_mode") refreshData(appContext, forceRefresh = true)
        }
        prefs.registerOnSharedPreferenceChangeListener(listener)
        settingsPreferences = prefs
        settingsListener = listener
    }

    fun loadCoreData(): Job? {
        if (isCoreDataLoaded) return null

        val job = viewModelScope.launch(Dispatchers.IO) {
            try {
                val kernelVersion = getKernelVersion()
                val isManager = try {
                    Natives.isManager
                } catch (_: Exception) {
                    false
                }

                val ksuVersion = if (isManager) Natives.version else null

                val fullVersion = try {
                    Natives.getFullVersion()
                } catch (_: Exception) {
                    "Unknown"
                }

                val ksuFullVersion = if (isKernelSimpleMode) {
                    try {
                        val startIndex = fullVersion.indexOf('v')
                        if (startIndex >= 0) {
                            val endIndex = fullVersion.indexOf('-', startIndex)
                            val versionStr = if (endIndex > startIndex) {
                                fullVersion.substring(startIndex, endIndex)
                            } else {
                                fullVersion.substring(startIndex)
                            }
                            val numericVersion = "v" + (Regex("""\d+(\.\d+)*""").find(versionStr)?.value ?: versionStr)
                            numericVersion
                        } else {
                            fullVersion
                        }
                    } catch (_: Exception) {
                        fullVersion
                    }
                } else {
                    fullVersion
                }

                val isRootAvailable = try {
                    rootAvailable()
                } catch (_: Exception) {
                    false
                }

                val isLkmBundled = isManager && runCatching { Natives.isLkmBundled }.getOrDefault(false)

                val kernelUapiVersion = if (isManager) {
                    try { Natives.getUapiVersion() } catch (_: Exception) { 0 }
                } else 0

                val managerUapiVersion = try {
                    Natives.getManagerUapiVersion()
                } catch (_: Exception) { 0 }

                systemStatus = SystemStatus(
                    isManager = isManager,
                    ksuVersion = ksuVersion,
                    ksuFullVersion = ksuFullVersion,
                    kernelVersion = kernelVersion,
                    isRootAvailable = isRootAvailable,
                    isLkmBundled = isLkmBundled,
                    kernelUapiVersion = kernelUapiVersion,
                    managerUapiVersion = managerUapiVersion,
                    safeMode = isManager && runCatching { Natives.isSafeMode }.getOrDefault(false),
                )

                isCoreDataLoaded = true
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                Log.e("HomeViewModel", "Unable to load system status", error)
                informationLoadFailed = true
                isCoreDataLoaded = true
            }
        }
        loadingJobs.add(job)
        return job
    }

    fun loadExtendedData(context: Context): Job? {
        if (isExtendedDataLoaded) return null

        val job = viewModelScope.launch(Dispatchers.IO) {
            try {
                delay(50)

                val basicInfo = loadBasicSystemInfo(context)
                systemInfo = systemInfo.copy(
                    kernelRelease = basicInfo.first,
                    androidVersion = basicInfo.second,
                    deviceModel = basicInfo.third,
                    managerVersion = basicInfo.fourth,
                    seLinuxStatus = basicInfo.fifth,
                    seccompStatus = readSeccompStatus(),
                    workingMode = if (systemStatus.isManager) readWorkingMode() else null,
                    architecture = Build.SUPPORTED_ABIS.joinToString(", "),
                    securityPatch = Build.VERSION.SECURITY_PATCH.orEmpty(),
                    buildId = Build.DISPLAY.orEmpty(),
                    fingerprint = Build.FINGERPRINT.orEmpty(),
                )

                delay(100)

                if (systemStatus.isManager) {
                    val moduleInfo = loadModuleInfo()
                    systemInfo = systemInfo.copy(
                        superuserCount = moduleInfo.first,
                        moduleCount = moduleInfo.second,
                        zygiskImplement = moduleInfo.third,
                        metaModuleImplement = moduleInfo.fourth
                    )
                } else {
                    systemInfo = systemInfo.copy(superuserCount = null, moduleCount = null,
                        zygiskImplement = "", metaModuleImplement = "")
                }

                delay(100)

                isExtendedDataLoaded = true
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                Log.e("HomeViewModel", "Unable to load device information", error)
                informationLoadFailed = true
                isExtendedDataLoaded = true
            }
        }
        loadingJobs.add(job)
        return job
    }

    fun refreshData(context: Context, forceRefresh: Boolean = false) {
        val currentTime = System.currentTimeMillis()

        if (!forceRefresh && currentTime - lastRefreshTime < refreshCooldown) {
            return
        }

        lastRefreshTime = currentTime

        val previousRefresh = refreshJob
        refreshJob = viewModelScope.launch {
            previousRefresh?.cancelAndJoin()
            isRefreshing = true
            informationLoadFailed = false

            try {
                loadingJobs.forEach { it.cancelAndJoin() }
                loadingJobs.clear()

                isCoreDataLoaded = false
                isExtendedDataLoaded = false

                _dataRefreshTrigger.value = currentTime

                loadUserSettings(context)

                loadCoreData()?.join()
                loadExtendedData(context)?.join()

                val settingsPrefs = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
                val checkUpdate = settingsPrefs.getBoolean("check_update", true)
                if (checkUpdate) {
                    try {
                        val newVersionInfo = withContext(Dispatchers.IO) {
                            checkNewVersion()
                        }
                        latestVersionInfo = newVersionInfo
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (_: Exception) {
                        // An update-server failure does not invalidate local device information.
                    }
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                Log.e("HomeViewModel", "Unable to refresh home", error)
                informationLoadFailed = true
            } finally {
                isRefreshing = false
            }
        }
    }

    fun onPullRefresh(context: Context) {
        refreshData(context, forceRefresh = true)
    }

    fun autoRefreshIfNeeded(context: Context, forceRefresh: Boolean = false) {
        viewModelScope.launch {
            val needsRefresh = checkIfDataNeedsRefresh()
            if (needsRefresh) {
                refreshData(context, forceRefresh)
            }
        }
    }

    private suspend fun checkIfDataNeedsRefresh(): Boolean {
        return withContext(Dispatchers.IO) {
            try {
                val currentKsuVersion = try {
                    if (Natives.isManager) {
                        Natives.version
                    } else null
                } catch (_: Exception) {
                    null
                }

                if (currentKsuVersion != systemStatus.ksuVersion) {
                    return@withContext true
                }

                if (!systemStatus.isManager) return@withContext false
                if (homeCountChanged(systemInfo.superuserCount, readSuperuserCount())) {
                    return@withContext true
                }
                // A failed module query must not look like an empty installation or
                // continuously trigger a refresh while root/the daemon is unavailable.
                homeCountChanged(systemInfo.moduleCount, readModuleCount())
            } catch (_: Exception) {
                false
            }
        }
    }

    // Reads the manager process seccomp mode from /proc/self/status.
    // -1 = unsupported (no Seccomp line), 0 = disabled, 1 = strict, 2 = filter.
    private fun readSeccompStatus(): Int = runCatching {
        java.io.File("/proc/self/status").useLines { lines ->
            lines.firstOrNull { it.startsWith("Seccomp:") }
                ?.substringAfter(':')?.trim()?.toIntOrNull() ?: -1
        }
    }.getOrDefault(-1)

    private fun readWorkingMode(): String? = runCatching {
        val hook = Natives.getHookType().takeIf(String::isNotBlank)
        val mode = when (Natives.getLoadMode()) {
            Natives.LOAD_MODE_RAMDISK -> "Ramdisk"
            Natives.LOAD_MODE_IMAGE_PATCH -> "ImgPatch"
            Natives.LOAD_MODE_LATE -> "Lateload"
            else -> null
        }
        listOfNotNull(hook, mode).takeIf { it.isNotEmpty() }?.joinToString(" · ")
    }.getOrNull()

    private suspend fun loadBasicSystemInfo(
        context: Context
    ): Tuple5<String, String, String, Pair<String, Long>, String> {
        return withContext(Dispatchers.IO) {
            val uname = try {
                Os.uname()
            } catch (_: Exception) {
                null
            }

            val deviceModel = try {
                resolveDeviceName()
            } catch (_: Exception) {
                "Unknown"
            }

            val managerVersion = try {
                getManagerVersion(context)
            } catch (_: Exception) {
                Pair("Unknown", 0L)
            }

            val seLinuxStatus = try {
                getSELinuxStatus(ksuApp.applicationContext)
            } catch (_: Exception) {
                "Unknown"
            }

            Tuple5(
                uname?.release ?: "Unknown",
                Build.VERSION.RELEASE ?: "Unknown",
                deviceModel,
                managerVersion,
                seLinuxStatus
            )
        }
    }

    private suspend fun loadModuleInfo(): Tuple4<Int?, Int?, String, String> {
        return withContext(Dispatchers.IO) {
            val superuserCount = readSuperuserCount()

            val moduleCount = readModuleCount()

            val zygiskImplement = try {
                getZygiskImplement()
            } catch (_: Exception) {
                "None"
            }

            val metaModuleImplement = try {
                getMetaModuleImplement()
            } catch (_: Exception) {
                "None"
            }

            Tuple4(superuserCount, moduleCount, zygiskImplement, metaModuleImplement)
        }
    }

    private fun readModuleCount(): Int? = try {
        val result = getRootShell().newJob().add(ksudCmd("module list"))
            .to(ArrayList<String>(), null).exec()
        if (result.isSuccess) JSONArray(result.out.joinToString("\n")).length() else null
    } catch (_: Exception) {
        null
    }

    private fun readSuperuserCount(): Int? = runCatching {
        getSuperuserCount().takeIf { it >= 0 }
    }.getOrNull()

    private fun getManagerVersion(context: Context): Pair<String, Long> {
        return try {
            val packageInfo = context.packageManager.getPackageInfo(context.packageName, 0)
            val versionCode = androidx.core.content.pm.PackageInfoCompat.getLongVersionCode(packageInfo)
            val versionName = packageInfo.versionName ?: "Unknown"
            Pair(versionName, versionCode)
        } catch (_: Exception) {
            Pair("Unknown", 0L)
        }
    }

    data class Tuple5<T1, T2, T3, T4, T5>(
        val first: T1,
        val second: T2,
        val third: T3,
        val fourth: T4,
        val fifth: T5
    )

    data class Tuple4<T1, T2, T3, T4>(
        val first: T1,
        val second: T2,
        val third: T3,
        val fourth: T4
    )

    override fun onCleared() {
        settingsListener?.let { settingsPreferences?.unregisterOnSharedPreferenceChangeListener(it) }
        settingsListener = null
        settingsPreferences = null
        loadingJobs.forEach { it.cancel() }
        loadingJobs.clear()
    }
}

/** A failed read is not a change to zero; a real zero still observes the final revocation. */
internal fun homeCountChanged(previous: Int?, current: Int?): Boolean =
    current != null && current >= 0 && current != previous
