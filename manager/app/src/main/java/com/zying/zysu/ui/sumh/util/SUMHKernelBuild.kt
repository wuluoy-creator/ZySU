package com.zying.zysu.ui.sumh.util

internal const val KERNEL_BUILD_EARLY_STAGE = "post-fs-data"
internal const val KERNEL_BUILD_LATE_STAGE = "boot-completed"

internal data class KernelBuildState(
    val enabled: Boolean,
    val release: String,
    val version: String,
    val originalRelease: String,
    val originalVersion: String,
    val suggestedVersion: String,
    val suggestionSource: String,
)

internal fun validKernelBuildField(value: String, release: Boolean = false): Boolean =
    value.toByteArray(Charsets.UTF_8).size <= 64 &&
        value.none { it < '\u0020' || it == '\u007f' || (release && it == ' ') }

internal fun validKernelBuildOverride(enabled: Boolean, release: String, version: String): Boolean =
    validKernelBuildField(release, release = true) && validKernelBuildField(version) &&
        (!enabled || release.isNotEmpty() || version.isNotEmpty())

internal data class KernelBuildOverride(val release: String, val version: String) {
    val enabled: Boolean get() = release.isNotEmpty() || version.isNotEmpty()
}

/** A saved pair is the setting: empty fields use the original kernel values. */
internal fun kernelBuildOverride(release: String, version: String): KernelBuildOverride? {
    val value = KernelBuildOverride(release.trim(), version.trim())
    return value.takeIf { validKernelBuildOverride(it.enabled, it.release, it.version) }
}
