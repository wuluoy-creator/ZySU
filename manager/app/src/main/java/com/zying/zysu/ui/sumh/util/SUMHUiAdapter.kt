package com.zying.zysu.ui.sumh.util

import android.util.Log
import com.zying.zysu.Natives
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.util.Locale

object SUMHUiAdapter {
    enum class SUMHStatus { AVAILABLE, NOT_PRESENT, KERNEL_TOO_OLD, MODULE_TOO_OLD }
    data class FeaturesResult(val bitmask: Int, val names: List<String>)
    data class ModuleInfo(val id: String, val name: String, val mode: String, val strategy: String)
    data class ActiveRule(val type: String, val src: String, val target: String? = null, val isUserDefined: Boolean = false,
        val hideState: Int? = null, val hideError: Int = 0, val persisted: Boolean = true,
        val rawOperands: Boolean = false)
    data class RuntimeRules(val rules: List<ActiveRule>, val incomplete: Boolean = false)
    data class PartitionInfo(val name: String)
    data class MountStats(
        val totalMounts: Int, val overlayfsMounts: Int,
    )
    data class SystemInfo(
        val kernel: String, val mountBase: String,
        val activeMounts: List<String>, val sumhModuleIds: List<String>,
        val sumhMismatch: Boolean, val mismatchMessage: String?,
        val mountStats: MountStats? = null, val detectedPartitions: List<PartitionInfo> = emptyList(),
        val hooks: String = "", val viewsEnabled: Boolean? = null,
    )
    data class StorageInfo(val size: String, val used: String, val avail: String, val percent: String, val type: String)
    data class MapsPathRule(val targetPath: String, val replacementPath: String)
    data class SUMHPConfig(
        val tempdir: String = "",
        val mountsource: String = "KSU", val debug: Boolean = false, val verbose: Boolean = false,
        val fsType: String = "auto", val enableKernelDebug: Boolean = false,
        val enableStealth: Boolean = true, val enableOverlayXattrHide: Boolean = false,
        val enableMountHide: Boolean = false,
        val enableMapsSpoof: Boolean = false, val enableStatfsSpoof: Boolean = false,
        val mapsRules: List<MapsPathRule> = emptyList(),
        val enableKernelBuildSpoof: Boolean = false,
        val kernelBuildRelease: String = "", val kernelBuildVersion: String = "",
        val kernelBuildApplyStage: String = KERNEL_BUILD_EARLY_STAGE,
        val mirrorPath: String = "", val partitions: List<String> = emptyList(),
        val mountBackend: String = "auto", val overlayfsEnabled: Boolean = true, val magicMountEnabled: Boolean = true,
        val kernelAvailable: Boolean = false, val externalOwner: String = "",
        val original: JSONObject = JSONObject(), val supported: Set<String> = emptySet(),
    )
    data class UiState(
        val version: String, val status: SUMHStatus, val config: SUMHPConfig,
        val modules: List<ModuleInfo>, val system: SystemInfo, val storage: StorageInfo,
        val rules: List<ActiveRule>, val features: FeaturesResult?, val rulesIncomplete: Boolean = false,
        val controlState: SUMHControlState = SUMHControlState(),
    )

