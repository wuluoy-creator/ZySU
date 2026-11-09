package com.zying.zysu.ui.viewmodel

import android.app.Application
import android.content.pm.ApplicationInfo
import android.content.pm.PackageInfo
import android.os.TransactionTooLargeException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.test.runTest
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class PackageListLoaderTest {
    private fun packages(count: Int) = List(count) { index ->
        PackageInfo().apply {
            packageName = "app.$index"
            applicationInfo = ApplicationInfo().apply { uid = 10000 + index }
        }
    }

    @Test fun oversizedBinderPagesShrinkWithoutSkippingApps() {
        val source = packages(123)
        val reads = mutableListOf<Pair<Int, Int>>()
        val result = readPackagePages(source.size, { start, count ->
            reads += start to count
            if (count > 25) throw TransactionTooLargeException()
            source.subList(start, start + count)
        })
        assertEquals(source, result)
        assertEquals(listOf(0 to 100, 0 to 50, 0 to 25), reads.take(3))
    }

    @Test fun emptyOrTruncatedResponsesAreNeverSuccessful() {
        assertFailsWith<IllegalStateException> { readPackagePages(0, { _, _ -> emptyList() }) }
        val source = packages(1)
        assertFailsWith<IllegalStateException> {
            readPackagePages(2, { start, _ -> if (start == 0) source else emptyList() })
        }
    }

    @Test fun duplicateAppsAndMissingApplicationInfoRejectTheSnapshot() {
        val source = packages(1)
        assertFailsWith<IllegalStateException> { readPackagePages(2, { _, _ -> source }) }
        assertFailsWith<IllegalStateException> {
            readPackagePages(1, { _, _ -> listOf(PackageInfo().apply { packageName = "missing.info" }) })
        }
    }

    @Test fun transientFailureRetriesWithAFreshSnapshot() = runTest {
        val attempts = mutableListOf<Boolean>()
        val result = retryPackageLoad { refresh ->
            attempts += refresh
            if (attempts.size == 1) error("Service temporarily unavailable")
            packages(2)
        }
        assertEquals(2, result.size)
        assertEquals(listOf(false, true), attempts)
    }

    @Test fun permanentFailureIsBoundedAndCancellationDoesNotRetry() = runTest {
        var attempts = 0
        assertFailsWith<IllegalStateException> {
            retryPackageLoad { attempts++; error("Unavailable") }
        }
        assertEquals(3, attempts)
        attempts = 0
        assertFailsWith<CancellationException> {
            retryPackageLoad { attempts++; throw CancellationException("Cancelled") }
        }
        assertEquals(1, attempts)
    }
}
