package com.zying.zysu.ui.theme

import android.annotation.SuppressLint
import android.content.Context
import android.content.SharedPreferences
import android.graphics.Typeface
import android.graphics.fonts.Font
import android.graphics.fonts.FontFamily as AndroidFontFamily
import android.net.Uri
import android.provider.OpenableColumns
import androidx.core.content.edit
import kotlinx.coroutines.CancellationException
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.InputStream
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.security.MessageDigest
import java.util.UUID

internal enum class CustomFontProblem { TOO_LARGE, INVALID, READ_FAILED, MISSING, CORRUPT }
internal class CustomFontException(val problem: CustomFontProblem, cause: Throwable? = null) : Exception(problem.name, cause)
internal data class CustomFontState(val name: String? = null, val typeface: Typeface? = null,
    val problem: CustomFontProblem? = null)

/** Owned immutable files make replacement atomic: publish metadata only after a validated copy. */
internal object CustomFontStore {
    const val MAX_BYTES = 32 * 1024 * 1024
    private const val FILE_KEY = "custom_font_file"
    private const val NAME_KEY = "custom_font_name"
    private const val HASH_KEY = "custom_font_sha256"
    private val ownedName = Regex("font-[a-f0-9]{32}\\.font")
    private fun prefs(context: Context) = context.getSharedPreferences("appearance", Context.MODE_PRIVATE)
    private fun directory(context: Context) = File(context.filesDir, "custom-font")
    private fun ownedFile(context: Context, name: String?): File? =
        name?.takeIf { ownedName.matches(it) }?.let { File(directory(context), it) }

