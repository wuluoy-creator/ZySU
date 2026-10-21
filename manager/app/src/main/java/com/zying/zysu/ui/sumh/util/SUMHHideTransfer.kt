package com.zying.zysu.ui.sumh.util

import com.google.gson.Strictness
import com.google.gson.stream.JsonReader
import com.google.gson.stream.JsonToken
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import java.io.InputStream
import java.io.OutputStream
import java.io.StringReader
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.security.MessageDigest

internal const val SUMH_HIDE_STORE_BYTES = 4 * 1024 * 1024
internal const val SUMH_HIDE_TRANSFER_BYTES = SUMH_HIDE_STORE_BYTES + 256
internal const val SUMH_HIDE_TRANSFER_TEXT_BYTES = 64 * 1024
internal const val SUMH_HIDE_TRANSFER_MAX_RULES = 4096
internal const val SUMH_HIDE_IMPORT_GROUP_SIZE = 16
private const val HIDE_TRANSFER_KIND = "sumh_user_hide_definitions"

/** Portable intent only. No kernel identity, operation receipt, boot marker or authority. */
internal data class SUMHHidePortableDefinition(val id: String, val path: String, val enabled: Boolean,
    val name: String, val note: String, val group: String)

internal enum class SUMHHideTransferResolution { Unresolved, KeepSaved, UseImported, AdoptLocalIdentity, ExplicitNewIdentity, Skip }
internal data class SUMHHideTransferChoice(val resolution: SUMHHideTransferResolution = SUMHHideTransferResolution.Unresolved,
    val newId: String? = null)
internal enum class SUMHHideTransferCollision { New, Identical, Changed, PathCollision, IdCollision, CrossCollision }
internal data class SUMHHideTransferRow(val imported: SUMHHidePortableDefinition, val saved: SUMHHidePortableDefinition?,
    val effective: SUMHHidePortableDefinition?, val collision: SUMHHideTransferCollision, val choice: SUMHHideTransferChoice,
    val skipped: Boolean, val error: String?) {
    val sourceId get() = imported.id
    val unresolved get() = !skipped && effective == null && error == null
}
internal data class SUMHHideImportStep(val sourceId: String, val before: SUMHHidePortableDefinition?,
    val after: SUMHHidePortableDefinition, val expect: String) {
    val id get() = after.id
    fun arguments(revision: String): List<String> {
        require(validHideDefinitionRevision(revision)) { "hide_revision_invalid" }
        require(expect == if (before == null) "absent" else "same_identity") { "hide_import_expect_invalid" }
        require(before == null || before.id == after.id && before.path == after.path) { "hide_import_identity_changed" }
        validateHidePortableRow(after)
        val fields = "{\"enabled\":${after.enabled}, \"expect\":${hideQuote(expect)}, \"group\":${hideQuote(after.group)}, " +
            "\"name\":${hideQuote(after.name)}, \"note\":${hideQuote(after.note)}, \"path\":${hideQuote(after.path)}}"
        return listOf("hide", "import-id", after.id, revision, fields)
    }
}
internal data class SUMHHideImportReview(val revision: String, val bootId: String, val inputDigest: String,
    val reviewToken: String, val preserveImportedEnabled: Boolean, val rows: List<SUMHHideTransferRow>,
    val candidate: List<SUMHHidePortableDefinition>, val steps: List<SUMHHideImportStep>, val errors: List<String>) {
    val unresolved get() = rows.any { it.unresolved }
    val withinBudget get() = errors.none { it.contains("budget") || it.contains("capacity") }
    val canApply get() = !unresolved && errors.isEmpty() && rows.none { it.error != null }
    val addedCount get() = steps.count { it.before == null }
    val changedCount get() = steps.count { it.before != null }
    val enabledCount get() = steps.count { it.after.enabled && it.before?.enabled != true }
    val disabledCount get() = steps.count { !it.after.enabled && it.before?.enabled == true }
    val skippedCount get() = rows.count { it.skipped }
    val unchangedCount get() = rows.count { !it.skipped && it.error == null && it.effective != null && it.effective == it.saved }
}
internal data class SUMHHideImportBatch(val attempted: List<Pair<SUMHHideImportStep, HideMutationResult>>,
    val unattempted: List<SUMHHideImportStep>, val nextRevision: String, val stoppedReason: String?) {
    val succeededCount get() = attempted.count { it.second.succeeded }
}

