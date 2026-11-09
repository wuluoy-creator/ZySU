package com.zying.zysu.ui.sumh.util

import com.zying.zysu.Natives
import com.zying.zysu.ui.util.getKsud
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject

object SUMHManager {
    val mountModes = listOf("auto", "sumh", "overlay", "magic", "none")
    val ruleModes = mountModes + "hide"
    // Keep multi-request reads from observing an in-progress Manager mutation.
    private val operations = Mutex()
    private val changes = MutableStateFlow(0L)
    val revision = changes.asStateFlow()

    data class ModuleRule(val path: String, val mode: String)
    data class ConfigSaveResult(
        val persisted: Boolean,
        val applied: Boolean,
        val error: String? = null,
        val noChanges: Boolean = false,
        val runtimeRequired: Boolean = false,
        val rebootRequired: Boolean = false,
    )
    data class MountConflict(val path: String, val modules: List<String>)
    data class ModuleInfo(
        val id: String, val mode: String, val rules: List<ModuleRule>,
        val name: String = id, val strategy: String = "none",
    )
    data class MountState(
        val modules: Map<String, ModuleInfo>,
        val available: Boolean,
        val globalMode: String,
        val externalOwner: String,
        val activeSUMHIds: Set<String> = emptySet(),
    )
    data class Snapshot(
        val system: JSONObject,
        val config: JSONObject,
        val activeRules: JSONArray,
        val hideRules: List<String>,
        val mounts: MountState,
    )
    data class RuleSnapshot(val system: JSONObject, val hideRules: List<String>)
    data class BackupSnapshot(val config: JSONObject, val hidePaths: List<String>)
    data class BackupRestoreResult(
        val configPersisted: Boolean,
        val configApplied: Boolean,
        val addedPaths: Int,
        val removedPaths: Int,
        val errors: List<String>,
        val runtimePending: Boolean,
        val rebootRequired: Boolean,
    )

    private suspend fun <T> read(block: () -> T): T = withContext(Dispatchers.IO) {
        operations.withLock { block() }
    }

    private fun command(vararg args: String): String {
        val request = JSONArray(args.toList()).toString().toByteArray(Charsets.UTF_8)
        val response = JSONObject(Natives.sumhpRequest(getKsud().toByteArray(Charsets.UTF_8), request).toString(Charsets.UTF_8))
        check(response.getBoolean("ok") && response.getInt("exit_code") == 0) {
            response.optString("stderr").ifBlank { "SUMHP error ${response.optInt("errno")}" }.take(1500)
        }
        return response.getString("stdout").trim()
    }

    private fun kernelSnapshot() = JSONObject(Natives.sumhKernelSnapshot().toString(Charsets.UTF_8))

    private fun objectResult(vararg args: String): JSONObject {
        val output = command(*args)
        val start = output.indexOf('{')
        check(start >= 0) { "Invalid SUMHP response" }
        return JSONObject(output.substring(start))
    }

    private fun arrayResult(vararg args: String): JSONArray {
        val output = command(*args)
        return JSONArray(output)
    }

    internal fun parseModules(root: JSONObject): Map<String, ModuleInfo> {
        val modules = root.getJSONArray("modules")
        return (0 until modules.length()).associate { i ->
            val module = modules.getJSONObject(i)
            val entries = module.optJSONArray("rules") ?: JSONArray()
            val info = ModuleInfo(
                module.getString("id"), module.getString("mode"),
                (0 until entries.length()).map { index ->
                    val rule = entries.getJSONObject(index)
                    ModuleRule(rule.getString("path"), rule.getString("mode"))
                },
                module.optString("name", module.getString("id")), module.optString("strategy", "none"),
            )
            info.id to info
        }
    }

