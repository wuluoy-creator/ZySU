package com.zying.zysu.ui.util

import org.junit.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertNotSame
import kotlin.test.assertSame
import kotlin.test.assertTrue

class IdleResourcePoolTest {
    private class Resource {
        var alive = true
        var closes = 0
    }

    private class Timer(val delay: Long, val action: () -> Unit) {
        var cancelled = false
        // Also invoke cancelled timers: cancellation can race with a dispatched callback.
        fun fire() = action()
    }

    private class Fixture {
        val resources = mutableListOf<Resource>()
        val timers = mutableListOf<Timer>()
        val pool = IdleResourcePool(
            idleMillis = 30_000,
            create = { Resource().also(resources::add) },
            isAlive = { resource: Resource -> resource.alive },
            dispose = { resource -> resource.closes++ },
            schedule = { delay, action ->
                val timer = Timer(delay, action).also(timers::add)
                val cancel: () -> Unit = { timer.cancelled = true }
                cancel
            },
        )
    }

    @Test fun initializationAndAuthenticationRefreshDoNotCreateShells() {
        val fixture = Fixture()
        fixture.pool.invalidate()
        assertTrue(fixture.resources.isEmpty())
        assertTrue(fixture.timers.isEmpty())
    }

    @Test fun idleShellClosesOnceAndNextRequestCreatesAnother() {
        val fixture = Fixture()
        val first = fixture.pool.acquire()
        first.close()
        val timer = fixture.timers.single()
        assertEquals(30_000L, timer.delay)
        assertEquals(0, first.value.closes)
        timer.fire()
        timer.fire()
        assertEquals(1, first.value.closes)
        fixture.pool.acquire().use { second -> assertNotSame(first.value, second.value) }
    }

    @Test fun cancelledTimerCannotCloseReacquiredOrRecentlyUsedShell() {
        val fixture = Fixture()
        val first = fixture.pool.acquire()
        first.close()
        val stale = fixture.timers.single()
        val second = fixture.pool.acquire()
        assertSame(first.value, second.value)
        assertTrue(stale.cancelled)
        stale.fire()
        assertEquals(0, first.value.closes)
        second.close()
        stale.fire()
        assertEquals(0, first.value.closes)
        fixture.timers.last().fire()
        assertEquals(1, first.value.closes)
    }

    @Test fun overlappingJobsAndStreamsKeepShellUntilLastLeaseReturns() {
        val fixture = Fixture()
        val job = fixture.pool.acquire()
        val stream = fixture.pool.acquire()
        assertSame(job.value, stream.value)
        job.close()
        job.close()
        assertTrue(fixture.timers.isEmpty())
        assertEquals(0, stream.value.closes)
        stream.close()
        assertEquals(1, fixture.timers.size)
        fixture.timers.single().fire()
        assertEquals(1, stream.value.closes)
    }

    @Test fun refreshReplacesShellWithoutInterruptingExistingLaunchTask() {
        val fixture = Fixture()
        val launch = fixture.pool.acquire()
        val queuedJob = fixture.pool.acquire()
        fixture.pool.invalidate()
        assertEquals(0, launch.value.closes)
        val fresh = fixture.pool.acquire()
        assertNotSame(launch.value, fresh.value)
        launch.close()
        assertEquals(0, queuedJob.value.closes)
        queuedJob.close()
        assertEquals(1, launch.value.closes)
        assertEquals(0, fresh.value.closes)
        fresh.close()
        fixture.timers.single().fire()
        assertEquals(1, fresh.value.closes)
    }

    @Test fun refreshingIdleShellClosesItAndInvalidatesOldTimer() {
        val fixture = Fixture()
        val lease = fixture.pool.acquire()
        lease.close()
        fixture.pool.invalidate()
        fixture.pool.invalidate()
        fixture.timers.single().fire()
        assertEquals(1, lease.value.closes)
        assertEquals(1, fixture.resources.size)
    }

    @Test fun failedOperationStillReturnsItsLease() {
        val fixture = Fixture()
        assertFailsWith<IllegalStateException> {
            fixture.pool.acquire().use { error("command failed") }
        }
        fixture.timers.single().fire()
        assertEquals(1, fixture.resources.single().closes)
    }

    @Test fun deadShellIsReplacedAndDisposedWithoutClosingOtherBorrowers() {
        val fixture = Fixture()
        val old = fixture.pool.acquire()
        old.value.alive = false
        fixture.pool.acquire().use { replacement ->
            assertNotSame(old.value, replacement.value)
            assertEquals(0, old.value.closes)
            old.close()
            assertEquals(1, old.value.closes)
            assertEquals(0, replacement.value.closes)
        }
    }
}
