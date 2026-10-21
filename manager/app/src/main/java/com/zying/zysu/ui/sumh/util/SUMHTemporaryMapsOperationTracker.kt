package com.zying.zysu.ui.sumh.util

import com.zying.zysu.Natives
import com.zying.zysu.ksuApp
import com.zying.zysu.ui.util.getKsud
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction

/** One process owner; checkpoints are confirmed before execute and recovery never executes. */
internal class SUMHTemporaryMapsOperationTracker(private val storage: SUMHTemporaryMapsOperationStorage,
    private val transport: SUMHOperationTransport,
    private val capabilityQuery: suspend () -> SUMHTemporaryMapsSupport) : SUMHTemporaryMapsTrackingService {
    private val mutex = Mutex()
    private val state = MutableStateFlow(SUMHTemporaryMapsTrackingSnapshot())
    override val snapshot = state.asStateFlow()
    private fun reason(failure: Throwable) = sumhOperationError(failure.message ?: "temporary_maps_tracking_unknown")
    private fun read(): List<SUMHTemporaryMapsTrackedOperation> = try {
        storage.read().also { state.value = SUMHTemporaryMapsTrackingSnapshot(true, it) }
    } catch (failure: Exception) {
        state.value = state.value.copy(loaded = false, error = reason(failure)); throw failure
    }
    private fun save(records: List<SUMHTemporaryMapsTrackedOperation>) {
        try {
            storage.write(records)
            state.value = SUMHTemporaryMapsTrackingSnapshot(true, records)
        } catch (failure: Exception) {
            state.value = state.value.copy(error = "temporary_maps_tracking_write_unconfirmed: ${reason(failure)}")
            throw failure
        }
    }
    override suspend fun capabilities(): SUMHTemporaryMapsSupport = withContext(Dispatchers.IO) {
        try { capabilityQuery() }
        catch (cancelled: CancellationException) { throw cancelled }
        catch (_: Exception) { SUMHTemporaryMapsSupport.Unknown }
        catch (_: LinkageError) { SUMHTemporaryMapsSupport.Unknown }
    }
    override suspend fun load(): SUMHTemporaryMapsTrackingSnapshot = withContext(Dispatchers.IO) { mutex.withLock {
        try { read() }
        catch (failure: Exception) { state.value = state.value.copy(loaded = false, error = reason(failure)) }
        state.value
    } }
    private fun request(action: String, id: String? = null, args: List<String>? = null, waitMs: Int? = null) =
        JSONObject().put("schema_version", 2).put("resource", "temporary_maps").put("action", action).apply {
            if (id != null) put("operation_id", id)
            if (args != null) put("input", JSONObject(sumhOperationInput(args)))
            if (waitMs != null) put("timeout_ms", waitMs)
        }.toString().also { require(it.toByteArray(Charsets.UTF_8).size <= 48 * 1024) }
    private suspend fun operationReply(request: String) = transport(request).also {
        SUMHTemporaryMapsOperationParser.validateDocument(it.stdout, SUMH_OPERATION_SUBMISSION_BYTES)
    }

    override suspend fun remove(request: SUMHTemporaryMapsRemoveRequest): SUMHTemporaryMapsTrackedRemoval =
        withContext(NonCancellable + Dispatchers.IO) {
            if (!mutex.tryLock()) return@withContext failure("temporary_maps_tracking_busy_not_started", null, false)
            var tracked: SUMHTemporaryMapsTrackedOperation? = null
            var dispatched = false
            try {
                val key = temporaryMapsOperationKey(request)
                val args = temporaryMapsOperationArguments(key); val revision = sumhOperationRevision(args)
                var records = read()
                if (records.size >= SUMH_TEMPORARY_MAPS_OPERATION_LOCAL_COUNT) {
                    val remove = records.firstOrNull { it.safelyPrunable } ?: error("temporary_maps_tracking_full_review_required")
                    records = records.filterNot { it.id == remove.id }
                }
                val placeholder = (0..SUMH_TEMPORARY_MAPS_OPERATION_LOCAL_COUNT).map { it.toString(16).padStart(32, '0') }
                    .first { candidate -> records.none { it.id == candidate } }
                // Reserve capacity for the largest valid origin boot before making any reservation request.
                val prototype = SUMHTemporaryMapsTrackedOperation(placeholder, revision, "x".repeat(80), key, "prepared")
                encodeSUMHTemporaryMapsOperations(records + prototype.copy(stage = "dispatching"))
                when (capabilities()) {
                    SUMHTemporaryMapsSupport.Supported -> Unit
                    SUMHTemporaryMapsSupport.Unsupported -> return@withContext failure("temporary_maps_tracking_unsupported", null, false)
                    SUMHTemporaryMapsSupport.Unknown -> return@withContext failure("temporary_maps_tracking_capability_unknown", null, false)
                }
                val prepared = SUMHOperationsParser.submission(operationReply(this@SUMHTemporaryMapsOperationTracker.request("prepare", args = args)), revision)
                if (prepared.status != "prepared")
                    return@withContext failure("operation_${prepared.status}: ${prepared.error}", prepared.id, false)
                val preparedId = requireNotNull(prepared.id)
                require(records.none { it.id == preparedId }) { "temporary_maps_operation_id_collision" }
                tracked = prototype.copy(id = preparedId, bootId = prepared.bootId)
                save(records + tracked)
                tracked = tracked.copy(stage = "dispatching")
                save(records + tracked)
                dispatched = true
                val submission = SUMHOperationsParser.submission(operationReply(this@SUMHTemporaryMapsOperationTracker.request("execute", tracked.id, args)),
                    revision, tracked.id, tracked.bootId)
                val received = tracked.copy(stage = "received", response = submission.raw, error = sumhOperationError(submission.error))
                // Validate a complete terminal resource before giving it a definitive received state.
                validateTemporaryMapsTrackedOperation(received)
                val resource = received.resource()
                save(records + received)
                tracked = received
                if (resource != null) SUMHTemporaryMapsTrackedRemoval(resource, tracked.id, true)
                else failure("operation_${submission.status}; query this operation ID", tracked.id, true)
            } catch (problem: Exception) {
                retainFailure(tracked, dispatched, problem)
            } catch (problem: LinkageError) {
                retainFailure(tracked, dispatched, problem)
            } finally { mutex.unlock() }
        }

    private fun failure(message: String, id: String?, dispatched: Boolean): SUMHTemporaryMapsTrackedRemoval {
        val detail = sumhOperationError(message)
        val result = SUMHTemporaryMapsRemoveResult(if (dispatched) SUMHTemporaryMapsRemoveStatus.Unknown else SUMHTemporaryMapsRemoveStatus.NotApplied,
            if (dispatched) "outcome_unknown" else "not_dispatched", attempted = if (dispatched) null else false, error = detail)
        return SUMHTemporaryMapsTrackedRemoval(result, id, dispatched, detail)
    }
    private fun retainFailure(entry: SUMHTemporaryMapsTrackedOperation?, dispatched: Boolean, problem: Throwable): SUMHTemporaryMapsTrackedRemoval {
        val detail = (if (dispatched) "temporary_maps_result_unknown: " else "temporary_maps_not_dispatched: ") + reason(problem)
        if (entry != null) {
            try {
                // A write that landed but was not confirmed is still not permission to execute.
                // Re-read before annotation so a future/corrupt document is never replaced by stale memory.
                val records = storage.read()
                if (records.any { it.id == entry.id }) save(records.map {
                    if (it.id == entry.id) it.copy(stage = if (dispatched) "unknown" else it.stage, error = sumhOperationError(detail)) else it
                })
            } catch (_: Exception) { /* Keep the last confirmed checkpoint or the unconfirmed on-disk bytes. */ }
        }
        state.value = state.value.copy(error = sumhOperationError(detail))
        return failure(detail, entry?.id, dispatched)
    }

    /** Manual query only, including for a checkpoint whose last phase was prepared. */
    override suspend fun recover(id: String, waitMs: Int): Boolean = withContext(Dispatchers.IO) { mutex.withLock {
        require(waitMs in 0..5000)
        try {
            var records = read(); var entry = records.single { it.id == id }
            val observed = SUMHOperationsParser.observation(operationReply(request(if (waitMs == 0) "show" else "wait", id,
                waitMs = waitMs.takeIf { it > 0 })), id, entry.requestRevision, entry.name, entry.bootId)
            entry = entry.copy(observation = observed.raw, error = "")
            records = records.map { if (it.id == id) entry else it }; save(records)
            val result = SUMHTemporaryMapsOperationParser.resourceResult(operationReply(request("result", id)), entry)
            save(records.map { if (it.id == id) it.copy(recovered = result.raw, error = "") else it })
            true
        } catch (cancelled: CancellationException) { throw cancelled }
        catch (problem: Exception) { queryFailure(id, problem); false }
        catch (problem: LinkageError) { queryFailure(id, problem); false }
    } }
    private fun queryFailure(id: String, problem: Throwable) {
        val detail = reason(problem)
        try {
            val records = storage.read()
            if (records.any { it.id == id }) save(records.map { if (it.id == id) it.copy(error = detail) else it })
        } catch (_: Exception) { /* Never reset an unreadable checkpoint to empty. */ }
        state.value = state.value.copy(error = detail)
    }
    /** Forgetting only removes this device's association; it does not cancel or undo anything. */
    override suspend fun forget(id: String): Boolean = withContext(Dispatchers.IO) { mutex.withLock {
        try {
            require(validHideDefinitionId(id)); val records = read()
            require(records.any { it.id == id }); save(records.filterNot { it.id == id }); true
        } catch (cancelled: CancellationException) { throw cancelled }
        catch (problem: Exception) { state.value = state.value.copy(error = reason(problem)); false }
    } }
}

