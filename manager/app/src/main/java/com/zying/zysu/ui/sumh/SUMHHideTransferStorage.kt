package com.zying.zysu.ui.sumh

import android.system.Os
import android.system.OsConstants
import com.zying.zysu.ui.sumh.util.*
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.charset.CodingErrorAction
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.StandardCopyOption
import java.nio.file.StandardOpenOption
import java.nio.file.attribute.BasicFileAttributes
import java.security.MessageDigest
import java.util.UUID

internal const val SUMH_HIDE_TRANSFER_STORAGE_BYTES = 5 * 1024 * 1024
private const val HIDE_TRANSFER_DRAFT_MAGIC = 0x53485431 // SHT1
private const val HIDE_TRANSFER_EXPORT_MAGIC = 0x53484531 // SHE1

/** Inactive user input. It deliberately has no persisted review approval or execution instruction. */
internal data class SUMHHideTransferDraft(
    val document: String = "",
    val choices: Map<String, SUMHHideTransferChoice> = emptyMap(),
    val preserveImportedEnabled: Boolean = false,
    val baselineRevision: String? = null,
    val baselineBootId: String? = null,
) {
    val fingerprint: String get() = MessageDigest.getInstance("SHA-256")
        .digest(encodeSUMHHideTransferDraft(this)).joinToString("") { "%02x".format(it) }
}

/** A single frozen export. Its token must be saved before opening the system document picker. */
internal data class SUMHHideExportArtifact(val token: String, val document: String,
    val revision: String, val scope: String, val count: Int)

private fun validateHideTransferDraft(value: SUMHHideTransferDraft) {
    require(Charsets.UTF_8.newEncoder().canEncode(value.document) &&
        value.document.toByteArray(Charsets.UTF_8).size <= SUMH_HIDE_TRANSFER_BYTES) { "hide_transfer_document_budget" }
    require(value.choices.size <= SUMH_HIDE_TRANSFER_MAX_RULES) { "hide_transfer_choices_budget" }
    value.choices.forEach { (id, choice) ->
        require(validHideDefinitionId(id)) { "hide_transfer_choice_id_invalid" }
        if (choice.resolution == SUMHHideTransferResolution.ExplicitNewIdentity)
            require(validHideDefinitionId(choice.newId.orEmpty())) { "hide_transfer_new_id_required" }
        else require(choice.newId == null) { "hide_transfer_unexpected_new_id" }
    }
    require(value.baselineRevision == null || validHideDefinitionRevision(value.baselineRevision)) { "hide_transfer_revision_invalid" }
    require(value.baselineBootId == null || validHideDefinitionText(value.baselineBootId, 80)) { "hide_transfer_boot_invalid" }
}

private fun validateHideExportArtifact(value: SUMHHideExportArtifact) {
    require(validHideDefinitionId(value.token) && validHideDefinitionRevision(value.revision)) { "hide_export_identity_invalid" }
    require(value.scope in setOf("all", "selected")) { "hide_export_scope_invalid" }
    require(value.count in 0..SUMH_HIDE_TRANSFER_MAX_RULES && importSUMHHideDefinitions(value.document).size == value.count) {
        "hide_export_count_invalid"
    }
}

private fun encodeHideTransfer(write: (DataOutputStream) -> Unit): ByteArray =
    ByteArrayOutputStream().also { buffer -> DataOutputStream(buffer).use(write) }.toByteArray().also {
        require(it.size <= SUMH_HIDE_TRANSFER_STORAGE_BYTES) { "hide_transfer_storage_budget" }
    }

private fun DataOutputStream.transferText(value: String) {
    require(Charsets.UTF_8.newEncoder().canEncode(value)) { "hide_transfer_utf8_invalid" }
    val bytes = value.toByteArray(Charsets.UTF_8); writeInt(bytes.size); write(bytes)
}
private fun DataOutputStream.transferNullable(value: String?) {
    writeBoolean(value != null); if (value != null) transferText(value)
}
private fun DataInputStream.transferText(maximum: Int): String {
    val count = readInt()
    require(count in 0..maximum && count <= available()) { "hide_transfer_field_budget" }
    val bytes = ByteArray(count); readFully(bytes)
    return Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
        .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString()
}
private fun DataInputStream.transferBoolean(): Boolean = readUnsignedByte().let {
    require(it == 0 || it == 1) { "hide_transfer_boolean_invalid" }; it == 1
}
private fun DataInputStream.transferNullable(maximum: Int): String? = if (transferBoolean()) transferText(maximum) else null

