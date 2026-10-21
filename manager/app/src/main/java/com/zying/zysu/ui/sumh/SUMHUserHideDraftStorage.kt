package com.zying.zysu.ui.sumh

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.charset.CodingErrorAction
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.nio.file.StandardOpenOption
import java.nio.file.LinkOption

internal interface UserHideDraftStorage {
    fun read(): UserHideDraftSnapshot
    fun write(snapshot: UserHideDraftSnapshot)
}

/** One bounded app-private document, excluded from Android backup. */
internal class UserHideDraftFile(private val directory: File) : UserHideDraftStorage {
    private val file get() = File(directory, "$USER_HIDE_DRAFT_ID.bin")

    override fun read(): UserHideDraftSnapshot = synchronized(lock) {
        val bytes = try {
            file.inputStream().use { input ->
                val buffer = ByteArray(USER_HIDE_DRAFT_BYTES + 1)
                var count = 0
                while (count < buffer.size) {
                    val read = input.read(buffer, count, buffer.size - count)
                    if (read < 0) break
                    if (read == 0) throw IOException("Draft read made no progress")
                    count += read
                }
                if (count > USER_HIDE_DRAFT_BYTES) throw IOException("Draft exceeds storage budget")
                buffer.copyOf(count)
            }
        } catch (e: FileNotFoundException) {
            if (Files.notExists(directory.toPath()) || (directory.isDirectory && Files.notExists(file.toPath())))
                return@synchronized UserHideDraftSnapshot()
            throw e
        }
        decodeUserHideDraft(bytes)
    }

    override fun write(snapshot: UserHideDraftSnapshot) = synchronized(lock) {
        val bytes = encodeUserHideDraft(snapshot)
        if (!directory.isDirectory && !directory.mkdirs()) throw IOException("Cannot create draft directory")
        // The single writer reuses one reserved slot, so process death cannot
        // accumulate unbounded orphan files. A stale slot is never a document.
        val temporary = File(directory, "$USER_HIDE_DRAFT_ID.tmp")
        try {
            FileChannel.open(temporary.toPath(), StandardOpenOption.CREATE, StandardOpenOption.WRITE,
                StandardOpenOption.TRUNCATE_EXISTING, LinkOption.NOFOLLOW_LINKS).use { channel ->
                val buffer = ByteBuffer.wrap(bytes)
                while (buffer.hasRemaining()) if (channel.write(buffer) <= 0) throw IOException("Draft write made no progress")
                channel.force(true)
            }
            // Refuse unsupported atomic replacement; do not delete the old document first.
            Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
        } finally {
            temporary.delete()
        }
        if (!file.readBytes().contentEquals(bytes)) throw IOException("Draft replacement was not confirmed")
    }

    companion object { private val lock = Any() }
}

internal fun encodeUserHideDraft(snapshot: UserHideDraftSnapshot): ByteArray {
    require(userHideDraftLimit(snapshot) == null) { "Draft exceeds budget" }
    return ByteArrayOutputStream().also { buffer ->
        DataOutputStream(buffer).use { out ->
            fun text(value: String) { val bytes = value.toByteArray(Charsets.UTF_8); out.writeInt(bytes.size); out.write(bytes) }
            out.writeInt(0x53484431) // SHD1; future formats fail closed.
            text(snapshot.add); text(snapshot.bulk)
            out.writeInt(snapshot.edits.size)
            snapshot.edits.forEach { (path, value) -> text(path); text(value) }
            out.writeInt(snapshot.retryPaths.size)
            snapshot.retryPaths.forEach(::text)
        }
    }.toByteArray()
}

internal fun decodeUserHideDraft(bytes: ByteArray): UserHideDraftSnapshot {
    if (bytes.size > USER_HIDE_DRAFT_BYTES) throw IOException("Draft exceeds budget")
    return DataInputStream(ByteArrayInputStream(bytes)).use { input ->
        fun text(): String {
            val size = input.readInt()
            if (size < 0 || size > input.available()) throw IOException("Invalid draft field length")
            val value = ByteArray(size)
            input.readFully(value)
            return Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(value)).toString()
        }
        fun count(max: Int): Int = input.readInt().also { if (it !in 0..max) throw IOException("Invalid draft count") }
        if (input.readInt() != 0x53484431) throw IOException("Unsupported draft format")
        val add = text(); val bulk = text()
        val edits = LinkedHashMap<String, String>()
        repeat(count(USER_HIDE_DRAFT_ENTRIES)) {
            val key = text(); val value = text()
            if (edits.put(key, value) != null) throw IOException("Duplicate draft key")
        }
        val retry = List(count(USER_HIDE_BULK_LINES)) { text() }
        val result = UserHideDraftSnapshot(add, bulk, edits, retry)
        if (input.available() != 0 || userHideDraftLimit(result) != null) throw IOException("Invalid draft document")
        result
    }
}
