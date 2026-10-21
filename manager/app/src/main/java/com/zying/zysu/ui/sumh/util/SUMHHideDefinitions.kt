package com.zying.zysu.ui.sumh.util

import com.google.gson.JsonObject
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.bool
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.booleanValue
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.integer
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.nullableText
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.objectValue
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.required
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.revision
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.text
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.unsignedOrNull
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive

internal data class HideRuntime(val id: String, val generation: String, val management: Int, val binding: Int, val errno: Int)
internal data class HideDefinition(val id: String, val path: String, val enabled: Boolean, val name: String,
    val note: String, val group: String, val registered: Boolean?, val runtime: HideRuntime?, val state: String)
internal data class HideDefinitionPage(val revision: String, val bootId: String, val count: Int, val cursor: Int,
    val next: Int, val atEnd: Boolean, val complete: Boolean, val managed: Boolean?, val runtimeQuery: String,
    val rows: List<HideDefinition>)
internal data class HideDefinitionCollection(val revision: String? = null, val bootId: String? = null,
    val rows: List<HideDefinition> = emptyList(), val complete: Boolean = false, val managed: Boolean? = null,
    val error: String? = null, val bytes: Int = 0) {
    val canMutate: Boolean get() = complete && error == null && revision != null
}
internal data class HideMutationItem(val id: String, val outcome: String, val errno: Int, val membership: Boolean?, val runtime: HideRuntime?)
internal data class HideMutationReceipt(val action: String, val status: String, val saved: Boolean?,
    val revision: String?, val previousRevision: String?, val bootId: String, val reason: String, val error: String,
    val affected: Int, val confirmed: Int, val failed: Int, val unknown: Int, val itemsComplete: Boolean,
    val items: List<HideMutationItem>) {
    val succeeded: Boolean get() = saved == true && status in setOf("applied", "saved", "unchanged")
}
internal data class HideMutationResult(val value: HideMutationReceipt? = null, val error: String? = null,
    val operationId: String? = null) {
    val succeeded: Boolean get() = value?.succeeded == true && error == null
}
internal data class HideDefinitionBatch(val attempted: List<Pair<String, HideMutationResult>>, val unattempted: List<String>,
    val stoppedReason: String? = null) {
    val succeededCount get() = attempted.count { it.second.succeeded }
}

internal fun validHideDefinitionId(value: String) = value.matches(Regex("[0-9a-f]{32}"))
internal fun validHideDefinitionRevision(value: String) = value.matches(Regex("sha256-v1:[0-9a-f]{64}"))
internal fun validHideDefinitionText(value: String, max: Int) = '\u0000' !in value &&
    Charsets.UTF_8.newEncoder().canEncode(value) && value.toByteArray(Charsets.UTF_8).size <= max

