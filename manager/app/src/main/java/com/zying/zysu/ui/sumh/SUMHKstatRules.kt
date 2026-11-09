package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Info
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.sumh.util.SUMHFeatureSupport
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as Controller
import com.zying.zysu.ui.sumh.util.SUMH_MAX_KSTAT_RULES
import com.zying.zysu.ui.sumh.util.SUMH_KSTAT_FIELDS
import com.zying.zysu.ui.sumh.util.validSUMHKstatPath
import com.zying.zysu.ui.sumh.util.validSUMHKstatNumber
import com.zying.zysu.ui.sumh.util.validSUMHKstatRules

@Composable
internal fun KstatRulesSettings(
    config: Controller.SUMHPConfig,
    status: Controller.SUMHStatus,
    features: Controller.FeaturesResult?,
    enabled: Boolean,
    onConfigChanged: (Controller.SUMHPConfig) -> Unit,
    headingActions: (@Composable RowScope.() -> Unit)?,
    runtimeStatus: Controller.ManagedKstatStatus? = null,
) {
    val support = when {
        status != Controller.SUMHStatus.AVAILABLE -> SUMHFeatureSupport.KernelUnavailable
        features == null -> SUMHFeatureSupport.Unknown
        "managed_kstat" in features.names -> SUMHFeatureSupport.Supported
        else -> SUMHFeatureSupport.Unsupported
    }
    val canEdit = enabled && support == SUMHFeatureSupport.Supported
    var editingTarget by rememberSaveable { mutableStateOf<String?>(null) }
    var showEditor by rememberSaveable { mutableStateOf(false) }
    var deletingTarget by rememberSaveable { mutableStateOf<String?>(null) }
    var detailTarget by rememberSaveable { mutableStateOf<String?>(null) }

    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        SUMHRuleOverview(stringResource(R.string.sumh_workspace_kstat), headingActions) {
            Text(stringResource(R.string.sumh_kstat_saved_count, config.kstatRules.size),
                style = MaterialTheme.typography.bodyLarge)
            Text(stringResource(R.string.sumh_kstat_description), style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (support == SUMHFeatureSupport.Supported && runtimeStatus != null) {
                Text(stringResource(R.string.sumh_kstat_active_count, runtimeStatus.activeCount),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                Text(stringResource(R.string.sumh_kstat_generation, runtimeStatus.generation),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                Text(stringResource(R.string.sumh_kstat_active_unknown),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            if (support != SUMHFeatureSupport.Supported) Text(stringResource(when (support) {
                SUMHFeatureSupport.KernelUnavailable -> R.string.sumh_features_kernel_unavailable
                SUMHFeatureSupport.Unknown -> R.string.sumh_features_support_unknown
                SUMHFeatureSupport.Unsupported -> R.string.sumh_kstat_unsupported
                SUMHFeatureSupport.Supported -> R.string.sumh_kstat_description
            }), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)

            SUMHRuleActions {
                FilledTonalButton(onClick = { editingTarget = null; showEditor = true },
                    enabled = canEdit && config.kstatRules.size < SUMH_MAX_KSTAT_RULES,
                    modifier = Modifier.heightIn(min = 48.dp)) {
                    YukiIcon(Icons.Filled.Add, null)
                    Spacer(Modifier.width(8.dp))
                    Text(stringResource(R.string.sumh_kstat_add))
                }
            }
        }
        if (config.kstatRules.isEmpty()) {
            SUMHRuleEmptyState(stringResource(R.string.sumh_kstat_empty))
        }
        config.kstatRules.forEach { rule ->
            key(rule.targetPath) {
                SUMHRuleCard {
                    Text(stringResource(R.string.sumh_kstat_target), style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Text(rule.targetPath, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodyMedium,
                        maxLines = 3, overflow = TextOverflow.Ellipsis)
                    if (rule.mode != "custom") {
                        Text(stringResource(R.string.sumh_kstat_source_label), style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Text(rule.sourcePath, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodyMedium,
                            maxLines = 3, overflow = TextOverflow.Ellipsis)
                    }
                    val modeLabel = when (rule.mode) {
                        "full" -> R.string.sumh_kstat_mode_full
                        "custom" -> R.string.sumh_kstat_mode_custom
                        else -> R.string.sumh_kstat_mode_identity
                    }
                    Text(stringResource(modeLabel) + if (rule.mode == "custom")
                        ": " + rule.customFields.keys.map { it.substringBefore('.') }.distinct().joinToString(", ") else "",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                        IconButton(onClick = { detailTarget = rule.targetPath }) {
                            YukiIcon(Icons.Filled.Info, stringResource(R.string.sumh_kstat_details))
                        }
                        IconButton(onClick = { editingTarget = rule.targetPath; showEditor = true }, enabled = canEdit) {
                            YukiIcon(Icons.Filled.Edit, stringResource(R.string.sumh_kstat_edit))
                        }
                        IconButton(onClick = { deletingTarget = rule.targetPath }, enabled = enabled) {
                            YukiIcon(Icons.Filled.Delete, stringResource(R.string.sumh_kstat_remove))
                        }
                    }
                }
            }
        }
    }

    if (showEditor) {
        key(editingTarget) {
            KstatRuleEditor(config.kstatRules, editingTarget, canEdit,
                onSave = { candidate -> onConfigChanged(config.copy(kstatRules = candidate)) },
                onDismiss = { showEditor = false })
        }
    }
    detailTarget?.let { target ->
        val rule = config.kstatRules.firstOrNull { it.targetPath == target }
        if (rule != null) YukiAlertDialog(
            onDismissRequest = { detailTarget = null },
            modifier = Modifier.padding(horizontal = 24.dp).widthIn(max = 560.dp).fillMaxWidth(),
            properties = DialogProperties(usePlatformDefaultWidth = false),
            title = { Text(stringResource(R.string.sumh_kstat_details)) },
            text = {
                SelectionContainer {
                    Column(Modifier.fillMaxWidth().heightIn(max = 400.dp).verticalScroll(rememberScrollState()),
                        verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(stringResource(R.string.sumh_kstat_target), style = MaterialTheme.typography.labelMedium)
                        Text(rule.targetPath, fontFamily = FontFamily.Monospace)
                        if (rule.mode != "custom") {
                            Text(stringResource(R.string.sumh_kstat_source_label), style = MaterialTheme.typography.labelMedium)
                            Text(rule.sourcePath, fontFamily = FontFamily.Monospace)
                        } else {
                            rule.customFields.forEach { (field, value) ->
                                Text("$field: $value", fontFamily = FontFamily.Monospace)
                            }
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { detailTarget = null }) { Text(stringResource(R.string.cancel)) }
            },
        )
    }
    deletingTarget?.let { target ->
        YukiAlertDialog(
            onDismissRequest = { deletingTarget = null },
            title = { Text(stringResource(R.string.sumh_kstat_remove)) },
            text = { Text(target, fontFamily = FontFamily.Monospace) },
            confirmButton = {
                TextButton(onClick = {
                    deletingTarget = null
                    onConfigChanged(config.copy(kstatRules = config.kstatRules.filterNot { it.targetPath == target }))
                }, enabled = enabled) { Text(stringResource(R.string.delete)) }
            },
            dismissButton = { TextButton(onClick = { deletingTarget = null }) { Text(stringResource(R.string.cancel)) } },
        )
    }
}

@Composable
private fun KstatRuleEditor(
    rules: List<Controller.KstatPathRule>,
    originalTarget: String?,
    enabled: Boolean,
    onSave: (List<Controller.KstatPathRule>) -> Unit,
    onDismiss: () -> Unit,
) {
    val original = rules.firstOrNull { it.targetPath == originalTarget }
    var target by rememberSaveable { mutableStateOf(original?.targetPath.orEmpty()) }
    var source by rememberSaveable { mutableStateOf(original?.sourcePath.orEmpty()) }
    var mode by rememberSaveable { mutableStateOf(original?.mode ?: "identity") }
    var customFields by rememberSaveable { mutableStateOf(original?.customFields.orEmpty()) }
    var submitted by rememberSaveable { mutableStateOf(false) }
    var pendingCandidate by remember { mutableStateOf<List<Controller.KstatPathRule>?>(null) }
    val candidate = Controller.KstatPathRule(target, if (mode == "custom") "" else source, mode,
        if (mode == "custom") customFields else emptyMap())
    val replacement = if (originalTarget == null) rules + candidate
        else rules.map { if (it.targetPath == originalTarget) candidate else it }
    val valid = validSUMHKstatRules(replacement) &&
        (originalTarget == null || original != null) && replacement != rules

    LaunchedEffect(rules, pendingCandidate) {
        if (pendingCandidate != null && rules == pendingCandidate) onDismiss()
    }

    YukiAlertDialog(
        onDismissRequest = onDismiss,
        modifier = Modifier.padding(horizontal = 24.dp).widthIn(max = 560.dp).fillMaxWidth(),
        properties = DialogProperties(usePlatformDefaultWidth = false),
        title = { Text(stringResource(if (originalTarget == null) R.string.sumh_kstat_add else R.string.sumh_kstat_edit)) },
        text = {
            Column(Modifier.fillMaxWidth().heightIn(max = 480.dp).verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(10.dp)) {
                OutlinedTextField(target, { target = it; pendingCandidate = null }, label = { Text(stringResource(R.string.sumh_kstat_target)) },
                    modifier = Modifier.fillMaxWidth(), enabled = enabled, singleLine = true,
                    isError = submitted && !validSUMHKstatPath(target, 256))
                if (mode != "custom") OutlinedTextField(source, { source = it; pendingCandidate = null },
                    label = { Text(stringResource(R.string.sumh_kstat_source_label)) },
                    modifier = Modifier.fillMaxWidth(), enabled = enabled, singleLine = true,
                    isError = submitted && !validSUMHKstatPath(source, 4096))
                Text(stringResource(R.string.sumh_kstat_mode), style = MaterialTheme.typography.titleSmall)
                listOf("identity" to R.string.sumh_kstat_mode_identity,
                    "full" to R.string.sumh_kstat_mode_full,
                    "custom" to R.string.sumh_kstat_mode_custom).forEach { (value, label) ->
                    FilterChip(selected = mode == value, onClick = { mode = value; pendingCandidate = null }, enabled = enabled,
                        label = { Text(stringResource(label)) })
                }
                if (mode == "custom") KstatCustomFieldEditor(customFields, submitted, enabled) {
                    customFields = it
                    pendingCandidate = null
                }
                if (submitted && !valid) Text(stringResource(R.string.sumh_kstat_invalid),
                    color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
            }
        },
        confirmButton = {
            TextButton(onClick = {
                submitted = true
                if (enabled && valid) { pendingCandidate = replacement; onSave(replacement) }
            }, enabled = enabled) {
                Text(stringResource(R.string.sumh_rules_save))
            }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.sumh_rules_cancel)) } },
    )
}

@Composable
private fun KstatCustomFieldEditor(
    values: Map<String, String>, submitted: Boolean, enabled: Boolean,
    onChange: (Map<String, String>) -> Unit,
) {
    val labels = listOf(
        R.string.sumh_kstat_field_dev, R.string.sumh_kstat_field_ino,
        R.string.sumh_kstat_field_nlink, R.string.sumh_kstat_field_size,
        R.string.sumh_kstat_field_atime, R.string.sumh_kstat_field_mtime,
        R.string.sumh_kstat_field_ctime, R.string.sumh_kstat_field_blksize,
        R.string.sumh_kstat_field_blocks,
    )
    SUMH_KSTAT_FIELDS.forEachIndexed { index, name ->
        val selected = if (name in setOf("atime", "mtime", "ctime")) "$name.sec" in values else name in values
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).toggleable(value = selected,
                enabled = enabled, onValueChange = { checked ->
                    onChange(values.toMutableMap().apply {
                        if (name in setOf("atime", "mtime", "ctime")) {
                            if (checked) { put("$name.sec", "0"); put("$name.nsec", "0") }
                            else { remove("$name.sec"); remove("$name.nsec") }
                        } else if (checked) put(name, "0") else remove(name)
                    })
                }), verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                Checkbox(checked = selected, enabled = enabled, onCheckedChange = null)
                Text(stringResource(labels[index]), style = MaterialTheme.typography.bodyMedium)
            }
            if (selected && name in setOf("atime", "mtime", "ctime")) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    listOf("sec" to R.string.sumh_kstat_seconds, "nsec" to R.string.sumh_kstat_nanoseconds)
                        .forEach { (unit, label) ->
                            val field = "$name.$unit"
                            OutlinedTextField(values[field].orEmpty(), { value ->
                                onChange(values + (field to value))
                            }, label = { Text(stringResource(label)) }, modifier = Modifier.weight(1f),
                                enabled = enabled, singleLine = true,
                                keyboardOptions = KeyboardOptions(keyboardType = if (unit == "sec") KeyboardType.Text else KeyboardType.Number),
                                isError = submitted && !validSUMHKstatNumber(field, values[field].orEmpty()))
                        }
                }
            } else if (selected) {
                OutlinedTextField(values[name].orEmpty(), { value -> onChange(values + (name to value)) },
                    label = { Text(stringResource(labels[index])) }, modifier = Modifier.fillMaxWidth(),
                    enabled = enabled, singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    isError = submitted && !validSUMHKstatNumber(name, values[name].orEmpty()))
            }
        }
    }
}
