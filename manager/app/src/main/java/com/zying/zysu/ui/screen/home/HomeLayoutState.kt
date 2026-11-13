package com.zying.zysu.ui.screen.home

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
    val showBuildDetails: Boolean = false,
)
