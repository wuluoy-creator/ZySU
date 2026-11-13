package com.zying.zysu.ui.util

import android.util.Log
import com.topjohnwu.superuser.Shell
import java.util.concurrent.ScheduledThreadPoolExecutor
import java.util.concurrent.TimeUnit

internal object RootShells {
    private val scheduler = ScheduledThreadPoolExecutor(1) { task ->
        Thread(task, "root-shell-idle").apply { isDaemon = true }
    }.apply { removeOnCancelPolicy = true }

    private fun pool(globalMnt: Boolean) = IdleResourcePool(
        idleMillis = 30_000,
        create = { createRootShell(globalMnt) },
        isAlive = { shell: Shell -> shell.isAlive },
        dispose = { shell ->
            runCatching { shell.close() }
                .onFailure { Log.w("RootShells", "Failed to close idle shell", it) }
        },
        schedule = { delay, action ->
            val task = scheduler.schedule({ action() }, delay, TimeUnit.MILLISECONDS)
            val cancel: () -> Unit = { task.cancel(false) }
            cancel
        },
    )

    private val local = pool(false)
    private val global = pool(true)

    fun acquire(globalMnt: Boolean) = (if (globalMnt) global else local).acquire()

    fun invalidate() {
        local.invalidate()
        global.invalidate()
    }
}

/** Keep jobs, RootService launch tasks and open SuFile streams inside this scope. */
internal inline fun <T> withRootShell(globalMnt: Boolean = false, block: (Shell) -> T): T =
    RootShells.acquire(globalMnt).use { block(it.value) }
