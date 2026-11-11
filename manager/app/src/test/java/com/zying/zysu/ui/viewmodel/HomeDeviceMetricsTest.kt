package com.zying.zysu.ui.viewmodel

import org.junit.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull

class HomeDeviceMetricsTest {
    @Test fun capacityRejectsUnavailableAndInvalidSources() {
        assertNull(HomeCapacity.fromAvailable(0L, 0L))
        assertNull(HomeCapacity.fromAvailable(100L, -1L))
        assertNull(HomeCapacity.fromAvailable(100L, 101L))
        assertEquals(0f, HomeCapacity.fromAvailable(100L, 100L)?.fraction)
        assertEquals(1f, HomeCapacity.fromAvailable(100L, 0L)?.fraction)
        assertEquals(40L, HomeCapacity.fromAvailable(100L, 60L)?.usedBytes)
    }

    @Test fun capacityDoesNotOverflowForLargeDevices() {
        val capacity = HomeCapacity.fromAvailable(Long.MAX_VALUE, Long.MAX_VALUE / 2)!!
        assertEquals(0.5f, capacity.fraction)
    }

    @Test fun batteryPercentageHandlesNonstandardScalesAndInvalidReadings() {
        assertEquals(50, batteryPercentage(127, 254))
        assertEquals(100, batteryPercentage(Int.MAX_VALUE, Int.MAX_VALUE))
        assertNull(batteryPercentage(-1, 100))
        assertNull(batteryPercentage(101, 100))
        assertNull(batteryPercentage(1, 0))
    }

    @Test fun thermalReadingsRejectMalformedAndNonfiniteValues() {
        assertEquals(42.5f, thermalMilliDegrees("42500\n"))
        assertEquals(-5f, thermalMilliDegrees("-5000"))
        listOf(null, "", "unknown", "NaN", "Infinity", "-Infinity", "999999").forEach {
            assertNull(thermalMilliDegrees(it))
        }
    }
}
