package ui.screen.moreSettings.component

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.ViewAgenda
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.activity.component.NavigationBarStyle
import com.zying.zysu.ui.activity.component.NavigationBarStylePreferences
import com.zying.zysu.ui.activity.component.rememberNavigationBarStyle

@Composable
internal fun NavigationStyleSetting() {
    val context = LocalContext.current
    val selected by rememberNavigationBarStyle()
    SettingsCard(title = stringResource(R.string.nav_style_title), icon = Icons.Default.ViewAgenda) {
        Column(Modifier.selectableGroup()) {
            NavigationBarStyle.entries.forEach { style ->
                val active = selected == style
                Row(Modifier.fillMaxWidth()
                    .selectable(active, role = Role.RadioButton,
                        onClick = { NavigationBarStylePreferences.set(context, style) })
                    .padding(horizontal = 16.dp, vertical = 16.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                    SettingsIconBadge(if (style == NavigationBarStyle.Glass) Icons.Default.Home else Icons.Default.ViewAgenda,
                        selected = active)
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Text(stringResource(if (style == NavigationBarStyle.Glass) R.string.nav_style_glass else R.string.nav_style_material),
                            style = MaterialTheme.typography.bodyLarge)
                        Text(stringResource(if (style == NavigationBarStyle.Glass) R.string.nav_style_glass_summary else R.string.nav_style_material_summary),
                            style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    RadioButton(selected = active, onClick = null)
                }
            }
        }
        Text(stringResource(R.string.nav_style_summary),
            modifier = Modifier.padding(start = 20.dp, end = 20.dp, bottom = 16.dp),
            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}
