package com.zying.zysu.ui.sumh.util

import com.google.gson.JsonObject
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.required
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.booleanValue
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.integer
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.text
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.nullableText
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.revision
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.objectValue
import com.zying.zysu.ui.sumh.util.SUMHMapsPreviewParser.bool
import java.security.MessageDigest

internal data class SUMHRecordedOperation(val id: String, val name: String, val phase: String, val bootId: String,
    val requestRevision: String?, val result: String, val sameBoot: Boolean?, val exitCode: Int?, val errno: Int?, val detail: String)
internal data class SUMHOperationObservation(val status: String, val ledger: String, val bootId: String?,
    val requestedId: String?, val records: List<SUMHRecordedOperation>, val error: String, val raw: String)
internal data class SUMHOperationOutput(val encoding: String, val value: String, val bytes: Int, val complete: Boolean)
internal data class SUMHOperationSubmission(val id: String?, val requestRevision: String?, val bootId: String,
    val status: String, val ran: Boolean?, val recorded: Boolean, val exitCode: Int?, val errno: Int?,
    val stdout: SUMHOperationOutput, val stderr: SUMHOperationOutput, val observed: SUMHRecordedOperation?,
    val error: String, val raw: String)
internal data class SUMHOperationResourceResult(val status: String, val ledger: String, val receiptStatus: String,
    val bootId: String?, val record: SUMHRecordedOperation?, val resource: HideMutationResult?, val error: String, val raw: String)
internal const val SUMH_OPERATION_SUBMISSION_BYTES = 512 * 1024

/** Match the controller's JSON string-array encoding exactly, including comma spaces. */
internal fun sumhOperationArguments(args: List<String>): String = args.joinToString(", ", "[", "]") { value ->
    require(validHideDefinitionText(value, 48 * 1024))
    buildString {
        append('"')
        value.forEach { c -> when (c) {
            '"' -> append("\\\""); '\\' -> append("\\\\"); '\b' -> append("\\b"); '\u000C' -> append("\\f")
            '\n' -> append("\\n"); '\r' -> append("\\r"); '\t' -> append("\\t")
            else -> if (c.code < 32) append("\\u" + c.code.toString(16).padStart(4, '0')) else append(c)
        } }
        append('"')
    }
}
internal fun sumhOperationInput(args: List<String>): String {
    require(args.size in 2..32)
    return "{\"schema_version\":1,\"args\":" + sumhOperationArguments(args) + "}"
}
internal fun sumhOperationRevision(args: List<String>): String = "sha256-v1:" + MessageDigest.getInstance("SHA-256")
    .digest(("sumhp-operation-v1\n" + sumhOperationArguments(args)).toByteArray(Charsets.UTF_8)).joinToString("") { "%02x".format(it) }

