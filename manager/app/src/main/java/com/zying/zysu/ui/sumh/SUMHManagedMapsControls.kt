package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.sumh.util.SUMHFeatureSupport
import com.zying.zysu.ui.sumh.util.SUMHMapsPathError
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter.MapsPathRule
import com.zying.zysu.ui.sumh.util.validateSUMHMapsPathRule
import com.zying.zysu.ui.sumh.util.mapsRulesWithinBudget
import com.zying.zysu.ui.sumh.util.updateSUMHMapsPathRule
import com.zying.zysu.ui.sumh.util.SUMH_MAX_MAPS_PATH_RULES
import ui.screen.moreSettings.component.MoreSettingsItemPosition

@Composable
internal fun ManagedMapsControls(
    rules: List<MapsPathRule>,
    enabled: Boolean,
    support: SUMHFeatureSupport,
    mapsEnabled: Boolean,
    onRulesChanged: (List<MapsPathRule>) -> Unit,
    position: MoreSettingsItemPosition = MoreSettingsItemPosition.Last,
    onRetryApply: () -> Unit = {},
    runtimeApplyPending: Boolean = false,
) {
    var showEditor by rememberSaveable { mutableStateOf(false) }
    var originalTarget by rememberSaveable { mutableStateOf<String?>(null) }
    var removeTarget by rememberSaveable { mutableStateOf<String?>(null) }
    val canEdit = enabled && support == SUMHFeatureSupport.Supported
    val limitReached = rules.size >= SUMH_MAX_MAPS_PATH_RULES
    if (showEditor) {
        key(originalTarget) {
            MapsPathRuleEditor(
                rules = rules, originalTarget = originalTarget, enabled = canEdit,
                onRulesChanged = onRulesChanged,
                onDismiss = { showEditor = false }, onSaved = { showEditor = false },
            )
        }
    }

    removeTarget?.let { selected ->
        val rule = rules.firstOrNull { it.targetPath == selected }
        if (rule != null) YukiAlertDialog(
            onDismissRequest = { removeTarget = null },
            modifier = Modifier.padding(horizontal = 24.dp).widthIn(max = 560.dp).fillMaxWidth(),
            properties = DialogProperties(usePlatformDefaultWidth = false),
            title = { Text(stringResource(R.string.sumh_maps_path_remove)) },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(stringResource(R.string.sumh_maps_path_remove_confirm))
                    Text(rule.targetPath, fontFamily = FontFamily.Monospace)
                    Text(rule.replacementPath, fontFamily = FontFamily.Monospace)
                }
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        if (enabled) {
                            removeTarget = null
                            onRulesChanged(rules.filterNot { it.targetPath == selected })
                        }
                    },
                    enabled = enabled,
                    modifier = Modifier.heightIn(min = 48.dp),
                    colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error),
                ) { Text(stringResource(R.string.delete)) }
            },
            dismissButton = {
                TextButton(onClick = { removeTarget = null }, modifier = Modifier.heightIn(min = 48.dp)) {
                    Text(stringResource(R.string.sumh_rules_cancel))
                }
            },
        )
    }

    SUMHControlGroup(position) {
        Text(stringResource(R.string.sumh_maps_path_saved, rules.size), style = MaterialTheme.typography.bodyLarge)
        Text(stringResource(R.string.sumh_maps_path_description), style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        val availability = when (support) {
            SUMHFeatureSupport.KernelUnavailable -> R.string.sumh_features_kernel_unavailable
            SUMHFeatureSupport.Unknown -> R.string.sumh_features_support_unknown
            SUMHFeatureSupport.Unsupported -> R.string.sumh_maps_path_unsupported
            SUMHFeatureSupport.Supported -> if (!mapsEnabled) R.string.sumh_maps_path_disabled else null
        }
        availability?.let { Text(stringResource(it), style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant) }
        rules.forEach { rule ->
            Row(Modifier.fillMaxWidth().padding(top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(stringResource(R.string.sumh_maps_path_target), style = MaterialTheme.typography.labelMedium)
                    Text(rule.targetPath, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodyMedium,
                        maxLines = 3, overflow = TextOverflow.Ellipsis)
                    Text(stringResource(R.string.sumh_maps_path_replacement), style = MaterialTheme.typography.labelMedium)
                    Text(rule.replacementPath, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodyMedium,
                        maxLines = 3, overflow = TextOverflow.Ellipsis)
                }
                IconButton(onClick = { originalTarget = rule.targetPath; showEditor = true }, enabled = canEdit) {
                    YukiIcon(Icons.Filled.Edit, stringResource(R.string.sumh_maps_path_edit_label, rule.targetPath))
                }
                IconButton(onClick = { removeTarget = rule.targetPath }, enabled = enabled) {
                    YukiIcon(Icons.Filled.Delete, stringResource(R.string.sumh_maps_path_remove_label, rule.targetPath))
                }
            }
        }
        if (limitReached) Text(stringResource(R.string.sumh_maps_path_limit), style = MaterialTheme.typography.bodySmall)
        FilledTonalButton(
            onClick = { if (canEdit) { originalTarget = null; showEditor = true } },
            enabled = canEdit && !limitReached,
            modifier = Modifier.padding(top = 8.dp).heightIn(min = 48.dp),
            shape = MaterialTheme.shapes.small,
        ) {
            YukiIcon(Icons.Filled.Add, null)
            Spacer(Modifier.width(8.dp))
            Text(stringResource(R.string.sumh_maps_path_add))
        }
        if (rules.isNotEmpty() && !runtimeApplyPending) OutlinedButton(
            onClick = onRetryApply,
            enabled = canEdit && mapsEnabled,
            modifier = Modifier.padding(top = 8.dp).heightIn(min = 48.dp),
            shape = MaterialTheme.shapes.small,
        ) {
            YukiIcon(Icons.Filled.Refresh, null)
            Spacer(Modifier.width(8.dp))
            Text(stringResource(R.string.sumh_maps_path_apply))
        }
        MapsPreviewControls(enabled, rules, mapsEnabled)
        MapsTransferControls(rules, canEdit, onRulesChanged)
        TemporaryMapsControls(enabled)
    }
}

