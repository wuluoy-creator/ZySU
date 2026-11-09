package com.zying.zysu.ui

import android.app.Application
import android.content.pm.ApplicationInfo
import android.content.pm.PackageInfo
import com.zying.zysu.ui.viewmodel.SuperUserViewModel.AppInfo
import com.zying.zysu.ui.webui.pm.KsuPm
import org.json.JSONArray
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class UserPackageSnapshotTest {
    class User(
        @JvmField val id: Int,
        private val enabled: Boolean = true,
        @JvmField val partial: Boolean = false,
        @JvmField val preCreated: Boolean = false,
    ) {
        fun isEnabled() = enabled
    }

    class Users(private val entries: List<User>) {
        var exclusions = emptyList<Boolean>()
        fun getUsers(partial: Boolean, dying: Boolean, preCreated: Boolean): List<User> {
            exclusions = listOf(partial, dying, preCreated)
            return entries
        }
    }

    class LegacyUsers(private val entries: List<User>) {
        fun getUsers(): List<User> = entries
    }

    class UnavailableUsers {
        fun getUsers(): List<User> = throw SecurityException("Hidden API unavailable")
    }

    class RecoveringUsers(private val entries: List<User>) {
        var available = false
        fun getUsers(): List<User> {
            check(available) { "System user service restarting" }
            return entries
        }
    }

    private fun packages(userId: Int) = listOf(PackageInfo().apply {
        packageName = if (userId == 12) "secondary.only" else "shared"
        applicationInfo = ApplicationInfo().apply {
            uid = userId * 100000 + 12345
        }
    })

    @Test fun independentUsersReachTheWebUiAlongsideProfiles() {
        val users = Users(listOf(User(0), User(10), User(12), User(10), User(13, enabled = false), User(14, partial = true), User(15, preCreated = true)))
        val queried = mutableListOf<Int>()
        val snapshot = loadUserPackageSnapshot(users, { error("Profile fallback must not limit the all-user result") }) {
            queried += it
            packages(it)
        }
        assertEquals(listOf(true, true, true), users.exclusions)
        assertEquals(listOf(0, 10, 12), queried)
        val pm = KsuPm { snapshot.map { AppInfo(it.packageName, it, null) } }
        val result = JSONArray(pm.getPackagesInfo("[\"shared\",\"secondary.only\"]"))
        assertEquals(listOf(0, 10, 12), (0 until result.length()).map { result.getJSONObject(it).getInt("userId") })
        assertEquals("secondary.only", result.getJSONObject(2).getString("packageName"))
    }

    @Test fun legacyEnumerationStillIncludesIndependentUsers() {
        val snapshot = loadUserPackageSnapshot(LegacyUsers(listOf(User(12), User(0))), { listOf(0) }, ::packages)
        assertEquals(listOf(12345, 1212345), snapshot.map { it.applicationInfo!!.uid })
    }

    @Test fun unavailableHiddenApiFallsBackToAssociatedProfiles() {
        val snapshot = loadUserPackageSnapshot(UnavailableUsers(), { listOf(10, 0, 10) }, ::packages)
        assertEquals(listOf(12345, 1012345), snapshot.map { it.applicationInfo!!.uid })
    }

    @Test fun failedUserQueryCannotBecomeAPartialSnapshot() {
        assertFailsWith<IllegalStateException> {
            loadUserPackageSnapshot(Users(listOf(User(0), User(10))), { emptyList() }) {
                if (it == 10) error("Package manager restarting") else packages(it)
            }
        }
    }

    @Test fun emptyUserQueryIsRejectedAndCanRecoverOnTheNextLoad() {
        val users = Users(listOf(User(0), User(10)))
        assertFailsWith<IllegalStateException> {
            loadUserPackageSnapshot(users, { emptyList() }) {
                if (it == 10) emptyList() else packages(it)
            }
        }
        assertEquals(2, loadUserPackageSnapshot(users, { emptyList() }, ::packages).size)
    }

    @Test fun missingUsersCannotBecomeASuccessfulEmptySnapshot() {
        assertFailsWith<IllegalStateException> {
            loadUserPackageSnapshot(UnavailableUsers(), { emptyList() }, ::packages)
        }
    }

    @Test fun emptyAllUserResponseCannotFallBackToAPartialSnapshot() {
        var profileFallbacks = 0
        assertFailsWith<IllegalStateException> {
            loadUserPackageSnapshot(Users(emptyList()), { profileFallbacks++; listOf(0) }, ::packages)
        }
        assertEquals(0, profileFallbacks)
    }

    @Test fun transientUserServiceFailureDoesNotFallBackOrLoseIndependentUsers() {
        val users = RecoveringUsers(listOf(User(0), User(12)))
        var profileFallbacks = 0
        val profiles = { profileFallbacks++; listOf(0) }
        val failure = assertFailsWith<IllegalStateException> {
            loadUserPackageSnapshot(users, profiles, ::packages)
        }
        assertEquals("System user service restarting", failure.cause?.message)
        assertEquals(0, profileFallbacks)

        users.available = true
        val snapshot = loadUserPackageSnapshot(users, profiles, ::packages)
        assertEquals(listOf(12345, 1212345), snapshot.map { it.applicationInfo!!.uid })
        assertEquals(0, profileFallbacks)
    }
}