internal fun encodeSUMHHideTransferDraft(value: SUMHHideTransferDraft): ByteArray {
    validateHideTransferDraft(value)
    return encodeHideTransfer { out ->
        out.writeInt(HIDE_TRANSFER_DRAFT_MAGIC); out.transferText(value.document)
        out.writeBoolean(value.preserveImportedEnabled)
        out.transferNullable(value.baselineRevision); out.transferNullable(value.baselineBootId)
        out.writeInt(value.choices.size)
        value.choices.toSortedMap().forEach { (id, choice) ->
            out.transferText(id); out.transferText(choice.resolution.name); out.transferNullable(choice.newId)
        }
    }
}

internal fun decodeSUMHHideTransferDraft(bytes: ByteArray): SUMHHideTransferDraft {
    require(bytes.size <= SUMH_HIDE_TRANSFER_STORAGE_BYTES) { "hide_transfer_storage_budget" }
    return DataInputStream(ByteArrayInputStream(bytes)).use { input ->
        require(input.readInt() == HIDE_TRANSFER_DRAFT_MAGIC) { "hide_transfer_draft_format" }
        val document = input.transferText(SUMH_HIDE_TRANSFER_BYTES); val preserve = input.transferBoolean()
        val revision = input.transferNullable(74); val boot = input.transferNullable(80)
        val count = input.readInt().also { require(it in 0..SUMH_HIDE_TRANSFER_MAX_RULES) }
        val choices = LinkedHashMap<String, SUMHHideTransferChoice>()
        repeat(count) {
            val id = input.transferText(32)
            val resolution = SUMHHideTransferResolution.valueOf(input.transferText(32))
            val choice = SUMHHideTransferChoice(resolution, input.transferNullable(32))
            require(choices.put(id, choice) == null) { "hide_transfer_duplicate_choice" }
        }
        require(input.available() == 0) { "hide_transfer_trailing_data" }
        SUMHHideTransferDraft(document, choices, preserve, revision, boot).also(::validateHideTransferDraft)
    }
}

internal fun encodeSUMHHideExportArtifact(value: SUMHHideExportArtifact): ByteArray {
    validateHideExportArtifact(value)
    return encodeHideTransfer { out ->
        out.writeInt(HIDE_TRANSFER_EXPORT_MAGIC); out.transferText(value.token); out.transferText(value.document)
        out.transferText(value.revision); out.transferText(value.scope); out.writeInt(value.count)
    }
}

internal fun decodeSUMHHideExportArtifact(bytes: ByteArray): SUMHHideExportArtifact {
    require(bytes.size <= SUMH_HIDE_TRANSFER_STORAGE_BYTES) { "hide_transfer_storage_budget" }
    return DataInputStream(ByteArrayInputStream(bytes)).use { input ->
        require(input.readInt() == HIDE_TRANSFER_EXPORT_MAGIC) { "hide_transfer_export_format" }
        val value = SUMHHideExportArtifact(input.transferText(32), input.transferText(SUMH_HIDE_TRANSFER_BYTES),
            input.transferText(74), input.transferText(8), input.readInt())
        require(input.available() == 0) { "hide_transfer_trailing_data" }
        value.also(::validateHideExportArtifact)
    }
}