    internal fun config(raw: JSONObject, names: Set<String>, available: Boolean, externalOwner: String): SUMHPConfig {
        val partitions = raw.optJSONArray("partitions")
        val mapsRules = raw.optJSONArray("maps_rules")
        return SUMHPConfig(
            tempdir = raw.optString("work_dir", "/dev/sumhp").takeUnless { it == "/dev/sumhp" }.orEmpty(),
            mountsource = raw.optString("mountsource", "KSU"),
            debug = raw.optBoolean("debug"), verbose = raw.optBoolean("verbose"),
            fsType = raw.optString("fs_type", "auto"), enableKernelDebug = raw.optBoolean("enable_kernel_debug"),
            enableStealth = raw.optBoolean("enable_stealth", true),
            enableOverlayXattrHide = raw.optBoolean("enable_overlay_xattr_hide"),
            enableMountHide = raw.optBoolean("enable_mount_hide"),
            enableMapsSpoof = raw.optBoolean("enable_maps_spoof"),
            mapsRules = if (mapsRules == null) emptyList() else (0 until mapsRules.length()).map {
                val rule = mapsRules.getJSONObject(it)
                MapsPathRule(rule.getString("target_path"), rule.getString("replacement_path"))
            },
            enableStatfsSpoof = raw.optBoolean("enable_statfs_spoof"),
            enableKernelBuildSpoof = raw.optBoolean("enable_kernel_build_spoof"),
            kernelBuildRelease = raw.optString("kernel_build_release"),
            kernelBuildVersion = raw.optString("kernel_build_version"),
            kernelBuildApplyStage = raw.optString("kernel_build_apply_stage", KERNEL_BUILD_EARLY_STAGE),
            mirrorPath = raw.optString("mirror_dir"),
            partitions = if (partitions == null) emptyList() else (0 until partitions.length()).map(partitions::getString),
            mountBackend = raw.optString("mount_backend", "auto"),
            overlayfsEnabled = raw.optBoolean("overlayfs_enabled", true), magicMountEnabled = raw.optBoolean("magic_mount_enabled", true),
            kernelAvailable = available, externalOwner = externalOwner,
            original = raw, supported = names,
        )
    }

    internal fun rules(raw: String, userRules: Set<String> = emptySet()): RuntimeRules {
        // LIST_RULES is newline terminated and can stop in the middle of a row at
        // its size limit. Never turn that final fragment into a valid rule.
        var incomplete = (raw.isNotEmpty() && !raw.endsWith('\n')) ||
            raw.toByteArray(Charsets.UTF_8).size >= 64 * 1024 - 1
        val complete = raw.substring(0, raw.lastIndexOf('\n') + 1)
        val parsed = complete.split('\n').mapNotNull { line ->
            if (line.isEmpty() || line.startsWith("SUMH Protocol: ") || line.startsWith("SUMH Enabled: "))
                return@mapNotNull null
            val type = line.substringBefore(' ')
            val payload = line.substringAfter(' ', "")
            val rule = when (type) {
                "hide" -> payload.takeIf { it.startsWith('/') && '\u0000' !in it }?.let {
                    ActiveRule(type, it, isUserDefined = it in userRules)
                }
                "add", "merge" -> {
                    val operands = if (type == "add") {
                        val suffix = Regex(" -?\\d+$").find(payload)
                        if (suffix == null) null else payload.substring(0, suffix.range.first)
                    } else payload
                    val boundaries = operands?.let { Regex(" /").findAll(it).map { match -> match.range.first }.toList() }.orEmpty()
                    when {
                        operands == null || !operands.startsWith('/') || '\u0000' in operands || boundaries.isEmpty() -> null
                        boundaries.size == 1 -> {
                            val split = boundaries.single()
                            ActiveRule(type, operands.substring(0, split), operands.substring(split + 1))
                        }
                        // The legacy text protocol does not escape spaces. Keep
                        // ambiguous operands intact rather than inventing paths.
                        else -> ActiveRule(type, operands, rawOperands = true)
                    }
                }
                "mount_hide" -> payload.takeIf { it == "enabled" || it in listOf("enabled mode=normal", "enabled mode=aggressive") }
                    ?.let { ActiveRule(type, it) }
                "maps_spoof", "statfs_spoof", "stealth", "kernel_build_spoof" ->
                    payload.takeIf { it == "enabled" }?.let { ActiveRule(type, it) }
                // Injection parents and xattr superblock bookkeeping support
                // other rules; showing them again adds no user-managed entry.
                "inject", "hide_xattr_sb" -> return@mapNotNull null
                else -> null
            }
            if (rule == null) incomplete = true
            rule
        }
        return RuntimeRules(parsed.distinctBy { Triple(it.type, it.src, it.target) }, incomplete)
    }

