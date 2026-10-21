package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog

@Composable internal fun SUMHEditorDraftStatus(drafts: SUMHEditorDrafts) {
    var managing by remember { mutableStateOf(false) }
    var discard by remember { mutableStateOf<Pair<String, EditorDraftRecord>?>(null) }
    when {
        drafts.storageError -> {
            Text(stringResource(if (drafts.loaded) R.string.sumh_editor_draft_write_failed else R.string.sumh_editor_draft_read_failed),
                color = MaterialTheme.colorScheme.error)
            TextButton(onClick = drafts::retry) { Text(stringResource(R.string.sumh_editor_draft_retry)) }
        }
        !drafts.loaded -> Text(stringResource(R.string.sumh_editor_draft_loading))
        drafts.saving -> Text(stringResource(R.string.sumh_editor_draft_saving), style = MaterialTheme.typography.bodySmall)
    }
    if (drafts.limitReached) {
        Text(stringResource(R.string.sumh_editor_draft_limit), color = MaterialTheme.colorScheme.error)
        TextButton(onClick = { managing = true }) { Text(stringResource(R.string.sumh_editor_draft_manage)) }
    }
    if (managing) YukiAlertDialog(
        onDismissRequest = { managing = false },
        title = { Text(stringResource(R.string.sumh_editor_draft_manage)) },
        text = { Column(Modifier.verticalScroll(rememberScrollState())) {
            drafts.snapshot.forEach { (key, record) ->
                TextButton(onClick = { discard = key to record }, modifier = Modifier.heightIn(min = 48.dp)) { Text(key) }
            }
        } },
        confirmButton = { TextButton(onClick = { managing = false }) { Text(stringResource(android.R.string.ok)) } },
    )
    discard?.let { (key, expected) -> YukiAlertDialog(
        onDismissRequest = { discard = null },
        title = { Text(stringResource(R.string.sumh_editor_draft_discard)) },
        text = { Text(stringResource(R.string.sumh_editor_draft_discard_confirm, key)) },
        confirmButton = { TextButton(onClick = { drafts.forget(key, expected); discard = null }) { Text(stringResource(R.string.delete)) } },
        dismissButton = { TextButton(onClick = { discard = null }) { Text(stringResource(android.R.string.cancel)) } },
    ) }
}
