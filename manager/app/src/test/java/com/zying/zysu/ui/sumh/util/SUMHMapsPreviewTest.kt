package com.zying.zysu.ui.sumh.util

import android.app.Application
import org.json.JSONObject
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHMapsPreviewTest {
    private val f = SUMHMapsPreviewFixtures
    private fun parse(value: JSONObject) = SUMHMapsPreviewParser.parse(value.toString())
    @Test fun pathsAndUnsignedIdentitiesArePreservedExactly() {
        val parsed = parse(f.preview())
        assertEquals("/目标/0 ", parsed.rows.single().targetPath)
        assertEquals("18446744073709551615", parsed.rows.single().target.ino)
        assertEquals("missing", parsed.submission.status); assertNull(parsed.submission.confirmed)
    }
    @Test fun unknownVersionWrongTypesDuplicateKeysAndTrailingDocumentsFail() {
        for (raw in listOf(f.preview().put("schema_version", 2), f.preview().put("read_only", false), f.preview().put("schema_version", "1"))) assertFails { parse(raw) }
        assertFails { SUMHMapsPreviewParser.parse(f.preview().toString() + "{}") }
        assertFails { SUMHMapsPreviewParser.parse("{\"schema_version\":1,\"schema_version\":1}") }
    }
    @Test fun malformedUnsignedMissingRegularIdentityAndMismatchedRowsFail() {
        for (value in listOf<Any>(1, "-1", "01", "18446744073709551616", JSONObject.NULL)) {
            val raw = f.preview(); raw.getJSONArray("rows").getJSONObject(0).getJSONObject("target").put("ino", value)
            assertFails { parse(raw) }
        }
        val count = f.preview(); count.getJSONObject("saved").put("count", 0); assertFails { parse(count) }
        val index = f.preview(); index.getJSONArray("rows").getJSONObject(0).put("index", 1); assertFails { parse(index) }
    }
    @Test fun partialMissingFilesTypeErrorsAndCollisionsRemainFacts() {
        val raw = f.preview().put("status", "partial")
        val row = raw.getJSONArray("rows").getJSONObject(0)
        row.put("same_target_collision", true).put("registered_match", "unknown")
        row.getJSONObject("target").put("status", "missing").put("ino", JSONObject.NULL).put("dev", JSONObject.NULL).put("errno", 2)
        row.getJSONObject("replacement").put("status", "not_regular")
        val parsed = parse(raw)
        assertEquals(SUMHMapsPreviewStatus.Partial, parsed.status)
        assertEquals(SUMHMapsIdentityStatus.Missing, parsed.rows[0].target.status)
        assertTrue(parsed.rows[0].collision)
        assertEquals(SUMHMapsRegistration.Unknown, parsed.rows[0].registration)
    }
    @Test fun missingConfigurationAndUnknownInventoryAreNotInventedAsValidEmpty() {
        val raw = f.preview(0).put("status", "unknown").put("registered_count", JSONObject.NULL)
        raw.getJSONObject("saved").put("status", "unreadable").put("enabled", JSONObject.NULL)
        raw.getJSONObject("sample").put("comparison", "unknown").put("definitions_complete", false).put("stable", false)
        val parsed = parse(raw)
        assertEquals(SUMHMapsSavedStatus.Unreadable, parsed.savedStatus); assertNull(parsed.registeredCount)
    }
    @Test fun receiptsAreActualHistoryAndLatestPendingDoesNotReplaceAcknowledgement() {
        val raw = f.withReceipt(latest = f.receipt("pending").put("generation_after", ""), confirmed = f.receipt().put("boot_id", "old-boot"))
        val parsed = parse(raw)
        assertEquals("pending", parsed.submission.latest?.phase)
        assertEquals("acknowledged", parsed.submission.confirmed?.phase)
        assertEquals("old-boot", parsed.submission.confirmed?.bootId)
        assertEquals("4", parsed.submission.confirmed?.rows?.single()?.targetIno)
        assertEquals("18446744073709551615", parsed.rows.single().target.ino)
    }
    @Test fun invalidReceiptCannotBecomeAcknowledgedButUsefulCurrentSampleRemains() {
        val raw = f.withReceipt(confirmed = f.receipt("pending"))
        val parsed = parse(raw)
        assertEquals("invalid", parsed.submission.status); assertNull(parsed.submission.confirmed)
        assertEquals(1, parsed.rows.size)
    }
    @Test fun validNonzeroTransportReplyRetainsPartialFactsAndError() {
        val result = SUMHMapsPreviewParser.reply(SUMHQueryReply(f.preview().toString(), "transport incomplete"))
        assertNotNull(result.value); assertEquals("transport incomplete", result.error)
        val malformed = SUMHMapsPreviewParser.reply(SUMHQueryReply("not JSON", "timeout"))
        assertNull(malformed.value); assertTrue(malformed.error!!.contains("timeout"))
    }
    @Test fun additiveV1FieldsAreAllowedButMalformedPathsAndBudgetAreRejected() {
        assertEquals(1, parse(f.preview().put("future_optional", true)).rows.size)
        val path = f.preview(); path.getJSONArray("rows").getJSONObject(0).put("target_path", "/a\u0000")
        assertFails { parse(path) }
        assertFails { SUMHMapsPreviewParser.parse(" ".repeat(512 * 1024) + f.preview()) }
    }
    @Test fun fullyEscapedPayloadUsesSeparateBoundedEnvelopeBudget() {
        val payload = "\\".repeat(512 * 1024)
        val envelope = JSONObject().put("ok", true).put("exit_code", 0).put("errno", 0)
            .put("stdout", payload).put("stderr", "").toString()
        assertTrue(envelope.toByteArray().size > 1024 * 1024)
        assertEquals(payload, SUMHControlStateParser.reply(envelope, 512 * 1024, 4 * 1024 * 1024).stdout)
        assertFails { SUMHControlStateParser.reply(envelope, 512 * 1024) }
        assertFails { SUMHControlStateParser.reply(" ".repeat(4 * 1024 * 1024) + "{}", 512 * 1024, 4 * 1024 * 1024) }
    }
    @Test fun contradictoryHistoricalAcknowledgementAndDuplicateIdentityAreNotTrusted() {
        val conflicting = parse(f.withReceipt(confirmed = f.receipt().put("boot_id", "other-boot")))
        assertEquals("invalid", conflicting.submission.status)
        val duplicate = f.receipt()
        val rows = duplicate.getJSONArray("rows")
        rows.put(JSONObject(rows.getJSONObject(0).toString()).put("target_path", "/different-path"))
        assertEquals("invalid", parse(f.withReceipt(latest = duplicate, confirmed = duplicate)).submission.status)
        val rejected = f.receipt("rejected").put("errno", 0)
        assertEquals("invalid", parse(f.withReceipt(latest = rejected)).submission.status)
    }
    @Test fun completeRequiresStableFullSampleSavedDefinitionsAndContext() {
        for (field in listOf("boot_id", "config_revision", "maps_revision", "saved_status", "identities_sampled",
            "definitions_complete", "stable", "namespace_ino", "instance_id", "generation", "registered_count", "count_mismatch")) {
            val raw = f.preview()
            when (field) {
                "boot_id", "config_revision", "registered_count" -> raw.put(field, JSONObject.NULL)
                "maps_revision" -> raw.getJSONObject("saved").put(field, JSONObject.NULL)
                "saved_status" -> raw.getJSONObject("saved").put("status", "unreadable")
                "count_mismatch" -> raw.put("registered_count", 0)
                "namespace_ino", "instance_id", "generation" -> raw.getJSONObject("sample").put(field, JSONObject.NULL)
                else -> raw.getJSONObject("sample").put(field, false)
            }
            assertFails(field) { parse(raw) }
        }
    }
    @Test fun unknownEnvelopeWithNoContextIsAcceptedWithoutInventedFacts() {
        val raw = f.preview(0).put("status", "unknown").put("boot_id", JSONObject.NULL).put("config_revision", JSONObject.NULL)
            .put("registered_count", JSONObject.NULL)
        raw.getJSONObject("saved").put("status", "unreadable").put("enabled", JSONObject.NULL).put("maps_revision", JSONObject.NULL)
        raw.getJSONObject("sample").put("stable", false).put("identities_sampled", false).put("definitions_complete", false)
            .put("namespace_ino", JSONObject.NULL).put("instance_id", JSONObject.NULL).put("generation", JSONObject.NULL).put("comparison", "unknown")
        assertEquals(SUMHMapsPreviewStatus.Unknown, parse(raw).status)
    }
    @Test fun regularIdentityMustBeRepresentableAndNonzero() {
        for ((field, value) in listOf("ino" to "0", "dev" to "0", "dev" to "4294967296")) {
            val raw = f.preview()
            raw.getJSONArray("rows").getJSONObject(0).getJSONObject("target").put(field, value)
            assertFails { parse(raw) }
        }
    }
    @Test fun historyStabilityRemainsSeparateFromCurrentObservation() {
        val raw = f.withReceipt()
        raw.getJSONObject("last_submission").put("stable", false)
        val result = parse(raw)
        assertFalse(result.submission.stable!!)
        assertNotNull(result.submission.confirmed)
        val missingLatest = f.withReceipt()
        missingLatest.getJSONObject("last_submission").put("latest", JSONObject.NULL)
        assertEquals("invalid", parse(missingLatest).submission.status)
    }
    @Test fun full256RowsAndBothCompleteReceiptsFitDocumentNodeAndByteLimits() {
        val receipt = f.receipt()
        val rows = org.json.JSONArray((0 until 256).map { index -> JSONObject().put("target_path", "/目标/$index ")
            .put("replacement_path", "/system/替换 ").put("target_ino", (index + 1).toString()).put("target_dev", "1")
            .put("spoofed_ino", "9").put("spoofed_dev", "1") })
        receipt.put("rows", rows)
        val parsed = parse(f.withReceipt(f.preview(256), receipt, receipt))
        assertEquals(256, parsed.rows.size)
        assertEquals(256, parsed.submission.confirmed?.rows?.size)
    }
    @Test fun explicitSupersessionReferencesAnotherPendingIdAndMissingFieldIsUntrusted() {
        val latest = f.receipt("pending").put("supersedes_pending_id", "b".repeat(32))
        assertEquals("b".repeat(32), parse(f.withReceipt(latest = latest)).submission.latest?.supersedesPendingId)
        assertEquals("invalid", parse(f.withReceipt(latest = f.receipt("pending").put("supersedes_pending_id", "a".repeat(32)))).submission.status)
        val missing = f.receipt(); missing.remove("supersedes_pending_id")
        assertEquals("invalid", parse(f.withReceipt(latest = missing, confirmed = missing)).submission.status)
    }
}
