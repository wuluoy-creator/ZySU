package com.zying.zysu.ui.sumh

import android.content.res.Configuration
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.zying.zysu.ui.util.SnackbarController
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.component.YukiTopAppBar
import com.zying.zysu.ui.component.YukiTopBarTitle
import com.zying.zysu.ui.screen.WorkspaceOperationGuard
import com.zying.zysu.ui.theme.UtilityPreviewTheme
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as SUMHManager
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter.SUMHStatus
import com.zying.zysu.ui.sumh.util.SUMHMapsPathError
import com.zying.zysu.ui.sumh.util.parseSUMHMapsNumber
import com.zying.zysu.ui.sumh.util.parseSUMHMapsDevice
import com.zying.zysu.ui.sumh.util.parseSUMHMapsRuleInput
import com.zying.zysu.ui.sumh.util.validateSUMHMapsPath
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.generated.destinations.SUMHConfigScreenDestination
import com.ramcosta.composedestinations.generated.destinations.SUMHCapabilitiesScreenDestination
import com.ramcosta.composedestinations.generated.destinations.SUMHProfilesScreenDestination
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.launch
import ui.screen.moreSettings.component.MoreSettingsItemPosition
import ui.screen.moreSettings.component.SwitchSettingItem


@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun SUMHConfigScreen(navigator: DestinationsNavigator) {
    var selectedSection by rememberSaveable { mutableStateOf(SUMHSection.Status) }
    var saving by remember { mutableStateOf(false) }
    WorkspaceOperationGuard(saving, SUMHConfigScreenDestination.route)
    Scaffold(
        topBar = {
            YukiTopAppBar(
                title = { YukiTopBarTitle(stringResource(R.string.sumh_title)) },
                navigationIcon = {
                    IconButton(onClick = { if (!saving) navigator.popBackStack() }, enabled = !saving) {
                        YukiIcon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.back))
                    }
                }
            )
        }
    ) { padding ->
        Column(Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding)) {
            SUMHTabs(selectedSection, enabled = !saving) { if (!saving) selectedSection = it }
            FlowRow(Modifier.fillMaxWidth().padding(horizontal = 18.dp)) {
                TextButton(
                    onClick = { navigator.navigate(SUMHCapabilitiesScreenDestination) },
                    enabled = !saving,
                    modifier = Modifier.heightIn(min = 48.dp),
                ) { Text(stringResource(R.string.sumh_capabilities_title)) }
                TextButton(
                    onClick = { navigator.navigate(SUMHProfilesScreenDestination) },
                    enabled = !saving,
                    modifier = Modifier.heightIn(min = 48.dp),
                ) { Text(stringResource(R.string.sumh_profiles_title)) }
            }
            Box(Modifier.weight(1f)) {
                SUMHWorkspace(selectedSection, onSavingChanged = { saving = it })
            }
        }
    }
}

@Composable
internal fun StatusTab(
    sumhStatus: SUMHStatus,
    version: String,
    systemInfo: SUMHManager.SystemInfo,
    onRefresh: () -> Unit,
    scrollable: Boolean = true,
    contentPadding: PaddingValues = PaddingValues(horizontal = 18.dp, vertical = 16.dp),
    refreshEnabled: Boolean = true,
    loading: Boolean = false,
    loadError: String? = null,
    controlState: com.zying.zysu.ui.sumh.util.SUMHControlState? = null,
    config: SUMHManager.SUMHPConfig = SUMHManager.SUMHPConfig(),
) {
    Column(
        modifier = (if (scrollable) Modifier.fillMaxSize().verticalScroll(rememberScrollState()) else Modifier.fillMaxWidth())
            .padding(contentPadding),
    ) {
        SUMHStatusSummaryCard(
            modifier = Modifier.fillMaxWidth().height(IntrinsicSize.Min),
            status = sumhStatus,
            version = version,
            systemInfo = systemInfo,
            onRefresh = onRefresh,
            refreshEnabled = refreshEnabled,
            loading = loading,
            loadError = loadError,
            includeRuntimeDetails = true,
            includeKernel = scrollable,
        )
        if (controlState != null) {
            Spacer(Modifier.height(12.dp))
            SUMHControlStatePanel(controlState, config, SUMHSection.Status, onRefresh, enabled = refreshEnabled)
        }
    }
}

@Composable
internal fun SettingSwitch(
    title: String,
    subtitle: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    enabled: Boolean = true,
    icon: ImageVector = Icons.Filled.Settings,
    position: MoreSettingsItemPosition = MoreSettingsItemPosition.Middle,
) {
    SwitchSettingItem(
        icon = icon,
        title = title,
        summary = subtitle,
        checked = checked,
        enabled = enabled,
        groupPosition = position,
        onChange = onCheckedChange,
    )
}