    private fun registeredUserRules(snapshot: SUMHManager.RuleSnapshot): List<ActiveRule> {
        val registered = snapshot.system.optJSONArray("user_hide") ?: return emptyList()
        val storedPaths = snapshot.hideRules.toSet()
        return (0 until registered.length()).mapNotNull { index ->
            val state = registered.optJSONObject(index) ?: return@mapNotNull null
            val path = state.opt("path") as? String ?: return@mapNotNull null
            if (!path.startsWith('/') || '\u0000' in path) return@mapNotNull null
            val management = state.opt("management") as? Int
            val binding = state.opt("binding") as? Int
            ActiveRule("hide", path, isUserDefined = true,
                hideState = when {
                    management == 1 -> 4
                    management == 0 && binding != null && binding in 0..3 -> binding
                    else -> null
                }, hideError = state.optInt("error"), persisted = path in storedPaths)
        }
    }

    internal fun userRules(snapshot: SUMHManager.RuleSnapshot): List<ActiveRule> {
        val storedPaths = snapshot.hideRules.toSet()
        if (!snapshot.system.has("user_hide")) return storedPaths.map {
            ActiveRule("hide", it, isUserDefined = true)
        }
        val registered = registeredUserRules(snapshot)
        val array = snapshot.system.optJSONArray("user_hide")
        val complete = array != null && registered.size == array.length()
        val live = registered.associateBy { it.src }
        return (snapshot.hideRules + live.keys).distinct().map { path ->
            live[path] ?: ActiveRule("hide", path, isUserDefined = true,
                hideState = if (complete) -1 else null)
        }
    }

    internal fun activeRules(snapshot: SUMHManager.RuleSnapshot): RuntimeRules {
        val legacy = rules(snapshot.system.optString("rules"), if (snapshot.system.has("user_hide")) emptySet() else snapshot.hideRules.toSet())
        if (!snapshot.system.has("user_hide")) return legacy
        val registered = registeredUserRules(snapshot)
        val bound = registered.filter { it.hideState == 2 }.distinctBy { it.src }
        val boundPaths = bound.mapTo(HashSet()) { it.src }
        val array = snapshot.system.optJSONArray("user_hide")
        val invalid = array == null || registered.size != array.length() ||
            registered.map { it.src }.distinct().size != registered.size || registered.any { it.hideState == null }
        // Saved definitions are not kernel bindings. Pending, failed and suspended
        // entries stay editable in Hidden paths without inflating runtime counts.
        return RuntimeRules(legacy.rules.filterNot { it.type == "hide" && it.src in boundPaths } + bound,
            legacy.incomplete || invalid)
    }

    private fun size(value: Long): String {
        val units = listOf("B", "KiB", "MiB", "GiB", "TiB")
        var amount = value.toDouble()
        var unit = 0
        while (amount >= 1024 && unit < units.lastIndex) { amount /= 1024; unit++ }
        return String.format(Locale.ROOT, "%.1f %s", amount, units[unit])
    }

    internal fun parseFeatures(raw: JSONObject?): FeaturesResult? {
        // Only an explicit capability list can establish that a feature is unsupported.
        val names = raw?.optJSONArray("names") ?: return null
        return FeaturesResult(raw.optInt("bitmask"), (0 until names.length()).map(names::getString))
    }