private fun hideQuote(value: String): String = buildString {
    append('"')
    for (c in value) when (c) {
        '"' -> append("\\\""); '\\' -> append("\\\\"); '\b' -> append("\\b"); '\u000c' -> append("\\f")
        '\n' -> append("\\n"); '\r' -> append("\\r"); '\t' -> append("\\t")
        else -> if (c.code < 32) append("\\u" + c.code.toString(16).padStart(4, '0')) else append(c)
    }
    append('"')
}
private fun String.utf8Size() = toByteArray(Charsets.UTF_8).size
private fun validateHidePortableRow(row: SUMHHidePortableDefinition) {
    require(validHideDefinitionId(row.id)) { "hide_import_id_invalid" }
    require(row.path.startsWith('/') && validHideDefinitionText(row.path, 4095)) { "hide_import_path_invalid" }
    require(validHideDefinitionText(row.name, 128) && validHideDefinitionText(row.group, 128) &&
        validHideDefinitionText(row.note, 1024)) { "hide_import_metadata_invalid" }
}
private fun hideRowJson(row: SUMHHidePortableDefinition) = "{\"enabled\":${row.enabled}, \"group\":${hideQuote(row.group)}, " +
    "\"id\":${hideQuote(row.id)}, \"name\":${hideQuote(row.name)}, \"note\":${hideQuote(row.note)}, \"path\":${hideQuote(row.path)}}"
private fun hideStoreBytes(rows: List<SUMHHidePortableDefinition>): Long =
    "{\"rules\":[], \"schema_version\":1}\n".utf8Size().toLong() +
        rows.sumOf { hideRowJson(it).utf8Size().toLong() } + maxOf(0, rows.size - 1) * 2L
internal fun validateSUMHHidePortableDefinitions(rules: List<SUMHHidePortableDefinition>) {
    require(rules.size <= SUMH_HIDE_TRANSFER_MAX_RULES) { "hide_import_rule_capacity" }
    val ids = HashSet<String>(); val paths = HashSet<String>()
    rules.forEach {
        validateHidePortableRow(it)
        require(ids.add(it.id)) { "hide_import_duplicate_id" }
        require(paths.add(it.path)) { "hide_import_duplicate_path" }
    }
    require(hideStoreBytes(rules) <= SUMH_HIDE_STORE_BYTES) { "hide_import_store_budget" }
}
internal fun exportSUMHHideDefinitions(rules: List<SUMHHidePortableDefinition>): String {
    validateSUMHHidePortableDefinitions(rules)
    // Match native sorted keys, comma-space, NO colon-space, and the final LF.
    return "{\"kind\":\"$HIDE_TRANSFER_KIND\", \"rules\":" + rules.joinToString(", ", "[", "]", transform = ::hideRowJson) +
        ", \"schema_version\":1}\n"
}
internal fun exportSUMHHideSelectedDefinitions(saved: List<SUMHHidePortableDefinition>, selectedIds: Set<String>): String {
    validateSUMHHidePortableDefinitions(saved)
    val savedIds = saved.map { it.id }.toHashSet()
    require(selectedIds.all { it in savedIds }) { "hide_export_selection_changed" }
    return exportSUMHHideDefinitions(saved.filter { it.id in selectedIds })
}

