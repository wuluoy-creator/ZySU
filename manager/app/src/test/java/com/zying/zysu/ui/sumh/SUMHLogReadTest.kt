package com.zying.zysu.ui.sumh

import android.app.Application
import androidx.activity.ComponentActivity
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.withContext
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.shadows.ShadowLooper

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHLogReadTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    private fun awaitRead(condition: () -> Boolean) = compose.waitUntil(10000) {
        // Reader continuations use Android's Handler, not only Compose's clock.
        ShadowLooper.idleMainLooper()
        condition()
    }

    @Test fun refreshFailureRetainsLastSuccessAndSuccessfulEmptyReadReplacesIt() {
        val refresh = mutableStateOf(0)
        val answers = List(3) { CompletableDeferred<String>() }
        var calls = 0
        var observed = SUMHLogReadState()
        compose.setContent {
            observed = rememberSUMHLogRead(true, false, refresh.value) { answers[calls++].await() }
        }
        awaitRead { calls == 1 }
        compose.runOnIdle { assertTrue(observed.loading); answers[0].complete("first\n") }
        awaitRead { observed.content == "first\n" && !observed.loading }
        compose.runOnIdle { refresh.value++ }
        awaitRead { calls == 2 && observed.loading }
        compose.runOnIdle {
            assertEquals("first\n", observed.content)
            answers[1].completeExceptionally(IllegalStateException("read denied"))
        }
        awaitRead { observed.error == "read denied" }
        compose.runOnIdle {
            assertFalse(observed.loading)
            assertEquals("first\n", observed.content)
            refresh.value++
        }
        awaitRead { calls == 3 && observed.loading }
        compose.runOnIdle { assertNull(observed.error); answers[2].complete("") }
        awaitRead { observed.content.isEmpty() && !observed.loading }
        compose.runOnIdle { assertNull(observed.error); assertEquals(3, calls) }
    }

    @Test fun sourceSwitchAndLeavingScreenRejectNonCooperativeLateRead() {
        val kernel = mutableStateOf(false)
        val active = mutableStateOf(true)
        val old = CompletableDeferred<String>()
        val newest = CompletableDeferred<String>()
        var calls = 0
        var observed = SUMHLogReadState()
        compose.setContent {
            observed = rememberSUMHLogRead(active.value, kernel.value, 0) { source ->
                calls++
                withContext(NonCancellable) { if (source) newest.await() else old.await() }
            }
        }
        awaitRead { calls == 1 }
        compose.runOnIdle { kernel.value = true }
        awaitRead { calls == 2 }
        compose.runOnIdle {
            assertEquals("", observed.content)
            assertTrue(observed.loading)
            newest.complete("kernel only")
        }
        awaitRead { observed.content == "kernel only" && !observed.loading }
        compose.runOnIdle { old.complete("stale daemon") }
        compose.waitForIdle()
        compose.runOnIdle { assertEquals("kernel only", observed.content); active.value = false }
        compose.waitForIdle()
        compose.runOnIdle { assertEquals(2, calls) }
    }

    @Test fun inactiveScreenDoesNotReadAndCancelDoesNotPublishAnError() {
        val active = mutableStateOf(false)
        val blocked = CompletableDeferred<String>()
        var calls = 0
        var observed = SUMHLogReadState()
        compose.setContent {
            observed = rememberSUMHLogRead(active.value, false, 0) { calls++; blocked.await() }
        }
        compose.runOnIdle { assertEquals(0, calls); assertFalse(observed.loading); active.value = true }
        awaitRead { calls == 1 && observed.loading }
        compose.runOnIdle { active.value = false }
        compose.waitForIdle()
        compose.runOnIdle { assertNull(observed.error); assertEquals("", observed.content); assertEquals(1, calls) }
    }
}
