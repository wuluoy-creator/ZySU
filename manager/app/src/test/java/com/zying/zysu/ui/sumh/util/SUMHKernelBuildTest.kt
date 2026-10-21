package com.zying.zysu.ui.sumh.util

import kotlin.test.Test
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import kotlin.test.assertEquals
import kotlin.test.assertNull

class SUMHKernelBuildTest {
    private val runtime = KernelBuildState(true, "6.6.1-android15-spoof", "#2 SMP",
        "6.1.75-android14-11-original", "#1 SMP", "#1 SMP Fri Aug 23 03:08:10 UTC 2024", "ro.build.date")

    @Test
    fun systemDateSuggestionRequiresKnownSourceAndValidUtf8Length() {
        assertEquals(runtime.suggestedVersion, runtime.systemDateSuggestion)
        assertNull(runtime.copy(suggestedVersion = "").systemDateSuggestion)
        assertNull(runtime.copy(suggestionSource = "future.source").systemDateSuggestion)
        assertNull(runtime.copy(suggestedVersion = "界".repeat(22)).systemDateSuggestion)
        assertNull(runtime.copy(suggestedVersion = "#1 SMP\n2026").systemDateSuggestion)
    }

    @Test
    fun kmiLabelUsesOnlyOriginalReleaseAndDoesNotInventUnknownValues() {
        assertEquals("android14-6.1", runtime.originalKmiLabel)
        assertEquals("android14-6.1", runtime.copy(release = "6.12.1-android16-custom").originalKmiLabel)
        assertEquals("6.1", runtime.copy(originalRelease = "6.1.75-custom").originalKmiLabel)
        assertNull(runtime.copy(originalRelease = "custom-build").originalKmiLabel)
    }

    @Test
    fun eitherFieldCanKeepItsOriginalValue() {
        assertTrue(validKernelBuildOverride(true, "6.1.75-custom", ""))
        assertTrue(validKernelBuildOverride(true, "", "#1 SMP PREEMPT Fri Aug 23 03:08:10 UTC 2024"))
        assertTrue(validKernelBuildOverride(true, "6.1.75-custom", "#2 SMP"))
        assertFalse(validKernelBuildOverride(true, "", ""))
        assertTrue(validKernelBuildOverride(false, "", ""))
    }

    @Test
    fun limitCountsUtf8Bytes() {
        assertTrue(validKernelBuildField("a".repeat(64)))
        assertFalse(validKernelBuildField("a".repeat(65)))
        assertTrue(validKernelBuildField("构".repeat(21) + "a"))
        assertFalse(validKernelBuildField("构".repeat(21) + "ab"))
        assertFalse(validKernelBuildOverride(false, "a".repeat(65), ""))
    }

    @Test
    fun controlCharactersCannotBeStored() {
        for (control in ('\u0000'..'\u001f') + '\u007f') {
            assertFalse(validKernelBuildField("6.1${control}75", release = true))
            assertFalse(validKernelBuildField("#1 SMP${control}PREEMPT"))
        }
    }

    @Test
    fun spacesAreOnlyAllowedInBuildVersion() {
        assertFalse(validKernelBuildField("6.1.75 custom", release = true))
        assertFalse(validKernelBuildField(" 6.1.75", release = true))
        assertTrue(validKernelBuildField("#1 SMP PREEMPT Fri Aug 23 03:08:10 UTC 2024"))
    }

    @Test
    fun validUtf8UsesTheSameBytePolicyAsTheDaemon() {
        assertTrue(validKernelBuildField("6.1.75-构建", release = true))
        assertTrue(validKernelBuildField("6.1.75\u0085\u00a0", release = true))
    }
}
