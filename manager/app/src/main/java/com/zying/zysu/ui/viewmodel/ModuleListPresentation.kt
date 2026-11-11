package com.zying.zysu.ui.viewmodel

import java.text.Collator
import java.util.Locale

internal enum class ModuleListFilter {
    All, Enabled, Disabled, Pending;

    fun matches(module: ModuleViewModel.ModuleInfo): Boolean = when (this) {
        All -> true
        Enabled -> module.enabled && !module.remove
        Disabled -> !module.enabled && !module.remove
        Pending -> module.update || module.remove
    }
}

/** Priorities are independent from alphabetical direction; removed modules are never active. */
internal fun moduleListComparator(
    enabledFirst: Boolean,
    actionFirst: Boolean,
    byName: Boolean,
    descending: Boolean,
    locale: Locale = Locale.getDefault(),
): Comparator<ModuleViewModel.ModuleInfo> {
    val collator = Collator.getInstance(locale)
    val names = Comparator<ModuleViewModel.ModuleInfo> { a, b ->
        val aName = if (byName) a.name.ifBlank { a.id } else a.id
        val bName = if (byName) b.name.ifBlank { b.id } else b.id
        if (descending) collator.compare(bName, aName) else collator.compare(aName, bName)
    }
    return compareByDescending<ModuleViewModel.ModuleInfo> { it.metamodule && it.enabled && !it.remove }
        .thenBy { enabledFirst && (!it.enabled || it.remove) }
        .thenBy { actionFirst && !(it.hasWebUi || it.hasActionScript) }
        .then(names)
        .thenBy { it.dirId }
}
