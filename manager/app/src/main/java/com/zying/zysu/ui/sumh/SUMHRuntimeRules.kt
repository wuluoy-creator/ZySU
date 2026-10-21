package com.zying.zysu.ui.sumh

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.component.YukiPanel
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as Controller
import kotlinx.coroutines.launch

internal enum class RuntimeRuleFilter(val label: Int) {
    All(R.string.sumh_runtime_filter_all),
    Mappings(R.string.sumh_runtime_filter_mappings),
    Hidden(R.string.sumh_runtime_filter_hidden),
    Features(R.string.sumh_runtime_filter_features),
}

private val runtimeFeatures = setOf("mount_hide", "maps_spoof", "statfs_spoof", "stealth", "kernel_build_spoof")

internal fun filterRuntimeRules(
    rules: List<Controller.ActiveRule>, query: String, filter: RuntimeRuleFilter,
): List<Controller.ActiveRule> = rules.filter { rule ->
    val matchesType = when (filter) {
        RuntimeRuleFilter.All -> true
        RuntimeRuleFilter.Mappings -> rule.type == "add" || rule.type == "merge"
        RuntimeRuleFilter.Hidden -> rule.type == "hide"
        RuntimeRuleFilter.Features -> rule.type in runtimeFeatures
    }
    matchesType && (query.isEmpty() || rule.src.contains(query, ignoreCase = true) ||
        rule.target?.contains(query, ignoreCase = true) == true)
}

// NUL cannot occur in protocol paths. Keep identity independent of list position,
// so refreshes do not attach remembered row state to a different mapping.
private fun runtimeRuleKey(rule: Controller.ActiveRule) = "rule:${rule.type}\u0000${rule.src}\u0000${rule.target.orEmpty()}"

