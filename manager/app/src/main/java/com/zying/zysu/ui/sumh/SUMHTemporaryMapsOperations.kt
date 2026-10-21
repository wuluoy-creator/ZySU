package com.zying.zysu.ui.sumh

import androidx.activity.ComponentActivity
import androidx.activity.compose.LocalActivity
import androidx.compose.runtime.*
import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.compose.viewModel
import com.zying.zysu.ui.sumh.util.*
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

internal enum class SUMHTemporaryMapsAction { Refresh, Next, Remove, LoadHistory, Recover, Forget }
internal enum class SUMHTemporaryMapsProblem { Unsupported, Unknown, Failed, Stale, Changed }

/** One Activity-owned action. Only recovery markers are saved; rows/tokens/mutations are never replayed. */
internal class SUMHTemporaryMapsOperations(private val savedState: SavedStateHandle) : ViewModel() {
    private val nativeService by lazy { NativeSUMHTemporaryMapsService() }
    private val nativeTracking get() = SUMHTemporaryMapsTracking.service
    var action by mutableStateOf<SUMHTemporaryMapsAction?>(null); private set
    val busy get() = action != null
    var capabilities by mutableStateOf(SUMHTemporaryMapsCapabilities()); private set
    var trackingSupport by mutableStateOf(SUMHTemporaryMapsSupport.Unknown); private set
    var history by mutableStateOf(SUMHTemporaryMapsTrackingSnapshot()); private set
    var historyError by mutableStateOf(false); private set
    var operationId by mutableStateOf<String?>(null); private set
    var snapshot by mutableStateOf<SUMHTemporaryMapsSnapshot?>(null); private set
    var fresh by mutableStateOf(false); private set
    var selected by mutableStateOf<SUMHTemporaryMapsRow?>(null); private set
    var result by mutableStateOf<SUMHTemporaryMapsRemoveResult?>(null); private set
    var problem by mutableStateOf<SUMHTemporaryMapsProblem?>(null); private set
    var queryRequired by mutableStateOf(savedState.get<Boolean>(QUERY_REQUIRED) == true ||
        savedState.get<String>(PENDING) != null); private set
    private var parentEnabled = true
    private var contextEpoch = 0L
    private var selectionEpoch = 0L

    init {
        // A recreated process can only recover uncertainty, never a running coroutine or its arguments.
        savedState[QUERY_REQUIRED] = queryRequired
        savedState.remove<String>(PENDING)
    }

    val canSelect get() = parentEnabled && !busy && fresh && snapshot != null &&
        capabilities.inventory == SUMHTemporaryMapsSupport.Supported &&
        capabilities.delete == SUMHTemporaryMapsSupport.Supported &&
        trackingSupport == SUMHTemporaryMapsSupport.Supported
    val canRemove get() = canSelect && selected != null && result == null &&
        snapshot?.rows?.any { it == selected } == true
    val canNext get() = parentEnabled && !busy && fresh && snapshot?.continuation != null

    fun observeParent(enabled: Boolean) {
        if (enabled == parentEnabled) return
        parentEnabled = enabled
        contextEpoch++
        if (snapshot != null || busy) {
            fresh = false
            requireQuery()
            problem = SUMHTemporaryMapsProblem.Changed
        }
    }

    fun select(row: SUMHTemporaryMapsRow): Boolean {
        if (!canSelect || snapshot?.rows?.contains(row) != true) return false
        selectionEpoch++
        selected = row
        result = null
        operationId = null
        return true
    }

    /** Explicitly hiding a receipt does not restore authorization or cancel an in-flight native call. */
    fun dismissSelection() {
        selectionEpoch++
        selected = null
        result = null
        operationId = null
    }

    fun refresh(service: SUMHTemporaryMapsService = nativeService,
        tracking: SUMHTemporaryMapsTrackingService = nativeTracking): Boolean {
        if (busy || !parentEnabled) return false
        val epoch = contextEpoch
        dismissSelection()
        trackingSupport = SUMHTemporaryMapsSupport.Unknown
        begin(SUMHTemporaryMapsAction.Refresh)
        viewModelScope.launch {
            try {
                val receivedCapabilities = service.capabilities()
                if (epoch != contextEpoch) return@launch
                capabilities = receivedCapabilities
                val receivedTracking = try { tracking.capabilities() } catch (failure: Exception) {
                    if (failure is CancellationException) throw failure
                    SUMHTemporaryMapsSupport.Unknown
                }
                if (epoch != contextEpoch) return@launch
                trackingSupport = receivedTracking
                if (receivedCapabilities.error != null || receivedCapabilities.inventory != SUMHTemporaryMapsSupport.Supported) {
                    problem = if (receivedCapabilities.inventory == SUMHTemporaryMapsSupport.Unsupported && receivedCapabilities.error == null)
                        SUMHTemporaryMapsProblem.Unsupported else SUMHTemporaryMapsProblem.Unknown
                    return@launch
                }
                val received = service.list()
                if (epoch == contextEpoch) acceptPage(null, received)
            } catch (failure: Exception) {
                if (epoch == contextEpoch) problem = SUMHTemporaryMapsProblem.Failed
                if (failure is CancellationException) throw failure
            } finally { finish() }
        }
        return true
    }

