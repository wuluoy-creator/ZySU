package com.zying.zysu.ui.sumh

import androidx.compose.runtime.*
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.zying.zysu.ui.sumh.util.*
import kotlinx.coroutines.*
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.util.concurrent.atomic.AtomicBoolean

internal typealias HideSavedRead = suspend () -> SUMHHideSavedDefinitions
internal typealias HideImportSupport = suspend () -> Boolean

/** Owns jobs across Activity recreation; only explicit user actions can dispatch mutations. */
internal class SUMHHideTransferOperations : ViewModel() {
    var draft by mutableStateOf(SUMHHideTransferDraft()); private set
    var loaded by mutableStateOf(false); private set
    var documentBytes by mutableIntStateOf(0); private set
    var busy by mutableStateOf(false); private set
    var applying by mutableStateOf(false); private set
    var error by mutableStateOf<String?>(null); private set
    var review by mutableStateOf<SUMHHideImportReview?>(null); private set
    var confirmedToken by mutableStateOf<String?>(null); private set
    var batch by mutableStateOf<SUMHHideImportBatch?>(null); private set
    var progress by mutableIntStateOf(0); private set
    var exportReady by mutableStateOf<SUMHHideExportArtifact?>(null); private set
    var exportDone by mutableStateOf(false); private set
    private var storage: SUMHHideTransferStorage? = null
    private var persistedFingerprint: String? = null
    private var baseline: SUMHHideSavedDefinitions? = null
    private var parsed: List<SUMHHidePortableDefinition> = emptyList()
    private val stop = AtomicBoolean(false)
    val fingerprint get() = persistedFingerprint
    val reviewed get() = confirmedToken != null && confirmedToken == review?.reviewToken

