package com.zying.zysu.ui.sumh

import androidx.activity.ComponentActivity
import androidx.activity.compose.LocalActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.component.YukiTopAppBar
import com.zying.zysu.ui.component.YukiTopBarTitle
import com.zying.zysu.ui.sumh.util.*
import java.io.File

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun SUMHDiagnosticPackagesScreen(navigator: DestinationsNavigator) {
    val activity = requireNotNull(LocalActivity.current as? ComponentActivity)
    val context = LocalContext.current.applicationContext
    val operations: SUMHDiagnosticPackageOperations = viewModel(viewModelStoreOwner = activity, key = "sumh-diagnostic-packages")
    val export: SUMHDiagnosticPackageExport = viewModel(viewModelStoreOwner = activity, key = "sumh-diagnostic-package-export")
    var pickerExpected by rememberSaveable { mutableStateOf(false) }
    LaunchedEffect(operations) { operations.load(File(context.noBackupFilesDir, "sumh-diagnostic-packages")) }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri ->
        pickerExpected = false
        if (uri == null) export.cancel()
        else export.write(operations::exportReviewed) {
            requireNotNull(context.contentResolver.openOutputStream(uri, "wt"))
        }
    }
    // A new process cannot reuse review consent or content from a restored picker.
    LaunchedEffect(Unit) {
        if (pickerExpected && !export.busy) { pickerExpected = false; export.lost() }
    }
    Scaffold(topBar = {
        YukiTopAppBar(title = { YukiTopBarTitle(stringResource(R.string.sumh_packages_title)) }, navigationIcon = {
            IconButton(onClick = { navigator.navigateUp() }) {
                YukiIcon(Icons.AutoMirrored.Outlined.ArrowBack, stringResource(R.string.back))
            }
        })
    }, contentWindowInsets = WindowInsets.safeDrawing) { padding ->
        SUMHDiagnosticPackagesContent(
            loaded = operations.loaded, busy = operations.busy || export.busy,
            draft = operations.draft, capabilities = operations.capabilities,
            items = operations.items, preview = operations.preview,
            result = operations.result, error = operations.error, pending = operations.pending,
            pendingObservation = operations.pendingObservation, exportStatus = export.status,
            onDraft = operations::updateDraft, onRefresh = {
                if (operations.loaded) operations.refresh()
                else operations.load(File(context.noBackupFilesDir, "sumh-diagnostic-packages"))
            },
            onCreate = { operations.create() }, onPreview = { operations.inspect(it) },
            onRemove = { operations.remove(it) }, onRecover = { operations.recoverPending() },
            onAcknowledge = { operations.acknowledgePending() },
            onExport = { reviewed ->
                if (export.begin(reviewed)) {
                    pickerExpected = true
                    try { picker.launch("sumh-diagnostic-${reviewed.item.id}.json") }
                    catch (_: Exception) { pickerExpected = false; export.destinationFailed() }
                }
            }, modifier = Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding),
        )
    }
}

