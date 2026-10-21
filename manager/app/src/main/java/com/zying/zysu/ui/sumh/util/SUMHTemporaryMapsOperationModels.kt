package com.zying.zysu.ui.sumh.util

import com.google.gson.JsonElement
import com.google.gson.JsonObject
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.booleanValue
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.bool
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.integer
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.nullableText
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.objectValue
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.required
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.revision
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.text
import kotlinx.coroutines.flow.StateFlow

/** Canonical selectors bind a historical operation. Loading them never authorizes a new deletion. */
internal data class SUMHTemporaryMapsOperationKey(val targetIno: String, val targetDev: String,
    val instance: String, val generation: String) {
    init {
        unsigned(targetIno); unsigned(targetDev, device = true)
        unsigned(instance, nonzero = true); unsigned(generation, nonzero = true)
    }
    private fun unsigned(value: String, device: Boolean = false, nonzero: Boolean = false) {
        require(value.matches(Regex("0|[1-9][0-9]{0,19}")))
        val number = requireNotNull(value.toULongOrNull())
        require((!device || number <= UInt.MAX_VALUE.toULong()) && (!nonzero || number > 0u))
    }
}
internal fun temporaryMapsOperationKey(request: SUMHTemporaryMapsRemoveRequest) =
    SUMHTemporaryMapsOperationKey(request.row.targetIno, request.row.targetDev, request.token.instance, request.token.generation)
internal fun temporaryMapsOperationArguments(key: SUMHTemporaryMapsOperationKey) =
    listOf("sumh", "maps", "remove", key.targetIno, key.targetDev, "--instance", key.instance, "--generation", key.generation)

internal data class SUMHTemporaryMapsTrackedOperation(val id: String, val requestRevision: String,
    val bootId: String, val key: SUMHTemporaryMapsOperationKey, val stage: String,
    val response: String = "", val observation: String = "", val error: String = "", val recovered: String = "") {
    val name get() = "sumh maps remove"
    fun fields() = listOf(id, requestRevision, bootId, key.targetIno, key.targetDev, key.instance, key.generation,
        stage, response, observation, error, recovered)
    fun submission() = response.takeIf { it.isNotEmpty() }?.let {
        SUMHTemporaryMapsOperationParser.validateDocument(it, SUMH_OPERATION_SUBMISSION_BYTES)
        SUMHOperationsParser.submission(SUMHQueryReply(it), requestRevision, id, bootId).also { result ->
            result.observed?.let { record -> require(record.name == name && record.bootId == bootId &&
                record.requestRevision == requestRevision) { "temporary_maps_operation_submission_identity_mismatch" } }
        }
    }
    fun resource(): SUMHTemporaryMapsRemoveResult? = submission()?.let { receipt ->
        if (receipt.ran != true || receipt.status !in setOf("command_completed", "failed_or_partial") ||
            receipt.stdout.encoding != "utf-8" || !receipt.stdout.complete || !receipt.stderr.complete) null
        else SUMHTemporaryMapsOperationParser.receipt(receipt.stdout.value, this,
            requireNotNull(receipt.exitCode), requireNotNull(receipt.errno))
    }
    fun history() = observation.takeIf { it.isNotEmpty() }?.let {
        SUMHTemporaryMapsOperationParser.validateDocument(it, 64 * 1024)
        SUMHOperationsParser.observation(SUMHQueryReply(it), id, requestRevision, name, bootId)
    }
    fun recovery() = recovered.takeIf { it.isNotEmpty() }?.let {
        SUMHTemporaryMapsOperationParser.resourceResult(SUMHQueryReply(it), this)
    }
    val safelyPrunable get() = runCatching {
        stage == "received" && resource()?.status == SUMHTemporaryMapsRemoveStatus.Deleted ||
            recovery()?.resource?.status == SUMHTemporaryMapsRemoveStatus.Deleted
    }.getOrDefault(false)
}
internal data class SUMHTemporaryMapsOperationResourceResult(val status: String, val record: SUMHRecordedOperation?,
    val resource: SUMHTemporaryMapsRemoveResult?, val raw: String, val error: String,
    val bootId: String? = null, val ledger: String = "", val receiptStatus: String = "")
internal data class SUMHTemporaryMapsTrackingSnapshot(val loaded: Boolean = false,
    val records: List<SUMHTemporaryMapsTrackedOperation> = emptyList(), val error: String? = null)
internal data class SUMHTemporaryMapsTrackedRemoval(val result: SUMHTemporaryMapsRemoveResult,
    val operationId: String? = null, val dispatched: Boolean = false, val error: String? = null)
internal interface SUMHTemporaryMapsTrackingService {
    val snapshot: StateFlow<SUMHTemporaryMapsTrackingSnapshot>
    suspend fun capabilities(): SUMHTemporaryMapsSupport
    suspend fun load(): SUMHTemporaryMapsTrackingSnapshot
    suspend fun remove(request: SUMHTemporaryMapsRemoveRequest): SUMHTemporaryMapsTrackedRemoval
    suspend fun recover(id: String, waitMs: Int = 0): Boolean
    suspend fun forget(id: String): Boolean
}

