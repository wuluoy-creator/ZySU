package com.zying.zysu.ui.sumh

import android.app.Application
import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.viewModelScope
import com.zying.zysu.ui.sumh.util.*
import kotlinx.coroutines.*
import kotlinx.coroutines.test.*
import org.junit.After
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.util.concurrent.CopyOnWriteArrayList
import kotlin.test.*

@OptIn(ExperimentalCoroutinesApi::class)
@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHProfileWorkflowTest {
    @get:Rule val temporary = TemporaryFolder()
    private val dispatcher = UnconfinedTestDispatcher()
    @Before fun setup() { Dispatchers.setMain(dispatcher) }
    @After fun cleanup() { Dispatchers.resetMain() }
    private class Store(directory: File) : SUMHProfileStorage(directory, {}) {
        var hint: SUMHProfilePending? = null; var failCheckpoint = false; var corrupt = false
        var failDraftAfterWrite = false
        override fun saveDraft(value: SUMHProfileDraft, expected: String?): SUMHProfileDraft {
            val saved = super.saveDraft(value, expected)
            if (failDraftAfterWrite) throw IOException("draft sync unconfirmed")
            return saved
        }
        override fun readPending(): SUMHProfilePending? { if (corrupt) throw IOException("future"); return hint }
        override fun comparePending(expected: SUMHProfilePending?, replacement: SUMHProfilePending?): Boolean {
            if (readPending() != expected) return false
            hint = replacement
            if (failCheckpoint) throw IOException("sync")
            return true
        }
    }
    private class Service(private val store: Store) : SUMHProfilesService {
        val calls = CopyOnWriteArrayList<SUMHProfileRequest>(); var failList = false; var mutationGate: CompletableDeferred<Unit>? = null
        var mutateFailure = false; var missing = false
        var staleSelected = false; var bank = ProfileFixtures.bank; var listedRevision: String? = null
        var selectedFailure: String? = null
        override suspend fun capabilities() = SUMHProfileCapabilities(SUMH_PROFILE_ACTIONS, known = true)
        override suspend fun execute(request: SUMHProfileRequest): SUMHProfileResult {
            calls += request
            if (request.action in setOf("show", "preview", "export")) {
                if (selectedFailure == "throw") throw IOException("selected read lost")
                selectedFailure?.let { return SUMHProfileResult(request.action, request.id, "error", it) }
            }
            val document = ProfileFixtures.document(); val revision = SUMHProfilesParser.revision(document)
            if (request.action in SUMH_PROFILE_MUTATIONS) {
                assertEquals(request.id, store.hint?.id); assertEquals("dispatching", store.hint?.phase)
                mutationGate?.await(); if (mutateFailure) throw IOException("response lost")
                return SUMHProfileResult(request.action, request.id, "saved", "inactive_profile_saved", ProfileFixtures.bank, revision,
                    saved = true, durabilityConfirmed = true, readbackConfirmed = true)
            }
            return when (request.action) {
                "list" -> if (failList) SUMHProfileResult("list", status = "error", reason = "bank_unavailable") else SUMHProfileResult("list", status = "complete", reason = "inactive_profile", bankRevision = bank,
                    items = if (missing) emptyList() else listOf(SUMHProfileItem("example", document.name, document.note, listedRevision ?: revision)))
                "show", "export" -> if (staleSelected) SUMHProfileResult(request.action, request.id, "stale", "bank_revision_conflict") else SUMHProfileResult(request.action, request.id, "complete", "inactive_profile", request.bankRevision, request.profileRevision, document = document)
                "import-preview", "preview" -> if (staleSelected) SUMHProfileResult(request.action, request.id, "stale", "bank_revision_conflict") else SUMHProfileResult(request.action, request.id, "complete", "activation_not_implemented", diff = SUMHProfileDiff(emptyList(), "configuration_only"))
                else -> error("unexpected")
            }
        }
    }
    private data class Environment(val operations: SUMHProfileOperations, val service: Service, val store: Store, val directory: File)
    private fun environment(): Environment {
        val directory = File(temporary.root, "profiles-${System.nanoTime()}"); val store = Store(directory); val service = Service(store)
        return Environment(SUMHProfileOperations(SavedStateHandle()), service, store, directory).also { it.operations.load(directory, service, store); await(it.operations) }
    }
    private fun await(operations: SUMHProfileOperations) = runBlocking {
        withTimeout(10_000) { while (operations.busy) delay(5) }
    }
    private fun refresh(env: Environment) { assertTrue(env.operations.refresh()); await(env.operations) }
    @Test fun refreshPreservesFormAndDraftAndFailureKeepsStaleRows() {
        val env = environment(); val op = env.operations
        op.updateForm(SUMHProfileForm("new-profile", "Name", "Note "))
        assertTrue(op.stageImport { ByteArrayInputStream(profileUtf8(ProfileFixtures.document().text)) }); await(op)
        val draft = op.draft; refresh(env); assertEquals(draft, op.draft); assertEquals("Note ", op.form.note)
        env.service.failList = true; refresh(env)
        assertEquals(1, op.items.size); assertEquals("bank_unavailable", op.error)
        op.updateForm(op.form.copy(name = "edit")); assertFalse(op.capture()); assertEquals("profile_refresh_required", op.error)
    }
    @Test fun malformedSelectionPreservesPriorDraftAndResourcesInvalidateReview() {
        val env = environment(); val op = env.operations; refresh(env)
        assertTrue(op.stageImport { ByteArrayInputStream(profileUtf8(ProfileFixtures.document().text)) }); await(op)
        val before = op.draft
        assertTrue(op.stageImport { ByteArrayInputStream(profileUtf8("{bad")) }); await(op); assertEquals(before, op.draft)
        assertTrue(op.previewImport()); await(op); assertNotNull(op.preview)
        assertTrue(op.setResources(setOf(SUMHProfileResource.Maps))); await(op); assertNull(op.preview)
        op.updateForm(SUMHProfileForm("imported", "ignored")); assertFalse(op.importProfile())
        assertEquals("profile_import_preview_required", op.error)
    }
    @Test fun checkpointFailurePreventsDispatchThenCanBeObservedAndAcknowledged() {
        val env = environment(); val op = env.operations; refresh(env)
        op.updateForm(SUMHProfileForm("created", "Name")); env.store.failCheckpoint = true
        assertTrue(op.capture()); await(op)
        assertTrue(env.service.calls.none { it.action == "capture" }); assertNotNull(op.pending)
        env.store.failCheckpoint = false; assertTrue(op.observePending()); await(op)
        assertEquals("missing", op.pendingObservation?.status)
        assertTrue(op.acknowledgePending()); await(op); assertNull(op.pending)
        assertTrue(op.capture()); await(op); assertEquals(1, env.service.calls.count { it.action == "capture" })
    }
    @Test fun lostMutationResponseIsNeverReplayedAfterRecreation() {
        val env = environment(); val op = env.operations; refresh(env); env.service.mutateFailure = true
        op.updateForm(SUMHProfileForm("created", "Name")); assertTrue(op.capture()); await(op)
        assertTrue(op.result!!.unknown); assertNotNull(op.pending)
        val restored = SUMHProfileOperations(SavedStateHandle()); restored.load(env.directory, env.service, env.store); await(restored)
        assertEquals(op.pending, restored.pending); assertEquals(1, env.service.calls.count { it.action == "capture" })
        assertTrue(restored.refresh()); await(restored); assertTrue(restored.observePending()); await(restored)
        assertEquals("missing", restored.pendingObservation?.status); assertFalse(restored.pendingObservation!!.saved)
    }
    @Test fun mutationFinishesAfterOwnerCancellationWithCheckpointRetained() {
        val env = environment(); val op = env.operations; refresh(env)
        val gate = CompletableDeferred<Unit>(); env.service.mutationGate = gate
        op.updateForm(SUMHProfileForm("created", "Name")); assertTrue(op.capture())
        runBlocking { withTimeout(10_000) { while (env.service.calls.none { it.action == "capture" }) delay(5) } }
        op.viewModelScope.cancel(); gate.complete(Unit); await(op)
        assertNotNull(env.store.hint); assertEquals(1, env.service.calls.count { it.action == "capture" })
    }
    @Test fun corruptCheckpointBlocksAllWritesWithoutPretendingEmpty() {
        val env = environment(); env.store.corrupt = true
        val restored = SUMHProfileOperations(SavedStateHandle()); restored.load(env.directory, env.service, env.store); await(restored)
        assertTrue(restored.refresh()); await(restored); restored.updateForm(SUMHProfileForm("new", "Name"))
        assertFalse(restored.capture()); assertEquals("profile_storage_unavailable", restored.error)
        assertTrue(env.service.calls.none { it.action in SUMH_PROFILE_MUTATIONS })
    }
    @Test fun frozenExportSurvivesRecreationAndProviderFailure() {
        val env = environment(); val op = env.operations; refresh(env)
        assertTrue(op.inspect(op.items.single())); await(op); assertTrue(op.prepareExport()); await(op)
        val frozen = requireNotNull(op.exportArtifact)
        val restored = SUMHProfileOperations(SavedStateHandle()); restored.load(env.directory, env.service, env.store); await(restored)
        assertNull(restored.detail); assertEquals("idle", restored.exportStatus); assertEquals(frozen, restored.exportArtifact)
        val backendCalls = env.service.calls.size
        assertTrue(restored.retryExport()); await(restored); assertEquals("ready", restored.exportStatus); assertEquals(backendCalls, env.service.calls.size)
        assertTrue(restored.writeExport(frozen.token) { throw IOException("provider") }); await(restored)
        assertEquals("failed", restored.exportStatus); assertEquals(frozen, restored.exportArtifact)
        val bytes = ByteArrayOutputStream(); assertTrue(restored.writeExport(frozen.token) { bytes }); await(restored)
        assertEquals("saved", restored.exportStatus); assertContentEquals(profileUtf8(frozen.document), bytes.toByteArray())
    }
    @Test fun wrongExportTokenNeverOpensDestination() {
        val env = environment(); val op = env.operations; refresh(env); op.inspect(op.items.single()); await(op); op.prepareExport(); await(op)
        var opened = false
        assertTrue(op.writeExport("0".repeat(32)) { opened = true; ByteArrayOutputStream() }); await(op)
        assertFalse(opened); assertEquals("failed", op.exportStatus)
    }
    @Test fun staleSelectedQueriesCannotRegainFreshnessThroughLocalDraftEdits() {
        for (action in listOf("show", "preview", "export")) {
            val env = environment(); val op = env.operations; refresh(env)
            op.inspect(op.items.single()); await(op); env.service.staleSelected = true
            assertTrue(when (action) { "show" -> op.inspect(op.items.single()); "preview" -> op.previewSelected(); else -> op.prepareExport() }); await(op)
            assertFalse(op.listFresh, action)
            assertTrue(op.setResources(setOf(SUMHProfileResource.Maps))); await(op)
            op.updateForm(SUMHProfileForm("new", "Name")); assertFalse(op.capture()); assertFalse(op.listFresh, action)
            assertTrue(env.service.calls.none { it.action in SUMH_PROFILE_MUTATIONS })
        }
    }
    @Test fun refreshedBankOrSelectedRevisionInvalidatesReviewedDetail() {
        for (change in listOf("bank", "revision", "missing")) {
            val env = environment(); val op = env.operations; refresh(env); op.inspect(op.items.single()); await(op)
            assertNotNull(op.detail)
            when (change) {
                "bank" -> env.service.bank = "sha256-v1:" + "b".repeat(64)
                "revision" -> env.service.listedRevision = "sha256-v1:" + "c".repeat(64)
                else -> env.service.missing = true
            }
            refresh(env); assertTrue(op.listFresh); assertNull(op.detail, change); assertNull(op.preview)
            op.updateForm(SUMHProfileForm("new", "Name")); assertFalse(op.cloneSelected(), change)
        }
    }
    @Test fun selectedKnownErrorAndThrownFailureRequireNewListAfterLocalEdit() {
        for (action in listOf("show", "preview", "export")) for (failure in listOf("bank_unavailable", "profile_not_found", "throw")) {
            val env = environment(); val op = env.operations; refresh(env); op.inspect(op.items.single()); await(op)
            env.service.selectedFailure = failure
            assertTrue(when (action) { "show" -> op.inspect(op.items.single()); "preview" -> op.previewSelected(); else -> op.prepareExport() }); await(op)
            assertFalse(op.listFresh, "$action/$failure")
            assertTrue(op.setResources(setOf(SUMHProfileResource.Maps))); await(op)
            op.updateForm(SUMHProfileForm("created", "Name")); assertFalse(op.capture())
            assertTrue(env.service.calls.none { it.action in SUMH_PROFILE_MUTATIONS })
            env.service.selectedFailure = null; refresh(env); assertTrue(op.capture()); await(op)
            assertEquals(1, env.service.calls.count { it.action == "capture" })
        }
    }
    @Test fun draftPostRenameFailureHasExplicitReloadAndNextEdit() {
        val env = environment(); val op = env.operations
        op.stageImport { ByteArrayInputStream(profileUtf8(ProfileFixtures.document().text)) }; await(op)
        val original = op.draft
        val changed = ProfileFixtures.profile().apply { addProperty("name", "New draft") }.toString()
        env.store.failDraftAfterWrite = true
        assertTrue(op.stageImport { ByteArrayInputStream(profileUtf8(changed)) }); await(op)
        assertTrue(op.draftNeedsReload); assertEquals(original, op.draft)
        env.store.failDraftAfterWrite = false
        assertTrue(op.reloadDraft()); await(op)
        assertFalse(op.draftNeedsReload); assertEquals("New draft", op.importedDocument?.name)
        assertTrue(op.setResources(setOf(SUMHProfileResource.Maps))); await(op)
        assertEquals(setOf(SUMHProfileResource.Maps), op.draft.resources)
        assertTrue(env.service.calls.isEmpty())
    }
}
