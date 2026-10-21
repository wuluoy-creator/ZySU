package com.zying.zysu.ui.sumh.util

import com.google.gson.JsonElement
import com.google.gson.JsonObject
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction

enum class SUMHInventoryCategory(val wire: String, val number: Int) {
    Metadata("metadata", 0), Mapping("mapping", 1), LegacyHide("legacy_hide", 2), Merge("merge", 3),
    ManagedMaps("maps_managed", 4), TemporaryMaps("maps_temporary", 5), Inject("inject", 6), UserHide("user_hide", 7),
    OverlayXattr("overlay_xattr", 8), Kstat("kstat", 9), Vnode("vnode", 10);
    val readable get() = number in 1..7
    fun supported(mask: Long) = number > 0 && mask and (1L shl (number - 1)) != 0L
}
/** UTF-8 is optional; the immutable hex value always preserves the exact byte string. */
data class SUMHInventoryBytes(val hex: String, val utf8: String?) { val size get() = hex.length / 2 }
enum class SUMHInventoryOwner { Unattributed, UserHide, ManagedMaps }
enum class SUMHInventoryIdKind { Ordinal, ManagedDefinition }
data class SUMHInventoryEntry(
    val category: SUMHInventoryCategory, val owner: SUMHInventoryOwner, val idKind: SUMHInventoryIdKind,
    val id: String, val definitionGeneration: String, val paths: Map<String, SUMHInventoryBytes>,
    val numbers: Map<String, String> = emptyMap(), val type: Long? = null, val storageManaged: Boolean? = null,
) { val key get() = "${category.wire}:$id" }
data class SUMHInventoryIdentity(val bootId: String, val instance: String, val generation: String, val supportedMask: Long)
enum class SUMHInventoryPageStatus { Known, Unsupported, Unavailable, Error, Stale }
data class SUMHInventoryPage(
    val status: SUMHInventoryPageStatus, val category: SUMHInventoryCategory, val bootId: String?,
    val reason: String, val errno: Int, val instance: String?, val generation: String?, val mask: Long?,
    val cursor: String?, val nextCursor: String?, val atEnd: Boolean, val entries: List<SUMHInventoryEntry>,
    val legacy: SUMHInventoryBytes? = null, val legacyErrno: Int? = null,
)
enum class SUMHInventoryRead { NotLoaded, Loading, Complete, Partial, Cancelled, Stale, Unsupported, Unavailable, Failed }
data class SUMHInventoryCollection(
    val status: SUMHInventoryRead = SUMHInventoryRead.NotLoaded,
    val entries: List<SUMHInventoryEntry> = emptyList(), val identity: SUMHInventoryIdentity? = null,
    val completedCategories: Set<SUMHInventoryCategory> = emptySet(), val unsupportedCategories: Set<SUMHInventoryCategory> = emptySet(),
    val requests: Int = 0, val responseBytes: Int = 0, val reason: String? = null,
    val legacy: SUMHInventoryBytes? = null, val legacyErrno: Int? = null,
)
data class SUMHInventoryBudget(val records: Int = 1024, val bytes: Int = 4 * 1024 * 1024, val milliseconds: Long = 30_000) {
    init { require(records in 1..1024 && bytes in 1..4 * 1024 * 1024 && milliseconds in 1..30_000) }
}

object SUMHInventoryParser {
    fun schema(reply: SUMHQueryReply): SUMHSchemaSupport {
        val root = SUMHControlStateParser.document(reply.stdout)
        if (reply.error != null) error(reply.error)
        val raw = root.get("rule_inventory_schema") ?: return SUMHSchemaSupport.NotAdvertised
        if (!raw.isJsonPrimitive || !raw.asJsonPrimitive.isNumber) return SUMHSchemaSupport.Invalid
        val number = raw.toString().toIntOrNull() ?: return SUMHSchemaSupport.Invalid
        return when { number == 1 -> SUMHSchemaSupport.Supported; number > 1 -> SUMHSchemaSupport.Future; else -> SUMHSchemaSupport.Invalid }
    }

