package ui.screen.moreSettings.component

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Image
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.zying.zysu.BuildConfig
import com.zying.zysu.R
import com.zying.zysu.ui.component.LocalHomeCardArtworkPreview
import com.zying.zysu.ui.screen.home.*
import com.zying.zysu.ui.theme.ArtworkImportResult
import com.zying.zysu.ui.theme.HomeCardArtworkStore
import com.zying.zysu.ui.theme.rememberHomeCardArtwork
import kotlinx.coroutines.launch
import kotlin.math.roundToInt

@Composable
internal fun HomeCardArtworkSetting() {
    val context = LocalContext.current.applicationContext
    val artwork = rememberHomeCardArtwork()
    val scope = rememberCoroutineScope()
    var expanded by rememberSaveable { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var error by rememberSaveable { mutableStateOf<Int?>(null) }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null && !busy) scope.launch {
            busy = true
            error = null
            try {
                error = when (HomeCardArtworkStore.import(context, uri)) {
                    ArtworkImportResult.Success -> null
                    ArtworkImportResult.TooLarge -> R.string.home_card_artwork_too_large
                    ArtworkImportResult.InvalidImage -> R.string.home_card_artwork_invalid
                    ArtworkImportResult.Failed -> R.string.home_card_artwork_error
                }
            } finally { busy = false }
        }
    }
    SettingItem(
        icon = Icons.Outlined.Image, title = stringResource(R.string.home_card_artwork_title),
        subtitle = stringResource(R.string.home_card_artwork_summary), onClick = { expanded = true },
    )
    if (expanded) {
        var scale by remember(artwork.scale) { mutableFloatStateOf(artwork.scale) }
        val scaleTitle = stringResource(R.string.home_card_artwork_scale)
        AlertDialog(
            onDismissRequest = { if (!busy) expanded = false },
            title = { Text(stringResource(R.string.home_card_artwork_title)) },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    CompositionLocalProvider(LocalHomeCardArtworkPreview provides artwork.copy(scale = scale)) {
                        HomeMainStatusCard(
                            style = HomeLayoutStyle.Circle,
                            status = HomeStatusPresentation(
                                title = stringResource(R.string.home_working), installed = true,
                                version = HomeFact("preview", stringResource(R.string.home_layout_kernel_module), homeReleaseLabel(BuildConfig.VERSION_NAME)),
                            ),
                        )
                    }
                    Text(stringResource(R.string.home_card_artwork_help), style = MaterialTheme.typography.bodySmall)
                    if (artwork.file != null) {
                        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            listOf(false to R.string.home_card_artwork_fit, true to R.string.home_card_artwork_crop).forEach { (crop, label) ->
                                FilterChip(selected = artwork.crop == crop, enabled = !busy,
                                    onClick = { HomeCardArtworkStore.setCrop(context, crop) }, label = { Text(stringResource(label)) })
                            }
                        }
                        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                            Text(scaleTitle, Modifier.weight(1f), style = MaterialTheme.typography.titleSmall)
                            Text(stringResource(R.string.appearance_percentage, (scale * 100).roundToInt()), style = MaterialTheme.typography.labelLarge)
                        }
                        Slider(value = scale, onValueChange = { scale = it }, enabled = !busy,
                            valueRange = HomeCardArtworkStore.MIN_SCALE..HomeCardArtworkStore.MAX_SCALE,
                            onValueChangeFinished = { HomeCardArtworkStore.setScale(context, scale) },
                            modifier = Modifier.testTag("home-artwork-scale").semantics { contentDescription = scaleTitle })
                        TextButton(enabled = !busy, onClick = { scale = 1f; HomeCardArtworkStore.setScale(context, 1f) }) {
                            Text(stringResource(R.string.home_card_artwork_reset))
                        }
                    }
                    if (busy) Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                        Text(stringResource(R.string.home_card_artwork_busy))
                    }
                    error?.let { Text(stringResource(it), color = MaterialTheme.colorScheme.error) }
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        FilledTonalButton(enabled = !busy, onClick = { picker.launch(arrayOf("image/*")) }) {
                            Text(stringResource(if (artwork.file == null) R.string.home_card_artwork_import else R.string.home_card_artwork_replace))
                        }
                        if (artwork.file != null) TextButton(enabled = !busy, onClick = {
                            scope.launch {
                                busy = true
                                try { error = if (HomeCardArtworkStore.clear(context)) null else R.string.home_card_artwork_error }
                                finally { busy = false }
                            }
                        }) { Text(stringResource(R.string.home_card_artwork_remove)) }
                    }
                }
            },
            confirmButton = { TextButton(enabled = !busy, onClick = { expanded = false }) { Text(stringResource(R.string.home_card_artwork_close)) } },
        )
    }
}
