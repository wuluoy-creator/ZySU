package com.zying.zysu.ui.sumh.util

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

class SUMHKernelBuildTest {
    @Test
    fun savingFieldsDeterminesWhetherAnOverrideIsNeeded() {
        val releaseOnly = kernelBuildOverride(" 6.1.75-custom ", " ")!!
        assertTrue(releaseOnly.enabled)
        assertEquals("6.1.75-custom", releaseOnly.release)
        assertEquals("", releaseOnly.version)

        val versionOnly = kernelBuildOverride("", " #1 SMP PREEMPT ")!!
        assertTrue(versionOnly.enabled)
        assertEquals("#1 SMP PREEMPT", versionOnly.version)

        val original = kernelBuildOverride(" ", " ")!!
        assertFalse(original.enabled)
        assertEquals("", original.release)
        assertEquals("", original.version)
    }

    @Test
    fun savingRejectsInvalidKernelFields() {
        assertNull(kernelBuildOverride("6.1.75 custom", "#1 SMP"))
        assertNull(kernelBuildOverride("6.1.75", "#1\u0000 SMP"))
        assertNull(kernelBuildOverride("构".repeat(22), ""))
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
