package com.zying.zysu.ui.sumh

import androidx.activity.ComponentActivity
import androidx.activity.compose.LocalActivity
import androidx.compose.runtime.*
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.compose.viewModel
import com.zying.zysu.ui.sumh.util.SUMHCapabilitiesResult
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

internal typealias SUMHCapabilitiesRead = suspend () -> SUMHCapabilitiesResult

/** One manual read per Activity, including a read detached by navigation. */
internal class SUMHCapabilitiesOperations : ViewModel() {
    var busy by mutableStateOf(false); private set
    var result by mutableStateOf<SUMHCapabilitiesResult?>(null); private set
    private var session: String? = null
    private var epoch = 0L

    fun attach(token: String) {
        if (session == token) return
        session = token; epoch++; result = null
    }

    fun close(token: String) {
        if (session != token) return
        session = null; epoch++; result = null
        // Do not release the single-flight guard before the bounded native call returns.
    }

    fun read(token: String, reader: SUMHCapabilitiesRead): Boolean {
        if (session != token || busy) return false
        val startedEpoch = epoch
        val previous = result ?: SUMHCapabilitiesResult()
        busy = true
        viewModelScope.launch {
            fun attached() = session == token && epoch == startedEpoch
            try {
                val received = reader()
                if (attached()) result = received.retaining(previous)
            } catch (failure: Exception) {
                if (attached()) result = SUMHCapabilitiesResult(error =
                    if (failure is CancellationException) "capability_read_cancelled"
                    else failure.message ?: "capability_read_failed").retaining(previous)
                if (failure is CancellationException) throw failure
            } finally { busy = false }
        }
        return true
    }
}

@Composable
internal fun rememberSUMHCapabilitiesOperations(): SUMHCapabilitiesOperations {
    val activity = requireNotNull(LocalActivity.current as? ComponentActivity)
    return viewModel(viewModelStoreOwner = activity, key = "sumh-capabilities")
}