    fun parse(text: String): SUMHInventoryPage {
        val root = SUMHControlStateParser.document(text, 1024 * 1024)
        require(root.number("schema_version", 1, Int.MAX_VALUE.toLong()) == 1L) { "Unsupported inventory format" }
        require(root.text("source") == "kernel_ioctl" && root.text("observation") == "registered_definitions_only")
        require(root.text("complete_scope") == "selected_category_from_cursor_zero" && root.number("page_capacity", 1, 1) == 1L)
        val category = category(root.text("category"))
        val status = when (root.text("status")) {
            "known" -> SUMHInventoryPageStatus.Known; "unsupported" -> SUMHInventoryPageStatus.Unsupported
            "unavailable" -> SUMHInventoryPageStatus.Unavailable; "error" -> SUMHInventoryPageStatus.Error
            "stale" -> SUMHInventoryPageStatus.Stale; else -> error("Unknown inventory status")
        }
        val reason = root.text("reason")
        val errno = root.number("errno", 0, 4095).toInt()
        val boot = root.required("boot_id").let { if (it.isJsonNull) null else root.text("boot_id", 80).also { id -> require(id.isNotEmpty() && '\u0000' !in id) } }
        val complete = root.bool("complete")
        val legacy = root.required("legacy")
        var legacyBytes: SUMHInventoryBytes? = null
        var legacyErrno: Int? = null
        if (!legacy.isJsonNull) {
            require(status == SUMHInventoryPageStatus.Unsupported && reason == "rule_inventory_not_advertised")
            val old = legacy.objectValue()
            require(!old.bool("complete") && old.text("scope") == "unstructured_legacy_text")
            legacyErrno = old.number("errno", 0, 4095).toInt()
            val raw = old.required("raw")
            if (legacyErrno == 0) legacyBytes = bytes(raw.objectValue(), 64 * 1024, path = false)
            else require(raw.isJsonNull)
        }
        val page = root.required("page")
        if (status != SUMHInventoryPageStatus.Known) {
            require(page.isJsonNull && !complete)
            return SUMHInventoryPage(status, category, boot, reason, errno, null, null, null, null, null, false,
                emptyList(), legacyBytes, legacyErrno)
        }
        require(errno == 0 && legacy.isJsonNull)
        val live = page.objectValue()
        require(live.text("consistency") == "definitions_coherent")
        val instance = live.unsigned("instance_id", true)
        val generation = live.unsigned("generation", true)
        val cursor = live.unsigned("cursor")
        val next = live.unsigned("next_cursor")
        val mask = live.number("supported_categories", 0, 0xffffffffL)
        val atEnd = live.bool("at_end")
        val rawEntries = live.required("entries")
        require(rawEntries.isJsonArray && rawEntries.asJsonArray.size() <= 1)
        val count = rawEntries.asJsonArray.size()
        require(count == 1 || atEnd)
        require(cursor.toULong() <= ULong.MAX_VALUE - count.toUInt() && next.toULong() == cursor.toULong() + count.toUInt())
        if (category == SUMHInventoryCategory.Metadata) require(count == 0 && cursor == "0" && atEnd)
        else require(category.readable && category.supported(mask)) { "Unsupported category data" }
        require(complete == (category != SUMHInventoryCategory.Metadata && cursor == "0" && atEnd))
        val entries = rawEntries.asJsonArray.map { entry(it.objectValue(), category, cursor) }
        return SUMHInventoryPage(status, category, boot, reason, errno, instance, generation, mask, cursor, next, atEnd, entries)
    }

    private fun entry(value: JsonObject, category: SUMHInventoryCategory, cursor: String): SUMHInventoryEntry {
        require(category(value.text("category")) == category && !value.bool("mutation_handle"))
        val owner = when (value.text("owner")) {
            "unattributed" -> SUMHInventoryOwner.Unattributed; "user_hide" -> SUMHInventoryOwner.UserHide
            "managed_maps" -> SUMHInventoryOwner.ManagedMaps; else -> error("Unknown owner")
        }
        require(owner == when (category) {
            SUMHInventoryCategory.UserHide -> SUMHInventoryOwner.UserHide
            SUMHInventoryCategory.ManagedMaps -> SUMHInventoryOwner.ManagedMaps
            else -> SUMHInventoryOwner.Unattributed
        }) { "Unexpected owner" }
        val kind = when (value.text("id_kind")) {
            "generation_ordinal" -> SUMHInventoryIdKind.Ordinal; "managed_definition" -> SUMHInventoryIdKind.ManagedDefinition
            else -> error("Unknown identity kind")
        }
        val id = value.unsigned("id", true)
        val def = value.unsigned("definition_generation")
        if (category == SUMHInventoryCategory.UserHide) require(kind == SUMHInventoryIdKind.ManagedDefinition && def != "0")
        else require(kind == SUMHInventoryIdKind.Ordinal && id.toULong() == cursor.toULong() + 1u && def == "0")
        val paths = linkedMapOf<String, SUMHInventoryBytes>()
        val numbers = linkedMapOf<String, String>()
        var type: Long? = null
        var storage: Boolean? = null
        when (category) {
            SUMHInventoryCategory.Mapping, SUMHInventoryCategory.Merge -> {
                for (key in listOf("visible_path", "backing_path")) paths[key] = bytes(value.required(key).objectValue())
                if (category == SUMHInventoryCategory.Mapping) type = value.number("type", 0, 0xffffffffL)
            }
            SUMHInventoryCategory.ManagedMaps, SUMHInventoryCategory.TemporaryMaps -> {
                paths["spoofed_path"] = bytes(value.required("spoofed_path").objectValue())
                for (key in listOf("target_ino", "target_dev", "spoofed_ino", "spoofed_dev")) numbers[key] = value.unsigned(key)
            }
            else -> {
                paths["path"] = bytes(value.required("path").objectValue())
                if (category == SUMHInventoryCategory.LegacyHide) storage = value.bool("storage_managed")
            }
        }
        return SUMHInventoryEntry(category, owner, kind, id, def, paths, numbers, type, storage)
    }