    suspend fun load(): UiState = withContext(Dispatchers.IO) {
        val snapshot = SUMHManager.snapshot()
        val sys = snapshot.system
        val protocol = sys.optInt("kernel_version")
        val status = when {
            snapshot.mounts.available -> SUMHStatus.AVAILABLE
            protocol == 0 -> SUMHStatus.NOT_PRESENT
            protocol < 1 -> SUMHStatus.KERNEL_TOO_OLD
            else -> SUMHStatus.MODULE_TOO_OLD
        }
        val features = parseFeatures(sys.optJSONObject("features"))
        val supported = features?.names.orEmpty()
        val viewConfig = config(snapshot.config, supported.toSet(), snapshot.mounts.available, snapshot.mounts.externalOwner)
        val base = sys.optString("mount_base")
        val disk = try {
            val path = base.ifBlank { "/data/adb/ksu/sumhp" }
            JSONObject(Natives.sumhStorageInfo(path.toByteArray(Charsets.UTF_8)).toString(Charsets.UTF_8))
        } catch (error: Exception) {
            if (error is CancellationException) throw error
            Log.w("SUMHUiAdapter", "Storage information unavailable", error)
            JSONObject()
        }
        val total = disk.optLong("size", -1)
        val avail = disk.optLong("avail", -1)
        val used = (total - avail).coerceAtLeast(0)
        val storage = if (total < 0) StorageInfo("—", "—", "—", "—", "unknown") else StorageInfo(
            size(total), size(used), size(avail), "${if (total == 0L) 0 else (100.0 * used / total).toInt()}%", disk.optString("type", "unknown"),
        )
        val active = sys.optJSONArray("active_mounts")
        val mounts = if (active == null) emptyList() else (0 until active.length()).map(active::getString)
        val parts = sys.optJSONArray("detectedPartitions")
        val partitions = if (parts == null) emptyList() else (0 until parts.length()).map {
            PartitionInfo(parts.getJSONObject(it).getString("name"))
        }
        val rawRules = sys.optString("rules")
        val ids = Regex("/data/adb/modules/([^/\\s]+)").findAll(rawRules).map { it.groupValues[1] }.distinct().toList()
        val stats = sys.optJSONObject("mountStats")
        val runtimeRules = activeRules(SUMHManager.RuleSnapshot(sys, snapshot.hideRules))
        UiState(
            protocol.toString(), status, viewConfig,
            snapshot.mounts.modules.values.map { ModuleInfo(it.id, it.name, it.mode, it.strategy) },
            SystemInfo(sys.optString("kernel"), base, mounts, ids,
                protocol != 0 && protocol != 1, null,
                stats?.let { MountStats(it.optInt("total_mounts"), it.optInt("overlayfs_mounts")) }, partitions,
                sys.optString("hooks"), sys.optBoolean("enabled")),
            storage, runtimeRules.rules,
            features, runtimeRules.incomplete,
            snapshot.controlState,
        )
    }

    private suspend fun success(action: suspend () -> Unit): Boolean = try {
        action(); true
    } catch (error: Exception) {
        if (error is CancellationException) throw error
        Log.w("SUMHUiAdapter", "Controller operation failed", error)
        false
    }

    internal fun configUpdates(value: SUMHPConfig): JSONObject {
        val old = config(value.original, value.supported, value.kernelAvailable, value.externalOwner)
        val updates = JSONObject()
        fun change(key: String, before: Any, after: Any) { if (before != after) updates.put(key, after) }
        if (old.tempdir != value.tempdir) updates.put("work_dir", value.tempdir.ifEmpty { "/dev/sumhp" })
        change("mountsource", old.mountsource, value.mountsource)
        change("debug", old.debug, value.debug)
        change("verbose", old.verbose, value.verbose)
        change("fs_type", old.fsType, value.fsType)
        change("enable_kernel_debug", old.enableKernelDebug, value.enableKernelDebug)
        change("enable_stealth", old.enableStealth, value.enableStealth)
        change("enable_overlay_xattr_hide", old.enableOverlayXattrHide, value.enableOverlayXattrHide)
        change("enable_mount_hide", old.enableMountHide, value.enableMountHide)
        change("enable_maps_spoof", old.enableMapsSpoof, value.enableMapsSpoof)
        if (old.mapsRules != value.mapsRules) {
            require(value.mapsRules.size <= SUMH_MAX_MAPS_PATH_RULES)
            require(value.mapsRules.map { it.targetPath }.distinct().size == value.mapsRules.size)
            require(value.mapsRules.all {
                validateSUMHManagedMapsPath(it.targetPath, 4095) == null && validateSUMHManagedMapsPath(it.replacementPath) == null
            })
            require(mapsRulesWithinBudget(value.mapsRules))
            updates.put("maps_rules", mapsRulesJson(value.mapsRules))
        }
        change("enable_statfs_spoof", old.enableStatfsSpoof, value.enableStatfsSpoof)
        change("enable_kernel_build_spoof", old.enableKernelBuildSpoof, value.enableKernelBuildSpoof)
        change("kernel_build_release", old.kernelBuildRelease, value.kernelBuildRelease)
        change("kernel_build_version", old.kernelBuildVersion, value.kernelBuildVersion)
        change("kernel_build_apply_stage", old.kernelBuildApplyStage, value.kernelBuildApplyStage)
        change("mirror_dir", old.mirrorPath, value.mirrorPath)
        change("mount_backend", old.mountBackend, value.mountBackend)
        change("overlayfs_enabled", old.overlayfsEnabled, value.overlayfsEnabled)
        change("magic_mount_enabled", old.magicMountEnabled, value.magicMountEnabled)
        if (old.partitions != value.partitions) updates.put("partitions", org.json.JSONArray(value.partitions))
        val plan = planSUMHConfigSave(
            updates.keys().asSequence().toSet(),
            value.kernelAvailable,
            value.original.optString("mount_hide_mode", "normal"),
        )
        if (plan.normalizeMountHideMode) updates.put("mount_hide_mode", "normal")
        return updates
    }

