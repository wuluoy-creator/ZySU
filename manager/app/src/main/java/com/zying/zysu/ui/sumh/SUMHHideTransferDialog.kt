package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material3.*
import androidx.compose.runtime.*
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
import org.json.JSONObject

/** Lazy rows keep a full 4096-definition artifact out of both the UI tree and Activity state. */
@Composable internal fun HideTransferDialog(documentBytes: Int, preserveEnabled: Boolean,
    review: SUMHHideImportReview?, reviewed: Boolean, busy: Boolean, applying: Boolean, loaded: Boolean,
    error: String?, progress: Int, batch: SUMHHideImportBatch?,
    onFile: () -> Unit, onText: () -> Unit, onPreserveEnabled: (Boolean) -> Unit,
    onChoice: (SUMHHideTransferRow, SUMHHideTransferResolution) -> Unit,
    onReview: () -> Unit, onApply: () -> Unit, onStop: () -> Unit, onDismiss: () -> Unit) {
    val candidateById = remember(review) { review?.candidate.orEmpty().associateBy { it.id } }
    YukiAlertDialog(onDismissRequest = { if (!applying) onDismiss() },
        modifier = Modifier.padding(horizontal = 16.dp).widthIn(max = 680.dp).fillMaxWidth(),
        properties = DialogProperties(usePlatformDefaultWidth = false),
        title = { Text(stringResource(R.string.sumh_hide_transfer_title)) },
        text = {
            LazyColumn(Modifier.fillMaxWidth().heightIn(max = 640.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                item {
                    Text(stringResource(R.string.sumh_hide_transfer_scope))
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedButton(onClick = onFile, enabled = loaded && !busy, modifier = Modifier.testTag("hide_transfer_file")) {
                            Text(stringResource(R.string.sumh_hide_transfer_file))
                        }
                        TextButton(onClick = onText, enabled = loaded && !busy) { Text(stringResource(R.string.sumh_hide_transfer_text)) }
                    }
                    Text(stringResource(R.string.sumh_hide_transfer_size, documentBytes), style = MaterialTheme.typography.bodySmall)
                    Row(Modifier.fillMaxWidth().toggleable(preserveEnabled, enabled = loaded && !busy,
                        role = Role.Checkbox, onValueChange = onPreserveEnabled).padding(vertical = 8.dp)) {
                        Checkbox(preserveEnabled, onCheckedChange = null)
                        Text(stringResource(R.string.sumh_hide_transfer_enabled), Modifier.padding(start = 8.dp))
                    }
                    if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
                    if (applying) Row {
                        Text(stringResource(R.string.sumh_hide_transfer_progress, progress), Modifier.weight(1f))
                        TextButton(onClick = onStop) { Text(stringResource(R.string.sumh_rules_stop)) }
                    }
                    error?.let { Text(it.take(1500), color = MaterialTheme.colorScheme.error, modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite }) }
                    OutlinedButton(onClick = onReview, enabled = loaded && !busy && documentBytes > 0,
                        modifier = Modifier.testTag("hide_transfer_review")) { Text(stringResource(R.string.sumh_hide_transfer_review)) }
                    if (review != null) {
                        Text(stringResource(R.string.sumh_hide_transfer_diff, review.addedCount, review.changedCount,
                            review.enabledCount, review.disabledCount, review.skippedCount, review.unchangedCount))
                        Text(stringResource(R.string.sumh_hide_transfer_group, minOf(16, review.steps.size), maxOf(0, review.steps.size - 16)))
                        if (!reviewed) Text(stringResource(R.string.sumh_hide_transfer_review_again), color = MaterialTheme.colorScheme.error)
                        review.errors.forEach { Text(it, color = MaterialTheme.colorScheme.error) }
                        if (review.unresolved) Text(stringResource(R.string.sumh_hide_transfer_unresolved), color = MaterialTheme.colorScheme.error)
                    }
                    batch?.let { result ->
                        Text(stringResource(R.string.sumh_hide_transfer_result, result.succeededCount, result.attempted.size, result.unattempted.size),
                            modifier = Modifier.testTag("hide_transfer_result").semantics { liveRegion = LiveRegionMode.Polite })
                        result.stoppedReason?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                        Text(stringResource(R.string.sumh_hide_transfer_retained), style = MaterialTheme.typography.bodySmall)
                        result.attempted.forEach { (step, response) ->
                            Text("${JSONObject.quote(step.after.path)} · ${response.value?.status ?: "unknown"} · ${response.operationId.orEmpty()}",
                                style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
                        }
                    }
                }
                items(review?.rows.orEmpty(), key = { it.sourceId }) { row -> HideTransferRow(row,
                    candidateById[row.sourceId]?.takeIf { row.collision == SUMHHideTransferCollision.CrossCollision && it.id != row.saved?.id }, !busy, onChoice) }
            }
        }, confirmButton = {
            TextButton(onClick = onApply, enabled = loaded && !busy && reviewed && review?.canApply == true && review.steps.isNotEmpty(),
                modifier = Modifier.testTag("hide_transfer_apply")) { Text(stringResource(R.string.sumh_hide_transfer_apply)) }
        }, dismissButton = { TextButton(onClick = onDismiss, enabled = !applying) { Text(stringResource(R.string.sumh_rules_cancel)) } })
}

