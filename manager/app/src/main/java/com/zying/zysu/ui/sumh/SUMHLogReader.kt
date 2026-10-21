package com.zying.zysu.ui.sumh

import androidx.compose.runtime.*
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.ensureActive
import kotlin.coroutines.coroutineContext

internal data class SUMHLogReadState(
    val content: String = "",
    val loading: Boolean = false,
    val error: String? = null,
)

private data class LogReadSnapshot(val kernel: Boolean, val state: SUMHLogReadState)

/** Retain a same-source read while refreshing; a cancelled read cannot publish. */
@Composable
internal fun rememberSUMHLogRead(
    active: Boolean,
    kernel: Boolean,
    refresh: Int,
    read: suspend (Boolean) -> String,
): SUMHLogReadState {
    val reader by rememberUpdatedState(read)
    var snapshot by remember { mutableStateOf<LogReadSnapshot?>(null) }
    LaunchedEffect(active, kernel, refresh) {
        if (!active) return@LaunchedEffect
        val previous = snapshot?.takeIf { it.kernel == kernel }?.state?.content.orEmpty()
        snapshot = LogReadSnapshot(kernel, SUMHLogReadState(previous, loading = true))
        try {
            val content = reader(kernel)
            coroutineContext.ensureActive()
            snapshot = LogReadSnapshot(kernel, SUMHLogReadState(content))
        } catch (error: Exception) {
            if (error is CancellationException) throw error
            coroutineContext.ensureActive()
            snapshot = LogReadSnapshot(kernel, SUMHLogReadState(previous, error = error.message ?: error.javaClass.simpleName))
        }
    }
    return snapshot?.takeIf { it.kernel == kernel }?.state ?: SUMHLogReadState(loading = active)
}
