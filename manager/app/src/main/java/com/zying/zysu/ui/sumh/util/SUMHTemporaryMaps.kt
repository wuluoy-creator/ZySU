package com.zying.zysu.ui.sumh.util

import com.google.gson.JsonElement
import com.google.gson.JsonObject
import com.zying.zysu.Natives
import com.zying.zysu.ui.util.getKsud
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.nio.ByteBuffer
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction

internal const val SUMH_TEMPORARY_MAPS_PAYLOAD_BYTES = 56 * 1024
internal const val SUMH_TEMPORARY_MAPS_ENVELOPE_BYTES = 4 * SUMH_TEMPORARY_MAPS_PAYLOAD_BYTES
internal const val SUMH_TEMPORARY_MAPS_MAX_ROWS = 1024
internal const val SUMH_TEMPORARY_MAPS_COLLECTION_BYTES = 4 * 1024 * 1024

internal enum class SUMHTemporaryMapsSupport { Supported, Unsupported, Unknown }
internal data class SUMHTemporaryMapsCapabilities(
    val inventory: SUMHTemporaryMapsSupport = SUMHTemporaryMapsSupport.Unknown,
    val delete: SUMHTemporaryMapsSupport = SUMHTemporaryMapsSupport.Unknown,
    val errno: Int = 0, val error: String? = null,
)

/** This local inventory token deliberately has no sampled or inferred boot identity. */
internal data class SUMHTemporaryMapsToken(val instance: String, val generation: String, val mask: Long) {
    init {
        temporaryMapsUnsigned(instance, nonzero = true)
        temporaryMapsUnsigned(generation, nonzero = true)
        require(mask in 0..0xffffffffL && SUMHInventoryCategory.TemporaryMaps.supported(mask))
    }
}
internal data class SUMHTemporaryMapsCursor(val token: SUMHTemporaryMapsToken, val cursor: String) {
    init { temporaryMapsUnsigned(cursor) }
}
internal data class SUMHTemporaryMapsRow(
    val ordinal: String, val targetIno: String, val targetDev: String,
    val spoofedIno: String, val spoofedDev: String, val spoofedPath: SUMHInventoryBytes,
) {
    init {
        temporaryMapsUnsigned(ordinal, nonzero = true)
        temporaryMapsUnsigned(targetIno); temporaryMapsUnsigned(targetDev, device = true)
        temporaryMapsUnsigned(spoofedIno); temporaryMapsUnsigned(spoofedDev, device = true)
        require(spoofedPath.hex.length <= 8190 && spoofedPath.hex.length % 2 == 0 &&
            spoofedPath.hex.all { it in '0'..'9' || it in 'a'..'f' })
        require(spoofedPath.hex.chunked(2).none { it == "00" })
        spoofedPath.utf8?.let { path ->
            val encoded = temporaryMapsUtf8(path)
            require(encoded.joinToString("") { "%02x".format(it.toInt() and 255) } == spoofedPath.hex)
        }
    }
    val key: String get() = "$targetIno:$targetDev"
}
internal data class SUMHTemporaryMapsPageResult(
    val page: SUMHInventoryPage? = null, val responseBytes: Int = 0, val error: String? = null,
)
internal data class SUMHTemporaryMapsSnapshot(
    val rows: List<SUMHTemporaryMapsRow>, val token: SUMHTemporaryMapsToken,
    val nextCursor: String?, val complete: Boolean, val responseBytes: Int,
) {
    val continuation: SUMHTemporaryMapsCursor? get() = nextCursor?.let { SUMHTemporaryMapsCursor(token, it) }
}

