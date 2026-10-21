package com.zying.zysu.ui.sumh.util

import android.os.ParcelFileDescriptor
import com.google.gson.*
import com.google.gson.stream.JsonReader
import com.google.gson.stream.JsonToken
import com.zying.zysu.Natives
import com.zying.zysu.ui.util.getKsud
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.io.InputStream
import java.io.StringReader
import java.nio.ByteBuffer
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.attribute.BasicFileAttributes
import java.nio.file.attribute.PosixFilePermissions
import java.security.MessageDigest
import java.util.concurrent.locks.ReentrantLock

internal const val SUMH_PROFILE_BYTES = 8 * 1024 * 1024
internal const val SUMH_PROFILE_INPUT_BYTES = SUMH_PROFILE_BYTES + 4096
internal const val SUMH_PROFILE_RESPONSE_BYTES = 24 * 1024 * 1024
internal val SUMH_PROFILE_ACTIONS = setOf("list", "show", "export", "preview", "import-preview", "capture", "import", "clone", "rename", "remove")
internal val SUMH_PROFILE_MUTATIONS = setOf("capture", "import", "clone", "rename", "remove")
internal enum class SUMHProfileResource(val wireName: String) { Settings("settings"), Modules("modules"), UserHide("user_hide"), Maps("maps") }
internal data class SUMHProfileForm(val id: String = "", val name: String = "", val note: String = "") {
    val error: String? get() = when {
        !validSUMHProfileId(id) -> "profile_id_invalid"
        !validSUMHProfileText(name, 128, true) -> "profile_name_invalid"
        !validSUMHProfileText(note, 1024) -> "profile_note_invalid"
        else -> null
    }
}
internal data class SUMHProfileItem(val id: String, val name: String, val note: String, val revision: String)
internal data class SUMHProfileDocument(val text: String, val name: String, val note: String, val resourceCounts: Map<SUMHProfileResource, Int>)
internal data class SUMHProfileChange(val key: String, val kind: String, val before: String?, val after: String?)
internal data class SUMHProfileResourceDiff(val resource: SUMHProfileResource, val added: Int, val removed: Int,
    val changed: Int, val unchanged: Int, val changes: List<SUMHProfileChange>, val enabledChanges: List<SUMHProfileChange> = emptyList(),
    val identityConflicts: List<String> = emptyList())
internal data class SUMHProfileDiff(val resources: List<SUMHProfileResourceDiff>, val observation: String)
internal data class SUMHProfileCapabilities(val actions: Set<String> = emptySet(), val error: String? = null, val known: Boolean = false) {
    fun supports(action: String) = known && error == null && action in actions
}
internal data class SUMHProfileResult(val action: String, val id: String? = null, val status: String = "unknown",
    val reason: String = "result_unknown", val bankRevision: String? = null, val profileRevision: String? = null,
    val items: List<SUMHProfileItem> = emptyList(), val document: SUMHProfileDocument? = null, val diff: SUMHProfileDiff? = null,
    val saved: Boolean = false, val durabilityConfirmed: Boolean = false, val readbackConfirmed: Boolean = false, val error: String? = null) {
    val complete get() = error == null && status in setOf("complete", "saved", "unchanged")
    val unknown get() = error != null || status == "unknown"
}
internal data class SUMHProfileRequest(val action: String, val id: String? = null, val bankRevision: String? = null,
    val profileRevision: String? = null, val sourceId: String? = null, val name: String? = null, val note: String? = null,
    val document: String? = null, val resources: Set<SUMHProfileResource> = emptySet())
internal fun validSUMHProfileId(text: String) = text.matches(Regex("[a-z0-9][a-z0-9_-]{0,63}"))
internal fun validSUMHProfileRevision(text: String) = text.matches(Regex("sha256-v1:[0-9a-f]{64}"))
internal fun validSUMHProfileText(text: String, maximum: Int, nonempty: Boolean = false): Boolean = try {
    (!nonempty || text.isNotEmpty()) && '\u0000' !in text && profileUtf8(text).size <= maximum
} catch (_: Exception) { false }
internal fun profileUtf8(text: String): ByteArray = Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
    .onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(text)).let { b -> ByteArray(b.remaining()).also(b::get) }
internal fun profileDecode(bytes: ByteArray): String = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
    .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString()
