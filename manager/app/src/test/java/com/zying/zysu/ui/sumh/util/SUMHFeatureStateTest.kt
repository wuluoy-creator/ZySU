package com.zying.zysu.ui.sumh.util

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class SUMHFeatureStateTest {
    @Test
    fun absentCapabilitySnapshotDoesNotClaimUnsupported() {
        val states = sumhFeatureStates(SUMHManagedFeature.entries.toSet(), true, null)
        assertEquals(SUMHFeatureSupport.Supported, states.first().support)
        assertTrue(states.drop(1).all { it.support == SUMHFeatureSupport.Unknown })
        assertTrue(unsupportedConfiguredFeatures(states).isEmpty())
    }

    @Test
    fun unavailableKernelAllowsClearingSavedSettingsButNeverBulkCleanup() {
        val states = sumhFeatureStates(setOf(SUMHManagedFeature.Maps), false, emptySet())
        assertTrue(states.all { it.support == SUMHFeatureSupport.KernelUnavailable })
        assertTrue(states.single { it.feature == SUMHManagedFeature.Maps }.canChange)
        assertFalse(states.single { it.feature == SUMHManagedFeature.Stealth }.canChange)
        assertTrue(unsupportedConfiguredFeatures(states).isEmpty())
    }

    @Test
    fun cleanupContainsOnlyConfiguredUnsupportedFeatures() {
        val states = sumhFeatureStates(
            setOf(SUMHManagedFeature.Stealth, SUMHManagedFeature.MountHide, SUMHManagedFeature.Maps),
            true,
            setOf("mount_hide"),
        )
        assertEquals(setOf(SUMHManagedFeature.Maps), unsupportedConfiguredFeatures(states))
        assertFalse(states.single { it.feature == SUMHManagedFeature.Statfs }.canChange)
    }

    @Test
    fun searchCombinesLocalizedLabelsAndStableCapabilitiesWithSelectedFilter() {
        val state = SUMHFeatureState(SUMHManagedFeature.MountHide, true, SUMHFeatureSupport.Supported)
        assertTrue(state.matches(SUMHFeatureFilter.All, " 挂载 ", listOf("挂载隐藏", "隐藏模块条目")))
        assertTrue(state.matches(SUMHFeatureFilter.ConfiguredOn, "MOUNT_HIDE", emptyList()))
        assertTrue(state.matches(SUMHFeatureFilter.Supported, "条目", listOf("隐藏模块条目")))
        assertFalse(state.matches(SUMHFeatureFilter.Unsupported, "mount_hide", emptyList()))
        assertFalse(state.matches(SUMHFeatureFilter.All, "missing", listOf("挂载隐藏")))
    }

    @Test
    fun unknownAndUnavailableAreExcludedFromUnsupportedFilter() {
        for (support in listOf(SUMHFeatureSupport.Unknown, SUMHFeatureSupport.KernelUnavailable)) {
            val state = SUMHFeatureState(SUMHManagedFeature.Maps, true, support)
            assertFalse(state.matches(SUMHFeatureFilter.Unsupported, "", emptyList()))
            assertTrue(state.matches(SUMHFeatureFilter.ConfiguredOn, "", emptyList()))
        }
    }

    @Test
    fun runtimeStateUsesOnlyCompleteRuleSnapshotAndDoesNotGuessOverlayXattrs() {
        val configured = setOf(SUMHManagedFeature.MountHide, SUMHManagedFeature.OverlayXattr)
        val known = sumhFeatureStates(configured, true,
            setOf("mount_hide", "overlay_xattr_hide"), setOf("mount_hide"))
        assertTrue(known.single { it.feature == SUMHManagedFeature.MountHide }.runtimeEnabled == true)
        assertFalse(known.single { it.feature == SUMHManagedFeature.Maps }.runtimeEnabled ?: true)
        assertEquals(null, known.single { it.feature == SUMHManagedFeature.OverlayXattr }.runtimeEnabled)
        val unknown = sumhFeatureStates(configured, true, emptySet())
        assertTrue(unknown.all { it.runtimeEnabled == null })
    }
}