/** Manual append only. A failure leaves the caller's previous immutable snapshot untouched. */
internal fun appendSUMHTemporaryMapsPage(
    previous: SUMHTemporaryMapsSnapshot?, result: SUMHTemporaryMapsPageResult,
): SUMHTemporaryMapsSnapshot {
    require(result.error == null) { result.error ?: "temporary_maps_query_failed" }
    require(result.responseBytes in 1..SUMH_TEMPORARY_MAPS_ENVELOPE_BYTES)
    val page = requireNotNull(result.page) { "temporary_maps_missing_page" }
    require(page.status == SUMHInventoryPageStatus.Known) { "${page.reason} (errno=${page.errno})" }
    validateTemporaryMapsPage(page)
    val token = SUMHTemporaryMapsToken(requireNotNull(page.instance), requireNotNull(page.generation), requireNotNull(page.mask))
    if (previous == null) require(page.cursor == "0") { "temporary_maps_first_cursor" }
    else {
        require(!previous.complete && previous.nextCursor != null) { "temporary_maps_already_complete" }
        require(token == previous.token && page.cursor == previous.nextCursor) { "temporary_maps_page_changed" }
    }
    val oldRows = previous?.rows.orEmpty()
    val added = page.entries.map(::temporaryMapsRow)
    require(oldRows.size + added.size <= SUMH_TEMPORARY_MAPS_MAX_ROWS) { "temporary_maps_record_budget" }
    val totalBytes = (previous?.responseBytes ?: 0).toLong() + result.responseBytes
    require(totalBytes <= SUMH_TEMPORARY_MAPS_COLLECTION_BYTES) { "temporary_maps_byte_budget" }
    val rows = oldRows + added
    require(rows.map { it.key }.toSet().size == rows.size) { "temporary_maps_duplicate_key" }
    require(rows.map { it.ordinal }.toSet().size == rows.size &&
        rows.withIndex().all { (index, row) -> row.ordinal == (index + 1).toString() }) { "temporary_maps_ordinal" }
    require(page.nextCursor == rows.size.toString()) { "temporary_maps_cursor_count" }
    return SUMHTemporaryMapsSnapshot(rows, token, if (page.atEnd) null else page.nextCursor, page.atEnd, totalBytes.toInt())
}

internal data class SUMHTemporaryMapsRemoveRequest(val token: SUMHTemporaryMapsToken, val row: SUMHTemporaryMapsRow)
internal enum class SUMHTemporaryMapsRemoveStatus { Deleted, NotApplied, Unknown }
internal data class SUMHTemporaryMapsRemoveResult(
    val status: SUMHTemporaryMapsRemoveStatus, val errorCode: String, val errno: Int? = null,
    val attempted: Boolean? = null, val acknowledged: Boolean = false, val error: String? = null,
)

internal interface SUMHTemporaryMapsService {
    suspend fun capabilities(): SUMHTemporaryMapsCapabilities
    suspend fun list(cursor: SUMHTemporaryMapsCursor? = null): SUMHTemporaryMapsPageResult
    suspend fun remove(request: SUMHTemporaryMapsRemoveRequest): SUMHTemporaryMapsRemoveResult
}

/** The only production transport is the bounded private JNI channel. No shell, retry or journal. */
internal class NativeSUMHTemporaryMapsService(
    private val transport: suspend (ByteArray) -> ByteArray = { request ->
        withContext(Dispatchers.IO) { Natives.sumhTemporaryMaps(getKsud().toByteArray(Charsets.UTF_8), request) }
    },
) : SUMHTemporaryMapsService {
    override suspend fun capabilities(): SUMHTemporaryMapsCapabilities = try {
        SUMHTemporaryMapsParser.capabilities(transport(SUMHTemporaryMapsParser.capabilitiesRequest()))
    } catch (cancelled: CancellationException) { throw cancelled }
    catch (failure: Exception) { SUMHTemporaryMapsCapabilities(error = failure.message ?: "temporary_maps_capability_query_failed") }
    catch (failure: LinkageError) { SUMHTemporaryMapsCapabilities(error = failure.message ?: "temporary_maps_transport_unavailable") }

    override suspend fun list(cursor: SUMHTemporaryMapsCursor?): SUMHTemporaryMapsPageResult = try {
        SUMHTemporaryMapsParser.page(transport(SUMHTemporaryMapsParser.listRequest(cursor)), cursor)
    } catch (cancelled: CancellationException) { throw cancelled }
    catch (failure: Exception) { SUMHTemporaryMapsPageResult(error = failure.message ?: "temporary_maps_query_failed") }
    catch (failure: LinkageError) { SUMHTemporaryMapsPageResult(error = failure.message ?: "temporary_maps_transport_unavailable") }

    override suspend fun remove(request: SUMHTemporaryMapsRemoveRequest): SUMHTemporaryMapsRemoveResult = try {
        SUMHTemporaryMapsParser.remove(transport(SUMHTemporaryMapsParser.removeRequest(request)), request)
    } catch (failure: Exception) {
        // Cancellation or a lost reply cannot establish whether publication occurred.
        SUMHTemporaryMapsRemoveResult(SUMHTemporaryMapsRemoveStatus.Unknown, "outcome_unknown",
            error = failure.message ?: "temporary_maps_result_unknown")
    } catch (failure: LinkageError) {
        SUMHTemporaryMapsRemoveResult(SUMHTemporaryMapsRemoveStatus.Unknown, "outcome_unknown",
            error = failure.message ?: "temporary_maps_transport_unavailable")
    }
}

