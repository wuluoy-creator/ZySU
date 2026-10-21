package com.zying.zysu.ui.sumh

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.viewModelScope
import com.zying.zysu.ui.sumh.util.*
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.sync.Mutex
import java.io.File
import java.util.UUID

// Activity owners share one private checkpoint. Never queue recovery or mutation
// behind another owner's native call, including the claim-before-dispatch gap.
private val diagnosticPackageCheckpointCoordinator = Mutex()

/** Activity-owned jobs; background completion survives navigation and rotation. */
internal class SUMHDiagnosticPackageOperations(private val savedState: SavedStateHandle = SavedStateHandle()) : ViewModel() {
    private val restored = restoreDiagnosticPackageDraft(savedState)
    var loaded by mutableStateOf(false); private set
    var busy by mutableStateOf(false); private set
    var draft by mutableStateOf(restored.first); private set
    var capabilities by mutableStateOf(SUMHDiagnosticPackageCapabilities()); private set
    var items by mutableStateOf<List<SUMHDiagnosticPackageItem>>(emptyList()); private set
    var preview by mutableStateOf<SUMHDiagnosticPackagePreview?>(null); private set
    var result by mutableStateOf<SUMHDiagnosticPackageResult?>(null); private set
    var error by mutableStateOf(restored.second); private set
    var pending by mutableStateOf<SUMHDiagnosticPackagePending?>(null); private set
    var pendingObservation by mutableStateOf<SUMHDiagnosticPackageResult?>(null); private set
    private var service: SUMHDiagnosticPackagesService? = null
    private var checkpoint: SUMHDiagnosticPackageCheckpoint? = null
    private var epoch = 0L

