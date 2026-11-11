package com.zying.zysu.ui.screen

import android.content.ActivityNotFoundException
import android.content.ClipData
import android.content.ClipboardManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.LauncherApps
import android.net.Uri
import android.os.Process
import android.os.UserHandle
import android.provider.Settings
import android.widget.Toast
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Security
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import coil.compose.AsyncImage
import coil.request.ImageRequest
import com.zying.zysu.R
import com.zying.zysu.Natives
import com.zying.zysu.ui.component.SwitchItem
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.theme.getCardBorder
import com.zying.zysu.ui.viewmodel.SuperUserViewModel
import com.zying.zysu.ui.viewmodel.AuthorizationPermissions

/**
 * Shows the permission state and app-management actions for one UID group.
 *
 * This sheet deliberately has no permission side effects of its own. Every
 * change is sent through [onPermissionChange] so the owning screen can apply
 * it, show errors and refresh the app list in one place.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AuthorizationAppSheet(
    group: SuperUserViewModel.AppGroup,
    busy: Boolean,
    onDismiss: () -> Unit,
    onEditProfile: (SuperUserViewModel.AppInfo) -> Unit,
    onPermissionChange: (allowSu: Boolean, umountModules: Boolean?) -> Unit,
) {
    val context = LocalContext.current
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val scrollState = rememberScrollState()
    val mainApp = group.mainApp
    val isGranted = group.allowSu
    val canUmountModules = AuthorizationPermissions.canConfigureModuleUnmount(group.uid)
    val umountModules = remember(group.uid, group.profile) {
        Natives.uidShouldUmount(group.uid)
    }

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = sheetState,
        contentWindowInsets = { WindowInsets.navigationBars },
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .verticalScroll(scrollState)
                .padding(horizontal = 20.dp)
                .padding(bottom = 20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = stringResource(R.string.auth_app_title),
                    modifier = Modifier.weight(1f),
                    style = MaterialTheme.typography.titleLarge,
                )
                IconButton(onClick = onDismiss) {
                    YukiIcon(Icons.Filled.Close, stringResource(R.string.auth_app_close))
                }
            }
            AppIdentityCard(
                app = mainApp,
                uid = group.uid,
                isGranted = isGranted,
                onCopyPackage = { copyPackageName(context, mainApp.packageName) },
            )

            if (group.apps.size > 1) {
                SharedUidWarning()
            }

            Card(
                modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                colors = CardDefaults.cardColors(
                    containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
                ),
                border = getCardBorder(),
            ) {
                Column {
                    SwitchItem(
                        icon = Icons.Filled.Security,
                        title = stringResource(R.string.auth_app_umount_modules),
                        summary = stringResource(
                            if (!canUmountModules) R.string.auth_app_umount_unavailable
                            else if (isGranted) R.string.auth_app_umount_revokes_root
                            else R.string.auth_app_umount_summary,
                        ),
                        checked = umountModules,
                        enabled = !busy && canUmountModules,
                        onCheckedChange = { checked ->
                            // Module isolation is available to non-root apps.
                            onPermissionChange(false, checked)
                        },
                    )
                }
            }

            Button(
                onClick = { onPermissionChange(!isGranted, null) },
                enabled = !busy,
                modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                colors = if (isGranted) {
                    ButtonDefaults.buttonColors(
                        containerColor = MaterialTheme.colorScheme.error,
                        contentColor = MaterialTheme.colorScheme.onError,
                    )
                } else {
                    ButtonDefaults.buttonColors()
                },
            ) {
                if (busy) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(18.dp),
                        strokeWidth = 2.dp,
                        color = LocalContentColor.current,
                    )
                    Spacer(Modifier.width(8.dp))
                }
                Text(
                    text = stringResource(
                        if (busy) R.string.auth_app_busy
                        else if (isGranted) R.string.auth_app_revoke_root
                        else R.string.auth_app_grant_root,
                    ),
                )
            }

            OutlinedButton(
                onClick = { onEditProfile(mainApp) },
                enabled = !busy,
                modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
            ) {
                YukiIcon(Icons.Filled.Edit, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.auth_app_edit_profile))
            }

            OutlinedButton(
                onClick = { openApplicationDetails(context, mainApp) },
                enabled = !busy,
                modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
            ) {
                YukiIcon(Icons.Filled.Info, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.auth_app_system_info))
            }

            if (group.apps.size > 1) {
                SharedAppsCard(apps = group.apps)
            }
        }
    }
}

@Composable
private fun AppIdentityCard(
    app: SuperUserViewModel.AppInfo,
    uid: Int,
    isGranted: Boolean,
    onCopyPackage: () -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow),
        border = getCardBorder(),
    ) {
        Column(modifier = Modifier.padding(vertical = 8.dp)) {
            ListItem(
                colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                leadingContent = {
                    AsyncImage(
                        model = ImageRequest.Builder(LocalContext.current)
                            .data(app.packageInfo)
                            .crossfade(true)
                            .build(),
                        contentDescription = null,
                        modifier = Modifier.size(52.dp),
                    )
                },
                content = {
                    Text(
                        app.label,
                        style = MaterialTheme.typography.titleLarge,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                    )
                },
                supportingContent = {
                    Text(
                        app.packageName,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                    )
                },
            )
            Column(modifier = Modifier.padding(horizontal = 16.dp)) {
                AuthStatusChip(isGranted = isGranted)
            }
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(start = 16.dp, end = 8.dp, bottom = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = stringResource(R.string.auth_app_uid, uid),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.weight(1f),
                )
                IconButton(onClick = onCopyPackage) {
                    YukiIcon(
                        imageVector = Icons.Filled.ContentCopy,
                        contentDescription = stringResource(R.string.auth_app_copy_package),
                    )
                }
            }
        }
    }
}

@Composable
private fun AuthStatusChip(isGranted: Boolean) {
    Surface(
        shape = MaterialTheme.shapes.small,
        color = if (isGranted) MaterialTheme.colorScheme.primaryContainer
            else MaterialTheme.colorScheme.surfaceContainerHighest,
        contentColor = if (isGranted) MaterialTheme.colorScheme.onPrimaryContainer
            else MaterialTheme.colorScheme.onSurfaceVariant,
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 6.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            YukiIcon(
                imageVector = Icons.Filled.Security,
                contentDescription = null,
                modifier = Modifier.size(16.dp),
            )
            Text(
                stringResource(
                    if (isGranted) R.string.auth_app_status_granted
                    else R.string.auth_app_status_denied,
                ),
                style = MaterialTheme.typography.labelMedium,
            )
        }
    }
}

@Composable
private fun SharedUidWarning() {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.tertiaryContainer,
        ),
    ) {
        Row(
            modifier = Modifier.padding(16.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.Top,
        ) {
            YukiIcon(
                Icons.Filled.Warning,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onTertiaryContainer,
            )
            Text(
                text = stringResource(R.string.auth_app_shared_uid_warning),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onTertiaryContainer,
            )
        }
    }
}

@Composable
private fun SharedAppsCard(
    apps: List<SuperUserViewModel.AppInfo>,
) {
    val context = LocalContext.current
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow),
        border = getCardBorder(),
    ) {
        Column {
            Text(
                text = stringResource(R.string.auth_app_shared_apps),
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.padding(start = 16.dp, top = 16.dp, end = 16.dp),
            )
            apps.forEach { app ->
                ListItem(
                    colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                    leadingContent = {
                        AsyncImage(
                            model = ImageRequest.Builder(LocalContext.current).data(app.packageInfo).build(),
                            contentDescription = null,
                            modifier = Modifier.size(36.dp),
                        )
                    },
                    content = {
                        Text(app.label, maxLines = 2, overflow = TextOverflow.Ellipsis)
                    },
                    supportingContent = {
                        Text(app.packageName, maxLines = 2, overflow = TextOverflow.Ellipsis)
                    },
                    trailingContent = {
                        Column {
                            IconButton(onClick = { openApplicationDetails(context, app) }) {
                                YukiIcon(
                                    Icons.Filled.Info,
                                    contentDescription = stringResource(R.string.auth_app_system_info_for_app, app.label),
                                )
                            }
                            IconButton(onClick = { copyPackageName(context, app.packageName) }) {
                                YukiIcon(
                                    Icons.Filled.ContentCopy,
                                    contentDescription = stringResource(R.string.auth_app_copy_package_for_app, app.label),
                                )
                            }
                        }
                    },
                )
            }
        }
    }
}

private fun copyPackageName(context: Context, packageName: String) {
    val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager ?: return
    clipboard.setPrimaryClip(ClipData.newPlainText(packageName, packageName))
    Toast.makeText(context, context.getString(R.string.auth_app_copy_done), Toast.LENGTH_SHORT).show()
}

private fun openApplicationDetails(context: Context, app: SuperUserViewModel.AppInfo) {
    val intent = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
        data = Uri.fromParts("package", app.packageName, null)
        addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    }
    try {
        val user = UserHandle.getUserHandleForUid(app.uid)
        if (user == Process.myUserHandle()) {
            context.startActivity(intent)
        } else {
            // The package can also be installed for the current user. Ask the
            // launcher API for this exact profile, never fall back to opening
            // the same package in the wrong user's settings.
            val launcherApps = context.getSystemService(LauncherApps::class.java)
            if (launcherApps == null) {
                Toast.makeText(context, R.string.auth_app_system_info_unavailable, Toast.LENGTH_SHORT).show()
                return
            }
            launcherApps.startAppDetailsActivity(
                ComponentName(app.packageName, ""), user, null, null,
            )
        }
    } catch (_: ActivityNotFoundException) {
        Toast.makeText(context, R.string.auth_app_system_info_unavailable, Toast.LENGTH_SHORT).show()
    } catch (_: SecurityException) {
        Toast.makeText(context, R.string.auth_app_system_info_unavailable, Toast.LENGTH_SHORT).show()
    } catch (_: IllegalArgumentException) {
        Toast.makeText(context, R.string.auth_app_system_info_unavailable, Toast.LENGTH_SHORT).show()
    }
}
