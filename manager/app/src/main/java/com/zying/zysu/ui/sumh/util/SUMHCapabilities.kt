package com.zying.zysu.ui.sumh.util

import com.google.gson.JsonElement
import com.google.gson.JsonObject
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction

internal const val SUMH_CAPABILITIES_BYTES = 65536
internal enum class SUMHCapabilityStatus { Known, Unsupported, Unknown, Error }
internal enum class SUMHCapabilityBackendStatus { Available, Unavailable, Unknown }
internal enum class SUMHCapabilityPhase { ControlApply, ModulePlanOrBoot, BindingEvents, MapsApply, KernelBuildStage, Unknown }
internal enum class SUMHCapabilityDestination { Debug, Modules, Mount, Isolation, UserHide }

/** Provenance always belongs to the displayed value, including after retention. */
internal data class SUMHCapabilitySection<T>(
    val status: SUMHCapabilityStatus, val source: String, val reason: String, val errno: Int,
    val startedMs: String, val finishedMs: String, val bootId: String?, val value: T?,
    val retained: Boolean = false, val error: String? = null,
    val attempt: SUMHCapabilitySection<T>? = null,
) {
    fun retaining(previous: SUMHCapabilitySection<T>): SUMHCapabilitySection<T> =
        if (status in setOf(SUMHCapabilityStatus.Unknown, SUMHCapabilityStatus.Error) &&
            previous.status == SUMHCapabilityStatus.Known && previous.value != null)
            previous.copy(retained = true, error = error ?: "$reason (errno=$errno)", attempt = copy(attempt = null))
        else this

    fun stale(failure: String): SUMHCapabilitySection<T> =
        if (value != null) copy(retained = true, error = failure, attempt = null) else this
}

internal data class SUMHCapabilitiesSaved(
    val revision: String, val controls: Map<String, Boolean>, val mountHideMode: String,
    val kernelBuildRelease: String, val kernelBuildVersion: String, val kernelBuildApplyStage: String,
    val mountBackend: String, val overlayEnabled: Boolean, val magicEnabled: Boolean,
)
internal data class SUMHCapabilitiesRuntime(
    val sample: SUMHRuntimeSample, val protocolStatus: Int, val kernelProtocol: Int, val capabilityBits: String?,
)
internal data class SUMHCapabilitiesBuild(
    val enabled: Boolean, val release: String, val version: String, val originalRelease: String, val originalVersion: String,
    val consistency: String, val instanceId: String?, val generation: String?,
) {
    val originalKmiLabel: String? get() = KernelBuildState(enabled, release, version, originalRelease, originalVersion, "", "").originalKmiLabel
}
internal data class SUMHCapabilityBackend(val id: String, val status: SUMHCapabilityBackendStatus, val source: String, val reason: String)
internal data class SUMHCapabilitiesBackends(val items: List<SUMHCapabilityBackend>, val liveBackendReason: String)

internal data class SUMHCapabilityFeature(
    val id: String, val supported: Boolean?, val saved: Boolean?, val configured: Boolean?, val active: Boolean?,
    val degraded: Boolean?, val lastSetErrno: Int?, val comparison: SUMHComparison, val reason: String,
    val phase: SUMHCapabilityPhase, val scope: String, val destination: SUMHCapabilityDestination, val retained: Boolean,
)

