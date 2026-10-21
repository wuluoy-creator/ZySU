package com.zying.zysu.ui.sumh.util

import com.google.gson.JsonArray
import com.google.gson.JsonElement
import com.google.gson.JsonNull
import com.google.gson.JsonObject
import com.google.gson.JsonPrimitive
import com.google.gson.Strictness
import com.google.gson.stream.JsonReader
import com.google.gson.stream.JsonToken
import org.json.JSONObject
import java.io.StringReader

/** Query formats are negotiated separately from kernel feature bits and protocol versions. */
enum class SUMHSchemaSupport { Supported, NotAdvertised, Future, Invalid, Unavailable }
data class SUMHStateCapabilities(
    val state: SUMHSchemaSupport = SUMHSchemaSupport.Unavailable,
    val runtime: SUMHSchemaSupport = SUMHSchemaSupport.Unavailable,
)
data class SUMHStateQuery<T>(val value: T? = null, val error: String? = null, val retained: Boolean = false) {
    fun retaining(previous: SUMHStateQuery<T>): SUMHStateQuery<T> =
        if (value == null && previous.value != null) copy(value = previous.value, retained = true) else this
}
enum class SUMHRuntimeRead { Known, Unsupported, Error, Unavailable }
data class SUMHRuntimeFeature(
    val supported: Boolean, val configured: Boolean, val active: Boolean,
    val degraded: Boolean, val lastSetErrno: Int,
)
data class SUMHRuntimeSample(
    val status: SUMHRuntimeRead, val reason: String, val errno: Int, val bootId: String?,
    val startedMs: String, val finishedMs: String, val instanceId: String?, val generation: String?,
    val conditionalSetSupported: Boolean?, val features: Map<String, SUMHRuntimeFeature>,
)
enum class SUMHComparison { Matches, Drift, Unknown, Unsupported, Deferred }
data class SUMHVerification(val revision: String, val bootId: String, val instanceId: String, val generation: String)
data class SUMHFeatureObservation(
    val desiredRevision: String?, val appliedRevision: String?, val comparison: SUMHComparison,
    val reason: String, val lastVerified: SUMHVerification?, val configured: Boolean? = null,
    val active: Boolean? = null, val lastSetErrno: Int? = null,
    val verificationScope: String? = null, val bindingConsistency: String? = null,
    val pendingCount: Int? = null, val errorCount: Int? = null, val suspendedCount: Int? = null,
)
enum class SUMHOperationResult { Completed, FailedOrPartial, Unknown }
data class SUMHLastOperation(
    val id: String, val name: String, val bootId: String, val started: Boolean,
    val result: SUMHOperationResult, val exitCode: Int, val errno: Int, val detail: String,
)
enum class SUMHHistoryRead { Valid, Missing, Unreadable, CorruptOrUnsupported, Invalid }
data class SUMHOperationHistory(val status: SUMHHistoryRead, val error: String?, val last: SUMHLastOperation?)
data class SUMHStateSample(
    val bootId: String?, val runtime: SUMHStateQuery<SUMHRuntimeSample>,
    val features: Map<String, SUMHFeatureObservation>, val history: SUMHOperationHistory,
)
data class SUMHControlState(
    val capabilities: SUMHStateCapabilities = SUMHStateCapabilities(),
    val state: SUMHStateQuery<SUMHStateSample> = SUMHStateQuery(),
    val runtime: SUMHStateQuery<SUMHRuntimeSample> = SUMHStateQuery(),
) {
    fun retaining(previous: SUMHControlState) = copy(state = state.retaining(previous.state), runtime = runtime.retaining(previous.runtime))
    fun stale(error: String) = copy(
        state = state.copy(error = error, retained = state.value != null),
        runtime = runtime.copy(error = error, retained = runtime.value != null),
    )
}

/** A nonzero command may still carry useful, valid state JSON (e.g. unreadable history). */
data class SUMHQueryReply(val stdout: String, val error: String? = null)

object SUMHControlStateParser {
    val controlNames = listOf("kernel_debug", "stealth", "mount_hide", "maps_spoof", "statfs_spoof", "kernel_build_spoof")
    private val uintPattern = Regex("0|[1-9][0-9]{0,19}")
    private val revisionPattern = Regex("sha256-v1:[0-9a-f]{64}")