    suspend fun saveConfig(value: SUMHPConfig): SUMHManager.ConfigSaveResult {
        val updates = configUpdates(value)
        if (updates.length() == 0) return SUMHManager.ConfigSaveResult(false, false, noChanges = true)
        return SUMHManager.saveAndApplyConfig(updates, value.kernelAvailable)
    }

    suspend fun getActiveRules(): RuntimeRules = withContext(Dispatchers.IO) {
        val snapshot = SUMHManager.ruleSnapshot()
        activeRules(snapshot)
    }
    suspend fun listUserHideRules(): List<String> = SUMHManager.listHideRules()
    suspend fun userHideStates(): List<ActiveRule> {
        val snapshot = SUMHManager.ruleSnapshot()
        // A saved deletion can fail in the kernel. Keep that live definition visible
        // so the user can retry removal instead of losing its management entry.
        return userRules(snapshot)
    }
    suspend fun changeUserHideRules(
        action: SUMHHideAction, paths: List<String>,
        shouldContinue: () -> Boolean = { true },
        onProgress: suspend (Int, Int) -> Unit = { _, _ -> },
    ) = SUMHManager.changeUserHideRules(action, paths, shouldContinue, onProgress)
    suspend fun editUserHideRule(original: String, replacement: String) =
        SUMHManager.editUserHideRule(original, replacement)
    suspend fun retryUserHide(path: String) = success { SUMHManager.retryUserHide(path) }
    suspend fun addUserHideRule(path: String) = success { SUMHManager.saveHide(path) }
    suspend fun removeUserHideRule(path: String) = success { SUMHManager.saveHide(path, true) }
    suspend fun clearAllRules() = success { SUMHManager.clearRules() }
    suspend fun clearMapsRules() = success { SUMHManager.clearMapsRules() }
    suspend fun addMapsRule(ti: Long, td: Long, si: Long, sd: Long, path: String) = success {
        SUMHManager.addMapsRule(listOf(ti, td, si, sd).map { it.toULong().toString() }, path)
    }
    suspend fun readLog(): String = SUMHManager.readLog()
    suspend fun readKernelLog(): String = withContext(Dispatchers.IO) {
        filterSUMHKernelLog(Natives.sumhReadLog(true).toString(Charsets.UTF_8))
    }
    suspend fun clearLog() = success { SUMHManager.clearLog() }
    suspend fun retryApply() = success { SUMHManager.applyConfig() }
    suspend fun syncPartitionsWithDaemon(): SUMHPConfig? = try {
        SUMHManager.syncPartitions(); load().config
    } catch (error: Exception) {
        if (error is CancellationException) throw error
        null
    }
}