/** A schema-specific streaming parser: the larger artifact budget never loosens IPC JSON parsers. */
internal fun importSUMHHideDefinitions(text: String): List<SUMHHidePortableDefinition> {
    require(Charsets.UTF_8.newEncoder().canEncode(text)) { "hide_import_utf8_invalid" }
    require(!text.startsWith('\uFEFF')) { "hide_import_format" } // Native JSON does not accept a UTF-8 BOM.
    require(text.utf8Size() <= SUMH_HIDE_TRANSFER_BYTES) { "hide_import_document_budget" }
    val reader = JsonReader(StringReader(text)).apply { strictness = Strictness.STRICT }
    var nodes = 0
    fun node(depth: Int) { require(depth <= 8 && ++nodes <= 40_000) { "hide_import_structure_budget" } }
    fun string(depth: Int): String {
        node(depth); require(reader.peek() == JsonToken.STRING) { "hide_import_field_type" }
        return reader.nextString()
    }
    fun row(): SUMHHidePortableDefinition {
        node(2); require(reader.peek() == JsonToken.BEGIN_OBJECT) { "hide_import_rule_type" }
        reader.beginObject(); val seen = HashSet<String>(); val texts = HashMap<String, String>(); var enabled: Boolean? = null
        while (reader.hasNext()) {
            val key = reader.nextName(); require(seen.add(key)) { "hide_import_duplicate_key" }
            when (key) {
                "id", "path", "name", "note", "group" -> texts[key] = string(3)
                "enabled" -> { node(3); require(reader.peek() == JsonToken.BOOLEAN) { "hide_import_field_type" }; enabled = reader.nextBoolean() }
                else -> error("hide_import_unknown_field")
            }
        }
        reader.endObject()
        require(seen == setOf("id", "path", "enabled", "name", "note", "group")) { "hide_import_missing_field" }
        return SUMHHidePortableDefinition(texts.getValue("id"), texts.getValue("path"), requireNotNull(enabled),
            texts.getValue("name"), texts.getValue("note"), texts.getValue("group"))
    }
    return reader.use {
        node(0); require(it.peek() == JsonToken.BEGIN_OBJECT) { "hide_import_format" }; it.beginObject()
        val seen = HashSet<String>(); var rows: List<SUMHHidePortableDefinition>? = null
        while (it.hasNext()) {
            val key = it.nextName(); require(seen.add(key)) { "hide_import_duplicate_key" }
            when (key) {
                "schema_version" -> {
                    // Native JSON stores numbers as doubles; accept the same numeric version one.
                    node(1); require(it.peek() == JsonToken.NUMBER && it.nextString().toDoubleOrNull() == 1.0) { "hide_import_format" }
                }
                "kind" -> require(string(1) == HIDE_TRANSFER_KIND) { "hide_import_format" }
                "rules" -> {
                    node(1); require(it.peek() == JsonToken.BEGIN_ARRAY) { "hide_import_rules_type" }; it.beginArray()
                    val result = ArrayList<SUMHHidePortableDefinition>()
                    while (it.hasNext()) {
                        require(result.size < SUMH_HIDE_TRANSFER_MAX_RULES) { "hide_import_rule_capacity" }
                        result += row()
                    }
                    it.endArray(); rows = result
                }
                else -> error("hide_import_unknown_field")
            }
        }
        it.endObject(); require(it.peek() == JsonToken.END_DOCUMENT) { "hide_import_trailing_data" }
        require(seen == setOf("schema_version", "kind", "rules")) { "hide_import_missing_field" }
        requireNotNull(rows).also(::validateSUMHHidePortableDefinitions)
    }
}

