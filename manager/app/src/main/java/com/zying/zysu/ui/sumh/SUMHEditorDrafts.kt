package com.zying.zysu.ui.sumh

import androidx.compose.runtime.*
import androidx.compose.ui.platform.LocalContext
import com.zying.zysu.ui.sumh.util.SUMHManager.ModuleRule
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter.MapsPathRule
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.Channel
import java.io.File
import java.security.MessageDigest

/** Main-thread process owner. All Activities mutate the latest shared document. */
internal class SUMHEditorDrafts(private val scope: CoroutineScope, private val storage: EditorDraftStorage) {
    var snapshot by mutableStateOf<EditorDraftSnapshot>(emptyMap()); private set
    /** In-memory epoch detects edits that return to identical bytes before the next composition. */
    var editEpoch by mutableLongStateOf(0L); private set
    var loaded by mutableStateOf(false); private set
    var saving by mutableStateOf(false); private set
    var storageError by mutableStateOf(false); private set
    var limitReached by mutableStateOf(false); private set
    var storageFailure: Exception? = null; private set
    private val writes = Channel<EditorDraftSnapshot>(Channel.CONFLATED)
    private var started = false
    init { retry() }

    fun retry() {
        if (started) { if (loaded) { saving = true; writes.trySend(snapshot) }; return }
        started = true
        scope.launch {
            var persisted: EditorDraftSnapshot? = null
            try {
                snapshot = withContext(Dispatchers.IO) { storage.read() }
                persisted = snapshot; loaded = true; storageError = false; storageFailure = null
                for (next in writes) {
                    try {
                        withContext(NonCancellable + Dispatchers.IO) { storage.write(next) }
                        persisted = next
                        if (next == snapshot) { saving = false; storageError = false; storageFailure = null }
                    } catch (e: Exception) {
                        if (e is CancellationException) throw e
                        storageError = true; storageFailure = e; saving = false
                    }
                }
            } catch (e: Exception) {
                if (e is CancellationException) throw e
                storageError = true; storageFailure = e
            } finally {
                if (loaded && persisted != snapshot) {
                    try { withContext(NonCancellable + Dispatchers.IO) { storage.write(snapshot) } }
                    catch (e: Exception) { storageError = true; storageFailure = e }
                }
                started = false
            }
        }
    }

    fun ensure(key: String, initial: EditorDraftRecord): Boolean =
        if (key in snapshot) true else change(key) { initial }

    fun change(key: String, transform: (EditorDraftRecord?) -> EditorDraftRecord?): Boolean {
        if (!loaded) return false
        val nextRecord = transform(snapshot[key])
        if (nextRecord != null && !validEditorDraftRecord(key, nextRecord)) { limitReached = true; return false }
        val next = if (nextRecord == null) snapshot - key else snapshot + (key to nextRecord)
        limitReached = !editorDraftWithinBudget(next)
        if (limitReached) return false // Preserve the whole previous input, never truncate.
        if (next != snapshot) { editEpoch++; snapshot = next; saving = true; writes.trySend(next) }
        return true
    }

    /** An older Activity's completion must not erase a newer edit. */
    fun forget(key: String, expected: EditorDraftRecord) = change(key) { if (it == expected) null else it }

    companion object {
        private var shared: Pair<File, SUMHEditorDrafts>? = null
        @Synchronized fun forDirectory(directory: File): SUMHEditorDrafts {
            shared?.takeIf { it.first == directory }?.let { return it.second }
            return SUMHEditorDrafts(CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate), EditorDraftFile(directory))
                .also { shared = directory to it }
        }
    }
}

@Composable internal fun rememberSUMHEditorDrafts(): SUMHEditorDrafts {
    val directory = File(LocalContext.current.applicationContext.noBackupFilesDir, "sumh-drafts")
    return remember(directory) { SUMHEditorDrafts.forDirectory(directory) }
}

internal fun draftFingerprint(fields: List<String>): String {
    val digest = MessageDigest.getInstance("SHA-256")
    fields.forEach { field ->
        val bytes = field.toByteArray(Charsets.UTF_8)
        digest.update(java.nio.ByteBuffer.allocate(4).putInt(bytes.size).array()); digest.update(bytes)
    }
    return digest.digest().joinToString("") { "%02x".format(it) }
}

internal data class ModuleEditorDraft(
    val mode: String, val rules: List<ModuleRule>, val newPath: String = "", val newMode: String = "sumh",
    val editingPath: String? = null, val baseline: String,
) {
    fun record() = EditorDraftRecord(listOf(mode, newPath, newMode, editingPath.orEmpty(), baseline) +
        rules.flatMap { listOf(it.path, it.mode) })
    companion object {
        fun from(record: EditorDraftRecord) = record.fields.let { fields ->
            ModuleEditorDraft(fields[0], fields.drop(5).chunked(2).map { ModuleRule(it[0], it[1]) },
                fields[1], fields[2], fields[3].ifEmpty { null }, fields[4])
        }
        fun baseline(mode: String, rules: List<ModuleRule>) = draftFingerprint(listOf(mode) + rules.flatMap { listOf(it.path, it.mode) })
    }
}

internal data class MapsEditorDraft(val target: String, val replacement: String, val baseline: String,
    val pendingTarget: String = "", val pendingReplacement: String = "") {
    fun record() = EditorDraftRecord(listOf(target, replacement, baseline, pendingTarget, pendingReplacement))
    companion object {
        fun from(record: EditorDraftRecord) = record.fields.let { MapsEditorDraft(it[0], it[1], it[2], it[3], it[4]) }
        fun baseline(rule: MapsPathRule?) = draftFingerprint(if (rule == null) emptyList() else listOf(rule.targetPath, rule.replacementPath))
    }
}

internal fun validEditorDraftRecord(key: String, record: EditorDraftRecord): Boolean = when {
    key.startsWith("hide-definition:") -> HideDefinitionDraft.valid(key, record)
    key == PARTITIONS_DRAFT_KEY -> PartitionsDraft.valid(record)
    key == MAPS_TRANSFER_DRAFT_KEY -> MapsTransferDraft.valid(record)
    key.startsWith("module-template:") -> ModuleTemplateDraft.valid(record)
    key.startsWith("module:") -> record.fields.size in 5..517 && (record.fields.size - 5) % 2 == 0 && record.fields[4].length == 64
    key.startsWith("maps:") -> record.fields.size == 5 && record.fields[2].length == 64
    else -> false
}
