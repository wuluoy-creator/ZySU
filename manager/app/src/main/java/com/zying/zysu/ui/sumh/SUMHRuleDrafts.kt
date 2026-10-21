package com.zying.zysu.ui.sumh

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

internal const val USER_HIDE_DRAFT_ID = "user-hide-v1"
internal const val USER_HIDE_DRAFT_BYTES = 1024 * 1024
internal const val USER_HIDE_BULK_BYTES = 256 * 1024
internal const val USER_HIDE_BULK_LINES = 1024
internal const val USER_HIDE_DRAFT_ENTRIES = 1024

internal data class UserHideDraftSnapshot(
    val add: String = "",
    val bulk: String = "",
    val edits: Map<String, String> = emptyMap(),
    val retryPaths: List<String> = emptyList(),
)

internal enum class UserHideDraftLimit { Line, Bulk, Total }

/** Budgets cover unfinished and invalid input too. Never shorten a path to fit. */
internal fun userHideDraftLimit(value: UserHideDraftSnapshot): UserHideDraftLimit? {
    fun bytes(text: String) = text.toByteArray(Charsets.UTF_8).size
    if (value.add.length >= 4096 || value.edits.any { it.key.length >= 4096 || it.value.length >= 4096 } ||
        value.retryPaths.any { it.length >= 4096 }) return UserHideDraftLimit.Line
    if (value.bulk.length > USER_HIDE_BULK_BYTES) return UserHideDraftLimit.Bulk
    if (bytes(value.add) >= 4096 || value.edits.any { bytes(it.key) >= 4096 || bytes(it.value) >= 4096 } ||
        value.retryPaths.any { bytes(it) >= 4096 }) return UserHideDraftLimit.Line
    if (bytes(value.bulk) > USER_HIDE_BULK_BYTES || value.bulk.lineSequence().take(USER_HIDE_BULK_LINES + 1).count() > USER_HIDE_BULK_LINES)
        return UserHideDraftLimit.Bulk
    if (value.bulk.lineSequence().any { bytes(it) >= 4096 }) return UserHideDraftLimit.Line
    // Reserve every bulk line's retry identity up front, including items that
    // were persisted before a failed apply. A later failure cannot exceed budget.
    val possibleRetries = value.bulk.lineSequence().toSet() + value.retryPaths
    val total = 32L + bytes(value.add) + bytes(value.bulk) + value.edits.entries.sumOf { 8L + bytes(it.key) + bytes(it.value) } +
        possibleRetries.sumOf { 4L + bytes(it) }
    if (value.edits.size > USER_HIDE_DRAFT_ENTRIES || value.retryPaths.size > USER_HIDE_BULK_LINES || total > USER_HIDE_DRAFT_BYTES)
        return UserHideDraftLimit.Total
    return null
}

/** Process-owned memory plus a serialized IO writer. SavedState contains no draft bodies. */
internal class UserHideDrafts(private val scope: CoroutineScope, private val storage: UserHideDraftStorage) {
    var snapshot by mutableStateOf(UserHideDraftSnapshot())
        private set
    var loaded by mutableStateOf(false)
        private set
    var saving by mutableStateOf(false)
        private set
    var storageError by mutableStateOf(false)
        private set
    var storageFailure: Exception? = null
        private set
    var limit by mutableStateOf<UserHideDraftLimit?>(null)
        private set
    private val writes = Channel<UserHideDraftSnapshot>(Channel.CONFLATED)
    private var started = false

    init { retry() }

    fun retry() {
        if (started) {
            if (loaded) { saving = true; writes.trySend(snapshot) }
            return
        }
        started = true
        scope.launch {
            var persisted: UserHideDraftSnapshot? = null
            try {
                snapshot = withContext(Dispatchers.IO) { storage.read() }
                persisted = snapshot
                loaded = true
                storageError = false
                storageFailure = null
                for (next in writes) {
                    try {
                        withContext(NonCancellable + Dispatchers.IO) { storage.write(next) }
                        persisted = next
                        if (next == snapshot) { saving = false; storageError = false; storageFailure = null }
                    } catch (e: Exception) {
                        if (e is CancellationException) throw e
                        storageError = true
                        storageFailure = e
                        saving = false
                    }
                }
            } catch (e: Exception) {
                if (e is CancellationException) throw e
                storageError = true
                storageFailure = e
            } finally {
                if (loaded && persisted != snapshot) {
                    try { withContext(NonCancellable + Dispatchers.IO) { storage.write(snapshot) } }
                    catch (e: Exception) { storageError = true; storageFailure = e }
                }
                started = false
            }
        }
    }

    private fun replace(next: UserHideDraftSnapshot): Boolean {
        if (!loaded) return false // Failed reads must never overwrite stored drafts.
        limit = userHideDraftLimit(next)
        if (limit != null) return false
        if (next != snapshot) {
            snapshot = next
            saving = true
            writes.trySend(next)
        }
        return true
    }

    val add get() = snapshot.add
    val bulk get() = snapshot.bulk
    val retryPaths get() = snapshot.retryPaths
    fun edit(path: String): String = snapshot.edits[path] ?: path
    fun updateAdd(value: String) = replace(snapshot.copy(add = value))
    fun updateBulk(value: String) = replace(snapshot.copy(bulk = value))
    fun update(path: String, value: String) = replace(snapshot.copy(edits =
        if (path == value) snapshot.edits - path else snapshot.edits + (path to value)))
    fun forget(path: String) { replace(snapshot.copy(edits = snapshot.edits - path)) }
    fun finishBulk(value: String, retryPaths: List<String>) = replace(snapshot.copy(bulk = value, retryPaths = retryPaths))

    companion object {
        private var shared: Pair<File, UserHideDrafts>? = null

        /** Re-entering via a new Activity must not read an old disk version ahead of queued writes. */
        @Synchronized fun forDirectory(directory: File): UserHideDrafts {
            shared?.takeIf { it.first == directory }?.let { return it.second }
            return UserHideDrafts(CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate), UserHideDraftFile(directory))
                .also { shared = directory to it }
        }
    }
}

/** Selection changes only affect the filtered list; hidden selections remain intact. */
internal fun toggleVisibleUserHideSelection(selected: Set<String>, visible: Set<String>): Set<String> =
    if (visible.isNotEmpty() && selected.containsAll(visible)) selected - visible else selected + visible

internal fun invertVisibleUserHideSelection(selected: Set<String>, visible: Set<String>): Set<String> =
    (selected - visible) + (visible - selected)
