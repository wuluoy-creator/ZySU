package ui.screen.moreSettings.component

import android.content.Context
import android.content.SharedPreferences
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Visibility
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.component.YukiAlertDialog
import com.zying.zysu.ui.component.YukiSwitch
import com.zying.zysu.ui.screen.home.HomeFactField
import com.zying.zysu.ui.screen.home.HomeFactVisibility

@Composable
fun HomeFactVisibilitySetting() {
    val context = LocalContext.current
    val preferences = remember(context) { context.getSharedPreferences("settings", Context.MODE_PRIVATE) }
    var visible by remember(preferences) { mutableStateOf(HomeFactVisibility.read(preferences)) }
    var expanded by rememberSaveable { mutableStateOf(false) }
    DisposableEffect(preferences) {
        val listener = SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
            if (HomeFactVisibility.observes(key)) visible = HomeFactVisibility.read(preferences)
        }
        preferences.registerOnSharedPreferenceChangeListener(listener)
        visible = HomeFactVisibility.read(preferences)
        onDispose { preferences.unregisterOnSharedPreferenceChangeListener(listener) }
    }
    SettingItem(
        icon = Icons.Outlined.Visibility,
        title = stringResource(R.string.home_fact_visibility_title),
        subtitle = stringResource(R.string.home_fact_visibility_count, visible.size, HomeFactField.entries.size),
        onClick = { expanded = true },
    )
    if (expanded) YukiAlertDialog(
        onDismissRequest = { expanded = false },
        title = { Text(stringResource(R.string.home_fact_visibility_title)) },
        text = {
            Column(Modifier.fillMaxWidth().heightIn(max = 480.dp).verticalScroll(rememberScrollState())) {
                Column(Modifier.padding(horizontal = 16.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(stringResource(R.string.home_fact_visibility_summary), style = MaterialTheme.typography.bodyMedium)
                    Text(stringResource(R.string.home_fact_visibility_availability),
                        style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                HomeFactField.entries.forEach { field ->
                    val checked = field.id in visible
                    Row(
                        Modifier.fillMaxWidth().heightIn(min = 56.dp)
                            .toggleable(checked, role = Role.Switch) { selected ->
                                HomeFactVisibility.setVisible(preferences, field, selected)
                                visible = HomeFactVisibility.read(preferences)
                            }.padding(horizontal = 8.dp, vertical = 8.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(16.dp),
                    ) {
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text(stringResource(field.label), style = MaterialTheme.typography.bodyLarge)
                            if (field.buildDetail) Text(stringResource(R.string.home_fact_visibility_build_detail),
                                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        YukiSwitch(checked = checked, onCheckedChange = null)
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = { expanded = false }) { Text(stringResource(android.R.string.ok)) }
        },
    )
}
