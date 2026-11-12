package com.zying.zysu.ui.screen.home

import com.zying.zysu.ui.viewmodel.HomeCapacity
import com.zying.zysu.ui.viewmodel.HomeDeviceMetrics
import com.zying.zysu.ui.viewmodel.appendHomeMetricSample
import org.junit.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

class HomeLayoutStyleTest {
    @Test fun allFiveStableIdsRoundTripAndLegacyLayoutsMigrate() {
        assertEquals(listOf("circle", "grid", "focus", "dashboard_ui", "stats"), HomeLayoutStyle.entries.map { it.id })
        HomeLayoutStyle.entries.forEach { assertEquals(it, HomeLayoutStyle.fromValue(it.id)) }
        assertEquals(HomeLayoutStyle.Grid, HomeLayoutStyle.fromValue("adaptive"))
        assertEquals(HomeLayoutStyle.Grid, HomeLayoutStyle.fromValue("kernelsu"))
        assertEquals(HomeLayoutStyle.Circle, HomeLayoutStyle.fromValue("list"))
        assertEquals(HomeLayoutStyle.Circle, HomeLayoutStyle.fromValue(null))
        assertEquals(HomeLayoutStyle.Circle, HomeLayoutStyle.fromValue("unsupported"))
    }

    @Test fun onlyLayoutsWithReadingsKeepMetricCollectionActive() {
        assertEquals(setOf(HomeLayoutStyle.Focus, HomeLayoutStyle.Stats), HomeLayoutStyle.entries.filter { it.usesMetrics }.toSet())
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

    @Test fun metricHistoryIsBoundedAndMissingReadingsRemainGaps() {
        var previous: HomeDeviceMetrics? = null
        repeat(35) { index ->
            previous = appendHomeMetricSample(previous, HomeDeviceMetrics(
                cpuTemperatureC = if (index == 33) null else index.toFloat(),
                memory = if (index == 33) null else HomeCapacity.fromAvailable(100, 50),
            ))
        }
        val history = previous!!
        assertEquals(30, history.cpuTemperatureHistory.size)
        assertEquals(5f, history.cpuTemperatureHistory.first())
        assertNull(history.cpuTemperatureHistory[28])
        assertNull(history.memoryHistory[28])
        assertEquals(34f, history.cpuTemperatureHistory.last())
        assertEquals(.5f, history.memoryHistory.last())
    }
}
