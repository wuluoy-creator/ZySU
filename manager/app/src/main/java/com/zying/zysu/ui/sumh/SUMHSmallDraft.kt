package com.zying.zysu.ui.sumh

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.saveable.Saver
import androidx.compose.ui.res.stringResource
import com.zying.zysu.R

/** Bounded singleton fields can remain in SavedState; never shorten input to fit. */
internal fun sumhSmallDraftFits(value: String): Boolean =
    value.length <= EDITOR_DRAFT_FIELD_BYTES && value.toByteArray(Charsets.UTF_8).size <= EDITOR_DRAFT_FIELD_BYTES

// An oversized legacy saved value remains visible and can be replaced, but is
// reloaded from its configuration instead of copied into Activity SavedState.
internal val SUMHSmallTextSaver = Saver<String, String>(save = { it.takeIf(::sumhSmallDraftFits) }, restore = { it })

@Composable internal fun SUMHSmallDraftLimit() {
    Text(stringResource(R.string.sumh_small_draft_limit), color = MaterialTheme.colorScheme.error,
        style = MaterialTheme.typography.bodySmall)
}
