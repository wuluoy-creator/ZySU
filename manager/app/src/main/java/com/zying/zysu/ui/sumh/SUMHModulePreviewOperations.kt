package com.zying.zysu.ui.sumh

import androidx.activity.ComponentActivity
import androidx.activity.compose.LocalActivity
import androidx.compose.runtime.*
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.compose.viewModel
import com.zying.zysu.ui.sumh.util.*
import kotlinx.coroutines.*

internal typealias ModulePreviewRead = suspend (SUMHModulePreviewRequest) -> SUMHModulePreviewResult

/** Local context is evidence of changes observed by this editor, not a live kernel revision. */
internal data class SUMHModulePreviewContext(
    val draft: EditorDraftRecord,
    val draftEpoch: Long,
    val savedBaseline: String,
    val globalMode: String,
    val backendAvailable: Boolean,
    val savedStrategy: String,
    val managerRevision: Long,
    val explainPath: String,
    val candidateAllowed: Boolean,
)

/** One bounded observation per Activity. Closing detaches a read; it does not claim native cancellation. */
internal class SUMHModulePreviewOperations : ViewModel() {
    var busy by mutableStateOf(false); private set
    var stale by mutableStateOf(false); private set
    var request by mutableStateOf<SUMHModulePreviewRequest?>(null); private set
    var result by mutableStateOf<SUMHModulePreviewResult?>(null); private set
    private var session: String? = null
    private var moduleId: String? = null
    private var context: SUMHModulePreviewContext? = null
    private var epoch = 0L
    private var requestSequence = 0L

    fun attach(token: String, id: String, current: SUMHModulePreviewContext) {
        if (session != token || moduleId != id) {
            session = token; moduleId = id; context = null; epoch++
            request = null; result = null; stale = false
        }
        observe(token, current)
    }

    fun observe(token: String, current: SUMHModulePreviewContext) {
        if (session != token || current == context) return
        epoch++
        if (request != null || result != null || busy) stale = true
        context = current.copy(draft = EditorDraftRecord(current.draft.fields.toList()))
    }

    fun close(token: String) {
        if (session != token) return
        session = null; moduleId = null; context = null; epoch++
        request = null; result = null; stale = false
        // Keep the single-flight guard until the bounded native call actually returns.
    }

    fun read(token: String, current: SUMHModulePreviewContext, candidate: Boolean,
        reader: ModulePreviewRead): Boolean {
        val id = moduleId ?: return false
        if (session != token || busy || context != current || (candidate && !current.candidateAllowed)) return false
        val frozen = current.copy(draft = EditorDraftRecord(current.draft.fields.toList()))
        val startedEpoch = epoch
        val sequence = ++requestSequence
        busy = true; stale = false; request = null; result = null
        viewModelScope.launch {
            fun attached() = session == token && moduleId == id && requestSequence == sequence
            try {
                val input = withContext(Dispatchers.Default) {
                    if (candidate) ModuleEditorDraft.from(frozen.draft).let {
                        SUMHModulePreviewRequest.candidate(id, it.mode, it.rules, frozen.explainPath)
                    } else SUMHModulePreviewRequest.saved(id, frozen.explainPath)
                }
                if (!attached()) return@launch
                request = input
                if (epoch != startedEpoch) { stale = true; return@launch }
                val received = reader(input)
                if (attached()) {
                    result = received
                    stale = epoch != startedEpoch
                }
            } catch (failure: Exception) {
                if (failure is CancellationException) throw failure
                if (attached()) {
                    result = SUMHModulePreviewResult(error = failure.message ?: "module_preview_unknown")
                    stale = epoch != startedEpoch
                }
            } finally { busy = false }
        }
        return true
    }
}

@Composable
internal fun rememberSUMHModulePreviewOperations(): SUMHModulePreviewOperations {
    val activity = requireNotNull(LocalActivity.current as? ComponentActivity)
    return viewModel(viewModelStoreOwner = activity, key = "sumh-module-preview")
}
