package com.zying.zysu.ui.sumh

import android.system.Os
import android.system.OsConstants
import com.zying.zysu.ui.sumh.util.*
import java.io.*
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.StandardCopyOption
import java.nio.file.StandardOpenOption
import java.nio.file.attribute.BasicFileAttributes
import java.util.UUID

internal data class SUMHProfilePending(val action: String, val id: String, val sourceId: String?, val bankRevision: String,
    val profileRevision: String?, val fingerprint: String, val phase: String = "prepared") {
    init {
        require(action in SUMH_PROFILE_MUTATIONS && validSUMHProfileId(id) && validSUMHProfileRevision(bankRevision))
        require(if (action == "clone") sourceId != null && validSUMHProfileId(sourceId) else sourceId == null)
        require(if (action in setOf("clone", "rename", "remove")) profileRevision != null && validSUMHProfileRevision(profileRevision) else profileRevision == null)
        require(fingerprint.matches(Regex("[0-9a-f]{64}")) && phase in setOf("prepared", "dispatching"))
    }
}
internal data class SUMHProfileDraft(val document: String = "", val resources: Set<SUMHProfileResource> = SUMHProfileResource.entries.toSet()) {
    val fingerprint by lazy { profileDigest(encodeSUMHProfileDraft(this)) }
}
internal data class SUMHProfileExport(val token: String, val document: String, val id: String, val revision: String)
internal const val SUMH_PROFILE_STORAGE_BYTES = SUMH_PROFILE_BYTES + 4096

private fun encodeProfile(magic: Int, write: (DataOutputStream) -> Unit): ByteArray = ByteArrayOutputStream().also { buffer ->
    DataOutputStream(buffer).use { it.writeInt(magic); write(it) }
}.toByteArray().also { require(it.size <= SUMH_PROFILE_STORAGE_BYTES) }
private fun DataOutputStream.profileText(value: String) { val bytes = profileUtf8(value); writeInt(bytes.size); write(bytes) }
private fun DataInputStream.profileText(max: Int): String {
    val count = readInt(); require(count in 0..max && count <= available()); return profileDecode(ByteArray(count).also(::readFully))
}
private fun <T> decodeProfile(bytes: ByteArray, magic: Int, read: (DataInputStream) -> T): T {
    require(bytes.size <= SUMH_PROFILE_STORAGE_BYTES)
    return DataInputStream(ByteArrayInputStream(bytes)).use { require(it.readInt() == magic); read(it).also { _ -> require(it.available() == 0) } }
}
internal fun encodeSUMHProfileDraft(value: SUMHProfileDraft): ByteArray {
    require(profileUtf8(value.document).size <= SUMH_PROFILE_BYTES)
    require(value.resources.size <= 4)
    return encodeProfile(0x53504431) { out -> out.profileText(value.document); out.writeInt(value.resources.size)
        SUMHProfileResource.entries.filter { it in value.resources }.forEach { out.profileText(it.wireName) } }
}
internal fun decodeSUMHProfileDraft(bytes: ByteArray): SUMHProfileDraft = decodeProfile(bytes, 0x53504431) { input ->
    val document = input.profileText(SUMH_PROFILE_BYTES); val count = input.readInt().also { require(it in 0..4) }
    val selected = List(count) { val wire = input.profileText(32); requireNotNull(SUMHProfileResource.entries.find { it.wireName == wire }) }
    require(selected.distinct().size == selected.size); SUMHProfileDraft(document, selected.toSet())
}
internal fun encodeSUMHProfilePending(value: SUMHProfilePending?): ByteArray = encodeProfile(0x53505031) { out ->
    out.writeByte(if (value == null) 0 else 1)
    if (value != null) listOf(value.action, value.id, value.sourceId.orEmpty(), value.bankRevision, value.profileRevision.orEmpty(), value.fingerprint, value.phase).forEach(out::profileText)
}.also { require(it.size <= 1024) }
internal fun decodeSUMHProfilePending(bytes: ByteArray): SUMHProfilePending? {
    require(bytes.size <= 1024)
    return decodeProfile(bytes, 0x53505031) { input -> when (input.readUnsignedByte()) {
        0 -> null
        1 -> SUMHProfilePending(input.profileText(32), input.profileText(64), input.profileText(64).ifEmpty { null }, input.profileText(74),
            input.profileText(74).ifEmpty { null }, input.profileText(64), input.profileText(16))
        else -> error("profile_checkpoint_invalid")
    } }
}
internal fun encodeSUMHProfileExport(value: SUMHProfileExport): ByteArray {
    require(value.token.matches(Regex("[0-9a-f]{32}")) && validSUMHProfileId(value.id) && validSUMHProfileRevision(value.revision))
    require(SUMHProfilesParser.revision(SUMHProfilesParser.document(value.document)) == value.revision)
    return encodeProfile(0x53504531) { out -> listOf(value.token, value.id, value.revision, value.document).forEach(out::profileText) }
}
internal fun decodeSUMHProfileExport(bytes: ByteArray): SUMHProfileExport = decodeProfile(bytes, 0x53504531) { input ->
    val token = input.profileText(32); val id = input.profileText(64); val revision = input.profileText(74)
    SUMHProfileExport(token, input.profileText(SUMH_PROFILE_BYTES), id, revision).also { encodeSUMHProfileExport(it) }
}