    fun load(directory: File, suppliedStorage: SUMHHideTransferStorage? = null) {
        if (loaded || busy) return
        if (storage == null) storage = suppliedStorage ?: SUMHHideTransferStorage(directory)
        task {
            val restored = withContext(Dispatchers.IO) { requireNotNull(storage).readDraft() }
            publish(restored); loaded = true
            // No saved preview or previous batch grants permission to execute after recreation.
        }
    }
    private suspend fun publish(value: SUMHHideTransferDraft?) {
        val metadata = withContext(Dispatchers.Default) { value?.fingerprint to (value?.document?.toByteArray(Charsets.UTF_8)?.size ?: 0) }
        draft = value ?: SUMHHideTransferDraft(); persistedFingerprint = metadata.first; documentBytes = metadata.second
    }
    private fun task(block: suspend () -> Unit) {
        if (busy) return
        busy = true; error = null
        viewModelScope.launch {
            try { block() }
            catch (failure: Exception) {
                if (failure is CancellationException) throw failure
                error = failure.message ?: "hide_transfer_unknown"
            } finally { busy = false }
        }
    }
    private suspend fun save(next: SUMHHideTransferDraft, expected: String? = persistedFingerprint) {
        try {
            val saved = withContext(Dispatchers.IO) { requireNotNull(storage).saveDraft(next, expected) }
            publish(saved)
        } catch (failure: Exception) {
            // An unconfirmed atomic write may have reached disk. Reload without approving it.
            confirmedToken = null; review = null
            try { publish(withContext(Dispatchers.IO) { requireNotNull(storage).readDraft() }) }
            catch (_: Exception) { loaded = false }
            throw failure
        }
    }
    fun stage(expected: String?, input: () -> InputStream): Boolean {
        if (!loaded || busy) return false
        task {
            require(persistedFingerprint == expected) { "hide_transfer_draft_changed" }
            val document = withContext(Dispatchers.IO) {
                readSUMHHideDocument(input()).also { importSUMHHideDefinitions(it) }
            }
            require(persistedFingerprint == expected) { "hide_transfer_draft_changed" }
            save(SUMHHideTransferDraft(document = document), expected)
            confirmedToken = null; review = null; baseline = null; parsed = emptyList(); batch = null
        }
        return true
    }
    fun preserve(value: Boolean) {
        if (!loaded) return
        confirmedToken = null
        task { save(draft.copy(preserveImportedEnabled = value)); rebuild() }
    }
    fun choose(row: SUMHHideTransferRow, resolution: SUMHHideTransferResolution) {
        if (!loaded || review?.rows?.none { it == row } != false) return
        confirmedToken = null
        task {
            val id = if (resolution == SUMHHideTransferResolution.ExplicitNewIdentity)
                row.choice.newId ?: withContext(Dispatchers.Default) {
                    newSUMHHideTransferIdentity(row.sourceId, sumhHideTransferDigest(draft.document),
                        (baseline?.rules.orEmpty().map { it.id } + parsed.map { it.id } + draft.choices.values.mapNotNull { it.newId }).toSet())
                } else null
            save(draft.copy(choices = draft.choices + (row.sourceId to SUMHHideTransferChoice(resolution, id))))
            rebuild()
        }
    }
    private suspend fun rebuild() {
        val saved = baseline ?: return
        val snapshot = draft
        review = withContext(Dispatchers.Default) {
            reviewSUMHHideImport(saved.rules, parsed, requireNotNull(saved.revision), requireNotNull(saved.bootId),
                snapshot.choices, snapshot.preserveImportedEnabled, sumhHideTransferDigest(snapshot.document))
        }
    }
    fun preview(read: HideSavedRead) {
        if (!loaded) return
        confirmedToken = null
        task {
            val incoming = withContext(Dispatchers.Default) { importSUMHHideDefinitions(draft.document) }
            val saved = read()
            require(saved.canReviewMutation) { saved.error ?: "hide_import_boot_or_snapshot_unknown" }
            save(draft.copy(baselineRevision = saved.revision, baselineBootId = saved.bootId))
            baseline = saved; parsed = incoming; rebuild()
            confirmedToken = review?.takeIf { it.canApply }?.reviewToken
        }
    }
    fun invalidate() { if (!applying) confirmedToken = null }
    fun requestStop() { stop.set(true) }
    fun apply(read: HideSavedRead, runtime: HideDefinitionRead, support: HideImportSupport,
        change: HideDefinitionChange, finished: () -> Unit = {}) {
        val planned = review ?: return
        val token = confirmedToken ?: return
        if (!loaded || busy || !planned.canApply || planned.steps.isEmpty()) return
        applying = true; stop.set(false); batch = null; progress = 0
        task {
            try {
                require(support()) { "hide_import_capability_not_supported" }
                val current = read()
                require(current.canReviewMutation && current.revision == planned.revision && current.bootId == planned.bootId) { "hide_import_preview_expired" }
                val observed = runtime()
                require(observed.canMutate && observed.managed == true && observed.revision == planned.revision && observed.bootId == planned.bootId) {
                    "hide_import_managed_runtime_unknown"
                }
                require(review?.reviewToken == token && confirmedToken == token) { "hide_import_preview_expired" }
                // Finish the dispatched group after Activity detachment; a stop request is checked before the next item.
                withContext(NonCancellable) {
                    batch = executeSUMHHideImport(planned, token, change, { !stop.get() }, { done, _ -> progress = done })
                }
            } finally {
                applying = false; confirmedToken = null; finished()
                // Original document and choices deliberately remain on disk, including after success.
            }
        }
    }
    fun captureExport(selected: Set<String>?, read: HideSavedRead) {
        val frozenSelection = selected?.toSet()
        task {
            exportDone = false
            val sample = read(); require(sample.canExport) { sample.error ?: "hide_export_incomplete" }
            exportReady = withContext(Dispatchers.IO) {
                val document = if (frozenSelection == null) exportSUMHHideDefinitions(sample.rules)
                    else exportSUMHHideSelectedDefinitions(sample.rules, frozenSelection)
                requireNotNull(storage).saveExport(document, requireNotNull(sample.revision),
                    if (frozenSelection == null) "all" else "selected", frozenSelection?.size ?: sample.rules.size)
            }
        }
    }
    fun consumeExportReady() { exportReady = null }
    fun writeExport(token: String, output: () -> OutputStream): Boolean {
        if (!loaded || busy || storage == null) return false
        task {
            exportDone = false
            try {
                withContext(Dispatchers.IO) {
                    val artifact = requireNotNull(storage).readExport(token)
                    writeSUMHHideDocument(output(), artifact.document)
                }
                exportDone = true
            } catch (failure: Exception) {
                if (failure is CancellationException) throw failure
                throw java.io.IOException("hide_export_write_incomplete: ${failure.message}", failure)
            }
        }
        return true
    }
}
