package com.zying.zysu.ui.sumh.util

import com.zying.zysu.Natives
import com.zying.zysu.ksuApp
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
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.io.File

object SUMHManager {
    val mountModes = listOf("auto", "sumh", "overlay", "magic", "none")
    val ruleModes = mountModes + "hide"
    // Keep multi-request reads from observing an in-progress Manager mutation.
    private val operations = Mutex()
    private val changes = MutableStateFlow(0L)
    val revision = changes.asStateFlow()
    internal val operationTracker by lazy {
        SUMHOperationTracker(SUMHOperationFile(File(ksuApp.noBackupFilesDir, "sumh-operations"))) { request ->
            val bytes = Natives.sumhOperation(getKsud().toByteArray(Charsets.UTF_8), request.toByteArray(Charsets.UTF_8))
            require(bytes.size <= 4 * 1024 * 1024)
            val text = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString()
            SUMHOperationsParser.dispatch(text)
        }
    }

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
        val controlState: SUMHControlState = SUMHControlState(),
    )
    data class RuleSnapshot(val system: JSONObject, val hideRules: List<String>)

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

    private fun queryCommand(vararg args: String): SUMHQueryReply = SUMHControlStateParser.reply(
        Natives.sumhpRequest(getKsud().toByteArray(Charsets.UTF_8),
            JSONArray(args.toList()).toString().toByteArray(Charsets.UTF_8)).toString(Charsets.UTF_8))

    internal suspend fun collectInventory(onProgress: (SUMHInventoryCollection) -> Unit = {}): SUMHInventoryCollection =
        withContext(Dispatchers.IO) {
            collectSUMHInventory(request = { args ->
                // Lock one request, not the entire scan. Concurrent mutation makes
                // the shared kernel token stale, rather than blocking all controls.
                read {
                    val raw = Natives.sumhpRequest(getKsud().toByteArray(Charsets.UTF_8),
                        JSONArray(args.toList()).toString().toByteArray(Charsets.UTF_8))
                    val text = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                        .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(raw)).toString()
                    SUMHControlStateParser.reply(text, maxPayloadBytes = 1024 * 1024)
                }
            }, onProgress = onProgress)
        }

    /** Dedicated exact preview transport: never routes through the recovering daemon. */
    internal suspend fun previewMaps(): SUMHMapsPreviewResult = withContext(Dispatchers.IO) {
        try {
            val raw = Natives.sumhMapsPreview(getKsud().toByteArray(Charsets.UTF_8))
            val text = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(raw)).toString()
            SUMHMapsPreviewParser.reply(SUMHControlStateParser.reply(text,
                maxPayloadBytes = 512 * 1024, maxEnvelopeBytes = 4 * 1024 * 1024))
        } catch (error: Exception) {
            if (error is kotlinx.coroutines.CancellationException) throw error
            SUMHMapsPreviewResult(error = error.message ?: "preview_transport_failed")
        }
    }

    private fun hideDefinitionReply(args: List<String>): SUMHQueryReply {
        val raw = Natives.sumhUserHide(getKsud().toByteArray(Charsets.UTF_8), JSONArray(args).toString().toByteArray(Charsets.UTF_8))
        val text = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(raw)).toString()
        return HideDefinitionsParser.dispatch(text)
    }

    /** Fixed, bounded read-only path; never queues behind mutations or starts the recovering daemon. */
    internal suspend fun capabilities(): SUMHCapabilitiesResult = withContext(Dispatchers.IO) {
        try {
            val support = SUMHCapabilitiesParser.capabilities(hideDefinitionReply(listOf("api", "meta", "--control-only")))
            if (support != SUMHSchemaSupport.Supported)
                return@withContext SUMHCapabilitiesResult(error = "capability_snapshot_${support.name.lowercase(java.util.Locale.ROOT)}")
            SUMHCapabilitiesParser.reply(SUMHCapabilitiesParser.envelope(Natives.sumhCapabilities(getKsud().toByteArray(Charsets.UTF_8))))
        } catch (failure: Exception) {
            if (failure is kotlinx.coroutines.CancellationException) throw failure
            SUMHCapabilitiesResult(error = failure.message ?: "capability_snapshot_transport_failed")
        }
    }

    /** Capability negotiation and planning both use exact, non-recovering private routes. */
    internal suspend fun previewModule(request: SUMHModulePreviewRequest): SUMHModulePreviewResult = withContext(Dispatchers.IO) {
        try {
            val support = SUMHModulePreviewParser.capabilities(hideDefinitionReply(listOf("api", "meta", "--control-only")))
            check(support.error == null) { support.error ?: "module_preview_capability_unknown" }
            check(if (request.candidateJson == null) support.saved else support.candidate) { "module_preview_capability_unsupported" }
            check(request.explainPath.isEmpty() || support.explain) { "module_explain_capability_unsupported" }
            val raw = Natives.sumhModulePreview(getKsud().toByteArray(Charsets.UTF_8),
                request.candidateJson?.toByteArray(Charsets.UTF_8), request.explainPath.toByteArray(Charsets.UTF_8))
            SUMHModulePreviewParser.reply(SUMHModulePreviewParser.envelope(raw), request)
        } catch (failure: Exception) {
            if (failure is kotlinx.coroutines.CancellationException) throw failure
            SUMHModulePreviewResult(error = failure.message ?: "module_preview_transport_failed")
        }
    }

    internal suspend fun hideDefinitions(onProgress: (Int) -> Unit = {}): HideDefinitionCollection = withContext(Dispatchers.IO) {
        // Queries must not wait behind an unrelated, unbounded legacy mutation.
        // File revision and the final page protect this cooperative read.
        collectHideDefinitions(request = ::hideDefinitionReply, onProgress = onProgress)
    }
    internal suspend fun savedHideDefinitions(onProgress: (Int) -> Unit = {}): SUMHHideSavedDefinitions = withContext(Dispatchers.IO) {
        try {
            val capabilities = parseSUMHHideTransferCapabilities(hideDefinitionReply(listOf("api", "meta", "--control-only")))
            require(capabilities.export) { "hide_export_capability_not_supported" }
            collectSUMHHideSavedDefinitions(request = ::hideDefinitionReply, progress = onProgress)
        } catch (error: Exception) {
            if (error is kotlinx.coroutines.CancellationException) throw error
            SUMHHideSavedDefinitions(error = error.message ?: "hide_export_capability_unknown")
        }
    }
    internal suspend fun hideImportSupported(): Boolean = withContext(Dispatchers.IO) {
        parseSUMHHideTransferCapabilities(hideDefinitionReply(listOf("api", "meta", "--control-only"))).import
    }

    internal suspend fun changeHideDefinition(args: List<String>, revision: String, boot: String): HideMutationResult =
        withContext(NonCancellable + Dispatchers.IO) {
            if (!operations.tryLock()) return@withContext HideMutationResult(error = "hide_manager_busy_not_started")
            try {
                operationTracker.changeHide(args, revision, boot)
            } catch (error: Exception) { HideMutationResult(error = error.message ?: "hide_result_unknown") }
            finally { changes.value++; operations.unlock() }
        }

    internal suspend fun discoverPartitions(): SUMHPartitionDiscoveryResult = withContext(Dispatchers.IO) {
        try { SUMHPartitionsParser.discoveryReply(partitionReply(Natives.sumhPartitionDiscovery(getKsud().toByteArray(Charsets.UTF_8)))) }
        catch (error: Exception) {
            if (error is kotlinx.coroutines.CancellationException) throw error
            SUMHPartitionDiscoveryResult(error = error.message ?: "discovery_transport_failed")
        }
    }

    /** Direct save-only native route. Never starts a daemon or retries an unknown result. */
    internal suspend fun savePartitions(revision: String, names: List<String>): SUMHPartitionSaveResult =
        withContext(NonCancellable + Dispatchers.IO) {
            operations.withLock {
                try {
                    require(revision.matches(Regex("sha256-v1:[0-9a-f]{64}")))
                    SUMHPartitionsParser.saveReply(partitionReply(Natives.sumhPartitionSave(getKsud().toByteArray(Charsets.UTF_8),
                        revision.toByteArray(Charsets.UTF_8), partitionsSaveJson(names).toByteArray(Charsets.UTF_8))))
                } catch (error: Exception) { SUMHPartitionSaveResult(error = error.message ?: "save_outcome_unknown") }
                // The dedicated caller updates its receipt locally. Broadcasting
                // the generic revision would trigger other screens' daemon queries.
            }
        }

    private fun partitionReply(raw: ByteArray): SUMHQueryReply {
        require(raw.size <= 4 * 1024 * 1024)
        val text = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(raw)).toString()
        return SUMHControlStateParser.reply(text, maxPayloadBytes = 512 * 1024, maxEnvelopeBytes = 4 * 1024 * 1024)
    }

    /** Optional reads cannot discard valid config. Nonzero exit status retains parsed partial facts. */
    internal fun readControlState(meta: JSONObject, request: (Array<out String>) -> SUMHQueryReply): SUMHControlState {
        val capabilities = SUMHControlStateParser.capabilities(meta)
        val state = if (capabilities.state == SUMHSchemaSupport.Supported) try {
            SUMHControlStateParser.state(request(arrayOf("api", "state")))
        } catch (error: Exception) { SUMHStateQuery<SUMHStateSample>(error = error.message ?: "State query failed") }
        else SUMHStateQuery()
        // Use the runtime sample belonging to the ledger when available, never combine
        // its comparisons with a later independent runtime generation.
        val runtime = state.value?.runtime?.takeIf { it.value != null } ?: if (capabilities.runtime == SUMHSchemaSupport.Supported) try {
            SUMHControlStateParser.runtime(request(arrayOf("api", "runtime")))
        } catch (error: Exception) { SUMHStateQuery<SUMHRuntimeSample>(error = error.message ?: "Runtime query failed") }
        else SUMHStateQuery()
        return SUMHControlState(capabilities, state, runtime)
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

    private fun readMountState(config: JSONObject, kernel: JSONObject,
        meta: JSONObject = objectResult("api", "meta", "--control-only")): MountState {
        check(meta.optBoolean("embedded") && meta.optBoolean("native_control")) {
            "Update ksud and restart the SUMHP controller"
        }
        return MountState(
            parseModules(objectResult("module", "list", "--all")), kernel.getBoolean("sumh_available"),
            config.optString("mount_backend", "auto"),
            meta.optString("external_mount_owner"),
            // Legacy path inference for display, never an owner or applied-state proof.
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
        val meta = objectResult("api", "meta", "--control-only")
        val mountState = readMountState(config, system, meta)
        val mounts = objectResult("api", "mounts")
        mounts.keys().forEach { key -> system.put(key, mounts.get(key)) }
        val hides = readHideRules()
        Snapshot(
            system, config,
            JSONArray(system.optString("rules").lineSequence().filter(String::isNotBlank)
                .map { JSONObject().put("args", it) }.toList()),
            hides,
            mountState,
            readControlState(meta) { queryCommand(*it) },
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
                                // The controller rechecks saved/enabled state under its
                                // transaction lock, including on legacy kernels.
                                command("hide", "retry-path", path)
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
        // The definition-aware controller refuses disabled saved rules. A
        // direct kernel retry cannot see persistent enabled state.
        command("hide", "retry-path", path)
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

    internal suspend fun syncPartitions() = discoverPartitions()
}
