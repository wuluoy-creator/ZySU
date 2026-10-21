package com.zying.zysu.ui.sumh.util

import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONObject

internal data class SUMHTrackingSnapshot(val loaded: Boolean = false,
    val records: List<SUMHTrackedOperation> = emptyList(), val error: String? = null)
internal typealias SUMHOperationTransport = suspend (String) -> SUMHQueryReply
internal fun sumhOperationError(value: String): String {
    var used = 0
    return buildString {
        val input = value.replace('\u0000', '\uFFFD').toByteArray(Charsets.UTF_8).toString(Charsets.UTF_8)
        val iterator = input.codePoints().iterator()
        while (iterator.hasNext()) {
            val point = String(Character.toChars(iterator.nextInt()))
            used += point.toByteArray(Charsets.UTF_8).size
            if (used > 1500) break
            append(point)
        }
    }
}

/** One process owner. The disk checkpoint is confirmed before any execute call; recovery never executes. */
internal class SUMHOperationTracker(private val storage: SUMHOperationStorage, private val transport: SUMHOperationTransport) {
    private val mutex = Mutex()
    private val state = MutableStateFlow(SUMHTrackingSnapshot())
    val snapshot = state.asStateFlow()
    private fun failure(error: Exception) = sumhOperationError(error.message ?: "operation_unknown")
    private fun read(): List<SUMHTrackedOperation> = storage.read().also {
        state.value = SUMHTrackingSnapshot(true, it)
    }
    private fun save(records: List<SUMHTrackedOperation>) {
        try {
            storage.write(records)
            state.value = SUMHTrackingSnapshot(true, records)
        } catch (error: Exception) {
            state.value = state.value.copy(error = "operation_tracking_write_unconfirmed: ${failure(error)}")
            throw error
        }
    }
    suspend fun load(): SUMHTrackingSnapshot = withContext(Dispatchers.IO) { mutex.withLock {
        try { read() } catch (error: Exception) { state.value = state.value.copy(loaded = false, error = failure(error)) }
        state.value
    } }
    private fun request(action: String, id: String? = null, args: List<String>? = null, wait: Int? = null): String =
        JSONObject().put("schema_version", 1).put("action", action).apply {
            if (id != null) put("operation_id", id)
            if (args != null) put("input", JSONObject(sumhOperationInput(args)))
            if (wait != null) put("timeout_ms", wait)
        }.toString().also { require(it.toByteArray(Charsets.UTF_8).size <= 48 * 1024) { "operation_input_capacity" } }

