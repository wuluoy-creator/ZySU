package com.zying.zysu.ui.viewmodel

import com.zying.zysu.Natives
import kotlinx.coroutines.CancellationException

/** Applies authorization changes once per UID while preserving unrelated profile settings. */
internal object AuthorizationPermissions {
    // Only installed app UIDs have a configurable module-content policy.
    // System services and isolated processes follow the kernel's fixed policy.
    fun canConfigureModuleUnmount(uid: Int): Boolean = uid % 100000 in 10000..19999

    data class Target(
        val uid: Int,
        val profileKey: String,
        val packageNames: Set<String> = setOf(profileKey),
    )
    data class Failure(val target: Target, val error: Exception? = null)
    data class Result(val updated: List<Natives.Profile>, val failed: List<Failure>)

    interface Store {
        fun read(target: Target): Natives.Profile
        fun write(profile: Natives.Profile): Boolean
    }

    /** A UID can be reused after uninstall; the confirmed package set must still match. */
    fun applyConfirmed(
        confirmedTargets: Collection<Target>,
        currentTargets: Collection<Target>,
        allowSu: Boolean,
        umountModules: Boolean?,
        store: Store,
    ): Result {
        val currentByUid = currentTargets.associateBy { it.uid }
        val validTargets = mutableListOf<Target>()
        val rejectedTargets = mutableListOf<Failure>()
        confirmedTargets.distinctBy { it.uid }.forEach { confirmed ->
            val current = currentByUid[confirmed.uid]
            if (current == null || confirmed.packageNames.isEmpty() ||
                current.packageNames != confirmed.packageNames
            ) {
                rejectedTargets += Failure(confirmed, IllegalStateException("Application identity changed"))
            } else {
                validTargets += current
            }
        }
        val result = apply(validTargets, allowSu, umountModules, store)
        return result.copy(failed = rejectedTargets + result.failed)
    }

    fun apply(
        targets: Collection<Target>,
        allowSu: Boolean,
        umountModules: Boolean?,
        store: Store,
    ): Result {
        val updated = mutableListOf<Natives.Profile>()
        val failed = mutableListOf<Failure>()
        targets.distinctBy { it.uid }.forEach { target ->
            try {
                require(umountModules == null || canConfigureModuleUnmount(target.uid)) {
                    "Module isolation is not configurable for this UID"
                }
                val before = store.read(target)
                val after = before.copy(
                    allowSu = allowSu,
                    umountModules = umountModules ?: before.umountModules,
                    // Explicit false is an override too; it must not inherit a true default.
                    nonRootUseDefault = if (umountModules != null) false else before.nonRootUseDefault,
                )
                if (store.write(after)) updated += after else failed += Failure(target)
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                failed += Failure(target, error)
            }
        }
        return Result(updated, failed)
    }
}
