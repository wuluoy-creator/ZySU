package com.zying.zysu.ui.viewmodel

import android.app.Application
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.assertEquals
import kotlin.test.assertFails

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class ModuleListQueryTest {
    @Test fun onlySuccessfulEmptyArrayMeansNoInstalledModules() {
        assertEquals(0, parseModuleListResponse(true, " [] \n").length())
        assertFails { parseModuleListResponse(false, "[]") }
        assertFails { parseModuleListResponse(false, "") }
        assertFails { parseModuleListResponse(true, " \n") }
    }

    @Test fun malformedOrDiagnosticOutputCannotPublishAnEmptySnapshot() {
        listOf("{}", "null", "[", "[] error", "permission denied").forEach { output ->
            assertFails(output) { parseModuleListResponse(true, output) }
        }
    }

    @Test fun successfulRecordsArePreservedForModelValidation() {
        val records = parseModuleListResponse(true, "[{\"id\":\"test\",\"enabled\":false}]")
        assertEquals("test", records.getJSONObject(0).getString("id"))
        assertEquals(false, records.getJSONObject(0).getBoolean("enabled"))
    }
}
