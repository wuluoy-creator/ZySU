package com.zying.zysu.ui.sumh.util

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class SUMHConfigEffectsTest {
    @Test
    fun eachKernelSettingRequiresRuntimeApply() {
        for (key in listOf(
            "enable_kernel_debug", "enable_stealth", "enable_overlay_xattr_hide",
            "enable_mount_hide", "mount_hide_mode", "enable_maps_spoof", "enable_statfs_spoof",
            "enable_kernel_build_spoof", "kernel_build_release", "kernel_build_version",
            "kernel_build_apply_stage", "maps_rules",
        )) {
            val plan = planSUMHConfigSave(setOf(key), kernelAvailable = true)
            assertTrue(plan.effects.runtimeRequired, key)
            assertTrue(plan.applyRuntime, key)
            assertFalse(plan.effects.rebootRequired, key)
        }
    }

    @Test
    fun mountChangesRequireRebootWithoutApplyingKernelFeatures() {
        for (key in listOf(
            "work_dir", "mountsource", "fs_type", "mirror_dir", "mount_backend",
            "mirror_img_size_mb", "overlay_writable", "overlayfs_enabled", "magic_mount_enabled", "partitions",
        )) {
            val plan = planSUMHConfigSave(setOf(key), kernelAvailable = true)
            assertTrue(plan.effects.rebootRequired, key)
            assertFalse(plan.effects.runtimeRequired, key)
            assertFalse(plan.applyRuntime, key)
        }
    }

    @Test
    fun loggingAndEmptyChangesDoNotRequestApplyOrReboot() {
        for (keys in listOf(emptySet(), setOf("debug"), setOf("verbose"), setOf("debug", "verbose"))) {
            val plan = planSUMHConfigSave(keys, kernelAvailable = true)
            assertEquals(SUMHConfigEffects(), plan.effects)
            assertFalse(plan.applyRuntime)
        }
    }

    @Test
    fun unavailableKernelPreservesRuntimeIntentWithoutAttemptingApply() {
        val plan = planSUMHConfigSave(setOf("enable_mount_hide"), kernelAvailable = false)
        assertTrue(plan.effects.runtimeRequired)
        assertFalse(plan.applyRuntime)
    }

    @Test
    fun mixedChangesPreserveBothEffects() {
        val plan = planSUMHConfigSave(setOf("mount_backend", "enable_stealth", "debug"), kernelAvailable = true)
        assertTrue(plan.effects.runtimeRequired)
        assertTrue(plan.effects.rebootRequired)
        assertTrue(plan.applyRuntime)
    }

    @Test
    fun legacyMountHideModeDoesNotTurnUnrelatedSavesIntoRuntimeChanges() {
        for (keys in listOf(emptySet(), setOf("debug"), setOf("mount_backend"), setOf("partitions", "verbose"))) {
            val plan = planSUMHConfigSave(keys, kernelAvailable = true, savedMountHideMode = "aggressive")
            assertFalse(plan.normalizeMountHideMode)
            assertFalse(plan.effects.runtimeRequired)
            assertFalse(plan.applyRuntime)
        }
    }

    @Test
    fun mountHideToggleNormalizesLegacyModeEvenWhenKernelIsUnavailable() {
        for (kernelAvailable in listOf(false, true)) {
            val plan = planSUMHConfigSave(setOf("enable_mount_hide"), kernelAvailable, "aggressive")
            assertTrue(plan.normalizeMountHideMode)
            assertTrue(plan.effects.runtimeRequired)
            assertEquals(kernelAvailable, plan.applyRuntime)
        }
        assertFalse(planSUMHConfigSave(setOf("enable_mount_hide"), true, "normal").normalizeMountHideMode)
    }
}
