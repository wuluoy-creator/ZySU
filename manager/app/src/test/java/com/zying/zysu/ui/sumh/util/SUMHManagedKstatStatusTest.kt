package com.zying.zysu.ui.sumh.util

import android.app.Application
import org.json.JSONObject
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHManagedKstatStatusTest {
    @Test
    fun parsesRuntimeCountAndLosslessGeneration() {
        val status = JSONObject().put("active_count", 3)
            .put("generation", "18446744073709551615")
        assertEquals(SUMHUiAdapter.ManagedKstatStatus(3, "18446744073709551615"),
            SUMHUiAdapter.parseManagedKstatStatus(status))
    }

    @Test
    fun rejectsMissingOrMalformedRuntimeStatus() {
        assertNull(SUMHUiAdapter.parseManagedKstatStatus(null))
        for (value in listOf(
            JSONObject().put("active_count", -1).put("generation", "1"),
            JSONObject().put("active_count", 33).put("generation", "1"),
            JSONObject().put("active_count", "2").put("generation", "1"),
            JSONObject().put("active_count", 1.5).put("generation", "1"),
            JSONObject().put("active_count", 2).put("generation", 1),
            JSONObject().put("active_count", 2).put("generation", "01"),
            JSONObject().put("active_count", 2).put("generation", "18446744073709551616"),
            JSONObject().put("active_count", 2).put("generation", "1").put("extra", true),
        )) assertNull(SUMHUiAdapter.parseManagedKstatStatus(value))
    }
}
