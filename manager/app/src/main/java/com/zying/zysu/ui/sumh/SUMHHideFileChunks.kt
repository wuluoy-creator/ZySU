package com.zying.zysu.ui.sumh

import java.io.Closeable
import java.io.IOException
import java.io.InputStream
import java.io.InputStreamReader
import java.io.PushbackReader
import java.nio.charset.CodingErrorAction

/** Read incrementally, with a bounded line buffer even for maliciously large lines. */
internal class SUMHHideFileChunks(input: InputStream) : Closeable {
    private val reader = PushbackReader(InputStreamReader(input, Charsets.UTF_8.newDecoder()
        .onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT)), 1)
    private var pending: String? = null
    var exhausted = false
        private set

    private fun line(): String? {
        val text = StringBuilder()
        while (true) {
            val char = reader.read()
            if (char == -1) { exhausted = true; return if (text.isEmpty()) null else text.toString() }
            if (char == '\n'.code) return text.toString()
            if (char == '\r'.code) {
                val next = reader.read()
                if (next >= 0 && next != '\n'.code) reader.unread(next)
                if (next < 0) exhausted = true
                return text.toString()
            }
            text.append(char.toChar())
            if (text.length >= 4096) throw IOException("Line exceeds 4095 bytes")
        }
    }

    fun next(): String {
        val lines = ArrayList<String>()
        var bytes = 0
        while (lines.size < 256) {
            val next = pending.also { pending = null } ?: line() ?: break
            val size = next.toByteArray(Charsets.UTF_8).size
            if (size >= 4096) throw IOException("Line exceeds 4095 bytes")
            if (lines.isNotEmpty() && bytes + size + 1 > 64 * 1024) { pending = next; break }
            lines.add(next)
            bytes += size + 1
        }
        return lines.joinToString("\n")
    }

    val hasMore get() = !exhausted || pending != null
    override fun close() = reader.close()
}