@Composable
private fun MapsPathRuleEditor(
    rules: List<MapsPathRule>,
    originalTarget: String?,
    enabled: Boolean,
    onRulesChanged: (List<MapsPathRule>) -> Unit,
    onDismiss: () -> Unit,
    onSaved: () -> Unit,
) {
    val original = rules.firstOrNull { it.targetPath == originalTarget }
    val drafts = rememberSUMHEditorDrafts()
    val draftKey = "maps:" + (originalTarget ?: "")
    val currentBaseline = MapsEditorDraft.baseline(original)
    val initialDraft = MapsEditorDraft(original?.targetPath.orEmpty(), original?.replacementPath.orEmpty(), currentBaseline)
    LaunchedEffect(drafts.loaded, draftKey, drafts.snapshot[draftKey]) {
        if (drafts.loaded) drafts.ensure(draftKey, initialDraft.record())
    }
    val draft = drafts.snapshot[draftKey]?.let(MapsEditorDraft::from) ?: initialDraft
    fun updateDraft(change: (MapsEditorDraft) -> MapsEditorDraft) = drafts.change(draftKey) {
        change(it?.let(MapsEditorDraft::from) ?: initialDraft).record()
    }
    val target = draft.target
    val replacement = draft.replacement
    val ready = drafts.loaded && draftKey in drafts.snapshot
    val conflict = draft.baseline != currentBaseline
    var editedFields by rememberSaveable { mutableIntStateOf(0) }
    var submitted by rememberSaveable { mutableStateOf(false) }
    val validation = validateSUMHMapsPathRule(target, replacement,
        rules.map { it.targetPath }, originalTarget)
    val candidate = remember(rules, originalTarget, target, replacement) {
        updateSUMHMapsPathRule(rules, originalTarget, MapsPathRule(target, replacement))
    }
    val withinBudget = remember(candidate) { candidate?.let(::mapsRulesWithinBudget) == true }
    val canSave = ready && !conflict && enabled && candidate != null && candidate != rules && validation.valid && withinBudget

    // Only a refreshed saved definition closes the editor. Failed saves retain the draft.
    LaunchedEffect(rules, draft) {
        if (drafts.snapshot[draftKey] == draft.record() && draft.pendingTarget.isNotEmpty() &&
            MapsPathRule(draft.pendingTarget, draft.pendingReplacement) in rules &&
            (originalTarget == null || originalTarget == draft.pendingTarget || rules.none { it.targetPath == originalTarget })) {
            if (target == draft.pendingTarget && replacement == draft.pendingReplacement) {
                drafts.forget(draftKey, draft.record())
                onSaved()
            } else if (originalTarget == draft.pendingTarget) {
                // A readback of an earlier same-target save must not close a newer draft.
                updateDraft { it.copy(baseline = currentBaseline, pendingTarget = "", pendingReplacement = "") }
            }
        }
    }

    val targetFocus = remember { FocusRequester() }
    val replacementFocus = remember { FocusRequester() }
    fun submit() {
        if (!ready || conflict || !enabled || candidate == null || candidate == rules || drafts.snapshot[draftKey] != draft.record()) return
        submitted = true
        if (!validation.valid || !withinBudget) {
            if (validation.targetError != null || validation.duplicateTarget) targetFocus.requestFocus()
            else replacementFocus.requestFocus()
            return
        }
        if (!updateDraft { it.copy(pendingTarget = target, pendingReplacement = replacement) }) return
        onRulesChanged(requireNotNull(candidate))
    }
    YukiAlertDialog(
        onDismissRequest = onDismiss,
        modifier = Modifier.padding(horizontal = 24.dp).widthIn(max = 560.dp).fillMaxWidth(),
        properties = DialogProperties(usePlatformDefaultWidth = false),
        title = { Text(stringResource(if (originalTarget == null) R.string.sumh_maps_path_add else R.string.sumh_maps_path_edit)) },
        text = {
            Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                SUMHEditorDraftStatus(drafts)
                if (conflict) {
                    Text(stringResource(R.string.sumh_editor_draft_conflict), color = MaterialTheme.colorScheme.error)
                    TextButton(onClick = { updateDraft { initialDraft } }) { Text(stringResource(R.string.sumh_editor_draft_reload)) }
                    if (originalTarget == null || original != null) TextButton(onClick = {
                        updateDraft { it.copy(baseline = currentBaseline, pendingTarget = "", pendingReplacement = "") }
                    }) { Text(stringResource(R.string.sumh_editor_draft_keep)) }
                }
                Text(stringResource(R.string.sumh_maps_path_hint), style = MaterialTheme.typography.bodySmall)
                val targetError = if (submitted || editedFields and 1 != 0) {
                    if (validation.duplicateTarget) R.string.sumh_maps_path_duplicate
                    else mapsPathErrorResource(validation.targetError, target = true)
                } else null
                val replacementError = if (submitted || editedFields and 2 != 0)
                    mapsPathErrorResource(validation.replacementError, target = false) else null
                OutlinedTextField(
                    value = target,
                    onValueChange = { value -> updateDraft {
                        it.copy(target = value,
                            pendingTarget = it.pendingTarget.takeIf { pending -> pending == originalTarget }.orEmpty(),
                            pendingReplacement = it.pendingReplacement.takeIf { _ -> it.pendingTarget == originalTarget }.orEmpty())
                    }; editedFields = editedFields or 1 },
                    label = { Text(stringResource(R.string.sumh_maps_path_target)) },
                    modifier = Modifier.fillMaxWidth().focusRequester(targetFocus),
                    enabled = ready && enabled,
                    singleLine = true,
                    shape = MaterialTheme.shapes.small,
                    isError = targetError != null,
                    supportingText = targetError?.let { error -> { Text(stringResource(error)) } },
                    keyboardOptions = KeyboardOptions(autoCorrectEnabled = false, imeAction = ImeAction.Next),
                    keyboardActions = KeyboardActions(onNext = { replacementFocus.requestFocus() }),
                )
                OutlinedTextField(
                    value = replacement,
                    onValueChange = { value -> updateDraft {
                        it.copy(replacement = value,
                            pendingTarget = it.pendingTarget.takeIf { pending -> pending == originalTarget }.orEmpty(),
                            pendingReplacement = it.pendingReplacement.takeIf { _ -> it.pendingTarget == originalTarget }.orEmpty())
                    }; editedFields = editedFields or 2 },
                    label = { Text(stringResource(R.string.sumh_maps_path_replacement)) },
                    modifier = Modifier.fillMaxWidth().focusRequester(replacementFocus),
                    enabled = ready && enabled,
                    singleLine = true,
                    shape = MaterialTheme.shapes.small,
                    isError = replacementError != null,
                    supportingText = replacementError?.let { error -> { Text(stringResource(error)) } },
                    keyboardOptions = KeyboardOptions(autoCorrectEnabled = false, imeAction = ImeAction.Done),
                    keyboardActions = KeyboardActions(onDone = { submit() }),
                )
                if (validation.limitReached) Text(stringResource(R.string.sumh_maps_path_limit), color = MaterialTheme.colorScheme.error)
                if (candidate == null) Text(stringResource(R.string.sumh_maps_path_missing), color = MaterialTheme.colorScheme.error)
                else if (!withinBudget) Text(stringResource(R.string.sumh_maps_path_budget), color = MaterialTheme.colorScheme.error)
            }
        },
        confirmButton = {
            TextButton(onClick = { submit() }, enabled = canSave, modifier = Modifier.heightIn(min = 48.dp)) {
                Text(stringResource(R.string.sumh_rules_save))
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss, modifier = Modifier.heightIn(min = 48.dp)) {
                Text(stringResource(R.string.sumh_rules_cancel))
            }
        },
    )
}

private fun mapsPathErrorResource(error: SUMHMapsPathError?, target: Boolean): Int? = when (error) {
    SUMHMapsPathError.AbsolutePath -> R.string.sumh_maps_input_path_absolute
    SUMHMapsPathError.ControlCharacter -> R.string.sumh_maps_path_control
    SUMHMapsPathError.TooLong -> if (target) R.string.sumh_maps_path_target_length else R.string.sumh_maps_input_path_length
    null -> null
}
