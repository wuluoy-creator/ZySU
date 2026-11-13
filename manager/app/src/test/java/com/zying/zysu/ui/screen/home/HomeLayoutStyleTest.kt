package com.zying.zysu.ui.screen.home

import org.junit.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class HomeLayoutStyleTest {
    @Test fun allThreeStableIdsRoundTripAndLegacyLayoutsMigrate() {
        assertEquals(listOf("circle", "grid", "focus"), HomeLayoutStyle.entries.map { it.id })
        HomeLayoutStyle.entries.forEach { assertEquals(it, HomeLayoutStyle.fromValue(it.id)) }
        assertEquals(HomeLayoutStyle.Grid, HomeLayoutStyle.fromValue("adaptive"))
        assertEquals(HomeLayoutStyle.Grid, HomeLayoutStyle.fromValue("kernelsu"))
        assertEquals(HomeLayoutStyle.Circle, HomeLayoutStyle.fromValue("list"))
        assertEquals(HomeLayoutStyle.Circle, HomeLayoutStyle.fromValue("dashboard_ui"))
        assertEquals(HomeLayoutStyle.Focus, HomeLayoutStyle.fromValue("stats"))
        assertEquals(HomeLayoutStyle.Circle, HomeLayoutStyle.fromValue(null))
        assertEquals(HomeLayoutStyle.Circle, HomeLayoutStyle.fromValue("unsupported"))
    }

    @Test fun longMachineValuesKeepOriginalButHaveCompactSingleLineSummary() {
        val raw = "device/build:16/" + "long-build-part-".repeat(10) + "\nrelease@builder"
        val fact = HomeFact("build", "Build", raw)
        assertEquals(raw, fact.value)
        assertTrue(fact.summary.length <= 48)
        assertTrue(fact.summary.endsWith("…"))
        assertFalse(fact.summary.contains('\n'))
        assertEquals("one two three", compactHomeValue(" one\n two\tthree "))
        assertEquals("6.12.52", homeReleaseLabel("6.12.52-android16-build-hash"))
        assertEquals("v3.6.2", homeReleaseLabel("ZySU v3.6.2-6908-release"))
    }

}
