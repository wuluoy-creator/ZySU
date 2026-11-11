package com.zying.zysu.ui.viewmodel

import java.util.Locale
import org.junit.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class ModuleListPresentationTest {
    private fun module(id: String, name: String = id, enabled: Boolean = true, removed: Boolean = false,
        update: Boolean = false, meta: Boolean = false, action: Boolean = false) = ModuleViewModel.ModuleInfo(
        id = id, name = name, author = "", version = "1", versionCode = 1, description = "", enabled = enabled,
        update = update, remove = removed, updateJson = "", hasWebUi = false, hasActionScript = action,
        metamodule = meta, dirId = id,
    )

    @Test fun removedModulesArePendingButNeverEnabledOrDisabled() {
        val removed = module("removed", removed = true)
        assertTrue(ModuleListFilter.Pending.matches(removed))
        assertFalse(ModuleListFilter.Enabled.matches(removed))
        assertFalse(ModuleListFilter.Disabled.matches(removed))
        assertTrue(ModuleListFilter.All.matches(removed))
        assertTrue(ModuleListFilter.Pending.matches(module("staged", update = true)))
        assertFalse(ModuleListFilter.Pending.matches(module("disabled", enabled = false)))
    }

    @Test fun descendingNamesKeepEnabledAndMetaPriorities() {
        val modules = listOf(module("disabled", enabled = false), module("alpha"), module("zeta"), module("meta", meta = true))
        assertEquals(listOf("meta", "zeta", "alpha", "disabled"),
            modules.sortedWith(moduleListComparator(true, false, false, true, Locale.US)).map { it.id })
    }

    @Test fun displayNamesFallBackToIdAndTiesUseDirectoryIdentity() {
        val modules = listOf(module("z", "Alpha"), module("b", ""), module("a", "Alpha"))
        assertEquals(listOf("a", "z", "b"),
            modules.sortedWith(moduleListComparator(false, false, true, false, Locale.US)).map { it.id })
        assertEquals(listOf("a", "b", "z"),
            modules.sortedWith(moduleListComparator(false, false, false, false, Locale.US)).map { it.id })
    }

    @Test fun actionPriorityWorksWithinEnabledGroups() {
        val modules = listOf(module("a"), module("z", action = true), module("d", enabled = false, action = true))
        assertEquals(listOf("z", "a", "d"),
            modules.sortedWith(moduleListComparator(true, true, false, false, Locale.US)).map { it.id })
    }
}
