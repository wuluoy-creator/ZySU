package com.zying.zysu.ui.sumh

import androidx.activity.compose.LocalActivity
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.sumh.util.SUMHManager
import com.zying.zysu.ui.sumh.util.SUMHModuleRuleError
import com.zying.zysu.ui.sumh.util.updateSUMHModuleRule
import com.zying.zysu.ui.sumh.util.validateSUMHModuleRulePath
import com.zying.zysu.ui.sumh.util.validateSUMHModuleRules
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch
import java.util.UUID

internal val SUMH_MODE_COLORS = mapOf(
    "auto" to Color(0xFF1976D2),
    "sumh" to Color(0xFF388E3C),
    "overlay" to Color(0xFFF57C00),
    "magic" to Color(0xFF7B1FA2),
    "none" to Color(0xFF616161)
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun SUMHMountConfigDialog(
    moduleId: String,
    moduleName: String,
    initialInfo: SUMHManager.ModuleInfo?,
    sumhAvailable: Boolean,
    globalMode: String,
    onDismiss: () -> Unit,
    onSaved: () -> Unit,
    onSave: suspend (String, String, List<SUMHManager.ModuleRule>) -> Unit = { id, mode, rules ->
        SUMHManager.saveModule(id, mode, rules)
    },
    previewOperation: SUMHModulePreviewOperations = rememberSUMHModulePreviewOperations(),
    previewRead: ModulePreviewRead = SUMHManager::previewModule,
) {
    val resources = LocalResources.current
    val scope = rememberCoroutineScope()
    var error by remember(moduleId) { mutableStateOf<String?>(null) }
    val selectableModes = SUMHManager.mountModes.filter { it != "sumh" || sumhAvailable }
    val selectableRuleModes = listOf("auto", "sumh", "none", "hide").filter { sumhAvailable || it !in listOf("sumh", "hide") }

    val drafts = rememberSUMHEditorDrafts()
    val draftKey = "module:$moduleId"
    val currentBaseline = ModuleEditorDraft.baseline(initialInfo?.mode ?: "auto", initialInfo?.rules.orEmpty())
    val initialDraft = ModuleEditorDraft(initialInfo?.mode ?: "auto", initialInfo?.rules.orEmpty(),
        newMode = if (sumhAvailable) "sumh" else "auto", baseline = currentBaseline)
    LaunchedEffect(drafts.loaded, draftKey, drafts.snapshot[draftKey]) {
        if (drafts.loaded) drafts.ensure(draftKey, initialDraft.record())
    }
    val draft = drafts.snapshot[draftKey]?.let(ModuleEditorDraft::from) ?: initialDraft
    fun updateDraft(change: (ModuleEditorDraft) -> ModuleEditorDraft) = drafts.change(draftKey) {
        change(it?.let(ModuleEditorDraft::from) ?: initialDraft).record()
    }
    val selectedMode = draft.mode
    val rules = draft.rules
    val newPath = draft.newPath
    val newMode = draft.newMode
    val editingPath = draft.editingPath
    val ready = drafts.loaded && draftKey in drafts.snapshot
    val conflict = draft.baseline != currentBaseline
    var modeExpanded by remember { mutableStateOf(false) }
    var isSaving by remember { mutableStateOf(false) }
    var rulesExpanded by rememberSaveable(moduleId) { mutableStateOf(false) }
    var templatesOpen by rememberSaveable(moduleId) { mutableStateOf(false) }
    var previewOpen by rememberSaveable(moduleId) { mutableStateOf(false) }
    var previewSession by rememberSaveable(moduleId) { mutableStateOf(UUID.randomUUID().toString()) }
    var previewPath by rememberSaveable(moduleId) { mutableStateOf("") }
    var previewPathTooLong by remember(moduleId) { mutableStateOf(false) }
    val managerRevision by SUMHManager.revision.collectAsState()
    val activity = LocalActivity.current
    val hasDraft = newPath.isNotEmpty() || editingPath != null
    val candidate = remember(rules, editingPath, newPath, newMode) {
        if (hasDraft) updateSUMHModuleRule(rules, editingPath, SUMHManager.ModuleRule(newPath, newMode)) else rules
    }
    val draftError = when {
        !hasDraft -> null
        validateSUMHModuleRulePath(newPath) != null -> validateSUMHModuleRulePath(newPath)
        rules.any { it.path == newPath && it.path != editingPath } -> SUMHModuleRuleError.DuplicatePath
        else -> null
    }
    val rulesError = remember(candidate) { candidate?.let(::validateSUMHModuleRules) }
    val original = rules.firstOrNull { it.path == editingPath }
    val editorModes = selectableRuleModes + listOfNotNull(original?.mode?.takeIf { it in listOf("overlay", "magic") })
    val draftModeAvailable = !hasDraft || newMode in editorModes || original == SUMHManager.ModuleRule(newPath, newMode)
    val canSave = ready && !conflict && !isSaving && candidate != null && rulesError == null && draftModeAvailable
    val previewContext = SUMHModulePreviewContext(draft.record(), drafts.editEpoch, currentBaseline,
        globalMode, sumhAvailable, initialInfo?.strategy.orEmpty(), managerRevision, previewPath,
        ready && !conflict && !isSaving && !hasDraft && rulesError == null && selectedMode in SUMHManager.mountModes)

    if (previewOpen) {
        SideEffect { previewOperation.attach(previewSession, moduleId, previewContext) }
        DisposableEffect(previewOperation, previewSession) {
            val token = previewSession
            onDispose { if (activity?.isChangingConfigurations != true) previewOperation.close(token) }
        }
        SUMHModulePreviewDialog(
            moduleId = moduleId, explainPath = previewPath,
            onExplainPathChange = { value ->
                previewPathTooLong = !Charsets.UTF_8.newEncoder().canEncode(value) || value.toByteArray(Charsets.UTF_8).size > 4095
                if (!previewPathTooLong) {
                    previewPath = value
                    previewOperation.observe(previewSession, previewContext.copy(explainPath = value))
                }
            },
            busy = previewOperation.busy, stale = previewOperation.stale,
            canReadSaved = ready && !isSaving && !previewPathTooLong,
            canReadCandidate = previewContext.candidateAllowed && !previewPathTooLong,
            result = previewOperation.result, request = previewOperation.request,
            error = if (previewPathTooLong) stringResource(R.string.sumh_module_preview_path_limit) else null,
            onReadSaved = { previewOperation.read(previewSession, previewContext, false, previewRead) },
            onReadCandidate = { previewOperation.read(previewSession, previewContext, true, previewRead) },
            onClose = { previewOperation.close(previewSession); previewOpen = false },
            eligibilityMessage = when {
                hasDraft -> stringResource(R.string.sumh_module_preview_finish_edit)
                conflict -> stringResource(R.string.sumh_editor_draft_conflict)
                rulesError != null -> stringResource(moduleRuleErrorResource(rulesError))
                else -> null
            },
        )
    }

    fun cancelEdit() {
        updateDraft { it.copy(editingPath = null, newPath = "", newMode = if (sumhAvailable) "sumh" else "auto") }
        error = null
    }

    val modeLabels = mapOf(
        "auto" to stringResource(R.string.sumh_mount_mode_auto),
        "sumh" to stringResource(R.string.sumh_mount_mode_sumh),
        "overlay" to stringResource(R.string.sumh_mount_mode_overlay),
        "magic" to stringResource(R.string.sumh_mount_mode_magic),
        "none" to stringResource(R.string.sumh_mount_mode_none),
        "hide" to stringResource(R.string.sumh_mount_mode_hide),
    )

    if (templatesOpen) SUMHModuleTemplateDialog(
        moduleId = moduleId, moduleName = moduleName, currentDraft = draft, drafts = drafts,
        externalConflict = conflict, enabled = ready && !isSaving && !hasDraft && sumhAvailable,
        globalMode = globalMode, onDismiss = { templatesOpen = false },
        onApply = { expected, generated ->
            var applied = false
            val changed = drafts.change(draftKey) { existing ->
                if (existing == expected && !conflict && !isSaving && sumhAvailable) {
                    applied = true
                    ModuleEditorDraft.from(requireNotNull(existing)).copy(mode = "sumh", rules = generated).record()
                } else existing
            }
            if (changed && applied) { error = null; templatesOpen = false; rulesExpanded = true }
            changed && applied
        },
    )

    if (!templatesOpen && !previewOpen) YukiAlertDialog(
        onDismissRequest = { if (!isSaving) onDismiss() },
        modifier = Modifier.padding(horizontal = 24.dp).widthIn(max = 560.dp).fillMaxWidth(),
        properties = DialogProperties(usePlatformDefaultWidth = false),
        title = {
            Column {
                Text(stringResource(R.string.sumh_mount_config))
                Text(
                    text = moduleName,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        },
        text = {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                SUMHEditorDraftStatus(drafts)
                if (conflict) {
                    Text(stringResource(R.string.sumh_editor_draft_conflict), color = MaterialTheme.colorScheme.error)
                    TextButton(onClick = { updateDraft { initialDraft } }) {
                        Text(stringResource(R.string.sumh_editor_draft_reload))
                    }
                    TextButton(onClick = { updateDraft { it.copy(baseline = currentBaseline) } }) {
                        Text(stringResource(R.string.sumh_editor_draft_keep))
                    }
                }
                error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                if (!rulesExpanded && rulesError != null) Text(stringResource(moduleRuleErrorResource(rulesError)),
                    color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
                Text(stringResource(R.string.sumh_module_rules_reboot), style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                if (globalMode != "auto") Text(stringResource(R.string.sumh_global_override, globalMode))
                OutlinedButton(
                    enabled = ready && !isSaving && !hasDraft && sumhAvailable,
                    onClick = { templatesOpen = true }, modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                ) { Text(stringResource(R.string.sumh_template_open)) }
                OutlinedButton(
                    enabled = ready && !isSaving && !hasDraft,
                    onClick = { previewSession = UUID.randomUUID().toString(); previewOpen = true },
                    modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("module_preview_open"),
                ) { Text(stringResource(R.string.sumh_module_preview_title)) }
                if (hasDraft) Text(stringResource(R.string.sumh_template_finish_edit),
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Text(
                    text = stringResource(R.string.sumh_mount_mode),
                    style = MaterialTheme.typography.titleSmall
                )
                FlowRow(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    selectableModes.forEach { mode ->
                        FilterChip(
                            selected = selectedMode == mode,
                            onClick = { updateDraft { it.copy(mode = mode) } },
                            enabled = ready && !isSaving,
                            label = { Text(modeLabels[mode] ?: mode) },
                            leadingIcon = {
                                YukiIcon(
                                    if (selectedMode == mode) Icons.Outlined.Check else when (mode) {
                                        "sumh" -> Icons.Outlined.Memory
                                        "overlay" -> Icons.Outlined.Layers
                                        "magic" -> Icons.Outlined.AutoFixHigh
                                        "none" -> Icons.Outlined.Block
                                        else -> Icons.Outlined.Settings
                                    },
                                    null, Modifier.size(FilterChipDefaults.IconSize),
                                )
                            }
                        )
                    }
                }

                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(min = 48.dp)
                        .clickable(enabled = ready && !isSaving, role = Role.Button) { rulesExpanded = !rulesExpanded }
                        .padding(vertical = 8.dp),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        text = stringResource(R.string.sumh_module_rules_title),
                        modifier = Modifier.weight(1f),
                        style = MaterialTheme.typography.titleSmall
                    )
                    YukiIcon(
                        imageVector = if (rulesExpanded) Icons.Outlined.ExpandLess else Icons.Outlined.ExpandMore,
                        contentDescription = stringResource(if (rulesExpanded) R.string.collapse_menu else R.string.expand_menu)
                    )
                }
                Column(
                    modifier = Modifier.fillMaxWidth()
                ) {
                    if (rulesExpanded) {
                        Column(
                            modifier = Modifier.fillMaxWidth(),
                            verticalArrangement = Arrangement.spacedBy(12.dp)
                        ) {
                        Text(stringResource(R.string.sumh_module_rules_hint), style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        if (!sumhAvailable) Text(stringResource(R.string.sumh_module_rules_unavailable),
                            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        rules.forEach { rule: SUMHManager.ModuleRule ->
                            Row(
                                modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                                    Text(rule.path, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
                                    Text(
                                        modeLabels[rule.mode] ?: rule.mode,
                                        style = MaterialTheme.typography.labelSmall,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant
                                    )
                                }
                                IconButton(
                                    enabled = ready && !isSaving,
                                    onClick = { updateDraft { it.copy(editingPath = rule.path, newPath = rule.path, newMode = rule.mode) }; error = null }
                                ) { YukiIcon(Icons.Outlined.Edit, stringResource(R.string.sumh_module_rule_edit_label, rule.path)) }
                                IconButton(
                                    enabled = ready && !isSaving,
                                    onClick = {
                                        updateDraft { it.copy(rules = it.rules.filter { r -> r != rule }) }
                                        if (editingPath == rule.path) cancelEdit()
                                        error = null
                                    }
                                ) {
                                    YukiIcon(Icons.Outlined.Delete, stringResource(R.string.sumh_module_rule_remove_label, rule.path))
                                }
                            }
                        }
                        OutlinedTextField(
                            value = newPath,
                            onValueChange = { value -> updateDraft { it.copy(newPath = value) }; error = null },
                            enabled = ready && !isSaving,
                            isError = draftError != null,
                            supportingText = draftError?.let { issue -> { Text(stringResource(moduleRuleErrorResource(issue))) } },
                            label = { Text(stringResource(R.string.sumh_absolute_path)) },
                            placeholder = { Text(stringResource(R.string.sumh_module_rules_placeholder)) },
                            modifier = Modifier.fillMaxWidth(),
                            singleLine = true,
                            keyboardOptions = KeyboardOptions(autoCorrectEnabled = false),
                        )
                        if (rulesError != null && rulesError != draftError) Text(
                            stringResource(moduleRuleErrorResource(rulesError)), color = MaterialTheme.colorScheme.error,
                            style = MaterialTheme.typography.bodySmall,
                        )
                        if (!draftModeAvailable) Text(stringResource(R.string.sumh_module_rules_unavailable),
                            color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
                        if (newMode in listOf("overlay", "magic")) Text(stringResource(R.string.sumh_module_rules_legacy_mode),
                            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        FlowRow(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                            verticalArrangement = Arrangement.spacedBy(4.dp)
                        ) {
                            Box {
                                FilledTonalButton(
                                    enabled = ready && !isSaving,
                                    onClick = { modeExpanded = true },
                                    modifier = Modifier.heightIn(min = 48.dp)
                                ) {
                                    Text(modeLabels[newMode] ?: newMode)
                                    YukiIcon(Icons.Outlined.ArrowDropDown, contentDescription = null)
                                }
                                DropdownMenu(
                                    expanded = modeExpanded,
                                    onDismissRequest = { modeExpanded = false }
                                ) {
                                    editorModes.forEach { mode ->
                                        DropdownMenuItem(
                                            text = { Text(modeLabels[mode] ?: mode) },
                                            onClick = {
                                                updateDraft { it.copy(newMode = mode) }
                                                modeExpanded = false
                                            }
                                        )
                                    }
                                }
                            }
                            FilledTonalButton(
                                enabled = canSave && hasDraft,
                                onClick = {
                                    if (canSave && drafts.snapshot[draftKey] == draft.record()) {
                                        if (updateDraft { it.copy(rules = requireNotNull(candidate)) }) cancelEdit()
                                    }
                                },
                                modifier = Modifier.heightIn(min = 48.dp)
                            ) {
                                Text(stringResource(if (editingPath == null) R.string.sumh_module_rules_add else R.string.sumh_rule_save))
                            }
                            if (hasDraft) TextButton(
                                enabled = ready && !isSaving, onClick = { cancelEdit() }, modifier = Modifier.heightIn(min = 48.dp),
                            ) { Text(stringResource(R.string.sumh_module_rule_cancel_edit)) }
                        }
                        }
                    }
                }
            }
        },
        confirmButton = {
            Button(
                onClick = {
                    if (isSaving || !canSave || drafts.snapshot[draftKey] != draft.record()) return@Button
                    val rulesToSave = candidate
                    val modeToSave = selectedMode
                    val submittedDraft = draft.record()
                    isSaving = true
                    scope.launch {
                        try {
                            onSave(moduleId, modeToSave, rulesToSave)
                            drafts.forget(draftKey, submittedDraft)
                            onSaved()
                        } catch (e: Exception) {
                            if (e is CancellationException) throw e
                            error = e.message ?: resources.getString(R.string.operation_failed)
                        } finally {
                            isSaving = false
                        }
                    }
                },
                enabled = canSave
            ) {
                Text(stringResource(android.R.string.ok))
            }
        },
        dismissButton = {
            TextButton(enabled = !isSaving, onClick = onDismiss) {
                Text(stringResource(android.R.string.cancel))
            }
        }
    )
}

internal fun moduleRuleErrorResource(error: SUMHModuleRuleError): Int = when (error) {
    SUMHModuleRuleError.AbsolutePath -> R.string.sumh_maps_input_path_absolute
    SUMHModuleRuleError.ControlCharacter -> R.string.sumh_maps_path_control
    SUMHModuleRuleError.TooLong -> R.string.sumh_maps_path_target_length
    SUMHModuleRuleError.NonCanonicalPath -> R.string.sumh_module_rule_canonical
    SUMHModuleRuleError.DuplicatePath -> R.string.sumh_module_rule_duplicate
    SUMHModuleRuleError.InvalidMode -> R.string.sumh_module_rule_invalid_mode
    SUMHModuleRuleError.RuleLimit -> R.string.sumh_module_rule_limit
    SUMHModuleRuleError.PayloadLimit -> R.string.sumh_module_rule_budget
}
