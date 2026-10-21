package com.zying.zysu.ui.sumh

import android.system.Os
import android.system.OsConstants
import com.zying.zysu.ui.sumh.util.validDiagnosticPackageId
import com.zying.zysu.ui.sumh.util.validDiagnosticPackageRevision
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.StandardCopyOption
import java.nio.file.StandardOpenOption
import java.nio.file.attribute.BasicFileAttributes

/** A recovery hint only. No request body, package bytes, identifiers opt-in or replay authority. */
internal data class SUMHDiagnosticPackagePending(val action: String, val id: String, val revision: String?) {
    init {
        require(action == "create" || action == "remove")
        require(validDiagnosticPackageId(id))
        require(if (action == "create") revision == null else revision != null && validDiagnosticPackageRevision(revision))
    }
}
internal interface SUMHDiagnosticPackageCheckpoint {
    fun read(): SUMHDiagnosticPackagePending?
    /** Compare under the file lock; true requires replacement, sync and readback. */
    fun compareAndSet(expected: SUMHDiagnosticPackagePending?, pending: SUMHDiagnosticPackagePending?): Boolean
}
internal const val SUMH_DIAGNOSTIC_CHECKPOINT_BYTES = 256
internal fun encodeSUMHDiagnosticPackagePending(value: SUMHDiagnosticPackagePending?): ByteArray =
    ByteArrayOutputStream().also { buffer -> DataOutputStream(buffer).use { out ->
        out.writeInt(0x53445031) // SDP1, unknown versions are preserved.
        out.writeByte(if (value == null) 0 else 1)
        if (value != null) {
            out.writeUTF(value.action); out.writeUTF(value.id); out.writeUTF(value.revision.orEmpty())
        }
    } }.toByteArray().also { require(it.size <= SUMH_DIAGNOSTIC_CHECKPOINT_BYTES) }
internal fun decodeSUMHDiagnosticPackagePending(bytes: ByteArray): SUMHDiagnosticPackagePending? {
    require(bytes.size <= SUMH_DIAGNOSTIC_CHECKPOINT_BYTES)
    return DataInputStream(ByteArrayInputStream(bytes)).use { input ->
        require(input.readInt() == 0x53445031)
        val present = input.readUnsignedByte().also { require(it in 0..1) }
        val result = if (present == 0) null else SUMHDiagnosticPackagePending(input.readUTF(), input.readUTF(), input.readUTF().ifEmpty { null })
        require(input.available() == 0)
        result
    }
}

/** Caller provides a dedicated child of app.noBackupFilesDir, not shared/external cache. */
internal class SUMHDiagnosticPackageFile(
    private val directory: File, private val syncDirectory: (File) -> Unit = ::syncDiagnosticPackageDirectory,
) : SUMHDiagnosticPackageCheckpoint {
    private val file get() = File(directory, "pending-v1.bin")
    private val stage get() = File(directory, "pending-v1.stage")
    override fun read(): SUMHDiagnosticPackagePending? = synchronized(lock) { readBytes()?.let(::decodeSUMHDiagnosticPackagePending) }
    override fun compareAndSet(expected: SUMHDiagnosticPackagePending?, pending: SUMHDiagnosticPackagePending?): Boolean = synchronized(lock) {
        val bytes = encodeSUMHDiagnosticPackagePending(pending)
        // A future/corrupt format cannot be silently overwritten or reset.
        if (read() != expected) return@synchronized false
        checkDirectory()
        if (!directory.isDirectory && !directory.mkdir()) throw IOException("diagnostic_checkpoint_directory")
        syncDirectory(requireNotNull(directory.parentFile))
        if (Files.exists(stage.toPath(), LinkOption.NOFOLLOW_LINKS)) {
            require(Files.readAttributes(stage.toPath(), BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS).isRegularFile)
            require(Files.size(stage.toPath()) <= SUMH_DIAGNOSTIC_CHECKPOINT_BYTES)
            Files.delete(stage.toPath()) // Never truncate an existing file or hardlink.
        }
        try {
            FileChannel.open(stage.toPath(), StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE, LinkOption.NOFOLLOW_LINKS).use { channel ->
                val buffer = ByteBuffer.wrap(bytes)
                while (buffer.hasRemaining()) if (channel.write(buffer) <= 0) throw IOException("diagnostic_checkpoint_write")
                channel.force(true)
            }
            Files.move(stage.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
            syncDirectory(directory)
            require(readBytes()?.contentEquals(bytes) == true)
        } finally { stage.delete() }
        true
    }
    private fun checkDirectory() {
        require(!Files.isSymbolicLink(directory.toPath()))
        if (Files.exists(directory.toPath(), LinkOption.NOFOLLOW_LINKS))
            require(Files.readAttributes(directory.toPath(), BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS).isDirectory)
    }
    private fun readBytes(): ByteArray? {
        checkDirectory(); require(!Files.isSymbolicLink(file.toPath()))
        if (Files.notExists(directory.toPath()) || Files.notExists(file.toPath())) return null
        require(Files.readAttributes(file.toPath(), BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS).isRegularFile)
        return Files.newInputStream(file.toPath(), LinkOption.NOFOLLOW_LINKS).use { input ->
            val bytes = ByteArray(SUMH_DIAGNOSTIC_CHECKPOINT_BYTES + 1); var used = 0
            while (used < bytes.size) {
                val count = input.read(bytes, used, bytes.size - used)
                if (count < 0) break
                if (count == 0) throw IOException("diagnostic_checkpoint_read")
                used += count
            }
            require(used <= SUMH_DIAGNOSTIC_CHECKPOINT_BYTES)
            bytes.copyOf(used)
        }
    }
    companion object { private val lock = Any() }
}
private fun syncDiagnosticPackageDirectory(directory: File) {
    val descriptor = Os.open(directory.absolutePath, OsConstants.O_RDONLY or OsConstants.O_NOFOLLOW or OsConstants.O_CLOEXEC, 0)
    try { require(OsConstants.S_ISDIR(Os.fstat(descriptor).st_mode)); Os.fsync(descriptor) } finally { Os.close(descriptor) }
}
