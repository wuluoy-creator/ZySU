package com.zying.zysu.ui.screen.home

import com.zying.zysu.ui.viewmodel.HomeDeviceMetrics

internal data class HomeFact(val id: String, val label: String, val value: String, val summary: String = compactHomeValue(value))

internal data class HomeStatusPresentation(
    val title: String,
    val installed: Boolean,
    val safeMode: Boolean = false,
    val version: HomeFact? = null,
    val badges: List<String> = emptyList(),
    val needsAuthentication: Boolean = false,
)

/** Pure presentation input. No renderer reads native state or opens a root shell. */
internal data class HomeLayoutState(
    val status: HomeStatusPresentation,
    val facts: List<HomeFact> = emptyList(),
    val buildFacts: List<HomeFact> = emptyList(),
    val superuserCount: Int? = null,
    val moduleCount: Int? = null,
    val metrics: HomeDeviceMetrics? = null,
    val showCounts: Boolean = true,
    val showMetrics: Boolean = true,
    val showBuildDetails: Boolean = false,
)

/** Unknown counts stay unknown; a partial read must not produce a misleading complete chart. */
internal data class HomeCountDistribution private constructor(val superuser: Int, val modules: Int) {
    val total: Long get() = superuser.toLong() + modules.toLong()
    val moduleFraction: Float get() = if (total == 0L) 0f else modules.toFloat() / total.toFloat()

    companion object {
        fun fromCounts(superuser: Int?, modules: Int?): HomeCountDistribution? =
            if (superuser == null || modules == null || superuser < 0 || modules < 0) null
            else HomeCountDistribution(superuser, modules)
    }
}
