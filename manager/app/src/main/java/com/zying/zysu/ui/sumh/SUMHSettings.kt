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
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.component.YukiPanel
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as Controller
import com.zying.zysu.ui.sumh.util.SUMHManager
import com.zying.zysu.ui.sumh.util.SUMHManagedFeature
import com.zying.zysu.ui.sumh.util.sumhFeatureStates
import com.zying.zysu.ui.sumh.util.sumhManagedMapsSupport
import com.zying.zysu.ui.sumh.util.validSUMHPartitionName
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
    section: SUMHSection,
    scrollable: Boolean = true,
    header: @Composable () -> Unit = {},
    contentPadding: PaddingValues = PaddingValues(horizontal = 18.dp, vertical = 16.dp),
    headingActions: (@Composable RowScope.() -> Unit)? = null,
    runtimeRules: List<Controller.ActiveRule>? = null,
    managedKstatStatus: Controller.ManagedKstatStatus? = null,
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
        if (runtimeApplyPending) {
            ConfigSection {
                Text(stringResource(R.string.sumh_runtime_apply_pending), color = MaterialTheme.colorScheme.onSurfaceVariant)
                OutlinedButton(onClick = onRetryApply, enabled = kernelEnabled, modifier = Modifier.fillMaxWidth()) {
                    YukiIcon(Icons.Filled.Refresh, null)
                    Spacer(Modifier.width(8.dp))
                    Text(stringResource(R.string.sumh_retry_apply))
                }
            }
        }
        when (section) {
            SUMHSection.Mount -> {
                ConfigSection(stringResource(R.string.sumh_group_mount_backends), segmented = true, actions = headingActions) {
                    if (config.externalOwner.isNotEmpty()) {
                        SUMHControlGroup {
                            Text(stringResource(R.string.sumh_external_mount, config.externalOwner),
                                color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                    GlobalMountBackendChoice(config.mountBackend, mountEnabled,
                        sumhStatus == Controller.SUMHStatus.AVAILABLE) {
                        onConfigChanged(config.copy(mountBackend = it))
                    }
                    SettingSwitch("OverlayFS", stringResource(R.string.sumh_backend_enable_desc), config.overlayfsEnabled,
                        { onConfigChanged(config.copy(overlayfsEnabled = it)) }, mountEnabled, Icons.Filled.Layers,
                        MoreSettingsItemPosition.Middle)
                    SettingSwitch("Magic Mount", stringResource(R.string.sumh_backend_enable_desc), config.magicMountEnabled,
                        { onConfigChanged(config.copy(magicMountEnabled = it)) }, mountEnabled, Icons.Filled.AutoFixHigh,
                        MoreSettingsItemPosition.Last)
                }
                // These values are shared by the mount backends, not owned by either switch.
                ConfigSection(stringResource(R.string.sumh_group_mount_storage), segmented = true) {
                    FileSystemChoice(config.fsType, mountEnabled) {
                        onConfigChanged(config.copy(fsType = it))
                    }
                    SettingSwitch(stringResource(R.string.sumh_overlay_writable),
                        stringResource(R.string.sumh_overlay_writable_desc), config.overlayWritable,
                        { onConfigChanged(config.copy(overlayWritable = it)) }, mountEnabled, Icons.Filled.Edit)
                    MirrorImageSizeInput(config.mirrorImageSizeMb, mountEnabled) {
                        onConfigChanged(config.copy(mirrorImageSizeMb = it))
                    }
                    MountSourceInput(config.mountsource, mountEnabled) { onConfigChanged(config.copy(mountsource = it)) }
                    EditableConfigPath(R.string.sumh_tempdir, R.string.sumh_tempdir_desc, config.tempdir, mountEnabled, true) {
                        onConfigChanged(config.copy(tempdir = it))
                    }
                    EditableConfigPath(R.string.sumh_mirror_path, R.string.sumh_mirror_path_desc, config.mirrorPath, mountEnabled, true,
                        MoreSettingsItemPosition.Middle) {
                        onConfigChanged(config.copy(mirrorPath = it))
                    }
                    PartitionEditor(config.partitions, mountEnabled) {
                        onConfigChanged(config.copy(partitions = it))
                    }
                }
            }
            SUMHSection.Isolation -> IsolationFeatureSettings(
                config = config,
                runtimeRules = runtimeRules,
                kernelAvailable = sumhStatus == Controller.SUMHStatus.AVAILABLE,
                features = features,
                controlsEnabled = enabled,
                onConfigChanged = onConfigChanged,
                headingActions = headingActions,
            )
            SUMHSection.Maps -> MapsRulesSettings(config, sumhStatus, features, enabled,
                runtimeApplyPending, onRetryApply, onConfigChanged, headingActions)
            SUMHSection.Kstat -> KstatRulesSettings(config, sumhStatus, features, enabled,
                onConfigChanged, headingActions, managedKstatStatus)
            SUMHSection.Kernel -> {
                val support = sumhFeatureStates(config.configuredSUMHFeatures(),
                    sumhStatus == Controller.SUMHStatus.AVAILABLE, features?.names?.toSet())
                    .first { it.feature == SUMHManagedFeature.KernelBuild }.support
                KernelBuildSpoofCard(config, enabled, kernelEnabled, support, onConfigChanged, headingActions)
            }
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
private fun MapsRulesSettings(
    config: Controller.SUMHPConfig,
    status: Controller.SUMHStatus,
    features: Controller.FeaturesResult?,
    enabled: Boolean,
    runtimeApplyPending: Boolean,
    onRetryApply: () -> Unit,
    onConfigChanged: (Controller.SUMHPConfig) -> Unit,
    headingActions: (@Composable RowScope.() -> Unit)?,
) {
    val support = sumhManagedMapsSupport(status == Controller.SUMHStatus.AVAILABLE, features?.names?.toSet())
    var confirmStop by rememberSaveable { mutableStateOf(false) }
    ConfigSection(stringResource(R.string.sumh_maps_rules_page), segmented = true, actions = headingActions) {
        ManagedMapsControls(
            rules = config.mapsRules,
            enabled = enabled,
            support = support,
            mapsEnabled = config.enableMapsSpoof,
            onRulesChanged = { rules ->
                onConfigChanged(config.copy(mapsRules = rules,
                    enableMapsSpoof = config.enableMapsSpoof || rules.isNotEmpty()))
            },
            onRetryApply = onRetryApply,
            runtimeApplyPending = runtimeApplyPending,
        )
        if (config.mapsRules.isNotEmpty() && !config.enableMapsSpoof) {
            SUMHControlGroup {
                FilledTonalButton(onClick = { onConfigChanged(config.copy(enableMapsSpoof = true)) },
                    enabled = enabled && support == com.zying.zysu.ui.sumh.util.SUMHFeatureSupport.Supported,
                    modifier = Modifier.fillMaxWidth()) {
                    Text(stringResource(R.string.sumh_maps_activate_saved))
                }
            }
        }
        if (config.mapsRules.isEmpty() && config.enableMapsSpoof) {
            SUMHControlGroup {
                Text(stringResource(R.string.sumh_maps_runtime_only),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                TextButton(onClick = { confirmStop = true }, enabled = enabled) {
                    Text(stringResource(R.string.sumh_maps_stop_filtering))
                }
            }
        }
    }
    if (confirmStop) com.zying.zysu.ui.component.YukiAlertDialog(
        onDismissRequest = { confirmStop = false },
        title = { Text(stringResource(R.string.sumh_maps_stop_filtering)) },
        text = { Text(stringResource(R.string.sumh_maps_stop_confirm)) },
        confirmButton = {
            TextButton(onClick = {
                confirmStop = false
                onConfigChanged(config.copy(enableMapsSpoof = false))
            }, enabled = enabled) { Text(stringResource(R.string.sumh_maps_stop_filtering)) }
        },
        dismissButton = {
            TextButton(onClick = { confirmStop = false }) { Text(stringResource(R.string.sumh_rules_cancel)) }
        },
    )
}

@Composable
private fun GlobalMountBackendChoice(value: String, enabled: Boolean, sumhAvailable: Boolean,
    onSelect: (String) -> Unit) {
    val labels = mapOf(
        "auto" to stringResource(R.string.sumh_mount_mode_auto),
        "sumh" to stringResource(R.string.sumh_mount_mode_sumh),
        "overlay" to stringResource(R.string.sumh_mount_mode_overlay),
        "magic" to stringResource(R.string.sumh_mount_mode_magic),
        "none" to stringResource(R.string.sumh_mount_mode_none),
    )
    SUMHControlGroup(MoreSettingsItemPosition.First) {
        ConfigChoiceField(
            stringResource(R.string.sumh_global_backend), labels[value] ?: value,
            SUMHManager.mountModes.filter { it != "sumh" || sumhAvailable }
                .map { it to (labels[it] ?: it) }, enabled, onSelect,
        )
        Text(stringResource(R.string.sumh_global_backend_desc),
            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        if (!sumhAvailable) Text(stringResource(R.string.sumh_global_sumh_unavailable),
            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun MirrorImageSizeInput(value: Int, enabled: Boolean, onSave: (Int) -> Unit) {
    var draft by rememberSaveable(value) { mutableStateOf(value.toString()) }
    val parsed = draft.toIntOrNull()?.takeIf { it > 0 }
    val changed = draft != value.toString()
    SUMHControlGroup {
        OutlinedTextField(
            value = draft, onValueChange = { draft = it }, enabled = enabled,
            modifier = Modifier.fillMaxWidth(), singleLine = true,
            label = { Text(stringResource(R.string.sumh_mirror_image_size)) },
            isError = changed && parsed == null,
            trailingIcon = if (changed) { {
                IconButton(onClick = { parsed?.let(onSave) }, enabled = enabled && parsed != null) {
                    YukiIcon(Icons.Filled.Check, stringResource(R.string.sumh_rule_save))
                }
            } } else null,
            supportingText = {
                Text(stringResource(if (changed && parsed == null) R.string.sumh_mirror_image_size_invalid
                    else R.string.sumh_mirror_image_size_desc))
            },
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number, imeAction = ImeAction.Done),
            keyboardActions = KeyboardActions(onDone = { if (enabled) parsed?.let(onSave) }),
        )
    }
}

@Composable
private fun PartitionEditor(value: List<String>, enabled: Boolean, onSave: (List<String>) -> Unit) {
    var draft by rememberSaveable { mutableStateOf("") }
    val valid = validSUMHPartitionName(draft) && draft !in value
    LaunchedEffect(value) { if (draft in value) draft = "" }
    SUMHControlGroup(MoreSettingsItemPosition.Last) {
        Text(stringResource(R.string.sumh_extra_partitions), style = MaterialTheme.typography.bodyLarge)
        Text(stringResource(R.string.sumh_extra_partitions_desc),
            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        value.forEachIndexed { index, name ->
            Row(Modifier.fillMaxWidth().heightIn(min = 48.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(name, Modifier.weight(1f), fontFamily = FontFamily.Monospace,
                    style = MaterialTheme.typography.bodyMedium)
                IconButton(onClick = { onSave(value.filterIndexed { i, _ -> i != index }.distinct()) }, enabled = enabled) {
                    YukiIcon(Icons.Filled.Delete, stringResource(R.string.sumh_partition_remove, name))
                }
            }
        }
        OutlinedTextField(
            value = draft, onValueChange = { draft = it }, enabled = enabled,
            modifier = Modifier.fillMaxWidth(), singleLine = true,
            label = { Text(stringResource(R.string.sumh_partition_add)) },
            isError = draft.isNotEmpty() && !valid,
            supportingText = if (draft.isNotEmpty() && !valid) { {
                Text(stringResource(R.string.sumh_partition_invalid))
            } } else null,
            trailingIcon = {
                IconButton(onClick = { onSave((value + draft).distinct()) }, enabled = enabled && valid) {
                    YukiIcon(Icons.Filled.Add, stringResource(R.string.sumh_partition_add))
                }
            },
            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
            keyboardActions = KeyboardActions(onDone = {
                if (enabled && valid) onSave((value + draft).distinct())
            }),
        )
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
    var draft by rememberSaveable(value) { mutableStateOf(value) }
    val changed = draft != value
    val valid = draft.isNotEmpty() && draft.none { it.isWhitespace() || it.isISOControl() || it == '\\' }

    fun save() {
        if (enabled && changed && valid) onSave(draft)
    }

    SUMHControlGroup(MoreSettingsItemPosition.Middle) {
        OutlinedTextField(
            value = draft,
            onValueChange = { draft = it },
            modifier = Modifier.fillMaxWidth(),
            enabled = enabled,
            singleLine = true,
            shape = MaterialTheme.shapes.small,
            isError = changed && !valid,
            label = { Text(stringResource(R.string.sumh_mountsource)) },
            placeholder = { Text("KSU") },
            trailingIcon = if (changed) {
                {
                    IconButton(onClick = { save() }, enabled = enabled && valid) {
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
    var draft by rememberSaveable(value) { mutableStateOf(value) }
    var invalid by remember(value) { mutableStateOf(false) }
    SettingTextField(stringResource(title), stringResource(subtitle), draft, { draft = it; invalid = false }, {
        if (draft.startsWith('/') || (allowEmpty && draft.isEmpty())) save(draft)
        else invalid = true
    }, enabled, position = position)
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