@Composable
internal fun RulesTab(
    activeRules: List<Controller.ActiveRule>,
    sumhStatus: Controller.SUMHStatus,
    controlsEnabled: Boolean,
    onRefresh: () -> Unit,
    onClearAll: () -> Unit,
    rulesIncomplete: Boolean = false,
) {
    var query by rememberSaveable { mutableStateOf("") }
    var queryLimit by remember { mutableStateOf(false) }
    var filter by rememberSaveable { mutableStateOf(RuntimeRuleFilter.All) }
    var selectedKey by rememberSaveable { mutableStateOf<String?>(null) }
    var showClearDialog by remember { mutableStateOf(false) }
    val visible = remember(activeRules, query, filter) { filterRuntimeRules(activeRules, query, filter) }
    val listState = rememberLazyListState()
    val scope = rememberCoroutineScope()
    val focus = LocalFocusManager.current
    val filtering = query.isNotEmpty() || filter != RuntimeRuleFilter.All
    val selected = remember(activeRules, selectedKey) { activeRules.find { runtimeRuleKey(it) == selectedKey } }

    if (showClearDialog) YukiAlertDialog(
        onDismissRequest = { showClearDialog = false },
        title = { Text(stringResource(R.string.sumh_rules_clear_all)) },
        text = { Text(stringResource(R.string.sumh_rules_clear_confirm)) },
        confirmButton = {
            TextButton(enabled = controlsEnabled,
                colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error),
                onClick = { showClearDialog = false; onClearAll() }) { Text(stringResource(R.string.sumh_rules_clear)) }
        },
        dismissButton = { TextButton(onClick = { showClearDialog = false }) { Text(stringResource(R.string.sumh_rules_cancel)) } },
    )
    if (selected != null) RuntimeRuleDetails(selected, onDismiss = { selectedKey = null })

    // Search, filters and notices share the list's scroll range, keeping rows
    // reachable in landscape and at large system font sizes.
    LazyColumn(
        state = listState,
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        if (sumhStatus != Controller.SUMHStatus.AVAILABLE) {
            item("unavailable") { YukiPanel { Text(stringResource(R.string.sumh_rules_not_available), Modifier.padding(16.dp)) } }
        } else {
            item("actions") {
                FlowRow(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    OutlinedButton(onClick = onRefresh, enabled = controlsEnabled) {
                        YukiIcon(Icons.Filled.Refresh, null)
                        Spacer(Modifier.width(8.dp))
                        Text(stringResource(R.string.sumh_rules_refresh))
                    }
                    TextButton(onClick = { showClearDialog = true }, enabled = controlsEnabled && activeRules.isNotEmpty(),
                        colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error)) {
                        Text(stringResource(R.string.sumh_rules_clear_all))
                    }
                }
            }
            item("search") {
                OutlinedTextField(query, onValueChange = { queryLimit = !sumhSmallDraftFits(it); if (!queryLimit) query = it },
                    supportingText = if (queryLimit) { { SUMHSmallDraftLimit() } } else null, modifier = Modifier.fillMaxWidth(),
                    singleLine = true, label = { Text(stringResource(R.string.sumh_runtime_search)) },
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                    keyboardActions = KeyboardActions(onSearch = { focus.clearFocus() }),
                    leadingIcon = { YukiIcon(Icons.Filled.Search, null) },
                    trailingIcon = { if (query.isNotEmpty()) IconButton(onClick = { query = "" }) {
                        YukiIcon(Icons.Filled.Close, stringResource(R.string.sumh_runtime_clear_search))
                    } },
                )
            }
            item("filters") {
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    RuntimeRuleFilter.entries.forEach { option ->
                        FilterChip(selected = filter == option, onClick = { filter = option; focus.clearFocus() },
                            label = { Text(stringResource(option.label)) })
                    }
                }
            }
            item("count") {
                Text(
                    if (filtering) stringResource(R.string.sumh_runtime_filtered_count, visible.size, activeRules.size)
                    else stringResource(R.string.sumh_rules_runtime_count, activeRules.size),
                    style = MaterialTheme.typography.titleSmall,
                    modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
                )
            }
            if (rulesIncomplete) item("incomplete") {
                Text(stringResource(R.string.sumh_rules_runtime_incomplete),
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
            }
            item("hint") {
                Text(stringResource(R.string.sumh_rules_runtime_hint),
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            items(visible, key = ::runtimeRuleKey, contentType = { it.type }) { rule ->
                RuntimeRuleItem(rule, onClick = { focus.clearFocus(); selectedKey = runtimeRuleKey(rule) })
            }
            if (visible.isEmpty()) item("empty") {
                Column(Modifier.fillMaxWidth().padding(vertical = 24.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(stringResource(if (filtering) R.string.sumh_runtime_no_matches else R.string.sumh_rules_runtime_empty),
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    if (filtering) TextButton(onClick = {
                        query = ""; filter = RuntimeRuleFilter.All; focus.clearFocus()
                        scope.launch { listState.scrollToItem(0) }
                    }) {
                        Text(stringResource(R.string.sumh_runtime_reset_filters))
                    }
                }
            }
        }
    }
}

@Composable
private fun runtimeRuleTitle(rule: Controller.ActiveRule): String = stringResource(when (rule.type) {
    "add" -> R.string.sumh_runtime_mapping
    "merge" -> R.string.sumh_runtime_merge
    "hide" -> R.string.sumh_runtime_hidden
    "mount_hide" -> R.string.sumh_rule_mount_hide
    "maps_spoof" -> R.string.sumh_rule_maps_spoof
    "statfs_spoof" -> R.string.sumh_rule_statfs_spoof
    "stealth" -> R.string.sumh_rule_stealth
    "kernel_build_spoof" -> R.string.sumh_kernel_build_spoof
    else -> R.string.sumh_rules_runtime_path
})

@Composable
private fun RuntimeRuleItem(rule: Controller.ActiveRule, onClick: () -> Unit) {
    YukiPanel {
        Column(Modifier.fillMaxWidth().clickable(onClickLabel = stringResource(R.string.sumh_runtime_details), onClick = onClick)
            .padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(runtimeRuleTitle(rule), style = MaterialTheme.typography.titleSmall, color = MaterialTheme.colorScheme.primary)
            if (rule.type !in runtimeFeatures) {
                RuntimePath(if (rule.rawOperands) R.string.sumh_runtime_original_paths else R.string.sumh_runtime_visible_path, rule.src)
                rule.target?.let { RuntimePath(R.string.sumh_runtime_source_path, it) }
            }
            if (rule.rawOperands) Text(stringResource(R.string.sumh_rules_runtime_raw_operands),
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (rule.hideState != null) Text(userHideStatus(rule), style = MaterialTheme.typography.bodySmall,
                color = if (rule.hideState == 3) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant)
            Text(stringResource(when {
                rule.isUserDefined -> R.string.sumh_rule_user
                rule.type in runtimeFeatures -> R.string.sumh_rules_runtime_feature
                else -> R.string.sumh_rules_runtime_path
            }), style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun RuntimePath(label: Int, path: String, expanded: Boolean = false) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(stringResource(label), style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(path, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall,
            maxLines = if (expanded) Int.MAX_VALUE else 2, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun RuntimeRuleDetails(rule: Controller.ActiveRule, onDismiss: () -> Unit) {
    val context = LocalContext.current
    val title = runtimeRuleTitle(rule)
    fun copyPath(path: String) {
        (context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager)
            .setPrimaryClip(ClipData.newPlainText(title, path))
    }
    YukiAlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                if (rule.type in runtimeFeatures) Text(stringResource(R.string.sumh_rules_runtime_feature))
                else {
                    SelectionContainer { RuntimePath(if (rule.rawOperands) R.string.sumh_runtime_original_paths else R.string.sumh_runtime_visible_path, rule.src, expanded = true) }
                    TextButton(onClick = { copyPath(rule.src) }) {
                        Text(stringResource(if (rule.rawOperands) R.string.sumh_runtime_copy_original else R.string.sumh_runtime_copy_visible))
                    }
                    rule.target?.let { target ->
                        SelectionContainer { RuntimePath(R.string.sumh_runtime_source_path, target, expanded = true) }
                        TextButton(onClick = { copyPath(target) }) { Text(stringResource(R.string.sumh_runtime_copy_source)) }
                    }
                }
                if (rule.rawOperands) Text(stringResource(R.string.sumh_rules_runtime_raw_operands))
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.close)) } },
    )
}