    fun import(context: Context, uri: Uri, ensureActive: () -> Unit = {}): CustomFontState {
        ensureActive()
        val name = runCatching {
            context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) cursor.getString(0) else null
            }
        }.getOrNull() ?: uri.lastPathSegment.orEmpty().substringAfterLast('/')
        val input = try { context.contentResolver.openInputStream(uri) } catch (_: Exception) { null }
            ?: throw CustomFontException(CustomFontProblem.READ_FAILED)
        return import(context, name, input, ensureActive)
    }

    // KTX edit returns Unit; publication must inspect commit's Boolean before deleting the old font.
    @SuppressLint("UseKtx")
    internal fun import(context: Context, displayName: String, input: InputStream,
        ensureActive: () -> Unit = {}): CustomFontState {
        val folder = directory(context)
        if (!folder.isDirectory && !folder.mkdirs()) {
            runCatching { input.close() }
            throw CustomFontException(CustomFontProblem.READ_FAILED)
        }
        val staged = try { File.createTempFile("pending-", ".font", folder) } catch (_: Exception) {
            runCatching { input.close() }
            throw CustomFontException(CustomFontProblem.READ_FAILED)
        }
        var published: File? = null
        try {
            input.use { source ->
                FileOutputStream(staged).use { output ->
                    val buffer = ByteArray(16 * 1024)
                    var total = 0L
                    while (true) {
                        ensureActive()
                        val read = source.read(buffer)
                        if (read < 0) break
                        if (read == 0) continue
                        total += read
                        if (total > MAX_BYTES) throw CustomFontException(CustomFontProblem.TOO_LARGE)
                        output.write(buffer, 0, read)
                    }
                    output.fd.sync()
                }
            }
            ensureActive()
            val typeface = validatedTypeface(staged)
            val hash = sha256(staged)
            val name = displayName.substringAfterLast('/').substringAfterLast('\\')
                .filterNot(Char::isISOControl).trim().take(100).ifBlank { "Custom font" }
            val target = File(folder, "font-${UUID.randomUUID().toString().replace("-", "")}.font")
            // Cancellation can discard staging, but publication and metadata form one short transaction.
            ensureActive()
            if (!staged.renameTo(target)) throw CustomFontException(CustomFontProblem.READ_FAILED)
            published = target
            val preferences = prefs(context)
            val previous = ownedFile(context, preferences.getString(FILE_KEY, null))
            val oldValues = listOf(FILE_KEY, NAME_KEY, HASH_KEY, "font_family")
                .associateWith { preferences.getString(it, null) }
            if (!preferences.edit().putString(FILE_KEY, target.name).putString(NAME_KEY, name)
                    .putString(HASH_KEY, hash).putString("font_family", "custom").commit()) {
                // SharedPreferences may have changed its in-memory map even when disk commit failed.
                preferences.edit(commit = true) { oldValues.forEach { (key, value) ->
                    if (value == null) remove(key) else putString(key, value)
                } }
                throw CustomFontException(CustomFontProblem.READ_FAILED)
            }
            published = null
            previous?.delete()
            return CustomFontState(name, typeface)
        } catch (error: CancellationException) {
            throw error
        } catch (error: CustomFontException) {
            throw error
        } catch (error: Exception) {
            throw CustomFontException(CustomFontProblem.READ_FAILED, error)
        } finally {
            staged.delete()
            published?.delete()
        }
    }

    fun load(context: Context): CustomFontState {
        val preferences = prefs(context)
        val path = preferences.getString(FILE_KEY, null) ?: return CustomFontState()
        val name = preferences.getString(NAME_KEY, null)
        val file = ownedFile(context, path)
        if (file == null || !file.isFile) return CustomFontState(name, problem = CustomFontProblem.MISSING)
        return try {
            if (file.length() > MAX_BYTES || preferences.getString(HASH_KEY, null) != sha256(file)) {
                CustomFontState(name, problem = CustomFontProblem.CORRUPT)
            } else CustomFontState(name, validatedTypeface(file))
        } catch (_: Exception) {
            CustomFontState(name, problem = CustomFontProblem.CORRUPT)
        }
    }

    fun remove(context: Context) {
        val preferences = prefs(context)
        val file = ownedFile(context, preferences.getString(FILE_KEY, null))
        commitWithRollback(preferences) {
            remove(FILE_KEY).remove(NAME_KEY).remove(HASH_KEY)
            if (preferences.getString("font_family", "system") == "custom") putString("font_family", "system")
        }
        file?.delete()
    }

    fun resetAppearance(context: Context) {
        val preferences = prefs(context)
        val file = ownedFile(context, preferences.getString(FILE_KEY, null))
        commitWithRollback(preferences) { clear() }
        file?.delete()
    }

    private fun commitWithRollback(preferences: SharedPreferences, change: SharedPreferences.Editor.() -> Unit) {
        val previous = preferences.all.toMap()
        if (preferences.edit().apply(change).commit()) return
        preferences.edit(commit = true) {
            clear()
            previous.forEach { (key, value) ->
                when (value) {
                    is String -> putString(key, value)
                    is Boolean -> putBoolean(key, value)
                    is Float -> putFloat(key, value)
                    is Int -> putInt(key, value)
                    is Long -> putLong(key, value)
                    is Set<*> -> putStringSet(key, value.filterIsInstance<String>().toSet())
                }
            }
        }
        throw CustomFontException(CustomFontProblem.READ_FAILED)
    }

    private fun validatedTypeface(file: File): Typeface {
        try {
            RandomAccessFile(file, "r").use { input ->
                if (input.length() < 12) throw CustomFontException(CustomFontProblem.INVALID)
                val signature = input.readInt()
                // TTF/OpenType sfnt only. Collections have separate indexing and are not imported.
                if (signature != 0x00010000 && signature != 0x4f54544f && signature != 0x74727565)
                    throw CustomFontException(CustomFontProblem.INVALID)
                val count = input.readUnsignedShort()
                if (count == 0 || 12L + count * 16L > input.length()) throw CustomFontException(CustomFontProblem.INVALID)
                input.seek(12)
                repeat(count) {
                    input.skipBytes(8)
                    val offset = input.readInt().toLong() and 0xffffffffL
                    val length = input.readInt().toLong() and 0xffffffffL
                    if (offset + length > input.length()) throw CustomFontException(CustomFontProblem.INVALID)
                }
            }
            // Keep the validated font in memory instead of retaining an OS mapping of the owned file.
            // This lets replacement/removal release its file immediately on every supported host.
            val buffer = ByteBuffer.allocateDirect(file.length().toInt())
            FileInputStream(file).use { input ->
                while (buffer.hasRemaining()) {
                    if (input.channel.read(buffer) < 0) throw CustomFontException(CustomFontProblem.INVALID)
                }
            }
            buffer.flip()
            val font = Font.Builder(buffer).build()
            return Typeface.CustomFallbackBuilder(AndroidFontFamily.Builder(font).build())
                .setStyle(font.style).build()
        } catch (error: Exception) {
            throw CustomFontException(CustomFontProblem.INVALID, error)
        }
    }

    private fun sha256(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(16 * 1024)
            while (true) {
                val read = input.read(buffer)
                if (read < 0) break
                digest.update(buffer, 0, read)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }
}