internal object HideDefinitionsParser {
    /** The fixed native transport wraps a bounded dispatch, which wraps command stdout. */
    fun dispatch(raw: String): SUMHQueryReply {
        val outer = SUMHControlStateParser.reply(raw, 512 * 1024, 4 * 1024 * 1024)
        require(outer.error == null) { outer.error.orEmpty() }
        val value = SUMHControlStateParser.document(outer.stdout, 512 * 1024)
        require(value.integer("schema_version", 1, Int.MAX_VALUE) == 1 && value.text("resource") == "user_hide_dispatch")
        require(value.bool("stdout_complete") && value.bool("stderr_complete")) { "hide_dispatch_incomplete" }
        val code = value.integer("exit_code", Int.MIN_VALUE, Int.MAX_VALUE)
        val stderr = value.text("stderr", 64 * 1024)
        return SUMHQueryReply(value.text("stdout", 64 * 1024), if (code == 0) null else stderr.take(1500).ifEmpty { "hide_command_exit_$code" })
    }
    private fun JsonObject.nullableBool(key: String) = required(key).let { if (it.isJsonNull) null else it.booleanValue() }
    private fun runtime(value: JsonObject) = HideRuntime(
        requireNotNull(value.unsignedOrNull("id")).also { require(it != "0") },
        requireNotNull(value.unsignedOrNull("generation")), value.integer("management", 0, 1),
        value.integer("binding", 0, 3), value.integer("errno", -4095, 4095))
    private fun JsonObject.runtimeOrNull() = required("runtime").takeUnless { it.isJsonNull }?.objectValue()?.let(::runtime)
    fun page(reply: SUMHQueryReply): HideDefinitionPage {
        val root = SUMHControlStateParser.document(reply.stdout, 56 * 1024)
        require(root.integer("schema_version", 1, Int.MAX_VALUE) == 1 && root.text("resource") == "user_hide_definitions")
        require(root.bool("read_only") && !root.bool("runtime_verified") && root.text("runtime_consistency") == "per_rule_sampled")
        require(reply.error == null && root.text("status") == "observed") { reply.error ?: root.text("status") + ": " + root.text("error") }
        val revision = requireNotNull(root.revision("revision"))
        val boot = root.text("boot_id", 80).also { require(validHideDefinitionText(it, 80)) }
        val count = root.integer("count", 0, 4096); val cursor = root.integer("cursor", 0, count)
        val next = root.integer("next_cursor", cursor, count); val atEnd = root.bool("at_end"); val complete = root.bool("complete")
        val managed = root.nullableBool("managed_ownership")
        val query = root.text("runtime_query").also { require(it in setOf("observed", "unsupported", "unavailable")) }
        require(query != "observed" || managed == true)
        require(query != "unsupported" || managed == false)
        require(root.integer("source_schema", 0, 1) in 0..1)
        val array = root.required("items"); require(array.isJsonArray && array.asJsonArray.size() <= 16)
        val rows = array.asJsonArray.map { raw -> raw.objectValue().let { value ->
            val id = value.text("id", 32).also { require(validHideDefinitionId(it)) }
            val path = value.text("path", 4095).also { require(it.startsWith('/') && validHideDefinitionText(it, 4095)) }
            val enabled = value.bool("enabled")
            val name = value.text("name", 128); val note = value.text("note", 1024); val group = value.text("group", 128)
            require(validHideDefinitionText(name, 128) && validHideDefinitionText(note, 1024) && validHideDefinitionText(group, 128))
            val registered = value.nullableBool("registered"); val runtime = value.runtimeOrNull(); val state = value.text("state", 80)
            require((registered == null) == (query != "observed") && (runtime != null) == (registered == true))
            val expected = if (registered == null) "unknown" else if (!enabled) {
                if (registered) "disabled_pending_removal" else "disabled"
            } else if (!registered) "unregistered" else if (runtime?.management == 1) "globally_suspended"
            else when (runtime?.binding) { 2 -> "bound"; 3 -> "binding_error"; else -> "pending" }
            require(state == expected)
            HideDefinition(id, path, enabled, name, note, group, registered, runtime, state)
        } }
        require(rows.map { it.id }.distinct().size == rows.size && rows.map { it.path }.distinct().size == rows.size)
        require(next == cursor + rows.size && atEnd == (next == count) && complete == (cursor == 0 && atEnd))
        require(atEnd || next > cursor)
        return HideDefinitionPage(revision, boot, count, cursor, next, atEnd, complete, managed, query, rows)
    }
    fun mutation(reply: SUMHQueryReply, action: String, expectedRevision: String, expectedBoot: String? = null,
        expectedId: String? = null): HideMutationResult = try {
        val root = SUMHControlStateParser.document(reply.stdout, 64 * 1024)
        require(root.integer("schema_version", 1, Int.MAX_VALUE) == 1 && root.text("resource") == "user_hide_mutation")
        require(root.text("action") == action && !root.bool("runtime_verified") &&
            root.text("verification_scope") == "registered_membership_not_observer_visibility")
        val status = root.text("status"); require(status in setOf("rejected", "not_saved", "outcome_unknown", "saved", "unchanged", "applied", "partial"))
        require(action in setOf("create", "annotate", "enable", "disable", "enable-group", "disable-group", "retry", "remove-id", "import-id"))
        val saved = root.nullableBool("saved"); val revision = root.revision("revision"); val previous = root.revision("previous_revision")
        val boot = root.text("boot_id", 80)
        require(expectedBoot == null || boot == expectedBoot) { "hide_boot_changed" }
        if (saved == true) require(revision != null && previous == expectedRevision)
        if (status in setOf("applied", "partial", "saved", "unchanged")) require(saved == true)
        if (status in setOf("rejected", "not_saved")) require(saved == false && revision == null)
        if (status == "outcome_unknown") require(saved == null && revision == null)
        val affected = root.integer("affected_count", 0, 4096); val confirmed = root.integer("confirmed_count", 0, affected)
        val failed = root.integer("failed_count", 0, affected); val unknown = root.integer("unknown_count", 0, affected)
        val complete = root.bool("items_complete")
        val array = root.required("items"); require(array.isJsonArray && array.asJsonArray.size() <= 64)
        val items = array.asJsonArray.map { raw -> raw.objectValue().let { value ->
            val id = value.text("id", 32).also { require(validHideDefinitionId(it)) }
            val outcome = value.text("outcome", 80).also { require(it in setOf("mutation_failed_or_partial", "outcome_unknown", "runtime_differs", "absent", "registered_bound", "registered_binding_error", "registered_pending")) }
            val errno = value.integer("errno", 0, 4095); val membership = value.nullableBool("membership_matches"); val runtime = value.runtimeOrNull()
            if (outcome in setOf("absent", "registered_bound", "registered_binding_error", "registered_pending")) require(errno == 0 && membership == true)
            if (outcome == "absent") require(runtime == null)
            if (outcome.startsWith("registered_")) require(runtime != null)
            if (outcome == "registered_bound") require(runtime?.binding == 2)
            if (outcome == "registered_binding_error") require(runtime?.binding == 3)
            if (outcome == "registered_pending") require(runtime?.binding in listOf(0, 1))
            if (outcome == "outcome_unknown") require(errno == 0 && membership == null && runtime == null)
            if (outcome == "runtime_differs") require(errno == 0 && membership == false)
            if (outcome == "mutation_failed_or_partial") require(errno != 0)
            HideMutationItem(id, outcome, errno, membership, runtime)
        } }
        require(items.map { it.id }.distinct().size == items.size)
        if (status == "applied" || status == "partial") {
            require(action != "annotate" && affected > 0)
            require(confirmed + failed + unknown == affected && items.size == minOf(affected, 64) && complete == (affected <= 64))
            require((status == "applied") == (failed == 0 && unknown == 0))
            if (complete) require(items.count { it.outcome in setOf("absent", "registered_bound", "registered_binding_error", "registered_pending") } == confirmed &&
                items.count { it.outcome == "outcome_unknown" } == unknown &&
                items.count { it.outcome in setOf("mutation_failed_or_partial", "runtime_differs") } == failed)
        } else require(items.isEmpty() && confirmed == 0 && failed == 0 && unknown == 0 && complete)
        if (status == "saved" || status == "unchanged") require(action == "annotate")
        if (saved == true && action !in setOf("enable-group", "disable-group")) require(affected == 1)
        if (expectedId != null && items.isNotEmpty()) require(items.size == 1 && items.single().id == expectedId)
        if (status == "applied") {
            if (action in setOf("disable", "disable-group", "remove-id")) require(items.all { it.outcome == "absent" })
            if (action in setOf("enable", "enable-group", "retry")) require(items.all { it.outcome.startsWith("registered_") })
        }
        val receipt = HideMutationReceipt(action, status, saved, revision, previous, boot, root.text("reason"), root.text("error"),
            affected, confirmed, failed, unknown, complete, items)
        HideMutationResult(receipt, reply.error)
    } catch (error: Exception) { HideMutationResult(error = error.message ?: "hide_result_unknown") }
}

