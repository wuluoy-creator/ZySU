package com.zying.zysu.ui.sumh.util

import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.bool
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.integer
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.objectValue
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.required
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.revision
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.text
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive

internal data class SUMHHideExportPage(val revision: String, val bootId: String, val sourceSchema: Int,
    val count: Int, val cursor: Int, val next: Int, val atEnd: Boolean, val complete: Boolean,
    val rules: List<SUMHHidePortableDefinition>)
internal data class SUMHHideTransferCapabilities(val export: Boolean, val import: Boolean)
internal fun parseSUMHHideTransferCapabilities(reply: SUMHQueryReply): SUMHHideTransferCapabilities {
    require(reply.error == null) { reply.error.orEmpty() }
    val root = SUMHControlStateParser.document(reply.stdout, 64 * 1024)
    fun supported(key: String) = root.get(key)?.let { value ->
        value.isJsonPrimitive && value.asJsonPrimitive.isNumber && value.asString.toIntOrNull() == 1
    } == true
    val export = supported("hide_definition_exchange_schema") && supported("hide_definition_export_page_schema")
    return SUMHHideTransferCapabilities(export, export && supported("hide_definition_import_schema"))
}
internal data class SUMHHideSavedDefinitions(val revision: String? = null, val bootId: String? = null,
    val rules: List<SUMHHidePortableDefinition> = emptyList(), val complete: Boolean = false,
    val error: String? = null, val bytes: Int = 0) {
    val canExport get() = complete && error == null && revision != null
    val canReviewMutation get() = canExport && !bootId.isNullOrEmpty()
}
internal fun parseSUMHHideExportPage(reply: SUMHQueryReply): SUMHHideExportPage {
    val root = SUMHControlStateParser.document(reply.stdout, 56 * 1024)
    require(root.keySet() == setOf("schema_version", "resource", "read_only", "runtime_verified", "scope", "boot_id", "status", "error",
        "revision", "source_schema", "count", "cursor", "next_cursor", "at_end", "complete", "items"))
    require(root.integer("schema_version", 1, Int.MAX_VALUE) == 1 && root.text("resource") == "user_hide_definition_export_page")
    require(root.bool("read_only") && !root.bool("runtime_verified") && root.text("scope") == "persistent_user_hide_definitions")
    require(reply.error == null && root.text("status") == "observed" && root.text("error").isEmpty()) {
        reply.error ?: "${root.text("status")}: ${root.text("error")}" }
    val revision = requireNotNull(root.revision("revision"))
    val boot = root.text("boot_id", 80).also { require(validHideDefinitionText(it, 80)) }
    val schema = root.integer("source_schema", 0, 1)
    val count = root.integer("count", 0, 4096); val cursor = root.integer("cursor", 0, count)
    val next = root.integer("next_cursor", cursor, count); val end = root.bool("at_end"); val complete = root.bool("complete")
    val items = root.required("items").also { require(it.isJsonArray && it.asJsonArray.size() <= 64) }
    val rules = items.asJsonArray.map { value -> value.objectValue().let { row ->
        require(row.keySet() == setOf("id", "path", "enabled", "name", "note", "group"))
        val id = row.text("id", 32).also { require(validHideDefinitionId(it)) }
        val path = row.text("path", 4095).also { require(it.startsWith('/') && validHideDefinitionText(it, 4095)) }
        val name = row.text("name", 128).also { require(validHideDefinitionText(it, 128)) }
        val note = row.text("note", 1024).also { require(validHideDefinitionText(it, 1024)) }
        val group = row.text("group", 128).also { require(validHideDefinitionText(it, 128)) }
        SUMHHidePortableDefinition(id, path, row.bool("enabled"), name, note, group)
    } }
    require(rules.map { it.id }.distinct().size == rules.size && rules.map { it.path }.distinct().size == rules.size)
    require(next == cursor + rules.size && end == (next == count) && complete == (cursor == 0 && end) && (end || next > cursor))
    return SUMHHideExportPage(revision, boot, schema, count, cursor, next, end, complete, rules)
}

/** Configuration-only export remains usable when kernel state or boot identity is unavailable. */
internal suspend fun collectSUMHHideSavedDefinitions(request: suspend (List<String>) -> SUMHQueryReply,
    progress: (Int) -> Unit = {}, clock: () -> Long = { System.nanoTime() / 1_000_000 }): SUMHHideSavedDefinitions {
    val started = clock(); var bytes = 0; var revision: String? = null; var boot: String? = null
    var schema = -1; var count = -1; var cursor = 0
    val rules = ArrayList<SUMHHidePortableDefinition>(); val ids = HashSet<String>(); val paths = HashSet<String>()
    return try {
        suspend fun page(args: List<String>): SUMHHideExportPage {
            currentCoroutineContext().ensureActive()
            require(clock() - started < 30_000) { "hide_export_deadline" }
            val reply = request(args); bytes += reply.stdout.toByteArray(Charsets.UTF_8).size
            require(bytes <= 8 * 1024 * 1024 && clock() - started < 30_000) { "hide_export_budget" }
            return parseSUMHHideExportPage(reply)
        }
        do {
            val next = page(if (revision == null) listOf("hide", "definitions") else listOf("hide", "definitions", revision, cursor.toString()))
            if (revision == null) { revision = next.revision; boot = next.bootId; schema = next.sourceSchema; count = next.count }
            require(next.revision == revision && next.bootId == boot && next.sourceSchema == schema && next.count == count && next.cursor == cursor) { "hide_export_changed" }
            require(next.rules.all { ids.add(it.id) && paths.add(it.path) }) { "hide_export_duplicate" }
            rules += next.rules; cursor = next.next; progress(rules.size)
        } while (!next.atEnd)
        val last = page(listOf("hide", "definitions", requireNotNull(revision), count.toString()))
        require(last.revision == revision && last.bootId == boot && last.sourceSchema == schema && last.count == count &&
            last.cursor == count && last.rules.isEmpty() && last.atEnd && rules.size == count) { "hide_export_final_changed" }
        SUMHHideSavedDefinitions(revision, boot, rules, true, bytes = bytes)
    } catch (error: Exception) {
        if (error is CancellationException) throw error
        SUMHHideSavedDefinitions(revision, boot, rules, false, error.message ?: "hide_export_unknown", bytes)
    }
}
