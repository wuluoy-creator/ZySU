package com.zying.zysu.ui.sumh

import java.io.*
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.charset.CodingErrorAction
import java.nio.file.*

internal const val EDITOR_DRAFT_BYTES = 4 * 1024 * 1024
internal const val EDITOR_DRAFT_COUNT = 512
internal const val EDITOR_DRAFT_RECORD_BYTES = 128 * 1024
internal const val EDITOR_DRAFT_FIELD_BYTES = 4095
internal data class EditorDraftRecord(val fields: List<String>)
internal typealias EditorDraftSnapshot = Map<String, EditorDraftRecord>

internal fun editorDraftWithinBudget(snapshot: EditorDraftSnapshot): Boolean {
    if (snapshot.size > EDITOR_DRAFT_COUNT) return false
    var total = 8L
    for ((key, record) in snapshot) {
        if (key.length > 4200 || key.toByteArray().size > 4200 || record.fields.size > 520) return false
        var size = 8L + key.toByteArray().size
        for (field in record.fields) {
            if (field.length > EDITOR_DRAFT_FIELD_BYTES) return false
            if (!Charsets.UTF_8.newEncoder().canEncode(field)) return false
            val bytes = field.toByteArray(Charsets.UTF_8).size
            if (bytes > EDITOR_DRAFT_FIELD_BYTES) return false
            size += 4L + bytes
        }
        if (size > EDITOR_DRAFT_RECORD_BYTES) return false
        total += size
        if (total > EDITOR_DRAFT_BYTES) return false
    }
    return true
}

internal interface EditorDraftStorage {
    fun read(): EditorDraftSnapshot
    fun write(snapshot: EditorDraftSnapshot)
}

/** One document and one reusable temporary slot, both excluded from Android backup. */
internal class EditorDraftFile(private val directory: File) : EditorDraftStorage {
    private val file get() = File(directory, "editors-v1.bin")
    override fun read(): EditorDraftSnapshot = synchronized(lock) {
        val bytes = try {
            file.inputStream().use { input ->
                val buffer = ByteArray(EDITOR_DRAFT_BYTES + 1)
                var count = 0
                while (count < buffer.size) {
                    val n = input.read(buffer, count, buffer.size - count)
                    if (n < 0) break
                    if (n == 0) throw IOException("Draft read made no progress")
                    count += n
                }
                if (count > EDITOR_DRAFT_BYTES) throw IOException("Draft exceeds budget")
                buffer.copyOf(count)
            }
        } catch (e: FileNotFoundException) {
            if (Files.notExists(directory.toPath()) || (directory.isDirectory && Files.notExists(file.toPath())))
                return@synchronized emptyMap()
            throw e
        }
        decodeEditorDrafts(bytes)
    }
    override fun write(snapshot: EditorDraftSnapshot) = synchronized(lock) {
        val bytes = encodeEditorDrafts(snapshot)
        if (!directory.isDirectory && !directory.mkdirs()) throw IOException("Cannot create draft directory")
        val temporary = File(directory, "editors-v1.tmp")
        try {
            FileChannel.open(temporary.toPath(), StandardOpenOption.CREATE, StandardOpenOption.WRITE,
                StandardOpenOption.TRUNCATE_EXISTING, LinkOption.NOFOLLOW_LINKS).use { channel ->
                val buffer = ByteBuffer.wrap(bytes)
                while (buffer.hasRemaining()) if (channel.write(buffer) <= 0) throw IOException("Draft write made no progress")
                channel.force(true)
            }
            Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
        } finally { temporary.delete() }
        if (!file.readBytes().contentEquals(bytes)) throw IOException("Draft replacement was not confirmed")
    }
    companion object { private val lock = Any() }
}

internal fun encodeEditorDrafts(snapshot: EditorDraftSnapshot): ByteArray {
    require(editorDraftWithinBudget(snapshot)) { "Draft exceeds budget" }
    return ByteArrayOutputStream().also { buffer -> DataOutputStream(buffer).use { out ->
        fun text(value: String) { val bytes = value.toByteArray(Charsets.UTF_8); out.writeInt(bytes.size); out.write(bytes) }
        out.writeInt(0x53454431); out.writeInt(snapshot.size) // SED1, fail closed on unknown versions.
        snapshot.forEach { (key, record) -> text(key); out.writeInt(record.fields.size); record.fields.forEach(::text) }
    } }.toByteArray()
}

internal fun decodeEditorDrafts(bytes: ByteArray): EditorDraftSnapshot {
    if (bytes.size > EDITOR_DRAFT_BYTES) throw IOException("Draft exceeds budget")
    return DataInputStream(ByteArrayInputStream(bytes)).use { input ->
        fun text(max: Int): String {
            val size = input.readInt()
            if (size !in 0..max || size > input.available()) throw IOException("Invalid draft field length")
            val value = ByteArray(size); input.readFully(value)
            return Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(value)).toString()
        }
        fun count(max: Int) = input.readInt().also { if (it !in 0..max) throw IOException("Invalid draft count") }
        if (input.readInt() != 0x53454431) throw IOException("Unsupported draft format")
        val result = linkedMapOf<String, EditorDraftRecord>()
        repeat(count(EDITOR_DRAFT_COUNT)) {
            val key = text(4200)
            val record = EditorDraftRecord(List(count(520)) { text(EDITOR_DRAFT_FIELD_BYTES) })
            if (!validEditorDraftRecord(key, record) || result.put(key, record) != null) throw IOException("Invalid draft record")
        }
        if (input.available() != 0 || !editorDraftWithinBudget(result)) throw IOException("Invalid draft document")
        result
    }
}