internal object SUMHTemporaryMapsParser {
    fun capabilitiesRequest(): ByteArray = request("capabilities")
    fun listRequest(cursor: SUMHTemporaryMapsCursor? = null): ByteArray = request("list") {
        cursor?.let {
            addProperty("instance_id", it.token.instance); addProperty("generation", it.token.generation)
            addProperty("cursor", it.cursor)
        }
    }
    fun removeRequest(request: SUMHTemporaryMapsRemoveRequest): ByteArray = this.request("remove") {
        addProperty("instance_id", request.token.instance); addProperty("generation", request.token.generation)
        addProperty("target_ino", request.row.targetIno); addProperty("target_dev", request.row.targetDev)
    }

    fun capabilities(raw: ByteArray): SUMHTemporaryMapsCapabilities {
        val dispatch = dispatch(raw, "capabilities")
        val value = dispatch.result
        value.fields("schema_version", "resource", "inventory", "delete", "errno")
        require(value.integer("schema_version", 1, Int.MAX_VALUE) == 1 && value.text("resource") == "temporary_maps_capabilities")
        fun support(key: String) = when (value.text(key)) {
            "supported" -> SUMHTemporaryMapsSupport.Supported
            "unsupported" -> SUMHTemporaryMapsSupport.Unsupported
            "unknown" -> SUMHTemporaryMapsSupport.Unknown
            else -> error("temporary_maps_invalid_support")
        }
        val inventory = support("inventory"); val delete = support("delete")
        val errno = value.integer("errno", 0, 4095)
        if (inventory == SUMHTemporaryMapsSupport.Unknown || delete == SUMHTemporaryMapsSupport.Unknown) {
            require(inventory == SUMHTemporaryMapsSupport.Unknown && delete == SUMHTemporaryMapsSupport.Unknown && errno > 0 && dispatch.exit == 1)
        } else {
            require(errno == 0 && dispatch.exit == 0)
            require(delete != SUMHTemporaryMapsSupport.Supported || inventory == SUMHTemporaryMapsSupport.Supported)
        }
        return SUMHTemporaryMapsCapabilities(inventory, delete, errno)
    }

    fun page(raw: ByteArray, cursor: SUMHTemporaryMapsCursor? = null): SUMHTemporaryMapsPageResult {
        val dispatch = dispatch(raw, "list")
        val value = dispatch.result
        value.fields("schema_version", "status", "reason", "errno", "source", "observation", "boot_id", "category",
            "page_capacity", "complete", "complete_scope", "page", "legacy")
        require(value.required("boot_id").isJsonNull && value.required("legacy").isJsonNull)
        val live = value.required("page")
        if (!live.isJsonNull) {
            val item = live.objectValue()
            item.fields("instance_id", "generation", "cursor", "next_cursor", "at_end", "consistency", "supported_categories", "entries")
            val entries = item.required("entries").also { require(it.isJsonArray) }.asJsonArray
            for (rawEntry in entries) {
                val entry = rawEntry.objectValue()
                entry.fields("category", "owner", "id_kind", "id", "definition_generation", "mutation_handle",
                    "target_ino", "target_dev", "spoofed_ino", "spoofed_dev", "spoofed_path")
                entry.required("spoofed_path").objectValue().fields("encoding", "value", "byte_length")
            }
        }
        val page = SUMHInventoryParser.parse(value.toString())
        validateTemporaryMapsPage(page)
        require(dispatch.exit == if (page.status == SUMHInventoryPageStatus.Known ||
            page.status == SUMHInventoryPageStatus.Unsupported && page.errno == 0) 0 else 1)
        if (page.status == SUMHInventoryPageStatus.Known) {
            require(page.cursor == (cursor?.cursor ?: "0")) { "temporary_maps_cursor_changed" }
            cursor?.let {
                require(page.instance == it.token.instance && page.generation == it.token.generation && page.mask == it.token.mask) {
                    "temporary_maps_token_changed"
                }
            }
        }
        return SUMHTemporaryMapsPageResult(page, raw.size)
    }

