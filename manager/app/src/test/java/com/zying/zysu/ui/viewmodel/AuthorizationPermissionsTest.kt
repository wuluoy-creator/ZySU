package com.zying.zysu.ui.viewmodel

import com.zying.zysu.Natives
import kotlinx.coroutines.CancellationException
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class AuthorizationPermissionsTest {
    private val original = Natives.Profile(
        name = "test.app", currentUid = 10123, allowSu = false,
        rootUseDefault = false, rootTemplate = "custom", uid = 1000, gid = 1000,
        groups = listOf(3003), capabilities = listOf(1), capabilitiesPermitted = listOf(1),
        context = "u:r:custom:s0", namespace = 2, flags = 1L,
        nonRootUseDefault = true, umountModules = true, rules = "custom rules",
    )

    private class Store(vararg profiles: Natives.Profile) : AuthorizationPermissions.Store {
        val profiles = profiles.associateBy { it.currentUid }.toMutableMap()
        val writes = mutableListOf<Int>()
        val failed = mutableSetOf<Int>()
        val throwing = mutableSetOf<Int>()

        override fun read(target: AuthorizationPermissions.Target): Natives.Profile {
            if (target.uid in throwing) error("Profile read failed")
            return profiles.getValue(target.uid)
        }

        override fun write(profile: Natives.Profile): Boolean {
            writes += profile.currentUid
            if (profile.currentUid in failed) return false
            profiles[profile.currentUid] = profile
            return true
        }
    }

    @Test fun rootChangesPreserveCustomAndNonRootSettings() {
        val store = Store(original)
        val targets = listOf(AuthorizationPermissions.Target(original.currentUid, original.name))
        val granted = AuthorizationPermissions.apply(targets, true, null, store)
        assertEquals(listOf(original.copy(allowSu = true)), granted.updated)
        assertTrue(granted.failed.isEmpty())

        val denied = AuthorizationPermissions.apply(targets, false, null, store)
        assertEquals(listOf(original), denied.updated)
    }

    @Test fun explicitlyDisablingUnmountDoesNotInheritTheGlobalDefault() {
        val store = Store(original)
        val target = AuthorizationPermissions.Target(original.currentUid, original.name)
        val result = AuthorizationPermissions.apply(listOf(target), false, false, store)
        assertEquals(listOf(original.copy(umountModules = false, nonRootUseDefault = false)), result.updated)
        assertFalse(store.profiles.getValue(original.currentUid).nonRootUseDefault)
    }

    @Test fun sharedUidIsWrittenOnceAndPartialFailuresAreReported() {
        val falseWrite = original.copy(name = "failed.app", currentUid = 10124)
        val failedRead = original.copy(name = "throwing.app", currentUid = 10125)
        val store = Store(original, falseWrite, failedRead).apply {
            failed += falseWrite.currentUid
            throwing += failedRead.currentUid
        }
        val targets = listOf(
            AuthorizationPermissions.Target(original.currentUid, original.name),
            AuthorizationPermissions.Target(original.currentUid, "shared.uid.app"),
            AuthorizationPermissions.Target(falseWrite.currentUid, falseWrite.name),
            AuthorizationPermissions.Target(failedRead.currentUid, failedRead.name),
        )
        val result = AuthorizationPermissions.apply(targets, true, null, store)
        assertEquals(listOf(10123, 10124), store.writes)
        assertEquals(listOf(10123), result.updated.map { it.currentUid })
        assertEquals(listOf(10124, 10125), result.failed.map { it.target.uid })
        assertEquals(falseWrite, store.profiles[10124])
        assertEquals(failedRead, store.profiles[10125])
    }

    @Test fun moduleIsolationIsExplicitAndOnlyConfigurableForInstalledAppUids() {
        val app = original.copy(currentUid = 210123, allowSu = true, umountModules = false)
        val system = original.copy(currentUid = 1000)
        val shell = original.copy(currentUid = 2000)
        val isolated = original.copy(currentUid = 290001)
        val store = Store(app, system, shell, isolated)
        val targets = listOf(app, system, shell, isolated).map {
            AuthorizationPermissions.Target(it.currentUid, it.name)
        }
        val result = AuthorizationPermissions.apply(targets, false, true, store)
        assertEquals(listOf(app.copy(allowSu = false, umountModules = true, nonRootUseDefault = false)), result.updated)
        assertEquals(listOf(1000, 2000, 290001), result.failed.map { it.target.uid })
        assertEquals(listOf(210123), store.writes)
        // System Root controls remain independent of module isolation.
        val rootResult = AuthorizationPermissions.apply(listOf(targets[1]), true, null, store)
        assertEquals(listOf(system.copy(allowSu = true)), rootResult.updated)
    }

    @Test fun cancellationIsNeverReportedAsAPermissionFailure() {
        val target = AuthorizationPermissions.Target(original.currentUid, original.name)
        val store = object : AuthorizationPermissions.Store {
            override fun read(target: AuthorizationPermissions.Target): Natives.Profile =
                throw CancellationException("Cancelled")
            override fun write(profile: Natives.Profile): Boolean = error("Must not write")
        }
        assertFailsWith<CancellationException> {
            AuthorizationPermissions.apply(listOf(target), true, null, store)
        }
    }

    @Test fun removedAppsAndReusedUidsCannotReceiveAConfirmedGrant() {
        val survivor = original.copy(currentUid = 10126, name = "surviving.app")
        val reusedUid = original.copy(currentUid = 10124, name = "replacement.app")
        val store = Store(survivor, reusedUid)
        val confirmed = listOf(
            AuthorizationPermissions.Target(10123, "removed.app"),
            AuthorizationPermissions.Target(10124, "previous.app"),
            // Preserve a selected UID that had already disappeared before confirmation.
            AuthorizationPermissions.Target(10125, "", emptySet()),
            AuthorizationPermissions.Target(10126, "surviving.app"),
        )
        val current = listOf(
            AuthorizationPermissions.Target(10124, "replacement.app"),
            AuthorizationPermissions.Target(10126, "surviving.app"),
        )
        val result = AuthorizationPermissions.applyConfirmed(confirmed, current, true, null, store)
        assertEquals(listOf(10126), store.writes)
        assertEquals(listOf(10123, 10124, 10125), result.failed.map { it.target.uid })
        assertEquals(listOf(10126), result.updated.map { it.currentUid })
        assertFalse(store.profiles.getValue(10124).allowSu)
    }

    @Test fun changedSharedUidMembershipRejectsTheWholeGroup() {
        val store = Store(original)
        val confirmed = AuthorizationPermissions.Target(10123, "test.app", setOf("test.app", "shared.app"))
        val current = AuthorizationPermissions.Target(10123, "test.app", setOf("test.app", "new.app"))
        val result = AuthorizationPermissions.applyConfirmed(listOf(confirmed), listOf(current), true, null, store)
        assertTrue(store.writes.isEmpty())
        assertEquals(listOf(10123), result.failed.map { it.target.uid })
    }

    @Test fun unchangedSharedUidMembershipCanUseANewValidProfileKey() {
        val store = Store(original)
        val packages = setOf("test.app", "shared.app")
        val confirmed = AuthorizationPermissions.Target(10123, "test.app", packages)
        val current = AuthorizationPermissions.Target(10123, "shared.app", packages.reversed().toSet())
        val result = AuthorizationPermissions.applyConfirmed(listOf(confirmed), listOf(current), true, null, store)
        assertEquals(listOf(10123), store.writes)
        assertTrue(result.failed.isEmpty())
    }
}
