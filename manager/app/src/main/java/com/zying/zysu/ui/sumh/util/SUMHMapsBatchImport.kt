package com.zying.zysu.ui.sumh.util

import com.zying.zysu.ui.sumh.util.SUMHUiAdapter.MapsPathRule

internal enum class SUMHMapsImportError {
    Format, TargetAbsolute, TargetControl, TargetTooLong,
    ReplacementAbsolute, ReplacementControl, ReplacementTooLong, DuplicateTarget,
}

internal data class SUMHMapsImportIssue(val line: Int, val error: SUMHMapsImportError)

internal data class SUMHMapsImportPreview(
    val candidate: List<MapsPathRule>,
    val additions: Int,
    val replacements: List<MapsPathRule>,
    val issues: List<SUMHMapsImportIssue>,
    val limitExceeded: Boolean,
    val budgetExceeded: Boolean,
) {
    val hasChanges: Boolean get() = additions > 0 || replacements.isNotEmpty()
    val canImport: Boolean get() = hasChanges && issues.isEmpty() && !limitExceeded && !budgetExceeded
}

/** Parse literal tab-separated paths. No trimming or path normalization is applied to either field. */
internal fun previewSUMHMapsImport(input: String, existing: List<MapsPathRule>): SUMHMapsImportPreview {
    val merged = existing.toMutableList()
    val positions = existing.mapIndexed { index, rule -> rule.targetPath to index }.toMap().toMutableMap()
    val seen = mutableSetOf<String>()
    val replacements = mutableListOf<MapsPathRule>()
    val issues = mutableListOf<SUMHMapsImportIssue>()
    var additions = 0

    input.lineSequence().forEachIndexed { index, line ->
        if (line.isBlank() || line.trimStart().startsWith('#')) return@forEachIndexed
        val tab = line.indexOf('\t')
        if (tab < 0 || line.indexOf('\t', tab + 1) >= 0) {
            issues += SUMHMapsImportIssue(index + 1, SUMHMapsImportError.Format)
            return@forEachIndexed
        }
        val target = line.substring(0, tab)
        val replacement = line.substring(tab + 1)
        val targetError = validateSUMHManagedMapsPath(target, 4095)
        val replacementError = validateSUMHManagedMapsPath(replacement)
        if (targetError != null) issues += SUMHMapsImportIssue(index + 1, targetError.toImportError(true))
        if (replacementError != null) issues += SUMHMapsImportIssue(index + 1, replacementError.toImportError(false))
        val duplicate = !seen.add(target)
        if (duplicate) issues += SUMHMapsImportIssue(index + 1, SUMHMapsImportError.DuplicateTarget)
        if (targetError != null || replacementError != null || duplicate) return@forEachIndexed

        val rule = MapsPathRule(target, replacement)
        val position = positions[target]
        if (position == null) {
            positions[target] = merged.size
            merged += rule
            additions++
        } else if (merged[position] != rule) {
            merged[position] = rule
            replacements += rule
        }
    }

    return SUMHMapsImportPreview(
        candidate = merged,
        additions = additions,
        replacements = replacements,
        issues = issues,
        limitExceeded = merged.size > SUMH_MAX_MAPS_PATH_RULES,
        budgetExceeded = !mapsRulesWithinBudget(merged),
    )
}

private fun SUMHMapsPathError.toImportError(target: Boolean): SUMHMapsImportError = when (this) {
    SUMHMapsPathError.AbsolutePath -> if (target) SUMHMapsImportError.TargetAbsolute else SUMHMapsImportError.ReplacementAbsolute
    SUMHMapsPathError.ControlCharacter -> if (target) SUMHMapsImportError.TargetControl else SUMHMapsImportError.ReplacementControl
    SUMHMapsPathError.TooLong -> if (target) SUMHMapsImportError.TargetTooLong else SUMHMapsImportError.ReplacementTooLong
}