internal object SUMHOperationsParser {
    private fun JsonObject.nullableBool(key: String) = required(key).let { if (it.isJsonNull) null else it.booleanValue() }
    private fun JsonObject.nullableInt(key: String, max: Int) = required(key).let { if (it.isJsonNull) null else integer(key, 0, max) }
    private fun id(value: String) = value.also { require(validHideDefinitionId(it)) }
    private fun boot(value: String) = value.also { require(it.isNotEmpty() && validHideDefinitionText(it, 80)) }
    fun dispatch(raw: String): SUMHQueryReply {
        val outer = SUMHControlStateParser.reply(raw, 512 * 1024, 4 * 1024 * 1024)
        require(outer.error == null) { outer.error.orEmpty() }
        val root = SUMHControlStateParser.document(outer.stdout, 512 * 1024)
        require(root.integer("schema_version", 1, Int.MAX_VALUE) == 1 && root.text("resource") == "operation_dispatch")
        val code = root.integer("exit_code", Int.MIN_VALUE, Int.MAX_VALUE)
        require(root.keySet() == setOf("schema_version", "resource", "exit_code", "result"))
        val result = root.required("result").objectValue().toString()
        require(result.toByteArray(Charsets.UTF_8).size <= SUMH_OPERATION_SUBMISSION_BYTES)
        return SUMHQueryReply(result, if (code == 0) null else "operation_command_exit_$code")
    }
    internal fun record(root: JsonObject, currentBoot: String?): SUMHRecordedOperation {
        require(root.keySet() == setOf("id", "name", "phase", "boot_id", "request_revision", "result", "same_boot", "exit_code", "errno", "detail"))
        val phase = root.text("phase", 8).also { require(it in setOf("prepared", "started", "finished")) }
        val savedBoot = boot(root.text("boot_id", 80)); val sameBoot = root.nullableBool("same_boot")
        require(sameBoot == currentBoot?.let { savedBoot == it })
        val code = root.nullableInt("exit_code", 255); val errno = root.nullableInt("errno", 4095)
        require(if (phase == "finished") code != null && errno != null else code == null && errno == null)
        val expected = when (phase) { "prepared" -> "not_started"; "started" -> "outcome_unknown"; else -> if (code == 0) "command_completed" else "failed_or_partial" }
        require(root.text("result") == expected)
        val request = root.revision("request_revision"); if (phase == "prepared") require(request != null)
        val name = root.text("name", 80).also { require(it.isNotEmpty() && validHideDefinitionText(it, 80)) }
        val detail = root.text("detail", 1024).also { require(validHideDefinitionText(it, 1024)) }
        return SUMHRecordedOperation(id(root.text("id", 32)), name, phase, savedBoot, request, expected, sameBoot, code, errno, detail)
    }
    fun observation(reply: SUMHQueryReply, expectedId: String? = null, expectedRequest: String? = null,
        expectedName: String? = null, expectedBoot: String? = null): SUMHOperationObservation {
        val root = SUMHControlStateParser.document(reply.stdout, 64 * 1024)
        require(root.integer("schema_version", 1, Int.MAX_VALUE) == 2 && root.text("resource") == "operation_history")
        require(root.keySet() == setOf("schema_version", "resource", "read_only", "scope", "runtime_verified", "durability_confirmation",
            "ledger_status", "error", "boot_id", "requested_id", "history_complete", "history_pruned", "retention_limit", "records",
            "status", "wait_budget_ms", "elapsed_ms"))
        require(root.bool("read_only") && !root.bool("runtime_verified") && !root.bool("history_complete"))
        require(root.text("scope") == "recorded_commands_in_caller_filesystem_namespace" && root.text("durability_confirmation") == "not_observable_from_read")
        val currentBoot = root.nullableText("boot_id", 80)?.let(::boot)
        val requested = root.nullableText("requested_id", 32)?.let(::id); require(requested == expectedId)
        val ledger = root.text("ledger_status").also { require(it in setOf("valid", "missing", "unreadable", "corrupt_or_unsupported")) }
        val pruned = root.nullableBool("history_pruned"); require((pruned != null) == (ledger == "valid"))
        require(root.integer("retention_limit", 1, 16) == 16)
        root.integer("wait_budget_ms", 0, 10000); root.integer("elapsed_ms", 0, Int.MAX_VALUE)
        val array = root.required("records"); require(array.isJsonArray && array.asJsonArray.size() <= if (expectedId == null) 16 else 1)
        val records = array.asJsonArray.map { record(it.objectValue(), currentBoot) }
        require(records.map { it.id }.distinct().size == records.size)
        require(ledger == "valid" || records.isEmpty())
        records.forEach { item ->
            require(expectedId == null || item.id == expectedId)
            require(expectedRequest == null || item.requestRevision == expectedRequest) { "operation_request_mismatch" }
            require(expectedName == null || item.name == expectedName) { "operation_name_mismatch" }
            require(expectedBoot == null || item.bootId == expectedBoot) { "operation_boot_mismatch" }
        }
        val status = root.text("status").also { require(it in setOf("observed", "not_retained", "wait_timeout", "unavailable", "context_changed_or_unknown")) }
        when (status) {
            "observed" -> require(currentBoot != null && ledger in setOf("valid", "missing") && (expectedId == null || records.size == 1) && reply.error == null)
            "not_retained" -> require(currentBoot != null && ledger in setOf("valid", "missing") && expectedId != null && records.isEmpty())
            "wait_timeout" -> require(currentBoot != null && records.size == 1 && records.single().phase != "finished")
            "unavailable" -> require(ledger !in setOf("valid", "missing"))
        }
        return SUMHOperationObservation(status, ledger, currentBoot, requested, records, root.text("error"), reply.stdout)
    }
    private fun output(root: JsonObject): SUMHOperationOutput {
        require(root.keySet() == setOf("encoding", "value", "byte_length", "complete"))
        val encoding = root.text("encoding", 8); val value = root.text("value", 128 * 1024)
        val bytes = root.integer("byte_length", 0, 64 * 1024)
        require(if (encoding == "utf-8") value.toByteArray(Charsets.UTF_8).size == bytes
            else encoding == "hex" && value.length == bytes * 2 && value.all { it in "0123456789abcdef" })
        return SUMHOperationOutput(encoding, value, bytes, root.bool("complete"))
    }
    fun submission(reply: SUMHQueryReply, expectedRequest: String, expectedId: String? = null,
        expectedBoot: String? = null): SUMHOperationSubmission {
        val root = SUMHControlStateParser.document(reply.stdout, SUMH_OPERATION_SUBMISSION_BYTES)
        require(root.integer("schema_version", 1, Int.MAX_VALUE) == 1 && root.text("kind") == "sumh.operation.submission")
        require(!root.bool("runtime_verified"))
        val status = root.text("status").also { require(it in setOf("prepared", "reservation_unconfirmed", "rejected", "command_completed", "failed_or_partial", "outcome_unknown", "transport_unknown", "already_started_or_finished", "not_retained", "request_conflict", "boot_mismatch", "journal_unavailable")) }
        val keys = setOf("schema_version", "kind", "operation_id", "request_revision", "boot_id", "status",
            "command_executed_this_attempt", "final_record_confirmed", "runtime_verified", "command_exit_code",
            "command_errno", "observed_record", "stdout", "stderr", "error")
        require(root.keySet() == if (status == "prepared") keys + "reservation_confirmed" else keys)
        val id = root.nullableText("operation_id", 32)?.let(::id)
        val request = root.revision("request_revision")
        require(expectedId == null || id == expectedId)
        require(request == expectedRequest || request == null && status in setOf("rejected", "reservation_unconfirmed"))
        val currentBoot = root.text("boot_id", 80).also { require(validHideDefinitionText(it, 80)) }
        require(expectedBoot == null || currentBoot == expectedBoot) { "operation_current_boot_changed" }
        val ran = root.nullableBool("command_executed_this_attempt"); val recorded = root.bool("final_record_confirmed")
        val code = root.nullableInt("command_exit_code", 255); val errno = root.nullableInt("command_errno", 4095)
        val out = output(root.required("stdout").objectValue()); val err = output(root.required("stderr").objectValue())
        val observed = root.required("observed_record").takeUnless { it.isJsonNull }?.let { record(it.objectValue(), currentBoot.ifEmpty { null }) }
        if (observed != null) require(observed.id == id && ran == false)
        when (status) {
            "prepared" -> require(expectedId == null && id != null && request == expectedRequest && currentBoot.isNotEmpty() &&
                root.bool("reservation_confirmed") && ran == false && !recorded && code == null && errno == null && observed == null && reply.error == null)
            "command_completed", "failed_or_partial" -> require(expectedId != null && ran == true && recorded && code != null && errno != null &&
                (if (status == "command_completed") code == 0 && errno == 0 && reply.error == null else code != 0) && observed == null)
            "outcome_unknown" -> require(expectedId != null && !recorded && ran != false && if (ran == null) code == null && errno == null else code != null && errno != null)
            "transport_unknown" -> require(expectedId != null && !recorded && ran == null && code == null && errno == null && observed == null)
            else -> require(ran == false && !recorded && code == null && errno == null)
        }
        if (status in setOf("already_started_or_finished", "request_conflict", "boot_mismatch")) {
            require(observed != null)
            if (status != "request_conflict") require(observed.requestRevision == expectedRequest)
            if (status == "already_started_or_finished") require(observed.phase != "prepared" && observed.sameBoot == true)
            if (status == "boot_mismatch") require(observed.sameBoot != true)
        } else require(observed == null)
        if (ran == false) require(out.bytes == 0 && out.complete && err.complete)
        return SUMHOperationSubmission(id, request, currentBoot, status, ran, recorded, code, errno, out, err, observed,
            root.text("error"), reply.stdout)
    }

