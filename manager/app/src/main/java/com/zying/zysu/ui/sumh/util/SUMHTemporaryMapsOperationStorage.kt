package com.zying.zysu.ui.sumh.util

import android.system.Os
import android.system.OsConstants
import java.io.*
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.charset.CodingErrorAction
import java.nio.file.*
import java.nio.file.attribute.BasicFileAttributes

internal const val SUMH_TEMPORARY_MAPS_OPERATION_LOCAL_BYTES = 1024 * 1024
internal const val SUMH_TEMPORARY_MAPS_OPERATION_LOCAL_COUNT = 16

internal interface SUMHTemporaryMapsOperationStorage {
    fun read(): List<SUMHTemporaryMapsTrackedOperation>
    /** Return only after replacement, file/directory sync and exact readback have been confirmed. */
    fun write(records: List<SUMHTemporaryMapsTrackedOperation>)
}
internal fun validateTemporaryMapsTrackedOperation(record: SUMHTemporaryMapsTrackedOperation) {
    require(validHideDefinitionId(record.id) && record.requestRevision == sumhOperationRevision(temporaryMapsOperationArguments(record.key)))
    require(record.bootId.isNotEmpty() && validHideDefinitionText(record.bootId, 80))
    require(record.stage in setOf("prepared", "dispatching", "received", "unknown"))
    require(validHideDefinitionText(record.error, 1500))
    require(validHideDefinitionText(record.response, SUMH_OPERATION_SUBMISSION_BYTES) &&
        validHideDefinitionText(record.observation, 64 * 1024) && validHideDefinitionText(record.recovered, 64 * 1024))
    if (record.response.isNotEmpty()) {
        require(record.stage in setOf("received", "unknown"))
        val receipt = requireNotNull(record.submission())
        require(receipt.status !in setOf("prepared", "reservation_unconfirmed"))
        // A complete terminal response must carry the exact typed receipt before it can be retained as received.
        if (receipt.status in setOf("command_completed", "failed_or_partial"))
            requireNotNull(record.resource())
    }
    if (record.stage == "received") require(record.response.isNotEmpty())
    if (record.observation.isNotEmpty()) record.history()
    if (record.recovered.isNotEmpty()) record.recovery()
}
internal fun encodeSUMHTemporaryMapsOperations(records: List<SUMHTemporaryMapsTrackedOperation>): ByteArray {
    require(records.size <= SUMH_TEMPORARY_MAPS_OPERATION_LOCAL_COUNT && records.map { it.id }.distinct().size == records.size)
    records.forEach(::validateTemporaryMapsTrackedOperation)
    val bytes = ByteArrayOutputStream().also { buffer -> DataOutputStream(buffer).use { output ->
        output.writeInt(0x53544D31); output.writeInt(records.size) // STM1: separate from hide SOP1, no implicit migration.
        records.forEach { record -> record.fields().forEach { value ->
            require(Charsets.UTF_8.newEncoder().canEncode(value))
            val encoded = value.toByteArray(Charsets.UTF_8); output.writeInt(encoded.size); output.write(encoded)
        } }
    } }.toByteArray()
    require(bytes.size <= SUMH_TEMPORARY_MAPS_OPERATION_LOCAL_BYTES) { "temporary_maps_tracking_capacity" }
    return bytes
}
internal fun decodeSUMHTemporaryMapsOperations(bytes: ByteArray): List<SUMHTemporaryMapsTrackedOperation> {
    require(bytes.size <= SUMH_TEMPORARY_MAPS_OPERATION_LOCAL_BYTES)
    return DataInputStream(ByteArrayInputStream(bytes)).use { input ->
        require(input.readInt() == 0x53544D31) { "temporary_maps_tracking_future_format" }
        val count = input.readInt(); require(count in 0..SUMH_TEMPORARY_MAPS_OPERATION_LOCAL_COUNT)
        fun text(max: Int): String {
            val length = input.readInt(); require(length in 0..max && length <= input.available())
            val field = ByteArray(length); input.readFully(field)
            return Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(field)).toString()
        }
        val records = List(count) {
            val id = text(32); val revision = text(74); val boot = text(80)
            val key = SUMHTemporaryMapsOperationKey(text(20), text(20), text(20), text(20))
            SUMHTemporaryMapsTrackedOperation(id, revision, boot, key, text(16), text(SUMH_OPERATION_SUBMISSION_BYTES),
                text(64 * 1024), text(1500), text(64 * 1024))
        }
        require(input.available() == 0 && records.map { it.id }.distinct().size == records.size)
        records.forEach(::validateTemporaryMapsTrackedOperation); records
    }
}

