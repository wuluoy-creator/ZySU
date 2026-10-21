package com.zying.zysu.ui.sumh.util

import com.google.gson.JsonElement
import com.google.gson.JsonObject

internal enum class SUMHMapsPreviewStatus { Complete, Partial, Unknown, Stale }
internal enum class SUMHMapsSavedStatus { Valid, Missing, Invalid, Unreadable, Changed }
internal enum class SUMHMapsIdentityStatus { Regular, Missing, NotRegular, Unrepresentable, Error, Unknown }
internal enum class SUMHMapsRegistration { Matches, Missing, Different, Unknown }
internal data class SUMHMapsIdentity(val status: SUMHMapsIdentityStatus, val ino: String?, val dev: String?, val errno: Int)
internal data class SUMHMapsPreviewRow(
    val index: Int, val targetPath: String, val replacementPath: String, val target: SUMHMapsIdentity,
    val replacement: SUMHMapsIdentity, val collision: Boolean, val stable: Boolean, val registration: SUMHMapsRegistration,
)
internal data class SUMHMapsPreviewSample(
    val namespace: String?, val instance: String?, val generation: String?, val identitiesSampled: Boolean,
    val definitionsComplete: Boolean, val stable: Boolean, val comparison: SUMHComparison, val reason: String, val errno: Int,
)
internal data class SUMHMapsSubmittedRow(val targetPath: String, val replacementPath: String, val targetIno: String,
    val targetDev: String, val spoofedIno: String, val spoofedDev: String)
internal data class SUMHMapsSubmissionReceipt(val id: String, val bootId: String, val definitionRevision: String,
    val namespace: String, val instance: String, val generationBefore: String, val generationAfter: String,
    val phase: String, val errno: Int, val rows: List<SUMHMapsSubmittedRow>, val supersedesPendingId: String)
internal data class SUMHMapsSubmission(val status: String, val reason: String,
    val latest: SUMHMapsSubmissionReceipt? = null, val confirmed: SUMHMapsSubmissionReceipt? = null, val stable: Boolean? = null)
internal data class SUMHMapsPreview(
    val status: SUMHMapsPreviewStatus, val reason: String, val errno: Int, val bootId: String?, val configRevision: String?,
    val savedStatus: SUMHMapsSavedStatus, val enabled: Boolean?, val mapsRevision: String?, val rows: List<SUMHMapsPreviewRow>,
    val sample: SUMHMapsPreviewSample, val registeredCount: Int?, val submission: SUMHMapsSubmission,
)
internal data class SUMHMapsPreviewResult(val value: SUMHMapsPreview? = null, val error: String? = null)

