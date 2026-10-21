package com.zying.zysu.ui.sumh.util

import android.system.Os
import android.system.OsConstants
import java.io.*
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.charset.CodingErrorAction
import java.nio.file.*
import java.nio.file.attribute.BasicFileAttributes

internal const val SUMH_OPERATION_LOCAL_BYTES = 1024 * 1024
internal const val SUMH_OPERATION_LOCAL_COUNT = 16
internal data class SUMHTrackedOperation(val id: String, val requestRevision: String, val bootId: String,
    val action: String, val selector: String, val configRevision: String, val stage: String,
    val response: String = "", val observation: String = "", val error: String = "", val recovered: String = "") {
    val name get() = "hide $action"
    val expectedDefinitionId get() = selector.takeIf { action in setOf("annotate", "enable", "disable", "retry", "remove-id", "import-id") }
    fun fields() = listOf(id, requestRevision, bootId, action, selector, configRevision, stage, response, observation, error, recovered)
    fun submission() = response.takeIf { it.isNotEmpty() }?.let {
        // The resource's exit code remains inside this saved submission. This is a historical receipt, not a new successful transport.
        SUMHOperationsParser.submission(SUMHQueryReply(it), requestRevision, id, bootId)
    }
    fun resource(): HideMutationResult? = submission()?.let { receipt ->
        if (receipt.ran != true || receipt.stdout.encoding != "utf-8" || !receipt.stdout.complete || !receipt.stderr.complete) null
        else HideDefinitionsParser.mutation(SUMHQueryReply(receipt.stdout.value,
            if (receipt.status == "command_completed") null else "operation_${receipt.status}"), action, configRevision, bootId, expectedDefinitionId)
    }
    fun recovery() = recovered.takeIf { it.isNotEmpty() }?.let { SUMHOperationsParser.resourceResult(SUMHQueryReply(it), this) }
    val safelyPrunable get() = stage == "received" && runCatching { resource()?.succeeded == true }.getOrDefault(false)
}
internal interface SUMHOperationStorage {
    fun read(): List<SUMHTrackedOperation>
    /** Must confirm atomic replacement, file and directory sync, then readback before returning. */
    fun write(records: List<SUMHTrackedOperation>)
}
private val trackedActions = setOf("create", "annotate", "enable", "disable", "retry", "remove-id", "enable-group", "disable-group", "import-id")
internal fun validateTrackedOperation(record: SUMHTrackedOperation) {
    require(validHideDefinitionId(record.id) && validHideDefinitionRevision(record.requestRevision) && validHideDefinitionRevision(record.configRevision))
    require(record.bootId.isNotEmpty() && validHideDefinitionText(record.bootId, 80))
    require(record.action in trackedActions && record.stage in setOf("prepared", "dispatching", "received", "unknown"))
    when {
        record.action == "create" -> require(record.selector.isEmpty())
        record.action.endsWith("-group") -> require(validHideDefinitionText(record.selector, 128))
        else -> require(validHideDefinitionId(record.selector))
    }
    require(validHideDefinitionText(record.error, 1500))
    require(record.response.toByteArray(Charsets.UTF_8).size <= SUMH_OPERATION_SUBMISSION_BYTES && record.observation.toByteArray(Charsets.UTF_8).size <= 64 * 1024)
    require(record.recovered.toByteArray(Charsets.UTF_8).size <= 64 * 1024)
    if (record.response.isNotEmpty()) {
        require(record.stage in setOf("received", "unknown"))
        val receipt = requireNotNull(record.submission())
        require(receipt.status !in setOf("prepared", "reservation_unconfirmed"))
    }
    if (record.stage == "received") require(record.response.isNotEmpty())
    if (record.observation.isNotEmpty()) SUMHOperationsParser.observation(SUMHQueryReply(record.observation), record.id,
        record.requestRevision, record.name, record.bootId)
    if (record.recovered.isNotEmpty()) record.recovery()
}
internal fun encodeSUMHTrackedOperations(records: List<SUMHTrackedOperation>): ByteArray {
    require(records.size <= SUMH_OPERATION_LOCAL_COUNT && records.map { it.id }.distinct().size == records.size)
    records.forEach(::validateTrackedOperation)
    val bytes = ByteArrayOutputStream().also { buffer -> DataOutputStream(buffer).use { out ->
        out.writeInt(0x534F5031); out.writeInt(records.size) // SOP1; unknown versions never overwritten.
        records.forEach { record -> record.fields().forEach { value ->
            val field = value.toByteArray(Charsets.UTF_8); require(Charsets.UTF_8.newEncoder().canEncode(value))
            out.writeInt(field.size); out.write(field)
        } }
    } }.toByteArray()
    require(bytes.size <= SUMH_OPERATION_LOCAL_BYTES) { "operation_tracking_capacity" }
    return bytes
}
internal fun decodeSUMHTrackedOperations(bytes: ByteArray): List<SUMHTrackedOperation> {
    require(bytes.size <= SUMH_OPERATION_LOCAL_BYTES)
    return DataInputStream(ByteArrayInputStream(bytes)).use { input ->
        require(input.readInt() == 0x534F5031) { "operation_tracking_future_format" }
        val count = input.readInt(); require(count in 0..SUMH_OPERATION_LOCAL_COUNT)
        fun text(max: Int): String {
            val length = input.readInt(); require(length in 0..max && length <= input.available())
            val value = ByteArray(length); input.readFully(value)
            return Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(value)).toString()
        }
        val records = List(count) { SUMHTrackedOperation(text(32), text(74), text(80), text(32), text(128), text(74),
            text(16), text(SUMH_OPERATION_SUBMISSION_BYTES), text(64 * 1024), text(1500), text(64 * 1024)) }
        require(input.available() == 0 && records.map { it.id }.distinct().size == records.size)
        records.forEach(::validateTrackedOperation); records
    }
}