    /** A retained receipt is historical evidence bound to this exact reservation, never a live rule query. */
    fun resourceResult(reply: SUMHQueryReply, entry: SUMHTrackedOperation): SUMHOperationResourceResult {
        val root = SUMHControlStateParser.document(reply.stdout, 64 * 1024)
        require(root.keySet() == setOf("schema_version", "resource", "read_only", "runtime_verified", "durability_confirmation",
            "status", "error", "ledger_status", "receipt_status", "requested_id", "boot_id", "origin_boot_id", "same_boot",
            "request_revision", "record", "result", "retention_limit"))
        require(root.integer("schema_version", 1, Int.MAX_VALUE) == 1 && root.text("resource") == "operation_resource_result")
        require(root.bool("read_only") && !root.bool("runtime_verified") && root.text("durability_confirmation") == "not_observable_from_read")
        require(root.text("requested_id", 32) == entry.id && root.integer("retention_limit", 1, 16) == 16)
        val currentBoot = root.nullableText("boot_id", 80)?.let(::boot)
        val originBoot = root.nullableText("origin_boot_id", 80)?.let(::boot)
        val sameBoot = root.nullableBool("same_boot")
        val revision = root.revision("request_revision")
        val ledger = root.text("ledger_status").also { require(it in setOf("valid", "missing", "unreadable", "corrupt_or_unsupported")) }
        val receipt = root.text("receipt_status").also { require(it in setOf("not_read", "missing", "valid", "unreadable", "corrupt_or_unsupported", "not_retained", "mismatch")) }
        val observed = root.required("record").takeUnless { it.isJsonNull }?.let { record(it.objectValue(), currentBoot) }
        if (observed == null) require(originBoot == null && sameBoot == null && revision == null)
        else {
            require(ledger == "valid" && observed.id == entry.id && observed.name == entry.name && observed.bootId == entry.bootId &&
                observed.requestRevision == entry.requestRevision) { "operation_resource_identity_mismatch" }
            require(originBoot == observed.bootId && revision == observed.requestRevision && sameBoot == observed.sameBoot)
        }
        val status = root.text("status")
        when (status) {
            "observed" -> require(reply.error == null && currentBoot != null && observed?.phase == "finished" && receipt == "valid")
            "not_retained" -> require(currentBoot != null && ledger in setOf("valid", "missing") && observed == null && receipt == "not_read")
            "not_finished" -> require(currentBoot != null && observed != null && observed.phase != "finished" && receipt == "not_read")
            "resource_unavailable" -> require(currentBoot != null && observed?.phase == "finished" && receipt in setOf("not_read", "missing", "not_retained"))
            "unavailable" -> require(ledger !in setOf("valid", "missing") || observed?.phase == "finished" && receipt in setOf("unreadable", "corrupt_or_unsupported", "mismatch"))
            "context_changed_or_unknown" -> Unit
            else -> error("operation_resource_status_unsupported")
        }
        val value = root.required("result")
        val resource = if (status == "observed") {
            require(root.text("error").isEmpty())
            val result = HideDefinitionsParser.mutation(SUMHQueryReply(value.objectValue().toString(),
                if (observed?.exitCode == 0) null else "operation_failed_or_partial"), entry.action, entry.configRevision, entry.bootId, entry.expectedDefinitionId)
            require(result.value != null) { "operation_resource_payload_invalid" }
            // Command completion alone cannot promote a partial or malformed resource receipt to success.
            if (observed?.exitCode == 0) require(observed.errno == 0 && result.succeeded)
            result.copy(operationId = entry.id)
        } else { require(value.isJsonNull); null }
        return SUMHOperationResourceResult(status, ledger, receipt, currentBoot, observed, resource, root.text("error"), reply.stdout)
    }
}
