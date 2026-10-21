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
) {
    // Only offer the suggestion whose meaning the UI can explain. Unknown sources
    // remain readable through the CLI without becoming an automatic draft action.
    val systemDateSuggestion: String?
        get() = suggestedVersion.takeIf {
            suggestionSource == "ro.build.date" && it.isNotEmpty() && validKernelBuildField(it)
        }

    // This is a label inferred from the *original* release, not an ABI probe or
    // a value suitable for choosing a kernel image automatically.
    val originalKmiLabel: String?
        get() {
            val release = Regex("^(\\d+\\.\\d+)(?:\\.\\d+)?(?:-.*)?$")
                .matchEntire(originalRelease) ?: return null
            val android = Regex("(?:^|-)android(\\d+)(?:-|$)").find(originalRelease)
            return android?.let { "android${it.groupValues[1]}-${release.groupValues[1]}" }
                ?: release.groupValues[1]
        }
}

internal fun validKernelBuildField(value: String, release: Boolean = false): Boolean =
    value.toByteArray(Charsets.UTF_8).size <= 64 &&
        value.none { it < '\u0020' || it == '\u007f' || (release && it == ' ') }

internal fun validKernelBuildOverride(enabled: Boolean, release: String, version: String): Boolean =
    validKernelBuildField(release, release = true) && validKernelBuildField(version) &&
        (!enabled || release.isNotEmpty() || version.isNotEmpty())