    fun remove(raw: ByteArray, request: SUMHTemporaryMapsRemoveRequest): SUMHTemporaryMapsRemoveResult {
        val dispatch = dispatch(raw, "remove")
        val value = dispatch.result
        value.fields("schema_version", "resource", "action", "status", "error_code", "errno", "saved", "ioctl_attempted",
            "acknowledged", "operation_id", "history_supported", "observer_verified", "request")
        require(value.integer("schema_version", 1, Int.MAX_VALUE) == 1 && value.text("resource") == "temporary_maps" &&
            value.text("action") == "remove")
        require(!value.bool("saved") && !value.bool("history_supported") && !value.bool("observer_verified") &&
            value.required("operation_id").isJsonNull)
        val echoed = value.required("request").objectValue()
        echoed.fields("target_ino", "target_dev", "expected_instance_id", "expected_generation")
        require(echoed.unsigned("target_ino") == request.row.targetIno &&
            echoed.unsigned("target_dev", device = true) == request.row.targetDev &&
            echoed.unsigned("expected_instance_id", nonzero = true) == request.token.instance &&
            echoed.unsigned("expected_generation", nonzero = true) == request.token.generation) { "temporary_maps_receipt_mismatch" }
        val status = when (value.text("status")) {
            "deleted" -> SUMHTemporaryMapsRemoveStatus.Deleted
            "not_applied" -> SUMHTemporaryMapsRemoveStatus.NotApplied
            "outcome_unknown" -> SUMHTemporaryMapsRemoveStatus.Unknown
            else -> error("temporary_maps_invalid_result")
        }
        val errno = value.integer("errno", 0, 4095)
        val attempted = value.bool("ioctl_attempted"); val acknowledged = value.bool("acknowledged")
        val errorCode = value.text("error_code")
        val prepublicationErrors = setOf(116, 2, 22, 75, 12, 108, 14, 95, 25, 13, 1)
        require(errorCode == when (errno) {
            0 -> "deleted"; 116 -> "snapshot_changed"; 2 -> if (attempted) "temporary_key_not_found" else "capability_query_failed"
            95, 25 -> "unsupported"; 22 -> "invalid_request"; 75 -> "key_not_representable"
            12 -> "insufficient_memory"; 13, 1 -> "permission_denied"; 108 -> "runtime_stopped"; 14 -> "invalid_user_memory"
            else -> if (attempted) "outcome_unknown" else "capability_query_failed"
        })
        when (status) {
            SUMHTemporaryMapsRemoveStatus.Deleted -> require(errno == 0 && attempted && acknowledged && dispatch.exit == 0)
            SUMHTemporaryMapsRemoveStatus.NotApplied -> require(errno > 0 && !acknowledged && dispatch.exit == 1 &&
                (!attempted || errno in prepublicationErrors))
            SUMHTemporaryMapsRemoveStatus.Unknown -> require(errno > 0 && attempted && !acknowledged && dispatch.exit == 1 &&
                errno !in prepublicationErrors)
        }
        return SUMHTemporaryMapsRemoveResult(status, errorCode, errno, attempted, acknowledged)
    }