    private fun readMountState(config: JSONObject, kernel: JSONObject): MountState {
        val meta = objectResult("api", "meta", "--control-only")
        check(meta.optBoolean("embedded") && meta.optBoolean("native_control")) {
            "Update ksud and restart the SUMHP controller"
        }
        return MountState(
            parseModules(objectResult("module", "list", "--all")), kernel.getBoolean("sumh_available"),
            config.optString("mount_backend", "auto"),
            meta.optString("external_mount_owner"),
            if (kernel.optBoolean("enabled"))
                Regex("/data/adb/modules/([^/\\s]+)").findAll(kernel.optString("rules")).map { it.groupValues[1] }.toSet()
            else emptySet(),
        )
    }

    suspend fun getMountState(): MountState = read {
        val config = objectResult("config", "show", "--stored")
        readMountState(config, kernelSnapshot())
    }

    private fun readHideRules(): List<String> {
        val entries = arrayResult("hide", "list")
        return (0 until entries.length()).map(entries::getString)
    }

    suspend fun ruleSnapshot(): RuleSnapshot = read {
        RuleSnapshot(kernelSnapshot(), readHideRules())
    }

    suspend fun listHideRules(): List<String> = read { readHideRules() }

    suspend fun backupSnapshot(): BackupSnapshot = read {
        BackupSnapshot(objectResult("config", "show", "--stored"), readHideRules())
    }

    internal suspend fun restoreBackup(backup: SUMHConfigBackup, expected: BackupSnapshot): BackupRestoreResult =
        withContext(NonCancellable + Dispatchers.IO) {
            operations.withLock {
                val current = BackupSnapshot(objectResult("config", "show", "--stored"), readHideRules())
                check(sumhJsonEqual(current.config, expected.config) && current.hidePaths == expected.hidePaths) {
                    "SUMH settings changed; preview the backup again"
                }
                validateSUMHRestore(backup, current.config)
                val changedKeys = backup.config.keys().asSequence().filter { key ->
                    !current.config.has(key) || !sumhJsonEqual(current.config.get(key), backup.config.get(key))
                }.toSet()
                val effects = sumhConfigEffects(changedKeys)
                val errors = mutableListOf<String>()
                var persisted = changedKeys.isEmpty()
                var applied = false
                var added = 0
                var removed = 0
                try {
                    if (changedKeys.isNotEmpty()) {
                        try {
                            command("config", "merge-json", backup.config.toString())
                            persisted = true
                        } catch (error: Exception) {
                            errors += "Configuration: ${error.message.orEmpty()}"
                        }
                    }
                    if (persisted && effects.runtimeRequired) {
                        try {
                            if (kernelSnapshot().optBoolean("sumh_available")) {
                                command("config", "apply")
                                applied = true
                            }
                        } catch (error: Exception) {
                            errors += "Runtime apply: ${error.message.orEmpty()}"
                        }
                    }
                    if (persisted) {
                        val old = current.hidePaths.toSet()
                        val desired = backup.hidePaths.toSet()
                        for (path in backup.hidePaths.filterNot { it in old }) {
                            try { command("hide", "add", path); added++ }
                            catch (error: Exception) { errors += "Add $path: ${error.message.orEmpty()}" }
                        }
                        for (path in current.hidePaths.filterNot { it in desired }) {
                            try {
                                requireSUMHHideRemoval(path, old, kernelSnapshot())
                                command("hide", "remove", path)
                                removed++
                            } catch (error: Exception) { errors += "Remove $path: ${error.message.orEmpty()}" }
                        }
                        try {
                            val stored = readHideRules().toSet()
                            added = backup.hidePaths.count { it !in old && it in stored }
                            removed = current.hidePaths.count { it !in desired && it !in stored }
                        } catch (error: Exception) {
                            errors += "Verify hidden paths: ${error.message.orEmpty()}"
                        }
                    }
                } finally { changes.value++ }
                BackupRestoreResult(persisted, applied, added, removed, errors,
                    persisted && effects.runtimeRequired && !applied, persisted && effects.rebootRequired)
            }
        }

    internal suspend fun kernelBuild(): KernelBuildState = read {
        val result = objectResult("sumh", "kernel-build", "show")
        KernelBuildState(
            enabled = result.getBoolean("enabled"),
            release = result.getString("release"),
            version = result.getString("version"),
            originalRelease = result.getString("original_release"),
            originalVersion = result.getString("original_version"),
            suggestedVersion = result.getString("suggested_version"),
            suggestionSource = result.getString("suggestion_source"),
        )
    }

