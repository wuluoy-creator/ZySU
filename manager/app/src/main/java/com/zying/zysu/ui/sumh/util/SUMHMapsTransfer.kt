package com.zying.zysu.ui.sumh.util

import com.google.gson.JsonObject
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter.MapsPathRule
import org.json.JSONObject
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction

internal const val SUMH_MAPS_TRANSFER_BYTES = 64 * 1024
internal enum class SUMHMapsImportMode { Merge, Replace }
internal enum class SUMHMapsConflictChoice { Unresolved, KeepSaved, UseImported }
internal data class SUMHMapsImportReview(
    val candidate: List<MapsPathRule>, val added: List<MapsPathRule>, val removed: List<MapsPathRule>,
    val changed: List<Pair<MapsPathRule, MapsPathRule>>, val conflicts: List<Pair<MapsPathRule, MapsPathRule>>,
    val unresolved: Boolean, val withinBudget: Boolean,
)

internal fun validateMapsTransferRules(rules: List<MapsPathRule>) {
    require(rules.size <= SUMH_MAX_MAPS_PATH_RULES) { "rule_count" }
    require(rules.map { it.targetPath }.distinct().size == rules.size) { "duplicate_target" }
    require(rules.all {
        Charsets.UTF_8.newEncoder().canEncode(it.targetPath) && Charsets.UTF_8.newEncoder().canEncode(it.replacementPath) &&
            validateSUMHManagedMapsPath(it.targetPath, 4095) == null && validateSUMHManagedMapsPath(it.replacementPath) == null
    }) { "invalid_path" }
    require(mapsRulesWithinBudget(rules)) { "encoded_budget" }
}

/** Configuration exchange includes definitions only, never transient inode or authority. */
internal fun exportSUMHMapsDefinitions(rules: List<MapsPathRule>): String {
    validateMapsTransferRules(rules)
    return JSONObject().put("schema_version", 1).put("kind", "sumh_managed_maps")
        .put("rules", mapsRulesJson(rules)).toString().also {
            require(it.toByteArray(Charsets.UTF_8).size <= SUMH_MAPS_TRANSFER_BYTES) { "document_budget" }
        }
}

internal fun importSUMHMapsDefinitions(text: String): List<MapsPathRule> {
    require(Charsets.UTF_8.newEncoder().canEncode(text)) { "invalid_utf8" }
    val root = SUMHControlStateParser.document(text, SUMH_MAPS_TRANSFER_BYTES)
    require(root.keySet() == setOf("schema_version", "kind", "rules")) { "unknown_fields" }
    val version = root.get("schema_version")
    require(version.isJsonPrimitive && version.asJsonPrimitive.isNumber && version.toString() == "1") { "unsupported_format" }
    require(root.strictString("kind") == "sumh_managed_maps") { "unsupported_format" }
    val raw = root.get("rules")
    require(raw.isJsonArray && raw.asJsonArray.size() <= SUMH_MAX_MAPS_PATH_RULES) { "rule_count" }
    return raw.asJsonArray.map { item ->
        require(item.isJsonObject) { "invalid_rule" }
        item.asJsonObject.let { rule ->
            require(rule.keySet() == setOf("target_path", "replacement_path")) { "unknown_fields" }
            MapsPathRule(rule.strictString("target_path"), rule.strictString("replacement_path"))
        }
    }.also(::validateMapsTransferRules)
}

private fun JsonObject.strictString(key: String): String = get(key).let {
    require(it.isJsonPrimitive && it.asJsonPrimitive.isString) { "invalid_field" }; it.asString
}

internal fun reviewSUMHMapsImport(saved: List<MapsPathRule>, imported: List<MapsPathRule>,
    mode: SUMHMapsImportMode, choice: SUMHMapsConflictChoice): SUMHMapsImportReview {
    val old = saved.associateBy { it.targetPath }
    val incoming = imported.associateBy { it.targetPath }
    val conflicts = imported.mapNotNull { next -> old[next.targetPath]?.takeIf { it != next }?.let { it to next } }
    val selected = incoming.mapValues { (target, next) ->
        if (choice == SUMHMapsConflictChoice.KeepSaved) old[target] ?: next else next
    }
    val candidate = when (mode) {
        SUMHMapsImportMode.Merge -> saved.map { selected[it.targetPath] ?: it } + imported.filter { it.targetPath !in old }
        SUMHMapsImportMode.Replace -> imported.map { selected.getValue(it.targetPath) }
    }
    return SUMHMapsImportReview(candidate, candidate.filter { it.targetPath !in old },
        saved.filter { row -> candidate.none { it.targetPath == row.targetPath } },
        candidate.mapNotNull { next -> old[next.targetPath]?.takeIf { it != next }?.let { it to next } }, conflicts,
        conflicts.isNotEmpty() && choice == SUMHMapsConflictChoice.Unresolved,
        candidate.size <= SUMH_MAX_MAPS_PATH_RULES && mapsRulesWithinBudget(candidate))
}

internal fun readSUMHMapsDocument(input: InputStream): String = input.use {
    val buffer = ByteArray(SUMH_MAPS_TRANSFER_BYTES + 1)
    var count = 0
    while (count < buffer.size) {
        val read = it.read(buffer, count, buffer.size - count)
        if (read < 0) break
        require(read > 0) { "read_no_progress" }
        count += read
    }
    require(count <= SUMH_MAPS_TRANSFER_BYTES) { "document_budget" }
    Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
        .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(buffer, 0, count)).toString()
}