    fun load(directory: File, service: SUMHDiagnosticPackagesService = NativeSUMHDiagnosticPackagesService(),
        checkpoint: SUMHDiagnosticPackageCheckpoint? = null) {
        if (loaded || busy) return
        this.service = service; this.checkpoint = checkpoint ?: SUMHDiagnosticPackageFile(directory)
        task {
            pending = withContext(Dispatchers.IO) { requireNotNull(this@SUMHDiagnosticPackageOperations.checkpoint).read() }
            pendingObservation = null; loaded = true
            if (restored.second != null) error = restored.second
        }
    }
    fun updateDraft(value: SUMHDiagnosticPackageDraft) {
        if (busy) return
        if (!boundedDiagnosticDraft(value)) { error = "diagnostic_draft_capacity"; return }
        // Bundle contains only four bounded form fields. Package/preview bytes,
        // revision approvals and mutation replay instructions never enter it.
        savedState["diagnostic_package_scenario"] = value.scenario.wireName
        savedState["diagnostic_package_target"] = value.targetId
        savedState["diagnostic_package_operation"] = value.operationId
        savedState["diagnostic_package_include_identifiers"] = value.includeIdentifiers
        draft = value; error = value.validationError
    }
    private fun task(coordinateCheckpoint: Boolean = false, body: suspend (Long) -> Unit): Boolean {
        if (busy) return false
        if (coordinateCheckpoint && !diagnosticPackageCheckpointCoordinator.tryLock()) {
            error = "diagnostic_operation_busy"; return false
        }
        val ticket = ++epoch; busy = true; error = null
        // UNDISPATCHED enters the try/finally even if the owner was already
        // cancelled, so a pre-acquired process-wide permit cannot leak.
        viewModelScope.launch(start = if (coordinateCheckpoint) CoroutineStart.UNDISPATCHED else CoroutineStart.DEFAULT) {
            try { coroutineContext.ensureActive(); body(ticket) }
            catch (cancelled: CancellationException) { if (epoch == ticket) error = "diagnostic_operation_cancelled"; throw cancelled }
            catch (_: Exception) { if (epoch == ticket) error = "diagnostic_operation_failed" }
            catch (_: LinkageError) { if (epoch == ticket) error = "diagnostic_transport_unavailable" }
            finally {
                if (epoch == ticket) busy = false
                if (coordinateCheckpoint) diagnosticPackageCheckpointCoordinator.unlock()
            }
        }
        return true
    }
    private fun adoptPending(value: SUMHDiagnosticPackagePending?) {
        if (pending != value) pendingObservation = null
        pending = value
    }
    private fun publish(ticket: Long, received: SUMHDiagnosticPackageResult) {
        if (epoch != ticket) return
        result = received; error = received.error
    }
    fun refresh(): Boolean {
        if (!loaded) return false
        return task { ticket ->
            val backend = requireNotNull(service)
            capabilities = try { backend.capabilities() }
            catch (cancelled: CancellationException) { throw cancelled }
            catch (_: Exception) { SUMHDiagnosticPackageCapabilities(error = "diagnostic_capabilities_unknown") }
            if (capabilities.list != SUMHDiagnosticPackageSupport.Supported) { error = capabilities.error; return@task }
            val received = backend.list(); publish(ticket, received)
            if (epoch == ticket && received.status == "observed" && received.error == null) items = received.items.toList()
        }
    }
    fun create(): Boolean {
        if (!loaded || busy || pending != null || capabilities.create != SUMHDiagnosticPackageSupport.Supported) return false
        val frozen = draft
        if (frozen.validationError != null) { error = frozen.validationError; return false }
        // Validate the complete bounded request before persisting a mutation hint.
        val id = UUID.randomUUID().toString().replace("-", "")
        try { SUMHDiagnosticPackagesParser.request("create", id, draft = frozen) }
        catch (_: Exception) { error = "diagnostic_request_invalid"; return false }
        return mutate(SUMHDiagnosticPackagePending("create", id, null)) { requireNotNull(service).create(id, frozen) }
    }
    fun inspect(id: String): Boolean {
        if (!loaded || !validDiagnosticPackageId(id) || capabilities.preview != SUMHDiagnosticPackageSupport.Supported) return false
        return task { ticket ->
            val received = requireNotNull(service).preview(id); publish(ticket, received)
            if (epoch == ticket && received.status == "observed" && received.error == null) preview = received.preview
        }
    }
    fun remove(reviewed: SUMHDiagnosticPackagePreview): Boolean {
        if (!loaded || busy || pending != null || capabilities.remove != SUMHDiagnosticPackageSupport.Supported || preview != reviewed) return false
        return mutate(SUMHDiagnosticPackagePending("remove", reviewed.item.id, reviewed.item.revision)) {
            requireNotNull(service).remove(reviewed.item.id, reviewed.item.revision)
        }
    }
    private fun mutate(hint: SUMHDiagnosticPackagePending, change: suspend () -> SUMHDiagnosticPackageResult): Boolean = task(coordinateCheckpoint = true) { ticket ->
        // Neither lifecycle cancellation nor a lost response can skip recording
        // the caller-known identity. Failure to confirm this write forbids dispatch.
        withContext(NonCancellable) {
            try {
                val claimed = withContext(Dispatchers.IO) { requireNotNull(checkpoint).compareAndSet(null, hint) }
                if (!claimed) {
                    adoptPending(withContext(Dispatchers.IO) { requireNotNull(checkpoint).read() })
                    pendingObservation = null; error = "diagnostic_checkpoint_conflict"; return@withContext
                }
                if (epoch != ticket) return@withContext
                adoptPending(hint); pendingObservation = null
            } catch (_: Exception) {
                // A failed directory sync/readback may have replaced the file.
                // Keep the intended ID locally even if a best-effort reread fails.
                adoptPending(hint); pendingObservation = null
                try { withContext(Dispatchers.IO) { requireNotNull(checkpoint).read() }?.let(::adoptPending) } catch (_: Exception) { }
                error = "diagnostic_checkpoint_unconfirmed"; return@withContext
            }
            val received = try { change() }
            catch (_: Exception) { SUMHDiagnosticPackageResult(hint.action, hint.id, error = "diagnostic_result_unknown") }
            catch (_: LinkageError) { SUMHDiagnosticPackageResult(hint.action, hint.id, error = "diagnostic_transport_unavailable") }
            publish(ticket, received)
            // The identity remains recoverable even for a definitive failure.
            // Only explicit querying and acknowledgement clear the checkpoint.
            if (hint.action == "remove" && received.deleted && preview?.item?.id == hint.id) preview = null
        }
    }
    fun recoverPending(): Boolean {
        val hint = pending ?: return false
        if (!loaded || capabilities.preview != SUMHDiagnosticPackageSupport.Supported) return false
        return task(coordinateCheckpoint = true) { ticket ->
            pendingObservation = null
            val current = withContext(Dispatchers.IO) { requireNotNull(checkpoint).read() }
            if (current != hint) {
                adoptPending(current); error = "diagnostic_checkpoint_conflict"; return@task
            }
            val received = requireNotNull(service).preview(hint.id)
            if (epoch == ticket && pending == hint) {
                pendingObservation = received; publish(ticket, received)
                if (received.status == "observed" && received.error == null) preview = received.preview
            }
        }
    }
    fun acknowledgePending(): Boolean {
        val hint = pending ?: return false
        val observed = pendingObservation ?: return false
        if (!loaded || observed.id != hint.id || observed.error != null || observed.status !in setOf("observed", "not_found")) return false
        return task(coordinateCheckpoint = true) {
            withContext(NonCancellable) {
                try {
                    val cleared = withContext(Dispatchers.IO) { requireNotNull(checkpoint).compareAndSet(hint, null) }
                    adoptPending(withContext(Dispatchers.IO) { requireNotNull(checkpoint).read() })
                    pendingObservation = null
                    if (!cleared) error = "diagnostic_checkpoint_conflict"
                } catch (_: Exception) {
                    pendingObservation = null
                    try { adoptPending(withContext(Dispatchers.IO) { requireNotNull(checkpoint).read() }) } catch (_: Exception) { }
                    error = "diagnostic_checkpoint_unconfirmed"
                }
            }
        }
    }
    suspend fun exportReviewed(reviewed: SUMHDiagnosticPackagePreview): SUMHDiagnosticPackageResult {
        if (!loaded || busy || preview != reviewed || capabilities.export != SUMHDiagnosticPackageSupport.Supported)
            return SUMHDiagnosticPackageResult("export", reviewed.item.id, error = "diagnostic_preview_required")
        val ticket = ++epoch; busy = true; error = null
        try {
            val received = requireNotNull(service).export(reviewed.item.id, reviewed.item.revision)
            val checked = if (received.error == null && received.status == "observed" && received.preview?.item == reviewed.item &&
                received.preview?.exportText == reviewed.exportText) received
            else if (received.status == "observed") SUMHDiagnosticPackageResult("export", reviewed.item.id, error = "diagnostic_export_changed")
            else received
            publish(ticket, checked); return checked
        } catch (cancelled: CancellationException) {
            error = "diagnostic_export_cancelled"; throw cancelled
        } catch (_: Exception) {
            return SUMHDiagnosticPackageResult("export", reviewed.item.id, error = "diagnostic_export_unknown").also { publish(ticket, it) }
        } catch (_: LinkageError) {
            return SUMHDiagnosticPackageResult("export", reviewed.item.id, error = "diagnostic_transport_unavailable").also { publish(ticket, it) }
        } finally { if (epoch == ticket) busy = false }
    }
}

