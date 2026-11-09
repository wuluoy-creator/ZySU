package com.zying.zysu.ui.sumh

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.SUMHConfigBackup
import com.zying.zysu.ui.sumh.util.SUMHManager
import com.zying.zysu.ui.sumh.util.validateSUMHRestore
import com.zying.zysu.ui.sumh.util.sumhJsonEqual
import com.zying.zysu.ui.util.LocalSnackbarHost
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
internal fun SUMHConfigBackupActions(
    enabled: Boolean,
    onBusyChanged: (Boolean) -> Unit,
    onRestored: () -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val snackbar = LocalSnackbarHost.current
    val exportDone = stringResource(R.string.sumh_backup_export_done)
    val exportFailed = stringResource(R.string.sumh_backup_export_failed)
    val importFailed = stringResource(R.string.sumh_backup_import_failed)
    var menuOpen by remember { mutableStateOf(false) }
    var preview by remember { mutableStateOf<Pair<SUMHConfigBackup, SUMHManager.BackupSnapshot>?>(null) }
    var result by remember { mutableStateOf<SUMHManager.BackupRestoreResult?>(null) }

    val exportLauncher = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        onBusyChanged(true)
        scope.launch {
            try {
                val snapshot = SUMHManager.backupSnapshot()
                val text = SUMHConfigBackup.create(snapshot.config, snapshot.hidePaths).toJson()
                withContext(Dispatchers.IO) {
                    checkNotNull(context.contentResolver.openOutputStream(uri, "wt")) {
                        "Cannot open backup destination"
                    }.bufferedWriter(Charsets.UTF_8).use { it.write(text) }
                }
                snackbar.showSnackbar(exportDone)
            } catch (error: Exception) {
                if (error is CancellationException) throw error
                snackbar.showSnackbar("$exportFailed: ${error.message.orEmpty()}")
            } finally { onBusyChanged(false) }
        }
    }
    val importLauncher = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri == null) return@rememberLauncherForActivityResult
        onBusyChanged(true)
        scope.launch {
            try {
                val backup = withContext(Dispatchers.IO) {
                    val bytes = ByteArrayOutputStream()
                    checkNotNull(context.contentResolver.openInputStream(uri)).use { input ->
                        val buffer = ByteArray(8192)
                        while (true) {
                            val count = input.read(buffer)
                            if (count < 0) break
                            require(bytes.size() + count <= SUMHConfigBackup.MAX_BYTES) { "Backup exceeds 256 KiB" }
                            bytes.write(buffer, 0, count)
                        }
                    }
                    val text = Charsets.UTF_8.newDecoder()
                        .onMalformedInput(CodingErrorAction.REPORT)
                        .onUnmappableCharacter(CodingErrorAction.REPORT)
                        .decode(ByteBuffer.wrap(bytes.toByteArray())).toString()
                    SUMHConfigBackup.parse(text)
                }
                val current = SUMHManager.backupSnapshot()
                validateSUMHRestore(backup, current.config)
                preview = backup to current
            } catch (error: Exception) {
                if (error is CancellationException) throw error
                snackbar.showSnackbar("$importFailed: ${error.message.orEmpty()}")
            } finally { onBusyChanged(false) }
        }
    }

    IconButton(onClick = { menuOpen = true }, enabled = enabled) {
        Icon(Icons.Filled.MoreVert, contentDescription = stringResource(R.string.sumh_backup_menu))
    }
    DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
        DropdownMenuItem(text = { Text(stringResource(R.string.sumh_backup_export)) }, onClick = {
            menuOpen = false
            exportLauncher.launch("sumh-backup.json")
        })
        DropdownMenuItem(text = { Text(stringResource(R.string.sumh_backup_import)) }, onClick = {
            menuOpen = false
            importLauncher.launch(arrayOf("application/json", "text/plain"))
        })
    }

    preview?.let { (backup, current) ->
        val existing = current.hidePaths.toSet()
        val desired = backup.hidePaths.toSet()
        val changedFields = backup.config.keys().asSequence().filter { key ->
            !current.config.has(key) || !sumhJsonEqual(current.config.get(key), backup.config.get(key))
        }.sorted().toList()
        val addedPaths = backup.hidePaths.filterNot { it in existing }
        val removedPaths = current.hidePaths.filterNot { it in desired }
        AlertDialog(
            onDismissRequest = { preview = null },
            title = { Text(stringResource(R.string.sumh_backup_preview_title)) },
            text = {
                Column(Modifier.heightIn(max = 400.dp).verticalScroll(rememberScrollState())) {
                    Text(stringResource(
                        R.string.sumh_backup_preview_body,
                        changedFields.size,
                        addedPaths.size,
                        removedPaths.size,
                    ) + "\n\n" + stringResource(R.string.sumh_backup_scope_note))
                    if (changedFields.isNotEmpty()) Text("\n" + stringResource(R.string.sumh_backup_changed_fields) +
                        "\n" + changedFields.joinToString("\n"))
                    if (addedPaths.isNotEmpty()) Text("\n" + stringResource(R.string.sumh_backup_paths_add) +
                        "\n" + addedPaths.joinToString("\n") { it.take(200) })
                    if (removedPaths.isNotEmpty()) Text("\n" + stringResource(R.string.sumh_backup_paths_remove) +
                        "\n" + removedPaths.joinToString("\n") { it.take(200) })
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    preview = null
                    onBusyChanged(true)
                    scope.launch {
                        try {
                            result = SUMHManager.restoreBackup(backup, current)
                            onRestored()
                        } catch (error: Exception) {
                            if (error is CancellationException) throw error
                            snackbar.showSnackbar("$importFailed: ${error.message.orEmpty()}")
                        } finally { onBusyChanged(false) }
                    }
                }) { Text(stringResource(R.string.sumh_backup_restore)) }
            },
            dismissButton = {
                TextButton(onClick = { preview = null }) { Text(stringResource(R.string.cancel)) }
            },
        )
    }
    result?.let { outcome ->
        AlertDialog(
            onDismissRequest = { result = null },
            title = { Text(stringResource(R.string.sumh_backup_result_title)) },
            text = {
                Column(Modifier.heightIn(max = 400.dp).verticalScroll(rememberScrollState())) {
                    Text(stringResource(
                        R.string.sumh_backup_result_body,
                        if (outcome.configPersisted) stringResource(R.string.sumh_backup_yes) else stringResource(R.string.sumh_backup_no),
                        outcome.addedPaths,
                        outcome.removedPaths,
                    ) + (if (outcome.rebootRequired) "\n" + stringResource(R.string.sumh_backup_reboot) else "") +
                        (if (outcome.runtimePending) "\n" + stringResource(R.string.sumh_backup_runtime_pending) else "") +
                        (if (outcome.errors.isNotEmpty()) "\n\n" + outcome.errors.take(8).joinToString("\n") {
                            if (it.length > 200) it.take(200) + "..." else it
                        } +
                            (if (outcome.errors.size > 8) "\n+${outcome.errors.size - 8}" else "") else ""))
                }
            },
            confirmButton = {
                TextButton(onClick = { result = null }) { Text(stringResource(R.string.sumh_backup_close)) }
            },
        )
    }
}
