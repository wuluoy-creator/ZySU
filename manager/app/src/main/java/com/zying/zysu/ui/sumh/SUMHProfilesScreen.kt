package com.zying.zysu.ui.sumh

import androidx.activity.ComponentActivity
import androidx.activity.compose.LocalActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
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
import androidx.compose.ui.window.DialogProperties
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
import java.util.UUID

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun SUMHProfilesScreen(navigator: DestinationsNavigator) {
    val activity = requireNotNull(LocalActivity.current as? ComponentActivity)
    val context = LocalContext.current.applicationContext
    val operations: SUMHProfileOperations = viewModel(viewModelStoreOwner = activity, key = "sumh-profiles")
    // Only a small, exact artifact token crosses the system picker boundary.
    var pickerToken by rememberSaveable { mutableStateOf<String?>(null) }
    var exportRequested by remember { mutableStateOf(false) }
    val exportPicker = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri ->
        val token = pickerToken
        pickerToken = null
        if (uri == null) operations.exportCancelled()
        else if (token == null || operations.exportArtifact?.token != token) operations.reportError("profile_export_session_lost")
        else operations.writeExport(token) { requireNotNull(context.contentResolver.openOutputStream(uri, "wt")) }
    }
    val importPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) operations.stageImport { requireNotNull(context.contentResolver.openInputStream(uri)) }
    }
    LaunchedEffect(operations) { operations.load(File(context.noBackupFilesDir, "sumh-profiles")) }
    LaunchedEffect(operations.loaded) {
        if (operations.loaded && operations.form.id.isEmpty()) operations.updateForm(operations.form.copy(id = newProfileUiId()))
    }
    LaunchedEffect(operations.exportArtifact?.token, operations.exportStatus, operations.busy, exportRequested) {
        val artifact = operations.exportArtifact
        if (exportRequested && !operations.busy && operations.exportStatus == "ready" && artifact != null && operations.error == null) {
            exportRequested = false
            pickerToken = artifact.token
            try { exportPicker.launch("sumh-profile-${artifact.id}.json") }
            catch (_: Exception) { pickerToken = null; operations.exportCancelled(); operations.reportError("profile_export_picker_failed") }
        } else if (exportRequested && !operations.busy) exportRequested = false
    }
    Scaffold(topBar = {
        YukiTopAppBar(title = { YukiTopBarTitle(stringResource(R.string.sumh_profiles_title)) }, navigationIcon = {
            IconButton(onClick = { navigator.navigateUp() }) {
                YukiIcon(Icons.AutoMirrored.Outlined.ArrowBack, stringResource(R.string.back))
            }
        })
    }, contentWindowInsets = WindowInsets.safeDrawing) { padding ->
        SUMHProfilesContent(
            state = SUMHProfilesUiState(
                loaded = operations.loaded, busy = operations.busy || pickerToken != null,
                capabilities = operations.capabilities, items = operations.items, bankRevision = operations.bankRevision,
                detail = operations.detail, preview = operations.preview, result = operations.result, error = operations.error,
                form = operations.form, draft = operations.draft, pending = operations.pending,
                pendingObservation = operations.pendingObservation, exportStatus = operations.exportStatus,
                importedDocument = operations.importedDocument,
                listCurrent = operations.listFresh, draftFingerprint = operations.draftFingerprint,
                exportArtifact = operations.exportArtifact,
                draftNeedsReload = operations.draftNeedsReload,
            ),
            actions = SUMHProfilesActions(
                refresh = { operations.refresh() }, form = operations::updateForm,
                inspect = { operations.inspect(it) }, resources = { operations.setResources(it) },
                chooseImport = {
                    try { importPicker.launch(arrayOf("application/json", "text/plain", "application/octet-stream")) }
                    catch (_: Exception) { operations.reportError("profile_import_picker_failed") }
                },
                previewImport = { operations.previewImport() }, previewSelected = { operations.previewSelected() },
                discardImport = { operations.discardImport() },
                reloadDraft = { operations.reloadDraft() },
                capture = { operations.capture() }, importProfile = { operations.importProfile() },
                clone = { operations.cloneSelected() }, rename = { operations.renameSelected() }, remove = { operations.removeSelected() },
                export = { exportRequested = operations.prepareExport() },
                retryExport = { exportRequested = operations.retryExport() },
                observe = { operations.observePending() }, acknowledge = { operations.acknowledgePending() },
            ), modifier = Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding),
        )
    }
}

