package com.zying.zysu.ui

import android.content.pm.PackageInfo
import java.lang.reflect.InvocationTargetException

/** Root can query full users as well as managed profiles; userProfiles alone cannot. */
internal fun loadUserPackageSnapshot(
    userManager: Any,
    profileIds: () -> List<Int>,
    queryPackages: (Int) -> List<PackageInfo>,
): List<PackageInfo> {
    val ids = queryAllUserIds(userManager).ifEmpty { profileIds() }
        .filter { it >= 0 }.distinct().sorted()
    check(ids.isNotEmpty()) { "No Android users were returned by the package service" }
    return ids.flatMap { userId ->
        // Every active Android user has system packages. An empty result is a
        // failed query, not an authoritative snapshot to cache indefinitely.
        queryPackages(userId).also {
            check(it.isNotEmpty()) { "No packages were returned for user $userId" }
        }
    }
}

private fun queryAllUserIds(userManager: Any): List<Int> {
    // Signatures vary across Android releases. Exclude partial, dying and
    // pre-created users when the full overload is available.
    val booleanType = Boolean::class.javaPrimitiveType!!
    for (arguments in listOf(arrayOf(true, true, true), arrayOf(true), emptyArray())) {
        val ids = try {
            val method = userManager.javaClass.getMethod("getUsers", *Array(arguments.size) { booleanType })
            val users = method.invoke(userManager, *arguments) as List<*>
            users.mapNotNull { user ->
                user ?: return@mapNotNull null
                val type = user.javaClass
                val enabled = type.getMethod("isEnabled").invoke(user) as Boolean
                val partial = runCatching { type.getField("partial").getBoolean(user) }.getOrDefault(false)
                val preCreated = runCatching { type.getField("preCreated").getBoolean(user) }.getOrDefault(false)
                if (!enabled || partial || preCreated) null else type.getField("id").getInt(user)
            }
        } catch (error: InvocationTargetException) {
            // Hidden API access can be denied on some Android versions, but a
            // temporary system-service failure must not become a cached profile-
            // only snapshot that silently omits independent users.
            if (error.targetException is SecurityException) null
            else throw IllegalStateException("Unable to enumerate Android users", error.targetException)
        } catch (_: NoSuchMethodException) {
            null
        } catch (_: NoSuchFieldException) {
            null
        } catch (_: IllegalAccessException) {
            null
        } catch (_: SecurityException) {
            null
        }
        if (ids != null) {
            check(ids.isNotEmpty()) { "No Android users were returned by the user service" }
            return ids
        }
    }
    return emptyList()
}
