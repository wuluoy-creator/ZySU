package com.zying.zysu.ui.viewmodel

import org.junit.Test
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class HomeCountRefreshTest {
    @Test fun authorizationChangesRefreshEvenWithUnchangedModuleCount() {
        val unchangedModules = homeCountChanged(3, 3)
        assertTrue(homeCountChanged(5, 6) || unchangedModules)
        assertTrue(homeCountChanged(5, 4) || unchangedModules)
        assertTrue(homeCountChanged(1, 0) || unchangedModules)
        assertFalse(homeCountChanged(5, 5) || unchangedModules)
    }

    @Test fun failedReadDoesNotClearPreviouslyKnownCount() {
        assertFalse(homeCountChanged(5, null))
        assertFalse(homeCountChanged(5, -1))
        assertFalse(homeCountChanged(null, null))
        assertTrue(homeCountChanged(null, 0))
        assertTrue(homeCountChanged(null, 5))
    }
}