internal data class SUMHProfilesUiState(
    val loaded: Boolean = false,
    val busy: Boolean = false,
    val capabilities: SUMHProfileCapabilities = SUMHProfileCapabilities(),
    val items: List<SUMHProfileItem> = emptyList(),
    val bankRevision: String? = null,
    val detail: SUMHProfileResult? = null,
    val preview: SUMHProfileResult? = null,
    val result: SUMHProfileResult? = null,
    val error: String? = null,
    val form: SUMHProfileForm = SUMHProfileForm(),
    val draft: SUMHProfileDraft = SUMHProfileDraft(),
    val pending: SUMHProfilePending? = null,
    val pendingObservation: SUMHProfileResult? = null,
    val exportStatus: String = "idle",
    val importedDocument: SUMHProfileDocument? = null,
    val listCurrent: Boolean = false,
    val draftFingerprint: String? = null,
    val exportArtifact: SUMHProfileExport? = null,
    val draftNeedsReload: Boolean = false,
)

internal data class SUMHProfilesActions(
    val refresh: () -> Unit = {}, val form: (SUMHProfileForm) -> Unit = {},
    val inspect: (SUMHProfileItem) -> Unit = {}, val resources: (Set<SUMHProfileResource>) -> Unit = {},
    val chooseImport: () -> Unit = {}, val previewImport: () -> Unit = {}, val previewSelected: () -> Unit = {},
    val capture: () -> Unit = {}, val importProfile: () -> Unit = {}, val clone: () -> Unit = {},
    val rename: () -> Unit = {}, val remove: () -> Unit = {}, val export: () -> Unit = {},
    val observe: () -> Unit = {}, val acknowledge: () -> Unit = {},
    val discardImport: () -> Unit = {},
    val retryExport: () -> Unit = {},
    val reloadDraft: () -> Unit = {},
)

private data class ProfileUiConfirmation(
    val action: String, val bank: String?, val id: String?, val revision: String?, val draftFingerprint: String?,
    val form: SUMHProfileForm,
)

