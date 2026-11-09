package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.AutoFixHigh
import androidx.compose.material.icons.outlined.Block
import androidx.compose.material.icons.outlined.Check
import androidx.compose.material.icons.outlined.Layers
import androidx.compose.material.icons.outlined.Memory
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.sumh.util.SUMHManager
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

@Composable
internal fun SUMHMountConfigDialog(
    moduleId: String,
    moduleName: String,
    initialInfo: SUMHManager.ModuleInfo?,
    sumhAvailable: Boolean,
    globalMode: String,
    onDismiss: () -> Unit,
    onSaved: () -> Unit,
    onSave: suspend (String, String) -> Unit = { id, mode -> SUMHManager.saveModuleMode(id, mode) },
) {
    val resources = LocalResources.current
    val scope = rememberCoroutineScope()
    val initialMode = initialInfo?.mode ?: "auto"
    var selectedMode by rememberSaveable(moduleId) { mutableStateOf(initialMode) }
    var error by rememberSaveable(moduleId) { mutableStateOf<String?>(null) }
    var isSaving by remember { mutableStateOf(false) }
    val modeLabels = mapOf(
        "auto" to stringResource(R.string.sumh_mount_mode_auto),
        "sumh" to stringResource(R.string.sumh_mount_mode_sumh),
        "overlay" to stringResource(R.string.sumh_mount_mode_overlay),
        "magic" to stringResource(R.string.sumh_mount_mode_magic),
        "none" to stringResource(R.string.sumh_mount_mode_none),
    )
    val selectableModes = SUMHManager.mountModes.filter { it != "sumh" || sumhAvailable || it == initialMode }
    val canSave = !isSaving && selectedMode != initialMode && (selectedMode != "sumh" || sumhAvailable)

    YukiAlertDialog(
        onDismissRequest = { if (!isSaving) onDismiss() },
        modifier = Modifier.padding(horizontal = 24.dp).widthIn(max = 560.dp).fillMaxWidth(),
        properties = DialogProperties(usePlatformDefaultWidth = false),
        title = {
            Column {
                Text(stringResource(R.string.sumh_mount_config))
                Text(moduleName, style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        },
        text = {
            Column(Modifier.heightIn(max = 400.dp).verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp)) {
                error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                if (globalMode != "auto") Text(
                    stringResource(R.string.sumh_global_override, modeLabels[globalMode] ?: globalMode),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(stringResource(R.string.sumh_mount_mode), style = MaterialTheme.typography.titleSmall)
                FlowRow(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    selectableModes.forEach { mode ->
                        FilterChip(
                            selected = selectedMode == mode,
                            onClick = { selectedMode = mode; error = null },
                            enabled = !isSaving && (mode != "sumh" || sumhAvailable),
                            label = { Text(modeLabels[mode] ?: mode) },
                            leadingIcon = {
                                YukiIcon(
                                    if (selectedMode == mode) Icons.Outlined.Check else when (mode) {
                                        "sumh" -> Icons.Outlined.Memory
                                        "overlay" -> Icons.Outlined.Layers
                                        "magic" -> Icons.Outlined.AutoFixHigh
                                        "none" -> Icons.Outlined.Block
                                        else -> Icons.Outlined.Settings
                                    }, null, Modifier.size(FilterChipDefaults.IconSize),
                                )
                            },
                        )
                    }
                }
                Text(stringResource(R.string.sumh_module_rules_reboot),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        },
        confirmButton = {
            Button(onClick = {
                if (!canSave) return@Button
                val mode = selectedMode
                isSaving = true
                scope.launch {
                    try {
                        onSave(moduleId, mode)
                        onSaved()
                    } catch (failure: Exception) {
                        if (failure is CancellationException) throw failure
                        error = failure.message ?: resources.getString(R.string.operation_failed)
                    } finally {
                        isSaving = false
                    }
                }
            }, enabled = canSave) { Text(stringResource(android.R.string.ok)) }
        },
        dismissButton = {
            TextButton(enabled = !isSaving, onClick = onDismiss) {
                Text(stringResource(android.R.string.cancel))
            }
        },
    )
}
