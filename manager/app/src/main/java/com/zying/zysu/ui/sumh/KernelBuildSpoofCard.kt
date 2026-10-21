package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.KERNEL_BUILD_EARLY_STAGE
import com.zying.zysu.ui.sumh.util.KERNEL_BUILD_LATE_STAGE
import com.zying.zysu.ui.sumh.util.SUMHManager
import com.zying.zysu.ui.sumh.util.SUMHFeatureState
import com.zying.zysu.ui.sumh.util.SUMHFeatureSupport
import com.zying.zysu.ui.sumh.util.SUMHManagedFeature
import com.zying.zysu.ui.sumh.util.KernelBuildState
import com.zying.zysu.ui.sumh.util.validKernelBuildField
import com.zying.zysu.ui.sumh.util.validKernelBuildOverride
import kotlinx.coroutines.CancellationException
import ui.screen.moreSettings.component.MoreSettingsItemPosition
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as Controller

internal sealed interface KernelBuildRead {
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
    // First-time enabling opens a draft; only a valid save changes the kernel.
    var pendingEnable by rememberSaveable { mutableStateOf(false) }
    val editorState = rememberSaveableStateHolder()
    val expanded = config.enableKernelBuildSpoof || pendingEnable
    var editing by rememberSaveable { mutableStateOf(false) }
    val editorVisible = pendingEnable || (expanded && editing)
    var refresh by remember { mutableIntStateOf(0) }
    var read by remember { mutableStateOf<KernelBuildRead>(KernelBuildRead.Unavailable) }
    val revision by SUMHManager.revision.collectAsState()
    val editable = controlsEnabled && kernelEnabled && supported
    val canDisable = controlsEnabled && expanded
    LaunchedEffect(config.enableKernelBuildSpoof) {
        if (config.enableKernelBuildSpoof && pendingEnable) { pendingEnable = false; editing = true }
    }
    LaunchedEffect(supported, config.kernelAvailable, revision, refresh, expanded,
        config.enableKernelBuildSpoof, config.kernelBuildRelease, config.kernelBuildVersion,
        config.kernelBuildApplyStage) {
        if (!supported || !config.kernelAvailable || !expanded) {
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
    ConfigSection(
        title = stringResource(R.string.sumh_kernel_build_spoof),
        segmented = true,
        actions = headingActions,
    ) {
        SettingSwitch(
            title = stringResource(R.string.sumh_kernel_build_enable),
            subtitle = sumhFeatureSummary(stringResource(when {
                pendingEnable && !config.enableKernelBuildSpoof -> R.string.sumh_kernel_build_enable_pending
                else -> R.string.sumh_kernel_build_spoof_desc
            }), SUMHFeatureState(SUMHManagedFeature.KernelBuild, config.enableKernelBuildSpoof, support)),
            checked = expanded,
            onCheckedChange = { requested ->
                if (!requested) {
                    pendingEnable = false
                    if (config.enableKernelBuildSpoof) {
                        onConfigChanged(config.copy(enableKernelBuildSpoof = false))
                    }
                } else {
                    val savedOverrideValid = validKernelBuildOverride(
                        true, config.kernelBuildRelease, config.kernelBuildVersion,
                    ) && config.kernelBuildApplyStage in listOf(KERNEL_BUILD_EARLY_STAGE, KERNEL_BUILD_LATE_STAGE)
                    if (savedOverrideValid) {
                        onConfigChanged(config.copy(enableKernelBuildSpoof = true))
                    } else {
                        pendingEnable = true
                    }
                }
            },
            enabled = editable || canDisable,
            icon = Icons.Filled.Memory,
            position = if (expanded) MoreSettingsItemPosition.First else MoreSettingsItemPosition.Only,
        )
        if (expanded) {
            SUMHControlGroup(if (editorVisible) MoreSettingsItemPosition.Middle else MoreSettingsItemPosition.Last) {
                TextButton(onClick = { editing = !editing }, enabled = !pendingEnable && controlsEnabled) {
                    Text(stringResource(if (editorVisible) R.string.sumh_rules_done else R.string.sumh_rules_edit_settings))
                }
            }
        }
        if (editorVisible) {
            editorState.SaveableStateProvider("kernel-build-draft") {
                SUMHControlGroup(MoreSettingsItemPosition.Last) {
                    KernelBuildSpoofEditor(
                        config = config,
                        editable = editable,
                        read = read,
                        onRetry = { refresh++ },
                        onSave = { release, version, stage ->
                            onConfigChanged(config.copy(
                                enableKernelBuildSpoof = true,
                                kernelBuildRelease = release,
                                kernelBuildVersion = version,
                                kernelBuildApplyStage = stage,
                            ))
                        },
                    )
                }
            }
        }
    }
}

@Composable
private fun KernelBuildRuntime(
    read: KernelBuildRead,
    showOriginal: Boolean,
    retryEnabled: Boolean,
    onRetry: () -> Unit,
) {
    when (read) {
        KernelBuildRead.Unavailable -> Unit
        KernelBuildRead.Loading -> Text(stringResource(R.string.sumh_kernel_build_loading),
            style = MaterialTheme.typography.bodySmall)
        KernelBuildRead.Failed -> {
            Text(stringResource(R.string.sumh_kernel_build_read_failed),
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
            TextButton(onClick = onRetry, enabled = retryEnabled, modifier = Modifier.heightIn(min = 48.dp)) {
                Text(stringResource(R.string.retry))
            }
        }
        is KernelBuildRead.Ready -> {
            val runtime = read.value
            Text(stringResource(if (runtime.enabled) R.string.sumh_kernel_build_live_enabled
                else R.string.sumh_kernel_build_live_disabled), style = MaterialTheme.typography.labelLarge)
            Text(runtime.release, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
            Text(runtime.version, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
            if (showOriginal) {
                Text(stringResource(R.string.sumh_kernel_build_original), style = MaterialTheme.typography.labelLarge)
                Text(runtime.originalRelease, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
                Text(runtime.originalVersion, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
                Text(stringResource(R.string.sumh_kernel_build_original_kmi,
                    runtime.originalKmiLabel ?: stringResource(R.string.sumh_kernel_build_kmi_unknown)),
                    style = MaterialTheme.typography.bodySmall)
                Text(stringResource(R.string.sumh_kernel_build_kmi_desc),
                    style = MaterialTheme.typography.bodySmall)
            }
            TextButton(onClick = onRetry, enabled = retryEnabled, modifier = Modifier.heightIn(min = 48.dp)) {
                Text(stringResource(R.string.sumh_kernel_build_refresh))
            }
        }
    }
}

@Composable
internal fun KernelBuildSpoofEditor(
    config: Controller.SUMHPConfig,
    editable: Boolean,
    read: KernelBuildRead,
    onRetry: () -> Unit,
    onSave: (String, String, String) -> Unit,
) {
    val releaseDraft = rememberSUMHRefreshAwareDraft(config.kernelBuildRelease)
    val versionDraft = rememberSUMHRefreshAwareDraft(config.kernelBuildVersion)
    val stageDraft = rememberSUMHRefreshAwareDraft(config.kernelBuildApplyStage)
    val release = releaseDraft.text
    val version = versionDraft.text
    val stage = stageDraft.text
    val conflict = releaseDraft.conflict(config.kernelBuildRelease) || versionDraft.conflict(config.kernelBuildVersion) ||
        stageDraft.conflict(config.kernelBuildApplyStage)
    var draftLimit by remember { mutableStateOf(false) }
    val runtime = (read as? KernelBuildRead.Ready)?.value
    val dateSuggestion = runtime?.systemDateSuggestion
    val releaseValid = validKernelBuildField(release, release = true)
    val versionValid = validKernelBuildField(version)
    val missingOverride = release.isEmpty() && version.isEmpty()
    val changed = !config.enableKernelBuildSpoof || release != config.kernelBuildRelease ||
        version != config.kernelBuildVersion || stage != config.kernelBuildApplyStage
    val canSave = editable && !conflict && changed && validKernelBuildOverride(true, release, version) &&
        stage in listOf(KERNEL_BUILD_EARLY_STAGE, KERNEL_BUILD_LATE_STAGE)
    val stages = listOf(
        KERNEL_BUILD_EARLY_STAGE to stringResource(R.string.sumh_kernel_build_stage_early),
        KERNEL_BUILD_LATE_STAGE to stringResource(R.string.sumh_kernel_build_stage_late),
    )
    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        if (draftLimit) SUMHSmallDraftLimit()
        OutlinedTextField(
            value = release,
            onValueChange = { draftLimit = !sumhSmallDraftFits(it); if (!draftLimit) releaseDraft.edit(it) },
            modifier = Modifier.fillMaxWidth(),
            enabled = editable,
            singleLine = true,
            label = { Text(stringResource(R.string.sumh_kernel_build_release)) },
            placeholder = {
                Text(runtime?.originalRelease ?: stringResource(R.string.sumh_kernel_build_use_original))
            },
            isError = !releaseValid,
            supportingText = {
                Text(stringResource(if (releaseValid) R.string.sumh_kernel_build_use_original
                    else R.string.sumh_kernel_build_release_invalid))
            },
            keyboardOptions = KeyboardOptions(autoCorrectEnabled = false, imeAction = ImeAction.Next),
        )
        SUMHDraftConflict(releaseDraft, config.kernelBuildRelease, stringResource(R.string.sumh_kernel_build_release))
        TextButton(
            onClick = { runtime?.let { releaseDraft.edit(it.originalRelease); draftLimit = false } },
            enabled = editable && runtime != null && runtime.originalRelease.isNotEmpty() &&
                validKernelBuildField(runtime.originalRelease, release = true) && release != runtime.originalRelease,
            modifier = Modifier.heightIn(min = 48.dp),
        ) {
            Text(stringResource(R.string.sumh_kernel_build_fill_original_release))
        }
        OutlinedTextField(
            value = version,
            onValueChange = { draftLimit = !sumhSmallDraftFits(it); if (!draftLimit) versionDraft.edit(it) },
            modifier = Modifier.fillMaxWidth(),
            enabled = editable,
            singleLine = true,
            label = { Text(stringResource(R.string.sumh_kernel_build_version)) },
            placeholder = {
                Text(runtime?.originalVersion ?: stringResource(R.string.sumh_kernel_build_use_original))
            },
            isError = !versionValid,
            supportingText = {
                Text(stringResource(if (versionValid) R.string.sumh_kernel_build_use_original
                    else R.string.sumh_kernel_build_invalid))
            },
            keyboardOptions = KeyboardOptions(autoCorrectEnabled = false, imeAction = ImeAction.Done),
        )
        SUMHDraftConflict(versionDraft, config.kernelBuildVersion, stringResource(R.string.sumh_kernel_build_version))
        TextButton(
            onClick = { runtime?.let { versionDraft.edit(it.originalVersion); draftLimit = false } },
            enabled = editable && runtime != null && runtime.originalVersion.isNotEmpty() &&
                validKernelBuildField(runtime.originalVersion) && version != runtime.originalVersion,
            modifier = Modifier.heightIn(min = 48.dp),
        ) {
            Text(stringResource(R.string.sumh_kernel_build_fill_original_version))
        }
        TextButton(
            onClick = { dateSuggestion?.let { versionDraft.edit(it); draftLimit = false } },
            enabled = editable && dateSuggestion != null && version != dateSuggestion,
            modifier = Modifier.heightIn(min = 48.dp),
        ) {
            Text(stringResource(R.string.sumh_kernel_build_fill_system_date))
        }
        Text(stringResource(if (dateSuggestion != null) R.string.sumh_kernel_build_system_date_source
            else R.string.sumh_kernel_build_system_date_unavailable),
            style = MaterialTheme.typography.bodySmall)
        Text(stringResource(R.string.sumh_kernel_build_helpers_draft_only),
            style = MaterialTheme.typography.bodySmall)
        if (missingOverride) {
            Text(stringResource(R.string.sumh_kernel_build_override_required),
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
        }
        ConfigChoiceField(
            title = stringResource(R.string.sumh_kernel_build_apply_stage),
            valueLabel = stages.firstOrNull { it.first == stage }?.second ?: stage,
            choices = stages,
            enabled = editable,
            selected = { stageDraft.edit(it) },
        )
        SUMHDraftConflict(stageDraft, config.kernelBuildApplyStage, stringResource(R.string.sumh_kernel_build_apply_stage))
        Text(stringResource(R.string.sumh_kernel_build_stage_desc),
            style = MaterialTheme.typography.bodySmall)
        if (releaseValid && versionValid && !missingOverride) {
            Text(stringResource(R.string.sumh_kernel_build_draft_preview),
                style = MaterialTheme.typography.labelLarge)
            KernelBuildDraftField(stringResource(R.string.sumh_kernel_build_release),
                release.ifEmpty { runtime?.originalRelease.orEmpty() }, runtime?.release)
            KernelBuildDraftField(stringResource(R.string.sumh_kernel_build_version),
                version.ifEmpty { runtime?.originalVersion.orEmpty() }, runtime?.version)
        }
        Button(
            onClick = {
                if (!canSave) return@Button
                releaseDraft.submitted(); versionDraft.submitted(); stageDraft.submitted()
                onSave(release, version, stage)
            },
            enabled = canSave,
            modifier = Modifier.align(Alignment.End).heightIn(min = 48.dp),
        ) {
            Text(stringResource(R.string.sumh_rule_save))
        }
        Text(stringResource(R.string.sumh_kernel_build_restore),
            style = MaterialTheme.typography.bodySmall)
        KernelBuildRuntime(read, showOriginal = true, retryEnabled = editable, onRetry = onRetry)
    }
}

@Composable
private fun KernelBuildDraftField(label: String, value: String, current: String?) {
    Text(label, style = MaterialTheme.typography.labelMedium)
    Text(value.ifEmpty { stringResource(R.string.sumh_kernel_build_original_pending) },
        style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
    Text(stringResource(when {
        current == null -> R.string.sumh_kernel_build_comparison_unavailable
        value == current -> R.string.sumh_kernel_build_matches_current
        else -> R.string.sumh_kernel_build_differs_current
    }), style = MaterialTheme.typography.bodySmall)
}