internal suspend fun collectHideDefinitions(request: suspend (List<String>) -> SUMHQueryReply,
    onProgress: (Int) -> Unit = {}, clock: () -> Long = { System.nanoTime() / 1_000_000 }): HideDefinitionCollection {
    val started = clock(); var revision: String? = null; var boot: String? = null; var count = -1; var managed: Boolean? = null
    var cursor = 0; var bytes = 0; val rows = ArrayList<HideDefinition>(); val ids = HashSet<String>(); val paths = HashSet<String>()
    return try {
        suspend fun read(args: List<String>): HideDefinitionPage {
            currentCoroutineContext().ensureActive()
            require(clock() - started < 30_000) { "hide_collection_deadline" }
            val reply = request(args); bytes += reply.stdout.toByteArray(Charsets.UTF_8).size
            require(bytes <= 8 * 1024 * 1024 && clock() - started < 30_000) { "hide_collection_budget" }
            return HideDefinitionsParser.page(reply)
        }
        do {
            val page = read(if (revision == null) listOf("hide", "status") else listOf("hide", "status", requireNotNull(revision), cursor.toString()))
            if (revision == null) { revision = page.revision; boot = page.bootId; count = page.count; managed = page.managed }
            require(page.revision == revision && page.bootId == boot && page.count == count && page.cursor == cursor && page.managed == managed) { "hide_collection_changed" }
            require(page.rows.all { ids.add(it.id) && paths.add(it.path) }) { "hide_duplicate_across_pages" }
            rows += page.rows; require(rows.size <= 4096); cursor = page.next; onProgress(rows.size)
        } while (!page.atEnd)
        // A final zero-length page checks the saved revision after the last page.
        val end = read(listOf("hide", "status", requireNotNull(revision), count.toString()))
        require(end.revision == revision && end.bootId == boot && end.count == count && end.cursor == count && end.rows.isEmpty() && end.atEnd && end.managed == managed)
        require(rows.size == count && !boot.isNullOrEmpty()) { "hide_collection_identity_unknown" }
        HideDefinitionCollection(revision, boot, rows, true, managed, bytes = bytes)
    } catch (error: Exception) {
        if (error is CancellationException) throw error
        HideDefinitionCollection(revision, boot, rows, false, managed, error.message ?: "hide_collection_unknown", bytes)
    }
}

