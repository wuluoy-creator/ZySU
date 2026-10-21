package com.zying.zysu.ui.sumh

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.zying.zysu.ui.sumh.util.*
import kotlinx.coroutines.*
import kotlinx.coroutines.sync.Mutex
import java.io.File
import java.io.InputStream
import java.io.OutputStream

private val profileCheckpointCoordinator = Mutex()

/** Activity owned. Only a caller-known recovery hint is persisted; no automatic request replay. */
internal class SUMHProfileOperations(private val savedState: SavedStateHandle) : ViewModel() {
    var loaded by mutableStateOf(false); private set
    var busy by mutableStateOf(false); private set
    var capabilities by mutableStateOf(SUMHProfileCapabilities()); private set
    var items by mutableStateOf<List<SUMHProfileItem>>(emptyList()); private set
    var bankRevision by mutableStateOf<String?>(null); private set
    var detail by mutableStateOf<SUMHProfileResult?>(null); private set
    var preview by mutableStateOf<SUMHProfileResult?>(null); private set
    var result by mutableStateOf<SUMHProfileResult?>(null); private set
    var error by mutableStateOf<String?>(null); private set
    var form by mutableStateOf(restoreForm()); private set
    var draft by mutableStateOf(SUMHProfileDraft()); private set
    var importedDocument by mutableStateOf<SUMHProfileDocument?>(null); private set
    var pending by mutableStateOf<SUMHProfilePending?>(null); private set
    var pendingObservation by mutableStateOf<SUMHProfileResult?>(null); private set
    var exportArtifact by mutableStateOf<SUMHProfileExport?>(null); private set
    var exportStatus by mutableStateOf("idle"); private set
    private var service: SUMHProfilesService? = null
    private var storage: SUMHProfileStorage? = null
    var draftFingerprint by mutableStateOf<String?>(null); private set
    var draftNeedsReload by mutableStateOf(false); private set
    private var reviewedDraftFingerprint: String? = null
    private var draftStorageFailure = false
    private var checkpointStorageFailure = false
    private var exportStorageFailure = false
    var listFresh by mutableStateOf(false); private set
    private var epoch = 0L

