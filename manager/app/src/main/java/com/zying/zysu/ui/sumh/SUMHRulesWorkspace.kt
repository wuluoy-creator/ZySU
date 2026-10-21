package com.zying.zysu.ui.sumh

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter as Controller
import com.zying.zysu.ui.util.SnackbarController

@Composable
internal fun SUMHRulesWorkspace(
    rules: List<Controller.ActiveRule>, status: Controller.SUMHStatus, enabled: Boolean, runtimeEnabled: Boolean,
    saving: Boolean, snackbar: SnackbarController, onSavingChanged: (Boolean) -> Unit,
    onRefresh: () -> Unit, onClearAll: () -> Unit,
    rulesIncomplete: Boolean = false,
) {
    var runtime by rememberSaveable { mutableStateOf(false) }
    val pages = rememberSaveableStateHolder()
    Column(Modifier.fillMaxSize()) {
        FlowRow(Modifier.fillMaxWidth().padding(horizontal = 16.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FilterChip(selected = !runtime, onClick = { runtime = false }, enabled = !saving,
                label = { Text(stringResource(R.string.sumh_rules_personal)) })
            FilterChip(selected = runtime, onClick = { runtime = true }, enabled = !saving,
                label = { Text(stringResource(R.string.sumh_rules_runtime)) })
        }
        Box(Modifier.weight(1f)) {
            pages.SaveableStateProvider(runtime) {
                if (runtime) SUMHRuntimeInventoryWorkspace(rules, status, runtimeEnabled, onRefresh, onClearAll)
                else SUMHUserHideWorkspace(enabled, snackbar, onSavingChanged, status == Controller.SUMHStatus.AVAILABLE)
            }
        }
    }
}