internal suspend fun changeHideDefinitionSelection(action: String, ids: List<String>, initial: HideDefinitionCollection,
    request: suspend (List<String>, String, String) -> HideMutationResult,
    shouldContinue: () -> Boolean = { true }, onProgress: (Int, Int) -> Unit = { _, _ -> },
    clock: () -> Long = { System.nanoTime() / 1_000_000 }): HideDefinitionBatch {
    require(action in setOf("enable", "disable", "retry", "remove-id") && initial.canMutate && initial.managed == true)
    val selected = ids.distinct(); require(selected.isNotEmpty() && selected.size <= 4096)
    val byId = initial.rows.associateBy { it.id }; require(selected.all { it in byId })
    require(action != "retry" || selected.all { byId.getValue(it).enabled })
    val results = ArrayList<Pair<String, HideMutationResult>>(); var revision = requireNotNull(initial.revision)
    val boot = requireNotNull(initial.bootId); val started = clock(); var reason: String? = null
    onProgress(0, selected.size)
    for (id in selected) {
        if (!shouldContinue() || clock() - started >= 30_000) { reason = "hide_batch_stopped"; break }
        val received = try { request(listOf("hide", action, id, revision), revision, boot) }
            catch (error: Exception) { if (error is CancellationException) throw error; HideMutationResult(error = error.message ?: "hide_result_unknown") }
        val receipt = received.value
        val result = if (received.succeeded && (receipt?.previousRevision != revision || receipt.bootId != boot ||
                receipt.action != action || receipt.affected != 1 || receipt.items.singleOrNull()?.id != id ||
                receipt.revision == null)) received.copy(error = "hide_receipt_identity_unknown") else received
        results += id to result; onProgress(results.size, selected.size)
        if (!result.succeeded) {
            reason = "hide_batch_refresh_before_retry"; break
        }
        revision = requireNotNull(result.value?.revision)
    }
    return HideDefinitionBatch(results, selected.drop(results.size), reason)
}
