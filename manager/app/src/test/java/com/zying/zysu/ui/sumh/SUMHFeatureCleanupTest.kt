package com.zying.zysu.ui.sumh

import android.app.Application
import com.zying.zysu.ui.sumh.util.SUMHManagedFeature
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter
import com.zying.zysu.ui.sumh.util.sumhFeatureStates
import com.zying.zysu.ui.sumh.util.unsupportedConfiguredFeatures
import org.json.JSONObject
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertSame
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHFeatureCleanupTest {
    @Test
    fun cleanupKeepsSupportedSettingsAndCustomConfiguration() {
        val original = JSONObject().put("future_setting", "keep me")
        val config = SUMHUiAdapter.SUMHPConfig(
            enableMapsSpoof = true,
            enableKernelBuildSpoof = true,
            kernelBuildRelease = "6.1.75-custom",
            kernelBuildVersion = "#1 SMP custom",
            kernelBuildApplyStage = "boot-completed",
            tempdir = "/dev/custom",
            mirrorPath = "/data/custom",
            mountsource = "CUSTOM",
            partitions = listOf("system", "vendor"),
            debug = true,
            magicMountEnabled = false,
            original = original,
        )
        val targets = unsupportedConfiguredFeatures(sumhFeatureStates(
            config.configuredSUMHFeatures(), kernelAvailable = true, capabilities = setOf("mount_hide"),
        ))
        val cleaned = config.disableSUMHFeatures(targets)

        assertEquals(setOf(SUMHManagedFeature.Maps, SUMHManagedFeature.KernelBuild), targets)
        assertFalse(cleaned.enableMapsSpoof)
        assertFalse(cleaned.enableKernelBuildSpoof)
        assertTrue(cleaned.configuredSUMHFeatures().containsAll(
            SUMHManagedFeature.entries.filter { it.alwaysEnabled },
        ))
        assertEquals(config, cleaned.copy(enableMapsSpoof = true, enableKernelBuildSpoof = true))
        assertSame(original, cleaned.original)
        assertEquals("keep me", cleaned.original.getString("future_setting"))
    }
}