    private data class Dispatch(val exit: Int, val result: JsonObject)
    private fun dispatch(raw: ByteArray, action: String): Dispatch {
        require(raw.size <= SUMH_TEMPORARY_MAPS_ENVELOPE_BYTES) { "temporary_maps_envelope_budget" }
        val text = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(raw)).toString()
        // The private transport envelope is validated before trusting any callback output.
        val envelope = SUMHControlStateParser.document(text, SUMH_TEMPORARY_MAPS_ENVELOPE_BYTES)
        envelope.fields("ok", "exit_code", "errno", "stdout", "stderr")
        require(envelope.bool("ok") && envelope.integer("exit_code", Int.MIN_VALUE, Int.MAX_VALUE) == 0 &&
            envelope.integer("errno", Int.MIN_VALUE, Int.MAX_VALUE) == 0) { "temporary_maps_transport_failed" }
        envelope.text("stderr", SUMH_TEMPORARY_MAPS_PAYLOAD_BYTES)
        val stdout = envelope.text("stdout", SUMH_TEMPORARY_MAPS_PAYLOAD_BYTES)
        val value = SUMHControlStateParser.document(stdout, SUMH_TEMPORARY_MAPS_PAYLOAD_BYTES)
        validateStrings(value)
        value.fields("schema_version", "resource", "action", "exit_code", "result")
        require(value.integer("schema_version", 1, Int.MAX_VALUE) == 1 && value.text("resource") == "temporary_maps_dispatch" &&
            value.text("action") == action)
        return Dispatch(value.integer("exit_code", 0, 1), value.required("result").objectValue())
    }

    private fun validateStrings(value: JsonElement) {
        when {
            value.isJsonObject -> value.asJsonObject.entrySet().forEach { (key, child) ->
                temporaryMapsUtf8(key); require('\u0000' !in key); validateStrings(child)
            }
            value.isJsonArray -> value.asJsonArray.forEach(::validateStrings)
            value.isJsonPrimitive && value.asJsonPrimitive.isString -> {
                temporaryMapsUtf8(value.asString); require('\u0000' !in value.asString)
            }
        }
    }

    private fun request(action: String, fields: JsonObject.() -> Unit = {}): ByteArray = JsonObject().apply {
        addProperty("schema_version", 1); addProperty("action", action); fields()
    }.toString().let(::temporaryMapsUtf8).also { require(it.size <= 1024) }
    private fun JsonObject.fields(vararg keys: String) { require(keySet() == keys.toSet()) { "temporary_maps_unexpected_fields" } }
    private fun JsonElement.objectValue(): JsonObject { require(isJsonObject); return asJsonObject }
    private fun JsonObject.required(key: String): JsonElement = get(key) ?: error("temporary_maps_missing_$key")
    private fun JsonObject.text(key: String, maximum: Int = 256): String = required(key).let {
        require(it.isJsonPrimitive && it.asJsonPrimitive.isString)
        it.asString.also { text -> require(temporaryMapsUtf8(text).size <= maximum && '\u0000' !in text) }
    }
    private fun JsonObject.bool(key: String): Boolean = required(key).let {
        require(it.isJsonPrimitive && it.asJsonPrimitive.isBoolean); it.asBoolean
    }
    private fun JsonObject.integer(key: String, minimum: Int, maximum: Int): Int = required(key).let {
        require(it.isJsonPrimitive && it.asJsonPrimitive.isNumber)
        requireNotNull(it.toString().toIntOrNull()).also { number -> require(number in minimum..maximum) }
    }
    private fun JsonObject.unsigned(key: String, nonzero: Boolean = false, device: Boolean = false) =
        text(key, 20).also { temporaryMapsUnsigned(it, nonzero, device) }
}

private fun temporaryMapsUnsigned(value: String, nonzero: Boolean = false, device: Boolean = false) {
    require(value.matches(Regex("0|[1-9][0-9]{0,19}")))
    val parsed = requireNotNull(value.toULongOrNull())
    require((!nonzero || parsed > 0u) && (!device || parsed <= UInt.MAX_VALUE.toULong()))
}
private fun temporaryMapsUtf8(value: String): ByteArray {
    val buffer = Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
        .onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(value))
    return ByteArray(buffer.remaining()).also(buffer::get)
}
private fun temporaryMapsRow(entry: SUMHInventoryEntry): SUMHTemporaryMapsRow {
    require(entry.category == SUMHInventoryCategory.TemporaryMaps && entry.owner == SUMHInventoryOwner.Unattributed &&
        entry.idKind == SUMHInventoryIdKind.Ordinal && entry.definitionGeneration == "0" && entry.type == null && entry.storageManaged == null)
    require(entry.paths.keys == setOf("spoofed_path") && entry.numbers.keys == setOf("target_ino", "target_dev", "spoofed_ino", "spoofed_dev"))
    return SUMHTemporaryMapsRow(entry.id, entry.numbers.getValue("target_ino"), entry.numbers.getValue("target_dev"),
        entry.numbers.getValue("spoofed_ino"), entry.numbers.getValue("spoofed_dev"), entry.paths.getValue("spoofed_path"))
}
private fun validateTemporaryMapsPage(page: SUMHInventoryPage) {
    require(page.category == SUMHInventoryCategory.TemporaryMaps && page.bootId == null && page.legacy == null && page.legacyErrno == null)
    page.entries.forEach(::temporaryMapsRow)
    if (page.status == SUMHInventoryPageStatus.Known) {
        require(page.errno == 0 && page.entries.size <= 1 && (page.entries.isNotEmpty() || page.atEnd))
        val cursor = requireNotNull(page.cursor); val next = requireNotNull(page.nextCursor)
        temporaryMapsUnsigned(cursor); temporaryMapsUnsigned(next)
        require(cursor.toULong() <= ULong.MAX_VALUE - page.entries.size.toUInt() &&
            next.toULong() == cursor.toULong() + page.entries.size.toUInt())
        require(page.entries.all { it.id.toULong() == cursor.toULong() + 1u })
        SUMHTemporaryMapsToken(requireNotNull(page.instance), requireNotNull(page.generation), requireNotNull(page.mask))
    } else require(page.entries.isEmpty())
}