internal data class SUMHCapabilitiesSnapshot(
    val bootId: String?, val startedMs: String, val finishedMs: String,
    val saved: SUMHCapabilitySection<SUMHCapabilitiesSaved>, val runtime: SUMHCapabilitySection<SUMHCapabilitiesRuntime>,
    val build: SUMHCapabilitySection<SUMHCapabilitiesBuild>, val backends: SUMHCapabilitySection<SUMHCapabilitiesBackends>,
    val owner: SUMHCapabilitySection<String>,
) {
    val sections: List<Pair<String, SUMHCapabilitySection<*>>> get() = listOf("saved" to saved, "runtime" to runtime,
        "build" to build, "backends" to backends, "owner" to owner)
    val hasRetained: Boolean get() = sections.any { it.second.retained }

    fun retaining(previous: SUMHCapabilitiesSnapshot) = copy(saved = saved.retaining(previous.saved),
        runtime = runtime.retaining(previous.runtime), build = build.retaining(previous.build),
        backends = backends.retaining(previous.backends), owner = owner.retaining(previous.owner))

    fun stale(error: String) = copy(saved = saved.stale(error), runtime = runtime.stale(error),
        build = build.stale(error), backends = backends.stale(error), owner = owner.stale(error))

    /** Equality is between sampled flags, never an applied revision or application visibility claim. */
    fun features(): List<SUMHCapabilityFeature> {
        val currentPair = bootId != null && saved.bootId == bootId && runtime.bootId == bootId &&
            !saved.retained && !runtime.retained && saved.status == SUMHCapabilityStatus.Known &&
            runtime.status == SUMHCapabilityStatus.Known && runtime.value?.sample?.status == SUMHRuntimeRead.Known
        val controls = SUMHControlStateParser.controlNames.map { id ->
            val live = runtime.value?.sample?.features?.get(id)
            val desired = saved.value?.controls?.get(id)
            val retained = saved.retained || runtime.retained
            val comparison = when {
                !currentPair || desired == null || live == null || id == "kernel_build_spoof" -> SUMHComparison.Unknown
                !live.supported -> SUMHComparison.Unsupported
                desired == live.configured -> SUMHComparison.Matches
                else -> SUMHComparison.Drift
            }
            val reason = when {
                retained -> "retained_previous_sample"
                id == "kernel_build_spoof" -> "build_strings_and_stage_require_separate_verification"
                live == null -> runtime.reason
                !live.supported -> "control_not_supported"
                live.lastSetErrno != 0 -> "last_control_set_failed"
                live.degraded -> "configured_but_inactive"
                comparison == SUMHComparison.Matches -> "sampled_flags_equal"
                comparison == SUMHComparison.Drift -> "sampled_flags_differ"
                else -> "saved_runtime_comparison_unavailable"
            }
            SUMHCapabilityFeature(id, live?.supported, desired, live?.configured, live?.active, live?.degraded, live?.lastSetErrno,
                comparison, reason, if (id == "kernel_build_spoof") SUMHCapabilityPhase.KernelBuildStage else SUMHCapabilityPhase.ControlApply,
                id, if (id == "kernel_debug") SUMHCapabilityDestination.Debug else SUMHCapabilityDestination.Isolation, retained)
        }
        fun resource(id: String, phase: SUMHCapabilityPhase, destination: SUMHCapabilityDestination, scope: String = id,
            desired: Boolean? = null) = SUMHCapabilityFeature(id, null, desired, null, null, null, null, SUMHComparison.Unknown,
            "resource_runtime_inventory_not_collected", phase, scope, destination, desired != null && saved.retained)
        return controls + listOf(
            resource("user_hide", SUMHCapabilityPhase.BindingEvents, SUMHCapabilityDestination.UserHide),
            resource("maps_rules", SUMHCapabilityPhase.MapsApply, SUMHCapabilityDestination.Isolation, "maps_spoof"),
            resource("modules", SUMHCapabilityPhase.ModulePlanOrBoot, SUMHCapabilityDestination.Modules),
            resource("overlay_xattr_hide", SUMHCapabilityPhase.ModulePlanOrBoot, SUMHCapabilityDestination.Mount,
                desired = saved.value?.controls?.get("overlay_xattr_hide")),
        )
    }
}

internal data class SUMHCapabilitiesResult(val snapshot: SUMHCapabilitiesSnapshot? = null, val error: String? = null) {
    fun retaining(previous: SUMHCapabilitiesResult): SUMHCapabilitiesResult = when {
        snapshot != null && previous.snapshot != null -> copy(snapshot = snapshot.retaining(previous.snapshot))
        snapshot == null && previous.snapshot != null -> copy(snapshot = previous.snapshot.stale(error ?: "capability_query_failed"))
        else -> this
    }
}

