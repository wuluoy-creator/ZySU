package ui.screen.moreSettings.component

import android.util.Log
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.theme.AppearancePreferences
import com.zying.zysu.ui.theme.CustomFontException
import com.zying.zysu.ui.theme.CustomFontProblem
import com.zying.zysu.ui.theme.CustomFontStore
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** The system picker grants access only long enough to copy into app-private font storage. */
@Composable
internal fun CustomFontSetting() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var importing by remember { mutableStateOf(false) }
    var importProblem by remember { mutableStateOf<CustomFontProblem?>(null) }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null && !importing) scope.launch {
            importing = true
            importProblem = null
            try {
                withContext(Dispatchers.IO) {
                    val importContext = currentCoroutineContext()
                    val font = CustomFontStore.import(context, uri) { importContext.ensureActive() }
                    // Once storage commits, always synchronize the app-wide appearance, including rotation.
                    withContext(NonCancellable + Dispatchers.Main.immediate) {
                        AppearancePreferences.applyCustomFont(font)
                    }
                }
            } catch (error: Exception) {
                if (error is CancellationException) throw error
                Log.w("CustomFontSetting", "Font import failed", error)
                importProblem = (error as? CustomFontException)?.problem ?: CustomFontProblem.READ_FAILED
            } finally {
                importing = false
            }
        }
    }
    val name = AppearancePreferences.customFontName
    val valid = AppearancePreferences.customFontFamily != null
    Column(Modifier.fillMaxWidth().testTag("custom-font-setting"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(stringResource(R.string.custom_font_title), style = MaterialTheme.typography.titleSmall)
        Text(stringResource(R.string.custom_font_summary), style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        if (name != null) {
            Text(name, style = MaterialTheme.typography.bodyMedium)
            if (valid) FilterChip(
                selected = AppearancePreferences.fontFamily == "custom", enabled = !importing,
                onClick = { AppearancePreferences.setFontFamily(context, "custom") },
                modifier = Modifier.testTag("custom-font-select"),
                label = { Text(stringResource(R.string.custom_font_use)) },
            )
        }
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            OutlinedButton(onClick = { picker.launch(arrayOf("*/*")) }, enabled = !importing,
                modifier = Modifier.testTag("custom-font-import")) {
                Text(stringResource(if (name == null) R.string.custom_font_import else R.string.custom_font_replace))
            }
            if (AppearancePreferences.fontFamily == "custom") TextButton(
                onClick = { AppearancePreferences.setFontFamily(context, "system") }, enabled = !importing,
                modifier = Modifier.testTag("custom-font-revert"),
            ) { Text(stringResource(R.string.custom_font_revert)) }
            if (name != null) TextButton(
                onClick = {
                    try {
                        AppearancePreferences.removeCustomFont(context)
                        importProblem = null
                    } catch (_: CustomFontException) { importProblem = CustomFontProblem.READ_FAILED }
                }, enabled = !importing, modifier = Modifier.testTag("custom-font-remove"),
            ) { Text(stringResource(R.string.custom_font_remove)) }
        }
        if (importing) {
            LinearProgressIndicator(Modifier.fillMaxWidth())
            Text(stringResource(R.string.custom_font_importing), style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite })
        }
        (importProblem ?: AppearancePreferences.customFontProblem)?.let { problem ->
            Text(stringResource(when (problem) {
                CustomFontProblem.TOO_LARGE -> R.string.custom_font_too_large
                CustomFontProblem.INVALID -> R.string.custom_font_invalid
                CustomFontProblem.READ_FAILED -> R.string.custom_font_read_failed
                CustomFontProblem.MISSING, CustomFontProblem.CORRUPT -> R.string.custom_font_missing
            }), color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.testTag("custom-font-error").semantics { liveRegion = LiveRegionMode.Polite })
        }
    }
}
