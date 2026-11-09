package com.zying.zysu.ui.viewmodel

import android.content.pm.PackageInfo
import android.os.TransactionTooLargeException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlin.coroutines.coroutineContext

/** Publish only complete reads; a failed/short Binder response must not empty the UI cache. */
internal fun readPackagePages(
    total: Int,
    readPage: (start: Int, count: Int) -> List<PackageInfo>,
    onProgress: (Float) -> Unit = {},
): List<PackageInfo> {
    check(total > 0) { "The package service returned an empty app list" }
    val packages = ArrayList<PackageInfo>(total)
    val identities = HashSet<Pair<Int, String>>()
    var pageSize = 100
    while (packages.size < total) {
        val requested = minOf(pageSize, total - packages.size)
        val page = try {
            readPage(packages.size, requested)
        } catch (error: TransactionTooLargeException) {
            if (requested == 1) throw error
            pageSize = maxOf(1, requested / 2)
            continue
        }
        check(page.isNotEmpty() && page.size <= requested) { "Incomplete package list response" }
        page.forEach { info ->
            val app = checkNotNull(info.applicationInfo) { "Package is missing application info" }
            check(!info.packageName.isNullOrBlank() && identities.add(app.uid to info.packageName)) {
                "Package list changed while reading; retry"
            }
        }
        packages += page
        onProgress(packages.size.toFloat() / total)
    }
    return packages
}

internal suspend fun <T> retryPackageLoad(load: suspend (retry: Boolean) -> T): T {
    repeat(3) { attempt ->
        coroutineContext.ensureActive()
        try {
            return load(attempt > 0)
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            if (attempt == 2) throw error
            delay(300L * (attempt + 1))
        }
    }
    error("Unreachable")
}
