package com.zying.zysu.ui.webui

import android.app.Activity
import android.app.DownloadManager
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.Manifest
import android.content.ContentValues
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import android.util.Base64
import android.util.Log
import android.webkit.CookieManager
import android.webkit.JavascriptInterface
import android.webkit.URLUtil
import android.webkit.WebView
import android.widget.Toast
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import androidx.core.net.toUri
import com.zying.zysu.R
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import java.io.ByteArrayInputStream
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.net.URLConnection
import java.util.UUID
import java.util.concurrent.atomic.AtomicInteger
import kotlin.coroutines.coroutineContext

internal fun sanitizeWebUIDownloadFileName(name: String?): String = name.orEmpty().trim()
    .substringAfterLast('/').substringAfterLast('\\')
    .filter { it.code >= 32 && it != '\u007f' }.trim()
    .takeUnless { it.isEmpty() || it == "." || it == ".." } ?: "download.bin"

internal fun resolveWebUIDownloadMimeType(name: String, supplied: String?): String =
    supplied?.substringBefore(';')?.trim()?.takeIf { '/' in it && it != "application/octet-stream" }
        ?: URLConnection.guessContentTypeFromName(name) ?: "application/octet-stream"

/** Each WebView owns its temporary transfers; saved downloads belong to the user. */
class WebUIDownloadInterface(private val activity: Activity, private val webView: WebView) {
    private val context = activity.applicationContext
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val chunks = mutableMapOf<String, Chunk>()
    @Volatile private var closed = false
    private data class Chunk(val file: File, val name: String, val mime: String, val output: OutputStream)

    companion object {
        private const val TAG = "WebUIDownload"
        private const val CHANNEL = "webui_downloads"
        private const val CHUNK_LIMIT = 1024 * 1024
        private val notificationId = AtomicInteger(200000)
    }

    @JavascriptInterface
    fun openExternal(url: String) {
        val uri = url.toUri()
        if (uri.scheme != "http" && uri.scheme != "https") return
        post { activity.startActivity(Intent(Intent.ACTION_VIEW, uri)) }
    }

    @JavascriptInterface
    fun download(url: String, fileName: String?, mimeType: String?) {
        webView.post {
            if (closed) return@post
            enqueueDownload(url, webView.settings.userAgentString, null, mimeType, fileName)
        }
    }

