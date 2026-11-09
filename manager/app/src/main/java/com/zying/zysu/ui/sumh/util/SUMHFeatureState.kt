package com.zying.zysu.ui.sumh.util

internal enum class SUMHManagedFeature(val capability: String) {
    Stealth("stealth"),
    MountHide("mount_hide"),
    OverlayXattr("overlay_xattr_hide"),
    Statfs("statfs_spoof"),
    Maps("maps_spoof"),
    KernelBuild("kernel_build_spoof"),
}

internal enum class SUMHFeatureSupport { Supported, Unsupported, Unknown, KernelUnavailable }

internal enum class SUMHFeatureFilter { All, ConfiguredOn, Supported, Unsupported }

internal data class SUMHFeatureState(
    val feature: SUMHManagedFeature,
    val configuredEnabled: Boolean,
    val support: SUMHFeatureSupport,
    val runtimeEnabled: Boolean? = null,
) {
    // Saved settings can always be cleared, including after changing kernels.
    val canChange: Boolean
        get() = configuredEnabled || support == SUMHFeatureSupport.Supported
}

internal fun sumhFeatureStates(
    configuredEnabled: Set<SUMHManagedFeature>,
    kernelAvailable: Boolean,
    capabilities: Set<String>?,
    activeRuleTypes: Set<String>? = null,
): List<SUMHFeatureState> = SUMHManagedFeature.entries.map { feature ->
    SUMHFeatureState(
        feature = feature,
        configuredEnabled = feature in configuredEnabled,
        support = when {
            !kernelAvailable -> SUMHFeatureSupport.KernelUnavailable
            feature == SUMHManagedFeature.Stealth -> SUMHFeatureSupport.Supported
            capabilities == null -> SUMHFeatureSupport.Unknown
            feature.capability in capabilities -> SUMHFeatureSupport.Supported
            else -> SUMHFeatureSupport.Unsupported
        },
        runtimeEnabled = if (kernelAvailable && feature != SUMHManagedFeature.OverlayXattr)
            activeRuleTypes?.contains(feature.capability) else null,
    )
}

internal fun SUMHFeatureState.matches(
    filter: SUMHFeatureFilter,
    query: String,
    searchTerms: List<String>,
): Boolean {
    val matchesFilter = when (filter) {
        SUMHFeatureFilter.All -> true
        SUMHFeatureFilter.ConfiguredOn -> configuredEnabled
        SUMHFeatureFilter.Supported -> support == SUMHFeatureSupport.Supported
        SUMHFeatureFilter.Unsupported -> support == SUMHFeatureSupport.Unsupported
    }
    val trimmed = query.trim()
    return matchesFilter && (trimmed.isEmpty() ||
        (searchTerms + feature.capability).any { it.contains(trimmed, ignoreCase = true) })
}

internal fun unsupportedConfiguredFeatures(states: List<SUMHFeatureState>): Set<SUMHManagedFeature> =
    states.filter { it.configuredEnabled && it.support == SUMHFeatureSupport.Unsupported }
        .mapTo(linkedSetOf()) { it.feature }
