package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.listSaver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.zying.zysu.R

/** A saved-value observation is evidence about configuration, not runtime apply success. */
internal class SUMHRefreshAwareDraft(initial: String, baseline: String = fingerprint(initial), pending: String? = null) {
    var text by mutableStateOf(initial); private set
    var baseline by mutableStateOf(baseline); private set
    var pending by mutableStateOf(pending); private set

    fun edit(value: String): Boolean {
        if (!sumhSmallDraftFits(value)) return false
        text = value
        return true
    }

    fun observe(saved: String) {
        val savedKey = fingerprint(saved)
        val draftKey = fingerprint(text)
        when {
            // A confirmed older submission advances the baseline, preserving a newer edit.
            pending != null && pending == savedKey -> { baseline = savedKey; pending = null }
            savedKey == baseline -> Unit
            draftKey == baseline -> { text = saved; baseline = savedKey; pending = null }
            draftKey == savedKey -> { baseline = savedKey; pending = null }
            // The dirty value and its original baseline both survive an external change.
            else -> Unit
        }
    }

    fun conflict(saved: String): Boolean = fingerprint(saved) != baseline &&
        fingerprint(text) != baseline && fingerprint(text) != fingerprint(saved) && pending != fingerprint(saved)

    fun useSaved(saved: String) { text = saved; baseline = fingerprint(saved); pending = null }
    fun keepDraft(saved: String) { baseline = fingerprint(saved); pending = null }
    fun submitted() { pending = fingerprint(text) }

    companion object { fun fingerprint(value: String) = draftFingerprint(listOf(value)) }
}

@Composable internal fun rememberSUMHRefreshAwareDraft(saved: String): SUMHRefreshAwareDraft {
    val draft = rememberSaveable(saver = listSaver(
        save = { value: SUMHRefreshAwareDraft ->
            // Only typed/size-bounded text enters SavedState. An untouched oversized
            // legacy value comes from the saved configuration again after recreation.
            if (sumhSmallDraftFits(value.text)) listOf("text", value.text, value.baseline, value.pending.orEmpty())
            else listOf("saved")
        },
        restore = { values ->
            if (values.size == 4 && values[0] == "text" && sumhSmallDraftFits(values[1]))
                SUMHRefreshAwareDraft(values[1], values[2], values[3].ifEmpty { null })
            else SUMHRefreshAwareDraft(saved)
        },
    )) { SUMHRefreshAwareDraft(saved) }
    SideEffect { draft.observe(saved) }
    return draft
}

/** Field-local decisions never discard another field's unfinished input. */
@Composable internal fun SUMHDraftConflict(draft: SUMHRefreshAwareDraft, saved: String, label: String, modifier: Modifier = Modifier) {
    if (!draft.conflict(saved)) return
    Column(modifier, verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(stringResource(R.string.sumh_field_draft_conflict, label), color = MaterialTheme.colorScheme.error,
            style = MaterialTheme.typography.bodySmall)
        Text(stringResource(R.string.sumh_field_draft_saved_value, saved), style = MaterialTheme.typography.bodySmall)
        TextButton(onClick = { draft.useSaved(saved) }, modifier = Modifier.heightIn(min = 48.dp)) {
            Text(stringResource(R.string.sumh_field_draft_use_saved, label))
        }
        TextButton(onClick = { draft.keepDraft(saved) }, modifier = Modifier.heightIn(min = 48.dp)) {
            Text(stringResource(R.string.sumh_field_draft_keep, label))
        }
    }
}
