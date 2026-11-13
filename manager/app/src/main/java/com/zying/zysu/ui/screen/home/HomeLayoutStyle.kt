package com.zying.zysu.ui.screen.home

/** Stable settings/home_layout IDs; legacy choices migrate without losing the user's intent. */
enum class HomeLayoutStyle(val id: String) {
    Circle("circle"),
    Grid("grid"),
    Focus("focus");

    companion object {
        fun fromValue(value: String?): HomeLayoutStyle = when (value) {
            "adaptive", "kernelsu" -> Grid
            "list" -> Circle
            "dashboard_ui" -> Circle
            "stats" -> Focus
            else -> entries.firstOrNull { it.id == value } ?: Circle
        }
    }
}

/** Collapse machine output to one readable line while keeping the original available in details. */
internal fun compactHomeValue(value: String, limit: Int = 48): String {
    require(limit >= 2)
    val normalized = value.trim().replace(Regex("\\s+"), " ")
    return if (normalized.length <= limit) normalized else normalized.take(limit - 1).trimEnd() + "…"
}

internal fun homeReleaseLabel(value: String): String =
    Regex("[vV]?\\d+(?:\\.\\d+){1,3}").find(value)?.value ?: compactHomeValue(value, 24)