    fun capabilities(meta: JSONObject): SUMHStateCapabilities {
        fun field(name: String): SUMHSchemaSupport {
            if (!meta.has(name)) return SUMHSchemaSupport.NotAdvertised
            val value = meta.opt(name)
            if (value !is Number || value.toString().toIntOrNull() == null) return SUMHSchemaSupport.Invalid
            return when (value.toInt()) { 1 -> SUMHSchemaSupport.Supported; in 2..Int.MAX_VALUE -> SUMHSchemaSupport.Future; else -> SUMHSchemaSupport.Invalid }
        }
        return SUMHStateCapabilities(field("state_ledger_schema"), field("runtime_status_schema"))
    }

    fun reply(raw: String, maxPayloadBytes: Int = 256 * 1024,
        maxEnvelopeBytes: Int = maxOf(384 * 1024, maxPayloadBytes)): SUMHQueryReply {
        require(maxPayloadBytes in 1..1024 * 1024)
        require(maxEnvelopeBytes in maxPayloadBytes..4 * 1024 * 1024)
        val value = document(raw, maxEnvelopeBytes)
        val ok = value.bool("ok")
        // Transport adapters can use negative failure codes. The payload is still
        // independently parseable; only zero together with ok confirms success.
        val exit = value.int("exit_code", Int.MIN_VALUE, Int.MAX_VALUE)
        val errno = value.int("errno", Int.MIN_VALUE, Int.MAX_VALUE)
        val stderr = value.text("stderr", 64 * 1024)
        return SUMHQueryReply(value.text("stdout", maxPayloadBytes),
            if (ok && exit == 0) null else stderr.take(1500).ifEmpty { "SUMHP query failed (exit=$exit, errno=$errno)" })
    }

    fun runtime(reply: SUMHQueryReply): SUMHStateQuery<SUMHRuntimeSample> = query(reply) { runtimeObject(it) }
    fun state(reply: SUMHQueryReply): SUMHStateQuery<SUMHStateSample> = query(reply) { root ->
        root.schema()
        require(root.text("consistency") == "sampled_definitions_and_kernel_control")
        val boot = root.nullableText("boot_id", 80)
        val runtime = try { SUMHStateQuery(runtimeObject(root.obj("runtime"))) }
            catch (error: Exception) { SUMHStateQuery<SUMHRuntimeSample>(error = "Invalid runtime state: ${error.message}") }
        require(runtime.value == null || runtime.value.bootId == boot) { "Runtime boot identity differs" }
        val history = try { history(root.obj("history")) }
            catch (error: Exception) { SUMHOperationHistory(SUMHHistoryRead.Invalid, "Invalid operation history: ${error.message}", null) }
        val entries = root.obj("features")
        require(entries.size() <= 64)
        val features = entries.entrySet().associate { (name, raw) ->
            require(name.length <= 80)
            val item = raw.objectValue()
            val desired = item.revision("desired_revision")
            val applied = item.revision("applied_revision")
            val comparison = when (item.text("status")) {
                "matches" -> SUMHComparison.Matches; "drift" -> SUMHComparison.Drift
                "unknown" -> SUMHComparison.Unknown; "unsupported" -> SUMHComparison.Unsupported
                "deferred" -> SUMHComparison.Deferred; else -> error("Unknown feature status")
            }
            val verified = item.required("last_verified").takeUnless { it.isJsonNull }?.objectValue()?.let { v ->
                SUMHVerification(requireNotNull(v.revision("revision")), v.text("boot_id", 80).also { require(it.isNotEmpty()) },
                    v.unsigned("instance_id", nonzero = true), v.unsigned("generation", nonzero = true))
            }
            if (applied != null) require(comparison == SUMHComparison.Matches && applied == desired &&
                verified?.revision == applied && verified.bootId == boot && verified.instanceId == runtime.value?.instanceId) {
                "Applied revision lacks current verification"
            }
            val detail = item.obj("detail")
            name to SUMHFeatureObservation(desired, applied, comparison, item.text("reason"), verified,
                detail.optionalBool("configured"), detail.optionalBool("active"), detail.optionalInt("last_set_errno", -4095, 0),
                detail.optionalText("verification_scope"), detail.optionalText("binding_consistency"),
                detail.optionalInt("pending_count", 0, Int.MAX_VALUE), detail.optionalInt("error_count", 0, Int.MAX_VALUE),
                detail.optionalInt("suspended_count", 0, Int.MAX_VALUE))
        }
        SUMHStateSample(boot, runtime, features, history)
    }

