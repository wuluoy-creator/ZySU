package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.sumh.util.*

@Composable internal fun HideDefinitionEditor(key: String, drafts: SUMHEditorDrafts, sample: HideDefinitionCollection,
    ready: Boolean, busy: Boolean, result: HideMutationResult?, onDismiss: () -> Unit, onSubmit: (HideDefinitionDraft) -> Unit) {
    val draft = drafts.snapshot[key]?.let(HideDefinitionDraft::from)
    val row = sample.rows.find { it.id == draft?.id }
    val removed = draft != null && draft.id.isNotEmpty() && sample.complete && row == null
    val conflict = draft != null && sample.complete && (draft.revision != sample.revision || draft.baseline != HideDefinitionDraft.baseline(row))
    val canSave = ready && !busy && draft != null && !conflict && !removed && draft.error() == null &&
        (draft.id.isNotEmpty() || sample.managed == true && sample.rows.none { it.path == draft.path })
    fun edit(transform: (HideDefinitionDraft) -> HideDefinitionDraft) {
        drafts.change(key) { current -> current?.let { transform(HideDefinitionDraft.from(it)).record() } }
    }
    YukiAlertDialog(onDismissRequest = { if (!busy) onDismiss() },
        modifier = Modifier.padding(horizontal = 24.dp).widthIn(max = 600.dp).fillMaxWidth(),
        properties = DialogProperties(usePlatformDefaultWidth = false),
        title = { Text(stringResource(if (draft?.id.isNullOrEmpty()) R.string.sumh_user_hide_add else R.string.sumh_definitions_edit)) },
        text = { Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            SUMHEditorDraftStatus(drafts)
            if (draft != null) {
                if (draft.id.isEmpty()) OutlinedTextField(draft.path, { edit { d -> d.copy(path = it) } }, Modifier.fillMaxWidth().testTag("hide_definition_path"),
                    enabled = !busy, label = { Text(stringResource(R.string.sumh_user_rules_add_label)) }, isError = draft.path.isNotEmpty() && draft.error() == "path")
                else {
                    Text(draft.path, fontFamily = FontFamily.Monospace)
                    Text(stringResource(R.string.sumh_definitions_path_fixed), style = MaterialTheme.typography.bodySmall)
                }
                OutlinedTextField(draft.name, { edit { d -> d.copy(name = it) } }, Modifier.fillMaxWidth().testTag("hide_definition_name"),
                    enabled = !busy, label = { Text(stringResource(R.string.sumh_definitions_name)) }, isError = !validHideDefinitionText(draft.name, 128))
                OutlinedTextField(draft.note, { edit { d -> d.copy(note = it) } }, Modifier.fillMaxWidth().testTag("hide_definition_note"),
                    enabled = !busy, label = { Text(stringResource(R.string.sumh_definitions_note)) }, maxLines = 5, isError = !validHideDefinitionText(draft.note, 1024))
                OutlinedTextField(draft.group, { edit { d -> d.copy(group = it) } }, Modifier.fillMaxWidth().testTag("hide_definition_group"),
                    enabled = !busy, label = { Text(stringResource(R.string.sumh_definitions_group)) }, isError = !validHideDefinitionText(draft.group, 128))
                Text(stringResource(R.string.sumh_definitions_limits), style = MaterialTheme.typography.bodySmall)
                if (draft.error() != null && draft.path.isNotEmpty()) Text(stringResource(R.string.sumh_definitions_invalid), color = MaterialTheme.colorScheme.error)
                if (draft.id.isEmpty()) Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).toggleable(draft.enabled, enabled = !busy,
                    role = Role.Switch, onValueChange = { edit { d -> d.copy(enabled = it) } }), verticalAlignment = Alignment.CenterVertically) {
                    Text(stringResource(R.string.sumh_definitions_enable_on_create), Modifier.weight(1f)); Switch(draft.enabled, null)
                }
                if (draft.id.isEmpty() && sample.rows.any { it.path == draft.path }) Text(stringResource(R.string.sumh_definitions_duplicate), color = MaterialTheme.colorScheme.error)
                if (removed) Text(stringResource(R.string.sumh_definitions_removed), color = MaterialTheme.colorScheme.error)
                if (conflict && !removed) {
                    Text(stringResource(R.string.sumh_definitions_conflict), color = MaterialTheme.colorScheme.error, modifier = Modifier.testTag("hide_definition_conflict"))
                    FlowRow {
                        TextButton(enabled = ready && !busy, onClick = { edit { it.rebased(row, requireNotNull(sample.revision), false) } }) { Text(stringResource(R.string.sumh_definitions_use_saved)) }
                        TextButton(enabled = ready && !busy, onClick = { edit { it.rebased(row, requireNotNull(sample.revision), true) } }) { Text(stringResource(R.string.sumh_definitions_keep_input)) }
                    }
                }
                if (row != null) Text(stringResource(R.string.sumh_definitions_current_metadata, row.name, row.note, row.group), style = MaterialTheme.typography.bodySmall)
                result?.takeUnless { it.succeeded }?.let { HideMutationSummary(it) }
                if (!sample.complete) Text(stringResource(R.string.sumh_definitions_incomplete), color = MaterialTheme.colorScheme.error)
            }
            if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
        } },
        confirmButton = { TextButton(enabled = canSave, onClick = { onSubmit(requireNotNull(draft)) }, modifier = Modifier.testTag("hide_definition_save")) { Text(stringResource(R.string.sumh_rules_save)) } },
        dismissButton = { TextButton(enabled = !busy, onClick = onDismiss) { Text(stringResource(R.string.sumh_rules_cancel)) } })
}