@Composable private fun HideTransferRow(row: SUMHHideTransferRow, otherConflict: SUMHHidePortableDefinition?, enabled: Boolean,
    onChoice: (SUMHHideTransferRow, SUMHHideTransferResolution) -> Unit) {
    var open by remember(row.sourceId) { mutableStateOf(false) }
    Column(Modifier.fillMaxWidth()) {
        HorizontalDivider()
        Text(JSONObject.quote(row.imported.path), fontFamily = FontFamily.Monospace)
        Text(row.sourceId, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
        Text(stringResource(row.collision.label), style = MaterialTheme.typography.labelLarge)
        row.saved?.let { DefinitionFields(stringResource(R.string.sumh_hide_transfer_saved), it) }
        otherConflict?.let { DefinitionFields(stringResource(R.string.sumh_hide_transfer_saved), it) }
        DefinitionFields(stringResource(R.string.sumh_hide_transfer_imported), row.imported)
        row.effective?.let { DefinitionFields(stringResource(R.string.sumh_hide_transfer_effective), it) }
        row.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
        Box {
            TextButton(onClick = { open = true }, enabled = enabled,
                modifier = Modifier.testTag("hide_transfer_choice_${row.sourceId}")) { Text(stringResource(row.choice.resolution.label)) }
            DropdownMenu(open, { open = false }) {
                hideTransferChoices(row.collision).forEach { choice -> DropdownMenuItem(text = { Text(stringResource(choice.label)) },
                    onClick = { open = false; onChoice(row, choice) }) }
            }
        }
    }
}

@Composable private fun DefinitionFields(label: String, row: SUMHHidePortableDefinition) {
    Text(label, style = MaterialTheme.typography.labelMedium)
    Text(JSONObject.quote(row.path), style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
    Text("${row.id} · ${stringResource(if (row.enabled) R.string.sumh_definitions_enabled else R.string.sumh_definitions_disabled)}", style = MaterialTheme.typography.bodySmall)
    if (row.name.isNotEmpty() || row.group.isNotEmpty() || row.note.isNotEmpty())
        Text(listOf(row.name, row.group, row.note).joinToString(" · ") { JSONObject.quote(it) }, style = MaterialTheme.typography.bodySmall)
}

internal fun hideTransferChoices(collision: SUMHHideTransferCollision) = when (collision) {
    SUMHHideTransferCollision.New, SUMHHideTransferCollision.Identical -> listOf(SUMHHideTransferResolution.Unresolved, SUMHHideTransferResolution.Skip)
    SUMHHideTransferCollision.Changed -> listOf(SUMHHideTransferResolution.KeepSaved, SUMHHideTransferResolution.UseImported, SUMHHideTransferResolution.Skip)
    SUMHHideTransferCollision.PathCollision -> listOf(SUMHHideTransferResolution.KeepSaved, SUMHHideTransferResolution.AdoptLocalIdentity, SUMHHideTransferResolution.Skip)
    SUMHHideTransferCollision.IdCollision -> listOf(SUMHHideTransferResolution.ExplicitNewIdentity, SUMHHideTransferResolution.Skip)
    SUMHHideTransferCollision.CrossCollision -> listOf(SUMHHideTransferResolution.Skip)
}
private val SUMHHideTransferResolution.label get() = when (this) {
    SUMHHideTransferResolution.Unresolved -> R.string.sumh_hide_transfer_choose
    SUMHHideTransferResolution.KeepSaved -> R.string.sumh_hide_transfer_keep
    SUMHHideTransferResolution.UseImported -> R.string.sumh_hide_transfer_use
    SUMHHideTransferResolution.AdoptLocalIdentity -> R.string.sumh_hide_transfer_local_id
    SUMHHideTransferResolution.ExplicitNewIdentity -> R.string.sumh_hide_transfer_new_id
    SUMHHideTransferResolution.Skip -> R.string.sumh_hide_transfer_skip
}
private val SUMHHideTransferCollision.label get() = when (this) {
    SUMHHideTransferCollision.New -> R.string.sumh_hide_transfer_new
    SUMHHideTransferCollision.Identical -> R.string.sumh_hide_transfer_same
    SUMHHideTransferCollision.Changed -> R.string.sumh_hide_transfer_changed
    SUMHHideTransferCollision.PathCollision -> R.string.sumh_hide_transfer_path_collision
    SUMHHideTransferCollision.IdCollision -> R.string.sumh_hide_transfer_id_collision
    SUMHHideTransferCollision.CrossCollision -> R.string.sumh_hide_transfer_cross_collision
}