internal fun readSUMHHideDocument(input: InputStream): String = input.use {
    val bytes = ByteArray(SUMH_HIDE_TRANSFER_BYTES + 1); var used = 0
    while (used < bytes.size) {
        val count = it.read(bytes, used, bytes.size - used)
        if (count < 0) break
        require(count > 0) { "hide_import_read_no_progress" }; used += count
    }
    require(used <= SUMH_HIDE_TRANSFER_BYTES) { "hide_import_document_budget" }
    Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT)
        .decode(ByteBuffer.wrap(bytes, 0, used)).toString()
}
internal fun writeSUMHHideDocument(output: OutputStream, text: String) = output.use {
    importSUMHHideDefinitions(text) // Validate before the first write, including UTF-8 and canonical store capacity.
    it.write(text.toByteArray(Charsets.UTF_8)); it.flush()
}
internal fun sumhHideTransferDigest(text: String): String {
    require(Charsets.UTF_8.newEncoder().canEncode(text)) { "hide_import_utf8_invalid" }
    return MessageDigest.getInstance("SHA-256").digest(text.toByteArray(Charsets.UTF_8)).joinToString("") { "%02x".format(it) }
}
private fun hideFingerprint(fields: List<String>): String {
    val digest = MessageDigest.getInstance("SHA-256")
    fields.forEach { val bytes = it.toByteArray(Charsets.UTF_8); digest.update(ByteBuffer.allocate(4).putInt(bytes.size).array()); digest.update(bytes) }
    return digest.digest().joinToString("") { "%02x".format(it) }
}
internal fun newSUMHHideTransferIdentity(importedId: String, inputDigest: String, occupiedIds: Set<String>): String {
    require(validHideDefinitionId(importedId) && inputDigest.matches(Regex("[0-9a-f]{64}")))
    for (counter in 0..(SUMH_HIDE_TRANSFER_MAX_RULES * 2 + 1)) {
        val proposed = hideFingerprint(listOf("sumh-hide-import-copy-v1", importedId, inputDigest, counter.toString())).take(32)
        if (proposed != importedId && proposed !in occupiedIds) return proposed
    }
    error("hide_import_new_identity_exhausted")
}
internal fun sumhHideActivationPathError(path: String): String? {
    val leaf = path.substringAfterLast('/')
    return if (leaf.isEmpty() || leaf == "." || leaf == ".." || leaf.utf8Size() > 255) "hide_import_unregistrable_leaf" else null
}

private fun hideRequestWithinBudget(step: SUMHHideImportStep, revision: String): Boolean {
    val args = sumhOperationArguments(step.arguments(revision))
    val input = "{\"args\":$args, \"schema_version\":1}"
    val execute = "{\"action\":\"execute\", \"input\":$input, \"operation_id\":\"${"a".repeat(32)}\", \"schema_version\":1}"
    val frame = sumhOperationArguments(listOf("operation", "execute", "a".repeat(32), input)) + "\n"
    return args.utf8Size() <= 48 * 1024 && input.utf8Size() <= 48 * 1024 && execute.utf8Size() <= 48 * 1024 && frame.utf8Size() <= 64 * 1024
}

