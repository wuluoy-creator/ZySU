package com.zying.zysu.ui.sumh.util

import android.app.Application
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.test.runTest
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.IOException
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHHideDefinitionsTest {
    private val f = SUMHHideDefinitionsFixtures
    private fun page(value: JSONObject) = HideDefinitionsParser.page(f.reply(value))
    private fun invalidPage(value: JSONObject) { assertFailsWith<IllegalArgumentException> { page(value) } }
    private fun invalidMutation(value: JSONObject, action: String = value.getString("action")) {
        val result = f.parsedMutation(value, action)
        assertNull(result.value); assertFalse(result.succeeded); assertNotNull(result.error)
    }

    @Test fun dispatchSeparatesTransportCommandAndResourceSuccess() {
        val value = HideDefinitionsParser.dispatch(f.envelope())
        assertNull(value.error); assertEquals(1, page(JSONObject(value.stdout)).count)
        val partial = f.mutation(status = "partial")
        val failed = HideDefinitionsParser.dispatch(f.envelope(f.dispatch(partial.toString(), 1)))
        assertEquals(partial.toString(), failed.stdout); assertNotNull(failed.error)
        val parsed = HideDefinitionsParser.mutation(failed, "disable", f.revision, f.boot, f.id(1))
        assertEquals("partial", parsed.value?.status); assertFalse(parsed.succeeded)
        assertFailsWith<IllegalArgumentException> { HideDefinitionsParser.dispatch(f.envelope(ok = false)) }
    }

    @Test fun malformedOrTruncatedDispatchNeverExposesSuccessfulReceipt() {
        for (field in listOf("stdout_complete", "stderr_complete")) {
            assertFailsWith<IllegalArgumentException> { HideDefinitionsParser.dispatch(f.envelope(f.dispatch().put(field, false))) }
        }
        for (bad in listOf(f.dispatch().put("schema_version", 2), f.dispatch().put("resource", "other"),
            f.dispatch().put("exit_code", "0"), f.dispatch().put("stdout", "x".repeat(64 * 1024 + 1)),
            f.dispatch().put("stderr", "x".repeat(64 * 1024 + 1)))) {
            assertFailsWith<IllegalArgumentException> { HideDefinitionsParser.dispatch(f.envelope(bad)) }
        }
        for (raw in listOf("{}", f.envelope() + " {}", "{\"ok\":true,\"ok\":true}")) {
            assertFails { HideDefinitionsParser.dispatch(raw) }
        }
    }

    @Test fun pagePreservesAllDefinitionBytesAndUnsignedRuntimeIdentity() {
        val path = "/路径\n尾部 "
        val row = page(f.page(listOf(f.row(path = path)))).rows.single()
        assertEquals(path, row.path); assertEquals("组 ", row.group); assertEquals("备注\n保留", row.note)
        assertEquals("18446744073709551615", row.runtime?.id)
        assertEquals("18446744073709551614", row.runtime?.generation)
    }

    @Test fun pageDistinguishesDisabledSuspendedPendingErrorAndUnknown() {
        val rows = listOf(f.row(1, false, false), f.row(2, false, true), f.row(3, true, false),
            f.row(4, management = 1), f.row(5, binding = 0), f.row(6, binding = 1), f.row(7, binding = 3))
        assertEquals(listOf("disabled", "disabled_pending_removal", "unregistered", "globally_suspended", "pending", "pending", "binding_error"), page(f.page(rows)).rows.map { it.state })
        val unsupported = page(f.page(listOf(f.row(registered = null)), managed = false, query = "unsupported"))
        assertEquals(false, unsupported.managed); assertNull(unsupported.rows.single().registered)
        val unknown = page(f.page(listOf(f.row(registered = null)), managed = null, query = "unavailable"))
        assertNull(unknown.managed); assertEquals("unknown", unknown.rows.single().state)
    }

    @Test fun pageRejectsContradictoryRegistrationStateAndCapabilities() {
        for (row in listOf(f.row().put("state", "disabled"), f.row().put("registered", false),
            f.row().put("runtime", JSONObject.NULL), f.row(registered = null).put("state", "bound"),
            f.row().put("enabled", "true"))) invalidPage(f.page(listOf(row)))
        invalidPage(f.page(managed = false)); invalidPage(f.page(query = "unsupported"))
        invalidPage(f.page().put("runtime_verified", true)); invalidPage(f.page().put("read_only", false))
        invalidPage(f.page().put("runtime_consistency", "atomic"))
    }

    @Test fun pageRejectsMalformedIdsPathBytesCapacityAndDuplicates() {
        for (bad in listOf("", "A".repeat(32), "f".repeat(33))) invalidPage(f.page(listOf(f.row().put("id", bad))))
        for (bad in listOf("relative", "/" + "a".repeat(4095), "/nul\u0000x", "/\uD800")) invalidPage(f.page(listOf(f.row(path = bad))))
        for ((field, size) in listOf("name" to 128, "group" to 128, "note" to 1024)) {
            invalidPage(f.page(listOf(f.row().put(field, "é".repeat(size / 2 + 1)))))
        }
        invalidPage(f.page(listOf(f.row(), f.row())))
        invalidPage(f.page(listOf(f.row(1), f.row(2, path = "/test/1"))))
        invalidPage(f.page((1..17).map { f.row(it) }))
        invalidPage(f.page().put("count", 4097)); invalidPage(f.page().put("schema_version", 2))
        assertFailsWith<IllegalArgumentException> { HideDefinitionsParser.page(SUMHQueryReply("{\"schema_version\":1,\"schema_version\":1}")) }
        assertFails { HideDefinitionsParser.page(SUMHQueryReply(f.page().toString() + " {}")) }
    }

    @Test fun pageRejectsCursorAndCompleteContradictions() {
        for (bad in listOf(f.page().put("next_cursor", 0), f.page().put("at_end", false),
            f.page().put("complete", false), f.page().put("cursor", -1), f.page().put("count", 0),
            f.page(emptyList(), 1).put("next_cursor", 0))) invalidPage(bad)
        val final = page(f.page(emptyList(), count = 17, cursor = 17))
        assertTrue(final.atEnd); assertFalse(final.complete); assertTrue(final.rows.isEmpty())
    }

    @Test fun mutationAcceptsSavedMetadataAndRegistrationOnlyOutcomes() {
        for (status in listOf("saved", "unchanged")) {
            val value = f.parsedMutation(f.mutation("annotate", status))
            assertTrue(value.succeeded); assertTrue(value.value!!.items.isEmpty())
        }
        for (outcome in listOf("registered_bound", "registered_pending", "registered_binding_error")) {
            val raw = f.mutation("enable").put("items", JSONArray().put(f.item(outcome = outcome)))
            val value = f.parsedMutation(raw)
            assertTrue(value.succeeded); assertEquals(outcome, value.value!!.items.single().outcome)
        }
        for (action in listOf("disable", "remove-id")) assertTrue(f.parsedMutation(f.mutation(action)).succeeded)
    }

    @Test fun mutationPreservesPartialAndUnknownWithoutCallingThemSuccess() {
        val partial = f.parsedMutation(f.mutation(status = "partial"), error = "command failed")
        assertEquals(true, partial.value?.saved); assertEquals(1, partial.value?.failed); assertFalse(partial.succeeded)
        val readbackUnknown = f.mutation(status = "partial").put("failed_count", 0).put("unknown_count", 1)
            .put("items", JSONArray().put(f.item(outcome = "outcome_unknown")))
        assertEquals(1, f.parsedMutation(readbackUnknown).value?.unknown)
        for (status in listOf("rejected", "not_saved", "outcome_unknown")) {
            val value = f.parsedMutation(f.mutation(status = status))
            assertEquals(status, value.value?.status); assertFalse(value.succeeded)
        }
        assertFalse(f.parsedMutation(f.mutation(), error = "disconnect").succeeded)
    }

    @Test fun mutationRejectsWrongSelectorRevisionBootOrAction() {
        invalidMutation(f.mutation(number = 2))
        invalidMutation(f.mutation(previous = f.nextRevision))
        invalidMutation(f.mutation().put("boot_id", "another-boot"))
        invalidMutation(f.mutation(), "enable")
        invalidMutation(f.mutation().put("runtime_verified", true))
        invalidMutation(f.mutation().put("verification_scope", "observer_visibility"))
        invalidMutation(f.mutation().put("revision", "bad"))
        invalidMutation(f.mutation().put("saved", false))
    }

    @Test fun mutationRejectsImpossibleActionAndStatusCombinations() {
        invalidMutation(f.mutation("annotate", "applied"))
        invalidMutation(f.mutation("annotate", "partial"))
        invalidMutation(f.mutation("enable", "saved"))
        invalidMutation(f.mutation("disable", "unchanged"))
        for (action in listOf("enable-group", "disable-group")) {
            val empty = f.mutation(action).put("affected_count", 0).put("confirmed_count", 0).put("items", JSONArray())
            assertFalse(f.parsedMutation(empty, expectedId = null).succeeded)
        }
        invalidMutation(f.mutation("disable").put("items", JSONArray().put(f.item(outcome = "registered_bound"))))
        invalidMutation(f.mutation("enable").put("items", JSONArray().put(f.item())))
    }

    @Test fun mutationRejectsInconsistentItemCountsCompletenessAndBinding() {
        for (bad in listOf(f.mutation().put("confirmed_count", 0), f.mutation().put("failed_count", 1),
            f.mutation().put("unknown_count", 1), f.mutation().put("items_complete", false),
            f.mutation().put("items", JSONArray()), f.mutation().put("affected_count", 2))) invalidMutation(bad)
        for (bad in listOf(f.item().put("membership_matches", false), f.item().put("errno", 5), f.item().put("runtime", f.runtime()))) {
            invalidMutation(f.mutation().put("items", JSONArray().put(bad)))
        }
        invalidMutation(f.mutation("enable").put("items", JSONArray().put(f.item(outcome = "registered_bound").put("runtime", f.runtime(3)))))
        invalidMutation(f.mutation().put("schema_version", 2))
    }

    @Test fun groupResultCanBeExplicitlyTruncatedWhileRetainingWholeCounts() {
        val raw = f.mutation("disable-group").put("affected_count", 65).put("confirmed_count", 65)
            .put("items_complete", false).put("items", JSONArray((1..64).map { f.item(it) }))
        val result = f.parsedMutation(raw, expectedId = null)
        assertTrue(result.succeeded); assertEquals(65, result.value?.confirmed); assertEquals(64, result.value?.items?.size)
        assertFalse(result.value!!.itemsComplete)
        raw.put("items_complete", true)
        assertNull(f.parsedMutation(raw, expectedId = null).value)
    }

    @Test fun collectionChecksEveryPageThenFinalRevisionWithoutMutations() = runTest {
        val calls = mutableListOf<List<String>>(); val progress = mutableListOf<Int>()
        val result = collectHideDefinitions(request = { args ->
            calls += args
            when (calls.size) {
                1 -> f.reply(f.page((1..16).map { f.row(it) }, 17))
                2 -> f.reply(f.page(listOf(f.row(17)), 17, 16))
                else -> f.reply(f.page(emptyList(), 17, 17))
            }
        }, onProgress = { progress += it }, clock = { 0 })
        assertTrue(result.complete); assertTrue(result.canMutate); assertEquals(17, result.rows.size)
        assertEquals(listOf(16, 17), progress)
        assertEquals(listOf(listOf("hide", "status"), listOf("hide", "status", f.revision, "16"), listOf("hide", "status", f.revision, "17")), calls)
    }

    @Test fun emptyCollectionStillPerformsFinalVersionCheck() = runTest {
        var calls = 0
        val result = collectHideDefinitions(request = { calls++; f.reply(f.page(emptyList())) }, clock = { 0 })
        assertTrue(result.complete); assertEquals(2, calls); assertTrue(result.rows.isEmpty())
    }

    @Test fun collectionRetainsPartialRowsOnChangedIdentityDuplicateOrUnavailablePage() = runTest {
        for (variant in 0..6) {
            var calls = 0
            val result = collectHideDefinitions(request = {
                calls++
                if (calls == 1) f.reply(f.page(listOf(f.row(1)), 2)) else {
                    val next = f.page(listOf(f.row(if (variant == 4) 1 else 2)), 2, 1)
                    when (variant) {
                        0 -> next.put("revision", f.nextRevision)
                        1 -> next.put("boot_id", "other")
                        2 -> next.put("count", 3).put("at_end", false)
                        3 -> next.put("cursor", 0).put("next_cursor", 1).put("at_end", false)
                        5 -> return@collectHideDefinitions f.reply(next, "unavailable")
                        6 -> throw IOException("timeout")
                    }
                    f.reply(next)
                }
            }, clock = { 0 })
            assertFalse(result.complete); assertFalse(result.canMutate); assertNotNull(result.error)
            assertEquals(listOf(f.id(1)), result.rows.map { it.id }); assertEquals(2, calls)
        }
    }

    @Test fun collectionFinalRereadChangeCannotAuthorizeMutation() = runTest {
        var calls = 0
        val result = collectHideDefinitions(request = {
            calls++; f.reply(if (calls == 1) f.page() else f.page(emptyList(), 1, 1, version = f.nextRevision))
        }, clock = { 0 })
        assertFalse(result.complete); assertFalse(result.canMutate); assertEquals(1, result.rows.size); assertEquals(2, calls)
    }

    @Test fun collectionRejectsEmptyBootAndRetainsUnknownCapability() = runTest {
        var calls = 0
        val unknown = collectHideDefinitions(request = {
            calls++; f.reply(if (calls == 1) f.page(listOf(f.row(registered = null)), managed = null, query = "unavailable")
                else f.page(emptyList(), 1, 1, managed = null, query = "unavailable"))
        }, clock = { 0 })
        assertTrue(unknown.complete); assertNull(unknown.managed); assertEquals("unknown", unknown.rows.single().state)
        calls = 0
        val empty = collectHideDefinitions(request = { calls++; f.reply(f.page(emptyList(), bootId = "")) }, clock = { 0 })
        assertFalse(empty.complete); assertFalse(empty.canMutate)
    }

    @Test fun collectionDeadlineAndCancellationDoNotTriggerReplay() = runTest {
        var now = 0L; var calls = 0
        val result = collectHideDefinitions(request = { calls++; now = 30_000; f.reply(f.page()) }, clock = { now })
        assertFalse(result.complete); assertEquals(1, calls)
        calls = 0
        assertFailsWith<CancellationException> {
            collectHideDefinitions(request = { calls++; throw CancellationException("cancelled") }, clock = { 0 })
        }
        assertEquals(1, calls)
    }

    @Test fun maximumDefinitionCountCollectsWithBoundedPages() = runTest {
        var calls = 0
        val result = collectHideDefinitions(request = { args ->
            calls++; val start = args.getOrNull(3)?.toInt() ?: 0
            val end = minOf(start + 16, 4096)
            f.reply(f.page((start until end).map { f.row(it + 1) }, 4096, start))
        }, clock = { 0 })
        assertTrue(result.complete); assertEquals(4096, result.rows.size); assertEquals(257, calls)
    }

    @Test fun collectionByteBudgetStopsBeforeUnboundedAccumulation() = runTest {
        var calls = 0
        val result = collectHideDefinitions(request = { args ->
            calls++; val start = args.getOrNull(3)?.toInt() ?: 0
            val end = minOf(start + 10, 4096)
            f.reply(f.page((start until end).map { f.row(it + 1, path = "/" + "x".repeat(4080) + it) }, 4096, start))
        }, clock = { 0 })
        assertFalse(result.complete); assertFalse(result.canMutate); assertTrue(result.bytes > 8 * 1024 * 1024)
        assertTrue(calls < 410); assertTrue(result.rows.size < 4096)
    }

    @Test fun batchChainsOnlyConfirmedRevisionAndDeduplicatesStableIds() = runTest {
        val initial = f.collection(); val calls = mutableListOf<List<String>>()
        val progress = mutableListOf<Pair<Int, Int>>()
        val result = changeHideDefinitionSelection("disable", listOf(f.id(1), f.id(2), f.id(1)), initial,
            request = { args, previous, boot ->
                calls += args; assertEquals(f.boot, boot)
                val number = if (calls.size == 1) 1 else 2
                f.parsedMutation(f.mutation(number = number, previous = previous), expected = previous, expectedId = f.id(number))
            }, onProgress = { done, count -> progress += done to count }, clock = { 0 })
        assertEquals(2, result.succeededCount); assertTrue(result.unattempted.isEmpty()); assertNull(result.stoppedReason)
        assertEquals(f.revision, calls[0][3]); assertEquals(f.nextRevision, calls[1][3])
        assertEquals(listOf(0 to 2, 1 to 2, 2 to 2), progress)
    }

    @Test fun batchStopsAfterPartialUnknownTransportOrContradictoryIdentity() = runTest {
        for (variant in 0..4) {
            var calls = 0
            val result = changeHideDefinitionSelection("disable", listOf(f.id(1), f.id(2), f.id(3)), f.collection(),
                request = { _, previous, _ ->
                    calls++
                    when (variant) {
                        0 -> f.parsedMutation(f.mutation(status = "partial"))
                        1 -> f.parsedMutation(f.mutation(status = "outcome_unknown"))
                        2 -> throw IOException("timeout")
                        3 -> f.parsedMutation(f.mutation(number = 2), expectedId = f.id(2))
                        else -> f.parsedMutation(f.mutation(previous = f.nextRevision), expected = f.nextRevision)
                    }.also { assertEquals(f.revision, previous) }
                }, clock = { 0 })
            assertEquals(1, calls); assertEquals(listOf(f.id(2), f.id(3)), result.unattempted)
            assertNotNull(result.stoppedReason)
        }
    }

    @Test fun batchRejectsIncompleteUnknownOwnershipMissingAndDisabledRetryBeforeWrite() = runTest {
        var calls = 0
        for (initial in listOf(f.collection().copy(complete = false), f.collection().copy(managed = null), f.collection().copy(managed = false))) {
            assertFailsWith<IllegalArgumentException> {
                changeHideDefinitionSelection("disable", listOf(f.id(1)), initial, request = { _, _, _ -> calls++; error("must not run") })
            }
        }
        for ((action, ids, initial) in listOf(Triple("retry", listOf(f.id(1)), f.collection(listOf(f.row(enabled = false, registered = false)))),
            Triple("disable", listOf(f.id(99)), f.collection()), Triple("disable", emptyList(), f.collection()))) {
            assertFailsWith<IllegalArgumentException> {
                changeHideDefinitionSelection(action, ids, initial, request = { _, _, _ -> calls++; error("must not run") })
            }
        }
        assertEquals(0, calls)
    }

    @Test fun batchStopDeadlineAndCancellationDoNotResubmit() = runTest {
        var calls = 0; var now = 0L
        val stopped = changeHideDefinitionSelection("disable", listOf(f.id(1), f.id(2)), f.collection(),
            request = { _, _, _ -> calls++; now = 30_000; f.parsedMutation(f.mutation()) }, clock = { now })
        assertEquals(1, calls); assertEquals(listOf(f.id(2)), stopped.unattempted)
        calls = 0
        val cancelled = changeHideDefinitionSelection("disable", listOf(f.id(1)), f.collection(),
            request = { _, _, _ -> calls++; error("must not run") }, shouldContinue = { false }, clock = { 0 })
        assertEquals(0, calls); assertEquals(listOf(f.id(1)), cancelled.unattempted)
        assertFailsWith<CancellationException> {
            changeHideDefinitionSelection("disable", listOf(f.id(1), f.id(2)), f.collection(),
                request = { _, _, _ -> calls++; throw CancellationException() }, clock = { 0 })
        }
        assertEquals(1, calls)
    }
}
