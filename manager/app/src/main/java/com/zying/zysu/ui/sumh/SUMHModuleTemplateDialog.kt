package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.sumh.util.*
import com.zying.zysu.ui.sumh.util.SUMHManager.ModuleRule

@Composable internal fun SUMHModuleTemplateDialog(
    moduleId: String, moduleName: String, currentDraft: ModuleEditorDraft, drafts: SUMHEditorDrafts,
    externalConflict: Boolean, enabled: Boolean, globalMode: String, onDismiss: () -> Unit,
    onApply: (EditorDraftRecord, List<ModuleRule>) -> Boolean,
) {
    val key = "module-template:$moduleId"
    val currentRecord = currentDraft.record()
    val baseline = draftFingerprint(currentRecord.fields)
    val initial = ModuleTemplateDraft(baseline = baseline)
    LaunchedEffect(drafts.loaded, key, drafts.snapshot[key]) {
        if (drafts.loaded) drafts.ensure(key, initial.record())
    }
    val draft = drafts.snapshot[key]?.let(ModuleTemplateDraft::from) ?: initial
    val ready = enabled && drafts.loaded && key in drafts.snapshot
    var reviewed by rememberSaveable(moduleId) { mutableStateOf("") }
    fun update(change: (ModuleTemplateDraft) -> ModuleTemplateDraft) {
        drafts.change(key) { change(it?.let(ModuleTemplateDraft::from) ?: initial).record() }
        reviewed = ""
    }
    val result = remember(draft.template, draft.paths, draft.parent) {
        moduleTemplateRules(draft.template, draft.paths, draft.parent)
    }
    val signature = draftFingerprint(listOf(draft.template.name, draft.paths, draft.parent, baseline, globalMode, enabled.toString()))
    LaunchedEffect(signature, externalConflict) {
        if (externalConflict || (reviewed.isNotEmpty() && reviewed != signature)) reviewed = ""
    }
    val validReview = ready && !externalConflict && draft.baseline == baseline && result.rules != null && reviewed == signature
    val changed = draft.baseline != baseline

    YukiAlertDialog(
        onDismissRequest = onDismiss,
        modifier = Modifier.padding(horizontal = 24.dp).widthIn(max = 560.dp).fillMaxWidth(),
        properties = DialogProperties(usePlatformDefaultWidth = false),
        title = { Column { Text(stringResource(R.string.sumh_template_open)); Text(moduleName, style = MaterialTheme.typography.bodyMedium) } },
        text = {
            Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                SUMHEditorDraftStatus(drafts)
                Text(stringResource(R.string.sumh_template_intro), style = MaterialTheme.typography.bodyMedium)
                if (externalConflict) Text(stringResource(R.string.sumh_template_external), color = MaterialTheme.colorScheme.error)
                else if (changed) Text(stringResource(R.string.sumh_template_draft_changed), color = MaterialTheme.colorScheme.error)
                if (globalMode !in listOf("auto", "sumh")) Text(stringResource(R.string.sumh_template_override, globalMode))
                Text(stringResource(R.string.sumh_template_limits), style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    SUMHModuleTemplate.entries.forEach { template ->
                        FilterChip(selected = draft.template == template, enabled = ready,
                            onClick = { update { it.copy(template = template) } },
                            label = { Text(stringResource(templateLabel(template))) })
                    }
                }
                if (draft.template == SUMHModuleTemplate.ExceptChildren) OutlinedTextField(
                    value = draft.parent, onValueChange = { value -> update { it.copy(parent = value) } }, enabled = ready,
                    label = { Text(stringResource(R.string.sumh_template_parent)) },
                    modifier = Modifier.fillMaxWidth(), singleLine = true, keyboardOptions = KeyboardOptions(autoCorrectEnabled = false),
                )
                if (draft.template != SUMHModuleTemplate.All) OutlinedTextField(
                    value = draft.paths, onValueChange = { value -> update { it.copy(paths = value) } }, enabled = ready,
                    label = { Text(stringResource(if (draft.template == SUMHModuleTemplate.ExceptChildren)
                        R.string.sumh_template_children else R.string.sumh_template_paths)) },
                    supportingText = { Text(stringResource(R.string.sumh_template_path_hint)) },
                    modifier = Modifier.fillMaxWidth(), minLines = 3, maxLines = 6,
                    keyboardOptions = KeyboardOptions(autoCorrectEnabled = false),
                )
                result.ruleError?.let { Text(stringResource(moduleRuleErrorResource(it)), color = MaterialTheme.colorScheme.error) }
                result.templateError?.let { issue -> Text(stringResource(when (issue) {
                    SUMHModuleTemplateError.PathsRequired -> R.string.sumh_template_paths_required
                    SUMHModuleTemplateError.ChildOutsideParent -> R.string.sumh_template_child_outside
                }), color = MaterialTheme.colorScheme.error) }
                OutlinedButton(enabled = ready && !externalConflict && result.rules != null,
                    onClick = {
                        if (drafts.snapshot[key] == draft.record() && drafts.snapshot["module:$moduleId"] == currentRecord) {
                            if (drafts.change(key) { draft.copy(baseline = baseline).record() }) reviewed = signature
                        }
                    }, modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                ) { Text(stringResource(R.string.sumh_template_review)) }
                if (validReview) {
                    val generated = requireNotNull(result.rules)
                    val diff = moduleTemplateDiff(currentDraft.rules, generated)
                    HorizontalDivider()
                    Text(stringResource(R.string.sumh_template_review_title), style = MaterialTheme.typography.titleSmall)
                    Text(stringResource(R.string.sumh_template_mode_change, currentDraft.mode))
                    Text(stringResource(R.string.sumh_template_diff_count, diff.added.size, diff.changed.size, diff.removed.size))
                    diff.removed.forEach { rule -> TemplateDiffRow(R.string.sumh_template_removed, rule.path, rule.mode) }
                    diff.changed.forEach { (old, next) -> TemplateDiffRow(R.string.sumh_template_changed, next.path, "${old.mode} → ${next.mode}") }
                    diff.added.forEach { rule -> TemplateDiffRow(R.string.sumh_template_added, rule.path, rule.mode) }
                    Text(stringResource(R.string.sumh_template_complete, generated.size), style = MaterialTheme.typography.titleSmall)
                    if (generated.isEmpty()) Text(stringResource(R.string.sumh_template_empty))
                    generated.forEach { rule -> Text("${moduleTemplatePathLabel(rule.path)} → ${rule.mode}",
                        style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace) }
                    Text(stringResource(R.string.sumh_template_apply_hint), style = MaterialTheme.typography.bodySmall)
                }
            }
        },
        confirmButton = {
            Button(enabled = validReview, onClick = {
                if (validReview && drafts.snapshot[key] == draft.record() &&
                    drafts.snapshot["module:$moduleId"] == currentRecord && onApply(currentRecord, requireNotNull(result.rules))) {
                    drafts.forget(key, draft.record())
                }
            }) { Text(stringResource(R.string.sumh_template_apply)) }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(android.R.string.cancel)) } },
    )
}

@Composable private fun TemplateDiffRow(label: Int, path: String, value: String) {
    Text(stringResource(label, moduleTemplatePathLabel(path), value), style = MaterialTheme.typography.bodySmall,
        fontFamily = FontFamily.Monospace)
}

internal fun templateLabel(template: SUMHModuleTemplate): Int = when (template) {
    SUMHModuleTemplate.All -> R.string.sumh_template_all
    SUMHModuleTemplate.Exclude -> R.string.sumh_template_exclude
    SUMHModuleTemplate.Only -> R.string.sumh_template_only
    SUMHModuleTemplate.ExceptChildren -> R.string.sumh_template_except_children
    SUMHModuleTemplate.Hide -> R.string.sumh_template_hide
}
