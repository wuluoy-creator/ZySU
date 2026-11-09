package com.zying.zysu.ui.viewmodel

import android.app.Application
import android.content.ComponentName
import android.content.ServiceConnection
import android.os.Binder
import android.os.IBinder
import com.topjohnwu.superuser.ipc.RootService
import kotlinx.coroutines.CancellableContinuation
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.async
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.Implementation
import org.robolectric.annotation.Implements
import kotlin.test.assertNull
import kotlin.test.assertSame
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], shadows = [ShadowPackageRootService::class])
class KsuServiceConnectionTest {
    private val binderField = SuperUserViewModel::class.java.getDeclaredField("ksuServiceBinder").apply {
        isAccessible = true
    }
    private val connectionField = SuperUserViewModel::class.java.getDeclaredField("ksuServiceConnection").apply {
        isAccessible = true
    }
    private val startTaskField = SuperUserViewModel::class.java.getDeclaredField("ksuServiceStartTask").apply {
        isAccessible = true
    }
    private var savedBinder: Any? = null
    private var savedConnection: Any? = null
    private var savedStartTask: Any? = null
    private val component = ComponentName("com.zying.zysu", "com.zying.zysu.ui.KsuService")

    @Before fun isolateSharedConnection() {
        savedBinder = binderField.get(null)
        savedConnection = connectionField.get(null)
        savedStartTask = startTaskField.get(null)
        binderField.set(null, null)
        connectionField.set(null, null)
        startTaskField.set(null, null)
        ShadowPackageRootService.unbound.clear()
    }

    @After fun restoreSharedConnection() {
        binderField.set(null, savedBinder)
        connectionField.set(null, savedConnection)
        startTaskField.set(null, savedStartTask)
        ShadowPackageRootService.unbound.clear()
    }

    private data class PendingConnection(
        val connection: ServiceConnection,
        val result: Deferred<IBinder?>,
    )

    /** Construct the production callback without starting a service or a shell. */
    private fun CoroutineScope.pendingConnection(): PendingConnection {
        lateinit var continuation: CancellableContinuation<IBinder?>
        val result = async(start = CoroutineStart.UNDISPATCHED) {
            suspendCancellableCoroutine<IBinder?> { continuation = it }
        }
        val type = SuperUserViewModel.Companion::class.java.declaredClasses.single {
            it.simpleName == "KsuServiceConnection"
        }
        val constructor = type.getDeclaredConstructor(CancellableContinuation::class.java).apply {
            isAccessible = true
        }
        return PendingConnection(constructor.newInstance(continuation) as ServiceConnection, result)
    }

    private fun ServiceConnection.cancelBinding() {
        javaClass.getDeclaredMethod("cancel").apply { isAccessible = true }.invoke(this)
    }

    @Test fun cancelledBindingRejectsItsLateConnectedCallback() = runTest {
        val pending = pendingConnection()
        connectionField.set(null, pending.connection)
        pending.result.cancel()
        pending.connection.cancelBinding()

        pending.connection.onServiceConnected(component, Binder())

        assertNull(connectionField.get(null))
        assertNull(binderField.get(null))
        assertTrue(pending.result.isCancelled)
        assertTrue(ShadowPackageRootService.unbound.all { it === pending.connection })
        assertTrue(ShadowPackageRootService.unbound.isNotEmpty())
    }

    @Test fun staleCallbacksCannotReplaceOrClearANewerBinding() = runTest {
        val old = pendingConnection()
        val current = pendingConnection()
        val currentBinder = Binder()
        connectionField.set(null, current.connection)
        current.connection.onServiceConnected(component, currentBinder)
        assertSame(currentBinder, current.result.await())

        old.result.cancel()
        old.connection.cancelBinding()
        old.connection.onServiceConnected(component, Binder())
        old.connection.onServiceDisconnected(component)
        old.connection.onBindingDied(component)

        assertSame(current.connection, connectionField.get(null))
        assertSame(currentBinder, binderField.get(null))
        assertTrue(ShadowPackageRootService.unbound.all { it === old.connection })
    }

    @Test fun disconnectClearsTheCurrentBindingAndResumesOnlyOnce() = runTest {
        val pending = pendingConnection()
        connectionField.set(null, pending.connection)
        binderField.set(null, Binder())

        pending.connection.onServiceDisconnected(component)
        assertNull(pending.result.await())
        pending.connection.onNullBinding(component)
        pending.connection.onBindingDied(component)

        assertNull(connectionField.get(null))
        assertNull(binderField.get(null))
        assertTrue(ShadowPackageRootService.unbound.all { it === pending.connection })
    }

    @Test fun successfulContinuationIsNotResumedAgainByDisconnect() = runTest {
        val pending = pendingConnection()
        val binder = Binder()
        connectionField.set(null, pending.connection)

        pending.connection.onServiceConnected(component, binder)
        assertSame(binder, pending.result.await())
        pending.connection.onServiceDisconnected(component)
        pending.connection.onBindingDied(component)

        assertSame(binder, pending.result.await())
        assertNull(connectionField.get(null))
        assertNull(binderField.get(null))
    }
}

/** Keep these callback tests independent of libsu's real process and binding manager. */
@Implements(RootService::class)
class ShadowPackageRootService {
    companion object {
        val unbound = mutableListOf<ServiceConnection>()

        @JvmStatic
        @Implementation
        fun unbind(connection: ServiceConnection) {
            unbound += connection
        }
    }
}