    private fun runtimeObject(root: JsonObject): SUMHRuntimeSample {
        root.schema()
        require(root.text("source") == "kernel_ioctl" && root.text("observation") == "control_plane_only")
        val status = when (root.text("status")) {
            "known" -> SUMHRuntimeRead.Known; "unsupported" -> SUMHRuntimeRead.Unsupported
            "error" -> SUMHRuntimeRead.Error; "unavailable" -> SUMHRuntimeRead.Unavailable
            else -> error("Unknown runtime status")
        }
        val started = root.unsigned("sample_started_monotonic_ms")
        val finished = root.unsigned("sample_finished_monotonic_ms")
        require(finished.toULong() >= started.toULong())
        val conditional = root.required("conditional_set_supported").let { if (it.isJsonNull) null else it.booleanValue() }
        root.int("protocol_status", -1, 10)
        root.int("kernel_protocol", 0, Int.MAX_VALUE)
        root.required("capabilities").let { if (!it.isJsonNull) require(it.isJsonPrimitive && it.asJsonPrimitive.isNumber && it.toString().toULongOrNull() != null) }
        val runtime = root.required("runtime")
        var instance: String? = null
        var generation: String? = null
        val entries = mutableMapOf<String, SUMHRuntimeFeature>()
        if (status == SUMHRuntimeRead.Known) {
            val live = runtime.objectValue()
            instance = live.unsigned("instance_id", nonzero = true)
            generation = live.unsigned("generation", nonzero = true)
            require(live.text("consistency") == "control_serialized_readiness_sampled")
            require(live.text("generation_scope") == "control_mutations_only")
            live.int("mount_hide_mode", 0, 1)
            val values = live.required("features")
            require(values.isJsonArray && values.asJsonArray.size() <= 64)
            val ids = mutableSetOf<Int>()
            for (raw in values.asJsonArray) {
                val item = raw.objectValue()
                val id = item.int("id", 1, Int.MAX_VALUE)
                require(ids.add(id))
                val name = item.text("name", 80)
                if (id <= controlNames.size) require(name == controlNames[id - 1])
                val feature = SUMHRuntimeFeature(item.bool("supported"), item.bool("configured"), item.bool("active"),
                    item.bool("degraded"), item.int("last_set_errno", -4095, 0))
                require(!feature.active || feature.supported && feature.configured)
                require(feature.degraded == (feature.configured && !feature.active))
                require(entries.put(name, feature) == null)
            }
            require(controlNames.all { it in entries }) { "Missing runtime feature" }
        } else require(runtime.isJsonNull) { "Unexpected runtime data" }
        return SUMHRuntimeSample(status, root.text("reason"), root.int("errno", 0, 4095), root.nullableText("boot_id", 80),
            started, finished, instance, generation, conditional, entries)
    }

    private fun history(value: JsonObject): SUMHOperationHistory {
        val status = when (value.text("status")) {
            "valid" -> SUMHHistoryRead.Valid; "missing" -> SUMHHistoryRead.Missing
            "unreadable" -> SUMHHistoryRead.Unreadable; "corrupt_or_unsupported" -> SUMHHistoryRead.CorruptOrUnsupported
            else -> error("Unknown history status")
        }
        val error = value.text("error").ifEmpty { null }
        val raw = value.required("last_operation")
        if (status != SUMHHistoryRead.Valid) { require(raw.isJsonNull); return SUMHOperationHistory(status, error, null) }
        // The ledger may contain reservations only. They are not executed
        // commands and are deliberately absent from this legacy projection.
        if (raw.isJsonNull) return SUMHOperationHistory(status, error, null)
        val op = raw.objectValue()
        val started = when (op.text("phase")) { "started" -> true; "finished" -> false; else -> error("Unknown operation phase") }
        val result = when (op.text("result")) {
            "completed" -> SUMHOperationResult.Completed; "failed_or_partial" -> SUMHOperationResult.FailedOrPartial
            "outcome_unknown" -> SUMHOperationResult.Unknown; else -> error("Unknown operation result")
        }
        val exit = op.int("exit_code", 0, 255)
        require(if (started) result == SUMHOperationResult.Unknown else
            result == if (exit == 0) SUMHOperationResult.Completed else SUMHOperationResult.FailedOrPartial)
        return SUMHOperationHistory(status, error, SUMHLastOperation(
            op.text("id", 32).also { require(it.matches(Regex("[0-9a-f]{32}"))) }, op.text("name", 80),
            op.text("boot_id", 80).also { require(it.isNotEmpty()) }, started, result, exit,
            op.int("errno", 0, 4095), op.text("detail", 1024)))
    }