internal class SUMHOperationFile(private val directory: File,
    private val syncDirectory: (File) -> Unit = ::syncSUMHOperationDirectory) : SUMHOperationStorage {
    private val file get() = File(directory, "operations-v1.bin")
    override fun read(): List<SUMHTrackedOperation> = synchronized(lock) {
        require(!Files.isSymbolicLink(directory.toPath()) && !Files.isSymbolicLink(file.toPath()))
        if (Files.notExists(directory.toPath()) || directory.isDirectory && Files.notExists(file.toPath())) return@synchronized emptyList()
        require(Files.readAttributes(file.toPath(), BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS).isRegularFile)
        val bytes = Files.newInputStream(file.toPath(), LinkOption.NOFOLLOW_LINKS).use { input ->
            val buffer = ByteArray(SUMH_OPERATION_LOCAL_BYTES + 1); var used = 0
            while (used < buffer.size) {
                val n = input.read(buffer, used, buffer.size - used)
                if (n < 0) break
                if (n == 0) throw IOException("Operation read made no progress")
                used += n
            }
            require(used <= SUMH_OPERATION_LOCAL_BYTES); buffer.copyOf(used)
        }
        decodeSUMHTrackedOperations(bytes)
    }
    override fun write(records: List<SUMHTrackedOperation>) = synchronized(lock) {
        val bytes = encodeSUMHTrackedOperations(records)
        require(!Files.isSymbolicLink(directory.toPath()) && !Files.isSymbolicLink(file.toPath()))
        // Refuse to replace unreadable, corrupt or newer local formats, even when called directly.
        read()
        if (!directory.isDirectory) {
            if (!directory.mkdir()) throw IOException("Cannot create operation directory")
        }
        // Repeat parent sync after a previous unconfirmed mkdir; an existing directory alone is not confirmation.
        syncDirectory(requireNotNull(directory.parentFile))
        val temporary = File(directory, "operations-v1.tmp")
        if (Files.exists(temporary.toPath(), LinkOption.NOFOLLOW_LINKS)) {
            require(Files.readAttributes(temporary.toPath(), BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS).isRegularFile)
            // One app process owns this exact scratch name. Never truncate a leftover file (or a hard-linked target).
            Files.delete(temporary.toPath())
        }
        try {
            FileChannel.open(temporary.toPath(), StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE,
                LinkOption.NOFOLLOW_LINKS).use { channel ->
                val buffer = ByteBuffer.wrap(bytes)
                while (buffer.hasRemaining()) if (channel.write(buffer) <= 0) throw IOException("Operation write made no progress")
                channel.force(true)
            }
            Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
            syncDirectory(directory)
            if (read() != records) throw IOException("Operation tracking readback differs")
        } finally { temporary.delete() }
    }
    companion object { private val lock = Any() }
}
private fun syncSUMHOperationDirectory(directory: File) {
    val fd = Os.open(directory.absolutePath, OsConstants.O_RDONLY or OsConstants.O_NOFOLLOW or OsConstants.O_CLOEXEC, 0)
    try { require(OsConstants.S_ISDIR(Os.fstat(fd).st_mode)); Os.fsync(fd) } finally { Os.close(fd) }
}