/** Merge-only. Every collision choice is explicit, and no source field is discarded from the review. */
internal fun reviewSUMHHideImport(saved: List<SUMHHidePortableDefinition>, imported: List<SUMHHidePortableDefinition>,
    revision: String, bootId: String, choices: Map<String, SUMHHideTransferChoice> = emptyMap(),
    preserveImportedEnabled: Boolean = false,
    inputDigest: String = sumhHideTransferDigest(exportSUMHHideDefinitions(imported))): SUMHHideImportReview {
    validateSUMHHidePortableDefinitions(saved); validateSUMHHidePortableDefinitions(imported)
    require(validHideDefinitionRevision(revision) && bootId.isNotEmpty() && validHideDefinitionText(bootId, 80)) { "hide_import_context_invalid" }
    require(inputDigest.matches(Regex("[0-9a-f]{64}"))) { "hide_import_digest_invalid" }
    val incomingIds = imported.map { it.id }.toSet()
    require(choices.keys.all { it in incomingIds }) { "hide_import_choice_not_in_document" }
    val byId = saved.associateBy { it.id }; val byPath = saved.associateBy { it.path }
    val candidate = saved.toMutableList(); val positions = saved.mapIndexed { index, row -> row.id to index }.toMap().toMutableMap()
    val targetIds = HashSet<String>(); val targetPaths = HashSet<String>(); val steps = ArrayList<SUMHHideImportStep>()
    val errors = linkedSetOf<String>(); var bytes = hideStoreBytes(saved)
    val rows = imported.map { source ->
        val idMatch = byId[source.id]; val pathMatch = byPath[source.path]
        val collision = when {
            idMatch != null && pathMatch != null && idMatch.id != pathMatch.id -> SUMHHideTransferCollision.CrossCollision
            idMatch != null && idMatch.path != source.path -> SUMHHideTransferCollision.IdCollision
            pathMatch != null && pathMatch.id != source.id -> SUMHHideTransferCollision.PathCollision
            idMatch == source.copy(enabled = if (preserveImportedEnabled) source.enabled else idMatch?.enabled ?: false) ->
                SUMHHideTransferCollision.Identical
            idMatch != null -> SUMHHideTransferCollision.Changed
            else -> SUMHHideTransferCollision.New
        }
        val local = pathMatch ?: idMatch; val choice = choices[source.id] ?: SUMHHideTransferChoice()
        var skipped = false; var error: String? = null; var effective: SUMHHidePortableDefinition? = null
        fun invalid() { error = "hide_import_choice_invalid" }
        // A persisted explicit copy mapping can prove an exact saved no-op after rebasing.
        // It never authorizes updating either side of a crossed collision. Any changed
        // target field (including enabled under the selected policy) remains blocked.
        val mappedCopyNoop = choice.resolution == SUMHHideTransferResolution.ExplicitNewIdentity &&
            choice.newId != null && validHideDefinitionId(choice.newId) && choice.newId !in incomingIds &&
            pathMatch?.id == choice.newId && pathMatch == source.copy(id = choice.newId,
                enabled = if (preserveImportedEnabled) source.enabled else pathMatch.enabled)
        if (choice.resolution != SUMHHideTransferResolution.ExplicitNewIdentity && choice.newId != null) invalid()
        else when (choice.resolution) {
            SUMHHideTransferResolution.Skip -> skipped = true
            SUMHHideTransferResolution.KeepSaved -> if (collision in setOf(SUMHHideTransferCollision.Identical,
                    SUMHHideTransferCollision.Changed, SUMHHideTransferCollision.PathCollision)) skipped = true else invalid()
            SUMHHideTransferResolution.UseImported -> if (collision in setOf(SUMHHideTransferCollision.New,
                    SUMHHideTransferCollision.Identical, SUMHHideTransferCollision.Changed)) effective = source else invalid()
            SUMHHideTransferResolution.AdoptLocalIdentity -> if (collision == SUMHHideTransferCollision.PathCollision)
                effective = source.copy(id = requireNotNull(pathMatch).id) else invalid()
            SUMHHideTransferResolution.ExplicitNewIdentity -> if (mappedCopyNoop) effective = pathMatch
                else if (collision == SUMHHideTransferCollision.IdCollision &&
                    choice.newId != null && validHideDefinitionId(choice.newId) && choice.newId !in byId && choice.newId !in incomingIds)
                    effective = source.copy(id = choice.newId) else invalid()
            SUMHHideTransferResolution.Unresolved -> if (collision in setOf(SUMHHideTransferCollision.New, SUMHHideTransferCollision.Identical)) effective = source
        }
        effective = effective?.let { next ->
            next.copy(enabled = if (preserveImportedEnabled) next.enabled else byId[next.id]?.enabled ?: false)
        }
        val next = effective
        if (next != null && error == null) {
            if (!targetIds.add(next.id) || !targetPaths.add(next.path)) error = "hide_import_resolved_identity_collision"
            else if (next.enabled) error = sumhHideActivationPathError(next.path)
            val old = byId[next.id]
            if (error == null && old != next) {
                val step = SUMHHideImportStep(source.id, old, next, if (old == null) "absent" else "same_identity")
                val index = positions[next.id]
                if (index == null) {
                    bytes += hideRowJson(next).utf8Size() + if (candidate.isEmpty()) 0 else 2
                    positions[next.id] = candidate.size; candidate += next
                } else {
                    bytes += hideRowJson(next).utf8Size() - hideRowJson(candidate[index]).utf8Size(); candidate[index] = next
                }
                if (candidate.size > SUMH_HIDE_TRANSFER_MAX_RULES || bytes > SUMH_HIDE_STORE_BYTES) errors += "hide_import_intermediate_store_budget"
                if (!hideRequestWithinBudget(step, revision)) errors += "hide_import_request_capacity"
                steps += step
            }
        }
        SUMHHideTransferRow(source, local, effective, collision, choice, skipped, error)
    }
    if (candidate.size > SUMH_HIDE_TRANSFER_MAX_RULES || bytes > SUMH_HIDE_STORE_BYTES) errors += "hide_import_store_budget"
    val token = hideFingerprint(listOf("sumh-hide-import-review-v1", inputDigest, revision, bootId, preserveImportedEnabled.toString(),
        sumhHideTransferDigest(exportSUMHHideDefinitions(saved)), sumhHideTransferDigest(exportSUMHHideDefinitions(imported))) +
        rows.flatMap { listOf(it.sourceId, it.choice.resolution.name, it.choice.newId.orEmpty(), it.effective?.let(::hideRowJson).orEmpty(),
            it.skipped.toString(), it.error.orEmpty()) })
    return SUMHHideImportReview(revision, bootId, inputDigest, token, preserveImportedEnabled, rows, candidate.toList(), steps.toList(), errors.toList())
}