    suspend fun changeHide(args: List<String>, configRevision: String, bootId: String): HideMutationResult = withContext(NonCancellable + Dispatchers.IO) {
        if (!mutex.tryLock()) return@withContext HideMutationResult(error = "operation_tracking_busy_not_started")
        var tracked: SUMHTrackedOperation? = null
        var executeAttempted = false
        try {
            require(args.size >= 3 && args[0] == "hide" && validHideDefinitionRevision(configRevision) && bootId.isNotEmpty())
            val action = args[1]; val selector = if (action == "create") "" else args[2]
            require(args.getOrNull(if (action == "create") 2 else 3) == configRevision)
            val requestRevision = sumhOperationRevision(args)
            var records = read()
            val placeholder = (0..SUMH_OPERATION_LOCAL_COUNT).map { it.toString(16).padStart(32, '0') }.first { candidate -> records.none { it.id == candidate } }
            val prototype = SUMHTrackedOperation(placeholder, requestRevision, bootId, action, selector, configRevision, "prepared")
            validateTrackedOperation(prototype)
            if (records.size >= SUMH_OPERATION_LOCAL_COUNT) {
                val remove = records.firstOrNull { it.safelyPrunable } ?: error("operation_tracking_full_review_required")
                records = records.filterNot { it.id == remove.id }
            }
            // Reject local capacity before even reserving a server ID. No input is persisted for replay.
            encodeSUMHTrackedOperations(records + prototype)
            val prepare = request("prepare", args = args)
            val prepared = SUMHOperationsParser.submission(transport(prepare), requestRevision, expectedBoot = bootId)
            if (prepared.status != "prepared") return@withContext HideMutationResult(error = "operation_${prepared.status}: ${prepared.error}")
            tracked = prototype.copy(id = requireNotNull(prepared.id))
            save(records + tracked)
            tracked = tracked.copy(stage = "dispatching")
            save(records + tracked)
            // No cancellation window can cause this owner to retry. A recovered owner only queries the known ID.
            executeAttempted = true
            val receipt = SUMHOperationsParser.submission(transport(request("execute", tracked.id, args)), requestRevision, tracked.id, bootId)
            tracked = tracked.copy(stage = "received", response = receipt.raw, error = sumhOperationError(receipt.error))
            save(records + tracked)
            tracked.resource()?.copy(operationId = tracked.id) ?: HideMutationResult(error = "operation_${receipt.status}; query this ID before retrying", operationId = tracked.id)
        } catch (error: Exception) {
            val reason = if (executeAttempted) "operation_result_unknown: ${failure(error)}" else "operation_not_dispatched: ${failure(error)}"
            val retained = tracked
            if (retained != null) {
                // Read again before best-effort error annotation; never overwrite an unreadable/future document.
                try {
                    val current = storage.read()
                    if (current.any { it.id == retained.id }) save(current.map {
                        if (it.id == retained.id) it.copy(stage = if (executeAttempted) "unknown" else it.stage, error = sumhOperationError(reason)) else it
                    })
                } catch (_: Exception) { /* The previous confirmed checkpoint remains the recovery authority. */ }
            }
            HideMutationResult(error = reason, operationId = retained?.id)
        } finally { mutex.unlock() }
    }

    suspend fun observe(id: String, waitMs: Int = 0): SUMHOperationObservation? = withContext(Dispatchers.IO) { mutex.withLock {
        require(waitMs in 0..5000)
        try {
            val records = read(); val entry = records.single { it.id == id }
            val reply = transport(request(if (waitMs == 0) "show" else "wait", id, wait = waitMs.takeIf { it > 0 }))
            val observed = SUMHOperationsParser.observation(reply, id, entry.requestRevision, entry.name, entry.bootId)
            save(records.map { if (it.id == id) it.copy(observation = observed.raw, error = "") else it })
            observed
        } catch (error: Exception) {
            if (error is CancellationException) throw error
            state.value = state.value.copy(error = failure(error)); null
        }
    } }

    /** Explicit recovery reads the exact reservation, never replays execute or infers results from current configuration. */
    suspend fun recover(id: String): Boolean = withContext(Dispatchers.IO) { mutex.withLock {
        try {
            var records = read(); var entry = records.single { it.id == id }
            val history = SUMHOperationsParser.observation(transport(request("show", id)), id, entry.requestRevision, entry.name, entry.bootId)
            entry = entry.copy(observation = history.raw, error = "")
            records = records.map { if (it.id == id) entry else it }; save(records)
            // The result query is independently bound by the server and parser, including concurrent pruning.
            val result = SUMHOperationsParser.resourceResult(transport(request("result", id)), entry)
            save(records.map { if (it.id == id) it.copy(recovered = result.raw, error = "") else it })
            true
        } catch (error: Exception) {
            if (error is CancellationException) throw error
            val message = failure(error)
            try {
                val records = storage.read()
                save(records.map { if (it.id == id) it.copy(error = message) else it })
            } catch (_: Exception) { /* Keep the last confirmed association. */ }
            state.value = state.value.copy(error = message); false
        }
    } }

    /** Removing a local association is explicit; it neither cancels the server command nor edits its history. */
    suspend fun acknowledge(id: String): Boolean = withContext(Dispatchers.IO) { mutex.withLock {
        try { save(read().filterNot { it.id == id }); true }
        catch (error: Exception) { state.value = state.value.copy(error = failure(error)); false }
    } }
}
