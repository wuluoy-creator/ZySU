package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.*
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch
import org.json.JSONObject

@Composable internal fun MapsPreviewControls(enabled: Boolean, rules: List<SUMHUiAdapter.MapsPathRule>, mapsEnabled: Boolean,
    read: suspend () -> SUMHMapsPreviewResult = { SUMHManager.previewMaps() }) {
    var open by rememberSaveable { mutableStateOf(false) }
    val fingerprint = remember(rules, mapsEnabled) { mapsPreviewFingerprint(rules, mapsEnabled) }
    OutlinedButton(onClick = { open = true }, enabled = enabled, modifier = Modifier.testTag("maps_preview_open")) {
        Text(stringResource(R.string.sumh_maps_preview))
    }
    if (open) MapsPreviewDialog(read, fingerprint) { open = false }
}

internal fun mapsPreviewFingerprint(rules: List<SUMHUiAdapter.MapsPathRule>, enabled: Boolean) =
    draftFingerprint(listOf(enabled.toString()) + rules.flatMap { listOf(it.targetPath, it.replacementPath) })

@Composable internal fun MapsPreviewDialog(read: suspend () -> SUMHMapsPreviewResult,
    definitionFingerprint: String = "", onDismiss: () -> Unit) {
    var result by remember { mutableStateOf<SUMHMapsPreviewResult?>(null) }
    var loading by remember { mutableStateOf(false) }
    var readFingerprint by remember { mutableStateOf<String?>(null) }
    var invalidated by remember { mutableStateOf(false) }
    LaunchedEffect(definitionFingerprint) {
        if (readFingerprint != null && readFingerprint != definitionFingerprint) invalidated = true
    }
    val locallyStale = invalidated || readFingerprint != null && readFingerprint != definitionFingerprint
    val scope = rememberCoroutineScope()
    Dialog(onDismissRequest = onDismiss, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Surface(Modifier.fillMaxWidth().fillMaxHeight(.94f).padding(horizontal = 12.dp), shape = MaterialTheme.shapes.extraLarge) {
            Column(Modifier.padding(16.dp)) {
                Text(stringResource(R.string.sumh_maps_preview), style = MaterialTheme.typography.titleLarge)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = {
                        if (!loading) {
                            result = null; loading = true; readFingerprint = definitionFingerprint; invalidated = false
                            scope.launch {
                                try { result = read() }
                                catch (error: Exception) {
                                    if (error is CancellationException) throw error
                                    result = SUMHMapsPreviewResult(error = error.message ?: "preview_failed")
                                } finally { loading = false }
                            }
                        }
                    }, enabled = !loading, modifier = Modifier.testTag("maps_preview_load")) { Text(stringResource(R.string.sumh_maps_preview_read)) }
                    TextButton(onClick = onDismiss) { Text(stringResource(R.string.close)) }
                }
                if (loading) LinearProgressIndicator(Modifier.fillMaxWidth())
                MapsPreviewContent(result, loading, Modifier.weight(1f), locallyStale)
            }
        }
    }
}

