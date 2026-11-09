package com.zying.zysu.ui.screen

import androidx.annotation.StringRes
import com.zying.zysu.R
import com.zying.zysu.ui.viewmodel.ModuleViewModel

/** Explain why this module's preinit actions will not run on the next boot. */
@StringRes
internal fun ModuleViewModel.ModuleInfo.preinitWarning(): Int? = when (preinit) {
    "no_space" -> R.string.module_preinit_no_space
    "error" -> R.string.module_preinit_error
    "bootloop" -> R.string.module_preinit_bootloop
    "unsupported" -> R.string.module_preinit_unsupported
    else -> null
}
