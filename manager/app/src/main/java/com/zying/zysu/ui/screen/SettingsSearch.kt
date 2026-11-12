package com.zying.zysu.ui.screen

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.zying.zysu.Natives
import com.zying.zysu.R
import com.zying.zysu.ui.component.LocalBottomBarPadding
import com.zying.zysu.ui.component.YukiPanel
import com.zying.zysu.ui.component.YukiSectionHeading
import java.text.Normalizer
import java.util.Locale

internal enum class SettingsSearchTarget { Appearance, Display, App, Authorization, Mount, Kernel, Diagnostics, About }

private data class SettingsSearchEntry(
    val target: SettingsSearchTarget,
    val title: Int,
    val summary: Int,
    val icon: ImageVector,
    val keywords: List<Int> = emptyList(),
    val rootRequired: Boolean = false,
)

/** All query words must match; accent/case folding also works for non-Latin category names. */
internal fun settingsSearchScore(query: String, title: String, summary: String, keywords: String): Int? {
    fun fold(value: String) = Normalizer.normalize(value, Normalizer.Form.NFKD)
        .replace(Regex("\\p{M}+"), "").lowercase(Locale.ROOT)
    val words = fold(query).trim().split(Regex("\\s+")).filter { it.isNotEmpty() }
    val foldedTitle = fold(title)
    val haystack = "$foldedTitle ${fold(summary)} ${fold(keywords)}"
    if (words.isEmpty() || words.any { it !in haystack }) return null
    return words.sumOf { if (foldedTitle.startsWith(it)) 3 else if (it in foldedTitle) 2 else 1 }
}

@Composable
internal fun SettingsSearchResults(query: String, contentPadding: PaddingValues, onOpen: (SettingsSearchTarget) -> Unit) {
    val rootAvailable = remember { Natives.isManager && Natives.version != null }
    val entries = listOf(
        SettingsSearchEntry(SettingsSearchTarget.Appearance, R.string.settings_category_appearance,
            R.string.settings_category_appearance_summary, Icons.Filled.Palette,
            listOf(R.string.theme_color, R.string.settings_custom_background, R.string.settings_card_alpha,
                R.string.settings_card_dim, R.string.app_dpi_title, R.string.settings_language,
                R.string.appearance_amoled, R.string.appearance_font_scale, R.string.appearance_card_corners,
                R.string.appearance_font_family, R.string.appearance_reset, R.string.appearance_background_blur,
                R.string.nav_style_title, R.string.nav_style_glass, R.string.nav_style_material,
                R.string.nav_style_floating_standard, R.string.nav_style_floating_drawer)),
        SettingsSearchEntry(SettingsSearchTarget.Display, R.string.settings_category_display,
            R.string.settings_category_display_summary, Icons.Filled.ViewList,
            listOf(R.string.settings_module_description_max_lines, R.string.show_more_module_info,
                R.string.simple_mode, R.string.hide_other_info, R.string.hide_seccomp_status,
                R.string.hide_link_card, R.string.hide_tag_card, R.string.appearance_display_home,
                R.string.appearance_home_metrics, R.string.appearance_home_counts, R.string.appearance_home_details,
                R.string.appearance_home_layout, R.string.home_layout_circle, R.string.home_layout_grid,
                R.string.home_layout_focus, R.string.home_layout_dashboard, R.string.home_layout_stats,
                R.string.hide_kernel_kernelsu_version, R.string.kernel_simple_kernel,
                R.string.hide_zygisk_implement, R.string.hide_meta_module_implement)),
        SettingsSearchEntry(SettingsSearchTarget.App, R.string.settings_category_app,
            R.string.settings_category_app_summary, Icons.Filled.SystemUpdate,
            listOf(R.string.settings_check_update, R.string.settings_auto_update_ksud, R.string.settings_uninstall)),
        SettingsSearchEntry(SettingsSearchTarget.Authorization, R.string.nav_policies,
            R.string.settings_profile_template_summary, Icons.Filled.AdminPanelSettings,
            listOf(R.string.settings_profile_template, R.string.settings_superkey_management, R.string.settings_umount_modules_default), true),
        SettingsSearchEntry(SettingsSearchTarget.Mount, R.string.settings_category_mount_isolation,
            R.string.settings_category_mount_isolation_summary, Icons.Filled.Folder, rootRequired = true),
        SettingsSearchEntry(SettingsSearchTarget.Kernel, R.string.settings_category_kernel,
            R.string.settings_category_kernel_summary, Icons.Filled.Security,
            listOf(R.string.selinux, R.string.settings_category_webui), true),
        SettingsSearchEntry(SettingsSearchTarget.Diagnostics, R.string.settings_category_diagnostics,
            R.string.settings_category_diagnostics_summary, Icons.Filled.BugReport, listOf(R.string.send_log)),
        SettingsSearchEntry(SettingsSearchTarget.About, R.string.about, R.string.app_name, Icons.Filled.Info),
    )
    val matches = entries.filter { rootAvailable || !it.rootRequired }.mapNotNull { entry ->
        val score = settingsSearchScore(query, stringResource(entry.title), stringResource(entry.summary),
            entry.keywords.map { stringResource(it) }.joinToString(" "))
        score?.let { entry to it }
    }.sortedByDescending { it.second }
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(contentPadding)
            .padding(horizontal = 18.dp).padding(bottom = LocalBottomBarPadding.current + 24.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        YukiSectionHeading(stringResource(R.string.settings_search_results))
        if (matches.isEmpty()) {
            Text(stringResource(R.string.settings_search_empty), Modifier.padding(16.dp),
                style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        matches.forEach { (entry, _) ->
            YukiPanel {
                SettingItem(icon = entry.icon, title = stringResource(entry.title),
                    summary = stringResource(entry.summary), groupPosition = SettingsItemPosition.Only,
                    onClick = { onOpen(entry.target) })
            }
        }
    }
}
