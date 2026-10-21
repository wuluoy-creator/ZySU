package com.zying.zysu.ui.sumh.util

import android.app.Application
import kotlinx.coroutines.*
import org.json.JSONObject
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHInventoryTest {
    private val f = SUMHInventoryFixtures
    private val mapping = SUMHInventoryCategory.Mapping
    private fun page(entry: JSONObject = f.entry(mapping)) = f.page(mapping, entries = listOf(entry))
    private fun parse(raw: JSONObject) = SUMHInventoryParser.parse(raw.toString())
    @Test fun pathsPreserveUnicodeNewlinesAndTrailingSpacesExactly() {
        val parsed = parse(page()).entries.single()
        assertEquals("/visible 1\n名字 ", parsed.paths.getValue("visible_path").utf8)
        assertEquals("/source/1", parsed.paths.getValue("backing_path").utf8)
        assertEquals(SUMHInventoryOwner.Unattributed, parsed.owner)
    }
    @Test fun invalidUtf8IsRetainedAsBytesWithoutReplacement() {
        val item = f.entry(mapping).put("visible_path", JSONObject().put("encoding", "hex").put("value", "2fff80").put("byte_length", 3))
        val bytes = parse(page(item)).entries.single().paths.getValue("visible_path")
        assertEquals("2fff80", bytes.hex)
        assertNull(bytes.utf8)
    }
    @Test fun malformedEncodingLengthsAndNullPathsFail() {
        val bad = listOf(f.bytes("/a").put("byte_length", 1), f.bytes("/a\u0000"), f.bytes("/a").put("encoding", "unknown"),
            JSONObject().put("encoding", "hex").put("value", "2fg0").put("byte_length", 2),
            JSONObject().put("encoding", "hex").put("value", "2f0").put("byte_length", 1),
            f.bytes("/a").put("byte_length", "2"), f.bytes("/a").put("value", "\ud800"))
        for (value in bad) assertFails { parse(page(f.entry(mapping).put("visible_path", value))) }
        assertFails { parse(page(f.entry(mapping).put("visible_path", JSONObject.NULL))) }
    }
    @Test fun mapsNumbersAreUnsignedStringsAndOwnersAreNotGuessed() {
        val category = SUMHInventoryCategory.ManagedMaps
        val item = f.entry(category)
        assertEquals("18446744073709551615", parse(f.page(category, entries = listOf(item))).entries.single().numbers["target_ino"])
        for (bad in listOf<Any>(1, "-1", "01", "18446744073709551616", JSONObject.NULL)) {
            assertFails { parse(f.page(category, entries = listOf(f.entry(category).put("target_ino", bad)))) }
        }
        assertFails { parse(page(f.entry(mapping).put("owner", "managed_maps"))) }
        assertFails { parse(page(f.entry(mapping).put("mutation_handle", true))) }
        assertFails { parse(page(f.entry(mapping).put("id_kind", "managed_definition"))) }
    }
    @Test fun malformedCursorEmptyNonfinalAndCompletenessClaimsFail() {
        for (key in listOf("cursor", "next_cursor", "generation", "instance_id")) {
            val raw = page(); raw.getJSONObject("page").put(key, "01")
            assertFails { parse(raw) }
        }
        val gap = page(); gap.getJSONObject("page").put("next_cursor", "2")
        assertFails { parse(gap) }
        assertFails { parse(f.page(mapping, end = false)) }
        assertFails { parse(f.page().put("complete", true)) }
        assertFails { parse(f.page(mapping, cursor = 1, entries = listOf(f.entry(mapping, 2))).put("complete", true)) }
    }
    @Test fun futureMissingAndUnquotedDocumentsNeverBecomeEmpty() {
        assertFails { parse(f.page().put("schema_version", 2)) }
        val missing = f.page(); missing.remove("page")
        assertFails { parse(missing) }
        assertFails { SUMHInventoryParser.parse("{schema_version:1}") }
        assertFails { SUMHInventoryParser.parse(f.page().toString() + "{}") }
        assertFails { SUMHInventoryParser.parse("{\"schema_version\":1,\"schema_version\":1}") }
    }
    @Test fun unsupportedLegacyIncludingLargeControlTextIsExplicitlyIncomplete() {
        val legacy = "\u0001".repeat(64 * 1024 - 1)
        val raw = f.unsupported(f.bytes(legacy)).toString()
        assertTrue(raw.toByteArray().size > 384 * 1024)
        val envelope = JSONObject().put("ok", true).put("exit_code", 0).put("errno", 0).put("stderr", "").put("stdout", raw)
        val reply = SUMHControlStateParser.reply(envelope.toString(), 1024 * 1024)
        val parsed = SUMHInventoryParser.parse(reply.stdout)
        assertEquals(SUMHInventoryPageStatus.Unsupported, parsed.status)
        assertEquals(legacy, parsed.legacy!!.utf8)
        assertFalse(parsed.atEnd)
    }
    @Test fun completeReadReusesSameIdentityAcrossEveryCategoryAndFinalCheck() = runBlocking {
        val calls = mutableListOf<List<String>>()
        val progress = mutableListOf<SUMHInventoryCollection>()
        val result = collectSUMHInventory({ args -> calls.add(args.toList()); f.respond(args, perCategory = 2) }, onProgress = progress::add)
        assertEquals(SUMHInventoryRead.Complete, result.status)
        assertEquals(14, result.entries.size)
        assertEquals(7, result.completedCategories.size)
        assertEquals(setOf(SUMHInventoryCategory.OverlayXattr, SUMHInventoryCategory.Kstat, SUMHInventoryCategory.Vnode), result.unsupportedCategories)
        assertTrue(calls.drop(2).all { it[3] == "23" && it[4] == "9" })
        assertEquals(listOf("api", "inventory", "metadata", "23", "9", "0"), calls.last())
        assertTrue(progress.all { it.status == SUMHInventoryRead.Loading })
    }
    @Test fun stalePageStopsWithoutRestartingOrPromotingPartialRecords() = runBlocking {
        var calls = 0
        val result = collectSUMHInventory({ args -> calls++; if (calls == 4) SUMHQueryReply(f.failure().toString(), "stale") else f.respond(args, 3) })
        assertEquals(4, calls)
        assertEquals(1, result.entries.size)
        assertEquals(SUMHInventoryRead.Stale, result.status)
    }
    @Test fun bootGenerationMaskCategoryAndCursorChangesAbort() = runBlocking {
        for (bad in listOf("boot_id", "instance_id", "generation", "supported_categories", "category", "cursor")) {
            var calls = 0
            val result = collectSUMHInventory({ args ->
                calls++
                val reply = f.respond(args)
                if (calls != 3) reply else {
                    val raw = JSONObject(reply.stdout)
                    when (bad) {
                        "boot_id" -> raw.put(bad, "other-boot")
                        "category" -> raw.put(bad, "merge")
                        "supported_categories" -> raw.getJSONObject("page").put(bad, 63)
                        else -> raw.getJSONObject("page").put(bad, "99")
                    }
                    SUMHQueryReply(raw.toString())
                }
            })
            assertEquals(SUMHInventoryRead.Failed, result.status, bad)
            assertEquals(3, calls, bad)
        }
    }
    @Test fun duplicateManagedHideRuntimeIdsAbort() = runBlocking {
        val result = collectSUMHInventory({ args ->
            val reply = f.respond(args, 2, mask = 64)
            if (args.getOrNull(2) != "user_hide" || args.last() != "1") reply else {
                val raw = JSONObject(reply.stdout)
                raw.getJSONObject("page").getJSONArray("entries").getJSONObject(0).put("id", "1")
                SUMHQueryReply(raw.toString())
            }
        })
        assertEquals(SUMHInventoryRead.Failed, result.status)
        assertEquals(1, result.entries.size)
    }
    @Test fun recordByteAndTimeBudgetsNeverClaimComplete() = runBlocking {
        val records = collectSUMHInventory({ f.respond(it, 3) }, SUMHInventoryBudget(records = 1))
        assertEquals(SUMHInventoryRead.Partial, records.status)
        assertEquals("record_budget", records.reason)
        assertEquals(1, records.entries.size)
        val bytes = collectSUMHInventory({ f.respond(it) }, SUMHInventoryBudget(bytes = 1))
        assertEquals(SUMHInventoryRead.Partial, bytes.status)
        assertEquals("byte_budget", bytes.reason)
        var time = 0L
        val timed = collectSUMHInventory({ time = 31; f.respond(it) }, SUMHInventoryBudget(milliseconds = 30), clockMs = { time })
        assertEquals(SUMHInventoryRead.Partial, timed.status)
        assertEquals("time_budget", timed.reason)
    }
    @Test fun cancellationAfterResponsePreventsNextPage() = runBlocking {
        var calls = 0
        val job = launch {
            collectSUMHInventory({ args -> calls++; if (calls == 3) currentCoroutineContext().cancel(); f.respond(args, 3) })
        }
        job.join()
        assertTrue(job.isCancelled)
        assertEquals(3, calls)
    }
    @Test fun finalMetadataStalePreventsCompleteAfterAllCategoryPages() = runBlocking {
        val result = collectSUMHInventory({ args ->
            if (args.size == 6 && args[2] == "metadata") SUMHQueryReply(f.failure(category = SUMHInventoryCategory.Metadata).toString())
            else f.respond(args)
        })
        assertEquals(7, result.completedCategories.size)
        assertEquals(SUMHInventoryRead.Stale, result.status)
    }
    @Test fun unsupportedKernelAndFutureControllerDoNotScanOrInventEmptyCollections() = runBlocking {
        var calls = 0
        val oldKernel = collectSUMHInventory({ args -> calls++; if (calls == 2) SUMHQueryReply(f.unsupported().toString()) else f.respond(args) })
        assertEquals(2, calls)
        assertEquals(SUMHInventoryRead.Unsupported, oldKernel.status)
        assertNotNull(oldKernel.legacy)
        calls = 0
        val future = collectSUMHInventory({ calls++; SUMHQueryReply("{\"rule_inventory_schema\":2}") })
        assertEquals(1, calls)
        assertEquals(SUMHInventoryRead.Unavailable, future.status)
        assertTrue(future.completedCategories.isEmpty())
    }
    @Test fun unknownAdditionalCategoriesAndNonzeroRepliesStayIncomplete() = runBlocking {
        val extra = collectSUMHInventory({ f.respond(it, mask = 255) })
        assertEquals(SUMHInventoryRead.Partial, extra.status)
        assertFalse(SUMHInventoryCategory.OverlayXattr in extra.unsupportedCategories)
        var calls = 0
        val partial = collectSUMHInventory({ args -> calls++; f.respond(args).let { if (calls == 4) it.copy(error = "transport failed after response") else it } })
        assertEquals(SUMHInventoryRead.Failed, partial.status)
        assertEquals(1, partial.entries.size)
    }
}