    internal fun bytes(value: JsonObject, maximum: Int = 4095, path: Boolean = true): SUMHInventoryBytes {
        val length = value.number("byte_length", 0, maximum.toLong()).toInt()
        val text = value.text("value", maximum * 2)
        val bytes: ByteArray
        val utf8: String?
        when (value.text("encoding")) {
            "utf-8" -> {
                val encoded = Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(text))
                bytes = ByteArray(encoded.remaining()).also(encoded::get)
                utf8 = text
            }
            "hex" -> {
                require(text.length % 2 == 0 && text.all { it in '0'..'9' || it in 'a'..'f' })
                bytes = ByteArray(text.length / 2) { index -> text.substring(index * 2, index * 2 + 2).toInt(16).toByte() }
                // Preserve the declared byte encoding; never decode with replacement characters.
                utf8 = null
            }
            else -> error("Unknown byte encoding")
        }
        require(bytes.size == length && (!path || bytes.none { it == 0.toByte() })) { "Invalid byte length or NUL path" }
        val digits = "0123456789abcdef"
        val hex = CharArray(bytes.size * 2) { index ->
            val byte = bytes[index / 2].toInt() and 255
            digits[if (index % 2 == 0) byte ushr 4 else byte and 15]
        }.concatToString()
        return SUMHInventoryBytes(hex, utf8)
    }
    private fun category(name: String) = SUMHInventoryCategory.entries.singleOrNull { it.wire == name } ?: error("Unknown category")
    private fun JsonElement.objectValue(): JsonObject { require(isJsonObject); return asJsonObject }
    private fun JsonObject.required(key: String): JsonElement = get(key) ?: error("Missing $key")
    private fun JsonObject.text(key: String, maximum: Int = 4096) = required(key).let {
        require(it.isJsonPrimitive && it.asJsonPrimitive.isString)
        it.asString.also { text -> require(text.length <= maximum) }
    }
    private fun JsonObject.bool(key: String) = required(key).let { require(it.isJsonPrimitive && it.asJsonPrimitive.isBoolean); it.asBoolean }
    private fun JsonObject.number(key: String, minimum: Long, maximum: Long): Long = required(key).let {
        require(it.isJsonPrimitive && it.asJsonPrimitive.isNumber)
        requireNotNull(it.toString().toLongOrNull()).also { value -> require(value in minimum..maximum) }
    }
    private fun JsonObject.unsigned(key: String, nonzero: Boolean = false) = text(key, 20).also {
        require(it.matches(Regex("0|[1-9][0-9]{0,19}")) && it.toULongOrNull() != null && (!nonzero || it != "0"))
    }
}