internal object SUMHTemporaryMapsOperationParser {
    internal fun validateDocument(raw: String, limit: Int) { document(raw, limit) }
    private fun document(raw: String, limit: Int): JsonObject {
        require(validHideDefinitionText(raw, limit))
        return SUMHControlStateParser.document(raw, limit).also(::strings)
    }
    private fun strings(value: JsonElement) {
        when {
            value.isJsonObject -> value.asJsonObject.entrySet().forEach { (key, child) ->
                require(validHideDefinitionText(key, 64 * 1024)); strings(child)
            }
            value.isJsonArray -> value.asJsonArray.forEach(::strings)
            value.isJsonPrimitive && value.asJsonPrimitive.isString -> require(validHideDefinitionText(value.asString, SUMH_OPERATION_SUBMISSION_BYTES))
        }
    }
    private fun JsonObject.nullableBool(key: String) = required(key).let { if (it.isJsonNull) null else it.booleanValue() }
    private fun boot(value: String) = value.also { require(it.isNotEmpty() && validHideDefinitionText(it, 80)) }

    /** Missing support is distinct from malformed/future data. Only explicit support authorizes prepare. */
    fun capabilities(reply: SUMHQueryReply): SUMHTemporaryMapsSupport { return try {
        require(reply.error == null)
        val root = document(reply.stdout, 64 * 1024)
        var supported = true
        for (field in listOf("operation_prepare", "operation_execute", "operation_show", "operation_wait", "operation_result",
                "operation_reserved_id", "operation_local_temporary_maps_remove", "maps_temporary_tracked_manager")) {
            if (!root.has(field)) supported = false else if (!root.bool(field)) supported = false
        }
        for ((field, expected) in mapOf("operation_manager_transport_schema" to 1,
                "operation_manager_temporary_maps_schema" to 2, "operation_submission_schema" to 1,
                "operation_query_schema" to 2, "operation_resource_result_schema" to 1)) {
            if (!root.has(field)) supported = false
            else {
                val version = root.integer(field, 1, Int.MAX_VALUE)
                if (version > expected) return SUMHTemporaryMapsSupport.Unknown
                if (version != expected) supported = false
            }
        }
        if (!root.has("operation_resource_result_resources")) supported = false
        else {
            val values = root.required("operation_resource_result_resources").also { require(it.isJsonArray) }.asJsonArray
            require(values.size() <= 32)
            val resources = values.map { require(it.isJsonPrimitive && it.asJsonPrimitive.isString); it.asString }
            require(resources.distinct().size == resources.size)
            if ("temporary_maps_mutation" !in resources) supported = false
        }
        if (supported) SUMHTemporaryMapsSupport.Supported else SUMHTemporaryMapsSupport.Unsupported
    } catch (_: Exception) { SUMHTemporaryMapsSupport.Unknown } }

    /** Android errno values are part of this wire contract, independent of the host running tests. */
    fun receipt(raw: String, entry: SUMHTemporaryMapsTrackedOperation, exitCode: Int, errno: Int): SUMHTemporaryMapsRemoveResult {
        require(validHideDefinitionId(entry.id) && entry.requestRevision == sumhOperationRevision(temporaryMapsOperationArguments(entry.key)))
        val value = document(raw, SUMH_TEMPORARY_MAPS_PAYLOAD_BYTES)
        require(value.keySet() == setOf("schema_version", "resource", "action", "status", "error_code", "errno", "saved",
            "ioctl_attempted", "acknowledged", "operation_id", "history_supported", "observer_verified", "request"))
        require(value.integer("schema_version", 1, Int.MAX_VALUE) == 1 && value.text("resource") == "temporary_maps_mutation" &&
            value.text("action") == "remove" && value.text("operation_id", 32) == entry.id)
        require(!value.bool("saved") && value.bool("history_supported") && !value.bool("observer_verified"))
        val echoed = value.required("request").objectValue()
        require(echoed.keySet() == setOf("target_ino", "target_dev", "expected_instance_id", "expected_generation"))
        val key = SUMHTemporaryMapsOperationKey(echoed.text("target_ino", 20), echoed.text("target_dev", 20),
            echoed.text("expected_instance_id", 20), echoed.text("expected_generation", 20))
        require(key == entry.key) { "temporary_maps_operation_selector_mismatch" }
        val number = value.integer("errno", 0, 4095); require(number == errno)
        val attempted = value.bool("ioctl_attempted"); val acknowledged = value.bool("acknowledged")
        val code = value.text("error_code", 80)
        require(code == when (number) {
            0 -> "deleted"; 116 -> "snapshot_changed"; 2 -> if (attempted) "temporary_key_not_found" else "capability_query_failed"
            95, 25 -> "unsupported"; 22 -> "invalid_request"; 75 -> "key_not_representable"
            12 -> "insufficient_memory"; 13, 1 -> "permission_denied"; 108 -> "runtime_stopped"; 14 -> "invalid_user_memory"
            else -> if (attempted) "outcome_unknown" else "capability_query_failed"
        })
        val prepublication = setOf(116, 2, 22, 75, 12, 108, 14, 95, 25, 13, 1)
        val status = when (value.text("status")) {
            "deleted" -> SUMHTemporaryMapsRemoveStatus.Deleted.also { require(number == 0 && attempted && acknowledged && exitCode == 0) }
            "not_applied" -> SUMHTemporaryMapsRemoveStatus.NotApplied.also {
                require(number > 0 && !acknowledged && exitCode == 1 && (!attempted || number in prepublication))
            }
            "outcome_unknown" -> SUMHTemporaryMapsRemoveStatus.Unknown.also {
                require(number > 0 && attempted && !acknowledged && exitCode == 1 && number !in prepublication)
            }
            else -> error("temporary_maps_operation_status_unsupported")
        }
        return SUMHTemporaryMapsRemoveResult(status, code, number, attempted, acknowledged)
    }

