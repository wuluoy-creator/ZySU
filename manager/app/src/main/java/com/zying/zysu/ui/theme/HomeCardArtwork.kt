package com.zying.zysu.ui.theme

import android.annotation.SuppressLint
import android.content.Context
import android.content.SharedPreferences
import android.graphics.ImageDecoder
import android.net.Uri
import android.util.Log
import androidx.compose.runtime.*
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.edit
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.util.UUID
import kotlin.math.max
import kotlin.math.roundToInt
import kotlin.coroutines.coroutineContext

internal data class HomeCardArtwork(
    val file: File? = null,
    val scale: Float = 1f,
    val crop: Boolean = false,
    val overlayOpacity: Float = HomeCardArtworkStore.DEFAULT_OVERLAY_OPACITY,
)

internal enum class ArtworkImportResult { Success, TooLarge, InvalidImage, Failed }

/** Original image bytes are retained so alpha and orientation are never lost through JPEG conversion. */
internal object HomeCardArtworkStore {
    const val MIN_SCALE = 0.5f
    const val MAX_SCALE = 3f
    const val DEFAULT_OVERLAY_OPACITY = 0.82f
    internal const val MAX_BYTES = 20 * 1024 * 1024
    private val ownedName = Regex("artwork-[a-f0-9-]+\\.img")
    internal fun preferences(context: Context): SharedPreferences =
        context.getSharedPreferences("home_card_artwork", Context.MODE_PRIVATE)

    private fun directory(context: Context) = File(context.filesDir, "home-card-artwork")
    internal fun validScale(value: Float) = if (value.isFinite()) value.coerceIn(MIN_SCALE, MAX_SCALE) else 1f
    internal fun validOverlayOpacity(value: Float) =
        if (value.isFinite()) value.coerceIn(0f, 1f) else DEFAULT_OVERLAY_OPACITY

    fun read(context: Context): HomeCardArtwork {
        val prefs = preferences(context)
        val file = prefs.getString("filename", null)?.takeIf(ownedName::matches)
            ?.let { File(directory(context), it) }?.takeIf { it.isFile }
        return HomeCardArtwork(
            file, validScale(prefs.getFloat("scale", 1f)), prefs.getBoolean("crop", false),
            validOverlayOpacity(prefs.getFloat("overlay_opacity", DEFAULT_OVERLAY_OPACITY)),
        )
    }

    fun setScale(context: Context, scale: Float) {
        preferences(context).edit { putFloat("scale", validScale(scale)) }
    }

    fun setCrop(context: Context, crop: Boolean) {
        preferences(context).edit { putBoolean("crop", crop) }
    }

    fun setOverlayOpacity(context: Context, opacity: Float) {
        preferences(context).edit { putFloat("overlay_opacity", validOverlayOpacity(opacity)) }
    }

    private fun restore(preferences: SharedPreferences, previous: Map<String, *>) {
        preferences.edit(commit = true) {
            clear()
            (previous["filename"] as? String)?.let { putString("filename", it) }
            (previous["scale"] as? Float)?.let { putFloat("scale", it) }
            (previous["crop"] as? Boolean)?.let { putBoolean("crop", it) }
            (previous["overlay_opacity"] as? Float)?.let { putFloat("overlay_opacity", it) }
        }
    }

    // KTX edit returns Unit; atomic publication needs commit's Boolean before deleting an owned file.
    @SuppressLint("UseKtx")
    suspend fun import(context: Context, uri: Uri): ArtworkImportResult = withContext(Dispatchers.IO) {
        val folder = directory(context)
        var pending: File? = null
        var destination: File? = null
        var saved = false
        try {
            if (!folder.isDirectory && !folder.mkdirs()) return@withContext ArtworkImportResult.Failed
            val staged = File.createTempFile("pending-", ".img", folder)
            pending = staged
            context.contentResolver.openInputStream(uri)?.use { input ->
                staged.outputStream().use { output ->
                    val buffer = ByteArray(16 * 1024)
                    var total = 0L
                    while (true) {
                        coroutineContext.ensureActive()
                        val count = input.read(buffer)
                        if (count < 0) break
                        total += count
                        if (total > MAX_BYTES) return@withContext ArtworkImportResult.TooLarge
                        output.write(buffer, 0, count)
                    }
                }
            } ?: return@withContext ArtworkImportResult.Failed
            try {
                // Decode bounded bytes without retaining a platform file descriptor across publication.
                ImageDecoder.decodeBitmap(ImageDecoder.createSource(ByteBuffer.wrap(staged.readBytes()))) { decoder, info, _ ->
                    val width = info.size.width
                    val height = info.size.height
                    require(width > 0 && height > 0 && width <= 32768 && height <= 32768)
                    val ratio = 2048f / max(width, height).coerceAtLeast(2048)
                    decoder.setTargetSize((width * ratio).roundToInt().coerceAtLeast(1), (height * ratio).roundToInt().coerceAtLeast(1))
                    decoder.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
                }.recycle()
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                Log.w("HomeCardArtwork", "Imported image could not be decoded", error)
                return@withContext ArtworkImportResult.InvalidImage
            }
            coroutineContext.ensureActive()
            val oldFile = read(context).file
            val prefs = preferences(context)
            val previous = prefs.all.toMap()
            val imported = File(folder, "artwork-${UUID.randomUUID()}.img")
            destination = imported
            if (!staged.renameTo(imported)) return@withContext ArtworkImportResult.Failed
            if (!prefs.edit().putString("filename", imported.name)
                    .putFloat("scale", 1f).putBoolean("crop", false).commit()) {
                // Failed disk writes still update SharedPreferences' in-memory values.
                restore(prefs, previous)
                return@withContext ArtworkImportResult.Failed
            }
            saved = true
            oldFile?.delete()
            ArtworkImportResult.Success
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (_: IOException) {
            ArtworkImportResult.Failed
        } catch (_: SecurityException) {
            ArtworkImportResult.Failed
        } catch (_: Exception) {
            ArtworkImportResult.Failed
        } finally {
            pending?.delete()
            if (!saved) destination?.delete()
        }
    }

    @SuppressLint("UseKtx") // Keep the file unless the metadata removal was persisted successfully.
    suspend fun clear(context: Context): Boolean = withContext(Dispatchers.IO) {
        val oldFile = read(context).file
        val prefs = preferences(context)
        val previous = prefs.all.toMap()
        if (!prefs.edit().clear().commit()) {
            restore(prefs, previous)
            return@withContext false
        }
        oldFile?.delete()
        true
    }
}

@Composable
internal fun rememberHomeCardArtwork(): HomeCardArtwork {
    val context = LocalContext.current.applicationContext
    val prefs = remember(context) { HomeCardArtworkStore.preferences(context) }
    var artwork by remember(prefs) { mutableStateOf(HomeCardArtworkStore.read(context)) }
    DisposableEffect(prefs) {
        val listener = SharedPreferences.OnSharedPreferenceChangeListener { _, _ -> artwork = HomeCardArtworkStore.read(context) }
        prefs.registerOnSharedPreferenceChangeListener(listener)
        artwork = HomeCardArtworkStore.read(context)
        onDispose { prefs.unregisterOnSharedPreferenceChangeListener(listener) }
    }
    return artwork
}