@Composable
internal fun SUMHDiagnosticPackagesContent(
    loaded: Boolean, busy: Boolean,
    draft: SUMHDiagnosticPackageDraft,
    capabilities: SUMHDiagnosticPackageCapabilities?,
    items: List<SUMHDiagnosticPackageItem>, preview: SUMHDiagnosticPackagePreview?,
    result: SUMHDiagnosticPackageResult?, error: String?,
    pending: SUMHDiagnosticPackagePending?, pendingObservation: SUMHDiagnosticPackageResult?,
    exportStatus: SUMHDiagnosticExportStatus,
    onDraft: (SUMHDiagnosticPackageDraft) -> Unit, onRefresh: () -> Unit, onCreate: () -> Unit,
    onPreview: (String) -> Unit, onRemove: (SUMHDiagnosticPackagePreview) -> Unit,
    onRecover: () -> Unit, onAcknowledge: () -> Unit,
    onExport: (SUMHDiagnosticPackagePreview) -> Unit,
    modifier: Modifier = Modifier,
) {
    var scenarioMenu by remember { mutableStateOf(false) }
    var confirmCreate by remember { mutableStateOf<SUMHDiagnosticPackageDraft?>(null) }
    var confirmRemove by remember { mutableStateOf<SUMHDiagnosticPackagePreview?>(null) }
    var expanded by remember(preview?.item?.id, preview?.item?.revision) { mutableStateOf(false) }
    var reviewed by remember(preview?.item?.id, preview?.item?.revision) { mutableStateOf(false) }
    val ready = loaded && !busy
    val noPending = pending == null
    val canCreate = ready && noPending && error == null && capabilities?.create == SUMHDiagnosticPackageSupport.Supported
    val chunks = remember(preview, expanded) { if (expanded) diagnosticPackageTextChunks(preview?.exportText.orEmpty()) else emptyList() }
    LazyColumn(modifier.testTag("packages_list"), contentPadding = PaddingValues(16.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        item("intro") {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.sumh_packages_intro))
                Text(stringResource(when {
                    busy -> R.string.sumh_packages_busy
                    error != null -> R.string.sumh_packages_unavailable
                    capabilities == null || capabilities.create == SUMHDiagnosticPackageSupport.Unknown && result == null && capabilities.error == null -> R.string.sumh_packages_idle
                    capabilities.create == SUMHDiagnosticPackageSupport.Unsupported -> R.string.sumh_packages_unsupported
                    capabilities.create != SUMHDiagnosticPackageSupport.Supported -> R.string.sumh_packages_unavailable
                    else -> R.string.sumh_packages_ready
                }), modifier = Modifier.testTag("packages_status").semantics { liveRegion = LiveRegionMode.Polite })
                if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
                OutlinedButton(onClick = onRefresh, enabled = !busy, modifier = Modifier.heightIn(min = 48.dp).testTag("packages_refresh")) {
                    Text(stringResource(R.string.sumh_packages_load))
                }
                error?.let { Text(stringResource(R.string.sumh_packages_failure_reason, it), color = MaterialTheme.colorScheme.error,
                    modifier = Modifier.testTag("packages_error")) }
                diagnosticExportMessage(exportStatus)?.let { Text(stringResource(it), modifier = Modifier.testTag("packages_export_status")) }
            }
        }
        if (pending != null) item("pending") {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                PackageHeading(R.string.sumh_packages_pending_title)
                Text(stringResource(R.string.sumh_packages_pending_scope))
                SelectionContainer { Text(pending.id, fontFamily = FontFamily.Monospace) }
                OutlinedButton(onClick = onRecover, enabled = ready && capabilities?.preview == SUMHDiagnosticPackageSupport.Supported,
                    modifier = Modifier.heightIn(min = 48.dp).testTag("packages_recover")) {
                    Text(stringResource(R.string.sumh_packages_query_pending))
                }
                pendingObservation?.let { PackageResult(it, "packages_pending_result") }
                TextButton(onClick = onAcknowledge, enabled = ready && pendingObservation?.error == null &&
                    pendingObservation?.status in setOf("observed", "not_found"),
                    modifier = Modifier.heightIn(min = 48.dp).testTag("packages_acknowledge")) {
                    Text(stringResource(R.string.sumh_packages_acknowledge))
                }
            }
        }
        item("create") {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                PackageHeading(R.string.sumh_packages_create_title)
                Box {
                    OutlinedButton(onClick = { scenarioMenu = true }, enabled = ready && noPending,
                        modifier = Modifier.heightIn(min = 48.dp).testTag("packages_scenario")) {
                        Text(stringResource(R.string.sumh_packages_scenario) + ": " + stringResource(draft.scenario.label()))
                    }
                    DropdownMenu(scenarioMenu, { scenarioMenu = false }) {
                        SUMHDiagnosticScenario.entries.forEach { scenario -> DropdownMenuItem(
                            text = { Text(stringResource(scenario.label())) }, onClick = {
                                scenarioMenu = false; onDraft(draft.copy(scenario = scenario, targetId = ""))
                            }, modifier = Modifier.testTag("packages_scenario_${scenario.wireName}")) }
                    }
                }
                if (draft.scenario.requiresTarget) OutlinedTextField(
                    value = draft.targetId, onValueChange = { if (it.length <= 4096) onDraft(draft.copy(targetId = it)) },
                    enabled = ready && noPending, singleLine = true,
                    label = { Text(stringResource(if (draft.scenario == SUMHDiagnosticScenario.UserHide) R.string.sumh_packages_definition_id else R.string.sumh_packages_module_id)) },
                    supportingText = { Text(stringResource(R.string.sumh_packages_id_hint)) },
                    modifier = Modifier.fillMaxWidth().testTag("packages_target"),
                )
                OutlinedTextField(value = draft.operationId, onValueChange = { if (it.length <= 80) onDraft(draft.copy(operationId = it)) },
                    enabled = ready && noPending, singleLine = true, label = { Text(stringResource(R.string.sumh_packages_operation_id)) },
                    modifier = Modifier.fillMaxWidth().testTag("packages_operation"))
                Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).toggleable(draft.includeIdentifiers,
                    enabled = ready && noPending, role = Role.Checkbox, onValueChange = { onDraft(draft.copy(includeIdentifiers = it)) })
                    .testTag("packages_identifiers")) {
                    Checkbox(draft.includeIdentifiers, onCheckedChange = null)
                    Text(stringResource(R.string.sumh_packages_include), Modifier.padding(start = 8.dp, top = 12.dp))
                }
                Text(stringResource(if (draft.includeIdentifiers) R.string.sumh_packages_identifiers_warning else R.string.sumh_packages_redaction),
                    style = MaterialTheme.typography.bodySmall)
                if (draft.validationError != null) Text(stringResource(R.string.sumh_packages_invalid_input), color = MaterialTheme.colorScheme.error)
                Button(onClick = { if (draft.includeIdentifiers) confirmCreate = draft else onCreate() },
                    enabled = canCreate && draft.validationError == null, modifier = Modifier.heightIn(min = 48.dp).testTag("packages_create")) {
                    Text(stringResource(R.string.sumh_packages_create))
                }
            }
        }
        result?.let { value -> item("result") { PackageResult(value, "packages_result") } }
        item("retained-title") {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                PackageHeading(R.string.sumh_packages_retained_title)
                Text(stringResource(R.string.sumh_packages_capacity), style = MaterialTheme.typography.bodySmall)
                if (capabilities?.list == SUMHDiagnosticPackageSupport.Supported && items.isEmpty() && result?.action == "list" && result.status == "observed")
                    Text(stringResource(R.string.sumh_packages_empty))
            }
        }
        itemsIndexed(items, key = { _, item -> "retained_${item.id}" }) { _, item ->
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    PackageMetadata(item)
                    OutlinedButton(onClick = { onPreview(item.id) }, enabled = ready && capabilities?.preview == SUMHDiagnosticPackageSupport.Supported,
                        modifier = Modifier.heightIn(min = 48.dp).testTag("packages_preview_${item.id}")) { Text(stringResource(R.string.sumh_packages_preview)) }
                }
            }
        }
        preview?.let { value ->
            item("preview") {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    PackageHeading(R.string.sumh_packages_preview_title)
                    PackageMetadata(value.item)
                    SelectionContainer { Text(stringResource(R.string.sumh_packages_revision, value.item.revision), fontFamily = FontFamily.Monospace,
                        style = MaterialTheme.typography.bodySmall) }
                    TextButton(onClick = { expanded = !expanded }, modifier = Modifier.heightIn(min = 48.dp).testTag("packages_full_content")) {
                        Text(stringResource(if (expanded) R.string.sumh_packages_hide_details else R.string.sumh_packages_details))
                    }
                }
            }
            itemsIndexed(chunks, key = { index, _ -> "content_$index" }) { index, text ->
                SelectionContainer { Text(text, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.fillMaxWidth().testTag("packages_content_$index")) }
            }
            item("preview-actions") {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).toggleable(reviewed, enabled = ready,
                        role = Role.Checkbox, onValueChange = { reviewed = it }).testTag("packages_reviewed")) {
                        Checkbox(reviewed, onCheckedChange = null)
                        Text(stringResource(R.string.sumh_packages_reviewed), Modifier.padding(start = 8.dp, top = 12.dp))
                    }
                    Button(onClick = { onExport(value) }, enabled = ready && reviewed && error == null && capabilities?.export == SUMHDiagnosticPackageSupport.Supported,
                        modifier = Modifier.heightIn(min = 48.dp).testTag("packages_export")) { Text(stringResource(R.string.sumh_packages_export)) }
                    OutlinedButton(onClick = { confirmRemove = value }, enabled = ready && noPending && reviewed && error == null && capabilities?.remove == SUMHDiagnosticPackageSupport.Supported,
                        modifier = Modifier.heightIn(min = 48.dp).testTag("packages_remove")) { Text(stringResource(R.string.sumh_packages_remove)) }
                }
            }
        }
    }
    confirmCreate?.let { original -> YukiAlertDialog(onDismissRequest = { confirmCreate = null },
        title = { Text(stringResource(R.string.sumh_packages_create_confirm)) },
        text = { Text(stringResource(R.string.sumh_packages_identifiers_warning)) },
        confirmButton = { TextButton(onClick = { confirmCreate = null; onCreate() },
            enabled = canCreate && draft == original, modifier = Modifier.testTag("packages_confirm_create")) { Text(stringResource(R.string.sumh_packages_confirm)) } },
        dismissButton = { TextButton(onClick = { confirmCreate = null }) { Text(stringResource(R.string.sumh_packages_cancel)) } }) }
    confirmRemove?.let { original -> YukiAlertDialog(onDismissRequest = { confirmRemove = null },
        title = { Text(stringResource(R.string.sumh_packages_remove_title)) },
        text = { Column { Text(stringResource(R.string.sumh_packages_remove_scope)); Text(original.item.id, fontFamily = FontFamily.Monospace) } },
        confirmButton = { TextButton(onClick = { confirmRemove = null; onRemove(original) },
            enabled = ready && noPending && preview == original && error == null,
            modifier = Modifier.testTag("packages_confirm_remove")) { Text(stringResource(R.string.sumh_packages_confirm)) } },
        dismissButton = { TextButton(onClick = { confirmRemove = null }) { Text(stringResource(R.string.sumh_packages_cancel)) } }) }
}