@Composable
internal fun SettingTextField(
    title: String,
    subtitle: String,
    value: String,
    onValueChange: (String) -> Unit,
    onConfirm: () -> Unit,
    enabled: Boolean = true,
    placeholder: String = "",
    position: MoreSettingsItemPosition = MoreSettingsItemPosition.Middle,
    confirmEnabled: Boolean = enabled,
) {
    var isEditing by rememberSaveable { mutableStateOf(false) }
    SUMHControlGroup(position) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = if (enabled) 1f else 0.5f),
                )
                Text(
                    text = subtitle,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = if (enabled) 1f else 0.5f),
                )
            }
            IconButton(onClick = { isEditing = !isEditing }, enabled = enabled) {
                YukiIcon(
                    imageVector = if (isEditing) Icons.Filled.Close else Icons.Filled.Edit,
                    contentDescription = if (isEditing) stringResource(R.string.close) else title,
                )
            }
        }
        if (isEditing) {
            OutlinedTextField(
                value = value,
                onValueChange = onValueChange,
                modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
                label = { Text(title) },
                placeholder = { if (placeholder.isNotEmpty()) Text(placeholder) },
                shape = MaterialTheme.shapes.small,
                singleLine = true,
                enabled = enabled,
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = {
                    if (confirmEnabled) onConfirm()
                }),
                trailingIcon = {
                    IconButton(onClick = onConfirm, enabled = confirmEnabled) {
                        YukiIcon(Icons.Filled.Done, contentDescription = stringResource(R.string.sumh_rule_save))
                    }
                },
            )
        } else if (value.isNotEmpty()) {
            Text(
                text = value,
                modifier = Modifier.padding(top = 8.dp),
                style = MaterialTheme.typography.bodyMedium,
                fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
internal fun MapsSpoofControls(
    enabled: Boolean,
    onClearRules: () -> Unit,
    onAddRule: (Long, Long, Long, Long, String) -> Unit,
) {
    var showClearConfirm by rememberSaveable { mutableStateOf(false) }
    var showAddDialog by rememberSaveable { mutableStateOf(false) }
    // Keep a failed submission or an accidentally dismissed dialog editable.
    var tIno by rememberSaveable { mutableStateOf("") }
    var tDev by rememberSaveable { mutableStateOf("0") }
    var sIno by rememberSaveable { mutableStateOf("") }
    var sDev by rememberSaveable { mutableStateOf("0") }
    var path by rememberSaveable { mutableStateOf("") }
    var submitted by rememberSaveable { mutableStateOf(false) }
    var editedFields by rememberSaveable { mutableIntStateOf(0) }
    var draftLimit by remember { mutableStateOf(false) }

    if (showClearConfirm) {
        YukiAlertDialog(
            onDismissRequest = { showClearConfirm = false },
            title = { Text(stringResource(R.string.sumh_maps_runtime_clear)) },
            text = { Text(stringResource(R.string.sumh_maps_runtime_clear_confirm)) },
            confirmButton = {
                TextButton(
                    enabled = enabled,
                    colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error),
                    onClick = {
                        if (!enabled) return@TextButton
                        showClearConfirm = false
                        onClearRules()
                    }
                ) {
                    Text(stringResource(R.string.sumh_rules_clear))
                }
            },
            dismissButton = {
                TextButton(onClick = { showClearConfirm = false }) {
                    Text(stringResource(R.string.sumh_rules_cancel))
                }
            }
        )
    }

    if (showAddDialog) {
        val focusRequesters = remember { List(5) { FocusRequester() } }
        val numbers = listOf(tIno, tDev, sIno, sDev)
        val numberErrors = numbers.mapIndexed { index, value ->
            (if (index == 1 || index == 3) parseSUMHMapsDevice(value) else parseSUMHMapsNumber(value)) == null
        }
        val pathError = validateSUMHMapsPath(path)
        val input = parseSUMHMapsRuleInput(tIno, tDev, sIno, sDev, path)
        fun submit() {
            if (!enabled) return
            submitted = true
            if (input == null) {
                val invalidIndex = numberErrors.indexOfFirst { it }.takeIf { it >= 0 } ?: 4
                focusRequesters[invalidIndex].requestFocus()
                return
            }
            showAddDialog = false
            submitted = false
            onAddRule(input.targetInode, input.targetDevice, input.spoofedInode, input.spoofedDevice, input.path)
        }
        YukiAlertDialog(
            onDismissRequest = { showAddDialog = false },
            title = { Text(stringResource(R.string.sumh_maps_runtime_add)) },
            text = {
                Column(
                    modifier = Modifier.fillMaxWidth().verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Text(
                        stringResource(R.string.sumh_maps_input_hint),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    if (draftLimit) SUMHSmallDraftLimit()
                    val labels = listOf(R.string.sumh_maps_target_ino, R.string.sumh_maps_target_dev,
                        R.string.sumh_maps_spoof_ino, R.string.sumh_maps_spoof_dev)
                    numbers.forEachIndexed { index, value ->
                        OutlinedTextField(
                            value = value,
                            onValueChange = {
                                draftLimit = !sumhSmallDraftFits(it)
                                if (!draftLimit) when (index) { 0 -> tIno = it; 1 -> tDev = it; 2 -> sIno = it; 3 -> sDev = it }
                                editedFields = editedFields or (1 shl index)
                            },
                            label = { Text(stringResource(labels[index])) },
                            modifier = Modifier.fillMaxWidth().focusRequester(focusRequesters[index]),
                            enabled = enabled,
                            shape = MaterialTheme.shapes.small,
                            singleLine = true,
                            isError = (submitted || editedFields and (1 shl index) != 0) && numberErrors[index],
                            supportingText = if ((submitted || editedFields and (1 shl index) != 0) && numberErrors[index]) {
                                { Text(stringResource(if (index == 1 || index == 3) R.string.sumh_maps_input_device_error
                                    else R.string.sumh_maps_input_number_error)) }
                            } else null,
                            keyboardOptions = KeyboardOptions(autoCorrectEnabled = false, keyboardType = KeyboardType.Number, imeAction = ImeAction.Next),
                            keyboardActions = KeyboardActions(onNext = { focusRequesters[index + 1].requestFocus() }),
                        )
                    }
                    OutlinedTextField(
                        value = path,
                        onValueChange = { draftLimit = !sumhSmallDraftFits(it); if (!draftLimit) path = it; editedFields = editedFields or (1 shl 4) },
                        label = { Text(stringResource(R.string.sumh_maps_spoof_path)) },
                        modifier = Modifier.fillMaxWidth().focusRequester(focusRequesters[4]),
                        enabled = enabled,
                        shape = MaterialTheme.shapes.small,
                        singleLine = true,
                        isError = (submitted || editedFields and (1 shl 4) != 0) && pathError != null,
                        supportingText = if ((submitted || editedFields and (1 shl 4) != 0) && pathError != null) {
                            { Text(stringResource(when (pathError) {
                                SUMHMapsPathError.AbsolutePath -> R.string.sumh_maps_input_path_absolute
                                SUMHMapsPathError.ControlCharacter -> R.string.sumh_maps_input_path_control
                                SUMHMapsPathError.TooLong -> R.string.sumh_maps_input_path_length
                            })) }
                        } else null,
                        keyboardOptions = KeyboardOptions(autoCorrectEnabled = false, imeAction = ImeAction.Done),
                        keyboardActions = KeyboardActions(onDone = { submit() }),
                    )
                }
            },
            confirmButton = {
                TextButton(
                    enabled = enabled && input != null,
                    onClick = { submit() }
                ) {
                    Text(stringResource(R.string.sumh_maps_add_action))
                }
            },
            dismissButton = {
                TextButton(onClick = { showAddDialog = false }) {
                    Text(stringResource(R.string.sumh_rules_cancel))
                }
            }
        )
    }

    SUMHControlGroup(MoreSettingsItemPosition.Last) {
        Text(
            text = stringResource(R.string.sumh_maps_desc),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(4.dp))
        Text(
            text = stringResource(R.string.sumh_maps_input_runtime),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(8.dp))
        FlowRow(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            OutlinedButton(
                onClick = { if (enabled) showClearConfirm = true },
                enabled = enabled,
                colors = ButtonDefaults.outlinedButtonColors(contentColor = MaterialTheme.colorScheme.error),
                modifier = Modifier.heightIn(min = 48.dp),
                shape = MaterialTheme.shapes.small,
            ) {
                YukiIcon(Icons.Filled.Delete, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.sumh_maps_runtime_clear))
            }
            FilledTonalButton(
                onClick = { if (enabled) showAddDialog = true },
                enabled = enabled,
                modifier = Modifier.heightIn(min = 48.dp),
                shape = MaterialTheme.shapes.small,
            ) {
                YukiIcon(Icons.Filled.Add, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.sumh_maps_runtime_add))
            }
        }
    }
}