internal class SUMHTemporaryMapsOperationFile(private val directory: File,
    private val syncDirectory: (File) -> Unit = ::syncTemporaryMapsOperationDirectory) : SUMHTemporaryMapsOperationStorage {
    private val file get() = File(directory, "temporary-maps-v1.bin")
    private fun readBytes(): ByteArray? {
        require(!Files.isSymbolicLink(directory.toPath()) && !Files.isSymbolicLink(file.toPath()))
        if (Files.notExists(directory.toPath()) || directory.isDirectory && Files.notExists(file.toPath())) return null
        require(directory.isDirectory && Files.readAttributes(file.toPath(), BasicFileAttributes::class.java,
            LinkOption.NOFOLLOW_LINKS).isRegularFile)
        return Files.newInputStream(file.toPath(), LinkOption.NOFOLLOW_LINKS).use { input ->
            val buffer = ByteArray(SUMH_TEMPORARY_MAPS_OPERATION_LOCAL_BYTES + 1); var used = 0
            while (used < buffer.size) {
                val count = input.read(buffer, used, buffer.size - used)
                if (count < 0) break
                if (count == 0) throw IOException("Temporary Maps operation read made no progress")
                used += count
            }
            require(used <= SUMH_TEMPORARY_MAPS_OPERATION_LOCAL_BYTES); buffer.copyOf(used)
        }
    }
    override fun read(): List<SUMHTemporaryMapsTrackedOperation> = synchronized(lock) {
        readBytes()?.let(::decodeSUMHTemporaryMapsOperations) ?: emptyList()
    }
    override fun write(records: List<SUMHTemporaryMapsTrackedOperation>) = synchronized(lock) {
        val bytes = encodeSUMHTemporaryMapsOperations(records)
        val original = readBytes()
        // Direct callers must preserve an unreadable, corrupt, or future document too.
        original?.let(::decodeSUMHTemporaryMapsOperations)
        if (!directory.isDirectory && !directory.mkdir()) throw IOException("Cannot create temporary Maps operation directory")
        // Retry directory durability even if an earlier unconfirmed mkdir left it present.
        syncDirectory(requireNotNull(directory.parentFile))
        val temporary = File(directory, "temporary-maps-v1.tmp")
        if (Files.exists(temporary.toPath(), LinkOption.NOFOLLOW_LINKS)) {
            require(Files.readAttributes(temporary.toPath(), BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS).isRegularFile)
            Files.delete(temporary.toPath()) // Unlink a stale scratch; never truncate an existing inode.
        }
        try {
            FileChannel.open(temporary.toPath(), StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE,
                LinkOption.NOFOLLOW_LINKS).use { channel ->
                val buffer = ByteBuffer.wrap(bytes)
                while (buffer.hasRemaining()) if (channel.write(buffer) <= 0) throw IOException("Temporary Maps operation write made no progress")
                channel.force(true)
            }
            val current = readBytes()
            require(if (original == null) current == null else current != null && original.contentEquals(current)) {
                "temporary_maps_tracking_source_changed"
            }
            Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
            syncDirectory(directory)
            require(readBytes()?.contentEquals(bytes) == true && read() == records) { "temporary_maps_tracking_readback_differs" }
        } finally { temporary.delete() }
    }
    companion object { private val lock = Any() }
}
private fun syncTemporaryMapsOperationDirectory(directory: File) {
    val fd = Os.open(directory.absolutePath, OsConstants.O_RDONLY or OsConstants.O_NOFOLLOW or OsConstants.O_CLOEXEC, 0)
    try { require(OsConstants.S_ISDIR(Os.fstat(fd).st_mode)); Os.fsync(fd) } finally { Os.close(fd) }
}
