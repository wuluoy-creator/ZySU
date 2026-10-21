package com.zying.zysu.ui.sumh

import android.app.Application
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import com.zying.zysu.ui.sumh.util.*
import kotlinx.coroutines.CompletableDeferred
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.shadows.ShadowLooper
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHCapabilitiesWorkflowTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    private fun owner() = SUMHCapabilitiesOperations().also { owner ->
        compose.setContent { MaterialTheme { Text("${owner.busy}:${owner.result?.error}") } }
    }
    private fun until(condition: () -> Boolean) {
        compose.waitUntil(10000) { ShadowLooper.idleMainLooper(); condition() }
        compose.waitForIdle()
    }
    private fun idle(owner: SUMHCapabilitiesOperations) = until { !owner.busy }

    @Test fun attachAndReattachDoNotReadOrMutate() {
        val owner = owner()
        compose.runOnIdle { owner.attach("s"); owner.attach("s") }
        assertFalse(owner.busy); assertNull(owner.result)
        var reads = 0
        compose.runOnIdle { assertTrue(owner.read("s") { reads++; SUMHCapabilitiesUiFixtures.result() }) }
        idle(owner)
        compose.runOnIdle { owner.attach("s") }
        assertEquals(1, reads); assertNotNull(owner.result?.snapshot)
    }

    @Test fun singleFlightSurvivesSameSessionReattachment() {
        val owner = owner(); val gate = CompletableDeferred<SUMHCapabilitiesResult>(); var reads = 0
        compose.runOnIdle { owner.attach("s"); owner.read("s") { reads++; gate.await() } }
        until { reads == 1 }
        compose.runOnIdle {
            owner.attach("s")
            assertFalse(owner.read("s") { reads++; SUMHCapabilitiesResult() })
            gate.complete(SUMHCapabilitiesUiFixtures.result())
        }
        idle(owner); assertEquals(1, reads); assertEquals("boot-A", owner.result?.snapshot?.bootId)
    }

    @Test fun closeAndReopenDetachLateResultsWithoutOverlappingRead() {
        val owner = owner(); val gate = CompletableDeferred<SUMHCapabilitiesResult>(); var reads = 0
        compose.runOnIdle { owner.attach("old"); owner.read("old") { reads++; gate.await() } }
        until { reads == 1 }
        compose.runOnIdle {
            owner.close("old"); owner.attach("new")
            assertFalse(owner.read("new") { error("overlapping read") })
            gate.complete(SUMHCapabilitiesUiFixtures.result())
        }
        idle(owner); assertNull(owner.result)
        compose.runOnIdle { owner.read("new") { reads++; SUMHCapabilitiesResult(error = "new_result") } }
        idle(owner); assertEquals(2, reads); assertEquals("new_result", owner.result?.error)
    }

    @Test fun failedRefreshRetainsOldDataAndOriginalProvenanceWithoutRetry() {
        val owner = owner(); var reads = 0
        compose.runOnIdle { owner.attach("s"); owner.read("s") { reads++; SUMHCapabilitiesUiFixtures.result() } }
        idle(owner)
        val previous = requireNotNull(owner.result?.snapshot)
        val gate = CompletableDeferred<SUMHCapabilitiesResult>()
        compose.runOnIdle { owner.read("s") { reads++; gate.await() }; assertEquals(previous, owner.result?.snapshot) }
        until { reads == 2 }
        compose.runOnIdle { gate.complete(SUMHCapabilitiesResult(error = "transport_timeout")) }
        idle(owner)
        val retained = requireNotNull(owner.result?.snapshot)
        assertTrue(retained.saved.retained); assertTrue(retained.runtime.retained)
        assertEquals(previous.runtime.bootId, retained.runtime.bootId)
        assertEquals(previous.runtime.startedMs, retained.runtime.startedMs)
        assertEquals(previous.runtime.value, retained.runtime.value)
        assertEquals("transport_timeout", owner.result?.error); assertEquals(2, reads)
    }

    @Test fun partialNewBootRetainsOnlyFailedSectionWithOldIdentity() {
        val owner = owner()
        compose.runOnIdle { owner.attach("s"); owner.read("s") { SUMHCapabilitiesUiFixtures.result() } }; idle(owner)
        val next = SUMHCapabilitiesUiFixtures.snapshot("boot-B")
        compose.runOnIdle { owner.read("s") {
            SUMHCapabilitiesResult(next.copy(runtime = next.runtime.copy(status = SUMHCapabilityStatus.Error,
                value = null, reason = "read_failed", errno = 5)))
        } }; idle(owner)
        val retained = requireNotNull(owner.result?.snapshot)
        assertEquals("boot-B", retained.saved.bootId); assertFalse(retained.saved.retained)
        assertEquals("boot-A", retained.runtime.bootId); assertTrue(retained.runtime.retained)
        assertEquals("boot-B", retained.runtime.attempt?.bootId)
        assertTrue(retained.features().all { it.comparison == SUMHComparison.Unknown })
    }

    @Test fun exceptionRetainsEvidenceAndClearsBusyWithoutReplay() {
        val owner = owner(); var reads = 0
        compose.runOnIdle { owner.attach("s"); owner.read("s") { reads++; SUMHCapabilitiesUiFixtures.result() } }; idle(owner)
        compose.runOnIdle { owner.read("s") { reads++; error("connection_failed") } }; idle(owner)
        assertEquals("connection_failed", owner.result?.error)
        assertTrue(requireNotNull(owner.result?.snapshot).hasRetained); assertEquals(2, reads)
    }

    @Test fun freshProcessAndOldSessionNeverReplay() {
        val owner = owner()
        compose.runOnIdle { owner.attach("s"); owner.read("s") { SUMHCapabilitiesUiFixtures.result() } }; idle(owner)
        val fresh = SUMHCapabilitiesOperations()
        compose.runOnIdle {
            fresh.attach("restored-small-token")
            assertFalse(fresh.read("s") { error("old session") })
        }
        assertNull(fresh.result); assertFalse(fresh.busy)
    }
}