internal object SUMHMapsPreviewParser {
    fun reply(reply: SUMHQueryReply): SUMHMapsPreviewResult = try { SUMHMapsPreviewResult(parse(reply.stdout), reply.error) }
        catch (error: Exception) { SUMHMapsPreviewResult(error = listOfNotNull(reply.error, error.message ?: "invalid_preview").joinToString("\n")) }
    fun parse(text: String): SUMHMapsPreview {
        val root = SUMHControlStateParser.document(text, 512 * 1024)
        require(root.integer("schema_version", 1, Int.MAX_VALUE) == 1 && root.text("kind") == "sumh.maps.preview" && root.bool("read_only"))
        val status = when (root.text("status")) {
            "complete" -> SUMHMapsPreviewStatus.Complete; "partial" -> SUMHMapsPreviewStatus.Partial
            "unknown" -> SUMHMapsPreviewStatus.Unknown; "stale" -> SUMHMapsPreviewStatus.Stale; else -> error("unknown_preview_status")
        }
        val saved = root.obj("saved")
        val savedStatus = when (saved.text("status")) {
            "valid" -> SUMHMapsSavedStatus.Valid; "missing" -> SUMHMapsSavedStatus.Missing; "invalid" -> SUMHMapsSavedStatus.Invalid
            "unreadable" -> SUMHMapsSavedStatus.Unreadable; "changed" -> SUMHMapsSavedStatus.Changed; else -> error("unknown_saved_status")
        }
        val enabled = saved.required("enabled").let { if (it.isJsonNull) null else it.booleanValue() }
        val count = saved.integer("count", 0, 256)
        val rawRows = root.required("rows")
        require(rawRows.isJsonArray && rawRows.asJsonArray.size() == count)
        val rows = rawRows.asJsonArray.mapIndexed { index, item ->
            val row = item.objectValue()
            require(row.integer("index", 0, 255) == index)
            val target = row.text("target_path", 4095)
            val replacement = row.text("replacement_path", 255)
            require(Charsets.UTF_8.newEncoder().canEncode(target) && Charsets.UTF_8.newEncoder().canEncode(replacement))
            require(validateSUMHManagedMapsPath(target, 4095) == null && validateSUMHManagedMapsPath(replacement) == null)
            SUMHMapsPreviewRow(index, target, replacement, identity(row.obj("target")), identity(row.obj("replacement")),
                row.bool("same_target_collision"), row.bool("stable"), when (row.text("registered_match")) {
                    "matches" -> SUMHMapsRegistration.Matches; "missing" -> SUMHMapsRegistration.Missing
                    "different" -> SUMHMapsRegistration.Different; "unknown" -> SUMHMapsRegistration.Unknown
                    else -> error("unknown_registration")
                })
        }
        validateMapsTransferRules(rows.map { SUMHUiAdapter.MapsPathRule(it.targetPath, it.replacementPath) })
        val live = root.obj("sample")
        val sample = SUMHMapsPreviewSample(live.unsignedOrNull("namespace_ino"), live.unsignedOrNull("instance_id"),
            live.unsignedOrNull("generation"), live.bool("identities_sampled"), live.bool("definitions_complete"), live.bool("stable"),
            when (live.text("comparison")) { "matches" -> SUMHComparison.Matches; "drift" -> SUMHComparison.Drift
                "unknown" -> SUMHComparison.Unknown; else -> error("unknown_comparison") }, live.text("reason"), live.integer("errno", 0, 4095))
        val limits = root.obj("limits")
        require(limits.integer("max_rules", 1, 256) == 256 && limits.integer("definition_encoded_bytes", 1, 57344) == 57344 &&
            limits.integer("response_bytes", 1, 524288) == 524288)
        val boot = root.nullableText("boot_id", 80)?.also { require(it.isNotEmpty() && '\u0000' !in it) }
        val configRevision = root.revision("config_revision")
        val mapsRevision = saved.revision("maps_revision")
        val registeredCount = root.required("registered_count").let { if (it.isJsonNull) null else root.integer("registered_count", 0, 256) }
        for (token in listOf(sample.namespace, sample.instance, sample.generation)) require(token != "0")
        if (status == SUMHMapsPreviewStatus.Complete) require(savedStatus == SUMHMapsSavedStatus.Valid && enabled != null &&
            boot != null && configRevision != null && mapsRevision != null && sample.stable && sample.definitionsComplete &&
            sample.identitiesSampled && sample.namespace != null && sample.instance != null && sample.generation != null &&
            registeredCount != null && root.integer("errno", 0, 4095) == 0)
        if (registeredCount != null) require(sample.stable && sample.definitionsComplete)
        if (sample.comparison != SUMHComparison.Unknown) require(sample.stable && sample.definitionsComplete && enabled != null)
        if (sample.comparison == SUMHComparison.Matches) require(registeredCount == if (enabled == true) count else 0)
        for (row in rows) if (row.registration != SUMHMapsRegistration.Unknown) require(sample.stable && sample.definitionsComplete &&
            row.stable && !row.collision && row.target.status == SUMHMapsIdentityStatus.Regular && row.replacement.status == SUMHMapsIdentityStatus.Regular)
        if (status == SUMHMapsPreviewStatus.Stale) require(!sample.stable && sample.comparison == SUMHComparison.Unknown &&
            rows.all { it.registration == SUMHMapsRegistration.Unknown })
        return SUMHMapsPreview(status, root.text("reason"), root.integer("errno", 0, 4095), boot,
            configRevision, savedStatus, enabled, mapsRevision, rows, sample, registeredCount,
            submission(root.obj("last_submission")))
    }
    private fun submission(value: JsonObject): SUMHMapsSubmission = try {
        val status = value.text("status", 80)
        val stable = value.get("stable")?.booleanValue()
        require(status in setOf("valid", "missing", "unreadable", "corrupt_or_unsupported", "unavailable"))
        if (status == "unavailable") SUMHMapsSubmission(status, value.text("reason"))
        else {
            val latest = value.required("latest").takeUnless { it.isJsonNull }?.let { receipt(it.objectValue()) }
            val confirmed = value.required("confirmed").takeUnless { it.isJsonNull }?.let { receipt(it.objectValue()) }
            require(status == "valid" || latest == null && confirmed == null)
            require(confirmed == null || confirmed.phase == "acknowledged")
            if (status == "valid") require(latest != null && (latest.phase != "acknowledged" || latest == confirmed))
            SUMHMapsSubmission(status, value.text("reason"), latest, confirmed, stable)
        }
    } catch (_: Exception) { SUMHMapsSubmission("invalid", "invalid_submission_history") }
    private fun receipt(value: JsonObject): SUMHMapsSubmissionReceipt {
        val id = value.text("id", 32).also { require(it.matches(Regex("[0-9a-f]{32}"))) }
        val supersedes = value.text("supersedes_pending_id", 32).also {
            require(it.isEmpty() || it != id && it.matches(Regex("[0-9a-f]{32}")))
        }
        val boot = value.text("boot_id", 80).also { require(it.isNotEmpty() && '\u0000' !in it) }
        fun token(key: String) = value.text(key, 20).also {
            require(it.isEmpty() || it.matches(Regex("[1-9][0-9]{0,19}")) && it.toULongOrNull() != null)
        }
        val raw = value.required("rows")
        require(raw.isJsonArray && raw.asJsonArray.size() <= 256)
        val rows = raw.asJsonArray.map { item -> item.objectValue().let { row ->
            SUMHMapsSubmittedRow(row.text("target_path", 4095), row.text("replacement_path", 255),
                requireNotNull(row.unsignedOrNull("target_ino")), requireNotNull(row.unsignedOrNull("target_dev")),
                requireNotNull(row.unsignedOrNull("spoofed_ino")), requireNotNull(row.unsignedOrNull("spoofed_dev")))
        } }
        validateMapsTransferRules(rows.map { SUMHUiAdapter.MapsPathRule(it.targetPath, it.replacementPath) })
        require(rows.all { it.targetIno != "0" && it.targetDev != "0" && it.spoofedIno != "0" && it.spoofedDev != "0" &&
            it.targetDev.toULong() <= UInt.MAX_VALUE.toULong() && it.spoofedDev.toULong() <= UInt.MAX_VALUE.toULong() })
        require(rows.map { it.targetIno to it.targetDev }.distinct().size == rows.size)
        val phase = value.text("phase", 20).also { require(it in setOf("pending", "acknowledged", "rejected")) }
        val namespace = token("namespace_ino").also { require(it.isNotEmpty()) }
        val instance = token("instance_id"); val before = token("generation_before"); val after = token("generation_after")
        require(instance.isEmpty() == before.isEmpty() && (after.isEmpty() || instance.isNotEmpty()))
        val errno = value.integer("errno", 0, 4095)
        require((phase == "rejected") == (errno != 0) && (phase == "acknowledged" || after.isEmpty()))
        return SUMHMapsSubmissionReceipt(id, boot, requireNotNull(value.revision("definition_revision")), namespace,
            instance, before, after, phase, errno, rows, supersedes)
    }
    private fun identity(raw: JsonObject): SUMHMapsIdentity {
        val status = when (raw.text("status")) {
            "regular" -> SUMHMapsIdentityStatus.Regular; "missing" -> SUMHMapsIdentityStatus.Missing
            "not_regular" -> SUMHMapsIdentityStatus.NotRegular; "unrepresentable" -> SUMHMapsIdentityStatus.Unrepresentable
            "error" -> SUMHMapsIdentityStatus.Error; "unknown" -> SUMHMapsIdentityStatus.Unknown; else -> error("unknown_identity_status")
        }
        val ino = raw.unsignedOrNull("ino"); val dev = raw.unsignedOrNull("dev")
        if (status == SUMHMapsIdentityStatus.Regular) require(ino != null && ino != "0" && dev != null && dev != "0" &&
            dev.toULong() <= UInt.MAX_VALUE.toULong() && raw.integer("errno", 0, 4095) == 0)
        return SUMHMapsIdentity(status, ino, dev, raw.integer("errno", 0, 4095))
    }
    internal fun JsonObject.required(key: String) = get(key) ?: error("missing_$key")
    internal fun JsonElement.objectValue(): JsonObject { require(isJsonObject); return asJsonObject }
    internal fun JsonObject.obj(key: String) = required(key).objectValue()
    internal fun JsonObject.text(key: String, maxBytes: Int = 4096) = required(key).let {
        require(it.isJsonPrimitive && it.asJsonPrimitive.isString)
        it.asString.also { text -> require(text.toByteArray().size <= maxBytes && Charsets.UTF_8.newEncoder().canEncode(text)) }
    }
    internal fun JsonObject.nullableText(key: String, maxBytes: Int = 4096) = required(key).let { if (it.isJsonNull) null else text(key, maxBytes) }
    internal fun JsonObject.revision(key: String) = nullableText(key, 74)?.also { require(it.matches(Regex("sha256-v1:[0-9a-f]{64}"))) }
    internal fun JsonElement.booleanValue(): Boolean { require(isJsonPrimitive && asJsonPrimitive.isBoolean); return asBoolean }
    internal fun JsonObject.bool(key: String) = required(key).booleanValue()
    internal fun JsonObject.integer(key: String, min: Int, max: Int): Int = required(key).let {
        require(it.isJsonPrimitive && it.asJsonPrimitive.isNumber)
        requireNotNull(it.toString().toIntOrNull()).also { number -> require(number in min..max) }
    }
    internal fun JsonObject.unsignedOrNull(key: String) = nullableText(key, 20)?.also {
        require(it.matches(Regex("0|[1-9][0-9]{0,19}")) && it.toULongOrNull() != null)
    }
}
