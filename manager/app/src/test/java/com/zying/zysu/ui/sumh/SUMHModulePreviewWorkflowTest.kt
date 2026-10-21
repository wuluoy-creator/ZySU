package com.zying.zysu.ui.sumh

import android.app.Application
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import com.zying.zysu.ui.sumh.util.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
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
class SUMHModulePreviewWorkflowTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    private fun context(path: String = "", epoch: Long = 1, allowed: Boolean = true): SUMHModulePreviewContext {
        val rules = listOf(SUMHManager.ModuleRule("/", "none"), SUMHManager.ModuleRule("/vendor/etc/中文 ", "sumh"))
        val draft = ModuleEditorDraft("sumh", rules, baseline = ModuleEditorDraft.baseline("auto", emptyList()))
        return SUMHModulePreviewContext(draft.record(), epoch, draft.baseline, "auto", true, "sumh", 0, path, allowed)
    }
    private fun owner(): SUMHModulePreviewOperations = SUMHModulePreviewOperations().also { owner ->
        compose.setContent { MaterialTheme { Text("${owner.busy}:${owner.stale}:${owner.result?.error}") } }
    }
    private fun until(condition: () -> Boolean) {
        compose.waitUntil(10000) { ShadowLooper.idleMainLooper(); condition() }
        compose.waitForIdle()
    }
    private fun idle(owner: SUMHModulePreviewOperations) = until { !owner.busy }

    @Test fun openingAndReattachingNeverReadAndExplicitInputContainsWholeDraft() {
        val owner = owner(); val input = context("/vendor/etc/中文 "); var calls = 0
        compose.runOnIdle { owner.attach("session", "example", input); owner.attach("session", "example", input) }
        assertNull(owner.request); assertNull(owner.result); assertFalse(owner.busy)
        compose.runOnIdle {
            assertTrue(owner.read("session", input, true) { request ->
                calls++; assertEquals("sumh", request.candidateMode)
                assertEquals(ModuleEditorDraft.from(input.draft).rules, request.candidateRules)
                assertEquals(input.explainPath, request.explainPath)
                assertFalse(requireNotNull(request.candidateJson).contains("\"config\""))
                SUMHModulePreviewResult(error = "sample_unavailable")
            })
        }
        idle(owner); assertEquals(1, calls); assertFalse(owner.stale); assertEquals("sample_unavailable", owner.result?.error)
    }

    @Test fun conflictedOrInvalidDraftStillAllowsSavedOnlyWithoutMutation() {
        val owner = owner(); val input = context(allowed = false).let {
            it.copy(draft = ModuleEditorDraft.from(it.draft).copy(mode = "future", rules = listOf(SUMHManager.ModuleRule("relative", "bad"))).record())
        }
        var calls = 0
        compose.runOnIdle {
            owner.attach("s", "example", input)
            assertFalse(owner.read("s", input, true) { error("candidate must be blocked") })
            assertTrue(owner.read("s", input, false) {
                calls++; assertNull(it.candidateJson); assertNull(it.candidateRules)
                SUMHModulePreviewResult(error = "saved_observation")
            })
        }
        idle(owner); assertEquals(1, calls); assertEquals("saved_observation", owner.result?.error)
    }

    @Test fun editThenRevertDuringReadCannotReviveOldResult() {
        val owner = owner(); val input = context(); val gate = CompletableDeferred<SUMHModulePreviewResult>(); var calls = 0
        compose.runOnIdle { owner.attach("s", "example", input); owner.read("s", input, true) { calls++; gate.await() } }
        until { calls == 1 }
        compose.runOnIdle {
            owner.observe("s", input.copy(globalMode = "magic")); owner.observe("s", input)
            gate.complete(SUMHModulePreviewResult(error = "old_observation"))
        }
        idle(owner); assertTrue(owner.stale); assertEquals("old_observation", owner.result?.error); assertEquals(1, calls)
    }

    @Test fun draftEpochDetectsSameBytesAfterEditAndRevert() {
        val owner = owner(); val input = context()
        compose.runOnIdle { owner.attach("s", "example", input); owner.read("s", input, false) { SUMHModulePreviewResult() } }
        idle(owner)
        compose.runOnIdle { owner.observe("s", input.copy(draftEpoch = input.draftEpoch + 2)) }
        assertTrue(owner.stale)
    }

    @Test fun savedBaselineCapabilityAndManagerRevisionChangesInvalidate() {
        val owner = owner(); var current = context(); var calls = 0
        compose.runOnIdle { owner.attach("s", "example", current) }
        val changes: List<(SUMHModulePreviewContext) -> SUMHModulePreviewContext> = listOf(
            { it.copy(savedBaseline = "b".repeat(64)) }, { it.copy(backendAvailable = false) },
            { it.copy(managerRevision = 2) }, { it.copy(explainPath = "/another") }, { it.copy(candidateAllowed = false) },
        )
        changes.forEach { change ->
            compose.runOnIdle { owner.read("s", current, false) { calls++; SUMHModulePreviewResult() } }; idle(owner)
            assertFalse(owner.stale)
            compose.runOnIdle { current = change(current); owner.observe("s", current) }
            assertTrue(owner.stale)
        }
        assertEquals(changes.size, calls)
    }

    @Test fun closeReopenRejectsLateResultAndDoesNotOverlapNativeCall() {
        val owner = owner(); val input = context(); val gate = CompletableDeferred<SUMHModulePreviewResult>(); var calls = 0
        compose.runOnIdle { owner.attach("old", "first", input); owner.read("old", input, true) { calls++; gate.await() } }
        until { calls == 1 }
        compose.runOnIdle {
            owner.close("old"); owner.attach("new", "second", input)
            assertTrue(owner.busy); assertNull(owner.result); assertNull(owner.request)
            assertFalse(owner.read("new", input, false) { calls++; SUMHModulePreviewResult() })
            gate.complete(SUMHModulePreviewResult(error = "must_not_publish"))
        }
        idle(owner); assertNull(owner.result); assertNull(owner.request); assertEquals(1, calls)
        compose.runOnIdle { assertTrue(owner.read("new", input, false) { calls++; SUMHModulePreviewResult(error = "second") }) }
        idle(owner); assertEquals(2, calls); assertEquals("second", owner.result?.error)
    }

    @Test fun sameSessionReattachmentRetainsInFlightReadWithoutAutomaticReplay() {
        val owner = owner(); val input = context(); val gate = CompletableDeferred<SUMHModulePreviewResult>(); var calls = 0
        compose.runOnIdle { owner.attach("s", "example", input); owner.read("s", input, false) { calls++; gate.await() } }
        until { calls == 1 }
        compose.runOnIdle { owner.attach("s", "example", input); gate.complete(SUMHModulePreviewResult(error = "one_read")) }
        idle(owner); assertFalse(owner.stale); assertEquals(1, calls); assertEquals("one_read", owner.result?.error)
        compose.runOnIdle { owner.attach("s", "example", input) }; assertEquals("one_read", owner.result?.error)
    }

    @Test fun freshProcessOwnerHasNoResultAndNeverRestartsRead() {
        val owner = owner(); val input = context(); var calls = 0
        compose.runOnIdle { owner.attach("s", "example", input); owner.read("s", input, false) { calls++; SUMHModulePreviewResult(error = "old") } }
        idle(owner)
        val fresh = SUMHModulePreviewOperations()
        compose.runOnIdle { fresh.attach("s", "example", input) }
        assertFalse(fresh.busy); assertNull(fresh.result); assertNull(fresh.request); assertEquals(1, calls)
    }

    @Test fun invalidLiteralInputNeverCallsReaderAndKeepsOriginalDraft() {
        val owner = owner(); val input = context("/bad/../path "); var calls = 0
        compose.runOnIdle { owner.attach("s", "example", input); owner.read("s", input, true) { calls++; SUMHModulePreviewResult() } }
        idle(owner); assertEquals(0, calls); assertNotNull(owner.result?.error)
        assertEquals("/bad/../path ", input.explainPath)
        assertEquals("/vendor/etc/中文 ", ModuleEditorDraft.from(input.draft).rules.last().path)
    }

    @Test fun exceptionClearsOldEvidenceAndDoesNotRetry() {
        val owner = owner(); val input = context(); var calls = 0
        compose.runOnIdle { owner.attach("s", "example", input); owner.read("s", input, false) { calls++; SUMHModulePreviewResult(error = "old") } }
        idle(owner); assertEquals("old", owner.result?.error)
        val failureGate = CompletableDeferred<Unit>()
        compose.runOnIdle {
            assertTrue(owner.read("s", input, false) {
                calls++; failureGate.await(); throw IllegalStateException("transport_failed")
            })
        }
        until { calls == 2 }
        compose.runOnIdle {
            assertTrue(owner.busy)
            assertNull(owner.result)
            failureGate.complete(Unit)
        }
        idle(owner); assertEquals(2, calls); assertEquals("transport_failed", owner.result?.error); assertFalse(owner.stale)
    }
}
