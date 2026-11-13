package com.zying.zysu.ui.screen.home

import android.content.SharedPreferences
import androidx.annotation.StringRes
import androidx.core.content.edit
import com.zying.zysu.R

/** Stable IDs shared by Home's facts, preferences and visibility picker. */
internal enum class HomeFactField(
    val id: String,
    @param:StringRes val label: Int,
    val buildDetail: Boolean = false,
    val legacyHideKey: String? = null,
) {
    Device("device", R.string.home_device_model),
    Android("android", R.string.home_android_version),
    Kernel("kernel", R.string.home_kernel),
    Manager("manager", R.string.home_manager_version),
    Selinux("selinux", R.string.home_selinux_status),
    Mode("mode", R.string.home_working_mode),
    Seccomp("seccomp", R.string.home_seccomp_status, legacyHideKey = "is_hide_seccomp_status"),
    Zygisk("zygisk", R.string.home_zygisk_implement, legacyHideKey = "is_hide_zygisk_Implement"),
    Mount("mount", R.string.home_meta_module_implement, legacyHideKey = "is_hide_meta_module_Implement"),
    Security("security", R.string.home_security_patch, buildDetail = true),
    Architecture("architecture", R.string.home_architecture, buildDetail = true),
    Build("build", R.string.home_build_id, buildDetail = true),
    Fingerprint("fingerprint", R.string.home_fingerprint, buildDetail = true),
}

internal object HomeFactVisibility {
    private const val PREFIX = "home_fact_visible_"

    fun read(preferences: SharedPreferences): Set<String> {
        return HomeFactField.entries.filterTo(linkedSetOf()) { field ->
            if (field.legacyHideKey != null) {
                // Reuse existing hide choices as the single source of truth for these rows.
                !preferences.getBoolean(field.legacyHideKey, false)
            } else {
                preferences.getBoolean(PREFIX + field.id, true)
            }
        }.mapTo(linkedSetOf()) { it.id }
    }

    fun setVisible(preferences: SharedPreferences, field: HomeFactField, visible: Boolean) {
        preferences.edit {
            if (field.legacyHideKey != null) putBoolean(field.legacyHideKey, !visible)
            else putBoolean(PREFIX + field.id, visible)
        }
    }

    fun observes(key: String?): Boolean = key == null ||
        key.startsWith(PREFIX) || HomeFactField.entries.any { it.legacyHideKey == key }
}

internal fun visibleHomeFacts(facts: List<HomeFact>, visibleIds: Set<String>): List<HomeFact> =
    facts.filter { it.id in visibleIds }
