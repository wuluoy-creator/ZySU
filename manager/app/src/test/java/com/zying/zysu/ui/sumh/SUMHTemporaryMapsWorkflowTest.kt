package com.zying.zysu.ui.sumh

import android.app.Application
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.lifecycle.SavedStateHandle
import com.zying.zysu.ui.sumh.util.*
import kotlinx.coroutines.CancellationException
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
class SUMHTemporaryMapsWorkflowTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    private fun owner(handle: SavedStateHandle = SavedStateHandle()) = SUMHTemporaryMapsOperations(handle).also { owner ->
        compose.setContent { MaterialTheme { Text("${owner.busy}:${owner.fresh}:${owner.result?.status}") } }
    }
    private fun until(condition: () -> Boolean) {
        compose.waitUntil(10000) { ShadowLooper.idleMainLooper(); condition() }
        compose.waitForIdle()
    }
    private fun idle(owner: SUMHTemporaryMapsOperations) = until { !owner.busy }
    private fun load(owner: SUMHTemporaryMapsOperations, service: SUMHTemporaryMapsUiFixtures.Service) {
        compose.runOnIdle { assertTrue(owner.refresh(service, service.tracking)) }; idle(owner)
    }

    @Test fun openingAndParentObservationNeverQuery() {
        val owner = owner()
        compose.runOnIdle { owner.observeParent(true); owner.observeParent(true) }
        assertNull(owner.snapshot); assertFalse(owner.busy); assertFalse(owner.canSelect); assertFalse(owner.queryRequired)
    }

    @Test fun refreshReadsCapabilitiesThenOnePageAndNextIsExplicit() {
        val owner = owner(); val service = SUMHTemporaryMapsUiFixtures.Service()
        service.pageReader = { cursor -> SUMHTemporaryMapsUiFixtures.page(cursor = cursor?.cursor?.toInt() ?: 0,
            end = cursor != null, ino = if (cursor == null) "0" else "18446744073709551615") }
        load(owner, service)
        assertEquals(1, service.reads); assertEquals(listOf<SUMHTemporaryMapsCursor?>(null), service.pages)
        assertFalse(owner.snapshot!!.complete); assertEquals(1, owner.snapshot!!.rows.size)
        assertTrue(owner.canSelect); assertTrue(owner.canNext)
        compose.runOnIdle { assertTrue(owner.next(service)) }; idle(owner)
        assertEquals(1, service.reads); assertEquals("1", service.pages.last()!!.cursor)
        assertEquals(owner.snapshot!!.token, service.pages.last()!!.token)
        assertEquals(2, owner.snapshot!!.rows.size); assertTrue(owner.snapshot!!.complete); assertFalse(owner.canNext)
    }

    @Test fun independentDeleteSupportKeepsReadAvailable() {
        val owner = owner(); val service = SUMHTemporaryMapsUiFixtures.Service()
        service.support = service.support.copy(delete = SUMHTemporaryMapsSupport.Unsupported)
        load(owner, service)
        assertTrue(owner.fresh); assertNotNull(owner.snapshot); assertFalse(owner.canSelect)
        compose.runOnIdle { assertFalse(owner.select(owner.snapshot!!.rows.single())); assertFalse(owner.remove(service.tracking)) }
        assertTrue(service.removes.isEmpty())
    }

    @Test fun missingOrUnknownTrackingKeepsInventoryReadableWithoutLegacyFallback() {
        val owner = owner(); val service = SUMHTemporaryMapsUiFixtures.Service()
        for (support in listOf(SUMHTemporaryMapsSupport.Unsupported, SUMHTemporaryMapsSupport.Unknown)) {
            service.tracking.support = support; load(owner, service)
            assertTrue(owner.fresh); assertNotNull(owner.snapshot); assertEquals(support, owner.trackingSupport)
            compose.runOnIdle { assertFalse(owner.select(owner.snapshot!!.rows.single())); assertFalse(owner.remove(service.tracking)) }
            assertTrue(service.removes.isEmpty()); assertEquals(0, service.legacyRemoves)
        }
        assertEquals(2, service.tracking.capabilityReads); assertEquals(2, service.pages.size)
    }

    @Test fun localHistoryAndManualRecoveryNeverRefreshOrDeleteCurrentInventory() {
        val owner = owner(); val service = SUMHTemporaryMapsUiFixtures.Service()
        val f = SUMHTemporaryMapsTrackingFixtures; service.tracking.records = listOf(f.entry("unknown"))
        compose.runOnIdle { assertTrue(owner.loadHistory(service.tracking)) }; idle(owner)
        assertEquals(1, service.tracking.loads); assertEquals(0, service.tracking.capabilityReads)
        assertTrue(service.pages.isEmpty()); assertTrue(service.removes.isEmpty()); assertFalse(owner.fresh)
        service.tracking.queryHandler = { _, _ -> service.tracking.records = listOf(f.recovered("historical")); true }
        compose.runOnIdle { assertTrue(owner.recover(f.id, tracking = service.tracking)) }; idle(owner)
        assertEquals(listOf(f.id to 0), service.tracking.queries); assertFalse(owner.fresh); assertNull(owner.snapshot)
        assertEquals(SUMHTemporaryMapsRemoveStatus.Deleted, owner.history.records.single().recovery()!!.resource!!.status)
        compose.runOnIdle { assertTrue(owner.recover(f.id, 5000, service.tracking)) }; idle(owner)
        assertEquals(f.id to 5000, service.tracking.queries.last())
        compose.runOnIdle { assertTrue(owner.forget(f.id, service.tracking)) }; idle(owner)
        assertTrue(owner.history.records.isEmpty()); assertEquals(listOf(f.id), service.tracking.forgotten)
        assertTrue(service.removes.isEmpty()); assertEquals(0, service.legacyRemoves); assertTrue(service.pages.isEmpty())
    }

    @Test fun unsupportedOrUnknownInventoryNeverQueriesPages() {
        val owner = owner(); val service = SUMHTemporaryMapsUiFixtures.Service()
        for (support in listOf(SUMHTemporaryMapsSupport.Unsupported, SUMHTemporaryMapsSupport.Unknown)) {
            service.support = service.support.copy(inventory = support)
            load(owner, service)
            assertTrue(service.pages.isEmpty()); assertFalse(owner.fresh); assertTrue(owner.queryRequired)
        }
        assertEquals(2, service.reads)
    }

    @Test fun refreshFailureKeepsRowsHistoricalAndCannotDelete() {
        val owner = owner(); val service = SUMHTemporaryMapsUiFixtures.Service(); load(owner, service)
        val previous = owner.snapshot
        service.pageReader = { throw IllegalStateException("disconnected") }
        load(owner, service)
        assertSame(previous, owner.snapshot); assertFalse(owner.fresh); assertFalse(owner.canSelect)
        assertEquals(SUMHTemporaryMapsProblem.Failed, owner.problem); assertTrue(owner.queryRequired)
        assertEquals(2, service.pages.size)
    }

    @Test fun staleOrMixedContinuationRetainsPreviousRowsWithoutRestart() {
        val owner = owner(); val service = SUMHTemporaryMapsUiFixtures.Service()
        service.pageReader = { SUMHTemporaryMapsUiFixtures.page(end = false) }; load(owner, service)
        val previous = owner.snapshot
        service.pageReader = { SUMHTemporaryMapsUiFixtures.failedPage() }
        compose.runOnIdle { assertTrue(owner.next(service)) }; idle(owner)
        assertSame(previous, owner.snapshot); assertEquals(SUMHTemporaryMapsProblem.Stale, owner.problem)
        assertFalse(owner.canNext); assertFalse(owner.canSelect); assertEquals(2, service.pages.size)
        service.pageReader = { SUMHTemporaryMapsUiFixtures.page(end = false) }; load(owner, service)
        service.pageReader = { SUMHTemporaryMapsUiFixtures.page(cursor = 1, ino = "7").let {
            it.copy(page = it.page!!.copy(generation = "10"))
        } }
        compose.runOnIdle { assertTrue(owner.next(service)) }; idle(owner)
        assertEquals(1, owner.snapshot!!.rows.size); assertFalse(owner.fresh)
        assertEquals(4, service.pages.size)
    }

    @Test fun deleteDoubleClickIsSingleShotAndEveryResultRequiresExplicitRefresh() {
        val owner = owner(); val service = SUMHTemporaryMapsUiFixtures.Service()
        val outcomes = listOf(
            SUMHTemporaryMapsRemoveResult(SUMHTemporaryMapsRemoveStatus.Deleted, "deleted", 0, true, true),
            SUMHTemporaryMapsRemoveResult(SUMHTemporaryMapsRemoveStatus.NotApplied, "snapshot_changed", 116, true),
            SUMHTemporaryMapsRemoveResult(SUMHTemporaryMapsRemoveStatus.Unknown, "outcome_unknown", 5, true),
        )
        for (outcome in outcomes) {
            load(owner, service)
            val row = owner.snapshot!!.rows.single(); val token = owner.snapshot!!.token
            val gate = CompletableDeferred<SUMHTemporaryMapsRemoveResult>()
            service.remover = { assertEquals(SUMHTemporaryMapsRemoveRequest(token, row), it); gate.await() }
            val before = service.removes.size
            compose.runOnIdle {
                assertTrue(owner.select(row)); assertTrue(owner.remove(service.tracking)); assertFalse(owner.remove(service.tracking))
                assertFalse(owner.refresh(service, service.tracking)); assertFalse(owner.next(service)); assertFalse(owner.fresh)
            }
            until { service.removes.size == before + 1 }
            compose.runOnIdle { gate.complete(outcome) }; idle(owner)
            assertEquals(row, owner.selected); assertEquals(outcome, owner.result)
            assertEquals("01".repeat(16), owner.operationId); assertEquals(0, service.legacyRemoves)
            assertTrue(owner.queryRequired); assertFalse(owner.canRemove); assertFalse(owner.canSelect)
            assertEquals(before + 1, service.removes.size)
        }
    }

    @Test fun thrownOrCancelledRemovalIsUnknownAndNeverRetried() {
        val owner = owner(); val service = SUMHTemporaryMapsUiFixtures.Service()
        for (failure in listOf(IllegalStateException("transport_failed"), CancellationException("cancelled"))) {
            load(owner, service)
            service.remover = { throw failure }
            compose.runOnIdle { assertTrue(owner.select(owner.snapshot!!.rows.single())); assertTrue(owner.remove(service.tracking)) }
            idle(owner)
            assertEquals(SUMHTemporaryMapsRemoveStatus.Unknown, owner.result?.status)
            assertFalse(owner.fresh); assertTrue(owner.queryRequired)
        }
        assertEquals(2, service.removes.size)
    }

    @Test fun parentChangesInvalidateInFlightReadEvenAfterReenable() {
        val owner = owner(); val service = SUMHTemporaryMapsUiFixtures.Service(); load(owner, service)
        val previous = owner.snapshot
        val gate = CompletableDeferred<SUMHTemporaryMapsPageResult>(); service.pageReader = { gate.await() }
        compose.runOnIdle { owner.refresh(service, service.tracking) }; until { service.pages.size == 2 }
        compose.runOnIdle { owner.observeParent(false); owner.observeParent(true); gate.complete(SUMHTemporaryMapsUiFixtures.page(ino = "7")) }
        idle(owner)
        assertSame(previous, owner.snapshot); assertFalse(owner.fresh); assertFalse(owner.canSelect)
        assertEquals(SUMHTemporaryMapsProblem.Changed, owner.problem)
    }

    @Test fun dismissedSelectionCannotBeResurrectedByLateReceipt() {
        val owner = owner(); val service = SUMHTemporaryMapsUiFixtures.Service(); load(owner, service)
        val gate = CompletableDeferred<SUMHTemporaryMapsRemoveResult>(); service.remover = { gate.await() }
        compose.runOnIdle { owner.select(owner.snapshot!!.rows.single()); owner.remove(service.tracking) }
        until { service.removes.size == 1 }
        compose.runOnIdle { owner.dismissSelection(); assertTrue(owner.busy)
            gate.complete(SUMHTemporaryMapsRemoveResult(SUMHTemporaryMapsRemoveStatus.Deleted, "deleted", 0, true, true)) }
        idle(owner); assertNull(owner.selected); assertNull(owner.result); assertTrue(owner.queryRequired)
        assertNull(owner.operationId)
    }

    @Test fun processRecoveryStoresOnlySmallMarkersAndNeverReplaysMutation() {
        val handle = SavedStateHandle(); val owner = owner(handle)
        val service = SUMHTemporaryMapsUiFixtures.Service(); load(owner, service)
        val gate = CompletableDeferred<SUMHTemporaryMapsRemoveResult>(); service.remover = { gate.await() }
        compose.runOnIdle { owner.select(owner.snapshot!!.rows.single()); owner.remove(service.tracking) }
        until { service.removes.size == 1 }
        val recoveredHandle = SavedStateHandle(handle.keys().associateWith { key -> handle.get<Any>(key) })
        val restored = SUMHTemporaryMapsOperations(recoveredHandle)
        assertEquals(setOf(SUMHTemporaryMapsOperations.QUERY_REQUIRED, SUMHTemporaryMapsOperations.PENDING), handle.keys())
        assertTrue(restored.queryRequired); assertFalse(restored.busy); assertNull(restored.snapshot)
        assertNull(restored.selected); assertNull(restored.result); assertFalse(restored.canRemove)
        assertEquals(1, service.removes.size)
        compose.runOnIdle { gate.complete(SUMHTemporaryMapsRemoveResult(SUMHTemporaryMapsRemoveStatus.Unknown, "outcome_unknown")) }
        idle(owner)
    }
}