    suspend fun snapshot(): Snapshot = read {
        val system = kernelSnapshot()
        val config = objectResult("config", "show", "--stored")
        val mountState = readMountState(config, system)
        val mounts = objectResult("api", "mounts")
        mounts.keys().forEach { key -> system.put(key, mounts.get(key)) }
        if (mountState.available) {
            val kstatStatus = try { objectResult("sumh", "kstat", "status") }
                catch (_: Exception) { null }
            if (SUMHUiAdapter.parseManagedKstatStatus(kstatStatus) != null)
                system.put("managed_kstat_status", kstatStatus)
        }
        val hides = readHideRules()
        Snapshot(
            system, config,
            JSONArray(system.optString("rules").lineSequence().filter(String::isNotBlank)
                .map { JSONObject().put("args", it) }.toList()),
            hides,
            mountState,
        )
    }

    private suspend fun mutate(block: () -> Unit) = withContext(NonCancellable + Dispatchers.IO) {
        operations.withLock {
            try { block() } finally { changes.value++ }
        }
    }

    suspend fun saveConfig(updates: JSONObject) = mutate {
        command("config", "merge-json", updates.toString())
    }

    suspend fun saveAndApplyConfig(updates: JSONObject, kernelAvailable: Boolean): ConfigSaveResult =
        withContext(NonCancellable + Dispatchers.IO) {
            operations.withLock {
                val plan = planSUMHConfigSave(updates.keys().asSequence().toSet(), kernelAvailable)
                var persisted = false
                try {
                    command("config", "merge-json", updates.toString())
                    persisted = true
                    if (plan.applyRuntime) command("config", "apply")
                    ConfigSaveResult(
                        persisted = true,
                        applied = plan.applyRuntime,
                        runtimeRequired = plan.effects.runtimeRequired,
                        rebootRequired = plan.effects.rebootRequired,
                    )
                } catch (error: Exception) {
                    ConfigSaveResult(
                        persisted = persisted,
                        applied = false,
                        error = error.message ?: "Configuration operation failed",
                        runtimeRequired = plan.effects.runtimeRequired,
                        rebootRequired = plan.effects.rebootRequired,
                    )
                } finally { changes.value++ }
            }
        }

    suspend fun checkConflicts(): List<MountConflict> = withContext(Dispatchers.IO) {
        val entries = arrayResult("module", "check-conflicts")
        (0 until entries.length()).map { index ->
            val entry = entries.getJSONObject(index)
            val modules = entry.getJSONArray("modules")
            MountConflict(entry.getString("file"), (0 until modules.length()).map(modules::getString))
        }
    }

    suspend fun clearLog() = mutate { Natives.sumhClearLog() }

    suspend fun applyConfig() = mutate { command("config", "apply") }

    suspend fun saveModule(id: String, mode: String, rules: List<ModuleRule>) = mutate {
        require(mode in mountModes)
        require(validateSUMHModuleRules(rules) == null) { "Invalid module path rules" }
        val meta = objectResult("api", "meta", "--control-only")
        check(meta.optString("external_mount_owner").isEmpty()) { "An external metamodule owns mounting" }
        check(meta.optBoolean("module_configure")) { "Update ksud and restart the SUMHP controller to save module settings atomically" }
        val previous = parseModules(objectResult("module", "list", "--all"))[id]
        check(previous != null) { "Module is no longer mountable" }
        if (mode != previous.mode || rules != previous.rules)
            command("module", "configure", id, mode, moduleRulesJson(rules).toString())
    }

    suspend fun saveModuleMode(id: String, mode: String) = mutate {
        require(mode in mountModes)
        val meta = objectResult("api", "meta", "--control-only")
        check(meta.optString("external_mount_owner").isEmpty()) { "An external metamodule owns mounting" }
        check(meta.optBoolean("module_configure")) { "Update ksud and restart the SUMHP controller to save module settings atomically" }
        val previous = parseModules(objectResult("module", "list", "--all"))[id]
        check(previous != null) { "Module is no longer mountable" }
        if (mode != previous.mode) command("module", "set-mode", id, mode)
    }

