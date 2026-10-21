package com.zying.zysu.ui.sumh

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.zying.zysu.ui.sumh.util.SUMHDiagnosticPackagePreview
import com.zying.zysu.ui.sumh.util.SUMHDiagnosticPackageResult
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.OutputStream

internal enum class SUMHDiagnosticExportStatus { Idle, Choosing, Writing, Saved, Failed, Lost }

/** Holds the reviewed content only in Activity memory, never in a Bundle or public cache. */
internal class SUMHDiagnosticPackageExport : ViewModel() {
    var status by mutableStateOf(SUMHDiagnosticExportStatus.Idle); private set
    private var reviewed: SUMHDiagnosticPackagePreview? = null
    val busy: Boolean get() = status == SUMHDiagnosticExportStatus.Choosing || status == SUMHDiagnosticExportStatus.Writing

    fun begin(value: SUMHDiagnosticPackagePreview): Boolean {
        if (busy) return false
        reviewed = value
        status = SUMHDiagnosticExportStatus.Choosing
        return true
    }

    fun cancel() {
        if (status != SUMHDiagnosticExportStatus.Choosing) return
        reviewed = null; status = SUMHDiagnosticExportStatus.Idle
    }

    fun lost() {
        if (busy) return
        reviewed = null; status = SUMHDiagnosticExportStatus.Lost
    }

    fun destinationFailed() {
        if (status == SUMHDiagnosticExportStatus.Writing) return
        reviewed = null; status = SUMHDiagnosticExportStatus.Failed
    }

    fun write(
        fetch: suspend (SUMHDiagnosticPackagePreview) -> SUMHDiagnosticPackageResult,
        open: () -> OutputStream,
    ): Boolean {
        val expected = reviewed
        if (status != SUMHDiagnosticExportStatus.Choosing || expected == null) { lost(); return false }
        status = SUMHDiagnosticExportStatus.Writing
        viewModelScope.launch {
            try {
                val reply = fetch(expected)
                val actual = reply.preview
                require(reply.error == null && reply.exitCode == 0 && reply.status == "observed" &&
                    actual != null && actual.item.id == expected.item.id && actual.item.revision == expected.item.revision &&
                    actual.exportText == expected.exportText)
                withContext(Dispatchers.IO) {
                    open().use { output -> output.write(actual.bytes()); output.flush() }
                }
                status = SUMHDiagnosticExportStatus.Saved
            } catch (failure: Exception) {
                status = SUMHDiagnosticExportStatus.Failed
                if (failure is CancellationException) throw failure
            } finally { reviewed = null }
        }
        return true
    }
}

/** Lossless lazy text chunks: keep the export string intact, including its last LF. */
internal fun diagnosticPackageTextChunks(text: String, limit: Int = 2048): List<String> {
    require(limit >= 2)
    val chunks = mutableListOf<String>()
    var start = 0
    while (start < text.length) {
        var end = minOf(text.length, start + limit)
        if (end < text.length && text[end - 1].isHighSurrogate() && text[end].isLowSurrogate()) end--
        chunks += text.substring(start, end); start = end
    }
    return chunks
}