    fun next(service: SUMHTemporaryMapsService = nativeService): Boolean {
        if (!canNext) return false
        val previous = requireNotNull(snapshot)
        val cursor = requireNotNull(previous.continuation)
        val epoch = contextEpoch
        begin(SUMHTemporaryMapsAction.Next)
        viewModelScope.launch {
            try {
                val received = service.list(cursor)
                if (epoch == contextEpoch) acceptPage(previous, received)
            } catch (failure: Exception) {
                if (epoch == contextEpoch) problem = SUMHTemporaryMapsProblem.Failed
                if (failure is CancellationException) throw failure
            } finally { finish() }
        }
        return true
    }

    fun remove(tracking: SUMHTemporaryMapsTrackingService = nativeTracking): Boolean {
        if (!canRemove) return false
        val row = requireNotNull(selected)
        val request = SUMHTemporaryMapsRemoveRequest(requireNotNull(snapshot).token, row)
        val selection = selectionEpoch
        begin(SUMHTemporaryMapsAction.Remove)
        viewModelScope.launch {
            try {
                val received = tracking.remove(request)
                if (selectionEpoch == selection && selected == row) {
                    result = received.result
                    operationId = received.operationId
                }
            } catch (failure: Exception) {
                if (selectionEpoch == selection && selected == row) result = SUMHTemporaryMapsRemoveResult(
                    SUMHTemporaryMapsRemoveStatus.Unknown, "outcome_unknown", error = failure.message)
                if (failure is CancellationException) throw failure
            } finally { history = tracking.snapshot.value; finish() }
        }
        return true
    }

    /** Local load and historical queries cannot make an inventory token fresh or dispatch a mutation. */
    fun loadHistory(tracking: SUMHTemporaryMapsTrackingService = nativeTracking): Boolean =
        historyAction(SUMHTemporaryMapsAction.LoadHistory, tracking) { tracking.load(); true }

    fun recover(id: String, waitMs: Int = 0,
        tracking: SUMHTemporaryMapsTrackingService = nativeTracking): Boolean {
        if (id !in history.records.map { it.id } || waitMs !in 0..5000) return false
        return historyAction(SUMHTemporaryMapsAction.Recover, tracking) { tracking.recover(id, waitMs) }
    }

    fun forget(id: String, tracking: SUMHTemporaryMapsTrackingService = nativeTracking): Boolean {
        if (id !in history.records.map { it.id }) return false
        return historyAction(SUMHTemporaryMapsAction.Forget, tracking) { tracking.forget(id) }
    }

    private fun historyAction(next: SUMHTemporaryMapsAction, tracking: SUMHTemporaryMapsTrackingService,
        work: suspend () -> Boolean): Boolean {
        if (busy || !parentEnabled) return false
        action = next
        historyError = false
        viewModelScope.launch {
            try { historyError = !work() } catch (failure: Exception) {
                historyError = true
                if (failure is CancellationException) throw failure
            } finally { history = tracking.snapshot.value; finish() }
        }
        return true
    }

    private fun acceptPage(previous: SUMHTemporaryMapsSnapshot?, received: SUMHTemporaryMapsPageResult) {
        try {
            val updated = appendSUMHTemporaryMapsPage(previous, received)
            snapshot = updated
            fresh = true
            queryRequired = false
            savedState[QUERY_REQUIRED] = false
            problem = null
        } catch (_: Exception) {
            problem = when (received.page?.status) {
                SUMHInventoryPageStatus.Stale -> SUMHTemporaryMapsProblem.Stale
                SUMHInventoryPageStatus.Unsupported -> SUMHTemporaryMapsProblem.Unsupported
                else -> SUMHTemporaryMapsProblem.Failed
            }
        }
    }

    private fun requireQuery() { queryRequired = true; savedState[QUERY_REQUIRED] = true }
    private fun begin(next: SUMHTemporaryMapsAction) {
        action = next
        fresh = false
        problem = null
        requireQuery()
        savedState[PENDING] = next.name
    }
    private fun finish() { action = null; savedState.remove<String>(PENDING) }

    internal companion object {
        const val QUERY_REQUIRED = "temporary_maps_query_required"
        const val PENDING = "temporary_maps_pending"
    }
}

@Composable
internal fun rememberSUMHTemporaryMapsOperations(): SUMHTemporaryMapsOperations {
    val activity = requireNotNull(LocalActivity.current as? ComponentActivity)
    return viewModel(viewModelStoreOwner = activity, key = "sumh-temporary-maps")
}