/** Dedicated noBackup child. Draft, immutable export and compact recovery hint have separate slots. */
internal open class SUMHProfileStorage(private val directory: File,
    private val syncDirectory: (File) -> Unit = ::syncProfileDirectory) {
    private fun file(name: String) = File(directory, "$name-v1.bin")
    open fun readDraft(): SUMHProfileDraft? = synchronized(lock) { read(file("draft"))?.let(::decodeSUMHProfileDraft) }
    open fun saveDraft(value: SUMHProfileDraft, expected: String?): SUMHProfileDraft = synchronized(lock) {
        val copy = value.copy(resources = value.resources.toSet()); val bytes = encodeSUMHProfileDraft(copy)
        require(readDraft()?.fingerprint == expected) { "profile_draft_changed" }; replace(file("draft"), bytes); copy
    }
    open fun readPending(): SUMHProfilePending? = synchronized(lock) { read(file("pending"), 1024)?.let(::decodeSUMHProfilePending) }
    open fun comparePending(expected: SUMHProfilePending?, replacement: SUMHProfilePending?): Boolean = synchronized(lock) {
        val bytes = encodeSUMHProfilePending(replacement)
        if (readPending() != expected) return@synchronized false
        replace(file("pending"), bytes); true
    }
    open fun saveExport(document: SUMHProfileDocument, id: String, revision: String): SUMHProfileExport = synchronized(lock) {
        read(file("export"))?.let(::decodeSUMHProfileExport) // Unknown versions are never silently replaced.
        val value = SUMHProfileExport(UUID.randomUUID().toString().replace("-", ""), document.text, id, revision)
        replace(file("export"), encodeSUMHProfileExport(value)); value
    }
    open fun readExport(token: String? = null): SUMHProfileExport? = synchronized(lock) {
        read(file("export"))?.let(::decodeSUMHProfileExport)?.also { require(token == null || token == it.token) { "profile_export_changed" } }
    }
    private fun checkDirectory() {
        require(!Files.isSymbolicLink(directory.toPath()))
        if (Files.exists(directory.toPath(), LinkOption.NOFOLLOW_LINKS)) require(Files.readAttributes(directory.toPath(), BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS).isDirectory)
    }
    private fun read(file: File, maximum: Int = SUMH_PROFILE_STORAGE_BYTES): ByteArray? {
        checkDirectory(); require(!Files.isSymbolicLink(file.toPath()))
        if (Files.notExists(directory.toPath()) || Files.notExists(file.toPath())) return null
        require(Files.readAttributes(file.toPath(), BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS).isRegularFile)
        return readSUMHProfileBytes(Files.newInputStream(file.toPath(), LinkOption.NOFOLLOW_LINKS), maximum)
    }
    private fun replace(file: File, bytes: ByteArray) {
        checkDirectory(); require(!Files.isSymbolicLink(file.toPath()))
        if (!directory.isDirectory) require(directory.mkdir())
        syncDirectory(requireNotNull(directory.parentFile))
        val stage = File(directory, file.name + ".stage")
        if (Files.exists(stage.toPath(), LinkOption.NOFOLLOW_LINKS)) {
            require(Files.readAttributes(stage.toPath(), BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS).isRegularFile)
            Files.delete(stage.toPath())
        }
        try {
            FileChannel.open(stage.toPath(), StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE, LinkOption.NOFOLLOW_LINKS).use { channel ->
                val buffer = ByteBuffer.wrap(bytes); while (buffer.hasRemaining()) require(channel.write(buffer) > 0); channel.force(true)
            }
            Files.move(stage.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
            syncDirectory(directory); require(read(file)?.contentEquals(bytes) == true)
        } finally { stage.delete() }
    }
    companion object { private val lock = Any() }
}
private fun syncProfileDirectory(directory: File) {
    val descriptor = Os.open(directory.absolutePath, OsConstants.O_RDONLY or OsConstants.O_NOFOLLOW or OsConstants.O_CLOEXEC, 0)
    try { require(OsConstants.S_ISDIR(Os.fstat(descriptor).st_mode)); Os.fsync(descriptor) } finally { Os.close(descriptor) }
}