private fun boundedDiagnosticDraft(value: SUMHDiagnosticPackageDraft): Boolean = try {
    diagnosticPackageUtf8(value.targetId).size <= 4096 && diagnosticPackageUtf8(value.operationId).size <= 128 &&
        '\u0000' !in value.targetId && '\u0000' !in value.operationId
} catch (_: Exception) { false }
private fun restoreDiagnosticPackageDraft(handle: SavedStateHandle): Pair<SUMHDiagnosticPackageDraft, String?> {
    val names = listOf("diagnostic_package_scenario", "diagnostic_package_target", "diagnostic_package_operation", "diagnostic_package_include_identifiers")
    if (names.none(handle::contains)) return SUMHDiagnosticPackageDraft() to null
    return try {
        val scenario = handle.get<Any?>(names[0]) as? String ?: error("invalid")
        val target = handle.get<Any?>(names[1]) as? String ?: error("invalid")
        val operation = handle.get<Any?>(names[2]) as? String ?: error("invalid")
        val included = handle.get<Any?>(names[3]) as? Boolean ?: error("invalid")
        val value = SUMHDiagnosticPackageDraft(requireNotNull(SUMHDiagnosticScenario.entries.find { it.wireName == scenario }), target, operation, included)
        require(boundedDiagnosticDraft(value))
        value to value.validationError
    } catch (_: Exception) { SUMHDiagnosticPackageDraft() to "diagnostic_draft_restore_invalid" }
}