    /** Called by WebView on its UI thread, retaining cookies and the actual user agent. */
    fun enqueueDownload(url: String, userAgent: String?, disposition: String?, mimeType: String?, name: String? = null) {
        if (closed) return
        val uri = url.toUri()
        if (uri.scheme != "http" && uri.scheme != "https") return
        runCatching {
            val fileName = sanitizeWebUIDownloadFileName(name ?: URLUtil.guessFileName(url, disposition, mimeType))
            val request = DownloadManager.Request(uri)
                .setTitle(fileName)
                .setMimeType(resolveWebUIDownloadMimeType(fileName, mimeType))
                .setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED)
                .setDestinationInExternalPublicDir(Environment.DIRECTORY_DOWNLOADS, fileName)
            userAgent?.takeIf { it.isNotBlank() }?.let { request.addRequestHeader("User-Agent", it) }
            CookieManager.getInstance().getCookie(url)?.let { request.addRequestHeader("Cookie", it) }
            context.getSystemService(DownloadManager::class.java).enqueue(request)
        }.onFailure(::reportFailure)
    }

    @JavascriptInterface
    fun save(base64: String, fileName: String?) = save(base64, fileName, null)

    @JavascriptInterface
    fun save(base64: String, fileName: String?, mimeType: String?) {
        if (closed) return
        scope.launch {
            runCatching {
                val name = sanitizeWebUIDownloadFileName(fileName)
                ByteArrayInputStream(Base64.decode(base64, Base64.DEFAULT)).use {
                    saveDownload(it, name, resolveWebUIDownloadMimeType(name, mimeType))
                }
            }.onFailure(::reportFailure)
        }
    }

    @JavascriptInterface
    @Synchronized
    fun startChunkedDownload(fileName: String?, mimeType: String?): String {
        if (closed || chunks.size >= 8) return ""
        return runCatching {
            val name = sanitizeWebUIDownloadFileName(fileName)
            val file = File.createTempFile("webui-", ".download", context.cacheDir)
            val id = UUID.randomUUID().toString()
            val output = try { file.outputStream().buffered(64 * 1024) } catch (error: Exception) {
                file.delete()
                throw error
            }
            chunks[id] = Chunk(file, name, resolveWebUIDownloadMimeType(name, mimeType), output)
            id
        }.onFailure(::reportFailure).getOrDefault("")
    }

    @JavascriptInterface
    @Synchronized
    fun writeDownloadChunk(downloadId: String, base64Chunk: String): Boolean {
        val chunk = chunks[downloadId] ?: return false
        return runCatching {
            require(base64Chunk.length <= CHUNK_LIMIT) { "Download chunk is too large" }
            chunk.output.write(Base64.decode(base64Chunk, Base64.DEFAULT))
            true
        }.onFailure {
            cancelChunkedDownload(downloadId)
            reportFailure(it)
        }.getOrDefault(false)
    }

    @JavascriptInterface
    @Synchronized
    fun completeChunkedDownload(downloadId: String): Boolean {
        val chunk = chunks.remove(downloadId) ?: return false
        return runCatching {
            chunk.output.close()
            val job = scope.launch {
                runCatching {
                    chunk.file.inputStream().use { saveDownload(it, chunk.name, chunk.mime) }
                }.onFailure(::reportFailure)
            }
            job.invokeOnCompletion { chunk.file.delete() }
            true
        }.onFailure {
            chunk.file.delete()
            reportFailure(it)
        }.getOrDefault(false)
    }

    @JavascriptInterface
    @Synchronized
    fun cancelChunkedDownload(downloadId: String) {
        chunks.remove(downloadId)?.let {
            runCatching { it.output.close() }
            it.file.delete()
        }
    }

    private suspend fun copy(source: InputStream, target: OutputStream) {
        val buffer = ByteArray(64 * 1024)
        while (true) {
            coroutineContext.ensureActive()
            val size = source.read(buffer)
            if (size < 0) break
            target.write(buffer, 0, size)
        }
        target.flush()
    }

    private suspend fun saveDownload(source: InputStream, name: String, mime: String) {
        var uri: Uri? = null
        try {
            val values = ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, name)
                put(MediaStore.Downloads.MIME_TYPE, mime)
                put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS)
                put(MediaStore.Downloads.IS_PENDING, 1)
            }
            uri = checkNotNull(context.contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values))
            checkNotNull(context.contentResolver.openOutputStream(uri)).use { copy(source, it) }
            context.contentResolver.update(uri, ContentValues().apply { put(MediaStore.Downloads.IS_PENDING, 0) }, null, null)
            notifySaved(checkNotNull(uri), name, mime)
        } catch (error: Exception) {
            uri?.let { runCatching { context.contentResolver.delete(it, null, null) } }
            throw error
        }
    }

    private fun notifySaved(uri: Uri, name: String, mime: String) {
        post { Toast.makeText(activity, activity.getString(R.string.webui_download_saved, name), Toast.LENGTH_LONG).show() }
        // Notification permission is optional; denial must not discard a saved file.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return
        runCatching {
            val manager = context.getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(NotificationChannel(CHANNEL, context.getString(R.string.webui_downloads), NotificationManager.IMPORTANCE_LOW))
            val id = notificationId.incrementAndGet()
            val intent = Intent(Intent.ACTION_VIEW).setDataAndType(uri, mime)
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
            val pending = PendingIntent.getActivity(context, id, Intent.createChooser(intent, null).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
            manager.notify(id, NotificationCompat.Builder(context, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_sys_download_done)
                .setContentTitle(context.getString(R.string.webui_download_saved, name))
                .setContentIntent(pending).setAutoCancel(true).build())
        }.onFailure { Log.w(TAG, "Download saved; notification unavailable", it) }
    }

    private fun post(action: () -> Unit) {
        webView.post {
            if (!closed && !activity.isFinishing && !activity.isDestroyed) runCatching(action).onFailure(::reportFailure)
        }
    }

    private fun reportFailure(error: Throwable) {
        if (error is kotlinx.coroutines.CancellationException || closed) return
        Log.e(TAG, "WebUI transfer failed", error)
        webView.post {
            if (!closed) Toast.makeText(activity, R.string.operation_failed, Toast.LENGTH_LONG).show()
        }
    }

    @Synchronized
    fun destroy() {
        closed = true
        scope.cancel()
        chunks.keys.toList().forEach(::cancelChunkedDownload)
    }
}
