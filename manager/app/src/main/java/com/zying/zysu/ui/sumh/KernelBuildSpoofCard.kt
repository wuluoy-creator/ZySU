package com.zying.zysu.ui.sumh

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.NavigateNext
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.sumh.util.KERNEL_BUILD_EARLY_STAGE
import com.zying.zysu.ui.sumh.util.KernelBuildState
import com.zying.zysu.ui.sumh.util.SUMHFeatureSupport
import com.zying.zysu.ui.sumh.util.SUMHManager
import com.zying.zysu.ui.sumh.util.kernelBuildOverride
import com.zying.zysu.ui.sumh.util.validKernelBuildField
import kotlinx.coroutines.CancellationException
import ui.screen.moreSettings.component.SettingsIconBadge
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as Controller

private sealed interface KernelBuildRead {
    data object Unavailable : KernelBuildRead
    data object Loading : KernelBuildRead
    data object Failed : KernelBuildRead
    data class Ready(val value: KernelBuildState) : KernelBuildRead
}

@Composable
internal fun KernelBuildSpoofCard(
    config: Controller.SUMHPConfig,
    controlsEnabled: Boolean,
    kernelEnabled: Boolean,
    support: SUMHFeatureSupport,
    onConfigChanged: (Controller.SUMHPConfig) -> Unit,
    headingActions: (@Composable RowScope.() -> Unit)? = null,
) {
    val supported = support == SUMHFeatureSupport.Supported
    val editable = controlsEnabled && kernelEnabled && supported
    var showEditor by rememberSaveable { mutableStateOf(false) }
    var refresh by remember { mutableIntStateOf(0) }
    var read by remember { mutableStateOf<KernelBuildRead>(KernelBuildRead.Unavailable) }
    val revision by SUMHManager.revision.collectAsState()

    LaunchedEffect(supported, config.kernelAvailable, revision, refresh) {
        if (!supported || !config.kernelAvailable) {
            read = KernelBuildRead.Unavailable
            return@LaunchedEffect
        }
        read = KernelBuildRead.Loading
        read = try {
            KernelBuildRead.Ready(SUMHManager.kernelBuild())
        } catch (error: Exception) {
            if (error is CancellationException) throw error
            KernelBuildRead.Failed
        }
    }

    val summary = when (support) {
        SUMHFeatureSupport.Unsupported -> stringResource(R.string.sumh_features_support_no)
        SUMHFeatureSupport.Unknown -> stringResource(R.string.sumh_features_support_unknown)
        SUMHFeatureSupport.KernelUnavailable -> stringResource(R.string.sumh_features_kernel_unavailable)
        SUMHFeatureSupport.Supported -> when (val current = read) {
            is KernelBuildRead.Ready -> "${current.value.release}\n${current.value.version}"
            KernelBuildRead.Failed -> stringResource(R.string.sumh_kernel_build_read_failed)
            else -> stringResource(R.string.sumh_kernel_build_loading)
        }
    }
    ConfigSection(stringResource(SUMHSection.Isolation.displayNameRes), segmented = true, actions = headingActions) {
        Row(
            modifier = Modifier.fillMaxWidth().heightIn(min = 76.dp)
                .clickable(enabled = editable, role = Role.Button) { showEditor = true }
                .alpha(if (editable) 1f else 0.5f)
                .padding(horizontal = 16.dp, vertical = 12.dp),
            horizontalArrangement = Arrangement.spacedBy(14.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            SettingsIconBadge(Icons.Filled.Memory)
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(stringResource(R.string.sumh_kernel_build_spoof),
                    style = MaterialTheme.typography.bodyLarge, fontWeight = FontWeight.Medium)
                Text(summary, style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            YukiIcon(Icons.AutoMirrored.Filled.NavigateNext, null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }

    if (showEditor) {
        KernelBuildSpoofDialog(
            config = config,
            editable = editable,
            read = read,
            onRetry = { refresh++ },
            onDismiss = { showEditor = false },
            onConfigChanged = onConfigChanged,
        )
    }
}

@Composable
private fun KernelBuildSpoofDialog(
    config: Controller.SUMHPConfig,
    editable: Boolean,
    read: KernelBuildRead,
    onRetry: () -> Unit,
    onDismiss: () -> Unit,
    onConfigChanged: (Controller.SUMHPConfig) -> Unit,
) {
    // Refreshes must not overwrite a draft, including when saving fails.
    var release by rememberSaveable { mutableStateOf(config.kernelBuildRelease) }
    var version by rememberSaveable { mutableStateOf(config.kernelBuildVersion) }
    var saveRequested by rememberSaveable { mutableStateOf(false) }
    val draft = kernelBuildOverride(release, version)
    val releaseValid = validKernelBuildField(release.trim(), release = true)
    val versionValid = validKernelBuildField(version.trim())
    val runtime = (read as? KernelBuildRead.Ready)?.value
    val matchesSaved = draft != null && draft.enabled == config.enableKernelBuildSpoof &&
        draft.release == config.kernelBuildRelease && draft.version == config.kernelBuildVersion &&
        config.kernelBuildApplyStage == KERNEL_BUILD_EARLY_STAGE
    val canSave = editable && draft != null && !matchesSaved

    LaunchedEffect(saveRequested, matchesSaved) {
        if (saveRequested && matchesSaved) onDismiss()
    }

    fun save() {
        val value = draft ?: return
        if (!canSave) return
        saveRequested = true
        onConfigChanged(config.copy(
            enableKernelBuildSpoof = value.enabled,
            kernelBuildRelease = value.release,
            kernelBuildVersion = value.version,
            kernelBuildApplyStage = KERNEL_BUILD_EARLY_STAGE,
        ))
    }

    YukiAlertDialog(
        modifier = Modifier.padding(horizontal = 24.dp).widthIn(max = 560.dp).fillMaxWidth(),
        properties = DialogProperties(usePlatformDefaultWidth = false),
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.sumh_kernel_build_spoof)) },
        text = {
            Column(
                modifier = Modifier.fillMaxWidth().verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                OutlinedTextField(
                    value = release,
                    onValueChange = { release = it; saveRequested = false },
                    modifier = Modifier.fillMaxWidth(),
                    enabled = editable,
                    singleLine = true,
                    label = { Text(stringResource(R.string.sumh_kernel_build_release)) },
                    placeholder = { Text(runtime?.originalRelease ?: stringResource(R.string.sumh_kernel_build_use_original)) },
                    isError = !releaseValid,
                    supportingText = {
                        Text(stringResource(if (releaseValid) R.string.sumh_kernel_build_use_original
                            else R.string.sumh_kernel_build_release_invalid))
                    },
                    keyboardOptions = KeyboardOptions(autoCorrectEnabled = false, imeAction = ImeAction.Next),
                )
                OutlinedTextField(
                    value = version,
                    onValueChange = { version = it; saveRequested = false },
                    modifier = Modifier.fillMaxWidth(),
                    enabled = editable,
                    singleLine = true,
                    label = { Text(stringResource(R.string.sumh_kernel_build_version)) },
                    placeholder = { Text(runtime?.originalVersion ?: stringResource(R.string.sumh_kernel_build_use_original)) },
                    isError = !versionValid,
                    supportingText = {
                        Text(stringResource(if (versionValid) R.string.sumh_kernel_build_use_original
                            else R.string.sumh_kernel_build_invalid))
                    },
                    keyboardOptions = KeyboardOptions(autoCorrectEnabled = false, imeAction = ImeAction.Done),
                    keyboardActions = KeyboardActions(onDone = { save() }),
                )
                Text(stringResource(R.string.sumh_kernel_build_save_desc),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                if (read == KernelBuildRead.Failed) {
                    Text(stringResource(R.string.sumh_kernel_build_read_failed),
                        style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
                    TextButton(onClick = onRetry, enabled = editable, modifier = Modifier.heightIn(min = 48.dp)) {
                        Text(stringResource(R.string.retry))
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = { save() }, enabled = canSave, modifier = Modifier.heightIn(min = 48.dp)) {
                Text(stringResource(R.string.sumh_rule_save))
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss, modifier = Modifier.heightIn(min = 48.dp)) {
                Text(stringResource(R.string.sumh_rules_cancel))
            }
        },
    )
}