    private fun restoreForm(): SUMHProfileForm = try {
        val fields = listOf("sumh_profile_id", "sumh_profile_name", "sumh_profile_note")
        val values = fields.map { if (savedState.contains(it)) savedState.get<Any?>(it) as? String ?: error("profile_draft_invalid") else "" }
        SUMHProfileForm(values[0], values[1], values[2]).also { require(boundedForm(it)) }
    } catch (_: Exception) { SUMHProfileForm() }
    private fun boundedForm(value: SUMHProfileForm) = validSUMHProfileText(value.id, 64) && validSUMHProfileText(value.name, 128) && validSUMHProfileText(value.note, 1024)
    fun updateForm(value: SUMHProfileForm) {
        if (busy) return
        if (!boundedForm(value)) { error = "profile_form_capacity"; return }
        savedState["sumh_profile_id"] = value.id; savedState["sumh_profile_name"] = value.name; savedState["sumh_profile_note"] = value.note
        form = value
    }
    fun reportError(code: String) { error = code.take(128) }
    fun load(directory: File, service: SUMHProfilesService? = null, storage: SUMHProfileStorage? = null) {
        if (loaded || busy) return
        this.service = service ?: NativeSUMHProfilesService(directory)
        this.storage = storage ?: SUMHProfileStorage(directory)
        task { _ ->
            // Failure of one slot cannot make a corrupt checkpoint look absent.
            val restored = withContext(Dispatchers.IO) {
                val store = requireNotNull(this@SUMHProfileOperations.storage)
                Triple(runCatching { store.readDraft()?.also { it.fingerprint } }, runCatching { store.readPending() }, runCatching { store.readExport() })
            }
            restored.first.onSuccess { value -> if (value != null) {
                draft = value; draftFingerprint = value.fingerprint
                importedDocument = withContext(Dispatchers.Default) { try { SUMHProfilesParser.document(value.document) } catch (_: Exception) { null } }
            } }.onFailure { draftStorageFailure = true; draftNeedsReload = true; error = "profile_draft_unavailable" }
            restored.second.onSuccess { pending = it }.onFailure { checkpointStorageFailure = true; error = "profile_checkpoint_unavailable" }
            restored.third.onSuccess { exportArtifact = it }.onFailure { exportStorageFailure = true; error = "profile_export_unavailable" }
            // Restored export is available for an explicit retry, never automatically opens a picker.
            exportStatus = "idle"; loaded = true
        }
    }
    private fun task(coordinate: Boolean = false, body: suspend (Long) -> Unit): Boolean {
        if (busy) return false
        if (coordinate && !profileCheckpointCoordinator.tryLock()) { error = "profile_operation_busy"; return false }
        val ticket = ++epoch; busy = true; error = null
        viewModelScope.launch(start = if (coordinate) CoroutineStart.UNDISPATCHED else CoroutineStart.DEFAULT) {
            try { coroutineContext.ensureActive(); body(ticket) }
            catch (cancelled: CancellationException) { if (ticket == epoch) error = "profile_operation_cancelled"; throw cancelled }
            catch (_: Exception) { if (ticket == epoch) error = "profile_operation_failed" }
            catch (_: LinkageError) { if (ticket == epoch) error = "profile_transport_unavailable" }
            finally { if (ticket == epoch) busy = false; if (coordinate) profileCheckpointCoordinator.unlock() }
        }
        return true
    }
    private fun publish(ticket: Long, received: SUMHProfileResult) { if (ticket == epoch) {
        result = received; error = received.error ?: if (!received.complete) received.reason else null
        if (received.action in setOf("show", "preview", "export") && !received.complete || received.status == "stale" ||
            received.reason in setOf("bank_revision_conflict", "profile_revision_conflict", "bank_changed")) listFresh = false
    } }
    private suspend fun query(request: SUMHProfileRequest): SUMHProfileResult = try { requireNotNull(service).execute(request) }
    catch (cancelled: CancellationException) {
        if (request.action in setOf("show", "preview", "export")) listFresh = false
        throw cancelled
    } catch (_: Exception) { SUMHProfileResult(request.action, request.id, error = "profile_result_unknown") }
    catch (_: LinkageError) { SUMHProfileResult(request.action, request.id, error = "profile_transport_unavailable") }
    private fun adoptList(received: SUMHProfileResult) {
        val selected = detail
        if (selected != null && (selected.bankRevision != received.bankRevision || received.items.none { it.id == selected.id && it.revision == selected.profileRevision })) {
            detail = null; preview = null; reviewedDraftFingerprint = null
        }
        items = received.items.toList(); bankRevision = received.bankRevision; listFresh = true
    }
    private fun supports(action: String): Boolean {
        if (!loaded || !capabilities.supports(action)) { error = "profile_capability_unavailable"; return false }; return true
    }
    fun refresh(): Boolean {
        if (!loaded) return false
        return task { ticket ->
            listFresh = false
            capabilities = requireNotNull(service).capabilities()
            if (!capabilities.supports("list")) { error = capabilities.error ?: "profile_list_unsupported"; return@task }
            val received = query(SUMHProfileRequest("list")); publish(ticket, received)
            if (received.complete) adoptList(received)
        }
    }
    fun inspect(item: SUMHProfileItem): Boolean {
        if (!supports("show")) return false
        val bank = bankRevision ?: return false
        if (items.none { it == item }) { error = "profile_list_changed"; return false }
        return task { ticket -> val received = query(SUMHProfileRequest("show", item.id, bank, item.revision)); publish(ticket, received)
            if (received.complete && received.document != null) { detail = received; preview = null } }
    }
    private fun selectedRequest(action: String): SUMHProfileRequest? {
        val selected = detail ?: return null
        if (!selected.complete || selected.document == null || selected.bankRevision == null || selected.profileRevision == null || selected.id == null) return null
        return SUMHProfileRequest(action, selected.id, selected.bankRevision, selected.profileRevision)
    }
    fun setResources(resources: Set<SUMHProfileResource>): Boolean {
        if (!loaded || draftStorageFailure) return false
        val value = draft.copy(resources = resources.toSet())
        return persistDraft { value }
    }
    private fun persistDraft(create: suspend () -> SUMHProfileDraft): Boolean = task { _ ->
        val value = create()
        val saved = try { withContext(Dispatchers.IO) { requireNotNull(storage).saveDraft(value, draftFingerprint).also { it.fingerprint } } }
        catch (cancelled: CancellationException) { draftNeedsReload = true; throw cancelled }
        catch (_: Exception) { draftNeedsReload = true; error = "profile_draft_readback_required"; return@task }
        draft = saved; draftFingerprint = saved.fingerprint; preview = null; reviewedDraftFingerprint = null
        draftNeedsReload = false
        importedDocument = withContext(Dispatchers.Default) { try { SUMHProfilesParser.document(saved.document) } catch (_: Exception) { null } }
    }
    fun stageImport(open: () -> InputStream): Boolean {
        if (!loaded || draftStorageFailure) return false
        return persistDraft { withContext(Dispatchers.IO) {
            val text = profileDecode(readSUMHProfileBytes(open(), SUMH_PROFILE_BYTES))
            SUMHProfilesParser.document(text) // Structural preflight only. Native semantic preview stays explicit.
            draft.copy(document = text)
        } }
    }
    fun discardImport(): Boolean {
        if (!loaded || draftStorageFailure) return false
        return persistDraft { draft.copy(document = "") }
    }
    fun reloadDraft(): Boolean {
        if (!loaded) return false
        return task { _ ->
            val observed = withContext(Dispatchers.IO) {
                val stored = requireNotNull(storage).readDraft()
                val imported = stored?.takeIf { it.document.isNotEmpty() }?.let { SUMHProfilesParser.document(it.document) }
                stored?.fingerprint
                stored to imported
            }
            draft = observed.first ?: SUMHProfileDraft(); draftFingerprint = observed.first?.fingerprint
            importedDocument = observed.second; preview = null; reviewedDraftFingerprint = null
            draftNeedsReload = false; draftStorageFailure = false
        }
    }
    fun previewImport(): Boolean {
        if (!supports("import-preview") || draft.resources.isEmpty()) return false
        val frozen = draft; val request = SUMHProfileRequest("import-preview", document = frozen.document, resources = frozen.resources)
        return task { ticket ->
            withContext(Dispatchers.Default) { SUMHProfilesParser.request(request) }
            val received = query(request); publish(ticket, received)
            if (frozen.fingerprint == draft.fingerprint) { preview = received; reviewedDraftFingerprint = if (received.complete && received.diff != null) frozen.fingerprint else null } }
    }
    fun previewSelected(): Boolean {
        if (!supports("preview") || draft.resources.isEmpty()) return false
        val request = selectedRequest("preview")?.copy(resources = draft.resources.toSet()) ?: return false
        return task { ticket -> val received = query(request); publish(ticket, received); preview = received; reviewedDraftFingerprint = null }
    }
    private fun mutationAllowed(action: String): Boolean {
        if (!supports(action) || busy) return false
        if (checkpointStorageFailure) { error = "profile_storage_unavailable"; return false }
        if (!listFresh) { error = "profile_refresh_required"; return false }
        if (pending != null) { error = "profile_pending_observation_required"; return false }
        return true
    }
    fun capture(): Boolean {
        if (!mutationAllowed("capture")) return false
        if (form.error != null) { error = form.error; return false }
        return mutate(SUMHProfileRequest("capture", form.id, bankRevision ?: return false, name = form.name, note = form.note))
    }
    fun importProfile(): Boolean {
        if (!mutationAllowed("import")) return false
        if (draftStorageFailure || draftNeedsReload) { error = "profile_draft_unavailable"; return false }
        if (!validSUMHProfileId(form.id)) { error = "profile_id_invalid"; return false }
        if (reviewedDraftFingerprint != draft.fingerprint || preview?.action != "import-preview" || preview?.complete != true) {
            error = "profile_import_preview_required"; return false }
        return mutate(SUMHProfileRequest("import", form.id, bankRevision ?: return false, document = draft.document))
    }
    fun cloneSelected(): Boolean {
        if (!mutationAllowed("clone")) return false
        if (form.error != null) { error = form.error; return false }
        val source = selectedRequest("clone") ?: return false
        return mutate(source.copy(id = form.id, sourceId = source.id, name = form.name, note = form.note))
    }
    fun renameSelected(): Boolean {
        if (!mutationAllowed("rename")) return false
        if (!validSUMHProfileText(form.name, 128, true) || !validSUMHProfileText(form.note, 1024)) { error = "profile_metadata_invalid"; return false }
        return mutate((selectedRequest("rename") ?: return false).copy(name = form.name, note = form.note))
    }
    fun removeSelected(): Boolean = mutationAllowed("remove") && selectedRequest("remove")?.let(::mutate) == true
    private fun mutate(request: SUMHProfileRequest): Boolean {
        return task(coordinate = true) { ticket ->
            val hint = withContext(Dispatchers.Default) {
                val encoded = SUMHProfilesParser.request(request)
                SUMHProfilePending(request.action, requireNotNull(request.id), request.sourceId, requireNotNull(request.bankRevision),
                    request.profileRevision, profileDigest(encoded))
            }
            withContext(NonCancellable) {
            try {
                if (!withContext(Dispatchers.IO) { requireNotNull(storage).comparePending(null, hint) }) {
                    pending = withContext(Dispatchers.IO) { requireNotNull(storage).readPending() }; error = "profile_checkpoint_conflict"; return@withContext
                }
                pending = hint; pendingObservation = null
                val dispatching = hint.copy(phase = "dispatching")
                require(withContext(Dispatchers.IO) { requireNotNull(storage).comparePending(hint, dispatching) })
                pending = dispatching
            } catch (_: Exception) {
                pending = pending ?: hint
                try { withContext(Dispatchers.IO) { requireNotNull(storage).readPending() }?.let { pending = it } } catch (_: Exception) { }
                error = "profile_checkpoint_unconfirmed"; return@withContext
            }
            val received = try { requireNotNull(service).execute(request) }
            catch (_: Exception) { SUMHProfileResult(request.action, request.id, error = "profile_result_unknown") }
            catch (_: LinkageError) { SUMHProfileResult(request.action, request.id, error = "profile_transport_unavailable") }
            publish(ticket, received)
            // No result, including saved, removes the identity hint without explicit observation.
            preview = null; reviewedDraftFingerprint = null; listFresh = false
            if (received.complete) { bankRevision = received.bankRevision; if (request.action in setOf("rename", "remove")) detail = null }
        } }
    }
    fun observePending(): Boolean {
        val hint = pending ?: return false
        if (!supports("list")) return false
        return task(coordinate = true) { ticket ->
            pendingObservation = null
            val current = withContext(Dispatchers.IO) { requireNotNull(storage).readPending() }
            if (current != hint) { pending = current; error = "profile_checkpoint_conflict"; return@task }
            val listing = query(SUMHProfileRequest("list")); publish(ticket, listing)
            if (!listing.complete || listing.bankRevision == null) return@task
            adoptList(listing)
            val target = listing.items.find { it.id == hint.id }
            if (target == null) {
                pendingObservation = SUMHProfileResult("observe", hint.id, "missing", "current_target_missing", listing.bankRevision)
            } else if (capabilities.supports("show")) {
                val received = query(SUMHProfileRequest("show", target.id, listing.bankRevision, target.revision))
                publish(ticket, received)
                if (received.complete && received.document != null) { pendingObservation = received; detail = received }
            }
        }
    }
    fun acknowledgePending(): Boolean {
        val hint = pending ?: return false; val observation = pendingObservation ?: return false
        if (!loaded || observation.id != hint.id || observation.error != null || observation.status !in setOf("complete", "missing")) return false
        return task(coordinate = true) { _ -> withContext(NonCancellable) {
            try {
                require(withContext(Dispatchers.IO) { requireNotNull(storage).comparePending(hint, null) })
                pending = withContext(Dispatchers.IO) { requireNotNull(storage).readPending() }; pendingObservation = null
                if (pending != null) error = "profile_checkpoint_conflict"
            } catch (_: Exception) { pendingObservation = null; error = "profile_checkpoint_unconfirmed" }
        } }
    }
    fun prepareExport(): Boolean {
        if (!supports("export")) return false
        if (exportStorageFailure) { error = "profile_export_unavailable"; return false }
        val request = selectedRequest("export") ?: return false; val reviewed = detail?.document ?: return false
        return task { ticket ->
            val received = query(request); publish(ticket, received)
            if (!received.complete || !withContext(Dispatchers.Default) { received.document == reviewed }) { error = "profile_export_changed"; return@task }
            val frozen = withContext(Dispatchers.IO) { requireNotNull(storage).saveExport(requireNotNull(received.document), requireNotNull(received.id), requireNotNull(received.profileRevision)) }
            exportArtifact = frozen; exportStatus = "ready"
        }
    }
    fun writeExport(token: String, open: () -> OutputStream): Boolean {
        if (!loaded || busy) return false
        return task { _ ->
            exportStatus = "writing"
            try {
                withContext(Dispatchers.IO) {
                    val frozen = requireNotNull(requireNotNull(storage).readExport(token))
                    require(frozen == exportArtifact); val bytes = profileUtf8(frozen.document)
                    open().use { it.write(bytes); it.flush() }
                }
                exportStatus = "saved"
            } catch (cancelled: CancellationException) { exportStatus = "failed"; throw cancelled }
            catch (_: Exception) { exportStatus = "failed"; error = "profile_export_write_failed" }
        }
    }
    fun retryExport(): Boolean {
        val expected = exportArtifact ?: return false
        if (!loaded || busy) return false
        return task { _ ->
            val frozen = withContext(Dispatchers.IO) { requireNotNull(requireNotNull(storage).readExport(expected.token)).also { require(it == expected) } }
            exportArtifact = frozen; exportStatus = "ready"
        }
    }
    fun exportCancelled() { if (exportStatus != "writing") exportStatus = "idle" }
}