@Composable internal fun MapsPreviewContent(result: SUMHMapsPreviewResult?, loading: Boolean, modifier: Modifier = Modifier,
    locallyStale: Boolean = false) {
    var diagnostics by rememberSaveable { mutableStateOf(false) }
    val value = result?.value
    LazyColumn(modifier.fillMaxWidth().testTag("maps_preview_list"), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        item("scope") { Text(stringResource(R.string.sumh_maps_preview_scope), style = MaterialTheme.typography.bodySmall) }
        item("status") {
            val title = when {
                loading -> R.string.sumh_maps_preview_loading
                result == null -> R.string.sumh_maps_preview_not_loaded
                value == null -> R.string.sumh_maps_preview_failed
                locallyStale -> R.string.sumh_maps_preview_local_change
                value.status == SUMHMapsPreviewStatus.Stale -> R.string.sumh_maps_preview_stale
                result.error != null || value.submission.status == "invalid" || value.submission.stable == false -> R.string.sumh_maps_preview_partial
                value.status == SUMHMapsPreviewStatus.Complete -> R.string.sumh_maps_preview_complete
                value.status == SUMHMapsPreviewStatus.Partial -> R.string.sumh_maps_preview_partial
                else -> R.string.sumh_maps_preview_unknown
            }
            Text(stringResource(title), modifier = Modifier.testTag("maps_preview_status"))
        }
        if (value != null) {
            item("saved") {
                Text(stringResource(when (value.savedStatus) {
                    SUMHMapsSavedStatus.Valid -> R.string.sumh_maps_preview_saved_valid
                    SUMHMapsSavedStatus.Missing -> R.string.sumh_maps_preview_saved_missing
                    SUMHMapsSavedStatus.Changed -> R.string.sumh_maps_preview_saved_changed
                    else -> R.string.sumh_maps_preview_saved_unknown
                }))
                if (value.enabled == false) Text(stringResource(R.string.sumh_maps_preview_disabled), style = MaterialTheme.typography.bodySmall)
                Text(stringResource(when (if (locallyStale) SUMHComparison.Unknown else value.sample.comparison) {
                    SUMHComparison.Matches -> R.string.sumh_maps_preview_set_matches
                    SUMHComparison.Drift -> R.string.sumh_maps_preview_set_drift
                    else -> R.string.sumh_maps_preview_set_unknown
                }))
                if (!value.sample.definitionsComplete) Text(stringResource(R.string.sumh_maps_preview_inventory_incomplete))
            }
            item("history") {
                Text(stringResource(if (value.submission.confirmed == null) R.string.sumh_maps_preview_no_submission
                    else R.string.sumh_maps_preview_history_scope))
                if (value.submission.confirmed != null && value.submission.stable != true)
                    Text(stringResource(R.string.sumh_maps_preview_history_unstable))
                if (value.submission.latest?.phase == "pending") Text(stringResource(R.string.sumh_maps_preview_pending))
                if (value.submission.latest?.phase == "rejected") Text(stringResource(R.string.sumh_maps_preview_rejected))
                value.submission.confirmed?.let {
                    if (it.bootId != value.bootId) Text(stringResource(R.string.sumh_maps_preview_history_other_boot))
                    if (it.rows.isEmpty()) Text(stringResource(R.string.sumh_maps_preview_history_clear))
                }
            }
            items(value.rows, key = { it.index }) { row ->
                Column(Modifier.fillMaxWidth().testTag("maps_preview_row_${row.index}"), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    HorizontalDivider()
                    Text(stringResource(R.string.sumh_maps_path_target), style = MaterialTheme.typography.labelLarge)
                    Text(JSONObject.quote(row.targetPath), fontFamily = FontFamily.Monospace)
                    MapsIdentity(row.target)
                    Text(stringResource(R.string.sumh_maps_path_replacement), style = MaterialTheme.typography.labelLarge)
                    Text(JSONObject.quote(row.replacementPath), fontFamily = FontFamily.Monospace)
                    MapsIdentity(row.replacement)
                    if (row.collision) Text(stringResource(R.string.sumh_maps_preview_collision), color = MaterialTheme.colorScheme.error)
                    if (!row.stable) Text(stringResource(R.string.sumh_maps_preview_row_unstable))
                    Text(stringResource(when (if (locallyStale) SUMHMapsRegistration.Unknown else row.registration) {
                        SUMHMapsRegistration.Matches -> R.string.sumh_maps_preview_row_matches
                        SUMHMapsRegistration.Missing -> R.string.sumh_maps_preview_row_missing
                        SUMHMapsRegistration.Different -> R.string.sumh_maps_preview_row_different
                        SUMHMapsRegistration.Unknown -> R.string.sumh_maps_preview_row_unknown
                    }))
                    val submitted = value.submission.confirmed?.rows?.find { it.targetPath == row.targetPath }
                    if (submitted != null) {
                        Text(stringResource(R.string.sumh_maps_preview_submitted_identity), style = MaterialTheme.typography.labelLarge)
                        Text("target dev=${submitted.targetDev} ino=${submitted.targetIno}\nreported dev=${submitted.spoofedDev} ino=${submitted.spoofedIno}", fontFamily = FontFamily.Monospace)
                        Text(JSONObject.quote(submitted.replacementPath), fontFamily = FontFamily.Monospace)
                        if (!locallyStale && row.stable && value.submission.stable == true && value.status != SUMHMapsPreviewStatus.Stale && result.error == null &&
                            row.target.status == SUMHMapsIdentityStatus.Regular && row.replacement.status == SUMHMapsIdentityStatus.Regular) {
                            val same = row.target.ino == submitted.targetIno && row.target.dev == submitted.targetDev &&
                                row.replacement.ino == submitted.spoofedIno && row.replacement.dev == submitted.spoofedDev &&
                                row.replacementPath == submitted.replacementPath
                            Text(stringResource(if (same) R.string.sumh_maps_preview_identity_same else R.string.sumh_maps_preview_identity_changed))
                        }
                    } else Text(stringResource(R.string.sumh_maps_preview_no_row_submission), style = MaterialTheme.typography.bodySmall)
                }
            }
            if (value.rows.isEmpty() && value.savedStatus in setOf(SUMHMapsSavedStatus.Valid, SUMHMapsSavedStatus.Missing)) item("empty") {
                Text(stringResource(R.string.sumh_maps_preview_no_definitions), Modifier.testTag("maps_preview_empty"))
            }
        }
        if (result != null) item("diagnostics") {
            TextButton(onClick = { diagnostics = !diagnostics }) { Text(stringResource(R.string.sumh_state_diagnostics)) }
            if (diagnostics) Text(buildString {
                result.error?.let { append("transport: $it\n") }
                value?.let { sample ->
                    append("reason: ${sample.reason}; errno: ${sample.errno}\nboot: ${sample.bootId}\nrevision: ${sample.configRevision}\n")
                    append("maps revision: ${sample.mapsRevision}\nnamespace: ${sample.sample.namespace}\ninstance: ${sample.sample.instance}\n")
                    append("generation: ${sample.sample.generation}\nsample: ${sample.sample.reason}; errno: ${sample.sample.errno}\n")
                    append("submission: ${sample.submission.status}; ${sample.submission.reason}\n")
                    sample.submission.latest?.let { append("latest: ${it.id}; phase=${it.phase}; errno=${it.errno}\nsupersedes pending: ${it.supersedesPendingId}\n") }
                    sample.submission.confirmed?.let { append("confirmed: ${it.id}; boot=${it.bootId}; revision=${it.definitionRevision}\n") }
                }
            }, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
        }
    }
}

@Composable private fun MapsIdentity(identity: SUMHMapsIdentity) {
    Text(stringResource(when (identity.status) {
        SUMHMapsIdentityStatus.Regular -> R.string.sumh_maps_preview_regular
        SUMHMapsIdentityStatus.Missing -> R.string.sumh_maps_preview_path_missing
        SUMHMapsIdentityStatus.NotRegular -> R.string.sumh_maps_preview_path_type
        SUMHMapsIdentityStatus.Unrepresentable -> R.string.sumh_maps_preview_path_unrepresentable
        SUMHMapsIdentityStatus.Error -> R.string.sumh_maps_preview_path_error
        SUMHMapsIdentityStatus.Unknown -> R.string.sumh_maps_preview_path_unknown
    }))
    if (identity.ino != null && identity.dev != null) Text("dev=${identity.dev} ino=${identity.ino}", fontFamily = FontFamily.Monospace)
    if (identity.errno != 0) Text("errno=${identity.errno}", style = MaterialTheme.typography.bodySmall)
}