/** Stateless protocol consumer: rendering never loads, saves, activates or retries a profile. */
@Composable
internal fun SUMHProfilesContent(state: SUMHProfilesUiState, actions: SUMHProfilesActions, modifier: Modifier = Modifier) {
    var confirmation by remember { mutableStateOf<ProfileUiConfirmation?>(null) }
    var replaceImport by remember { mutableStateOf(false) }
    var discardImport by remember { mutableStateOf(false) }
    var reloadDraft by remember { mutableStateOf(false) }
    var expanded by remember { mutableStateOf(emptySet<String>()) }
    fun toggle(key: String) { expanded = if (key in expanded) expanded - key else expanded + key }
    val ready = state.loaded && !state.busy
    val draftReady = ready && !state.draftNeedsReload
    val writable = ready && state.pending == null && state.listCurrent && state.bankRevision != null && state.error == null
    val selected = state.detail?.takeIf { it.complete && it.document != null }
    val selectedKey = selected?.let { "${it.id}:${it.profileRevision}" }
    fun allowed(action: String) = ready && state.capabilities.supports(action)
    fun confirm(action: String) {
        confirmation = ProfileUiConfirmation(action, state.bankRevision, selected?.id, selected?.profileRevision, state.draftFingerprint, state.form)
    }
    LazyColumn(modifier.testTag("profiles_list"), contentPadding = PaddingValues(16.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        item("intro") {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.sumh_profiles_intro))
                Text(stringResource(when {
                    state.busy -> R.string.sumh_profiles_loading
                    !state.listCurrent && state.bankRevision != null -> R.string.sumh_profiles_stale
                    !state.capabilities.known && state.capabilities.error != null -> R.string.sumh_profiles_unknown
                    state.capabilities.known && !state.capabilities.supports("list") -> R.string.sumh_profiles_unsupported
                    state.listCurrent && state.bankRevision != null -> R.string.sumh_profiles_ready
                    else -> R.string.sumh_profiles_idle
                }), Modifier.testTag("profiles_status").semantics { liveRegion = LiveRegionMode.Polite })
                if (state.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
                ProfileButton(R.string.sumh_profiles_load, "profiles_refresh", ready, actions.refresh)
                state.error?.let { Text(stringResource(R.string.sumh_profiles_failure, it), color = MaterialTheme.colorScheme.error,
                    modifier = Modifier.testTag("profiles_error")) }
                when (state.exportStatus) {
                    "saved" -> Text(stringResource(R.string.sumh_profiles_export_saved), Modifier.testTag("profiles_export_status"))
                    "failed" -> Text(stringResource(R.string.sumh_profiles_export_failed), Modifier.testTag("profiles_export_status"))
                }
            }
        }
        state.exportArtifact?.let { artifact -> item("frozen-export") {
            OutlinedCard(Modifier.fillMaxWidth().testTag("profiles_frozen_export")) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    ProfileHeading(R.string.sumh_profiles_frozen_export)
                    Text(stringResource(R.string.sumh_profiles_frozen_export_hint))
                    ProfileValue(stringResource(R.string.sumh_profiles_id), artifact.id)
                    ProfileValue(stringResource(R.string.sumh_profiles_revision), artifact.revision)
                    ProfileButton(R.string.sumh_profiles_retry_export, "profiles_retry_export", ready, actions.retryExport)
                }
            }
        } }
        state.pending?.let { pending -> item("pending") {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                ProfileHeading(R.string.sumh_profiles_pending_title)
                Text(stringResource(R.string.sumh_profiles_pending_scope))
                ProfileValue(stringResource(R.string.sumh_profiles_id), pending.id)
                ProfileButton(R.string.sumh_profiles_observe, "profiles_observe", allowed("list"), actions.observe)
                state.pendingObservation?.let { observation ->
                    ProfileHeading(R.string.sumh_profiles_observation)
                    // The producer/model describes current state; never convert this to saved success.
                    Text(stringResource(when {
                        observation.status == "missing" && observation.error == null -> R.string.sumh_profiles_observed_missing
                        observation.complete -> R.string.sumh_profiles_observed_present
                        else -> R.string.sumh_profiles_result_unknown
                    }), Modifier.testTag("profiles_observation"))
                    observation.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                }
                ProfileButton(R.string.sumh_profiles_acknowledge, "profiles_acknowledge",
                    ready && state.pendingObservation?.error == null && state.pendingObservation?.status in setOf("complete", "missing"), actions.acknowledge)
            }
        } }
        item("library") {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                ProfileHeading(R.string.sumh_profiles_library)
                if (state.bankRevision != null) {
                    Text(stringResource(R.string.sumh_profiles_count, state.items.size))
                    if (state.items.isEmpty() && state.listCurrent) Text(stringResource(R.string.sumh_profiles_empty), Modifier.testTag("profiles_empty"))
                }
            }
        }
        items(state.items, key = { "profile:${it.id}" }) { item ->
            OutlinedCard(Modifier.fillMaxWidth().testTag("profiles_item_${item.id}")) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(item.name, style = MaterialTheme.typography.titleMedium)
                    Text(stringResource(R.string.sumh_profiles_inactive), style = MaterialTheme.typography.labelLarge)
                    ProfileValue(stringResource(R.string.sumh_profiles_id), item.id)
                    if (item.note.isNotEmpty()) Text(item.note)
                    ProfileButton(R.string.sumh_profiles_open, "profiles_open_${item.id}", allowed("show")) { actions.inspect(item) }
                }
            }
        }
        if (selected != null && selectedKey != null) {
            val document = requireNotNull(selected.document)
            item("detail:$selectedKey") {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    ProfileHeading(R.string.sumh_profiles_details)
                    Text(document.name, style = MaterialTheme.typography.titleLarge)
                    if (document.note.isNotEmpty()) Text(document.note)
                    ProfileValue(stringResource(R.string.sumh_profiles_id), selected.id.orEmpty())
                    document.resourceCounts.forEach { (resource, count) -> Text("${stringResource(resource.profileLabel())}: $count") }
                    ProfileButton(R.string.sumh_profiles_export, "profiles_export", allowed("export"), actions.export)
                    Text(stringResource(R.string.sumh_profiles_export_hint), style = MaterialTheme.typography.bodySmall)
                    ProfileButton(R.string.sumh_profiles_clone, "profiles_clone", writable && state.capabilities.supports("clone")) {
                        actions.form(SUMHProfileForm(newProfileUiId(), document.name, document.note)); confirm("clone")
                    }
                    ProfileButton(R.string.sumh_profiles_rename, "profiles_rename", writable && state.capabilities.supports("rename")) {
                        actions.form(SUMHProfileForm(selected.id.orEmpty(), document.name, document.note)); confirm("rename")
                    }
                    ProfileButton(R.string.sumh_profiles_remove, "profiles_remove", writable && state.capabilities.supports("remove")) { confirm("remove") }
                    ProfileButton(if (selectedKey in expanded) R.string.sumh_profiles_hide_details else R.string.sumh_profiles_show_details,
                        "profiles_document_toggle", true) { toggle(selectedKey) }
                }
            }
            if (selectedKey in expanded) profileTextChunks("document:$selectedKey", document.text)
        }
        item("resources") {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                ProfileHeading(R.string.sumh_profiles_compare_title)
                Text(stringResource(R.string.sumh_profiles_compare_scope))
                SUMHProfileResource.entries.forEach { resource ->
                    Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).toggleable(
                        value = resource in state.draft.resources, enabled = draftReady, role = Role.Checkbox,
                        onValueChange = { checked -> actions.resources(if (checked) state.draft.resources + resource else state.draft.resources - resource) },
                    ).testTag("profiles_resource_${resource.wireName}"), verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                        Checkbox(checked = resource in state.draft.resources, onCheckedChange = null)
                        Text(stringResource(resource.profileLabel()), Modifier.weight(1f).padding(start = 8.dp))
                    }
                }
                if (state.draft.resources.isEmpty()) Text(stringResource(R.string.sumh_profiles_select_resource))
                ProfileButton(R.string.sumh_profiles_compare, "profiles_compare", allowed("preview") && selected != null && state.draft.resources.isNotEmpty(), actions.previewSelected)
            }
        }
        state.preview?.diff?.let { diff ->
            item("diff") {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    ProfileHeading(R.string.sumh_profiles_diff_title)
                    Text(stringResource(R.string.sumh_profiles_diff_scope), Modifier.testTag("profiles_diff_scope"))
                }
            }
            diff.resources.forEach { resource ->
                val root = "diff:${resource.resource.wireName}"
                item("$root:summary") {
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(stringResource(resource.resource.profileLabel()), style = MaterialTheme.typography.titleMedium, modifier = Modifier.semantics { heading() })
                        Text("${stringResource(R.string.sumh_profiles_added)} ${resource.added} · ${stringResource(R.string.sumh_profiles_removed)} ${resource.removed} · ${stringResource(R.string.sumh_profiles_changed)} ${resource.changed}")
                        Text(stringResource(R.string.sumh_profiles_unchanged, resource.unchanged))
                    }
                }
                resource.identityConflicts.forEachIndexed { index, conflict ->
                    val key = "$root:conflict:$index"
                    item(key) { Text(stringResource(R.string.sumh_profiles_conflicts), color = MaterialTheme.colorScheme.error, modifier = Modifier.testTag("profiles_conflict_${resource.resource.wireName}_$index")) }
                    profileTextChunks(key, conflict)
                }
                (resource.enabledChanges.map { "enabled" to it } + resource.changes.map { "entry" to it }).forEachIndexed { index, (type, change) ->
                    val key = "$root:$type:$index:${change.key}"
                    item(key) {
                        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            SelectionContainer { Text(change.key, fontFamily = FontFamily.Monospace) }
                            Text(stringResource(when (change.kind) {
                                "add" -> R.string.sumh_profiles_added
                                "remove" -> R.string.sumh_profiles_removed
                                else -> R.string.sumh_profiles_changed
                            }))
                            ProfileButton(if (key in expanded) R.string.sumh_profiles_collapse else R.string.sumh_profiles_expand,
                                "profiles_change_${resource.resource.wireName}_${type}_$index", true) { toggle(key) }
                        }
                    }
                    if (key in expanded) {
                        change.before?.let { value ->
                            item("$key:before") { Text(stringResource(R.string.sumh_profiles_before), style = MaterialTheme.typography.labelLarge) }
                            profileTextChunks("$key:before", value)
                        }
                        change.after?.let { value ->
                            item("$key:after") { Text(stringResource(R.string.sumh_profiles_after), style = MaterialTheme.typography.labelLarge) }
                            profileTextChunks("$key:after", value)
                        }
                    }
                }
            }
        }
        item("capture") {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                ProfileHeading(R.string.sumh_profiles_capture_title)
                Text(stringResource(R.string.sumh_profiles_capture_scope))
                ProfileForm(state.form, ready && state.pending == null, actions.form, "profiles_form")
                ProfileButton(R.string.sumh_profiles_new_id, "profiles_new_id", ready && state.pending == null) { actions.form(state.form.copy(id = newProfileUiId())) }
                ProfileButton(R.string.sumh_profiles_capture, "profiles_capture", writable && state.capabilities.supports("capture") && state.form.error == null) { confirm("capture") }
            }
        }
        item("import") {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                ProfileHeading(R.string.sumh_profiles_import_title)
                Text(stringResource(R.string.sumh_profiles_import_hint))
                if (state.draftNeedsReload) {
                    Text(stringResource(R.string.sumh_profiles_draft_reload_hint), color = MaterialTheme.colorScheme.error)
                    ProfileButton(R.string.sumh_profiles_reload_draft, "profiles_reload_draft", ready) { reloadDraft = true }
                }
                ProfileButton(R.string.sumh_profiles_choose_file, "profiles_import_file", draftReady && state.pending == null) {
                    if (state.draft.document.isEmpty()) actions.chooseImport() else replaceImport = true
                }
                if (state.draft.document.isNotEmpty()) {
                    Text(stringResource(R.string.sumh_profiles_import_ready))
                    Text(stringResource(R.string.sumh_profiles_import_metadata))
                    state.importedDocument?.let { imported ->
                        Text(imported.name, style = MaterialTheme.typography.titleMedium)
                        if (imported.note.isNotEmpty()) Text(imported.note)
                        imported.resourceCounts.forEach { (resource, count) -> Text("${stringResource(resource.profileLabel())}: $count") }
                    }
                    ProfileValue(stringResource(R.string.sumh_profiles_id), state.form.id)
                    ProfileButton(R.string.sumh_profiles_import_preview, "profiles_import_preview", draftReady && allowed("import-preview") && state.draft.resources.isNotEmpty(), actions.previewImport)
                    ProfileButton(R.string.sumh_profiles_import, "profiles_import", writable && draftReady && state.capabilities.supports("import") && state.draftFingerprint != null && validProfileUiId(state.form.id) && state.preview?.action == "import-preview" && state.preview.complete && state.preview.diff != null) { confirm("import") }
                    ProfileButton(if ("import" in expanded) R.string.sumh_profiles_collapse else R.string.sumh_profiles_expand,
                        "profiles_import_toggle", true) { toggle("import") }
                    ProfileButton(R.string.sumh_profiles_discard_import, "profiles_import_discard", draftReady && state.pending == null) { discardImport = true }
                }
            }
        }
        if ("import" in expanded && state.draft.document.isNotEmpty()) profileTextChunks("import", state.draft.document)
        state.result?.let { result -> item("result") {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                ProfileHeading(R.string.sumh_profiles_result)
                Text(stringResource(R.string.sumh_profiles_status_detail, result.status, result.reason), Modifier.testTag("profiles_result"))
                if (result.unknown && result.action in SUMH_PROFILE_MUTATIONS) Text(stringResource(R.string.sumh_profiles_result_unknown))
                else if (result.saved) Text(stringResource(R.string.sumh_profiles_saved))
                result.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
            }
        } }
    }
    confirmation?.let { frozen ->
        val same = frozen.bank == state.bankRevision && frozen.id == selected?.id && frozen.revision == selected?.profileRevision &&
            (frozen.action != "import" || frozen.draftFingerprint != null && frozen.draftFingerprint == state.draftFingerprint) &&
            (frozen.action !in setOf("capture", "import") || frozen.form == state.form)
        val canSubmit = writable && same && state.capabilities.supports(frozen.action) &&
            (frozen.action != "import" || draftReady) &&
            (frozen.action == "remove" || frozen.action == "import" && validProfileUiId(state.form.id) || state.form.error == null)
        YukiAlertDialog(onDismissRequest = { confirmation = null },
            // A fixed available width prevents platform exploratory measurements
            // from oscillating around the scrollable text-field form.
            properties = DialogProperties(usePlatformDefaultWidth = false),
            modifier = Modifier.padding(horizontal = 24.dp).widthIn(max = 560.dp).fillMaxWidth().testTag("profiles_confirmation"), title = {
            Text(stringResource(when (frozen.action) {
                "capture" -> R.string.sumh_profiles_confirm_capture
                "import" -> R.string.sumh_profiles_confirm_import
                "clone" -> R.string.sumh_profiles_confirm_clone
                "rename" -> R.string.sumh_profiles_confirm_rename
                else -> R.string.sumh_profiles_confirm_remove
            }))
        }, text = {
            // A short form has no lazy content. Fixed-width scrolling also avoids
            // competing text-field/viewport measures inside the dialog.
            Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(stringResource(if (frozen.action == "remove") R.string.sumh_profiles_remove_scope else R.string.sumh_profiles_save_scope))
                ProfileValue(stringResource(R.string.sumh_profiles_id), if (frozen.action in setOf("remove", "rename")) frozen.id.orEmpty() else state.form.id)
                if (frozen.action in setOf("clone", "rename")) ProfileForm(state.form, ready, actions.form, "profiles_dialog_form", editId = frozen.action == "clone")
                else if (frozen.action == "capture") { Text(state.form.name); if (state.form.note.isNotEmpty()) Text(state.form.note) }
                if (!same) Text(stringResource(R.string.sumh_profiles_preview_stale), color = MaterialTheme.colorScheme.error)
            }
        }, confirmButton = {
            TextButton(onClick = {
                if (canSubmit) {
                    confirmation = null
                    when (frozen.action) {
                        "capture" -> actions.capture()
                        "import" -> actions.importProfile()
                        "clone" -> actions.clone()
                        "rename" -> actions.rename()
                        "remove" -> actions.remove()
                    }
                }
            }, enabled = canSubmit, modifier = Modifier.heightIn(min = 48.dp).testTag("profiles_confirm_${frozen.action}")) { Text(stringResource(R.string.sumh_profiles_confirm)) }
        }, dismissButton = {
            TextButton(onClick = { confirmation = null }, modifier = Modifier.heightIn(min = 48.dp).testTag("profiles_confirm_cancel")) { Text(stringResource(R.string.sumh_profiles_cancel)) }
        })
    }
    if (replaceImport) YukiAlertDialog(onDismissRequest = { replaceImport = false }, title = { Text(stringResource(R.string.sumh_profiles_import_replace_title)) },
        text = { Text(stringResource(R.string.sumh_profiles_import_replace_body)) },
        confirmButton = { TextButton(onClick = { replaceImport = false; actions.chooseImport() }, enabled = draftReady, modifier = Modifier.testTag("profiles_confirm_replace_import")) { Text(stringResource(R.string.sumh_profiles_choose_file)) } },
        dismissButton = { TextButton(onClick = { replaceImport = false }) { Text(stringResource(R.string.sumh_profiles_cancel)) } })
    if (discardImport) YukiAlertDialog(onDismissRequest = { discardImport = false }, title = { Text(stringResource(R.string.sumh_profiles_discard_import)) },
        text = { Text(stringResource(R.string.sumh_profiles_discard_scope)) },
        confirmButton = { TextButton(onClick = { discardImport = false; actions.discardImport() }, enabled = draftReady && state.pending == null,
            modifier = Modifier.testTag("profiles_confirm_discard_import")) { Text(stringResource(R.string.sumh_profiles_confirm)) } },
        dismissButton = { TextButton(onClick = { discardImport = false }) { Text(stringResource(R.string.sumh_profiles_cancel)) } })
    if (reloadDraft) YukiAlertDialog(onDismissRequest = { reloadDraft = false }, title = { Text(stringResource(R.string.sumh_profiles_reload_draft)) },
        text = { Text(stringResource(R.string.sumh_profiles_draft_reload_scope)) },
        confirmButton = { TextButton(onClick = { reloadDraft = false; actions.reloadDraft() }, enabled = ready,
            modifier = Modifier.heightIn(min = 48.dp).testTag("profiles_confirm_reload_draft")) { Text(stringResource(R.string.sumh_profiles_confirm)) } },
        dismissButton = { TextButton(onClick = { reloadDraft = false }, modifier = Modifier.heightIn(min = 48.dp).testTag("profiles_cancel_reload_draft")) { Text(stringResource(R.string.sumh_profiles_cancel)) } })
}

