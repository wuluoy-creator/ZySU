package com.zying.zysu.ui.sumh.util

import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.CancellationException

class SUMHLogViewTest {
    private fun keys(snapshot: SUMHLogSnapshot) = snapshot.lines.map { it.key }
    private fun view(text: String, levels: Set<SUMHLogLevel> = emptySet(), search: String = "") =
        filterSUMHLog(parseSUMHLog(text), levels, search)

    @Test fun explicitDaemonHeadersUseOnlyThePrefix() {
        val rows = listOf(
            "2026-10-18 12:00:00 [INFO] [test] ERROR in body" to SUMHLogLevel.INFO,
            "early-boot +123.456 [WARN] [test] /INFO/ERROR" to SUMHLogLevel.WARN,
            "early-boot (clock unset) [ERROR] [test] message" to SUMHLogLevel.ERROR,
            "[DEBUG] [logging] fallback" to SUMHLogLevel.DEBUG,
            "[VERBOSE] message" to SUMHLogLevel.VERBOSE,
            "[warning] message" to SUMHLogLevel.WARN,
            "[info] message" to SUMHLogLevel.INFO,
        )
        rows.forEach { (text, expected) -> assertEquals(text, expected, parseSUMHLog(text).lines.single().level) }
    }

    @Test fun ordinaryTextAndMalformedHeadersRemainUnclassified() {
        listOf("ERROR", "/system/INFO/file", "body [ERROR] text", "[INFO]suffix",
            "2026-10-18 bad time [ERROR]", "[FATAL] message", "early-boot +1 [ERROR] x",
            "<3> ordinary daemon text", "<3>KernelSU: other: ERROR",
            "<3>KernelSU: sumh:suffix", "KernelSU: sumh: ERROR",
            "<8>KernelSU: sumh: x", "<33>KernelSU: sumh: x", "<-1>KernelSU: sumh: x",
            "prefix <3>KernelSU: sumh: x").forEach {
            assertNull(it, parseSUMHLog(it).lines.single().level)
        }
    }

    @Test fun kernelPriorityUsesStandardSeverityAndValidatedHeader() {
        val expected = listOf(SUMHLogLevel.ERROR, SUMHLogLevel.ERROR, SUMHLogLevel.ERROR,
            SUMHLogLevel.ERROR, SUMHLogLevel.WARN, SUMHLogLevel.INFO, SUMHLogLevel.INFO, SUMHLogLevel.DEBUG)
        expected.forEachIndexed { index, level ->
            assertEquals(level, parseSUMHLog(" <$index>[ 12.345] [ T43] KernelSU: sumh: message").lines.single().level)
            assertEquals(level, parseSUMHLog("<$index>KernelSU: sumh: message").lines.single().level)
        }
    }

    @Test fun emptyContentIsZeroRows() {
        val snapshot = parseSUMHLog("")
        assertTrue(snapshot.lines.isEmpty())
        assertEquals(1L, snapshot.nextKey)
        assertEquals("", filterSUMHLog(snapshot, emptySet(), "").copyText)
    }

    @Test fun separatorsBlankRowsWhitespaceAndUnicodeArePreserved() {
        val result = view("alpha \r\n\r\n中文😀\rbeta\n")
        assertEquals(listOf("alpha ", "", "中文😀", "beta", ""), result.lines.map { it.text })
        assertEquals("alpha \n\n中文😀\nbeta\n", result.copyText)
        assertEquals(5, result.totalCount)
        assertEquals("\n\n", view("\r\n\r").copyText)
    }

    @Test fun levelSelectionIsInclusiveAndUnknownRequiresAllLevels() {
        val text = "[INFO] one\n[ERROR] two\n[WARN] three\nunknown"
        assertEquals(listOf("[INFO] one", "[ERROR] two"),
            view(text, setOf(SUMHLogLevel.INFO, SUMHLogLevel.ERROR)).lines.map { it.text })
        assertEquals(4, view(text).lines.size)
        assertEquals(3, view(text, SUMHLogLevel.entries.toSet()).lines.size)
    }

    @Test fun searchIsCaseInsensitiveLiteralAndPreservesCopiedText() {
        val text = "[INFO] 中文 /a[.]  \n[WARN] /A[.]\n[INFO] /ab\n[ERROR] other"
        val result = view(text, setOf(SUMHLogLevel.INFO), "/A[.]")
        assertEquals("[INFO] 中文 /a[.]  ", result.copyText)
        assertEquals(4, result.totalCount)
        assertTrue(view(text, search = ".*").lines.isEmpty())
        assertEquals("中文😀", view("中文😀", search = "😀").copyText)
    }

    @Test fun unicodeCaseSearchMatchesKotlinIgnoreCaseSemantics() {
        val content = "İıiI\nΣσς\nKelvin K\nStraße"
        val snapshot = parseSUMHLog(content)
        listOf("i", "σ", "k", "ß", "STRASSE", "missing").forEach { query ->
            val expected = snapshot.lines.filter { it.text.contains(query, ignoreCase = true) }
            assertEquals(query, expected, filterSUMHLog(snapshot, emptySet(), query).lines)
        }
    }

    @Test fun duplicateLinesHaveUniqueMonotonicKeysAndFilteringKeepsIdentity() {
        val snapshot = parseSUMHLog("same\nsame\nother\nsame")
        assertEquals(listOf(1L, 2L, 3L, 4L), keys(snapshot))
        assertEquals(5L, snapshot.nextKey)
        assertEquals(listOf(1L, 2L, 4L), filterSUMHLog(snapshot, emptySet(), "same").lines.map { it.key })
        assertSame(snapshot, parseSUMHLog("same\nsame\nother\nsame", snapshot))
    }