internal fun CoroutineScope.launchUi(
    snackbar: SnackbarController,
    start: CoroutineStart = CoroutineStart.DEFAULT,
    action: suspend () -> Unit,
) = launch(start = start) {
    try { action() }
    catch (error: Exception) {
        if (error is CancellationException) throw error
        snackbar.showSnackbar(error.message ?: "Operation failed")
    }
}

@Preview(name = "SUMH status - light", widthDp = 360, heightDp = 800, locale = "zh-rCN")
@Preview(name = "SUMH status - dark", widthDp = 360, heightDp = 800, locale = "zh-rCN", uiMode = Configuration.UI_MODE_NIGHT_YES)
@Preview(name = "SUMH status - large text", widthDp = 320, heightDp = 800, locale = "zh-rCN", fontScale = 2f)
@Preview(name = "SUMH status - landscape", widthDp = 800, heightDp = 360)
@Composable
private fun SUMHStatusPreview() {
    UtilityPreviewTheme {
        Surface(color = MaterialTheme.colorScheme.background) {
            StatusTab(
                sumhStatus = SUMHStatus.AVAILABLE,
                version = "1",
                systemInfo = SUMHManager.SystemInfo(
                    kernel = "6.12.52-android16-5-g2cf44e703c2c-ab14552068-4k",
                    mountBase = "none",
                    activeMounts = emptyList(),
                    sumhModuleIds = emptyList(),
                    sumhMismatch = false,
                    mismatchMessage = null,
                    hooks = "vfs_getattr\nshow_vfsmnt",
                    viewsEnabled = true,
                ),
                onRefresh = {},
            )
        }
    }
}
