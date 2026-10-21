package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.sumh.util.*
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable internal fun SUMHPartitionsControls(enabled: Boolean, configFingerprint: String,
    onSaved: (List<String>) -> Unit, onSaving: (Boolean) -> Unit) {
    var open by rememberSaveable { mutableStateOf(false) }
    ConfigSection(stringResource(R.string.sumh_partitions_title)) {
        Text(stringResource(R.string.sumh_partitions_intro), style = MaterialTheme.typography.bodyMedium)
        OutlinedButton(onClick = { open = true }, enabled = enabled) { Text(stringResource(R.string.sumh_partitions_open)) }
    }
    if (open) SUMHPartitionsDialog(enabled, configFingerprint, onSaved, onSaving, { open = false })
}

@Composable internal fun SUMHPartitionsDialog(enabled: Boolean, configFingerprint: String,
    onSaved: (List<String>) -> Unit, onSaving: (Boolean) -> Unit, onDismiss: () -> Unit,
    discover: suspend () -> SUMHPartitionDiscoveryResult = { SUMHManager.discoverPartitions() },
    save: suspend (String, List<String>) -> SUMHPartitionSaveResult = { revision, names -> SUMHManager.savePartitions(revision, names) },
) {
    val drafts = rememberSUMHEditorDrafts()
    val scope = rememberCoroutineScope()
    var result by remember { mutableStateOf<SUMHPartitionDiscoveryResult?>(null) }
    var loading by remember { mutableStateOf(false) }
    var saving by remember { mutableStateOf(false) }
    var invalidated by remember { mutableStateOf(false) }
    var readFingerprint by remember { mutableStateOf<String?>(null) }
    var reviewed by remember { mutableStateOf<EditorDraftRecord?>(null) }
    var saveResult by remember { mutableStateOf<SUMHPartitionSaveResult?>(null) }
    var confirmed by remember { mutableStateOf(false) }
    var details by rememberSaveable { mutableStateOf(false) }
    val latestFingerprint by rememberUpdatedState(configFingerprint)
    val latestSaved by rememberUpdatedState(onSaved)
    val latestSaving by rememberUpdatedState(onSaving)
    val sample = result?.value
    val draft = drafts.snapshot[PARTITIONS_DRAFT_KEY]?.let(PartitionsDraft::from)
    val stale = invalidated || readFingerprint != null && readFingerprint != configFingerprint
    LaunchedEffect(configFingerprint) {
        if (readFingerprint != null && readFingerprint != configFingerprint) { invalidated = true; reviewed = null }
    }
    val ready = enabled && drafts.loaded && !loading && !saving && !stale && result?.error == null && sample?.selectionValid == true
    val needsRebase = draft != null && sample != null && (draft.revision != sample.revision || draft.saved != sample.saved)
    val canReview = ready && draft != null && !needsRebase && draft.canSave(requireNotNull(sample))
    val canSave = canReview && draft?.record() == reviewed

    YukiAlertDialog(
        onDismissRequest = { if (!saving) onDismiss() },
        modifier = Modifier.padding(horizontal = 24.dp).widthIn(max = 600.dp).fillMaxWidth(),
        properties = DialogProperties(usePlatformDefaultWidth = false),
        title = { Text(stringResource(R.string.sumh_partitions_title)) },
        text = {
            Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                SUMHEditorDraftStatus(drafts)
                Text(stringResource(R.string.sumh_partitions_intro))
                OutlinedButton(onClick = {
                    if (enabled && drafts.loaded && !loading && !saving) {
                        loading = true; reviewed = null; invalidated = false; result = null; confirmed = false; saveResult = null
                        val startedFingerprint = configFingerprint; readFingerprint = startedFingerprint
                        scope.launch {
                            try {
                                val response = discover(); result = response
                                if (startedFingerprint != latestFingerprint) invalidated = true
                                response.value?.takeIf { response.error == null && it.selectionValid && !invalidated }?.let {
                                    drafts.ensure(PARTITIONS_DRAFT_KEY, PartitionsDraft(requireNotNull(it.revision), it.saved, it.saved).record())
                                }
                            } catch (e: Exception) {
                                if (e is CancellationException) throw e
                                result = SUMHPartitionDiscoveryResult(error = e.message ?: "discovery_failed")
                            } finally { loading = false }
                        }
                    }
                }, enabled = enabled && drafts.loaded && !loading && !saving, modifier = Modifier.testTag("partitions_discover")) {
                    Text(stringResource(R.string.sumh_partitions_discover))
                }
                if (loading || saving) LinearProgressIndicator(Modifier.fillMaxWidth())
                Text(stringResource(when {
                    loading -> R.string.sumh_partitions_loading
                    stale -> R.string.sumh_partitions_stale
                    result == null -> R.string.sumh_partitions_not_loaded
                    result?.error != null -> R.string.sumh_partitions_unknown
                    sample?.status == "stale" -> R.string.sumh_partitions_stale
                    sample?.status == "partial" -> R.string.sumh_partitions_partial
                    sample?.selectionValid != true -> R.string.sumh_partitions_unknown
                    sample.candidates.isEmpty() -> R.string.sumh_partitions_empty
                    else -> R.string.sumh_partitions_ready
                }), modifier = Modifier.testTag("partitions_status"))
                result?.error?.let { Text(it.take(1500), color = MaterialTheme.colorScheme.error) }
                if (needsRebase) {
                    Text(stringResource(R.string.sumh_partitions_conflict), color = MaterialTheme.colorScheme.error)
                    OutlinedButton(onClick = {
                        if (ready && sample != null && draft != null) {
                            drafts.change(PARTITIONS_DRAFT_KEY) { current ->
                                if (current == draft.record()) draft.rebase(sample).record() else current
                            }; reviewed = null
                        }
                    }, enabled = ready, modifier = Modifier.testTag("partitions_rebase")) { Text(stringResource(R.string.sumh_partitions_rebase)) }
                }
                if (draft != null) {
                    val names = (draft.saved + draft.selected + sample?.saved.orEmpty() + sample?.candidates.orEmpty().map { it.name }).distinct()
                    names.forEach { name ->
                        val candidate = sample?.candidates?.find { it.name == name }
                        // A newly selected candidate can disappear before the next
                        // scan. Keep removal available so the draft can recover.
                        val selectable = ready && !needsRebase &&
                            (name in draft.saved || name in draft.selected || candidate?.applicable == true)
                        Column {
                            Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).toggleable(
                                value = name in draft.selected, enabled = selectable, role = Role.Checkbox,
                                onValueChange = { selected ->
                                    drafts.change(PARTITIONS_DRAFT_KEY) { current ->
                                        if (current != draft.record()) current else draft.copy(selected =
                                            if (selected) (draft.selected + name).distinct() else draft.selected - name).record()
                                    }; reviewed = null
                                }).testTag("partition_$name"), verticalAlignment = Alignment.CenterVertically) {
                                Checkbox(name in draft.selected, onCheckedChange = null, enabled = selectable)
                                Text(name, Modifier.weight(1f).padding(start = 8.dp), fontFamily = FontFamily.Monospace)
                            }
                            Text(stringResource(when {
                                name in draft.saved && candidate?.applicable != true -> R.string.sumh_partitions_saved_unknown
                                candidate?.applicable == true -> R.string.sumh_partitions_eligible
                                else -> R.string.sumh_partitions_not_eligible
                            }), style = MaterialTheme.typography.bodySmall)
                            if (details && candidate != null) {
                                Text("${candidate.status}: ${candidate.reason}\n${partitionBytesLabel(candidate.target)}: ${candidate.targetStatus}\n" +
                                    "${partitionBytesLabel(candidate.alias)}: ${candidate.aliasStatus}\n${candidate.mountStatus}: ${candidate.filesystem}",
                                    style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
                                candidate.sources.forEach { source ->
                                    Text("${partitionBytesLabel(source.module)} ${partitionBytesLabel(source.path)}\n" +
                                        "${source.layout}: ${source.status}; ${source.reason}; disabled=${source.disabled}; remove=${source.removePending}; skip_mount=${source.skipMount}",
                                        style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
                                }
                            }
                        }
                    }
                    OutlinedButton(onClick = { if (canReview && drafts.snapshot[PARTITIONS_DRAFT_KEY] == draft.record()) reviewed = draft.record() },
                        enabled = canReview, modifier = Modifier.testTag("partitions_review")) { Text(stringResource(R.string.sumh_partitions_review)) }
                    if (canSave) {
                        Text(stringResource(R.string.sumh_partitions_add, draft.added().joinToString().ifEmpty { "—" }))
                        Text(stringResource(R.string.sumh_partitions_remove, draft.removed().joinToString().ifEmpty { "—" }))
                        Text(stringResource(R.string.sumh_partitions_reboot), modifier = Modifier.testTag("partitions_diff"))
                    }
                }
                if (confirmed) Text(stringResource(R.string.sumh_partitions_saved), modifier = Modifier.testTag("partitions_saved"))
                else saveResult?.let { outcome ->
                    Text(stringResource(R.string.sumh_partitions_save_unconfirmed), color = MaterialTheme.colorScheme.error)
                    Text(outcome.error?.take(1500) ?: outcome.value?.let { "${it.status}: ${it.reason}\n${partitionBytesLabel(it.detail)}" }.orEmpty(),
                        style = MaterialTheme.typography.bodySmall)
                }
                TextButton(onClick = { details = !details }) { Text(stringResource(R.string.sumh_state_diagnostics)) }
                if (details) sample?.let {
                    Text("status=${it.status}; config=${it.configStatus}\nrevision=${it.revision}\nboot=${it.bootId}; namespace=${it.namespace}",
                        style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
                    it.issues.forEach { issue -> Text(issue, style = MaterialTheme.typography.bodySmall) }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = {
                if (canSave && draft != null && drafts.snapshot[PARTITIONS_DRAFT_KEY] == draft.record()) {
                    saving = true; latestSaving(true); reviewed = null; confirmed = false
                    scope.launch {
                        withContext(NonCancellable) {
                            try {
                                val response = save(draft.revision, draft.selected); saveResult = response
                                val value = response.value
                                confirmed = response.error == null && value?.saved == true && value.previousRevision == draft.revision && value.partitions == draft.selected
                                if (confirmed && value != null) {
                                    drafts.change(PARTITIONS_DRAFT_KEY) { current ->
                                        if (current == draft.record()) PartitionsDraft(requireNotNull(value.revision), value.partitions, value.partitions).record() else current
                                    }
                                    latestSaved(value.partitions)
                                }
                            } catch (e: Exception) { saveResult = SUMHPartitionSaveResult(error = e.message ?: "save_outcome_unknown") }
                            finally { invalidated = true; saving = false; latestSaving(false) }
                        }
                    }
                }
            }, enabled = canSave, modifier = Modifier.testTag("partitions_save")) { Text(stringResource(R.string.sumh_partitions_save)) }
        },
        dismissButton = { TextButton(onClick = onDismiss, enabled = !saving) { Text(stringResource(R.string.close)) } },
    )
}
