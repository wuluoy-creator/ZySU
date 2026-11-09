package com.zying.zysu.ui

import android.content.pm.PackageInfo

/** Root can query full users as well as managed profiles; userProfiles alone cannot. */
internal fun loadUserPackageSnapshot(
    userManager: Any,
    profileIds: () -> List<Int>,
    queryPackages: (Int) -> List<PackageInfo>,
): List<PackageInfo> {
    val ids = queryAllUserIds(userManager).ifEmpty { profileIds() }
        .filter { it >= 0 }.distinct().sorted()
    return ids.flatMap(queryPackages)
}

private fun queryAllUserIds(userManager: Any): List<Int> {
    // Signatures vary across Android releases. Exclude partial, dying and
    // pre-created users when the full overload is available.
    val booleanType = Boolean::class.javaPrimitiveType!!
    for (arguments in listOf(arrayOf(true, true, true), arrayOf(true), emptyArray())) {
        val ids = runCatching {
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
        }.getOrNull()
        if (!ids.isNullOrEmpty()) return ids
    }
    return emptyList()
}