@Composable private fun PackageHeading(label: Int) { Text(stringResource(label), style = MaterialTheme.typography.titleMedium, modifier = Modifier.semantics { heading() }) }
@Composable private fun PackageMetadata(item: SUMHDiagnosticPackageItem) {
    SelectionContainer { Text(item.id, fontFamily = FontFamily.Monospace) }
    SUMHDiagnosticScenario.entries.firstOrNull { it.wireName == item.scenario }?.let { Text(stringResource(it.label()), style = MaterialTheme.typography.labelLarge) }
    Text(stringResource(R.string.sumh_packages_metadata,
        stringResource(if (item.privacyPolicy == "redacted") R.string.sumh_packages_policy_redacted else R.string.sumh_packages_policy_identifiers),
        stringResource(diagnosticStatusLabel(item.diagnosticStatus)), item.byteCount), style = MaterialTheme.typography.bodyMedium)
}
@Composable private fun PackageResult(result: SUMHDiagnosticPackageResult, tag: String) {
    Column(Modifier.testTag(tag).semantics { liveRegion = LiveRegionMode.Polite }, verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(stringResource(when (result.status) {
            "saved" -> R.string.sumh_packages_saved; "deleted" -> R.string.sumh_packages_deleted
            "not_found" -> R.string.sumh_packages_missing; "conflict" -> R.string.sumh_packages_conflict
            "not_saved" -> R.string.sumh_packages_not_saved; "unknown" -> R.string.sumh_packages_unknown
            "observed" -> R.string.sumh_packages_ready; else -> R.string.sumh_packages_unavailable
        }))
        result.id?.let { SelectionContainer { Text(it, fontFamily = FontFamily.Monospace) } }
        if (result.status != "observed") Text(stringResource(R.string.sumh_packages_failure_reason, result.reason), style = MaterialTheme.typography.bodySmall)
        result.diagnosticStatus?.let { Text(stringResource(diagnosticStatusLabel(it))) }
    }
}
private fun SUMHDiagnosticScenario.label() = when (this) {
    SUMHDiagnosticScenario.UserHide -> R.string.sumh_packages_hide
    SUMHDiagnosticScenario.ModuleFallback -> R.string.sumh_packages_module
    SUMHDiagnosticScenario.MapsFailure -> R.string.sumh_packages_maps
    SUMHDiagnosticScenario.BootProtection -> R.string.sumh_packages_boot
}
private fun diagnosticStatusLabel(status: String) = when (status) {
    "observed" -> R.string.sumh_packages_complete; "partial" -> R.string.sumh_packages_partial
    "stale" -> R.string.sumh_packages_stale; "invalid" -> R.string.sumh_packages_source_invalid
    else -> R.string.sumh_packages_source_unavailable
}
private fun diagnosticExportMessage(status: SUMHDiagnosticExportStatus): Int? = when (status) {
    SUMHDiagnosticExportStatus.Choosing -> R.string.sumh_packages_export_pending
    SUMHDiagnosticExportStatus.Writing -> R.string.sumh_packages_busy
    SUMHDiagnosticExportStatus.Saved -> R.string.sumh_packages_export_done
    SUMHDiagnosticExportStatus.Failed -> R.string.sumh_packages_export_failed
    SUMHDiagnosticExportStatus.Lost -> R.string.sumh_packages_export_reselect
    SUMHDiagnosticExportStatus.Idle -> null
}