    /** A retained resource is historical evidence. It never refreshes inventory or authorizes replay. */
    fun resourceResult(reply: SUMHQueryReply, entry: SUMHTemporaryMapsTrackedOperation): SUMHTemporaryMapsOperationResourceResult {
        val root = document(reply.stdout, 64 * 1024)
        require(root.keySet() == setOf("schema_version", "resource", "read_only", "runtime_verified", "durability_confirmation",
            "status", "error", "ledger_status", "receipt_status", "requested_id", "boot_id", "origin_boot_id", "same_boot",
            "request_revision", "record", "result", "retention_limit"))
        require(root.integer("schema_version", 1, Int.MAX_VALUE) == 1 && root.text("resource") == "operation_resource_result")
        require(root.bool("read_only") && !root.bool("runtime_verified") && root.text("durability_confirmation") == "not_observable_from_read")
        require(root.text("requested_id", 32) == entry.id && root.integer("retention_limit", 1, 16) == 16)
        require(validHideDefinitionId(entry.id) && entry.requestRevision == sumhOperationRevision(temporaryMapsOperationArguments(entry.key)))
        val currentBoot = root.nullableText("boot_id", 80)?.let(::boot)
        val originBoot = root.nullableText("origin_boot_id", 80)?.let(::boot)
        val sameBoot = root.nullableBool("same_boot"); val revision = root.revision("request_revision")
        val ledger = root.text("ledger_status").also { require(it in setOf("valid", "missing", "unreadable", "corrupt_or_unsupported")) }
        val receiptStatus = root.text("receipt_status").also { require(it in setOf("not_read", "missing", "valid", "unreadable",
            "corrupt_or_unsupported", "not_retained", "mismatch")) }
        val record = root.required("record").takeUnless { it.isJsonNull }?.let { SUMHOperationsParser.record(it.objectValue(), currentBoot) }
        if (record == null) require(originBoot == null && sameBoot == null && revision == null)
        else {
            require(ledger == "valid" && record.id == entry.id && record.name == entry.name && record.bootId == entry.bootId &&
                record.requestRevision == entry.requestRevision) { "temporary_maps_operation_identity_mismatch" }
            require(originBoot == record.bootId && revision == record.requestRevision && sameBoot == record.sameBoot)
        }
        val status = root.text("status")
        when (status) {
            "observed" -> require(reply.error == null && currentBoot != null && record?.phase == "finished" && receiptStatus == "valid")
            "not_retained" -> require(currentBoot != null && ledger in setOf("valid", "missing") && record == null && receiptStatus == "not_read")
            "not_finished" -> require(currentBoot != null && record != null && record.phase != "finished" && receiptStatus == "not_read")
            "resource_unavailable" -> require(currentBoot != null && record?.phase == "finished" && receiptStatus in setOf("not_read", "missing", "not_retained"))
            "unavailable" -> require(ledger !in setOf("valid", "missing") || record?.phase == "finished" &&
                receiptStatus in setOf("unreadable", "corrupt_or_unsupported", "mismatch"))
            "context_changed_or_unknown" -> Unit
            else -> error("temporary_maps_operation_resource_status_unsupported")
        }
        val result = root.required("result")
        val resource = if (status == "observed") {
            require(root.text("error").isEmpty())
            receipt(result.objectValue().toString(), entry, requireNotNull(record?.exitCode), requireNotNull(record?.errno))
        } else { require(result.isJsonNull); null }
        return SUMHTemporaryMapsOperationResourceResult(status, record, resource, reply.stdout, root.text("error"),
            currentBoot, ledger, receiptStatus)
    }
}
