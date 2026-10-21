package com.zying.zysu.ui.sumh

import android.app.Application
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.viewModelScope
import com.zying.zysu.ui.sumh.util.*
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.shadows.ShadowLooper
import java.io.File
import java.io.IOException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHDiagnosticPackageWorkflowTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    @get:Rule val temporary = TemporaryFolder()
    private class Checkpoint : SUMHDiagnosticPackageCheckpoint {
        var value: SUMHDiagnosticPackagePending? = null; var failure = false; var writes = 0
        override fun read() = value
        override fun compareAndSet(expected: SUMHDiagnosticPackagePending?, pending: SUMHDiagnosticPackagePending?): Boolean {
            if (value != expected) return false
            writes++; value = pending
            if (failure) throw IOException("CANARY_CHECKPOINT")
            return true
        }
    }
    private class Service : SUMHDiagnosticPackagesService {
        var calls = mutableListOf<String>()
        var caps = SUMHDiagnosticPackageCapabilities(SUMHDiagnosticPackageSupport.Supported, SUMHDiagnosticPackageSupport.Supported,
            SUMHDiagnosticPackageSupport.Supported, SUMHDiagnosticPackageSupport.Supported, SUMHDiagnosticPackageSupport.Supported)
        var checkpoint: SUMHDiagnosticPackageCheckpoint? = null
        var gate: CompletableDeferred<Unit>? = null
        var stored = DiagnosticPackageFixtures.preview()
        var missing = false
        var createFailure = false
        override suspend fun capabilities(): SUMHDiagnosticPackageCapabilities { calls += "capabilities"; return caps }
        override suspend fun list(): SUMHDiagnosticPackageResult { calls += "list"; return SUMHDiagnosticPackageResult("list", status = "observed", exitCode = 0, items = listOf(stored.item)) }
        override suspend fun create(id: String, draft: SUMHDiagnosticPackageDraft): SUMHDiagnosticPackageResult {
            calls += "create:$id"; assertEquals(id, checkpoint?.read()?.id); gate?.await()
            if (createFailure) throw IOException("CANARY_CREATE")
            return SUMHDiagnosticPackageResult("create", id, "saved", "package_saved", 0, stored.item.revision,
                if (draft.includeIdentifiers) "identifiers_included" else "redacted", "partial", saved = true, durabilityConfirmed = true, readbackConfirmed = true)
        }
        override suspend fun preview(id: String): SUMHDiagnosticPackageResult {
            calls += "preview:$id"
            return if (missing) SUMHDiagnosticPackageResult("preview", id, "not_found", "package_missing", 3)
            else SUMHDiagnosticPackageResult("preview", id, "observed", "package_observed", 0, stored.item.revision,
                stored.item.privacyPolicy, stored.item.diagnosticStatus, preview = stored.copy(item = stored.item.copy(id = id)))
        }
        override suspend fun export(id: String, revision: String): SUMHDiagnosticPackageResult {
            calls += "export:$id"; return SUMHDiagnosticPackageResult("export", id, "observed", "package_observed", 0,
                stored.item.revision, stored.item.privacyPolicy, stored.item.diagnosticStatus, preview = stored)
        }
        override suspend fun remove(id: String, revision: String): SUMHDiagnosticPackageResult {
            calls += "remove:$id"; assertEquals(SUMHDiagnosticPackagePending("remove", id, revision), checkpoint?.read())
            return SUMHDiagnosticPackageResult("remove", id, "deleted", "package_deleted", 0, revision,
                stored.item.privacyPolicy, stored.item.diagnosticStatus, deleted = true, durabilityConfirmed = true, readbackConfirmed = true)
        }
    }
    private fun owner(handle: SavedStateHandle = SavedStateHandle()) = SUMHDiagnosticPackageOperations(handle).also { owner ->
        compose.setContent { MaterialTheme { Text("${owner.busy}:${owner.loaded}:${owner.result?.status}") } }
    }
    private fun until(condition: () -> Boolean) {
        compose.waitUntil(10000) { ShadowLooper.idleMainLooper(); condition() }; compose.waitForIdle()
    }
    private fun idle(owner: SUMHDiagnosticPackageOperations) = until { !owner.busy }
    private fun load(owner: SUMHDiagnosticPackageOperations, service: Service, checkpoint: SUMHDiagnosticPackageCheckpoint, refresh: Boolean = true) {
        service.checkpoint = checkpoint
        compose.runOnIdle { owner.load(File("unused-test-private"), service, checkpoint) }; idle(owner)
        if (refresh) { compose.runOnIdle { assertTrue(owner.refresh()) }; idle(owner) }
    }
    @Test fun loadAndProcessRecreationOnlyRestoreCheckpointNeverQueryOrReplay() {
        val owner = owner(); val service = Service(); val checkpoint = Checkpoint()
        checkpoint.value = SUMHDiagnosticPackagePending("create", DiagnosticPackageFixtures.id, null)
        load(owner, service, checkpoint, false)
        assertEquals(checkpoint.value, owner.pending); assertTrue(service.calls.isEmpty()); assertNull(owner.preview)
        compose.runOnIdle { assertFalse(owner.create()); assertFalse(owner.recoverPending()) }
        compose.runOnIdle { owner.refresh() }; idle(owner)
        compose.runOnIdle { assertTrue(owner.recoverPending()) }; idle(owner)
        assertEquals(listOf("capabilities", "list", "preview:${checkpoint.value!!.id}"), service.calls)
        assertNotNull(owner.pendingObservation)
    }
    @Test fun createPersistsIdentityBeforeDispatchAndIsSingleFlightAcrossBackgroundCompletion() {
        val owner = owner(); val service = Service(); val checkpoint = Checkpoint(); load(owner, service, checkpoint)
        service.gate = CompletableDeferred()
        val draft = SUMHDiagnosticPackageDraft(SUMHDiagnosticScenario.ModuleFallback, "module.one", includeIdentifiers = true)
        compose.runOnIdle { owner.updateDraft(draft); assertTrue(owner.create()) }
        until { service.calls.any { it.startsWith("create:") } }
        compose.runOnIdle { assertTrue(owner.busy); assertFalse(owner.create()); assertFalse(owner.refresh()); owner.updateDraft(SUMHDiagnosticPackageDraft()) }
        assertEquals(draft, owner.draft); assertNotNull(owner.pending)
        compose.runOnIdle { service.gate!!.complete(Unit) }; idle(owner)
        assertEquals("saved", owner.result?.status); assertEquals(draft, owner.draft); assertNotNull(owner.pending)
        assertEquals(1, service.calls.count { it.startsWith("create:") })
    }
    @Test fun unconfirmedCheckpointNeverDispatchesAndRetainsKnownId() {
        val owner = owner(); val service = Service(); val checkpoint = Checkpoint(); load(owner, service, checkpoint)
        checkpoint.failure = true
        compose.runOnIdle { assertTrue(owner.create()) }; idle(owner)
        assertFalse(service.calls.any { it.startsWith("create:") }); assertNotNull(owner.pending)
        assertEquals("diagnostic_checkpoint_unconfirmed", owner.error)
        assertFalse(owner.error.orEmpty().contains("CANARY"))
    }
    @Test fun mutationFailureKeepsDraftAndMissingRecoveryRequiresExplicitAcknowledgement() {
        val owner = owner(); val service = Service(); val checkpoint = Checkpoint(); load(owner, service, checkpoint)
        service.createFailure = true
        val draft = SUMHDiagnosticPackageDraft(SUMHDiagnosticScenario.UserHide, DiagnosticPackageFixtures.id)
        compose.runOnIdle { owner.updateDraft(draft); owner.create() }; idle(owner)
        assertEquals(draft, owner.draft); assertTrue(owner.result!!.unknown); assertNotNull(owner.pending)
        compose.runOnIdle { assertFalse(owner.acknowledgePending()) }
        service.missing = true
        compose.runOnIdle { owner.recoverPending() }; idle(owner)
        assertEquals("not_found", owner.pendingObservation?.status); assertNotNull(owner.pending)
        compose.runOnIdle { assertTrue(owner.acknowledgePending()) }; idle(owner)
        assertNull(owner.pending); assertNull(checkpoint.value)
        assertEquals(1, service.calls.count { it.startsWith("create:") })
    }
    @Test fun removalIsBoundToCurrentFullPreviewAndDurableSelectedRevision() {
        val owner = owner(); val service = Service(); val checkpoint = Checkpoint(); load(owner, service, checkpoint)
        compose.runOnIdle { assertFalse(owner.remove(service.stored)); assertTrue(owner.inspect(service.stored.item.id)) }; idle(owner)
        val reviewed = requireNotNull(owner.preview)
        compose.runOnIdle { assertFalse(owner.remove(reviewed.copy(exportText = "tampered"))); assertTrue(owner.remove(reviewed)) }; idle(owner)
        assertTrue(owner.result!!.deleted); assertNull(owner.preview)
        assertEquals(reviewed.item.revision, owner.pending!!.revision)
    }
    @Test fun exportRequiresReviewedIdentityAndExactBytesAndPreservesSourceOnMismatch() {
        val owner = owner(); val service = Service(); val checkpoint = Checkpoint(); load(owner, service, checkpoint)
        compose.runOnIdle { owner.inspect(service.stored.item.id) }; idle(owner)
        val reviewed = requireNotNull(owner.preview)
        val success = runBlocking { owner.exportReviewed(reviewed) }
        assertEquals(reviewed, success.preview); assertEquals(reviewed, owner.preview)
        service.stored = service.stored.copy(exportText = service.stored.exportText + " ")
        val failure = runBlocking { owner.exportReviewed(reviewed) }
        assertEquals("diagnostic_export_changed", failure.error); assertNull(failure.preview); assertEquals(reviewed, owner.preview)
        assertNull(owner.pending); assertEquals(0, checkpoint.writes)
    }
    @Test fun smallDraftRestoresAcrossProcessRecreationButNeverPreviewOrApproval() {
        val handle = SavedStateHandle(); val owner = owner(handle)
        val typed = SUMHDiagnosticPackageDraft(SUMHDiagnosticScenario.ModuleFallback, "example.module", DiagnosticPackageFixtures.other, true)
        compose.runOnIdle { owner.updateDraft(typed) }
        val recreated = SUMHDiagnosticPackageOperations(handle)
        assertEquals(typed, recreated.draft); assertNull(recreated.preview); assertNull(recreated.result); assertFalse(recreated.loaded)
        assertEquals(setOf("diagnostic_package_scenario", "diagnostic_package_target", "diagnostic_package_operation", "diagnostic_package_include_identifiers"), handle.keys())
        assertFalse(recreated.create()); assertFalse(recreated.remove(DiagnosticPackageFixtures.preview()))
    }
    @Test fun invalidRestoredFieldsFailClosedAndRetainNoReviewPermission() {
        val handle = SavedStateHandle(mapOf("diagnostic_package_scenario" to "future", "diagnostic_package_target" to "CANARY",
            "diagnostic_package_operation" to "", "diagnostic_package_include_identifiers" to true))
        val owner = owner(handle)
        assertEquals(SUMHDiagnosticPackageDraft(), owner.draft); assertEquals("diagnostic_draft_restore_invalid", owner.error)
        assertFalse(owner.create()); assertNull(owner.preview)
        compose.runOnIdle { owner.updateDraft(SUMHDiagnosticPackageDraft(targetId = "x".repeat(4097))) }
        assertEquals(SUMHDiagnosticPackageDraft(), owner.draft); assertEquals("diagnostic_draft_capacity", owner.error)
    }
    @Test fun staleEmptyOwnerNeverQueuesOrDispatchesOverAnotherOwnersClaim() {
        val directory = File(temporary.root, "diagnostic-packages")
        val firstFile = SUMHDiagnosticPackageFile(directory) { }
        val secondFile = SUMHDiagnosticPackageFile(directory) { }
        val first = owner(); val second = SUMHDiagnosticPackageOperations()
        val firstService = Service(); val secondService = Service()
        load(first, firstService, firstFile); load(second, secondService, secondFile)
        firstService.gate = CompletableDeferred()
        try {
            compose.runOnIdle { assertTrue(first.create()) }
            until { firstService.calls.any { it.startsWith("create:") } }
            compose.runOnIdle { assertFalse(second.create()) }
            assertEquals("diagnostic_operation_busy", second.error)
            assertFalse(secondService.calls.any { it.startsWith("create:") })
        } finally { compose.runOnIdle { firstService.gate!!.complete(Unit) } }
        idle(first)
        // Refusal was not queued; a subsequent explicit action finds the durable
        // claim and must adopt it without dispatching a second package mutation.
        assertFalse(secondService.calls.any { it.startsWith("create:") })
        compose.runOnIdle { assertTrue(second.create()) }; idle(second)
        assertEquals(firstFile.read(), second.pending); assertNull(second.pendingObservation)
        assertEquals("diagnostic_checkpoint_conflict", second.error)
        assertFalse(secondService.calls.any { it.startsWith("create:") })
    }
    @Test fun preDispatchClaimBlocksStaleRecoveryAndAcknowledgementAcrossOwners() {
        val directory = File(temporary.root, "diagnostic-packages")
        val storage = SUMHDiagnosticPackageFile(directory) { }
        val old = SUMHDiagnosticPackagePending("create", DiagnosticPackageFixtures.id, null)
        assertTrue(storage.compareAndSet(null, old))
        val recorded = CountDownLatch(1); val release = CountDownLatch(1)
        val paused = object : SUMHDiagnosticPackageCheckpoint by storage {
            override fun compareAndSet(expected: SUMHDiagnosticPackagePending?, pending: SUMHDiagnosticPackagePending?): Boolean {
                val changed = storage.compareAndSet(expected, pending)
                if (changed && expected == null && pending != null) {
                    recorded.countDown(); check(release.await(10, TimeUnit.SECONDS))
                }
                return changed
            }
        }
        val first = owner(); val second = SUMHDiagnosticPackageOperations()
        val firstService = Service(); val secondService = Service().also { it.missing = true }
        load(first, firstService, paused); load(second, secondService, SUMHDiagnosticPackageFile(directory) { })
        compose.runOnIdle { assertTrue(second.recoverPending()) }; idle(second)
        assertEquals("not_found", second.pendingObservation?.status)
        compose.runOnIdle { assertTrue(first.recoverPending()) }; idle(first)
        compose.runOnIdle { assertTrue(first.acknowledgePending()) }; idle(first)
        assertNull(storage.read())
        val callsBefore = secondService.calls.toList()
        try {
            compose.runOnIdle { assertTrue(first.create()) }
            until { recorded.count == 0L }
            assertNotEquals(old, storage.read())
            assertFalse(firstService.calls.any { it.startsWith("create:") })
            compose.runOnIdle {
                assertFalse(second.recoverPending())
                assertFalse(second.acknowledgePending())
            }
            assertEquals(callsBefore, secondService.calls)
            assertNotNull(storage.read())
        } finally { release.countDown() }
        idle(first)
        assertEquals(1, firstService.calls.count { it.startsWith("create:") })
        // The previously reviewed missing result cannot clear a newer identity.
        compose.runOnIdle { assertTrue(second.acknowledgePending()) }; idle(second)
        assertEquals(storage.read(), second.pending); assertNotEquals(old, second.pending)
        assertEquals("diagnostic_checkpoint_conflict", second.error); assertNull(second.pendingObservation)
        assertFalse(secondService.calls.any { it.startsWith("create:") })
    }
    @Test fun cancelledOwnerCannotClaimAndDoesNotLeakTheSharedPermit() {
        val first = owner(); val second = SUMHDiagnosticPackageOperations()
        val firstService = Service(); val secondService = Service(); val checkpoint = Checkpoint()
        load(first, firstService, checkpoint); load(second, secondService, checkpoint)
        compose.runOnIdle {
            first.viewModelScope.cancel()
            // Admission may accept a task, but its cancelled scope must stop
            // before any checkpoint write or native call and release the permit.
            assertTrue(first.create())
        }
        idle(first)
        assertEquals("diagnostic_operation_cancelled", first.error)
        assertEquals(0, checkpoint.writes); assertNull(checkpoint.value); assertNull(first.pending)
        assertFalse(firstService.calls.any { it.startsWith("create:") })
        compose.runOnIdle { assertTrue(second.create()) }; idle(second)
        assertEquals(1, checkpoint.writes); assertEquals(checkpoint.value, second.pending)
        assertEquals(1, secondService.calls.count { it.startsWith("create:") })
    }
    @Test fun cancellationDuringNativeCallRetainsHintAndPermitUntilManualRecoveryIsSafe() {
        val directory = File(temporary.root, "diagnostic-packages")
        val storage = SUMHDiagnosticPackageFile(directory) { }
        val first = owner(); val second = SUMHDiagnosticPackageOperations()
        val firstService = Service(); val secondService = Service()
        load(first, firstService, storage)
        firstService.gate = CompletableDeferred()
        try {
            compose.runOnIdle { assertTrue(first.create()) }
            until { firstService.calls.any { it.startsWith("create:") } }
            val recorded = requireNotNull(storage.read())
            load(second, secondService, SUMHDiagnosticPackageFile(directory) { })
            assertEquals(recorded, second.pending)
            compose.runOnIdle {
                first.viewModelScope.cancel()
                assertTrue(first.busy)
                assertFalse(second.recoverPending())
                assertFalse(second.acknowledgePending())
            }
            assertEquals("diagnostic_operation_busy", second.error)
            assertEquals(recorded, storage.read()); assertEquals(recorded, first.pending)
            assertFalse(secondService.calls.any { it.startsWith("preview:") || it.startsWith("create:") })
        } finally { compose.runOnIdle { firstService.gate!!.complete(Unit) } }
        idle(first)
        val recorded = requireNotNull(storage.read())
        assertEquals(recorded, first.pending); assertEquals(recorded, second.pending)
        assertEquals(1, firstService.calls.count { it.startsWith("create:") })
        compose.runOnIdle { assertTrue(second.recoverPending()) }; idle(second)
        assertEquals("observed", second.pendingObservation?.status)
        assertEquals(recorded, storage.read())
        compose.runOnIdle { assertTrue(second.acknowledgePending()) }; idle(second)
        assertNull(storage.read()); assertNull(second.pending)
        assertEquals(listOf("capabilities", "list", "preview:${recorded.id}"), secondService.calls)
        assertEquals(1, firstService.calls.count { it.startsWith("create:") })
    }
}