    private fun <T> query(reply: SUMHQueryReply, parse: (JsonObject) -> T): SUMHStateQuery<T> = try {
        SUMHStateQuery(parse(document(reply.stdout)), reply.error)
    } catch (error: Exception) { SUMHStateQuery(error = listOfNotNull(reply.error, "Invalid SUMH state: ${error.message}").joinToString("\n")) }

    // JsonReader STRICT plus explicit duplicate-key checks: no coercion, comments, trailing JSON or silent last-key-wins.
    internal fun document(text: String, maxBytes: Int = 384 * 1024): JsonObject {
        require(text.toByteArray(Charsets.UTF_8).size <= maxBytes) { "Response exceeds query budget" }
        val reader = JsonReader(StringReader(text)).apply { strictness = Strictness.STRICT }
        var nodes = 0
        fun read(depth: Int): JsonElement {
            require(depth <= 16 && ++nodes <= 8192)
            return when (reader.peek()) {
                JsonToken.BEGIN_OBJECT -> JsonObject().also { obj ->
                    reader.beginObject()
                    while (reader.hasNext()) { val key = reader.nextName(); require(!obj.has(key)); obj.add(key, read(depth + 1)) }
                    reader.endObject()
                }
                JsonToken.BEGIN_ARRAY -> JsonArray().also { list -> reader.beginArray(); while (reader.hasNext()) list.add(read(depth + 1)); reader.endArray() }
                JsonToken.STRING -> JsonPrimitive(reader.nextString())
                JsonToken.NUMBER -> JsonPrimitive(reader.nextString().toBigDecimal())
                JsonToken.BOOLEAN -> JsonPrimitive(reader.nextBoolean())
                JsonToken.NULL -> { reader.nextNull(); JsonNull.INSTANCE }
                else -> error("Invalid JSON token")
            }
        }
        return reader.use { val value = read(0).objectValue(); require(reader.peek() == JsonToken.END_DOCUMENT); value }
    }
    private fun JsonElement.objectValue(): JsonObject { require(isJsonObject); return asJsonObject }
    private fun JsonObject.required(key: String): JsonElement = get(key) ?: error("Missing $key")
    private fun JsonObject.obj(key: String) = required(key).objectValue()
    private fun JsonObject.text(key: String, max: Int = 4096): String = required(key).let {
        require(it.isJsonPrimitive && it.asJsonPrimitive.isString) { "Invalid $key" }
        it.asString.also { value -> require(value.length <= max && '\u0000' !in value) }
    }
    private fun JsonObject.nullableText(key: String, max: Int) = if (required(key).isJsonNull) null else text(key, max)
    private fun JsonObject.optionalText(key: String) = if (has(key)) text(key) else null
    private fun JsonElement.booleanValue(): Boolean { require(isJsonPrimitive && asJsonPrimitive.isBoolean); return asBoolean }
    private fun JsonObject.bool(key: String) = required(key).booleanValue()
    private fun JsonObject.optionalBool(key: String) = if (has(key)) bool(key) else null
    private fun JsonObject.int(key: String, min: Int, max: Int): Int = required(key).let {
        require(it.isJsonPrimitive && it.asJsonPrimitive.isNumber)
        requireNotNull(it.toString().toIntOrNull()).also { number -> require(number in min..max) }
    }
    private fun JsonObject.optionalInt(key: String, min: Int, max: Int) = if (has(key)) int(key, min, max) else null
    private fun JsonObject.unsigned(key: String, nonzero: Boolean = false) = text(key, 20).also {
        require(it.matches(uintPattern) && it.toULongOrNull() != null && (!nonzero || it != "0")) { "Invalid unsigned $key" }
    }
    private fun JsonObject.revision(key: String) = nullableText(key, 74)?.also { require(it.matches(revisionPattern)) }
    private fun JsonObject.schema() { require(int("schema_version", 1, Int.MAX_VALUE) == 1) { "Unsupported schema" } }
}
