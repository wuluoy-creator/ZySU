package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Subject
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.component.YukiPanel
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as Controller
import com.zying.zysu.ui.sumh.util.SUMHControlState
import com.zying.zysu.ui.theme.getCardColors
import com.zying.zysu.ui.theme.getCardBorder
import com.zying.zysu.ui.theme.getCardElevation
import ui.screen.moreSettings.component.MoreSettingsItemPosition

@Composable
internal fun SettingsTab(
    config: Controller.SUMHPConfig,
    sumhStatus: Controller.SUMHStatus,
    features: Controller.FeaturesResult?,
    controlsEnabled: Boolean,
    runtimeApplyPending: Boolean,
    onRetryApply: () -> Unit,
    onConfigChanged: (Controller.SUMHPConfig) -> Unit,
    onClearMapRules: () -> Unit,
    onAddMapRule: (Long, Long, Long, Long, String) -> Unit,
    section: SUMHSection,
    scrollable: Boolean = true,
    header: @Composable () -> Unit = {},
    contentPadding: PaddingValues = PaddingValues(horizontal = 18.dp, vertical = 16.dp),
    headingActions: (@Composable RowScope.() -> Unit)? = null,
    controlState: SUMHControlState = SUMHControlState(),
    legacyApplyPending: Boolean = runtimeApplyPending,
    onRefreshState: () -> Unit = {},
    onPartitionsSaved: (List<String>) -> Unit = {},
    onPartitionsSaving: (Boolean) -> Unit = {},
) {
    val enabled = controlsEnabled
    val mountEnabled = enabled && config.externalOwner.isEmpty()
    val kernelEnabled = enabled && sumhStatus == Controller.SUMHStatus.AVAILABLE
    Column(
        modifier = (if (scrollable) Modifier.fillMaxSize().verticalScroll(rememberScrollState()) else Modifier.fillMaxWidth())
            .padding(contentPadding),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        header()
        SUMHControlStatePanel(controlState, config, section, onRefreshState,
            onApply = if (kernelEnabled) onRetryApply else null,
            enabled = controlsEnabled, legacyPending = legacyApplyPending)
        when (section) {
            SUMHSection.Mount -> {
                ConfigSection(stringResource(R.string.sumh_group_mount_backends), segmented = true, actions = headingActions) {
                    if (config.externalOwner.isNotEmpty()) {
                        SUMHControlGroup {
                            Text(stringResource(R.string.sumh_external_mount, config.externalOwner),
                                color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                    SettingSwitch("OverlayFS", stringResource(R.string.sumh_backend_enable_desc), config.overlayfsEnabled,
                        { onConfigChanged(config.copy(overlayfsEnabled = it)) }, mountEnabled, Icons.Filled.Layers,
                        MoreSettingsItemPosition.First)
                    SettingSwitch("Magic Mount", stringResource(R.string.sumh_backend_enable_desc), config.magicMountEnabled,
                        { onConfigChanged(config.copy(magicMountEnabled = it)) }, mountEnabled, Icons.Filled.AutoFixHigh,
                        MoreSettingsItemPosition.Last)
                }
                // These values are shared by the mount backends, not owned by either switch.
                ConfigSection(stringResource(R.string.sumh_group_mount_storage), segmented = true) {
                    FileSystemChoice(config.fsType, mountEnabled) {
                        onConfigChanged(config.copy(fsType = it))
                    }
                    MountSourceInput(config.mountsource, mountEnabled) { onConfigChanged(config.copy(mountsource = it)) }
                    EditableConfigPath(R.string.sumh_tempdir, R.string.sumh_tempdir_desc, config.tempdir, mountEnabled, true) {
                        onConfigChanged(config.copy(tempdir = it))
                    }
                    EditableConfigPath(R.string.sumh_mirror_path, R.string.sumh_mirror_path_desc, config.mirrorPath, mountEnabled, true,
                        MoreSettingsItemPosition.Last) {
                        onConfigChanged(config.copy(mirrorPath = it))
                    }
                }
                SUMHPartitionsControls(mountEnabled, config.original.toString(), onPartitionsSaved, onPartitionsSaving)
            }
            SUMHSection.Isolation -> IsolationFeatureSettings(
                config = config,
                kernelAvailable = sumhStatus == Controller.SUMHStatus.AVAILABLE,
                features = features,
                controlsEnabled = enabled,
                onConfigChanged = onConfigChanged,
                onClearMapRules = onClearMapRules,
                onAddMapRule = onAddMapRule,
                headingActions = headingActions,
                onRetryApply = onRetryApply,
                runtimeApplyPending = runtimeApplyPending,
            )
            SUMHSection.Debug -> ConfigSection(stringResource(R.string.sumh_workspace_debug), segmented = true, actions = headingActions) {
                SettingSwitch(stringResource(R.string.sumh_debug), stringResource(R.string.sumh_debug_desc),
                    config.debug, { onConfigChanged(config.copy(debug = it)) }, enabled,
                    Icons.Filled.BugReport, MoreSettingsItemPosition.First)
                SettingSwitch(stringResource(R.string.sumh_verbose), stringResource(R.string.sumh_verbose_desc),
                    config.verbose, { onConfigChanged(config.copy(verbose = it)) }, enabled, Icons.AutoMirrored.Filled.Subject)
                SettingSwitch(stringResource(R.string.sumh_kernel_debug), stringResource(R.string.sumh_kernel_debug_desc), config.enableKernelDebug,
                    { onConfigChanged(config.copy(enableKernelDebug = it)) }, kernelEnabled,
                    Icons.Filled.BugReport, MoreSettingsItemPosition.Last)
            }
            else -> Unit
        }
    }
}
@Composable
private fun FileSystemChoice(value: String, enabled: Boolean, onSelect: (String) -> Unit) {
    val options = listOf(
        "auto" to R.string.sumh_fs_type_auto,
        "ext4" to R.string.sumh_fs_type_ext4,
        "erofs" to R.string.sumh_fs_type_erofs,
        "tmpfs" to R.string.sumh_fs_type_tmpfs,
    )
    SUMHControlGroup(MoreSettingsItemPosition.First) {
        Text(
            stringResource(R.string.sumh_fs_type_title),
            style = MaterialTheme.typography.bodyLarge,
        )
        Spacer(Modifier.height(4.dp))
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp)
        ) {
            options.forEach { (option, title) ->
                FilterChip(
                    selected = value == option,
                    onClick = { onSelect(option) },
                    enabled = enabled,
                    label = { Text(stringResource(title)) },
                    leadingIcon = if (value == option) {
                        { YukiIcon(Icons.Filled.Check, null, Modifier.size(FilterChipDefaults.IconSize)) }
                    } else null,
                )
            }
        }
    }
}

@Composable
internal fun ConfigSection(
    title: String = "",
    segmented: Boolean = false,
    actions: (@Composable RowScope.() -> Unit)? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        if (title.isNotEmpty()) {
            Row(
                modifier = Modifier.fillMaxWidth().padding(start = 12.dp, end = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Text(
                    title,
                    style = MaterialTheme.typography.titleSmall,
                    color = MaterialTheme.colorScheme.primary,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.weight(1f).padding(vertical = 8.dp).semantics { heading() }
                )
                actions?.invoke(this)
            }
        }
        YukiPanel {
            Column(
                modifier = if (segmented) Modifier.padding(vertical = 4.dp) else Modifier.padding(20.dp),
                verticalArrangement = Arrangement.spacedBy(if (segmented) 0.dp else 8.dp),
                content = content
            )
        }
    }
}

@Composable
internal fun ConfigChoice(title: String, value: String, values: List<String>, enabled: Boolean,
    position: MoreSettingsItemPosition = MoreSettingsItemPosition.Only, selected: (String) -> Unit) {
    val labels = mapOf("auto" to stringResource(R.string.sumh_mount_mode_auto), "sumh" to "SUMH",
        "overlay" to "OverlayFS", "magic" to "Magic Mount", "none" to stringResource(R.string.sumh_mount_mode_none),
        "normal" to stringResource(R.string.sumh_hide_normal), "aggressive" to stringResource(R.string.sumh_hide_aggressive), "magisk" to "Magisk",
        "all" to stringResource(R.string.sumh_filter_all), "sumh-active" to stringResource(R.string.sumh_filter_active))
    SUMHControlGroup(position) {
        ConfigChoiceField(title, labels[value] ?: value, values.map { it to (labels[it] ?: it) }, enabled, selected)
    }
}

/** Shared field for both standalone choices and choices inside an expanded settings card. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun ConfigChoiceField(
    title: String,
    valueLabel: String,
    choices: List<Pair<String, String>>,
    enabled: Boolean,
    selected: (String) -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    val menuExpanded = expanded && enabled
    ExposedDropdownMenuBox(
        expanded = menuExpanded,
        onExpandedChange = { if (enabled) expanded = it },
    ) {
        OutlinedTextField(
            value = valueLabel,
            onValueChange = {},
            readOnly = true,
            enabled = enabled,
            modifier = Modifier.fillMaxWidth()
                .menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable, enabled = enabled),
            shape = MaterialTheme.shapes.small,
            label = { Text(title) },
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(menuExpanded) },
        )
        ExposedDropdownMenu(expanded = menuExpanded, onDismissRequest = { expanded = false }) {
            choices.forEach { (choice, label) ->
                DropdownMenuItem(
                    text = { Text(label) },
                    onClick = { expanded = false; selected(choice) },
                )
            }
        }
    }
}

@Composable
internal fun MountSourceInput(value: String, enabled: Boolean, onSave: (String) -> Unit) {
    val draftState = rememberSUMHRefreshAwareDraft(value)
    val draft = draftState.text
    val conflict = draftState.conflict(value)
    var draftLimit by remember { mutableStateOf(false) }
    val changed = draft != value
    val valid = draft.isNotEmpty() && draft.none { it.isWhitespace() || it.isISOControl() || it == '\\' }

    fun save() {
        if (enabled && !conflict && changed && valid) { draftState.submitted(); onSave(draft) }
    }

    SUMHControlGroup(MoreSettingsItemPosition.Middle) {
        if (draftLimit) SUMHSmallDraftLimit()
        SUMHDraftConflict(draftState, value, stringResource(R.string.sumh_mountsource))
        OutlinedTextField(
            value = draft,
            onValueChange = { draftLimit = !sumhSmallDraftFits(it); if (!draftLimit) draftState.edit(it) },
            modifier = Modifier.fillMaxWidth(),
            enabled = enabled,
            singleLine = true,
            shape = MaterialTheme.shapes.small,
            isError = changed && !valid,
            label = { Text(stringResource(R.string.sumh_mountsource)) },
            placeholder = { Text("KSU") },
            trailingIcon = if (changed) {
                {
                    IconButton(onClick = { save() }, enabled = enabled && !conflict && valid) {
                        YukiIcon(Icons.Filled.Check, stringResource(R.string.sumh_rule_save))
                    }
                }
            } else null,
            supportingText = if (changed && !valid) {
                { Text(stringResource(R.string.sumh_mountsource_invalid)) }
            } else null,
            keyboardOptions = KeyboardOptions(autoCorrectEnabled = false, imeAction = ImeAction.Done),
            keyboardActions = KeyboardActions(onDone = { save() }),
        )
    }
}

@Composable
private fun EditableConfigPath(title: Int, subtitle: Int, value: String, enabled: Boolean, allowEmpty: Boolean,
    position: MoreSettingsItemPosition = MoreSettingsItemPosition.Middle, save: (String) -> Unit) {
    val draftState = rememberSUMHRefreshAwareDraft(value)
    val draft = draftState.text
    val conflict = draftState.conflict(value)
    var invalid by remember(value) { mutableStateOf(false) }
    var draftLimit by remember { mutableStateOf(false) }
    SettingTextField(stringResource(title), stringResource(subtitle), draft, { draftLimit = !sumhSmallDraftFits(it); if (!draftLimit) { draftState.edit(it); invalid = false } }, {
        if (!conflict && (draft.startsWith('/') || (allowEmpty && draft.isEmpty()))) {
            draftState.submitted(); save(draft)
        } else invalid = !conflict
    }, enabled, position = position, confirmEnabled = enabled && !conflict)
    SUMHDraftConflict(draftState, value, stringResource(title), Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
    if (draftLimit) SUMHSmallDraftLimit()
    if (invalid) Text(
        stringResource(R.string.sumh_absolute_path),
        color = MaterialTheme.colorScheme.error,
        style = MaterialTheme.typography.bodySmall,
        modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp)
    )
}

@Composable
internal fun userHideStatus(rule: Controller.ActiveRule): String = when (rule.hideState) {
    -1 -> stringResource(R.string.sumh_hide_saved)
    0 -> stringResource(R.string.sumh_hide_pending)
    1 -> stringResource(R.string.sumh_hide_binding)
    2 -> stringResource(R.string.sumh_hide_bound)
    4 -> stringResource(R.string.sumh_hide_suspended)
    else -> when (rule.hideError) {
        -13, -1 -> stringResource(R.string.sumh_hide_permission)
        -95 -> stringResource(R.string.sumh_hide_unsupported)
        else -> stringResource(R.string.sumh_hide_error, -rule.hideError)
    }
}

@Composable
internal fun KernelHooksCard(hooks: String) {
    if (hooks.isEmpty()) return
    var expanded by rememberSaveable { mutableStateOf(false) }
    val expandLabel = stringResource(if (expanded) R.string.collapse_menu else R.string.expand_menu)
    Card(
        onClick = { expanded = !expanded },
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.extraLarge,
        border = getCardBorder(),
        colors = getCardColors(MaterialTheme.colorScheme.surfaceContainerLow),
        elevation = getCardElevation()
    ) {
        Column(Modifier.padding(horizontal = 16.dp, vertical = 8.dp)) {
            Row(Modifier.fillMaxWidth().heightIn(min = 48.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(R.string.sumh_kernel_hooks), Modifier.weight(1f), style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold)
                YukiIcon(if (expanded) Icons.Filled.ExpandLess else Icons.Filled.ExpandMore, expandLabel)
            }
            if (expanded) Text(hooks, Modifier.padding(top = 8.dp), fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
        }
    }
}
