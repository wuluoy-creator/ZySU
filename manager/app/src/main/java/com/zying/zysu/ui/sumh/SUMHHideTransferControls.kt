package com.zying.zysu.ui.sumh

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.ImportExport
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.core.net.toUri
import androidx.lifecycle.viewmodel.compose.viewModel
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.sumh.util.*
import java.io.ByteArrayInputStream
import java.io.File

@Composable internal fun HideTransferControls(selected: Set<String>, enabled: Boolean, onBusy: (Boolean) -> Unit,
    onApplied: () -> Unit, read: HideSavedRead = { SUMHManager.savedHideDefinitions() },
    runtime: HideDefinitionRead = { SUMHManager.hideDefinitions() }, support: HideImportSupport = { SUMHManager.hideImportSupported() },
    change: HideDefinitionChange = SUMHManager::changeHideDefinition,
    operation: SUMHHideTransferOperations = viewModel()) {
    val context = LocalContext.current
    var opened by rememberSaveable { mutableStateOf(false) }
    var textOpen by rememberSaveable { mutableStateOf(false) }
    var text by rememberSaveable { mutableStateOf("") }
    var textError by remember { mutableStateOf(false) }
    var menu by remember { mutableStateOf(false) }
    var dismissedFeedback by remember { mutableStateOf<String?>(null) }
    // Only small CAS/token values enter saved Activity state; documents stay private files.
    var importFingerprint by rememberSaveable { mutableStateOf<String?>(null) }
    var exportToken by rememberSaveable { mutableStateOf<String?>(null) }
    var pendingImportUri by rememberSaveable { mutableStateOf<String?>(null) }
    var pendingExportUri by rememberSaveable { mutableStateOf<String?>(null) }
    val available = enabled && operation.loaded && !operation.busy
    val latestBusy by rememberUpdatedState(onBusy)
    SideEffect { latestBusy(operation.busy) }
    LaunchedEffect(Unit) { operation.load(File(context.applicationContext.noBackupFilesDir, "sumh-hide-transfer")) }
    val revision by SUMHManager.revision.collectAsState()
    LaunchedEffect(revision) { operation.invalidate() }
    val openFile = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) pendingImportUri = uri.toString()
    }
    val createFile = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri ->
        if (uri != null) pendingExportUri = uri.toString() else exportToken = null
    }
    // A restored picker callback may precede storage loading. Keep its small URI/token until accepted.
    LaunchedEffect(operation.loaded, operation.busy, pendingImportUri, pendingExportUri) {
        if (operation.loaded && !operation.busy) {
            pendingImportUri?.let { value ->
                if (operation.stage(importFingerprint) { requireNotNull(context.contentResolver.openInputStream(value.toUri())) }) pendingImportUri = null
            }
            val token = exportToken
            if (!operation.busy && token != null) pendingExportUri?.let { value ->
                if (operation.writeExport(token) { requireNotNull(context.contentResolver.openOutputStream(value.toUri(), "wt")) }) {
                    pendingExportUri = null; exportToken = null
                }
            }
        }
    }
    LaunchedEffect(operation.exportReady?.token) {
        operation.exportReady?.let { artifact ->
            exportToken = artifact.token; operation.consumeExportReady()
            createFile.launch("sumh-hide-${artifact.scope}.json")
        }
    }
    Box {
        IconButton(onClick = { menu = true }, enabled = available, modifier = Modifier.testTag("hide_transfer_menu")) {
            Icon(Icons.Outlined.ImportExport, stringResource(R.string.sumh_hide_transfer_title))
        }
        DropdownMenu(menu, { menu = false }) {
            DropdownMenuItem(onClick = { menu = false; opened = true }, enabled = available,
                modifier = Modifier.testTag("hide_transfer_open"), text = { Text(stringResource(R.string.sumh_hide_transfer_open)) })
            DropdownMenuItem(onClick = { menu = false; dismissedFeedback = null; operation.captureExport(null, read) }, enabled = available,
                modifier = Modifier.testTag("hide_transfer_export_all"), text = { Text(stringResource(R.string.sumh_hide_transfer_export_all)) })
            if (selected.isNotEmpty()) DropdownMenuItem(onClick = { menu = false; dismissedFeedback = null; operation.captureExport(selected, read) }, enabled = available,
                modifier = Modifier.testTag("hide_transfer_export_selected"), text = { Text(stringResource(R.string.sumh_hide_transfer_export_selected, selected.size)) })
            }
        if (operation.busy && !opened) CircularProgressIndicator(Modifier.size(24.dp))
    }
    val feedback = operation.error ?: if (operation.exportDone) stringResource(R.string.sumh_hide_transfer_export_done) else null
    if ((!opened || !operation.loaded) && !operation.busy && feedback != null && feedback != dismissedFeedback)
        YukiAlertDialog(onDismissRequest = { dismissedFeedback = feedback }, title = { Text(stringResource(R.string.sumh_hide_transfer_title)) },
            text = { Text(feedback.take(1500)) }, confirmButton = {
                TextButton(onClick = { if (!operation.loaded) operation.load(File(context.applicationContext.noBackupFilesDir, "sumh-hide-transfer"))
                    else dismissedFeedback = feedback }) { Text(stringResource(if (operation.loaded) android.R.string.ok else R.string.sumh_rules_refresh)) }
            }, dismissButton = { TextButton(onClick = { dismissedFeedback = feedback; pendingImportUri = null; pendingExportUri = null; exportToken = null }) {
                Text(stringResource(R.string.sumh_rules_cancel))
            } })
    if (opened) HideTransferDialog(operation.documentBytes, operation.draft.preserveImportedEnabled,
        operation.review, operation.reviewed, operation.busy, operation.applying, operation.loaded,
        operation.error, operation.progress, operation.batch,
        onFile = { importFingerprint = operation.fingerprint; openFile.launch(arrayOf("application/json", "text/plain", "application/octet-stream")) },
        onText = { textOpen = true; importFingerprint = operation.fingerprint; textError = false },
        onPreserveEnabled = operation::preserve, onChoice = operation::choose,
        onReview = { operation.preview(read) }, onApply = { operation.apply(read, runtime, support, change, onApplied) },
        onStop = operation::requestStop, onDismiss = { opened = false })
    if (textOpen) YukiAlertDialog(onDismissRequest = { textOpen = false },
        title = { Text(stringResource(R.string.sumh_hide_transfer_text)) }, text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.sumh_hide_transfer_text_hint))
                OutlinedTextField(text, { value ->
                    if (validHideDefinitionText(value, SUMH_HIDE_TRANSFER_TEXT_BYTES)) { text = value; textError = false }
                    else textError = true
                }, modifier = Modifier.fillMaxWidth().testTag("hide_transfer_json"), minLines = 4, maxLines = 8)
                if (textError) Text("hide_transfer_text_capacity", color = MaterialTheme.colorScheme.error)
                operation.error?.let { Text(it.take(1500), color = MaterialTheme.colorScheme.error) }
                if (operation.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
            }
        }, confirmButton = {
            TextButton(onClick = { operation.stage(importFingerprint) { ByteArrayInputStream(text.toByteArray(Charsets.UTF_8)) } },
                enabled = available && !textError, modifier = Modifier.testTag("hide_transfer_stage")) { Text(stringResource(R.string.sumh_hide_transfer_stage)) }
        }, dismissButton = { TextButton(onClick = { textOpen = false }) { Text(stringResource(R.string.sumh_rules_cancel)) } })
    LaunchedEffect(operation.fingerprint) {
        if (textOpen && operation.fingerprint != importFingerprint && operation.error == null) textOpen = false
    }
}