    @Test fun appendRetainsExistingRowsWithAndWithoutFinalDelimiter() {
        val plain = parseSUMHLog("a\nb")
        assertEquals(listOf(1L, 2L, 3L), keys(parseSUMHLog("a\nb\nc", plain)))
        val terminated = parseSUMHLog("a\nb\n")
        val appended = parseSUMHLog("a\nb\nc\n", terminated)
        assertEquals(listOf(1L, 2L, 4L, 5L), keys(appended))
        assertEquals(6L, appended.nextKey)
    }

    @Test fun overlappingTailReusesOnlySurvivingRows() {
        val old = parseSUMHLog("a\nb\nc\nd\n")
        val fresh = parseSUMHLog("c\nd\ne\n", old)
        assertEquals(listOf(3L, 4L, 6L, 7L), keys(fresh))
        assertEquals(listOf("c", "d", "e", ""), fresh.lines.map { it.text })
    }

    @Test fun longestDuplicateOverlapRetainsDistinctKeys() {
        val old = parseSUMHLog("start\na\nb\na\nb")
        assertEquals(listOf(2L, 3L, 4L, 5L, 6L), keys(parseSUMHLog("a\nb\na\nb\nnext", old)))
    }

    @Test fun internalBlankRowsSurviveAppendAndTailRotation() {
        val old = parseSUMHLog("a\n\nb\n")
        val fresh = parseSUMHLog("\nb\nc\n", old)
        assertEquals(listOf(2L, 3L, 5L, 6L), keys(fresh))
        assertEquals("\nb\nc\n", filterSUMHLog(fresh, emptySet(), "").copyText)
    }

    @Test fun completeReplacementDoesNotRecycleKeysOrMatchBlankSentinel() {
        val old = parseSUMHLog("a\nb\n")
        assertEquals(listOf(4L, 5L, 6L), keys(parseSUMHLog("\nx\n", old)))
        assertEquals(listOf(4L, 5L), keys(parseSUMHLog("x\ny", old)))
    }

    @Test fun changingUnfinishedLastLineKeepsOnlyUnchangedPrefix() {
        val old = parseSUMHLog("first\npartial")
        val fresh = parseSUMHLog("first\npartial completed\n", old)
        assertEquals(listOf(1L, 3L, 4L), keys(fresh))
    }

    @Test fun emptyRefreshDoesNotResetIdentityCounter() {
        val old = parseSUMHLog("a\nb")
        val empty = parseSUMHLog("", old)
        assertEquals(3L, empty.nextKey)
        assertEquals(listOf(3L), keys(parseSUMHLog("new", empty)))
    }

    @Test fun manyTailRotationsKeepKeysUniqueAndSurvivingAnchor() {
        var snapshot = parseSUMHLog((0..159).joinToString("\n") { "row-$it" })
        for (start in 1..60) {
            snapshot = parseSUMHLog((start..start + 159).joinToString("\n") { "row-$it" }, snapshot)
            assertEquals(160, keys(snapshot).toSet().size)
            assertEquals(61L, snapshot.lines.first { it.text == "row-60" }.key)
        }
    }

    private fun expectCancellation(action: (() -> Unit) -> Unit) {
        val expected = CancellationException("cancel work")
        var calls = 0
        try {
            action { if (++calls == 4) throw expected }
            fail("Expected cooperative cancellation, got $calls checks")
        } catch (actual: CancellationException) { assertSame(expected, actual) }
        assertEquals(4, calls)
    }

    @Test fun longSingleLineParsingIsCooperativelyCancelled() {
        expectCancellation { parseSUMHLog("x".repeat(200000), checkCancelled = it) }
    }

    @Test fun longSearchIsCooperativelyCancelled() {
        val snapshot = parseSUMHLog("a".repeat(200000))
        expectCancellation { filterSUMHLog(snapshot, emptySet(), "aaab", it) }
    }

    @Test fun longSearchPatternConstructionIsCooperativelyCancelled() {
        val snapshot = parseSUMHLog("a")
        expectCancellation { filterSUMHLog(snapshot, emptySet(), "a".repeat(200000), it) }
    }

    @Test fun copyOfLongSingleLineIsCooperativelyCancelled() {
        val snapshot = parseSUMHLog("a".repeat(200000))
        expectCancellation { filterSUMHLog(snapshot, emptySet(), "", it) }
    }

    @Test fun boundedHeaderClassifierDoesNotInspectLongMessageWords() {
        val text = " ".repeat(10000) + "ERROR " + "x".repeat(20000)
        assertNull(parseSUMHLog(text).lines.single().level)
        assertEquals(text, view(text).copyText)
    }

    @Test fun boundedHeaderMustNotManufactureAnEndOfLineDelimiter() {
        val kernel = "<3>KernelSU: sumh:".padStart(256)
        assertNull(parseSUMHLog(kernel + "suffix").lines.single().level)
        assertEquals(SUMHLogLevel.ERROR, parseSUMHLog(kernel + " message").lines.single().level)
        val start = "early-boot +"
        val end = ".000 [INFO]"
        val daemon = start + "1".repeat(96 - start.length - end.length) + end
        assertNull(parseSUMHLog(daemon + "suffix").lines.single().level)
    }
}
