package com.zying.zysu.ui.sumh

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.listSaver
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.sumh.util.*
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter.MapsPathRule
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject

@Composable
internal fun MapsTransferControls(rules: List<MapsPathRule>, enabled: Boolean, onRulesChanged: (List<MapsPathRule>) -> Unit) {
    var importOpen by rememberSaveable { mutableStateOf(false) }
    var exportOpen by rememberSaveable { mutableStateOf(false) }
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        TextButton(onClick = { importOpen = true }, enabled = enabled, modifier = Modifier.testTag("maps_import")) {
            Text(stringResource(R.string.sumh_maps_import))
        }
        TextButton(onClick = { exportOpen = true }, modifier = Modifier.testTag("maps_export")) {
            Text(stringResource(R.string.sumh_maps_export))
        }
    }
    if (importOpen) MapsImportDialog(rules, enabled, onRulesChanged) { importOpen = false }
    if (exportOpen) MapsExportDialog(rules) { exportOpen = false }
}

@Composable
internal fun MapsImportDialog(rules: List<MapsPathRule>, enabled: Boolean, onRulesChanged: (List<MapsPathRule>) -> Unit,
    onDismiss: () -> Unit) {
    val drafts = rememberSUMHEditorDrafts()
    val baseline = MapsTransferDraft.baseline(rules)
    val initial = MapsTransferDraft(baseline = baseline)
    val draft = drafts.snapshot[MAPS_TRANSFER_DRAFT_KEY]?.let(MapsTransferDraft::from) ?: initial
    LaunchedEffect(drafts.loaded, drafts.snapshot[MAPS_TRANSFER_DRAFT_KEY]) {
        if (drafts.loaded) drafts.ensure(MAPS_TRANSFER_DRAFT_KEY, initial.record())
    }
    val ready = drafts.loaded && MAPS_TRANSFER_DRAFT_KEY in drafts.snapshot
    fun change(transform: (MapsTransferDraft) -> MapsTransferDraft) = drafts.change(MAPS_TRANSFER_DRAFT_KEY) {
        transform(it?.let(MapsTransferDraft::from) ?: initial).record()
    }
    var inputError by remember { mutableStateOf<String?>(null) }
    var fileLoading by remember { mutableStateOf(false) }
    var reviewed by remember { mutableStateOf<String?>(null) }
    val parsed = remember(draft.text) { runCatching { importSUMHMapsDefinitions(draft.text) } }
    val review = remember(rules, draft.text, draft.mode, draft.choice) {
        parsed.getOrNull()?.let { reviewSUMHMapsImport(rules, it, draft.mode, draft.choice) }
    }
    val conflicted = draft.baseline != baseline
    val signature = draftFingerprint(listOf(draft.inputFingerprint(), baseline))
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val open = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            val expected = drafts.snapshot[MAPS_TRANSFER_DRAFT_KEY]
            fileLoading = true
            scope.launch {
                try {
                    val text = withContext(Dispatchers.IO) {
                        readSUMHMapsDocument(requireNotNull(context.contentResolver.openInputStream(uri)))
                    }
                    if (drafts.snapshot[MAPS_TRANSFER_DRAFT_KEY] == expected) {
                        change { it.copy(text = text, choice = SUMHMapsConflictChoice.Unresolved) }
                        reviewed = null; inputError = null
                    } else inputError = "draft_changed"
                } catch (error: Exception) {
                    if (error is CancellationException) throw error
                    inputError = error.message ?: "file_read_failed"
                } finally { fileLoading = false }
            }
        }
    }
    LaunchedEffect(rules, draft) {
        if (draft.pending.isNotEmpty() && draft.pending == baseline && drafts.snapshot[MAPS_TRANSFER_DRAFT_KEY] == draft.record()) {
            if (draft.pendingInput == draft.inputFingerprint()) {
                drafts.forget(MAPS_TRANSFER_DRAFT_KEY, draft.record()); onDismiss()
            } else change { it.copy(baseline = baseline, pending = "", pendingInput = "") }
        }
    }
    val canSave = ready && enabled && !fileLoading && !conflicted && reviewed == signature && review != null &&
        !review.unresolved && review.withinBudget && review.candidate != rules
    YukiAlertDialog(onDismissRequest = onDismiss,
        modifier = Modifier.padding(horizontal = 24.dp).widthIn(max = 560.dp).fillMaxWidth(),
        properties = DialogProperties(usePlatformDefaultWidth = false),
        title = { Text(stringResource(R.string.sumh_maps_import)) }, text = {
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            SUMHEditorDraftStatus(drafts)
            Text(stringResource(R.string.sumh_maps_transfer_scope))
            OutlinedButton(onClick = { open.launch(arrayOf("application/json", "text/plain", "application/octet-stream")) },
                enabled = ready && enabled && !fileLoading) { Text(stringResource(R.string.sumh_maps_import_file)) }
            if (fileLoading) LinearProgressIndicator(Modifier.fillMaxWidth())
            OutlinedTextField(value = draft.text, onValueChange = { value ->
                if (value.toByteArray().size > SUMH_MAPS_TRANSFER_BYTES || !Charsets.UTF_8.newEncoder().canEncode(value)) inputError = "document_budget"
                else { change { it.copy(text = value, choice = SUMHMapsConflictChoice.Unresolved) }; inputError = null; reviewed = null }
            }, label = { Text(stringResource(R.string.sumh_maps_import_json)) }, modifier = Modifier.fillMaxWidth().testTag("maps_import_json"),
                enabled = ready && enabled && !fileLoading, minLines = 4, maxLines = 8)
            if (inputError != null || reviewed != null && parsed.isFailure) {
                Text(stringResource(R.string.sumh_maps_import_invalid), color = MaterialTheme.colorScheme.error)
                Text(inputError ?: parsed.exceptionOrNull()?.message.orEmpty(), style = MaterialTheme.typography.bodySmall)
            }
            if (conflicted) {
                Text(stringResource(R.string.sumh_maps_import_external), color = MaterialTheme.colorScheme.error)
                TextButton(onClick = { change { it.copy(baseline = baseline, pending = "", pendingInput = "") }; reviewed = null }) {
                    Text(stringResource(R.string.sumh_maps_import_rebase))
                }
            }
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                SUMHMapsImportMode.entries.forEach { mode ->
                    FilterChip(selected = draft.mode == mode, onClick = { change { it.copy(mode = mode) }; reviewed = null },
                        enabled = ready && !fileLoading, label = { Text(stringResource(if (mode == SUMHMapsImportMode.Merge)
                            R.string.sumh_maps_import_merge else R.string.sumh_maps_import_replace)) })
                }
            }
            if (draft.mode == SUMHMapsImportMode.Replace) Text(stringResource(R.string.sumh_maps_import_replace_scope))
            if (review?.conflicts?.isNotEmpty() == true) {
                Text(stringResource(R.string.sumh_maps_import_conflicts, review.conflicts.size))
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    for (choice in listOf(SUMHMapsConflictChoice.KeepSaved, SUMHMapsConflictChoice.UseImported)) FilterChip(
                        selected = draft.choice == choice, onClick = { change { it.copy(choice = choice) }; reviewed = null },
                        label = { Text(stringResource(if (choice == SUMHMapsConflictChoice.KeepSaved)
                            R.string.sumh_maps_import_keep else R.string.sumh_maps_import_use)) })
                }
            }
            OutlinedButton(onClick = { reviewed = signature; inputError = null }, enabled = ready && !conflicted && !fileLoading,
                modifier = Modifier.testTag("maps_import_review")) { Text(stringResource(R.string.sumh_maps_import_review)) }
            if (reviewed == signature && review != null) {
                Text(stringResource(R.string.sumh_maps_import_diff, review.added.size, review.changed.size, review.removed.size))
                if (review.unresolved) Text(stringResource(R.string.sumh_maps_import_resolve), color = MaterialTheme.colorScheme.error)
                if (!review.withinBudget) Text(stringResource(R.string.sumh_maps_path_budget), color = MaterialTheme.colorScheme.error)
                review.added.forEach { MapsTransferRow(stringResource(R.string.sumh_maps_diff_added), it) }
                review.changed.forEach { (old, next) -> MapsTransferRow(stringResource(R.string.sumh_maps_diff_changed), next, old.replacementPath) }
                review.removed.forEach { MapsTransferRow(stringResource(R.string.sumh_maps_diff_removed), it) }
                if (review.unresolved) review.conflicts.forEach { (old, next) ->
                    MapsTransferRow(stringResource(R.string.sumh_maps_diff_conflict), next, old.replacementPath)
                }
            }
        }
    }, confirmButton = {
        TextButton(onClick = {
            if (canSave && drafts.snapshot[MAPS_TRANSFER_DRAFT_KEY] == draft.record()) {
                val candidate = requireNotNull(review).candidate
                if (change { it.copy(pending = MapsTransferDraft.baseline(candidate), pendingInput = draft.inputFingerprint()) }) onRulesChanged(candidate)
            }
        }, enabled = canSave, modifier = Modifier.testTag("maps_import_save")) { Text(stringResource(R.string.sumh_rules_save)) }
    }, dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.sumh_rules_cancel)) } })
}

