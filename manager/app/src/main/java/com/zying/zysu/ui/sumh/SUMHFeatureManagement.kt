package com.zying.zysu.ui.sumh

import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.SUMHFeatureState
import com.zying.zysu.ui.sumh.util.SUMHFeatureSupport
import com.zying.zysu.ui.sumh.util.SUMHManagedFeature
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as Controller

internal fun Controller.SUMHPConfig.configuredSUMHFeatures(): Set<SUMHManagedFeature> = buildSet {
    addAll(SUMHManagedFeature.entries.filter { it.alwaysEnabled })
    if (enableMapsSpoof) add(SUMHManagedFeature.Maps)
    if (enableKernelBuildSpoof) add(SUMHManagedFeature.KernelBuild)
}

internal fun Controller.SUMHPConfig.disableSUMHFeatures(features: Set<SUMHManagedFeature>) = copy(
    enableMapsSpoof = enableMapsSpoof && SUMHManagedFeature.Maps !in features,
    enableKernelBuildSpoof = enableKernelBuildSpoof && SUMHManagedFeature.KernelBuild !in features,
)

@Composable
internal fun sumhFeatureSummary(description: String, state: SUMHFeatureState): String {
    val support = if (state.support == SUMHFeatureSupport.Supported) null else stringResource(when (state.support) {
        SUMHFeatureSupport.Supported -> R.string.sumh_features_support_yes
        SUMHFeatureSupport.Unsupported -> if (state.configuredEnabled) R.string.sumh_disable_unsupported
            else R.string.sumh_features_support_no
        SUMHFeatureSupport.Unknown -> R.string.sumh_features_support_unknown
        SUMHFeatureSupport.KernelUnavailable -> R.string.sumh_features_kernel_unavailable
    })
    val runtime = when {
        state.runtimeEnabled != null && (state.configuredEnabled || state.runtimeEnabled) ->
            stringResource(if (state.runtimeEnabled) R.string.sumh_feature_running else R.string.sumh_feature_not_running)
        state.runtimeEnabled == null && state.configuredEnabled && state.support != SUMHFeatureSupport.KernelUnavailable ->
            stringResource(R.string.sumh_feature_runtime_unknown)
        else -> null
    }
    return listOfNotNull(description, support, runtime).joinToString("\n")
}
