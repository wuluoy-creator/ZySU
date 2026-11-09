package com.zying.zysu.ui.sumh.util

internal data class SUMHConfigEffects(
    val runtimeRequired: Boolean = false,
    val rebootRequired: Boolean = false,
)

internal data class SUMHConfigSavePlan(
    val effects: SUMHConfigEffects,
    val applyRuntime: Boolean,
    val normalizeMountHideMode: Boolean,
)

private val runtimeConfigKeys = setOf(
    "enable_kernel_debug", "enable_stealth", "enable_overlay_xattr_hide",
    "enable_mount_hide", "mount_hide_mode", "enable_maps_spoof", "enable_statfs_spoof",
    "enable_kernel_build_spoof", "kernel_build_release", "kernel_build_version",
    "kernel_build_apply_stage", "maps_rules", "kstat_rules",
)

private val rebootConfigKeys = setOf(
    "work_dir", "mountsource", "fs_type", "mirror_dir", "mount_backend",
    "mirror_img_size_mb", "overlay_writable", "overlayfs_enabled", "magic_mount_enabled", "partitions",
)

/** Logging changes are stored immediately; mount planning is consumed on the next boot. */
internal fun sumhConfigEffects(changedKeys: Set<String>) = SUMHConfigEffects(
    runtimeRequired = changedKeys.any { it in runtimeConfigKeys },
    rebootRequired = changedKeys.any { it in rebootConfigKeys },
)

internal fun planSUMHConfigSave(
    changedKeys: Set<String>,
    kernelAvailable: Boolean,
    savedMountHideMode: String = "normal",
): SUMHConfigSavePlan {
    // Do not turn an unrelated edit into a runtime operation solely to migrate
    // an older mount-hide mode. Normalize when the user changes mount hiding.
    val normalizeMountHideMode = savedMountHideMode != "normal" && "enable_mount_hide" in changedKeys
    val effects = sumhConfigEffects(
        if (normalizeMountHideMode) changedKeys + "mount_hide_mode" else changedKeys,
    )
    return SUMHConfigSavePlan(
        effects = effects,
        applyRuntime = kernelAvailable && effects.runtimeRequired,
        normalizeMountHideMode = normalizeMountHideMode,
    )
}
