package com.zying.zysu.ui.util

import java.io.Closeable
import java.util.concurrent.atomic.AtomicBoolean

/** A resource stays alive until its last borrower returns it and the idle delay expires. */
internal class IdleResourcePool<T : Any>(
    private val idleMillis: Long,
    private val create: () -> T,
    private val isAlive: (T) -> Boolean,
    private val dispose: (T) -> Unit,
    private val schedule: (Long, () -> Unit) -> (() -> Unit),
) {
    private class Entry<T>(val value: T) {
        var borrowers = 0
        var retired = false
        var idleToken: Any? = null
        var cancelIdle: (() -> Unit)? = null
    }

    class Lease<T> internal constructor(val value: T, private val release: () -> Unit) : Closeable {
        private val returned = AtomicBoolean()
        override fun close() {
            if (returned.compareAndSet(false, true)) release()
        }
    }

    private val lock = Any()
    private var current: Entry<T>? = null

    fun acquire(): Lease<T> {
        var obsolete: T? = null
        try {
            return synchronized(lock) {
                var entry = current
                if (entry != null && !isAlive(entry.value)) {
                    retire(entry)
                    if (entry.borrowers == 0) obsolete = entry.value
                    current = null
                    entry = null
                }
                if (entry == null) {
                    entry = Entry(create())
                    current = entry
                }
                entry.cancelIdle?.invoke()
                entry.cancelIdle = null
                entry.idleToken = null
                entry.borrowers++
                val borrowed = entry
                Lease(borrowed.value) { release(borrowed) }
            }
        } finally {
            obsolete?.let(dispose)
        }
    }

    /** Authentication refresh replaces the next shell without interrupting existing jobs. */
    fun invalidate() {
        val obsolete = synchronized(lock) {
            val entry = current ?: return
            current = null
            retire(entry)
            entry.value.takeIf { entry.borrowers == 0 }
        }
        obsolete?.let(dispose)
    }

    private fun retire(entry: Entry<T>) {
        entry.retired = true
        entry.idleToken = null
        entry.cancelIdle?.invoke()
        entry.cancelIdle = null
    }

    private fun release(entry: Entry<T>) {
        val obsolete = synchronized(lock) {
            check(entry.borrowers > 0)
            entry.borrowers--
            if (entry.borrowers != 0) return
            if (entry.retired) return@synchronized entry.value
            val token = Any()
            entry.idleToken = token
            entry.cancelIdle = schedule(idleMillis) { expire(entry, token) }
            null
        }
        obsolete?.let(dispose)
    }

    private fun expire(entry: Entry<T>, token: Any) {
        val obsolete = synchronized(lock) {
            // A timer already dispatched before cancellation must not close a reacquired shell.
            if (current !== entry || entry.idleToken !== token || entry.borrowers != 0) return
            current = null
            entry.retired = true
            entry.idleToken = null
            entry.cancelIdle = null
            entry.value
        }
        dispose(obsolete)
    }
}