@Composable private fun ProfileHeading(label: Int) {
    Text(stringResource(label), style = MaterialTheme.typography.titleMedium, modifier = Modifier.semantics { heading() })
}

@Composable private fun ProfileValue(label: String, value: String) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(label, style = MaterialTheme.typography.labelLarge)
        SelectionContainer { Text(value, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodyMedium) }
    }
}

@Composable private fun ProfileButton(label: Int, tag: String, enabled: Boolean, onClick: () -> Unit) {
    OutlinedButton(onClick = onClick, enabled = enabled, modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag(tag)) { Text(stringResource(label)) }
}

@Composable private fun ProfileForm(form: SUMHProfileForm, enabled: Boolean, onChange: (SUMHProfileForm) -> Unit, tag: String, editId: Boolean = true) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        if (editId) OutlinedTextField(form.id, { if (it.length <= 64) onChange(form.copy(id = it)) }, enabled = enabled, singleLine = true,
            label = { Text(stringResource(R.string.sumh_profiles_id)) }, supportingText = { Text(stringResource(R.string.sumh_profiles_id_hint)) }, modifier = Modifier.fillMaxWidth().testTag("${tag}_id"))
        OutlinedTextField(form.name, { if (it.length <= 128) onChange(form.copy(name = it)) }, enabled = enabled,
            label = { Text(stringResource(R.string.sumh_profiles_name)) }, supportingText = { Text(stringResource(R.string.sumh_profiles_name_hint)) }, modifier = Modifier.fillMaxWidth().testTag("${tag}_name"))
        OutlinedTextField(form.note, { if (it.length <= 1024) onChange(form.copy(note = it)) }, enabled = enabled,
            label = { Text(stringResource(R.string.sumh_profiles_note)) }, supportingText = { Text(stringResource(R.string.sumh_profiles_note_hint)) }, modifier = Modifier.fillMaxWidth().testTag("${tag}_note"))
        form.error?.let { Text(stringResource(R.string.sumh_profiles_form_invalid, it), color = MaterialTheme.colorScheme.error) }
    }
}