/** Manual collection. No retry/restart and no mutation; synchronous transport cancellation is checked between requests. */
internal suspend fun collectSUMHInventory(
    request: suspend (Array<String>) -> SUMHQueryReply,
    budget: SUMHInventoryBudget = SUMHInventoryBudget(),
    clockMs: () -> Long = { System.nanoTime() / 1_000_000 },
    onProgress: (SUMHInventoryCollection) -> Unit = {},
): SUMHInventoryCollection {
    var state = SUMHInventoryCollection(status = SUMHInventoryRead.Loading)
    val started = clockMs()
    val entries = mutableListOf<SUMHInventoryEntry>()
    val keys = hashSetOf<String>()
    val completed = linkedSetOf<SUMHInventoryCategory>()
    class BudgetExceeded(message: String) : Exception(message)
    suspend fun query(args: Array<String>): SUMHQueryReply {
        currentCoroutineContext().ensureActive()
        if (clockMs() - started >= budget.milliseconds) throw BudgetExceeded("time_budget")
        val response = request(args)
        currentCoroutineContext().ensureActive()
        val bytes = response.stdout.toByteArray(Charsets.UTF_8).size.toLong()
        val total = state.responseBytes.toLong() + bytes
        state = state.copy(requests = state.requests + 1)
        if (clockMs() - started >= budget.milliseconds) throw BudgetExceeded("time_budget")
        if (total > budget.bytes) throw BudgetExceeded("byte_budget")
        state = state.copy(responseBytes = total.toInt())
        return response
    }
    fun result(status: SUMHInventoryRead, reason: String? = null) = state.copy(status = status,
        entries = entries.toList(), completedCategories = completed.toSet(), reason = reason)
    fun rejected(page: SUMHInventoryPage, transport: String?) = result(when (page.status) {
        SUMHInventoryPageStatus.Stale -> SUMHInventoryRead.Stale
        SUMHInventoryPageStatus.Unsupported -> SUMHInventoryRead.Unsupported
        SUMHInventoryPageStatus.Unavailable -> SUMHInventoryRead.Unavailable
        else -> SUMHInventoryRead.Failed
    }, transport ?: "${page.reason} (errno=${page.errno})").copy(legacy = page.legacy, legacyErrno = page.legacyErrno)
    fun validate(page: SUMHInventoryPage, category: SUMHInventoryCategory, cursor: String) {
        val identity = requireNotNull(state.identity)
        require(page.category == category && page.cursor == cursor && page.bootId == identity.bootId &&
            page.instance == identity.instance && page.generation == identity.generation && page.mask == identity.supportedMask) {
            "Page identity, category, cursor or capability mask changed"
        }
    }
    try {
        when (SUMHInventoryParser.schema(query(arrayOf("api", "meta", "--control-only")))) {
            SUMHSchemaSupport.Supported -> Unit
            SUMHSchemaSupport.NotAdvertised -> return result(SUMHInventoryRead.Unsupported, "controller_schema_not_advertised")
            SUMHSchemaSupport.Future -> return result(SUMHInventoryRead.Unavailable, "future_inventory_schema")
            else -> return result(SUMHInventoryRead.Failed, "invalid_inventory_schema")
        }
        val metadataReply = query(arrayOf("api", "inventory"))
        val metadata = SUMHInventoryParser.parse(metadataReply.stdout)
        if (metadata.status != SUMHInventoryPageStatus.Known || metadataReply.error != null) return rejected(metadata, metadataReply.error)
        require(metadata.category == SUMHInventoryCategory.Metadata && !metadata.bootId.isNullOrEmpty()) { "Missing collection boot identity" }
        state = state.copy(identity = SUMHInventoryIdentity(metadata.bootId, requireNotNull(metadata.instance),
            requireNotNull(metadata.generation), requireNotNull(metadata.mask)),
            unsupportedCategories = SUMHInventoryCategory.entries.filter { it.number > 0 && !it.supported(metadata.mask) }.toSet())
        val identity = requireNotNull(state.identity)
        for (category in SUMHInventoryCategory.entries.filter { it.readable && it.supported(identity.supportedMask) }) {
            var cursor = "0"
            while (true) {
                val reply = query(arrayOf("api", "inventory", category.wire, identity.instance, identity.generation, cursor))
                val page = SUMHInventoryParser.parse(reply.stdout)
                if (page.status != SUMHInventoryPageStatus.Known || reply.error != null) return rejected(page, reply.error)
                validate(page, category, cursor)
                if (entries.size + page.entries.size > budget.records) throw BudgetExceeded("record_budget")
                for (entry in page.entries) { require(keys.add(entry.key)) { "Duplicate runtime identity" }; entries.add(entry) }
                if (page.atEnd) completed.add(category)
                onProgress(result(SUMHInventoryRead.Loading))
                if (page.atEnd) break
                cursor = requireNotNull(page.nextCursor)
            }
        }
        val finalReply = query(arrayOf("api", "inventory", "metadata", identity.instance, identity.generation, "0"))
        val final = SUMHInventoryParser.parse(finalReply.stdout)
        if (final.status != SUMHInventoryPageStatus.Known || finalReply.error != null) return rejected(final, finalReply.error)
        validate(final, SUMHInventoryCategory.Metadata, "0")
        val unreadMask = identity.supportedMask and 0x7fL.inv()
        return result(if (unreadMask == 0L) SUMHInventoryRead.Complete else SUMHInventoryRead.Partial,
            if (unreadMask == 0L) null else "additional_categories_not_understood")
    } catch (error: kotlinx.coroutines.CancellationException) { throw error }
    catch (error: BudgetExceeded) { return result(SUMHInventoryRead.Partial, error.message) }
    catch (error: Exception) { return result(SUMHInventoryRead.Failed, error.message ?: "Invalid inventory response") }
}
