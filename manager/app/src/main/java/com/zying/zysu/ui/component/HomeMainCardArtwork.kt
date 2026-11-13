package com.zying.zysu.ui.component

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import coil.compose.AsyncImage
import coil.request.ImageRequest
import com.zying.zysu.ui.theme.HomeCardArtwork
import com.zying.zysu.ui.theme.rememberHomeCardArtwork

internal val LocalHomeCardArtworkPreview = compositionLocalOf<HomeCardArtwork?> { null }

/** Decorative, non-interactive layer; the status card keeps its installation and detail actions. */
@Composable
internal fun HomeMainCardArtwork(modifier: Modifier = Modifier) {
    val stored = rememberHomeCardArtwork()
    HomeCardArtworkLayer(LocalHomeCardArtworkPreview.current ?: stored, modifier)
}

@Composable
internal fun HomeCardArtworkLayer(artwork: HomeCardArtwork, modifier: Modifier = Modifier) {
    val file = artwork.file ?: return
    val context = LocalContext.current
    val request = remember(file, context) {
        ImageRequest.Builder(context).data(file).size(2048).allowHardware(false).crossfade(false).build()
    }
    var loaded by remember(file) { mutableStateOf(false) }
    Box(modifier.clipToBounds().testTag("home-card-artwork")) {
        AsyncImage(
            model = request, contentDescription = null,
            contentScale = if (artwork.crop) ContentScale.Crop else ContentScale.Fit,
            onSuccess = { loaded = true }, onError = { loaded = false },
            modifier = Modifier.fillMaxSize().graphicsLayer { scaleX = artwork.scale; scaleY = artwork.scale }
                .then(if (loaded) Modifier.testTag("home-card-artwork-loaded") else Modifier),
        )
        // Keep the same foreground contrast for photographs, transparent cutouts and either theme.
        Box(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.surfaceContainerLow.copy(alpha = 0.82f)))
    }
}