private fun SUMHProfileResource.profileLabel() = when (this) {
    SUMHProfileResource.Settings -> R.string.sumh_profiles_settings
    SUMHProfileResource.Modules -> R.string.sumh_profiles_modules
    SUMHProfileResource.UserHide -> R.string.sumh_profiles_user_hide
    SUMHProfileResource.Maps -> R.string.sumh_profiles_maps
}

private fun newProfileUiId() = "profile-" + UUID.randomUUID().toString().replace("-", "")
private fun validProfileUiId(id: String) = id.matches(Regex("[a-z0-9][a-z0-9_-]{0,63}"))

/** Each lazy item creates one small layout; chunks preserve surrogate pairs and all source characters. */
private fun LazyListScope.profileTextChunks(key: String, text: String) {
    val size = 2048
    items((text.length + size - 1) / size, key = { "$key:text:$it" }) { index ->
        var start = index * size
        var end = minOf(text.length, start + size)
        if (start > 0 && text[start].isLowSurrogate() && text[start - 1].isHighSurrogate()) start--
        if (end < text.length && text[end].isLowSurrogate() && text[end - 1].isHighSurrogate()) end--
        SelectionContainer { Text(text.substring(start, end), fontFamily = FontFamily.Monospace,
            modifier = Modifier.fillMaxWidth().testTag("profiles_text_${key}_$index"), style = MaterialTheme.typography.bodySmall) }
    }
}