@Composable internal fun HideMutationSummary(result: HideMutationResult) {
    Column(Modifier.semantics { liveRegion = LiveRegionMode.Polite }, verticalArrangement = Arrangement.spacedBy(4.dp)) {
        result.operationId?.let { Text("ID: $it", fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall) }
        Text(stringResource(when (result.value?.status) {
            "applied" -> if (result.error == null) R.string.sumh_definitions_registered_result else R.string.sumh_definitions_unknown_result
            "saved", "unchanged" -> if (result.error == null) R.string.sumh_definitions_saved_result else R.string.sumh_definitions_unknown_result
            "partial" -> R.string.sumh_definitions_partial_result
            "rejected", "not_saved" -> R.string.sumh_definitions_rejected_result
            else -> R.string.sumh_definitions_unknown_result
        }))
        result.value?.let { value ->
            Text(stringResource(R.string.sumh_definitions_result_counts, value.affected, value.confirmed, value.failed, value.unknown), style = MaterialTheme.typography.bodySmall)
            if (!value.itemsComplete) Text(stringResource(R.string.sumh_definitions_result_incomplete), style = MaterialTheme.typography.bodySmall)
            if (value.reason.isNotEmpty()) Text(value.reason.take(1500), style = MaterialTheme.typography.bodySmall)
            if (value.error.isNotEmpty()) Text(value.error.take(1500), color = MaterialTheme.colorScheme.error)
        }
        result.error?.let { Text(it.take(1500), color = MaterialTheme.colorScheme.error) }
    }
}

@Composable internal fun HideDefinitionResultDialog(result: HideMutationResult?, batch: HideDefinitionBatch?, onDismiss: () -> Unit) {
    YukiAlertDialog(onDismissRequest = onDismiss, title = { Text(stringResource(R.string.sumh_rules_last_result)) },
        text = { LazyColumn(Modifier.heightIn(max = 440.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            if (batch != null) {
                item { Text(stringResource(R.string.sumh_definitions_batch_result, batch.succeededCount, batch.attempted.size, batch.unattempted.size)) }
                items(batch.attempted, key = { it.first }) { (id, outcome) ->
                    Text(id, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall); HideMutationSummary(outcome)
                }
                if (batch.unattempted.isNotEmpty()) item { Text(stringResource(R.string.sumh_definitions_refresh_retry)) }
            } else if (result != null) {
                item { HideMutationSummary(result) }
                items(result.value?.items.orEmpty(), key = { it.id }) { item ->
                    Text(item.id + "\n" + item.outcome + " · errno=" + item.errno, style = MaterialTheme.typography.bodySmall)
                }
            }
        } }, confirmButton = { TextButton(onClick = onDismiss) { Text(stringResource(android.R.string.ok)) } })
}