/** Caller supplies an app-private no-backup directory. No backend action is performed by this store. */
internal class SUMHHideTransferStorage(private val directory: File,
    private val syncDirectory: (File) -> Unit = ::syncHideTransferDirectory) {
    private val draftFile get() = File(directory, "hide-transfer-v1.bin")
    private val exportFile get() = File(directory, "hide-export-v1.bin")

    fun readDraft(): SUMHHideTransferDraft? = synchronized(lock) {
        readBytes(draftFile)?.let(::decodeSUMHHideTransferDraft)
    }

    fun saveDraft(draft: SUMHHideTransferDraft, expectedFingerprint: String?): SUMHHideTransferDraft = synchronized(lock) {
        // Copy the caller's potentially mutable choice map before encoding or comparison.
        val snapshot = draft.copy(choices = draft.choices.toMap())
        val bytes = encodeSUMHHideTransferDraft(snapshot)
        val current = readDraft()
        require(current?.fingerprint == expectedFingerprint) { "hide_transfer_draft_changed" }
        replace(draftFile, bytes)
        snapshot
    }

    fun saveExport(document: String, revision: String, scope: String, count: Int): SUMHHideExportArtifact = synchronized(lock) {
        val artifact = SUMHHideExportArtifact(UUID.randomUUID().toString().replace("-", ""), document, revision, scope, count)
        val bytes = encodeSUMHHideExportArtifact(artifact)
        // Unknown/corrupt previous snapshots are preserved, just like drafts.
        readBytes(exportFile)?.let(::decodeSUMHHideExportArtifact)
        replace(exportFile, bytes)
        artifact
    }

    fun readExport(token: String): SUMHHideExportArtifact = synchronized(lock) {
        require(validHideDefinitionId(token)) { "hide_export_token_invalid" }
        val artifact = readBytes(exportFile)?.let(::decodeSUMHHideExportArtifact) ?: error("hide_export_missing")
        require(artifact.token == token) { "hide_export_snapshot_changed" }
        artifact
    }

    private fun checkDirectory() {
        require(!Files.isSymbolicLink(directory.toPath())) { "hide_transfer_directory_symlink" }
        if (Files.exists(directory.toPath(), LinkOption.NOFOLLOW_LINKS))
            require(Files.readAttributes(directory.toPath(), BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS).isDirectory) {
                "hide_transfer_directory_invalid"
            }
    }

    private fun readBytes(file: File): ByteArray? {
        checkDirectory()
        require(!Files.isSymbolicLink(file.toPath())) { "hide_transfer_file_symlink" }
        if (Files.notExists(directory.toPath()) || Files.notExists(file.toPath())) return null
        require(Files.readAttributes(file.toPath(), BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS).isRegularFile) {
            "hide_transfer_file_invalid"
        }
        return Files.newInputStream(file.toPath(), LinkOption.NOFOLLOW_LINKS).use { input ->
            val bytes = ByteArray(SUMH_HIDE_TRANSFER_STORAGE_BYTES + 1); var used = 0
            while (used < bytes.size) {
                val count = input.read(bytes, used, bytes.size - used)
                if (count < 0) break
                if (count == 0) throw IOException("Hide transfer read made no progress")
                used += count
            }
            require(used <= SUMH_HIDE_TRANSFER_STORAGE_BYTES) { "hide_transfer_storage_budget" }
            bytes.copyOf(used)
        }
    }

    private fun replace(file: File, bytes: ByteArray) {
        checkDirectory()
        require(!Files.isSymbolicLink(file.toPath())) { "hide_transfer_file_symlink" }
        if (!directory.isDirectory && !directory.mkdir()) throw IOException("Cannot create hide transfer directory")
        // Retry parent sync even when a previous unconfirmed mkdir left the directory visible.
        syncDirectory(requireNotNull(directory.parentFile))
        val temporary = File(directory, file.nameWithoutExtension + ".tmp")
        if (Files.exists(temporary.toPath(), LinkOption.NOFOLLOW_LINKS)) {
            require(Files.readAttributes(temporary.toPath(), BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS).isRegularFile) {
                "hide_transfer_scratch_invalid"
            }
            Files.delete(temporary.toPath()) // Never truncate a leftover hard link.
        }
        try {
            FileChannel.open(temporary.toPath(), StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE,
                LinkOption.NOFOLLOW_LINKS).use { channel ->
                val buffer = ByteBuffer.wrap(bytes)
                while (buffer.hasRemaining()) if (channel.write(buffer) <= 0) throw IOException("Hide transfer write made no progress")
                channel.force(true)
            }
            Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
            syncDirectory(directory)
            if (readBytes(file)?.contentEquals(bytes) != true) throw IOException("Hide transfer readback differs")
        } finally { temporary.delete() }
    }

    companion object { private val lock = Any() }
}

private fun syncHideTransferDirectory(directory: File) {
    val fd = Os.open(directory.absolutePath, OsConstants.O_RDONLY or OsConstants.O_NOFOLLOW or OsConstants.O_CLOEXEC, 0)
    try { require(OsConstants.S_ISDIR(Os.fstat(fd).st_mode)); Os.fsync(fd) } finally { Os.close(fd) }
}
