package com.zying.zysu.ui.sumh.util

import kotlinx.coroutines.test.runTest
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import android.app.Application
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHHideExportPagesTest {
    private val revision = "sha256-v1:" + "a".repeat(64)
    private fun page(count: Int, cursor: Int, size: Int, boot: String = "boot", rev: String = revision): JSONObject = JSONObject()
        .put("schema_version", 1).put("resource", "user_hide_definition_export_page").put("read_only", true)
        .put("runtime_verified", false).put("scope", "persistent_user_hide_definitions").put("boot_id", boot)
        .put("status", "observed").put("error", "").put("revision", rev).put("source_schema", 1)
        .put("count", count).put("cursor", cursor).put("next_cursor", cursor + size)
        .put("at_end", cursor + size == count).put("complete", cursor == 0 && size == count)
        .put("items", JSONArray((cursor until cursor + size).map { index -> JSONObject()
            .put("id", index.toString(16).padStart(32, '0')).put("path", "/test/$index\n尾 ")
            .put("enabled", false).put("name", "名字").put("note", "\r\t ").put("group", "") }))
    private fun reply(page: JSONObject) = SUMHQueryReply(page.toString())
    private fun rejects(block: () -> Unit) { assertTrue(runCatching(block).isFailure) }

    @Test fun emptyBootAllowsPersistentExportButNeverMutationReview() = runTest {
        val sample = collectSUMHHideSavedDefinitions(request = { args ->
            reply(if (args.size == 2) page(1, 0, 1, "") else page(1, 1, 0, ""))
        })
        assertTrue(sample.canExport); assertFalse(sample.canReviewMutation)
        assertEquals("/test/0\n尾 ", sample.rules.single().path)
        assertEquals("\r\t ", sample.rules.single().note)
    }
    @Test fun full4096RowsAndFinalEmptyPageAreCollectedWithoutRuntimeSampling() = runTest {
        var calls = 0
        val sample = collectSUMHHideSavedDefinitions(request = { args ->
            calls++; assertEquals(listOf("hide", "definitions"), args.take(2))
            val cursor = args.getOrNull(3)?.toInt() ?: 0
            if (cursor != 0) assertEquals(revision, args[2])
            reply(page(4096, cursor, minOf(64, 4096 - cursor)))
        })
        assertTrue(sample.error, sample.canReviewMutation); assertEquals(4096, sample.rules.size); assertEquals(65, calls)
    }
    @Test fun failureRetainsPartialRowsButBlocksExport() = runTest {
        var calls = 0
        val sample = collectSUMHHideSavedDefinitions(request = {
            if (++calls == 1) reply(page(65, 0, 64)) else SUMHQueryReply("", "unavailable")
        })
        assertFalse(sample.canExport); assertEquals(64, sample.rules.size); assertNotNull(sample.error)
    }
    @Test fun finalRevisionAndBootChangesRefuseExport() = runTest {
        for (changeBoot in listOf(false, true)) {
            var calls = 0
            val sample = collectSUMHHideSavedDefinitions(request = {
                reply(if (++calls == 1) page(1, 0, 1) else page(1, 1, 0,
                    if (changeBoot) "new-boot" else "boot", if (changeBoot) revision else "sha256-v1:" + "b".repeat(64)))
            })
            assertFalse(sample.canExport); assertEquals(1, sample.rules.size)
        }
    }
    @Test fun malformedUnknownDuplicateAndRuntimeFieldsAreRejected() {
        listOf<(JSONObject) -> Unit>(
            { it.put("schema_version", 2) }, { it.put("runtime_verified", true) }, { it.put("managed_ownership", true) },
            { it.put("complete", false) }, { it.put("next_cursor", 0) }, { it.put("source_schema", 2) },
            { it.getJSONArray("items").getJSONObject(0).put("path", "relative") },
            { it.getJSONArray("items").getJSONObject(0).put("enabled", "false") },
            { it.getJSONArray("items").getJSONObject(0).put("runtime", JSONObject()) }
        ).forEach { mutation -> rejects { parseSUMHHideExportPage(reply(page(1, 0, 1).also(mutation))) } }
        rejects { parseSUMHHideExportPage(SUMHQueryReply(page(0, 0, 0).toString().replace("\"count\":0", "\"count\":0,\"count\":0"))) }
    }
    @Test fun requestBoundaryDeadlinePreventsFollowingPage() = runTest {
        var now = 0L; var calls = 0
        val sample = collectSUMHHideSavedDefinitions(request = { calls++; now = 30_000; reply(page(0, 0, 0)) }, clock = { now })
        assertFalse(sample.canExport); assertEquals(1, calls)
    }
    @Test fun capabilitiesRequireEveryExactNumericSchemaAndNeverFallback() {
        val value = JSONObject().put("hide_definition_exchange_schema", 1).put("hide_definition_export_page_schema", 1)
        assertTrue(parseSUMHHideTransferCapabilities(reply(value)).export)
        assertFalse(parseSUMHHideTransferCapabilities(reply(value)).import)
        value.put("hide_definition_import_schema", 1)
        assertTrue(parseSUMHHideTransferCapabilities(reply(value)).import)
        for (unsupported in listOf<Any>(2, "1", true, JSONObject.NULL)) {
            value.put("hide_definition_import_schema", unsupported)
            assertFalse(parseSUMHHideTransferCapabilities(reply(value)).import)
        }
        rejects { parseSUMHHideTransferCapabilities(SUMHQueryReply(value.toString(), "failed")) }
    }
    @Test fun duplicateAcrossPagesAndChangedCountAreRejected() = runTest {
        for (duplicate in listOf(true, false)) {
            var calls = 0
            val sample = collectSUMHHideSavedDefinitions(request = {
                reply(if (++calls == 1) page(65, 0, 64) else page(if (duplicate) 65 else 66, 64, 1).also { next ->
                    if (duplicate) next.getJSONArray("items").getJSONObject(0).put("id", "0".repeat(32))
                })
            })
            assertFalse(sample.canExport); assertEquals(64, sample.rules.size)
        }
    }
}