/** A single process owner survives Activity replacement; disk associations survive process replacement. */
internal object SUMHTemporaryMapsTracking {
    // Composing a closed entry point must not require application storage or native initialization.
    // Resolve the noBackup path only when an explicit local load or operation reaches storage.
    private val checkpoint by lazy {
        SUMHTemporaryMapsOperationFile(File(ksuApp.noBackupFilesDir, "temporary-maps-operations"))
    }
    private fun decode(bytes: ByteArray): String {
        require(bytes.size <= 4 * 1024 * 1024)
        return Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString()
    }
    val service: SUMHTemporaryMapsTrackingService by lazy {
        SUMHTemporaryMapsOperationTracker(object : SUMHTemporaryMapsOperationStorage {
            override fun read() = checkpoint.read()
            override fun write(records: List<SUMHTemporaryMapsTrackedOperation>) = checkpoint.write(records)
        },
            transport = { request -> SUMHOperationsParser.dispatch(decode(Natives.sumhOperation(
                getKsud().toByteArray(Charsets.UTF_8), request.toByteArray(Charsets.UTF_8)))) },
            capabilityQuery = {
                val args = JSONArray(listOf("api", "meta", "--control-only")).toString().toByteArray(Charsets.UTF_8)
                SUMHTemporaryMapsOperationParser.capabilities(HideDefinitionsParser.dispatch(
                    decode(Natives.sumhUserHide(getKsud().toByteArray(Charsets.UTF_8), args))))
            })
    }
}