internal fun profileDigest(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) }
internal fun readSUMHProfileBytes(input: InputStream, maximum: Int): ByteArray = input.use {
    val buffer = ByteArray(maximum + 1); var used = 0
    while (used < buffer.size) { val n = it.read(buffer, used, buffer.size - used); if (n < 0) break; require(n > 0); used += n }
    require(used <= maximum) { "profile_capacity" }; buffer.copyOf(used)
}

/** Isolated artifact parser. Its capacity does not change any shared IPC parser. */
internal object SUMHProfilesParser {
    fun json(text: String, maximum: Int = SUMH_PROFILE_RESPONSE_BYTES): JsonObject {
        require(profileUtf8(text).size <= maximum && !text.startsWith('\uFEFF'))
        val reader = JsonReader(StringReader(text)).apply { strictness = Strictness.STRICT }; var nodes = 0
        fun read(depth: Int): JsonElement {
            require(depth <= 20 && ++nodes <= 400_000) { "profile_structure_capacity" }
            return when (reader.peek()) {
                JsonToken.BEGIN_OBJECT -> JsonObject().also { value -> reader.beginObject(); while (reader.hasNext()) {
                    val key = reader.nextName(); require(validSUMHProfileText(key, 4095) && !value.has(key)); value.add(key, read(depth + 1))
                }; reader.endObject() }
                JsonToken.BEGIN_ARRAY -> JsonArray().also { value -> reader.beginArray(); while (reader.hasNext()) value.add(read(depth + 1)); reader.endArray() }
                JsonToken.STRING -> JsonPrimitive(reader.nextString().also { require(validSUMHProfileText(it, maximum)) })
                JsonToken.NUMBER -> JsonPrimitive(reader.nextString().also { require(it.length <= 64) }.toBigDecimal().also {
                    require(it.precision() <= 20 && it.scale() in -10..10) { "profile_numeric_capacity" }
                })
                JsonToken.BOOLEAN -> JsonPrimitive(reader.nextBoolean())
                JsonToken.NULL -> { reader.nextNull(); JsonNull.INSTANCE }
                else -> error("profile_json_invalid")
            }
        }
        return reader.use { val out = read(0).obj(); require(it.peek() == JsonToken.END_DOCUMENT); out }
    }
    fun request(value: SUMHProfileRequest): ByteArray {
        require(value.action in SUMH_PROFILE_ACTIONS || value.action == "capabilities")
        val root = JsonObject().apply { addProperty("schema_version", 1); addProperty("action", value.action) }
        val mutation = value.action in SUMH_PROFILE_MUTATIONS
        val target = if (mutation) JsonObject().also { it.addProperty("schema_version", 1); root.add("input", it) } else root
        if (value.action !in setOf("capabilities", "list", "import-preview")) {
            require(validSUMHProfileId(requireNotNull(value.id))); target.addProperty("id", value.id)
            require(validSUMHProfileRevision(requireNotNull(value.bankRevision))); target.addProperty("bank_revision", value.bankRevision)
        } else require(value.id == null && value.bankRevision == null)
        if (value.action in setOf("show", "export", "preview", "clone", "rename", "remove")) {
            require(validSUMHProfileRevision(requireNotNull(value.profileRevision))); target.addProperty("profile_revision", value.profileRevision)
        } else require(value.profileRevision == null)
        if (value.action == "clone") { require(validSUMHProfileId(requireNotNull(value.sourceId))); target.addProperty("source_id", value.sourceId) }
        else require(value.sourceId == null)
        if (value.action in setOf("capture", "clone", "rename")) {
            require(validSUMHProfileText(requireNotNull(value.name), 128, true) && validSUMHProfileText(requireNotNull(value.note), 1024))
            target.addProperty("name", value.name); target.addProperty("note", value.note)
        } else require(value.name == null && value.note == null)
        if (value.action in setOf("import", "import-preview")) target.add("profile", json(document(requireNotNull(value.document)).text, SUMH_PROFILE_BYTES))
        else require(value.document == null)
        if (value.action in setOf("preview", "import-preview")) {
            require(value.resources.isNotEmpty()); root.add("resources", JsonArray().apply {
                SUMHProfileResource.entries.filter { it in value.resources }.forEach { add(it.wireName) }
            })
        } else require(value.resources.isEmpty())
        return profileUtf8(root.toString()).also { require(it.size <= SUMH_PROFILE_INPUT_BYTES) }
    }
    fun capabilities(bytes: ByteArray): SUMHProfileCapabilities {
        val root = json(profileDecode(bytes), 8192)
        root.fields("schema_version", "kind", "manager_schema", "actions", "limits", "activate", "restore", "operation_journal")
        require(root.int("schema_version", 1, 1) == 1 && root.text("kind") == "sumh.profile.manager.capabilities" && root.int("manager_schema", 1, 1) == 1)
        require(!root.bool("activate") && !root.bool("restore") && !root.bool("operation_journal"))
        val actions = root.obj("actions"); require(actions.keySet() == SUMH_PROFILE_ACTIONS)
        val limits = root.obj("limits"); limits.fields("profile_bytes", "input_bytes", "response_bytes", "bank_profiles", "bank_bytes")
        require(limits.int("profile_bytes", SUMH_PROFILE_BYTES, SUMH_PROFILE_BYTES) == SUMH_PROFILE_BYTES)
        limits.int("input_bytes", SUMH_PROFILE_INPUT_BYTES, SUMH_PROFILE_INPUT_BYTES)
        limits.int("response_bytes", SUMH_PROFILE_RESPONSE_BYTES, SUMH_PROFILE_RESPONSE_BYTES)
        limits.int("bank_profiles", 16, 16); limits.int("bank_bytes", 33554432, 33554432)
        return SUMHProfileCapabilities(actions.keySet().filter { actions.bool(it) }.toSet(), known = true)
    }
    fun transfer(bytes: ByteArray): Int {
        val value = json(profileDecode(bytes), 4096); value.fields("schema_version", "kind", "complete", "errno", "bytes")
        require(value.int("schema_version", 1, 1) == 1 && value.text("kind") == "sumh.profile.transfer")
        val complete = value.bool("complete"); val error = value.int("errno", 0, 4095)
        val size = value.int("bytes", 0, SUMH_PROFILE_RESPONSE_BYTES)
        require(complete && error == 0 && size > 0) { "profile_transfer_unknown" }; return size
    }
    fun document(text: String): SUMHProfileDocument {
        val root = json(text, SUMH_PROFILE_BYTES); root.fields("schema_version", "kind", "name", "note", "resources")
        root.int("schema_version", 1, 1); require(root.text("kind") == "sumh.profile")
        val name = root.text("name", 128).also { require(it.isNotEmpty()) }; val note = root.text("note", 1024)
        val resources = root.obj("resources"); require(resources.keySet() == SUMHProfileResource.entries.map { it.wireName }.toSet())
        val settings = resources.obj("settings"); validateSettingsShape(settings)
        val modules = resources.array("modules", 256); val ids = HashSet<String>(); var ruleCount = 0
        modules.forEach { raw -> val module = raw.obj(); module.fields("id", "mode", "rules")
            val id = module.text("id", 255); require(id.matches(Regex("[a-zA-Z0-9_.-]+")) && id !in setOf(".", "..") && ids.add(id))
            module.text("mode", 32)
            val paths = HashSet<String>(); val rules = module.array("rules", 256); ruleCount += rules.size; require(ruleCount <= 4096)
            rules.forEach { value -> val rule = value.obj(); rule.fields("path", "mode"); val path = rule.text("path", 4095)
                require(paths.add(path)); rule.text("mode", 32) }
        }
        val hidden = resources.obj("user_hide"); hidden.fields("schema_version", "rules"); hidden.int("schema_version", 1, 1)
        val hideRows = hidden.array("rules", 4096); val hideIds = HashSet<String>(); val hidePaths = HashSet<String>()
        hideRows.forEach { value -> val row = value.obj(); row.fields("id", "path", "enabled", "name", "note", "group")
            val id = row.text("id", 32); val path = row.text("path", 4095)
            require(validHideDefinitionId(id) && hideIds.add(id) && path.startsWith('/') && hidePaths.add(path))
            row.bool("enabled"); row.text("name", 128); row.text("note", 1024); row.text("group", 128)
        }
        require(profileUtf8(canonical(hidden) + "\n").size <= 4 * 1024 * 1024)
        val maps = resources.obj("maps"); maps.fields("enabled", "rules"); maps.bool("enabled")
        val mapTargets = HashSet<String>(); val mapRows = maps.array("rules", 256)
        mapRows.forEach { value -> val row = value.obj(); row.fields("target_path", "replacement_path")
            val target = row.text("target_path", 4095); val replacement = row.text("replacement_path", 255)
            require(validateSUMHManagedMapsPath(target, 4095) == null && validateSUMHManagedMapsPath(replacement) == null && mapTargets.add(target)) }
        // Native canonicalizes module order; exported documents must have matching semantic revisions.
        resources.add("modules", JsonArray().apply { modules.sortedBy { it.obj().text("id", 255) }.forEach(::add) })
        val normalized = canonical(root) + "\n"; require(profileUtf8(normalized).size <= SUMH_PROFILE_BYTES)
        return SUMHProfileDocument(normalized, name, note, mapOf(SUMHProfileResource.Settings to settings.size(),
            SUMHProfileResource.Modules to modules.size, SUMHProfileResource.UserHide to hideRows.size, SUMHProfileResource.Maps to mapRows.size))
    }
    fun revision(document: SUMHProfileDocument) = "sha256-v1:" + profileDigest(profileUtf8("sumhp-profile-document-v1\n" + document.text.removeSuffix("\n")))
    /** Display/import staging checks only. Native owns supported keys, enum values and write budgets. */
    private fun validateSettingsShape(value: JsonObject) {
        value.entrySet().forEach { (_, child) ->
            if (child.isJsonArray) child.asJsonArray.forEach { it.text(255) }
            else {
                require(child.isJsonPrimitive)
                if (child.asJsonPrimitive.isNumber) child.asBigDecimal.intValueExact()
            }
        }
        require(profileUtf8(canonical(value)).size <= 65536)
    }
    fun result(bytes: ByteArray, request: SUMHProfileRequest): SUMHProfileResult {
        val root = json(profileDecode(bytes)); root.int("schema_version", 1, 1)
        val mutation = request.action in SUMH_PROFILE_MUTATIONS; val kind = root.text("kind")
        require(kind == if (mutation && root.has("requested_action")) "sumh.profile.mutation" else "sumh.profile.result")
        val action = root.text("action")
        if (kind == "sumh.profile.mutation") {
            require(root.text("requested_action") == request.action && action == if (request.action in setOf("capture", "import")) "create" else request.action)
        } else require(action == request.action)
        val id = root.required("id").let { if (it.isJsonNull) null else it.text(64).ifEmpty { null } }
        require(id == request.id)
        val status = root.text("status").also { require(it in setOf("complete", "saved", "unchanged", "not_saved", "unknown", "error", "stale")) }
        if (mutation && status in setOf("saved", "unchanged", "unknown")) require(kind == "sumh.profile.mutation")
        val reason = root.text("reason", 256)
        require(!root.bool("applied") && !root.bool("profile_active") && !root.bool("active_configuration_saved"))
        val saved = root.bool("saved"); require(!saved || mutation && status in setOf("saved", "unchanged"))
        if (kind == "sumh.profile.result") require(!root.bool("can_activate") && !root.bool("runtime_evaluated") && !root.bool("global_atomic"))
        val bank = root.optionalRevision("bank_revision"); val profile = root.optionalRevision("profile_revision")
        val durable = if (root.has("durability_confirmed")) root.bool("durability_confirmed") else false
        val readback = if (root.has("readback_confirmed")) root.bool("readback_confirmed") else false
        if (status == "saved") require(mutation && saved && durable && readback && bank != null && (request.action == "remove" || profile != null))
        if (status == "unchanged") require(mutation && saved && readback && bank != null)
        if (status in setOf("saved", "unchanged")) {
            val previous = requireNotNull(root.optionalRevision("previous_bank_revision"))
            require(previous == request.bankRevision && if (request.action == "remove") profile == null else profile != null)
            if (status == "unchanged") require(bank == previous && (request.action !in setOf("rename", "remove") || profile == request.profileRevision))
        }
        if (status == "complete") require(!mutation && !saved && (request.action == "import-preview" || bank != null))
        val items = if (request.action == "list" && status == "complete") {
            val missing = root.bool("missing"); root.array("profiles", 16).map { raw -> val row = raw.obj(); row.fields("id", "name", "note", "profile_revision", "profile_active")
                require(!row.bool("profile_active")); val itemId = row.text("id", 64).also { require(validSUMHProfileId(it)) }
                SUMHProfileItem(itemId, row.text("name", 128).also { require(it.isNotEmpty()) }, row.text("note", 1024), requireNotNull(row.optionalRevision("profile_revision")))
            }.also { require(it.map { row -> row.id }.distinct().size == it.size && (!missing || it.isEmpty())) }
        } else emptyList()
        val document = if (request.action in setOf("show", "export") && status == "complete") {
            require(bank == request.bankRevision && profile == request.profileRevision)
            document(canonical(root.required("profile"))).also { require(revision(it) == profile) }
        } else null
        val diff = if (request.action in setOf("preview", "import-preview") && status == "complete") {
            if (request.action == "preview") require(bank == request.bankRevision && profile == request.profileRevision)
            require(root.bool("configuration_diff_only") && root.text("module_and_capability_applicability") == "unknown_not_evaluated" &&
                root.text("runtime_ownership_and_generation") == "not_evaluated" && root.text("module_source_contents") == "not_captured")
            val selected = root.array("selected_resources", 4).map { it.text(32) }; require(selected.size == selected.distinct().size && selected.toSet() == request.resources.map { it.wireName }.toSet())
            val resources = root.obj("diff").also { it.fields("resources") }.obj("resources"); require(resources.keySet() == selected.toSet())
            SUMHProfileDiff(SUMHProfileResource.entries.filter { it in request.resources }.map { scope -> parseDiff(scope, resources.obj(scope.wireName)) },
                canonical(root.obj("observation").also(::validateObservation)))
        } else null
        require(root.has("profiles") == (request.action == "list" && status == "complete") && root.has("missing") == root.has("profiles"))
        require(root.has("profile") == (document != null) && root.has("diff") == (diff != null))
        for (field in listOf("configuration_diff_only", "selected_resources", "module_and_capability_applicability", "runtime_ownership_and_generation", "module_source_contents"))
            require(root.has(field) == (diff != null))
        val allowed = setOf("schema_version", "kind", "action", "id", "status", "reason", "saved", "applied", "profile_active", "active_configuration_saved",
            "can_activate", "runtime_evaluated", "global_atomic", "bank_revision", "profile_revision", "previous_bank_revision", "detail", "requested_action", "observation",
            "durability_confirmed", "readback_confirmed", "namespace_restore_failed", "namespace_restore_errno", "errno", "profiles", "missing", "profile", "configuration_diff_only",
            "diff", "selected_resources", "module_and_capability_applicability", "runtime_ownership_and_generation", "module_source_contents")
        require(root.keySet().all { it in allowed })
        val namespaceUnknown = root.has("namespace_restore_failed") && root.bool("namespace_restore_failed")
        return SUMHProfileResult(request.action, id, if (namespaceUnknown) "unknown" else status, reason, bank, profile, items,
            document, diff, saved, durable, readback, if (namespaceUnknown) "profile_namespace_result_unknown" else null)
    }
    private fun validateObservation(value: JsonObject) {
        value.fields("capture_consistency", "global_atomic", "runtime_verified", "source_files_backed_up", "settings_defaults_materialized", "sources", "stable")
        require(value.text("capture_consistency") == "double_read_observation" && !value.bool("global_atomic") && !value.bool("runtime_verified") &&
            !value.bool("source_files_backed_up") && value.bool("settings_defaults_materialized") && value.bool("stable"))
        val sources = value.obj("sources"); sources.fields("config", "module_modes", "module_rules", "user_hide")
        sources.entrySet().forEach { (_, raw) -> val row = raw.obj(); row.fields("status", "errno", "revision")
            require(row.text("status") in setOf("valid", "missing")); row.int("errno", 0, 4095); requireNotNull(row.optionalRevision("revision")) }
    }
    private fun parseDiff(scope: SUMHProfileResource, value: JsonObject): SUMHProfileResourceDiff {
        val capacity = when (scope) { SUMHProfileResource.Settings -> 64; SUMHProfileResource.UserHide -> 4096; else -> 256 }
        val maximum = capacity * 2
        fun changes(v: JsonObject, max: Int): Pair<List<Int>, List<SUMHProfileChange>> {
            val counts = v.obj("counts"); counts.fields("added", "removed", "changed", "unchanged")
            val numbers = listOf("added", "removed", "changed", "unchanged").map { counts.int(it, 0, max) }
            val rows = v.array("changes", max).map { raw -> val row = raw.obj(); row.fields("key", "kind", "before", "after")
                val kind = row.text("kind"); val before = row.required("before"); val after = row.required("after")
                require(kind in setOf("add", "remove", "change") && before.isJsonNull == (kind == "add") && after.isJsonNull == (kind == "remove"))
                require(kind != "change" || before != after)
                SUMHProfileChange(row.text("key", 4095), kind, if (before.isJsonNull) null else canonical(before), if (after.isJsonNull) null else canonical(after))
            }
            require(rows.map { it.key }.distinct().size == rows.size && listOf("add", "remove", "change").map { k -> rows.count { it.kind == k } } == numbers.take(3))
            return numbers to rows
        }
        val expected = setOf("counts", "changes") + when (scope) { SUMHProfileResource.Maps -> setOf("enabled"); SUMHProfileResource.UserHide -> setOf("identity_conflicts"); else -> emptySet() }
        require(value.keySet() == expected); val (counts, rows) = changes(value, maximum)
        require(counts[0] + counts[2] + counts[3] <= capacity && counts[1] + counts[2] + counts[3] <= capacity)
        val enabled = if (scope == SUMHProfileResource.Maps) changes(value.obj("enabled").also { it.fields("counts", "changes") }, 1).second.also { list ->
            require(list.all { it.key == "enabled" && it.kind == "change" && it.before in setOf("true", "false") && it.after in setOf("true", "false") }) } else emptyList()
        val conflicts = if (scope == SUMHProfileResource.UserHide) value.array("identity_conflicts", 8192).map { raw -> val row = raw.obj()
            when (row.text("kind")) {
                "same_id_different_path" -> { row.fields("kind", "id", "before_path", "after_path"); require(validHideDefinitionId(row.text("id", 32)) && row.text("before_path", 4095) != row.text("after_path", 4095)) }
                "same_path_different_id" -> { row.fields("kind", "path", "before_id", "after_id"); require(row.text("path", 4095).startsWith('/') && validHideDefinitionId(row.text("before_id", 32)) && validHideDefinitionId(row.text("after_id", 32)) && row.text("before_id") != row.text("after_id")) }
                else -> error("profile_conflict_invalid")
            }; canonical(row)
        } else emptyList()
        return SUMHProfileResourceDiff(scope, counts[0], counts[1], counts[2], counts[3], rows, enabled, conflicts)
    }
    internal fun canonical(value: JsonElement): String = when {
        value.isJsonNull -> "null"
        value.isJsonObject -> value.asJsonObject.entrySet().sortedBy { it.key }.joinToString(", ", "{", "}") { quote(it.key) + ":" + canonical(it.value) }
        value.isJsonArray -> value.asJsonArray.joinToString(", ", "[", "]") { canonical(it) }
        value.asJsonPrimitive.isString -> quote(value.asString)
        value.asJsonPrimitive.isBoolean -> value.asBoolean.toString()
        else -> value.asBigDecimal.stripTrailingZeros().toPlainString()
    }
    private fun quote(text: String): String = buildString { append('"'); text.forEach { c -> append(when(c) {
        '"' -> "\\\""; '\\' -> "\\\\"; '\b' -> "\\b"; '\u000c' -> "\\f"; '\n' -> "\\n"; '\r' -> "\\r"; '\t' -> "\\t"
        else -> if (c < ' ') "\\u" + c.code.toString(16).padStart(4, '0') else c.toString()
    }) }; append('"') }
    internal fun JsonElement.obj(): JsonObject { require(isJsonObject); return asJsonObject }
    internal fun JsonObject.obj(key: String) = required(key).obj()
    internal fun JsonObject.required(key: String): JsonElement = requireNotNull(get(key))
    internal fun JsonObject.fields(vararg keys: String) { require(keySet() == keys.toSet()) }
    internal fun JsonElement.text(max: Int): String { require(isJsonPrimitive && asJsonPrimitive.isString); return asString.also { require(validSUMHProfileText(it, max)) } }
    internal fun JsonObject.text(key: String, max: Int = 256) = required(key).text(max)
    internal fun JsonObject.bool(key: String) = required(key).let { require(it.isJsonPrimitive && it.asJsonPrimitive.isBoolean); it.asBoolean }
    internal fun JsonObject.int(key: String, min: Int, max: Int) = required(key).let { require(it.isJsonPrimitive && it.asJsonPrimitive.isNumber); it.asBigDecimal.intValueExact().also { n -> require(n in min..max) } }
    internal fun JsonObject.array(key: String, max: Int) = required(key).let { require(it.isJsonArray && it.asJsonArray.size() <= max); it.asJsonArray.toList() }
    internal fun JsonObject.optionalRevision(key: String): String? = get(key)?.takeUnless { it.isJsonNull }?.text(74)?.also { require(validSUMHProfileRevision(it)) }
}