@Composable private fun MapsTransferRow(label: String, rule: MapsPathRule, previous: String? = null) {
    HorizontalDivider()
    Text(label, style = MaterialTheme.typography.labelLarge)
    Text(JSONObject.quote(rule.targetPath), fontFamily = FontFamily.Monospace)
    previous?.let { Text(stringResource(R.string.sumh_maps_diff_previous, JSONObject.quote(it)), fontFamily = FontFamily.Monospace) }
    Text(JSONObject.quote(rule.replacementPath), fontFamily = FontFamily.Monospace)
}

@Composable internal fun MapsExportDialog(rules: List<MapsPathRule>, onDismiss: () -> Unit) {
    // Save only bounded path fingerprints (at most 256 * 64 chars), not full long paths in the activity bundle.
    val keys = remember(rules) { rules.associate { it.targetPath to draftFingerprint(listOf(it.targetPath)) } }
    var selected by rememberSaveable(stateSaver = listSaver<Set<String>, String>(
        save = { it.toList() }, restore = { it.toSet() })) { mutableStateOf(keys.values.toSet()) }
    LaunchedEffect(keys) { selected = selected.intersect(keys.values.toSet()) }
    var copied by remember { mutableStateOf(false) }
    val context = LocalContext.current
    val output = remember(rules, selected) { runCatching { exportSUMHMapsDefinitions(rules.filter { keys[it.targetPath] in selected }) } }
    YukiAlertDialog(onDismissRequest = onDismiss,
        modifier = Modifier.padding(horizontal = 24.dp).widthIn(max = 560.dp).fillMaxWidth(),
        properties = DialogProperties(usePlatformDefaultWidth = false),
        title = { Text(stringResource(R.string.sumh_maps_export)) }, text = {
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(stringResource(R.string.sumh_maps_transfer_scope))
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TextButton(onClick = { selected = keys.values.toSet(); copied = false }) { Text(stringResource(R.string.sumh_maps_export_all)) }
                TextButton(onClick = { selected = emptySet(); copied = false }) { Text(stringResource(R.string.sumh_maps_export_none)) }
            }
            rules.forEach { rule -> val selectionKey = keys.getValue(rule.targetPath)
                Row(Modifier.fillMaxWidth().heightIn(min = 48.dp)
                .testTag("maps_export_${rule.targetPath}").toggleable(selectionKey in selected, role = Role.Checkbox) { checked ->
                    selected = if (checked) selected + selectionKey else selected - selectionKey; copied = false
                }) {
                Checkbox(selectionKey in selected, null)
                Column(Modifier.weight(1f)) {
                    Text(JSONObject.quote(rule.targetPath), fontFamily = FontFamily.Monospace)
                    Text(JSONObject.quote(rule.replacementPath), style = MaterialTheme.typography.bodySmall)
                }
            } }
            if (output.isFailure) Text(stringResource(R.string.sumh_maps_import_invalid), color = MaterialTheme.colorScheme.error)
            if (copied) Text(stringResource(R.string.sumh_maps_export_copied), modifier = Modifier.testTag("maps_export_copied"))
        }
    }, confirmButton = { TextButton(enabled = output.isSuccess, onClick = {
        (context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager)
            .setPrimaryClip(ClipData.newPlainText("SUMH managed Maps definitions", output.getOrThrow()))
        copied = true
    }, modifier = Modifier.testTag("maps_export_copy")) { Text(stringResource(R.string.sumh_maps_export_copy)) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.close)) } })
}
