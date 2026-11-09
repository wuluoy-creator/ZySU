package com.zying.zysu.ui.webui.file

import android.webkit.JavascriptInterface
import java.io.OutputStream
import java.util.concurrent.ConcurrentHashMap

internal const val TAG = "KsuIO"

class KsuIO {
    companion object {
        private const val KSU_IO_VERSION = 1
        private const val MAX_OPEN_STREAMS = 50
    }
    @Volatile internal var closed = false
        private set

    internal val openInputStreams = ConcurrentHashMap<String, ManagedInputStream>()
    internal val openOutputStreams = ConcurrentHashMap<String, OutputStream>()

    private val fileOutputStream = FileOutputStreamInterface(this)
    private val fileInputStream = FileInputStreamInterface(this)
    private val randomAccessFile = RandomAccessFileInterface(this)

    @JavascriptInterface
    fun version(): Int = KSU_IO_VERSION

    @JavascriptInterface
    fun File(path: String): FileInterface = FileInterface(path, this)

    @JavascriptInterface
    fun FileInputStream(): FileInputStreamInterface = fileInputStream

    @JavascriptInterface
    fun FileOutputStream(): FileOutputStreamInterface = fileOutputStream

    @JavascriptInterface
    fun RandomAccessFile(): RandomAccessFileInterface = randomAccessFile

    /**
     * Check if we can open a new stream without exceeding resource limits.
     * Returns true if under limit, false otherwise.
     */
    internal fun canOpenStream(): Boolean {
        return !closed && openInputStreams.size + openOutputStreams.size < MAX_OPEN_STREAMS
    }

    @Synchronized
    fun destroy() {
        closed = true
        fileInputStream.closeAll()
        fileOutputStream.closeAll()
        randomAccessFile.closeAll()
    }
}
