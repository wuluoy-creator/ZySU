package com.zying.zysu.ui.sumh.util

import android.app.Application
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.*

/** Fixtures follow operation_submit.cpp, operation_query.cpp and the native schema-1 result contract. */
@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHOperationsTest {
    private val f = SUMHHideDefinitionsFixtures
    private val operationId = "c".repeat(32)
    private val requestRevision = "sha256-v1:" + "d".repeat(64)
    private val otherRequest = "sha256-v1:" + "e".repeat(64)
    private val boot = f.boot
    private fun reply(value: JSONObject, error: String? = null) = SUMHQueryReply(value.toString(), error)
    private fun clone(value: JSONObject) = JSONObject(value.toString())
    private fun output(value: String = "", complete: Boolean = true, encoding: String = "utf-8", bytes: Int = value.toByteArray(Charsets.UTF_8).size) =
        JSONObject().put("encoding", encoding).put("value", value).put("byte_length", bytes).put("complete", complete)
    private fun record(phase: String = "finished", id: String = operationId, originBoot: String = boot,
        currentBoot: String? = boot, revision: String? = requestRevision, code: Int = 0) = JSONObject()
        .put("id", id).put("name", "hide disable").put("phase", phase).put("boot_id", originBoot)
        .put("request_revision", revision ?: JSONObject.NULL).put("exit_code", if (phase == "finished") code else JSONObject.NULL)
        .put("errno", if (phase == "finished") 0 else JSONObject.NULL).put("detail", "command status only")
        .put("result", when (phase) { "prepared" -> "not_started"; "started" -> "outcome_unknown"; else -> if (code == 0) "command_completed" else "failed_or_partial" })
        .put("same_boot", currentBoot?.let { it == originBoot } ?: JSONObject.NULL)
    private fun submission(status: String = "command_completed", stdout: JSONObject = output(f.mutation().toString())): JSONObject {
        val ran: Any = when (status) { "command_completed", "failed_or_partial", "outcome_unknown" -> true; "transport_unknown" -> JSONObject.NULL; else -> false }
        val finished = status == "command_completed" || status == "failed_or_partial"
        return JSONObject().put("schema_version", 1).put("kind", "sumh.operation.submission")
            .put("operation_id", if (status in setOf("rejected", "reservation_unconfirmed")) JSONObject.NULL else operationId)
            .put("request_revision", requestRevision).put("boot_id", boot).put("status", status)
            .put("command_executed_this_attempt", ran).put("final_record_confirmed", finished)
            .put("runtime_verified", false).put("command_exit_code", if (ran == true) if (status == "failed_or_partial") 1 else 0 else JSONObject.NULL)
            .put("command_errno", if (ran == true) 0 else JSONObject.NULL).put("observed_record", JSONObject.NULL)
            .put("stdout", if (ran == true) stdout else output()).put("stderr", output()).put("error", "")
            .apply { if (status == "prepared") put("reservation_confirmed", true) }
    }
    private fun parseSubmission(value: JSONObject, error: String? = null, expectedId: String? = operationId,
        expectedBoot: String? = boot) = SUMHOperationsParser.submission(reply(value, error), requestRevision, expectedId, expectedBoot)
    private fun history(status: String = "observed", records: List<JSONObject> = listOf(record()),
        ledger: String = "valid", currentBoot: String? = boot, requested: String? = operationId) = JSONObject()
        .put("schema_version", 2).put("resource", "operation_history").put("read_only", true)
        .put("runtime_verified", false).put("scope", "recorded_commands_in_caller_filesystem_namespace")
        .put("durability_confirmation", "not_observable_from_read").put("ledger_status", ledger).put("error", "")
        .put("boot_id", currentBoot ?: JSONObject.NULL).put("requested_id", requested ?: JSONObject.NULL)
        .put("history_complete", false).put("history_pruned", if (ledger == "valid") false else JSONObject.NULL)
        .put("retention_limit", 16).put("records", JSONArray(records)).put("status", status)
        .put("wait_budget_ms", 0).put("elapsed_ms", 0)
    private fun parseHistory(value: JSONObject, error: String? = null, expectedId: String? = operationId,
        expectedBoot: String? = boot) = SUMHOperationsParser.observation(reply(value, error), expectedId,
        requestRevision.takeIf { expectedId != null }, "hide disable".takeIf { expectedId != null }, expectedBoot)
    private fun dispatch(value: JSONObject = submission(), code: Int = 0) = JSONObject()
        .put("schema_version", 1).put("resource", "operation_dispatch").put("exit_code", code).put("result", value)
    private fun envelope(value: JSONObject = dispatch(), ok: Boolean = true, code: Int = if (ok) 0 else 1) = JSONObject()
        .put("ok", ok).put("exit_code", code).put("errno", if (ok) 0 else 110)
        .put("stdout", value.toString()).put("stderr", if (ok) "" else "transport timeout").toString()

    @Test fun canonicalArgumentsMatchControllerSpacingEscapingAndUtf8() {
        // common/json.hpp emits comma-space between args; operation_submit_input.cpp hashes this exact array.
        val args = listOf("hide", "disable", f.id(1), f.revision)
        val canonical = "[\"hide\", \"disable\", \"${f.id(1)}\", \"${f.revision}\"]"
        assertEquals(canonical, sumhOperationArguments(args))
        assertEquals("sha256-v1:309912edea013cab331ec477295a9e4d20e684b17ba260c2d05c8aaa4057725a", sumhOperationRevision(args))
        val document = "{\"name\":\"名称 😀\",\"note\":\"a\\nb \"}"
        assertEquals("sha256-v1:f55080bdf0a655e0f19cd34861322ed5b77aca19aa2739a282c8f547bc2eaad8",
            sumhOperationRevision(listOf("hide", "annotate", f.id(1), f.revision, document)))
        assertEquals("[\"quote\\\" slash/ back\\\\ \\b\\f\\n\\r\\t\\u0001\"]",
            sumhOperationArguments(listOf("quote\" slash/ back\\ \b\u000C\n\r\t\u0001")))
        assertNotEquals(sumhOperationRevision(args), sumhOperationRevision(args.dropLast(1) + (args.last() + " ")))
    }

    @Test fun inputKeepsArgumentsAsDataAndRejectsInvalidTextOrArity() {
        val args = listOf("hide", "annotate", f.id(1), f.revision, "{\"note\":\"$(id); `whoami`\"}")
        val input = JSONObject(sumhOperationInput(args))
        assertEquals(1, input.getInt("schema_version"))
        assertEquals(args, (0 until input.getJSONArray("args").length()).map { input.getJSONArray("args").getString(it) })
        for (invalid in listOf(emptyList(), listOf("hide"), List(33) { "a" }, listOf("hide", "\u0000"), listOf("hide", "\uD800"))) {
            assertFails { sumhOperationInput(invalid) }
        }
    }

    @Test fun structuredDispatchSeparatesTransportFromCommandResult() {
        val accepted = SUMHOperationsParser.dispatch(envelope())
        assertNull(accepted.error); assertEquals(operationId, parseSubmission(JSONObject(accepted.stdout)).id)
        val failed = SUMHOperationsParser.dispatch(envelope(dispatch(submission("failed_or_partial"), 1)))
        assertEquals("operation_command_exit_1", failed.error)
        assertEquals("failed_or_partial", SUMHOperationsParser.submission(failed, requestRevision, operationId, boot).status)
        assertFails { SUMHOperationsParser.dispatch(envelope(ok = false)) }
        assertFails { SUMHOperationsParser.dispatch(envelope(code = 1)) }
    }

    @Test fun dispatchRejectsForeignFutureIncompleteAndUnstructuredPayloads() {
        for (bad in listOf(dispatch().put("schema_version", 2), dispatch().put("resource", "user_hide_dispatch"),
            dispatch().put("result", "{}"), dispatch().put("result", JSONObject.NULL), dispatch().put("exit_code", "0"),
            dispatch().put("stdout", "not the structured contract"), dispatch(JSONObject().put("oversize", "x".repeat(512 * 1024))))) {
            assertFails { SUMHOperationsParser.dispatch(envelope(bad)) }
        }
        for (bad in listOf("{}", envelope() + " {}", "{\"ok\":true,\"ok\":true}")) assertFails { SUMHOperationsParser.dispatch(bad) }
    }

    @Test fun preparedIsConfirmedReservationOnlyAndExecutionCarriesTypedOutput() {
        val prepared = parseSubmission(submission("prepared"), expectedId = null)
        assertEquals(operationId, prepared.id); assertEquals(false, prepared.ran); assertFalse(prepared.recorded)
        assertNull(prepared.exitCode); assertEquals(0, prepared.stdout.bytes)
        val executed = parseSubmission(submission())
        assertEquals(true, executed.ran); assertTrue(executed.recorded); assertEquals(0, executed.exitCode)
        assertEquals("user_hide_mutation", JSONObject(executed.stdout.value).getString("resource"))
    }

    @Test fun submissionRejectsIdentitySchemaAndRequiredFieldErrors() {
        for (bad in listOf(submission().put("operation_id", f.id(9)), submission().put("request_revision", otherRequest),
            submission().put("request_revision", JSONObject.NULL), submission().put("boot_id", "other-boot"),
            submission().put("schema_version", 2), submission().put("kind", "other"), submission().put("runtime_verified", true),
            submission().put("operation_id", "C".repeat(32)), submission().put("status", "future"))) assertFails { parseSubmission(bad) }
        for (key in submission().keys().asSequence().toList()) assertFails("missing $key") { parseSubmission(submission().apply { remove(key) }) }
    }

    @Test fun submissionRejectsUnknownFieldsAndReservationFlagOutsidePrepare() {
        assertFails { parseSubmission(submission().put("future_field", true)) }
        assertFails { parseSubmission(submission().put("reservation_confirmed", true)) }
    }

    @Test fun preparedCannotMasqueradeAsExecutedOrUnconfirmed() {
        val changes: List<(JSONObject) -> Unit> = listOf(
            { it.put("reservation_confirmed", false) }, { it.remove("reservation_confirmed") }, { it.put("command_executed_this_attempt", true) },
            { it.put("final_record_confirmed", true) }, { it.put("command_exit_code", 0) }, { it.put("command_errno", 0) },
            { it.put("observed_record", record("prepared")) }, { it.put("stdout", output("unexpected")) }, { it.put("boot_id", "") })
        for (change in changes) assertFails { parseSubmission(submission("prepared").also(change), expectedId = null) }
        assertFails { parseSubmission(submission("prepared")) }
        assertFails { parseSubmission(submission("prepared"), error = "transport error", expectedId = null) }
    }

    @Test fun commandCompletionRequiresExecutedConfirmedMatchingZeroCodes() {
        for ((key, value) in listOf("command_executed_this_attempt" to false, "final_record_confirmed" to false,
            "command_exit_code" to 1, "command_errno" to 5, "observed_record" to record())) {
            assertFails { parseSubmission(submission().put(key, value)) }
        }
        assertFails { parseSubmission(submission(), error = "transport failed") }
        assertFails { parseSubmission(submission("failed_or_partial").put("command_exit_code", 0)) }
        assertEquals("failed_or_partial", parseSubmission(submission("failed_or_partial"), error = "exit 1").status)
    }

    @Test fun unknownOutcomesKeepExecutionUncertaintyWithoutInventingCompletion() {
        val afterExecution = parseSubmission(submission("outcome_unknown"))
        assertEquals(true, afterExecution.ran); assertFalse(afterExecution.recorded); assertEquals(0, afterExecution.exitCode)
        val unknown = submission("outcome_unknown").put("command_executed_this_attempt", JSONObject.NULL)
            .put("command_exit_code", JSONObject.NULL).put("command_errno", JSONObject.NULL)
        assertNull(parseSubmission(unknown).ran)
        val transport = parseSubmission(submission("transport_unknown"))
        assertNull(transport.ran); assertNull(transport.exitCode); assertFalse(transport.recorded)
        for (bad in listOf(submission("outcome_unknown").put("command_executed_this_attempt", false),
            submission("outcome_unknown").put("final_record_confirmed", true), clone(unknown).put("command_errno", 0),
            submission("transport_unknown").put("command_executed_this_attempt", true),
            submission("transport_unknown").put("observed_record", record()))) assertFails { parseSubmission(bad) }
    }

    @Test fun nonExecutedRejectionAndPrunedReservationRemainNonExecuted() {
        for (status in listOf("not_retained", "journal_unavailable")) {
            val value = parseSubmission(submission(status)); assertEquals(false, value.ran); assertNull(value.observed)
            assertFails { parseSubmission(submission(status).put("observed_record", record())) }
            assertFails { parseSubmission(submission(status).put("command_exit_code", 0)) }
        }
        for (status in listOf("reservation_unconfirmed", "rejected")) {
            val value = parseSubmission(submission(status).put("request_revision", JSONObject.NULL), expectedId = null)
            assertNull(value.id); assertNull(value.requestRevision); assertEquals(false, value.ran)
        }
    }

    @Test fun repeatedExecutionReturnsObservationInsteadOfCachedResourceOutput() {
        for (phase in listOf("started", "finished")) {
            val raw = submission("already_started_or_finished").put("observed_record", record(phase))
            val parsed = parseSubmission(raw)
            assertEquals(false, parsed.ran); assertFalse(parsed.recorded); assertEquals(phase, parsed.observed?.phase)
            assertEquals(0, parsed.stdout.bytes)
            assertFails { parseSubmission(clone(raw).put("stdout", output(f.mutation().toString()))) }
        }
        assertFails { parseSubmission(submission("already_started_or_finished").put("observed_record", record("prepared"))) }
        assertFails { parseSubmission(submission("already_started_or_finished").put("observed_record", record(revision = otherRequest))) }
        assertFails { parseSubmission(submission("already_started_or_finished").put("observed_record", record(id = f.id(2)))) }
    }

    @Test fun requestConflictMayDescribeAnotherRequestWhileBootMismatchIsHistorical() {
        val conflict = submission("request_conflict").put("observed_record", record("prepared", revision = otherRequest))
        assertEquals(otherRequest, parseSubmission(conflict).observed?.requestRevision)
        val old = submission("boot_mismatch").put("observed_record", record(originBoot = "old-boot"))
        assertEquals(false, parseSubmission(old).observed?.sameBoot)
        assertFails { parseSubmission(submission("boot_mismatch").put("observed_record", record())) }
        assertFails { parseSubmission(submission("boot_mismatch").put("observed_record", record(originBoot = "old-boot", revision = otherRequest))) }
    }

    @Test fun byteStreamsPreserveUtf8HexAndTruncationMarkers() {
        val utf8 = parseSubmission(submission(stdout = output("路径\n尾部 ")))
        assertEquals("路径\n尾部 ", utf8.stdout.value); assertEquals(14, utf8.stdout.bytes)
        val hex = parseSubmission(submission(stdout = output("78c3", encoding = "hex", bytes = 2)))
        assertEquals("hex", hex.stdout.encoding); assertEquals(2, hex.stdout.bytes)
        val truncated = parseSubmission(submission(stdout = output("partial", complete = false)))
        assertFalse(truncated.stdout.complete)
        for (bad in listOf(output("é", bytes = 1), output("AF", encoding = "hex", bytes = 1), output("a", encoding = "hex", bytes = 1),
            output("00", encoding = "base64", bytes = 1), output().put("byte_length", 1.5), output().put("complete", "true"),
            output().put("future", true), output().put("byte_length", 64 * 1024 + 1))) assertFails { parseSubmission(submission(stdout = bad)) }
    }

    @Test fun submissionRejectsDuplicateKeysTrailingDocumentsAndMalformedUtf8() {
        assertFails { SUMHOperationsParser.submission(SUMHQueryReply(submission().toString().replaceFirst("{", "{\"schema_version\":1,")), requestRevision, operationId, boot) }
        assertFails { SUMHOperationsParser.submission(SUMHQueryReply(submission().toString() + " {}"), requestRevision, operationId, boot) }
        assertFails { parseSubmission(submission(stdout = output("\uD800"))) }
    }

    @Test fun boundedStreamsCanExceed64KiBWhenCombinedWithoutIncreasingIndividualLimits() {
        val raw = submission(stdout = output("a".repeat(64 * 1024))).put("stderr", output("b".repeat(64 * 1024)))
        val parsed = SUMHOperationsParser.submission(SUMHOperationsParser.dispatch(envelope(dispatch(raw))), requestRevision, operationId, boot)
        assertEquals(64 * 1024, parsed.stdout.bytes); assertEquals(64 * 1024, parsed.stderr.bytes)
        assertFails { parseSubmission(submission(stdout = output("a".repeat(64 * 1024 + 1)))) }
    }

    @Test fun historyObservesPreparedStartedAndFinishedWithoutRuntimeClaims() {
        for (phase in listOf("prepared", "started", "finished")) {
            val value = parseHistory(history(records = listOf(record(phase))))
            assertEquals("observed", value.status); assertEquals(phase, value.records.single().phase)
            assertEquals(if (phase == "finished") 0 else null, value.records.single().exitCode)
        }
        val failed = parseHistory(history(records = listOf(record(code = 1))))
        assertEquals("failed_or_partial", failed.records.single().result)
    }

    @Test fun historyPinsIdRequestNameAndOriginBoot() {
        for (bad in listOf(history().put("requested_id", f.id(1)), history(records = listOf(record(id = f.id(1)))),
            history(records = listOf(record(revision = otherRequest))), history(records = listOf(record().put("name", "hide enable"))),
            history(records = listOf(record(originBoot = "unexpected-origin"))))) assertFails { parseHistory(bad) }
        val old = history(records = listOf(record(originBoot = "old-boot")))
        assertEquals(false, parseHistory(old, expectedBoot = "old-boot").records.single().sameBoot)
    }

    @Test fun historySupportsUnknownBootAndMissingPrunedUnavailableStates() {
        val unknown = history("context_changed_or_unknown", listOf(record(currentBoot = null)), currentBoot = null)
        assertNull(parseHistory(unknown, "unknown").records.single().sameBoot)
        for (ledger in listOf("valid", "missing")) {
            val missing = parseHistory(history("not_retained", emptyList(), ledger), "exit 3")
            assertTrue(missing.records.isEmpty()); assertEquals("not_retained", missing.status)
        }
        for (ledger in listOf("unreadable", "corrupt_or_unsupported")) {
            assertEquals("unavailable", parseHistory(history("unavailable", emptyList(), ledger), "exit 5").status)
        }
        assertEquals("observed", parseHistory(history(records = emptyList(), ledger = "missing", requested = null), expectedId = null).status)
    }

    @Test fun waitTimeoutRequiresAStillUnfinishedRetainedOperation() {
        for (phase in listOf("prepared", "started")) {
            val value = history("wait_timeout", listOf(record(phase))).put("wait_budget_ms", 5000).put("elapsed_ms", 5001)
            assertEquals(phase, parseHistory(value, "exit 4").records.single().phase)
        }
        assertFails { parseHistory(history("wait_timeout"), "exit 4") }
        assertFails { parseHistory(history("wait_timeout", emptyList()), "exit 4") }
        assertFails { parseHistory(history().put("wait_budget_ms", 10001)) }
        assertFails { parseHistory(history().put("elapsed_ms", -1)) }
    }

    @Test fun historyRejectsContradictoryReadSemanticsAndUnknownFields() {
        for (bad in listOf(history().put("read_only", false), history().put("runtime_verified", true), history().put("history_complete", true),
            history().put("durability_confirmation", "durable"), history().put("scope", "global"), history().put("retention_limit", 17),
            history().put("history_pruned", JSONObject.NULL), history().put("status", "future"), history().put("schema_version", 3),
            history().put("future", true), history("not_retained"), history("unavailable"),
            history().put("ledger_status", "missing"))) assertFails { parseHistory(bad) }
        assertFails { parseHistory(history(), "transport failed") }
    }

    @Test fun historyRejectsRecordContradictionsDuplicateIdsAndCapacityOverflow() {
        for (bad in listOf(record().put("same_boot", false), record().put("phase", "unknown"), record().put("result", "outcome_unknown"),
            record().put("exit_code", JSONObject.NULL), record().put("errno", 4096), record().put("exit_code", -1),
            record("prepared").put("request_revision", JSONObject.NULL), record("started").put("exit_code", 0),
            record().put("future", true), record().put("id", "C".repeat(32)))) assertFails { parseHistory(history(records = listOf(bad))) }
        assertFails { parseHistory(history(records = listOf(record(), record()))) }
        assertFails { parseHistory(history(records = List(17) { record(id = f.id(it + 1)) }, requested = null), expectedId = null) }
        assertFails { parseHistory(history(records = listOf(record(), record(id = f.id(2))), requested = null), expectedId = operationId) }
        assertEquals(16, parseHistory(history(records = List(16) { record(id = f.id(it + 1)) }, requested = null), expectedId = null).records.size)
    }

    @Test fun legacyFinishedHistoryAllowsNullRequestButCannotMatchAReservedRequest() {
        val legacy = history(records = listOf(record(revision = null)), requested = null)
        assertNull(parseHistory(legacy, expectedId = null).records.single().requestRevision)
        assertFails { parseHistory(history(records = listOf(record(revision = null)))) }
    }

    private fun tracked() = SUMHTrackedOperation(operationId, requestRevision, boot, "disable", f.id(1), f.revision, "unknown")
    private fun resourceQuery(status: String = "observed", currentBoot: String? = boot,
        ledger: String = "valid", receipt: String = if (status == "observed") "valid" else "not_read",
        saved: JSONObject? = if (status == "not_retained" || ledger != "valid") null else record(currentBoot = currentBoot),
        payload: JSONObject? = if (status == "observed") f.mutation() else null) = JSONObject()
        .put("schema_version", 1).put("resource", "operation_resource_result").put("read_only", true).put("runtime_verified", false)
        .put("durability_confirmation", "not_observable_from_read").put("status", status).put("error", "")
        .put("ledger_status", ledger).put("receipt_status", receipt).put("requested_id", operationId)
        .put("boot_id", currentBoot ?: JSONObject.NULL).put("origin_boot_id", saved?.get("boot_id") ?: JSONObject.NULL)
        .put("same_boot", saved?.get("same_boot") ?: JSONObject.NULL).put("request_revision", saved?.get("request_revision") ?: JSONObject.NULL)
        .put("record", saved ?: JSONObject.NULL).put("result", payload ?: JSONObject.NULL).put("retention_limit", 16)
    private fun parseResource(value: JSONObject, error: String? = null, entry: SUMHTrackedOperation = tracked()) =
        SUMHOperationsParser.resourceResult(reply(value, error), entry)

    @Test fun retainedResourceIsExactHistoricalEvidenceWithAnOperationAssociation() {
        val parsed = parseResource(resourceQuery())
        assertEquals("observed", parsed.status); assertTrue(parsed.resource!!.succeeded)
        assertEquals(operationId, parsed.resource.operationId); assertEquals(f.nextRevision, parsed.resource.value?.revision)
        assertEquals(requestRevision, parsed.record?.requestRevision)
        val oldBoot = parseResource(resourceQuery(currentBoot = "next-boot"))
        assertEquals(false, oldBoot.record?.sameBoot); assertEquals(boot, oldBoot.record?.bootId)
        assertEquals("next-boot", oldBoot.bootId); assertTrue(oldBoot.resource!!.succeeded)
        // Same resource can be preserved as history; current runtime proof is not part of this type.
        assertEquals(false, JSONObject(oldBoot.raw).getBoolean("runtime_verified"))
    }

    @Test fun retainedPartialResultNeverBecomesSuccessFromFinishedHistory() {
        val partial = resourceQuery(saved = record(code = 1), payload = f.mutation(status = "partial"))
        val parsed = parseResource(partial)
        assertEquals("partial", parsed.resource?.value?.status); assertFalse(parsed.resource!!.succeeded)
        assertEquals(operationId, parsed.resource.operationId)
        assertFails { parseResource(resourceQuery(payload = f.mutation(status = "partial"))) }
        assertFails { parseResource(resourceQuery(saved = record().put("errno", 5))) }
        assertFails { parseResource(resourceQuery(), "dispatch failed") }
    }

    @Test fun resourceQueryRejectsEveryReservationAndPayloadIdentityMismatch() {
        for (bad in listOf(resourceQuery().put("requested_id", f.id(2)), resourceQuery().put("request_revision", otherRequest),
            resourceQuery().put("origin_boot_id", "other-origin"), resourceQuery().put("same_boot", false),
            resourceQuery(saved = record(id = f.id(2))), resourceQuery(saved = record(revision = otherRequest)),
            resourceQuery(saved = record().put("name", "hide enable")), resourceQuery(saved = record(originBoot = "other-origin")),
            resourceQuery(payload = f.mutation(number = 2)), resourceQuery(payload = f.mutation("enable")),
            resourceQuery(payload = f.mutation(previous = f.nextRevision)), resourceQuery(payload = f.mutation().put("boot_id", "other-origin")))) {
            assertFails { parseResource(bad) }
        }
        assertFails { parseResource(resourceQuery(), entry = tracked().copy(configRevision = f.nextRevision)) }
    }

    @Test fun resourceQueryNonResultsNeverContainAnAppliedPayload() {
        for (phase in listOf("prepared", "started")) {
            val value = resourceQuery("not_finished", saved = record(phase))
            assertNull(parseResource(value, "exit 4").resource)
            assertFails { parseResource(clone(value).put("result", f.mutation())) }
        }
        for (ledger in listOf("valid", "missing")) {
            val value = resourceQuery("not_retained", ledger = ledger)
            assertNull(parseResource(value, "exit 3").record)
            assertNull(parseResource(value, "exit 3").resource)
        }
        for (receipt in listOf("not_read", "missing", "not_retained")) {
            assertNull(parseResource(resourceQuery("resource_unavailable", receipt = receipt), "exit 6").resource)
        }
        for (receipt in listOf("unreadable", "corrupt_or_unsupported", "mismatch")) {
            assertNull(parseResource(resourceQuery("unavailable", receipt = receipt), "exit 5").resource)
        }
        for (ledger in listOf("unreadable", "corrupt_or_unsupported")) {
            assertNull(parseResource(resourceQuery("unavailable", ledger = ledger), "exit 5").resource)
        }
        val contextUnknown = resourceQuery("context_changed_or_unknown", currentBoot = null, saved = null)
        assertNull(parseResource(contextUnknown, "exit 5").resource)
        assertFails { parseResource(clone(contextUnknown).put("result", f.mutation())) }
    }

    @Test fun resourceQueryRejectsContradictoryStatusesAndProofClaims() {
        for (bad in listOf(resourceQuery().put("schema_version", 2), resourceQuery().put("read_only", false),
            resourceQuery().put("runtime_verified", true), resourceQuery().put("durability_confirmation", "fsync_confirmed"),
            resourceQuery().put("receipt_status", "missing"), resourceQuery().put("retention_limit", 17),
            resourceQuery().put("ledger_status", "missing"), resourceQuery(saved = record("started")),
            resourceQuery().put("error", "inconsistent success"), resourceQuery().put("result", JSONObject.NULL),
            resourceQuery().put("status", "future"), resourceQuery().put("future", true),
            resourceQuery("not_finished"), resourceQuery("not_retained", saved = record()),
            resourceQuery("resource_unavailable", saved = record("started")), resourceQuery("unavailable"),
            resourceQuery("context_changed_or_unknown", saved = null).put("origin_boot_id", boot))) assertFails { parseResource(bad) }
        for (key in resourceQuery().keys().asSequence().toList()) assertFails("missing $key") { parseResource(resourceQuery().apply { remove(key) }) }
    }

    @Test fun resourceQueryRejectsDuplicateKeysTruncationAndOversize() {
        for (raw in listOf(resourceQuery().toString().replaceFirst("{", "{\"schema_version\":1,"), resourceQuery().toString().dropLast(1),
            resourceQuery().toString() + " {}", resourceQuery().put("error", "x".repeat(64 * 1024)).toString())) {
            assertFails { SUMHOperationsParser.resourceResult(SUMHQueryReply(raw), tracked()) }
        }
    }
}