/** This compact contract reuses the existing strict bounded JSON reader without raising any global limit. */
internal object SUMHCapabilitiesParser {
    fun capabilities(reply: SUMHQueryReply): SUMHSchemaSupport = try {
        require(reply.error == null)
        val root = SUMHControlStateParser.document(reply.stdout)
        val keys = listOf("capability_snapshot_schema", "capability_snapshot_transport_schema",
            "capability_snapshot_read_only", "capability_snapshot_response_bytes")
        if (keys.none(root::has)) SUMHSchemaSupport.NotAdvertised
        else {
            val schema = root.int("capability_snapshot_schema", 1, Int.MAX_VALUE)
            val transport = root.int("capability_snapshot_transport_schema", 1, Int.MAX_VALUE)
            require(root.bool("capability_snapshot_read_only") && root.int("capability_snapshot_response_bytes", 1, Int.MAX_VALUE) == SUMH_CAPABILITIES_BYTES)
            if (schema == 1 && transport == 1) SUMHSchemaSupport.Supported else SUMHSchemaSupport.Future
        }
    } catch (_: Exception) { if (reply.error != null) SUMHSchemaSupport.Unavailable else SUMHSchemaSupport.Invalid }

    fun envelope(raw: ByteArray): SUMHQueryReply {
        require(raw.size <= 512 * 1024) { "capability_envelope_capacity" }
        val text = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(raw)).toString()
        return SUMHControlStateParser.reply(text, SUMH_CAPABILITIES_BYTES, 512 * 1024)
    }

    fun reply(reply: SUMHQueryReply): SUMHCapabilitiesResult = try {
        SUMHCapabilitiesResult(parse(reply.stdout), reply.error)
    } catch (failure: Exception) {
        SUMHCapabilitiesResult(error = listOfNotNull(reply.error, failure.message ?: "capability_snapshot_invalid").joinToString("\n"))
    }

    fun parse(text: String): SUMHCapabilitiesSnapshot {
        val root = SUMHControlStateParser.document(text, SUMH_CAPABILITIES_BYTES)
        root.fields("schema_version", "resource", "observation", "atomic", "boot_id", "sample_started_monotonic_ms",
            "sample_finished_monotonic_ms", "sections")
        require(root.int("schema_version", 1, Int.MAX_VALUE) == 1 && root.text("resource") == "capability_snapshot" &&
            root.text("observation") == "control_plane_only" && !root.bool("atomic"))
        val boot = root.nullableText("boot_id", 80)?.also { require(it.isNotEmpty()) }
        val started = root.unsigned("sample_started_monotonic_ms"); val finished = root.unsigned("sample_finished_monotonic_ms")
        require(started.toULong() <= finished.toULong())
        val sections = root.obj("sections").also { it.fields("saved", "runtime", "build", "backends", "owner") }
        fun <T> section(name: String, sources: Set<String>, parser: (JsonObject) -> T): SUMHCapabilitySection<T> {
            val value = sections.obj(name)
            value.fields("status", "reason", "errno", "source", "sample_started_monotonic_ms", "sample_finished_monotonic_ms", "value")
            val status = when (value.text("status")) {
                "known" -> SUMHCapabilityStatus.Known; "unsupported" -> SUMHCapabilityStatus.Unsupported
                "unknown" -> SUMHCapabilityStatus.Unknown; "error" -> SUMHCapabilityStatus.Error
                else -> error("capability_section_status")
            }
            val source = value.text("source", 100).also { require(it in sources) { "capability_section_source:$name" } }
            val reason = value.text("reason", 256)
            val errno = value.int("errno", 0, 4095)
            val from = value.unsigned("sample_started_monotonic_ms"); val to = value.unsigned("sample_finished_monotonic_ms")
            require(from.toULong() in started.toULong()..finished.toULong() && to.toULong() in from.toULong()..finished.toULong())
            val payload = value.required("value")
            if (boot == null) require(status == SUMHCapabilityStatus.Unknown && payload.isJsonNull &&
                reason in setOf("boot_identity_unavailable", "boot_changed_during_sample", "capability_query_guard_unavailable",
                    "reply_memory_unavailable", "capability_snapshot_response_capacity", "capability_query_failed"))
            else if (name != "runtime") require((status == SUMHCapabilityStatus.Known) == !payload.isJsonNull)
            val parsed = if (payload.isJsonNull) null else parser(payload.objectValue())
            return SUMHCapabilitySection(status, source, reason, errno, from, to, boot, parsed,
                error = if (status == SUMHCapabilityStatus.Error) "$reason (errno=$errno)" else null)
        }
        val saved = section("saved", setOf("saved_config"), ::saved)
        val runtime = section("runtime", setOf("kernel_ioctl")) { obj ->
            val sample = requireNotNull(SUMHControlStateParser.runtime(SUMHQueryReply(obj.toString())).value)
            require(sample.bootId == boot && sample.startedMs.toULong() >= started.toULong() && sample.finishedMs.toULong() <= finished.toULong())
            val bits = obj.required("capabilities").let { if (it.isJsonNull) null else {
                require(it.isJsonPrimitive && it.asJsonPrimitive.isNumber)
                it.toString().also { n -> require(n.matches(Regex("0|[1-9][0-9]{0,19}")) && n.toULongOrNull() != null) }
            } }
            SUMHCapabilitiesRuntime(sample, obj.int("protocol_status", 0, 3), obj.int("kernel_protocol", 0, Int.MAX_VALUE), bits)
        }
        runtime.value?.let {
            val expected = when (it.sample.status) {
                SUMHRuntimeRead.Known -> SUMHCapabilityStatus.Known; SUMHRuntimeRead.Unsupported -> SUMHCapabilityStatus.Unsupported
                SUMHRuntimeRead.Error -> SUMHCapabilityStatus.Error; SUMHRuntimeRead.Unavailable -> SUMHCapabilityStatus.Unknown
            }
            require(runtime.status == expected && runtime.reason == it.sample.reason && runtime.errno == it.sample.errno)
            require(it.sample.startedMs.toULong() >= runtime.startedMs.toULong() && it.sample.finishedMs.toULong() <= runtime.finishedMs.toULong())
        }
        if (boot != null && runtime.value == null) require(runtime.status == SUMHCapabilityStatus.Error && runtime.reason == "runtime_encoding_failed")
        val build = section("build", setOf("initial_uts_ioctl"), ::build)
        val backends = section("backends", setOf("capability_probes"), ::backends)
        val owner = section<String>("owner", setOf("not_collected")) { error("runtime_owner_not_collected") }
        require(owner.status == SUMHCapabilityStatus.Unknown && owner.value == null)
        return SUMHCapabilitiesSnapshot(boot, started, finished, saved, runtime, build, backends, owner)
    }

    private fun saved(value: JsonObject): SUMHCapabilitiesSaved {
        val controls = SUMHControlStateParser.controlNames + "overlay_xattr_hide"
        val keys = controls.map { "enable_$it" }
        value.fields(*(keys + listOf("revision", "revision_scope", "mount_hide_mode", "kernel_build_release", "kernel_build_version",
            "kernel_build_apply_stage", "mount_backend", "overlayfs_enabled", "magic_mount_enabled")).toTypedArray())
        val revision = value.text("revision", 74).also { require(it.matches(Regex("sha256-v1:[0-9a-f]{64}"))) }
        require(value.text("revision_scope") == "saved_config_bytes_only")
        val release = value.text("kernel_build_release", 64).also { require(validKernelBuildField(it, true)) }
        val version = value.text("kernel_build_version", 64).also { require(validKernelBuildField(it)) }
        val stage = value.text("kernel_build_apply_stage").also { require(it in setOf(KERNEL_BUILD_EARLY_STAGE, KERNEL_BUILD_LATE_STAGE)) }
        val backend = value.text("mount_backend").also { require(it in setOf("auto", "sumh", "overlay", "magic", "none")) }
        val mode = value.text("mount_hide_mode").also { require(it in setOf("normal", "aggressive")) }
        return SUMHCapabilitiesSaved(revision, controls.associateWith { value.bool("enable_$it") }, mode,
            release, version, stage, backend, value.bool("overlayfs_enabled"), value.bool("magic_mount_enabled"))
    }

    private fun build(value: JsonObject): SUMHCapabilitiesBuild {
        value.fields("enabled", "release", "version", "original_release", "original_version", "consistency", "instance_id", "generation")
        val consistency = value.text("consistency").also { require(it in setOf("control_generation_bracketed", "independently_sampled")) }
        val instance = value.nullableUnsigned("instance_id"); val generation = value.nullableUnsigned("generation")
        require(if (consistency == "control_generation_bracketed") instance != null && generation != null else instance == null && generation == null)
        // Readback is observed UTS text, not an editable override. The native reader
        // permits spaces in release while retaining the byte and control bounds.
        fun field(key: String) = value.text(key, 64).also {
            require(it.isNotEmpty() && it.none { character -> character < '\u0020' || character == '\u007f' })
        }
        return SUMHCapabilitiesBuild(value.bool("enabled"), field("release"), field("version"), field("original_release"),
            field("original_version"), consistency, instance, generation)
    }

    private fun backends(value: JsonObject): SUMHCapabilitiesBackends {
        value.fields("scope", "live_backend", "live_backend_reason", "items")
        require(value.text("scope") == "capability_only" && value.text("live_backend") == "unknown" &&
            value.text("live_backend_reason") == "runtime_backend_ownership_not_collected")
        val raw = value.required("items").also { require(it.isJsonArray && it.asJsonArray.size() == 3) }.asJsonArray
        val items = raw.map { entry ->
            val item = entry.objectValue().also { it.fields("id", "status", "source", "reason") }
            val id = item.text("id")
            val source = item.text("source")
            require(source == mapOf("sumh" to "kernel_protocol", "overlay" to "proc_filesystems", "magic" to "controller_strategy")[id])
            val status = when (item.text("status")) {
                "available" -> SUMHCapabilityBackendStatus.Available; "unavailable" -> SUMHCapabilityBackendStatus.Unavailable
                "unknown" -> SUMHCapabilityBackendStatus.Unknown; else -> error("backend_capability_status")
            }
            SUMHCapabilityBackend(id, status, source, item.text("reason", 256))
        }
        require(items.map { it.id }.toSet() == setOf("sumh", "overlay", "magic"))
        return SUMHCapabilitiesBackends(items, value.text("live_backend_reason"))
    }

    private fun JsonElement.objectValue(): JsonObject { require(isJsonObject); return asJsonObject }
    private fun JsonObject.required(key: String): JsonElement = get(key) ?: error("Missing $key")
    private fun JsonObject.obj(key: String) = required(key).objectValue()
    private fun JsonObject.fields(vararg names: String) { require(keySet() == names.toSet()) { "capability_fields" } }
    private fun JsonObject.text(key: String, max: Int = 4096): String = required(key).let {
        require(it.isJsonPrimitive && it.asJsonPrimitive.isString)
        it.asString.also { text ->
            require(text.toByteArray(Charsets.UTF_8).size <= max && '\u0000' !in text)
            var index = 0
            while (index < text.length) {
                val c = text[index++]
                if (c.isHighSurrogate()) require(index < text.length && text[index++].isLowSurrogate())
                else require(!c.isLowSurrogate())
            }
        }
    }
    private fun JsonObject.nullableText(key: String, max: Int) = if (required(key).isJsonNull) null else text(key, max)
    private fun JsonObject.bool(key: String) = required(key).let { require(it.isJsonPrimitive && it.asJsonPrimitive.isBoolean); it.asBoolean }
    private fun JsonObject.int(key: String, min: Int, max: Int): Int = required(key).let {
        require(it.isJsonPrimitive && it.asJsonPrimitive.isNumber)
        requireNotNull(it.toString().toIntOrNull()).also { number -> require(number in min..max) }
    }
    private fun JsonObject.unsigned(key: String) = text(key, 20).also {
        require(it.matches(Regex("0|[1-9][0-9]{0,19}")) && it.toULongOrNull() != null)
    }
    private fun JsonObject.nullableUnsigned(key: String) = if (required(key).isJsonNull) null else unsigned(key).also { require(it != "0") }
}