/** The caller supplies the durable prepare/checkpoint/execute tracker; this runner never retries. */
internal suspend fun executeSUMHHideImport(review: SUMHHideImportReview, confirmedToken: String,
    request: suspend (List<String>, String, String) -> HideMutationResult,
    shouldContinue: () -> Boolean = { true }, onProgress: (Int, Int) -> Unit = { _, _ -> },
    clock: () -> Long = { System.nanoTime() / 1_000_000 }): SUMHHideImportBatch {
    require(review.canApply && review.reviewToken == confirmedToken) { "hide_import_review_required" }
    val attempted = ArrayList<Pair<SUMHHideImportStep, HideMutationResult>>(); var revision = review.revision
    val started = clock(); var reason: String? = null
    onProgress(0, review.steps.size)
    for (step in review.steps) {
        currentCoroutineContext().ensureActive()
        if (attempted.size >= SUMH_HIDE_IMPORT_GROUP_SIZE) { reason = "hide_import_group_complete_rebase_required"; break }
        if (!shouldContinue()) { reason = "hide_import_stopped"; break }
        if (clock() - started >= 30_000) { reason = "hide_import_deadline_rebase_required"; break }
        val received = try { request(step.arguments(revision), revision, review.bootId) }
            catch (error: Exception) {
                if (error is CancellationException) throw error
                HideMutationResult(error = error.message ?: "hide_import_result_unknown")
            }
        val receipt = received.value; val item = receipt?.items?.singleOrNull()
        val matches = received.succeeded && validHideDefinitionId(received.operationId.orEmpty()) && receipt?.action == "import-id" &&
            receipt.status == "applied" && receipt.previousRevision == revision && receipt.bootId == review.bootId &&
            validHideDefinitionRevision(receipt.revision.orEmpty()) && receipt.affected == 1 && receipt.confirmed == 1 &&
            receipt.failed == 0 && receipt.unknown == 0 && receipt.itemsComplete && item?.id == step.id &&
            item.errno == 0 && item.membership == true &&
            (if (step.after.enabled) item.outcome in setOf("registered_bound", "registered_pending", "registered_binding_error") && item.runtime != null
                else item.outcome == "absent" && item.runtime == null)
        val result = if (received.succeeded && !matches) received.copy(error = "hide_import_receipt_identity_unknown") else received
        attempted += step to result; onProgress(attempted.size, review.steps.size)
        if (!matches) { reason = "hide_import_query_or_rebase_required"; break }
        revision = requireNotNull(receipt?.revision)
    }
    return SUMHHideImportBatch(attempted.toList(), review.steps.drop(attempted.size), revision, reason)
}
