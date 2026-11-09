package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Upload
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
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
import com.zying.zysu.ui.sumh.util.SUMHMapsImportError
import com.zying.zysu.ui.sumh.util.SUMHMapsImportIssue
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter.MapsPathRule
import com.zying.zysu.ui.sumh.util.previewSUMHMapsImport
import com.zying.zysu.ui.sumh.util.validateSUMHMapsPathRule
import com.zying.zysu.ui.sumh.util.mapsRulesWithinBudget
import com.zying.zysu.ui.sumh.util.updateSUMHMapsPathRule
import com.zying.zysu.ui.sumh.util.SUMH_MAX_MAPS_PATH_RULES

@Composable
internal fun ManagedMapsControls(
    rules: List<MapsPathRule>,
    enabled: Boolean,
    support: SUMHFeatureSupport,
    mapsEnabled: Boolean,
    onRulesChanged: (List<MapsPathRule>) -> Unit,
    headingActions: (@Composable RowScope.() -> Unit)? = null,
    onRetryApply: () -> Unit = {},
    runtimeApplyPending: Boolean = false,
    additionalActions: @Composable ColumnScope.() -> Unit = {},
) {
    var showEditor by rememberSaveable { mutableStateOf(false) }
    var originalTarget by rememberSaveable { mutableStateOf<String?>(null) }
    var removeTarget by rememberSaveable { mutableStateOf<String?>(null) }
    var showImport by rememberSaveable { mutableStateOf(false) }
    val drafts = rememberSaveableStateHolder()
    val canEdit = enabled && support == SUMHFeatureSupport.Supported
    val limitReached = rules.size >= SUMH_MAX_MAPS_PATH_RULES
    if (showEditor) {
        val key = originalTarget ?: "add"
        drafts.SaveableStateProvider(key) {
            MapsPathRuleEditor(
                rules = rules, originalTarget = originalTarget, enabled = canEdit,
                onRulesChanged = onRulesChanged,
                onDismiss = { showEditor = false },
                onSaved = { showEditor = false; drafts.removeState(key) },
            )
        }
    }
    if (showImport) {
        MapsPathRuleImport(
            rules = rules, enabled = canEdit, onRulesChanged = onRulesChanged,
            onDismiss = { showImport = false }, onSaved = { showImport = false },
        )
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

    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        SUMHRuleOverview(stringResource(R.string.sumh_maps_rules_page), headingActions) {
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
            if (limitReached) Text(stringResource(R.string.sumh_maps_path_limit), style = MaterialTheme.typography.bodySmall)
            SUMHRuleActions {
                FilledTonalButton(
                    onClick = { if (canEdit) { originalTarget = null; showEditor = true } },
                    enabled = canEdit && !limitReached,
                    modifier = Modifier.heightIn(min = 48.dp),
                ) {
                    YukiIcon(Icons.Filled.Add, null)
                    Spacer(Modifier.width(8.dp))
                    Text(stringResource(R.string.sumh_maps_path_add))
                }
                OutlinedButton(
                    onClick = { showImport = true },
                    enabled = canEdit,
                    modifier = Modifier.heightIn(min = 48.dp),
                ) {
                    YukiIcon(Icons.Filled.Upload, null)
                    Spacer(Modifier.width(8.dp))
                    Text(stringResource(R.string.sumh_maps_import_title))
                }
                if (rules.isNotEmpty() && !runtimeApplyPending) OutlinedButton(
                    onClick = onRetryApply,
                    enabled = canEdit && mapsEnabled,
                    modifier = Modifier.heightIn(min = 48.dp),
                ) {
                    YukiIcon(Icons.Filled.Refresh, null)
                    Spacer(Modifier.width(8.dp))
                    Text(stringResource(R.string.sumh_maps_path_apply))
                }
            }
            additionalActions()
        }
        if (rules.isEmpty()) {
            SUMHRuleEmptyState(stringResource(R.string.sumh_maps_empty))
        }
        rules.forEach { rule ->
            key(rule.targetPath) {
                SUMHRuleCard {
                    Text(stringResource(R.string.sumh_maps_path_target), style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Text(rule.targetPath, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodyMedium,
                        maxLines = 3, overflow = TextOverflow.Ellipsis)
                    Text(stringResource(R.string.sumh_maps_path_replacement), style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Text(rule.replacementPath, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodyMedium,
                        maxLines = 3, overflow = TextOverflow.Ellipsis)
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                        IconButton(onClick = { originalTarget = rule.targetPath; showEditor = true }, enabled = canEdit) {
                            YukiIcon(Icons.Filled.Edit, stringResource(R.string.sumh_maps_path_edit_label, rule.targetPath))
                        }
                        IconButton(onClick = { removeTarget = rule.targetPath }, enabled = enabled) {
                            YukiIcon(Icons.Filled.Delete, stringResource(R.string.sumh_maps_path_remove_label, rule.targetPath))
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun MapsPathRuleImport(
    rules: List<MapsPathRule>,
    enabled: Boolean,
    onRulesChanged: (List<MapsPathRule>) -> Unit,
    onDismiss: () -> Unit,
    onSaved: () -> Unit,
) {
    var input by rememberSaveable { mutableStateOf("") }
    var allowReplace by rememberSaveable { mutableStateOf(false) }
    var pendingCandidate by remember { mutableStateOf<List<MapsPathRule>?>(null) }
    val preview = remember(input, rules) { previewSUMHMapsImport(input, rules) }
    val canSubmit = enabled && preview.canImport && (preview.replacements.isEmpty() || allowReplace)

    LaunchedEffect(rules, pendingCandidate) {
        if (pendingCandidate != null && rules == pendingCandidate) onSaved()
    }

    YukiAlertDialog(
        onDismissRequest = onDismiss,
        modifier = Modifier.padding(horizontal = 24.dp).widthIn(max = 560.dp).fillMaxWidth(),
        properties = DialogProperties(usePlatformDefaultWidth = false),
        title = { Text(stringResource(R.string.sumh_maps_import_title)) },
        text = {
            Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(stringResource(R.string.sumh_maps_import_format), style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                OutlinedTextField(
                    value = input,
                    onValueChange = { input = it; pendingCandidate = null; allowReplace = false },
                    label = { Text(stringResource(R.string.sumh_maps_import_input)) },
                    modifier = Modifier.fillMaxWidth(),
                    enabled = enabled,
                    minLines = 5,
                    maxLines = 9,
                    shape = MaterialTheme.shapes.small,
                    textStyle = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                )
                Text(stringResource(R.string.sumh_maps_import_preview, preview.additions,
                    preview.replacements.size, preview.issues.size), style = MaterialTheme.typography.bodyMedium)
                if (preview.limitExceeded) Text(stringResource(R.string.sumh_maps_path_limit),
                    color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
                if (preview.budgetExceeded) Text(stringResource(R.string.sumh_maps_path_budget),
                    color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
                if (!preview.hasChanges && preview.issues.isEmpty()) Text(
                    stringResource(R.string.sumh_maps_import_empty), style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                preview.issues.forEach { issue ->
                    Text(stringResource(R.string.sumh_maps_import_line_error, issue.line,
                        stringResource(mapsImportErrorResource(issue))),
                        color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
                }
                if (preview.replacements.isNotEmpty()) {
                    Text(stringResource(R.string.sumh_maps_import_replacements),
                        style = MaterialTheme.typography.labelMedium)
                    preview.replacements.forEach { rule ->
                        Text(rule.targetPath, fontFamily = FontFamily.Monospace,
                            style = MaterialTheme.typography.bodySmall)
                    }
                    Row(Modifier.fillMaxWidth().toggleable(value = allowReplace, enabled = enabled,
                        onValueChange = { allowReplace = it }), verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(checked = allowReplace, onCheckedChange = null, enabled = enabled)
                        Text(stringResource(R.string.sumh_maps_import_allow_replace),
                            style = MaterialTheme.typography.bodySmall)
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = {
                if (canSubmit) {
                    pendingCandidate = preview.candidate
                    onRulesChanged(preview.candidate)
                }
            }, enabled = canSubmit, modifier = Modifier.heightIn(min = 48.dp)) {
                Text(stringResource(R.string.sumh_maps_import_confirm))
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss, modifier = Modifier.heightIn(min = 48.dp)) {
                Text(stringResource(R.string.sumh_rules_cancel))
            }
        },
    )
}

private fun mapsImportErrorResource(issue: SUMHMapsImportIssue): Int = when (issue.error) {
    SUMHMapsImportError.Format -> R.string.sumh_maps_import_format_error
    SUMHMapsImportError.TargetAbsolute, SUMHMapsImportError.ReplacementAbsolute -> R.string.sumh_maps_input_path_absolute
    SUMHMapsImportError.TargetControl, SUMHMapsImportError.ReplacementControl -> R.string.sumh_maps_path_control
    SUMHMapsImportError.TargetTooLong -> R.string.sumh_maps_path_target_length
    SUMHMapsImportError.ReplacementTooLong -> R.string.sumh_maps_input_path_length
    SUMHMapsImportError.DuplicateTarget -> R.string.sumh_maps_import_duplicate
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
    var target by rememberSaveable { mutableStateOf(original?.targetPath.orEmpty()) }
    var replacement by rememberSaveable { mutableStateOf(original?.replacementPath.orEmpty()) }
    var editedFields by rememberSaveable { mutableIntStateOf(0) }
    var submitted by rememberSaveable { mutableStateOf(false) }
    var pendingSubmission by rememberSaveable { mutableStateOf(false) }
    val validation = validateSUMHMapsPathRule(target, replacement,
        rules.map { it.targetPath }, originalTarget)
    val candidate = remember(rules, originalTarget, target, replacement) {
        updateSUMHMapsPathRule(rules, originalTarget, MapsPathRule(target, replacement))
    }
    val withinBudget = remember(candidate) { candidate?.let(::mapsRulesWithinBudget) == true }
    val canSave = enabled && candidate != null && candidate != rules && validation.valid && withinBudget

    // Only a refreshed saved definition closes the editor. Failed saves retain the draft.
    LaunchedEffect(rules, pendingSubmission) {
        if (pendingSubmission && MapsPathRule(target, replacement) in rules &&
            (originalTarget == null || originalTarget == target || rules.none { it.targetPath == originalTarget })) {
            onSaved()
        }
    }

    val targetFocus = remember { FocusRequester() }
    val replacementFocus = remember { FocusRequester() }
    fun submit() {
        if (!enabled || candidate == null || candidate == rules) return
        submitted = true
        if (!validation.valid || !withinBudget) {
            if (validation.targetError != null || validation.duplicateTarget) targetFocus.requestFocus()
            else replacementFocus.requestFocus()
            return
        }
        pendingSubmission = true
        onRulesChanged(requireNotNull(candidate))
    }
    YukiAlertDialog(
        onDismissRequest = onDismiss,
        modifier = Modifier.padding(horizontal = 24.dp).widthIn(max = 560.dp).fillMaxWidth(),
        properties = DialogProperties(usePlatformDefaultWidth = false),
        title = { Text(stringResource(if (originalTarget == null) R.string.sumh_maps_path_add else R.string.sumh_maps_path_edit)) },
        text = {
            Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.sumh_maps_path_hint), style = MaterialTheme.typography.bodySmall)
                val targetError = if (submitted || editedFields and 1 != 0) {
                    if (validation.duplicateTarget) R.string.sumh_maps_path_duplicate
                    else mapsPathErrorResource(validation.targetError, target = true)
                } else null
                val replacementError = if (submitted || editedFields and 2 != 0)
                    mapsPathErrorResource(validation.replacementError, target = false) else null
                OutlinedTextField(
                    value = target,
                    onValueChange = { target = it; editedFields = editedFields or 1; pendingSubmission = false },
                    label = { Text(stringResource(R.string.sumh_maps_path_target)) },
                    modifier = Modifier.fillMaxWidth().focusRequester(targetFocus),
                    enabled = enabled,
                    singleLine = true,
                    shape = MaterialTheme.shapes.small,
                    isError = targetError != null,
                    supportingText = targetError?.let { error -> { Text(stringResource(error)) } },
                    keyboardOptions = KeyboardOptions(autoCorrectEnabled = false, imeAction = ImeAction.Next),
                    keyboardActions = KeyboardActions(onNext = { replacementFocus.requestFocus() }),
                )
                OutlinedTextField(
                    value = replacement,
                    onValueChange = { replacement = it; editedFields = editedFields or 2; pendingSubmission = false },
                    label = { Text(stringResource(R.string.sumh_maps_path_replacement)) },
                    modifier = Modifier.fillMaxWidth().focusRequester(replacementFocus),
                    enabled = enabled,
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