internal interface SUMHProfilesService {
    suspend fun capabilities(): SUMHProfileCapabilities
    suspend fun execute(request: SUMHProfileRequest): SUMHProfileResult
}
/** Private fixed descriptor transport; imported text is data and never shell/argv/path input. */
internal class NativeSUMHProfilesService(private val directory: File,
    private val transfer: (ByteArray, Int, Int) -> ByteArray = { path, input, output -> Natives.sumhProfilesTransfer(path, input, output) }) : SUMHProfilesService {
    private suspend fun call(request: SUMHProfileRequest): ByteArray = withContext(Dispatchers.IO) {
        require(transferLock.tryLock()) { "profile_transfer_busy" }
        try {
        val bytes = SUMHProfilesParser.request(request)
        require(!Files.isSymbolicLink(directory.toPath()))
        if (!directory.exists()) require(directory.mkdir())
        require(directory.isDirectory)
        // Two bounded reusable slots. A process death cannot accumulate arbitrary temp files.
        fun slot(name: String, maximum: Int): File {
            val file = File(directory, name)
            if (Files.exists(file.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                require(Files.readAttributes(file.toPath(), BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS).isRegularFile && file.length() <= maximum)
                Files.delete(file.toPath()) // Never truncate an old inode/hard-linked target.
            }
            Files.createFile(file.toPath(), PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rw-------"))); return file
        }
        val input = slot("transfer-input-v1.json", SUMH_PROFILE_INPUT_BYTES)
        val output = slot("transfer-output-v1.json", SUMH_PROFILE_RESPONSE_BYTES)
        try {
            input.outputStream().use { it.write(bytes); it.fd.sync() }
            ParcelFileDescriptor.open(input, ParcelFileDescriptor.MODE_READ_ONLY).use { source ->
                ParcelFileDescriptor.open(output, ParcelFileDescriptor.MODE_WRITE_ONLY).use { target ->
                    val count = SUMHProfilesParser.transfer(transfer(profileUtf8(getKsud()), source.fd, target.fd))
                    require(output.length() == count.toLong())
                    val result = readSUMHProfileBytes(Files.newInputStream(output.toPath(), LinkOption.NOFOLLOW_LINKS), SUMH_PROFILE_RESPONSE_BYTES)
                    require(result.size == count)
                    val digest = MessageDigest.getInstance("SHA-256"); var reread = 0
                    Files.newInputStream(output.toPath(), LinkOption.NOFOLLOW_LINKS).use { stream ->
                        val buffer = ByteArray(8192)
                        while (true) { val n = stream.read(buffer); if (n < 0) break; require(n > 0 && reread + n <= count); digest.update(buffer, 0, n); reread += n }
                    }
                    require(reread == count && MessageDigest.getInstance("SHA-256").digest(result).contentEquals(digest.digest()))
                    result
                }
            }
        } finally { input.delete(); output.delete() }
        } finally { transferLock.unlock() }
    }
    override suspend fun capabilities(): SUMHProfileCapabilities = try { withContext(Dispatchers.Default) { SUMHProfilesParser.capabilities(call(SUMHProfileRequest("capabilities"))) } }
    catch (cancelled: CancellationException) { throw cancelled }
    catch (_: Exception) { SUMHProfileCapabilities(error = "profile_capabilities_unknown") }
    catch (_: LinkageError) { SUMHProfileCapabilities(error = "profile_transport_unavailable") }
    override suspend fun execute(request: SUMHProfileRequest): SUMHProfileResult = try { withContext(Dispatchers.Default) { SUMHProfilesParser.result(call(request), request) } }
    catch (cancelled: CancellationException) { if (request.action !in SUMH_PROFILE_MUTATIONS) throw cancelled; SUMHProfileResult(request.action, request.id, error = "profile_result_unknown") }
    catch (_: Exception) { SUMHProfileResult(request.action, request.id, error = "profile_result_unknown") }
    catch (_: LinkageError) { SUMHProfileResult(request.action, request.id, error = "profile_transport_unavailable") }
    companion object { private val transferLock = ReentrantLock() }
}
