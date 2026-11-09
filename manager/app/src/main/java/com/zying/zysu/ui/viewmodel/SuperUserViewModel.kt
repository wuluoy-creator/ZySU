package com.zying.zysu.ui.viewmodel

import android.content.*
import android.content.pm.ApplicationInfo
import android.content.pm.PackageInfo
import android.graphics.drawable.Drawable
import android.os.IBinder
import android.os.Parcelable
import android.util.Log
import androidx.compose.runtime.*
import androidx.core.content.edit
import androidx.core.content.ContextCompat
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.zying.zysu.Natives
import com.zying.zysu.ksuApp
import com.zying.zysu.ui.KsuService
import com.zying.zysu.ui.util.*
import com.zying.zysu.ui.util.module.AllowlistBackup
import com.zying.zysu.ui.util.module.AllowlistRestore
import com.topjohnwu.superuser.Shell
import kotlinx.coroutines.*
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit
import kotlin.coroutines.resume
import com.zying.zysu.IKsuInterface
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.parcelize.IgnoredOnParcel
import kotlinx.parcelize.Parcelize

enum class SortType(val displayNameRes: Int, val persistKey: String) {
    NAME_ASC(com.zying.zysu.R.string.sort_name_asc, "NAME_ASC"),
    NAME_DESC(com.zying.zysu.R.string.sort_name_desc, "NAME_DESC"),
    INSTALL_TIME_NEW(com.zying.zysu.R.string.sort_install_time_new, "INSTALL_TIME_NEW"),
    INSTALL_TIME_OLD(com.zying.zysu.R.string.sort_install_time_old, "INSTALL_TIME_OLD");

    companion object {
        fun fromPersistKey(key: String): SortType = entries.find { it.persistKey == key } ?: NAME_ASC
    }
}