    suspend fun saveHide(path: String, remove: Boolean = false) = mutate {
        require(if (remove) path.startsWith('/') && '\u0000' !in path else validateSUMHHidePath(path) == null)
        if (remove) requireSUMHHideRemoval(path, readHideRules().toSet(), kernelSnapshot())
        command("hide", if (remove) "remove" else "add", path)
    }

    suspend fun changeUserHideRules(
        action: SUMHHideAction, paths: List<String>,
        shouldContinue: () -> Boolean = { true },
        onProgress: suspend (Int, Int) -> Unit = { _, _ -> },
    ): SUMHHideBatchResult =
        withContext(NonCancellable + Dispatchers.IO) {
            operations.withLock {
                // Ownership is read under the same Manager lock as the whole batch.
                // Never use a global rule clear or allow a selected module rule to be removed.
                val stored = readHideRules().toSet()
                val retryAvailable = action != SUMHHideAction.Retry || kernelSnapshot().optBoolean("sumh_available")
                try {
                    runSUMHHideBatch(action, paths, shouldContinue, onProgress) { path ->
                        when (action) {
                            SUMHHideAction.Add -> command("hide", "add", path)
                            SUMHHideAction.Remove -> {
                                // Recheck each item: a compatibility kernel can leave managed
                                // mode after the previous item removed its final registration.
                                requireSUMHHideRemoval(path, stored, kernelSnapshot())
                                command("hide", "remove", path)
                            }
                            SUMHHideAction.Retry -> {
                                check(retryAvailable) { "SUMH is unavailable; saved rules have been retained" }
                                check(path in stored) { "Rule is no longer saved; refresh the list" }
                                // Idempotent add also restores missing registrations and works
                                // with legacy kernels that lack managed-hide retry ioctls.
                                command("hide", "add", path)
                            }
                        }
                    }
                } finally { changes.value++ }
            }
        }

    suspend fun editUserHideRule(original: String, replacement: String): SUMHHideEditResult =
        withContext(NonCancellable + Dispatchers.IO) {
            operations.withLock {
                try {
                    val stored = readHideRules().toSet()
                    check(original in stored) { "Rule is no longer saved; refresh the list" }
                    runSUMHHideEdit(original, replacement,
                        prepareRemoval = { requireSUMHHideRemoval(original, stored, kernelSnapshot()) },
                        add = { command("hide", "add", it) },
                        remove = {
                            requireSUMHHideRemoval(it, stored, kernelSnapshot())
                            command("hide", "remove", it)
                        },
                    )
                } finally { changes.value++ }
            }
        }

    suspend fun clearRules() = mutate { command("sumh", "clear") }
    suspend fun retryUserHide(path: String) = mutate {
        Natives.sumhRetryUserHide(path.toByteArray(Charsets.UTF_8))
    }
    suspend fun addMapsRule(numbers: List<String>, path: String) = mutate {
        val parsed = numbers.mapIndexed { index, value ->
            if (index == 1 || index == 3) parseSUMHMapsDevice(value) else parseSUMHMapsNumber(value)
        }
        require(parsed.size == 4 && parsed.all { it != null } && validateSUMHMapsPath(path) == null)
        Natives.sumhAddMapsRule(parsed.map { requireNotNull(it).toLong() }.toLongArray(), path.toByteArray(Charsets.UTF_8))
    }
    suspend fun clearMapsRules() = mutate { Natives.sumhClearMapsRules() }
    suspend fun resetRecovery() = mutate { command("recovery", "reset") }

    suspend fun readLog(): String = withContext(Dispatchers.IO) {
        Natives.sumhReadLog(false).toString(Charsets.UTF_8)
    }

    internal suspend fun syncPartitions() = mutate { command("config", "sync-partitions") }
}