class SuperUserViewModel : ViewModel() {
    companion object {
        private const val TAG = "SuperUserViewModel"
        const val SHELL_UID = 2000
        private val appsLock = Any()
        private val webUiLoader by lazy { SuperUserViewModel() }

        suspend fun loadAppsForWebUI(): List<AppInfo> {
            webUiLoader.fetchAppList(useCache = true)
            return apps
        }

        var apps by mutableStateOf<List<AppInfo>>(emptyList())
        private val _isAppListLoaded = MutableStateFlow(false)
        val isAppListLoaded = _isAppListLoaded.asStateFlow()

        @JvmStatic
        fun getAppIconDrawable(context: Context, packageName: String): Drawable? {
            val appList = synchronized(appsLock) { apps }
            return appList.find { it.packageName == packageName }
                ?.packageInfo?.applicationInfo?.loadIcon(context.packageManager)
        }

        var appGroups by mutableStateOf<List<AppGroup>>(emptyList())

        private const val PREFS_NAME = "settings"
        private const val KEY_SHOW_SYSTEM_APPS = "show_system_apps"
        private const val KEY_CURRENT_SORT_TYPE = "current_sort_type"
        private const val CORE_POOL_SIZE = 8
        private const val MAX_POOL_SIZE = 16
        private const val KEEP_ALIVE_TIME = 60L
        private const val BATCH_SIZE = 20
        private const val PER_USER_RANGE = 100000
        private val fetchAppListMutex = Mutex()
        private val permissionMutex = Mutex()
        private val appListMutex = Mutex()
        private val allowlistBackupMutex = Mutex()
        private var sharedHasLoaded by mutableStateOf(false)
        private var sharedLoadError by mutableStateOf<String?>(null)
        private var sharedIsRefreshing by mutableStateOf(false)
        private var sharedIsUpdatingPermissions by mutableStateOf(false)
        private val ksuServiceLock = Any()
        private val ksuServiceConnectMutex = Mutex()
        private val ksuServiceScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
        // Keep one process-wide binding so ordinary reads reuse the service package snapshot.
        private var ksuServiceBinder: IBinder? = null
        private var ksuServiceConnection: ServiceConnection? = null
        private var ksuServiceStartTask: Shell.Task? = null
        private var ksuServiceStartRunning = false

        private fun unbindKsuService(connection: ServiceConnection) {
            try {
                com.topjohnwu.superuser.ipc.RootService.unbind(connection)
            } catch (error: Exception) {
                Log.w(TAG, "Failed to unbind KsuService", error)
            }
        }

        private fun clearKsuServiceConnection(connection: ServiceConnection) {
            synchronized(ksuServiceLock) {
                if (ksuServiceConnection === connection) {
                    ksuServiceBinder = null
                    ksuServiceConnection = null
                }
            }
        }

        private class KsuServiceConnection(
            private var continuation: CancellableContinuation<IBinder?>?
        ) : ServiceConnection {
            override fun onServiceDisconnected(name: ComponentName?) {
                fail()
            }

            override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
                if (binder == null) {
                    fail()
                    return
                } else {
                    val current = synchronized(ksuServiceLock) {
                        if (ksuServiceConnection === this) {
                            ksuServiceBinder = binder
                            ksuServiceStartTask = null
                            true
                        } else false
                    }
                    if (!current) {
                        unbindKsuService(this)
                        return
                    }
                }
                resume(binder)
            }

            override fun onNullBinding(name: ComponentName?) {
                fail()
            }

            override fun onBindingDied(name: ComponentName?) {
                fail()
            }

            fun fail() {
                clearKsuServiceConnection(this)
                unbindKsuService(this)
                resume(null)
            }

            fun cancel() {
                synchronized(this) { continuation = null }
                clearKsuServiceConnection(this)
                unbindKsuService(this)
            }

            private fun resume(binder: IBinder?) {
                val pending = synchronized(this) {
                    continuation.also { continuation = null }
                } ?: return
                pending.resume(binder)
            }
        }
    }

    @Immutable
    @Parcelize
    data class AppInfo(
        val label: String,
        val packageInfo: PackageInfo,
        val profile: Natives.Profile?,
    ) : Parcelable {
        @IgnoredOnParcel
        val packageName: String = packageInfo.packageName
        @IgnoredOnParcel
        val uid: Int = packageInfo.applicationInfo!!.uid
        @IgnoredOnParcel
        val profileKey: String = profile?.name?.takeIf { it.isNotBlank() } ?: packageName
    }

    @Immutable
    @Parcelize
    data class AppGroup(
        val uid: Int,
        val apps: List<AppInfo>,
        val profile: Natives.Profile?,
    ) : Parcelable {
        @IgnoredOnParcel
        val mainApp: AppInfo = apps.first()
        @IgnoredOnParcel
        val packageNames: List<String> = apps.map { it.packageName }
        @IgnoredOnParcel
        val profileKey: String = mainApp.profileKey
        @IgnoredOnParcel
        val allowSu: Boolean = profile?.allowSu == true
        @IgnoredOnParcel
        val userName: String? = Natives.getUserName(uid)
        @IgnoredOnParcel
        val hasCustomProfile : Boolean = profile?.let { if (it.allowSu) !it.rootUseDefault else !it.nonRootUseDefault } ?: false
    }

    private val appProcessingThreadPool = ThreadPoolExecutor(
        CORE_POOL_SIZE, MAX_POOL_SIZE, KEEP_ALIVE_TIME, TimeUnit.SECONDS,
        LinkedBlockingQueue()
    ) { runnable ->
        Thread(runnable, "AppProcessing-${System.currentTimeMillis()}").apply {
            isDaemon = true
            priority = Thread.NORM_PRIORITY
        }
    }.asCoroutineDispatcher()

    private val configChangeListeners = mutableSetOf<(String) -> Unit>()
    private val prefs = ksuApp.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    var search by mutableStateOf("")
    var showSystemApps by mutableStateOf(prefs.getBoolean(KEY_SHOW_SYSTEM_APPS, false))
        private set
    var currentSortType by mutableStateOf(loadCurrentSortType())
        private set
    val isRefreshing: Boolean get() = sharedIsRefreshing
    /** True after the first complete package snapshot. */
    val hasLoaded: Boolean get() = sharedHasLoaded
    /** Diagnostic for the last failed package load; the UI supplies localized recovery text. */
    val loadError: String? get() = sharedLoadError
    var showBatchActions by mutableStateOf(false)
        internal set
    var selectedUids by mutableStateOf<Set<Int>>(emptySet())
        private set
    /**
     * Compatibility bridge for older UI callers. Selection is UID based, so selecting
     * one package selects every package sharing that UID.
     */
    var selectedApps: Set<String>
        get() = apps.filter { it.uid in selectedUids }.mapTo(linkedSetOf()) { it.packageName }
        internal set(value) {
            selectedUids = apps.asSequence()
                .filter { it.packageName in value }
                .map { it.uid }
                .toSet()
        }
    val isUpdatingPermissions: Boolean get() = sharedIsUpdatingPermissions
    var loadingProgress by mutableFloatStateOf(0f)
        private set

    data class PermissionUpdateResult(val successCount: Int, val failureCount: Int) {
        val isSuccess: Boolean get() = failureCount == 0
    }

    private val permissionStore = object : AuthorizationPermissions.Store {
        override fun read(target: AuthorizationPermissions.Target): Natives.Profile {
            val profile = Natives.readAppProfile(target.uid)
                ?: return Natives.Profile(name = target.profileKey, currentUid = target.uid)
            check(profile.currentUid == target.uid) { "Profile UID does not match the requested application" }
            check(target.profileKey in target.packageNames) { "Profile key does not belong to the application" }
            return if (profile.name in target.packageNames) profile else profile.copy(name = target.profileKey)
        }

        override fun write(profile: Natives.Profile): Boolean = Natives.setAppProfile(profile)
    }

    private fun loadCurrentSortType(): SortType {
        val sortKey = prefs.getString(KEY_CURRENT_SORT_TYPE, SortType.NAME_ASC.persistKey)
            ?: SortType.NAME_ASC.persistKey
        return SortType.fromPersistKey(sortKey)
    }

    fun updateShowSystemApps(newValue: Boolean) {
        showSystemApps = newValue
        prefs.edit { putBoolean(KEY_SHOW_SYSTEM_APPS, newValue) }
    }

    fun updateCurrentSortType(newSortType: SortType) {
        currentSortType = newSortType
        prefs.edit { putString(KEY_CURRENT_SORT_TYPE, newSortType.persistKey) }
    }

    fun toggleBatchMode() {
        showBatchActions = !showBatchActions
        if (!showBatchActions) clearSelection()
    }

    fun toggleAppSelection(packageName: String) {
        apps.firstOrNull { it.packageName == packageName }?.let { toggleGroupSelection(it.uid) }
    }

    fun toggleGroupSelection(group: AppGroup) = toggleGroupSelection(group.uid)

    private fun toggleGroupSelection(uid: Int) {
        selectedUids = if (uid in selectedUids) selectedUids - uid else selectedUids + uid
    }

    fun selectGroups(groups: Collection<AppGroup>) {
        selectedUids = groups.mapTo(linkedSetOf()) { it.uid }
    }

    fun clearSelection() {
        selectedUids = emptySet()
    }

    fun exitBatchMode() {
        clearSelection()
        showBatchActions = false
    }

    suspend fun updateBatchPermissions(
        allowSu: Boolean,
        umountModules: Boolean? = null,
        requestedGroups: Collection<AppGroup>? = null,
    ): PermissionUpdateResult {
        val requestedUids = requestedGroups?.mapTo(linkedSetOf()) { it.uid } ?: selectedUids.toSet()
        if (requestedUids.isEmpty()) return PermissionUpdateResult(0, 0)
        val confirmedGroups = (requestedGroups ?: appGroups.filter { it.uid in requestedUids })
            .associateBy { it.uid }
        val confirmedTargets = requestedUids.map { uid ->
            confirmedGroups[uid]?.permissionTarget()
                ?: AuthorizationPermissions.Target(uid, "", emptySet())
        }
        if (!permissionMutex.tryLock()) return PermissionUpdateResult(0, requestedUids.size)
        sharedIsUpdatingPermissions = true
        try {
            // Finish the result bookkeeping even if the page leaves during a native write.
            return withContext(NonCancellable) {
                val result = withContext(Dispatchers.IO) {
                    appListMutex.withLock {
                        val currentTargets = appGroups.map { it.permissionTarget() }
                        AuthorizationPermissions.applyConfirmed(
                            confirmedTargets, currentTargets, allowSu, umountModules, permissionStore,
                        ).also { applyProfilesLocally(it.updated) }
                    }
                }
                reportPermissionUpdates(result)
                val failedUids = result.failed.mapTo(linkedSetOf()) { it.target.uid }
                selectedUids = (selectedUids - requestedUids) + failedUids
                if (selectedUids.isEmpty() && failedUids.isEmpty()) showBatchActions = false
                PermissionUpdateResult(result.updated.size, failedUids.size)
            }
        } finally {
            sharedIsUpdatingPermissions = false
            permissionMutex.unlock()
        }
    }

    private fun AppGroup.permissionTarget() = AuthorizationPermissions.Target(
        uid = uid,
        profileKey = profileKey,
        packageNames = packageNames.toSet(),
    )

    suspend fun updateGroupPermission(
        appGroup: AppGroup,
        allowSu: Boolean,
        umountModules: Boolean? = null
    ): Boolean {
        if (!permissionMutex.tryLock()) return false
        sharedIsUpdatingPermissions = true
        try {
            return withContext(NonCancellable) {
                val result = withContext(Dispatchers.IO) {
                    appListMutex.withLock {
                        AuthorizationPermissions.applyConfirmed(
                            listOf(appGroup.permissionTarget()),
                            appGroups.map { it.permissionTarget() },
                            allowSu,
                            umountModules,
                            permissionStore,
                        ).also { applyProfilesLocally(it.updated) }
                    }
                }
                reportPermissionUpdates(result)
                result.updated.isNotEmpty()
            }
        } finally {
            sharedIsUpdatingPermissions = false
            permissionMutex.unlock()
        }
    }

    /** Caller holds appListMutex so a package refresh cannot race identity validation. */
    private fun applyProfilesLocally(profiles: List<Natives.Profile>) {
        if (profiles.isEmpty()) return
        val updates = profiles.associateBy { it.currentUid }
        val updatedApps = apps.map { app ->
            updates[app.uid]?.let { app.copy(profile = it) } ?: app
        }
        apps = updatedApps
        appGroups = groupAppsByUid(updatedApps)
    }

    private fun reportPermissionUpdates(result: AuthorizationPermissions.Result) {
        result.failed.forEach { failure ->
            Log.e(TAG, "Failed to update permission for uid ${failure.target.uid}", failure.error)
        }
        result.updated.forEach { notifyConfigChange(it.name) }
    }

    fun updateUidProfileLocally(uid: Int, updatedProfile: Natives.Profile) {
        appListMutex.tryLock().let { locked ->
            if (locked) {
                try {
                    apps = apps.map { app ->
                        if (app.uid == uid) {
                            app.copy(profile = updatedProfile)
                        } else app
                    }
                    appGroups = groupAppsByUid(apps)
                } finally {
                    appListMutex.unlock()
                }
            }
        }
    }

    private fun notifyConfigChange(packageName: String) {
        configChangeListeners.forEach { listener ->
            try {
                listener(packageName)
            } catch (e: Exception) {
                Log.e(TAG, "Error notifying config change for $packageName", e)
            }
        }
    }

    suspend fun refreshAppConfigurations() {
        // Publish the complete snapshot before any concurrent successful write is published.
        // This avoids overwriting a just-granted permission with an older refresh result.
        appListMutex.withLock {
            withContext(appProcessingThreadPool) {
                supervisorScope {
                    val currentApps = apps.toList()
                    val uidGroups = currentApps.groupBy { it.uid }.values.toList()
                    val batches = uidGroups.chunked(BATCH_SIZE)
                    loadingProgress = 0f

                    val updatedApps = batches.mapIndexed { batchIndex, batch ->
                        async {
                            val batchResult = batch.flatMap { uidApps ->
                                try {
                                    val profile = loadUidProfile(uidApps)
                                    uidApps.map { it.copy(profile = profile) }
                                } catch (error: CancellationException) {
                                    throw error
                                } catch (error: Exception) {
                                    Log.e(TAG, "Error refreshing profile for uid ${uidApps.first().uid}", error)
                                    uidApps
                                }
                            }
                            loadingProgress = (batchIndex + 1).toFloat() / batches.size.coerceAtLeast(1)
                            batchResult
                        }
                    }.awaitAll().flatten()

                    apps = updatedApps
                    appGroups = groupAppsByUid(updatedApps)
                    loadingProgress = 1f
                }
            }
        }
    }
    private suspend fun connectKsuService(): IBinder? = ksuServiceConnectMutex.withLock {
        withTimeoutOrNull(15_000) {
            withContext(Dispatchers.Main.immediate) {
                var staleConnection: ServiceConnection? = null
                val cachedBinder = synchronized(ksuServiceLock) {
                    ksuServiceBinder?.takeIf { it.isBinderAlive } ?: run {
                        ksuServiceBinder = null
                        staleConnection = ksuServiceConnection
                        ksuServiceConnection = null
                        null
                    }
                }
                if (cachedBinder != null) return@withContext cachedBinder

                staleConnection?.let { connection ->
                    try {
                        com.topjohnwu.superuser.ipc.RootService.unbind(connection)
                    } catch (e: Exception) {
                        Log.e(TAG, "Failed to unbind stale KsuService connection", e)
                    }
                }

                suspendCancellableCoroutine<IBinder?> { continuation ->
                    val connection = KsuServiceConnection(continuation)
                    synchronized(ksuServiceLock) {
                        ksuServiceConnection = connection
                    }
                    continuation.invokeOnCancellation {
                        clearKsuServiceConnection(connection)
                        ksuServiceScope.launch { connection.cancel() }
                    }
                    val intent = Intent(ksuApp, KsuService::class.java)
                    try {
                        val task = com.topjohnwu.superuser.ipc.RootService.bindOrTask(
                            intent, ContextCompat.getMainExecutor(ksuApp), connection
                        )
                        // libsu keeps a pending-launch flag even if the launch fails.
                        // Keep its task so a subsequent connection can restart it.
                        if (task != null) ksuServiceStartTask = task
                        val startTask = ksuServiceStartTask
                        if (startTask != null && !ksuServiceStartRunning) {
                            ksuServiceStartRunning = true
                            ksuServiceScope.launch {
                                try {
                                    withContext(Dispatchers.IO) {
                                        val shell = KsuCli.SHELL
                                        check(shell.isRoot) { "Root access is required to start the package service" }
                                        shell.execTask(startTask)
                                    }
                                } catch (error: Exception) {
                                    Log.e(TAG, "Failed to start KsuService", error)
                                    val pending = synchronized(ksuServiceLock) {
                                        if (ksuServiceBinder == null) ksuServiceConnection else null
                                    }
                                    (pending as? KsuServiceConnection)?.fail()
                                } finally {
                                    ksuServiceStartRunning = false
                                }
                            }
                        }
                    } catch (e: Exception) {
                        connection.fail()
                        Log.e(TAG, "Failed to bind KsuService", e)
                    }
                }
            }
        }
    }

    suspend fun fetchAppList(forceRefresh: Boolean = false, useCache: Boolean = false) {
        fetchAppListMutex.withLock {
            if (useCache && sharedHasLoaded) return@withLock
            sharedIsRefreshing = true
            loadingProgress = 0f
            sharedLoadError = null
            try {
                retryPackageLoad { retry -> fetchAppListFromService(forceRefresh || retry) }
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                sharedLoadError = error.message?.takeIf { it.isNotBlank() }
                    ?: "Unable to load the app list"
                Log.e(TAG, "Failed to load app list", error)
            } finally {
                sharedIsRefreshing = false
            }
        }
    }

    internal suspend fun exportAllowlist(): String = withContext(Dispatchers.IO) {
        allowlistBackupMutex.withLock {
            val service = backupService()
            service.refreshPackages()
            val profiles = readAllowlistSnapshot()
            val protected = protectedProfileAppIds()
            val entries = profiles.values.filter {
                it.currentUid != AllowlistRestore.DEFAULT_UID &&
                    it.currentUid % PER_USER_RANGE !in protected
            }.map { profile ->
                val packages = service.getUidPackagesForBackup(profile.currentUid).toList()
                AllowlistBackup.Entry(packages, profile.copy(rules = service.readProfileRulesForBackup(profile.name)))
            }
            val document = AllowlistBackup.Document(
                profiles[AllowlistRestore.DEFAULT_UID]?.umountModules ?: false, entries
            )
            AllowlistBackup.validateAgainst(
                document,
                entries.associate { it.profile.currentUid to it.packages },
                protected
            )
            check(readAllowlistSnapshot() == profiles) { "Profiles changed during backup; retry" }
            AllowlistBackup.encode(document)
        }
    }

    internal suspend fun validateAllowlist(document: AllowlistBackup.Document) = withContext(Dispatchers.IO) {
        allowlistBackupMutex.withLock {
            validateBackup(document, backupService())
        }
    }

    internal suspend fun restoreAllowlist(document: AllowlistBackup.Document) = withContext(Dispatchers.IO) {
        allowlistBackupMutex.withLock {
            val service = backupService()
            validateBackup(document, service)
            val protected = protectedProfileAppIds()
            val before = readAllowlistSnapshot()
            before.values.filter {
                it.currentUid != AllowlistRestore.DEFAULT_UID &&
                    it.currentUid % PER_USER_RANGE !in protected
            }.forEach { profile ->
                check(profile.name in service.getUidPackagesForBackup(profile.currentUid)) {
                    "Existing profile package/UID mismatch: ${profile.currentUid}"
                }
            }
            check(readAllowlistSnapshot() == before) { "Profiles changed before restore; retry" }
            withContext(NonCancellable) {
                try {
                    AllowlistRestore.apply(document, before, protected, object : AllowlistRestore.Store {
                        override fun read(uid: Int) = Natives.readAppProfile(uid)
                        override fun write(profile: Natives.Profile) = Natives.setAppProfile(profile)
                        override fun readRules(key: String) = service.readProfileRulesForBackup(key)
                        override fun writeRules(key: String, rules: String) = service.writeProfileRulesForBackup(key, rules)
                    })
                } finally {
                    viewModelScope.launch {
                        runCatching {
                            withTimeout(30_000) { refreshAppConfigurations() }
                        }.onFailure { error ->
                            Log.e(TAG, "Failed to refresh app configurations after restore", error)
                        }
                    }
                }
            }
        }
    }

    private fun validateBackup(document: AllowlistBackup.Document, service: IKsuInterface) {
        service.refreshPackages()
        val installed = document.apps.associate { entry ->
            entry.profile.currentUid to service.getUidPackagesForBackup(entry.profile.currentUid).toList()
        }
        AllowlistBackup.validateAgainst(document, installed, protectedProfileAppIds())
    }

    private suspend fun backupService(): IKsuInterface {
        check(Natives.isManager && !Natives.checkUapiMismatch()) { "Compatible manager/kernel access is required" }
        check(KsuCli.SHELL.isRoot) { "Root access is required" }
        return IKsuInterface.Stub.asInterface(connectKsuService() ?: error("Package service is unavailable"))
    }

    private fun protectedProfileAppIds(): Set<Int> =
        Natives.getProtectedProfileAppIds().toSet() + (ksuApp.applicationInfo.uid % PER_USER_RANGE)

    private fun readAllowlistSnapshot(): Map<Int, Natives.Profile> {
        val uids = (Natives.getProfileUids(true).toList() + Natives.getProfileUids(false).toList()).sorted()
        check(uids.distinct().size == uids.size) { "Profile list changed while reading" }
        val result = uids.associateWith { uid -> Natives.readAppProfile(uid) ?: error("Profile disappeared: $uid") }.toMutableMap()
        val defaults = Natives.readAppProfile(AllowlistRestore.DEFAULT_UID)
        if (defaults != null) {
            check(defaults.name == AllowlistRestore.DEFAULT_KEY && !defaults.allowSu) {
                "Invalid default umount profile"
            }
            result[AllowlistRestore.DEFAULT_UID] = defaults
        } else {
            check(AllowlistRestore.DEFAULT_UID !in result) { "Default profile changed while reading" }
        }
        return result
    }

    private suspend fun fetchAppListFromService(forceRefresh: Boolean) {
        if (!withContext(Dispatchers.IO) { KsuCli.SHELL.isRoot }) {
            throw IllegalStateException("Root access is required to load the app list")
        }

        val binder = connectKsuService()
            ?: throw IllegalStateException("Package service is unavailable")

        withContext(Dispatchers.IO) {
            val pm = ksuApp.packageManager
            val allPackages = IKsuInterface.Stub.asInterface(binder)
            // Backup refreshes the same service snapshot. Keep it stable across
            // the count + page calls so a concurrent backup cannot mix snapshots.
            val packageSnapshot = allowlistBackupMutex.withLock {
                val total = if (forceRefresh) {
                    allPackages.refreshPackages()
                } else {
                    allPackages.packageCount
                }
                readPackagePages(total, allPackages::getPackages) { loadingProgress = it }
            }
            val result = packageSnapshot.map { packageInfo ->
                val appInfo = requireNotNull(packageInfo.applicationInfo)
                AppInfo(
                    label = try {
                        appInfo.loadLabel(pm).toString()
                    } catch (error: Exception) {
                        Log.w(TAG, "Unable to load label for ${packageInfo.packageName}", error)
                        packageInfo.packageName
                    },
                    packageInfo = packageInfo,
                    profile = null,
                )
            }

            appListMutex.withLock {
                val filteredApps = result.filter { it.packageName != ksuApp.packageName }
                val profiledApps = filteredApps.groupBy { it.uid }.values.flatMap { uidApps ->
                    val profile = loadUidProfile(uidApps)
                    uidApps.map { it.copy(profile = profile) }
                }
                apps = profiledApps
                appGroups = groupAppsByUid(profiledApps)
            }
            sharedHasLoaded = true
            sharedLoadError = null
            _isAppListLoaded.value = true
            loadingProgress = 1f
        }
    }

    val appGroupList by derivedStateOf {
        val query = search.trim()
        appGroups.filter { group ->
            query.isEmpty() || group.uid.toString().contains(query, true) || group.apps.any { app ->
                app.label.contains(query, true) ||
                        app.packageName.contains(query, true) ||
                        HanziToPinyin.getInstance().toPinyinString(app.label)?.contains(query, true) == true
            }
        }.filter { group ->
            group.uid == SHELL_UID || showSystemApps ||
                    group.apps.any { it.packageInfo.applicationInfo!!.flags.and(ApplicationInfo.FLAG_SYSTEM) == 0 }
        }
    }

    private fun groupAppsByUid(appList: List<AppInfo>): List<AppGroup> {
        return appList.groupBy { it.uid }
            .map { (uid, apps) ->
                val sortedApps = apps.sortedBy { it.label }
                val profile = apps.firstOrNull()?.profile
                AppGroup(
                    uid = uid,
                    apps = sortedApps,
                    profile = profile,
                )
            }
            .sortedBy { it.uid }
    }

    private fun loadUidProfile(uidApps: Collection<AppInfo>): Natives.Profile {
        val first = uidApps.first()
        val packageNames = uidApps.mapTo(mutableSetOf()) { it.packageName }
        val fallbackKey = packageNames.min()
        val profile = Natives.getAppProfile(fallbackKey, first.uid)
        return if (profile.name in packageNames) profile else profile.copy(name = fallbackKey)
    }
    override fun onCleared() {
        try {
            appProcessingThreadPool.close()
            configChangeListeners.clear()
        } catch (e: Exception) {
            Log.e(TAG, "Error cleaning up resources", e)
        }
    }
}
