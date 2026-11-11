package com.zying.zysu.ui.util

import android.content.ContentResolver
import android.content.Context
import android.database.Cursor
import android.media.MediaScannerConnection
import android.net.Uri
import android.os.Environment
import android.os.Parcelable
import android.os.SystemClock
import android.provider.OpenableColumns
import androidx.core.net.toUri
import android.util.Log
import com.topjohnwu.superuser.CallbackList
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ShellUtils
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.parcelize.Parcelize
import com.zying.zysu.BuildConfig
import com.zying.zysu.integrity.KsudIntegrity
import com.zying.zysu.integrity.KsudIntegrityStatus
import com.zying.zysu.Natives
import com.zying.zysu.core.tasks.ExtractImage
import com.zying.zysu.core.tasks.ProbeResult
import com.zying.zysu.core.utils.DataSourceChannel
import com.zying.zysu.ksu.KsuPaths
import com.zying.zysu.ksuApp
import com.topjohnwu.superuser.io.SuFile
import org.json.JSONArray
import okhttp3.OkHttpClient
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.charset.StandardCharsets
import java.util.Properties
import java.util.UUID
import java.util.concurrent.TimeUnit


/**
 * @author weishu
 * @date 2023/1/1.
 */
private const val TAG = "KsuCli"

private fun getKsuDaemonPath(): String {
    return File(ksuApp.applicationInfo.nativeLibraryDir, BuildConfig.KSUD_LIBRARY_NAME).absolutePath
}

/**
 * Public function to get ksud path for other modules
 */
fun getKsud(): String = getKsuDaemonPath()

object KsuCli {
    var SHELL: Shell = createRootShell()
        private set
    var GLOBAL_MNT_SHELL: Shell = createRootShell(true)
        private set
    
    /** Recreate shell instances and optionally reconcile the installed ksud. */
    fun refreshShells(checkKsud: Boolean = true) {
        Log.d(TAG, "refreshShells: starting, old SHELL.isRoot=${SHELL.isRoot}")
        try {
            SHELL.close()
        } catch (_: Exception) {}
        try {
            GLOBAL_MNT_SHELL.close()
        } catch (_: Exception) {}
        
        // Check if we're now a manager before creating shells
        val isManagerNow = try {
            Natives.isManager
        } catch (e: Exception) {
            Log.e(TAG, "refreshShells: failed to check isManager", e)
            false
        }
        Log.d(TAG, "refreshShells: Natives.isManager=$isManagerNow")
        
        SHELL = createRootShell()
        GLOBAL_MNT_SHELL = createRootShell(true)
        Log.d(TAG, "Shells refreshed, SHELL.isRoot=${SHELL.isRoot}, GLOBAL_MNT_SHELL.isRoot=${GLOBAL_MNT_SHELL.isRoot}")
        
        // After authentication, check if ksud needs to be installed/updated
        if (checkKsud && isManagerNow) {
            checkAndInstallKsud()
        }
    }
    
    /**
     * Check if ksud needs to be installed or updated.
     * Called after SuperKey authentication succeeds.
     */
    private fun checkAndInstallKsud() {
        try {
            if (!isBundledKsudUapiCompatible()) return
            val apkKsudVersion = getApkKsudVersion()
            val integrityStatus = getKsudIntegrityStatus()
            if (integrityStatus == KsudIntegrityStatus.UNAVAILABLE) {
                Log.w(TAG, "checkAndInstallKsud: JNI manager root is unavailable")
                return
            }
            val binaryMatches = integrityStatus == KsudIntegrityStatus.MATCH
            val installedKsudVersion = if (binaryMatches) apkKsudVersion else null
            
            Log.i(
                TAG,
                "checkAndInstallKsud: apk=$apkKsudVersion, installed=$installedKsudVersion, " +
                    "binaryMatches=$binaryMatches"
            )
            
            // Version-only checks miss local/reproducible builds made from the same
            // commit. Compare the actual ELF so the installed daemon always carries
            // the exact assets and fixes bundled by this APK.
            if (!binaryMatches) {
                Log.i(
                    TAG,
                    "Installing/updating ksud daemon: apk=$apkKsudVersion, " +
                        "installed=$installedKsudVersion, binaryMatches=$binaryMatches"
                )
                installOrUpdateKsudDaemon()
            } else {
                Log.d(TAG, "ksud is up-to-date: $installedKsudVersion")
                Natives.ensureKsudToolLinks()
            }
        } catch (e: Exception) {
            Log.e(TAG, "checkAndInstallKsud failed, falling back to install", e)
            // Fallback: always try to sync ksud daemon on error
            installOrUpdateKsudDaemon()
        }
    }
    
    /**
     * The APK-bundled ksud version is pinned at build time by
     * manager/build.gradle.kts (`computeKsudBundledVersion`), which mirrors
     * userspace/ksud/scripts/generate_version.py. No need to fork-exec the
     * daemon just to read its version.
     */
    private fun getApkKsudVersion(): String? =
        BuildConfig.KSUD_BUNDLED_VERSION.takeIf { it.isNotBlank() }

    /**
     * Normalize ksud version string to app-style display: vx.x.x-xxxxxxxx[-nc].
     * e.g. "1.3.0-1-g56b0efb0-nc" -> "v1.3.0-56b0efb0-nc"
     */
    fun formatKsudVersionForDisplay(raw: String?): String? {
        if (raw.isNullOrBlank()) return null
        val s = raw.trim().removePrefix("v")
        // Match x.x.x optionally followed by a describe hash and the dirty marker.
        val semverMatch = Regex("""^(\d+\.\d+\.\d+)""").find(s) ?: return "v$s"
        val semver = semverMatch.value
        val rest = s.drop(semver.length).trimStart('-')
        val isDirty = Regex("""(?:^|-)nc$""").containsMatchIn(rest)
        val versionRest = when {
            rest == "nc" -> ""
            isDirty -> rest.removeSuffix("-nc")
            else -> rest
        }
        val describeHash = Regex("""(?:^|-)g([a-fA-F0-9]{7,40})""")
            .find(versionRest)
            ?.groupValues
            ?.get(1)
        val hashPart = describeHash
            ?: Regex("""[a-fA-F0-9]{7,40}""").find(versionRest)?.value
        val suffix = when {
            hashPart != null -> hashPart.take(8)
            versionRest.isNotEmpty() -> versionRest.filter { it.isLetterOrDigit() }.take(8)
            else -> ""
        }
        return buildString {
            append("v$semver")
            if (suffix.isNotEmpty()) append("-$suffix")
            if (isDirty) append("-nc")
        }
    }

    fun getKsudIntegrityStatus(): KsudIntegrityStatus {
        return KsudIntegrity.verifyBundledDaemon(ksuApp)
    }

    private fun isInstalledKsudBinaryCurrent(): Boolean =
        getKsudIntegrityStatus() == KsudIntegrityStatus.MATCH

    /**
     * Public helper for UI: get ksud versions (APK-bundled and installed daemon),
     * formatted as vx.x.x-xxxxxxxx[-nc]. Returns (formattedApk, formattedInstalled).
     */
    suspend fun getKsudVersionsForUi(): Pair<String?, String?> = withContext(Dispatchers.IO) {
        val apk = getApkKsudVersion()
        val installed = if (isInstalledKsudBinaryCurrent()) apk else null
        formatKsudVersionForDisplay(apk) to formatKsudVersionForDisplay(installed)
    }

    /**
     * Public helper for UI: sync ksud daemon binary from APK into /data/adb/ksud.
     */
    suspend fun updateKsudDaemonForUi(): Boolean = withContext(Dispatchers.IO) {
        val updated = installOrUpdateKsudDaemon()
        if (updated) {
            refreshShells(checkKsud = false)
        }
        updated
    }

    /** Cold-start auto-sync through the JNI manager-root channel. */
    suspend fun autoSyncKsudIfNeeded(): Unit = withContext(Dispatchers.IO) {
        if (!runCatching { Natives.isManager }.getOrDefault(false)) {
            Log.d(TAG, "autoSyncKsudIfNeeded: not manager, skip")
            return@withContext
        }
        checkAndInstallKsud()
    }

    /**
     * Install or update the ksud daemon binary itself, without touching boot image.
     *
     * Atomically install the manager-bundled ksud ELF at `/data/adb/ksud`,
     * then keep the convenience applets under `/data/adb/ksu/bin` as symlinks.
     */
    private fun installOrUpdateKsudDaemon(): Boolean {
        if (!isBundledKsudUapiCompatible()) return false
        val nativeDir = ksuApp.applicationInfo.nativeLibraryDir
        val ksudSo = File(nativeDir, BuildConfig.KSUD_LIBRARY_NAME)
        if (!ksudSo.exists()) {
            Log.e(TAG, "installOrUpdateKsudDaemon: ${BuildConfig.KSUD_LIBRARY_NAME} not found in $nativeDir")
            return false
        }

        Log.i(TAG, "installOrUpdateKsudDaemon: syncing ${ksudSo.absolutePath} -> /data/adb/ksud")
        val result = runCatching { Natives.installKsudDaemon(ksudSo.absolutePath) }
            .onFailure { Log.e(TAG, "installOrUpdateKsudDaemon: JNI install failed", it) }
            .getOrDefault(false)
        Log.i(TAG, "installOrUpdateKsudDaemon: isSuccess=$result")

        if (result) {
            KsudIntegrity.markBundledDaemonInstalled(ksuApp)
        }
        return result
    }

    private fun isBundledKsudUapiCompatible(): Boolean {
        val kernelUapi = runCatching { Natives.getUapiVersion() }.getOrDefault(0)
        val bundledUapi = runCatching { Natives.getManagerUapiVersion() }.getOrDefault(0)
        val compatible = kernelUapi > 0 && kernelUapi == bundledUapi
        if (!compatible) {
            Log.e(
                TAG,
                "Refusing to sync ksud with mismatched UAPI: " +
                    "kernel=$kernelUapi bundled=$bundledUapi"
            )
        }
        return compatible
    }
}

fun getRootShell(globalMnt: Boolean = false): Shell {
    return if (globalMnt) KsuCli.GLOBAL_MNT_SHELL else {
        KsuCli.SHELL
    }
}

inline fun <T> withNewRootShell(
    globalMnt: Boolean = false,
    block: Shell.() -> T
): T {
    return createRootShell(globalMnt).use(block)
}

fun Uri.getFileName(context: Context): String? {
    var fileName: String? = null
    val contentResolver: ContentResolver = context.contentResolver
    val cursor: Cursor? = contentResolver.query(this, null, null, null, null)
    cursor?.use {
        if (it.moveToFirst()) {
            fileName = it.getString(it.getColumnIndexOrThrow(OpenableColumns.DISPLAY_NAME))
        }
    }
    return fileName
}

fun createRootShell(globalMnt: Boolean = false): Shell {
    Shell.enableVerboseLogging = BuildConfig.DEBUG
    val builder = Shell.Builder.create()
    return try {
        val shell = if (globalMnt) {
            builder.build(getKsuDaemonPath(), "debug", "su", "-g")
        } else {
            builder.build(getKsuDaemonPath(), "debug", "su")
        }
        Log.d(TAG, "ksud shell created, isRoot=${shell.isRoot}, globalMnt=$globalMnt")
        shell
    } catch (e: Throwable) {
        Log.w(TAG, "ksu failed (globalMnt=$globalMnt): ", e)
        try {
            val shell = if (globalMnt) {
                builder.build("su", "-mm")
            } else {
                builder.build("su")
            }
            Log.d(TAG, "su shell created, isRoot=${shell.isRoot}, globalMnt=$globalMnt")
            shell
        } catch (e: Throwable) {
            Log.e(TAG, "su failed (globalMnt=$globalMnt): ", e)
            val shell = builder.build("sh")
            Log.w(TAG, "fallback to sh, isRoot=${shell.isRoot}")
            shell
        }
    }
}

/** Build a "ksud <args>" command string. Use this instead of pasting
 *  `${getKsuDaemonPath()}` next to a subcommand literal. */
internal fun ksudCmd(args: String): String = "${shellArg(getKsuDaemonPath())} $args"

fun execKsud(args: String, newShell: Boolean = false): Boolean {
    return if (newShell) {
        withNewRootShell {
            ShellUtils.fastCmdResult(this, ksudCmd(args))
        }
    } else {
        ShellUtils.fastCmdResult(getRootShell(), ksudCmd(args))
    }
}

/** Run a ksud subcommand and return its trimmed stdout (single value). */
internal fun ksudReadString(args: String, shell: Shell = getRootShell()): String =
    ShellUtils.fastCmd(shell, ksudCmd(args)).trim()

/** Run a ksud subcommand and return non-blank trimmed stdout lines. */
internal fun ksudReadLines(args: String, shell: Shell = getRootShell()): List<String> =
    shell.newJob().add(ksudCmd(args)).to(ArrayList(), null).exec().out
        .filter { it.isNotBlank() }.map { it.trim() }

/** Boot overrides are immutable until the image is patched again. */
fun getFeatureStatus(feature: Int): String = when {
    !Natives.isFeatureSupported(feature) -> "unsupported"
    Natives.isFeatureForced(feature) -> "forced"
    else -> "supported"
}

internal fun getSuggestedBlockModules(): String = suggestedBlockModules(
    ShellUtils.fastCmd(getRootShell(), "cat /proc/modules")
)

/** A feature's on/off state. Unsupported reads as off. */
fun getFeatureValue(feature: Int): Boolean = Natives.isFeatureEnabled(feature)

/** Like [getFeatureValue], but distinguishes "off" from "the kernel has never
 *  heard of this". */
internal fun getFeatureValueOrNull(feature: Int): Boolean? =
    Natives.getFeature(feature).takeIf { it >= 0 }?.let { it > 0 }

/** Atomically set and persist a mutable feature value. ksud restores the previous
 *  runtime state if persistence fails. SUMH and the su mode are kernel-owned. */
suspend fun setFeatureValue(feature: String, enabled: Boolean): Boolean =
    withContext(Dispatchers.IO) {
        if (feature in setOf("sumh", "su_compat", "sumh_sucompat", "selinux_hide_status", "unicode_path_guard_status")) {
            return@withContext false
        }
        execKsud(
            "feature set-save ${shellArg(feature)} ${if (enabled) 1 else 0}",
            true
        )
    }

fun install() {
    val start = SystemClock.elapsedRealtime()
    val ksudPath = getKsuDaemonPath()
    val libadbrootPath =
        ksuApp.applicationInfo.nativeLibraryDir + File.separator + "libadbroot.so"
    // No --magiskboot: it would copy this whole ELF over
    // /data/adb/ksu/bin/magiskboot, replacing the native repair path's symlink.
    // Both dispatch correctly, but the symlink is the one we keep in sync.
    Log.i(TAG, "install: ksud=$ksudPath")
    val result = execKsud("install --libadbroot ${shellArg(libadbrootPath)}", true)
    Log.w(TAG, "install result: $result, cost: ${SystemClock.elapsedRealtime() - start}ms")
}

fun hasMetaModule(): Boolean {
    return getMetaModuleImplement() != "None"
}

fun listModules(): String =
    ksudReadLines("module list").joinToString("\n").ifBlank { "[]" }

fun getModuleCount(): Int {
    val result = listModules()
    runCatching {
        val array = JSONArray(result)
        return array.length()
    }.getOrElse { return 0 }
}

fun getSuperuserCount(): Int {
    return Natives.getSuperuserCount()
}

fun toggleModule(id: String, enable: Boolean): Boolean {
    val cmd = if (enable) {
        "module enable ${shellArg(id)}"
    } else {
        "module disable ${shellArg(id)}"
    }
    val result = execKsud(cmd, true)
    Log.i(TAG, "$cmd result: $result")
    return result
}

fun uninstallModule(id: String): Boolean {
    val cmd = "module uninstall ${shellArg(id)}"
    val result = execKsud(cmd, true)
    Log.i(TAG, "uninstall module $id result: $result")
    return result
}

fun restoreModule(id: String): Boolean {
    val cmd = "module restore ${shellArg(id)}"
    val result = execKsud(cmd, true)
    Log.i(TAG, "restore module $id result: $result")
    return result
}

fun undoUninstallModule(id: String): Boolean {
    val cmd = "module undo-uninstall ${shellArg(id)}"
    val result = execKsud(cmd, true)
    Log.i(TAG, "undo uninstall module $id result: $result")
    return result
}

private fun flashWithIO(
    cmd: String,
    onStdout: (String) -> Unit,
    onStderr: (String) -> Unit
): Shell.Result {

    val stdoutCallback: CallbackList<String?> = object : CallbackList<String?>() {
        override fun onAddElement(s: String?) {
            onStdout(s ?: "")
        }
    }

    val stderrCallback: CallbackList<String?> = object : CallbackList<String?>() {
        override fun onAddElement(s: String?) {
            onStderr(s ?: "")
        }
    }

    // Set TMPDIR to app cache directory so ksud can create temp files without root
    val tmpDir = ksuApp.cacheDir.absolutePath
    val cmdWithEnv = "TMPDIR=${shellArg(tmpDir)} $cmd"

    return withNewRootShell {
        newJob().add(cmdWithEnv).to(stdoutCallback, stderrCallback).exec()
    }
}

fun flashAnyKernel3(
    zipPath: String,
    targetSlot: String?,
    useMkbootfs: Boolean,
    onFinish: (Boolean, Int) -> Unit,
    onStdout: (String) -> Unit,
    onStderr: (String) -> Unit,
): Boolean {
    var success = false
    var exitCode = 1
    try {
        val command = buildString {
            append("flash ak3 ")
            append(shellArg(zipPath))
            targetSlot?.let {
                append(" --slot ")
                append(shellArg(it))
            }
            if (useMkbootfs) {
                append(" --use-mkbootfs")
            }
        }
        val result = flashWithIO(
            ksudCmd(command),
            onStdout = { output -> output.lineSequence().forEach(onStdout) },
            onStderr = { output -> output.lineSequence().forEach(onStderr) },
        )
        success = result.isSuccess
        exitCode = result.code
        Log.i(TAG, "AnyKernel3 flash result: success=$success, code=$exitCode")
    } catch (error: Exception) {
        Log.e(TAG, "Failed to flash AnyKernel3 package", error)
        onStderr(error.message ?: "Unknown AnyKernel3 error")
    } finally {
        File(zipPath).delete()
        onFinish(success, exitCode)
    }
    return success
}

fun flashModule(
    uri: Uri,
    onFinish: (Boolean, Int) -> Unit,
    onStdout: (String) -> Unit,
    onStderr: (String) -> Unit
): Boolean {
    var file: File? = null
    var success = false
    var exitCode = 1
    try {
        val stagedFile = copyPatchInputToTemporaryFile(ksuApp.cacheDir, "module-", ".zip") {
            ksuApp.contentResolver.openInputStream(uri)
        }
        file = stagedFile
        val cmd = "module install ${shellArg(stagedFile.absolutePath)}"
        val result = flashWithIO(ksudCmd(cmd), onStdout, onStderr)
        success = result.isSuccess
        exitCode = if (success) 0 else result.code.takeIf { it != 0 } ?: 1
        Log.i(TAG, "install module $uri result: $result")
    } catch (error: Exception) {
        Log.e(TAG, "Failed to install module $uri", error)
        onStderr("! Unable to install module: ${error.message ?: "Unknown error"}")
    } finally {
        file?.delete()
        onFinish(success, exitCode)
    }
    return success
}

fun runModuleAction(
    moduleId: String, onStdout: (String) -> Unit, onStderr: (String) -> Unit
): Boolean {
    val stdoutCallback: CallbackList<String?> = object : CallbackList<String?>() {
        override fun onAddElement(s: String?) {
            onStdout(s ?: "")
        }
    }

    val stderrCallback: CallbackList<String?> = object : CallbackList<String?>() {
        override fun onAddElement(s: String?) {
            onStderr(s ?: "")
        }
    }

    val result = withNewRootShell(true) {
        newJob().add(ksudCmd("module action ${shellArg(moduleId)}"))
            .to(stdoutCallback, stderrCallback).exec()
    }
    Log.i("KernelSU", "Module runAction result: $result")

    return result.isSuccess
}

fun restoreBoot(
    onFinish: (Boolean, Int) -> Unit, onStdout: (String) -> Unit, onStderr: (String) -> Unit
): Boolean {
    val result = flashWithIO(ksudCmd("boot-restore -f"), onStdout, onStderr)
    onFinish(result.isSuccess, result.code)
    return result.isSuccess
}

fun uninstallPermanently(
    onFinish: (Boolean, Int) -> Unit, onStdout: (String) -> Unit, onStderr: (String) -> Unit
): Boolean {
    val result = flashWithIO(ksudCmd("uninstall"), onStdout, onStderr)
    onFinish(result.isSuccess, result.code)
    return result.isSuccess
}

@Parcelize
sealed class LkmSelection : Parcelable {
    data class LkmUri(val uri: Uri) : LkmSelection()
    data object KmiNone : LkmSelection()
}

private fun patchBootImage(
    bootFile: File?,
    lkm: LkmSelection,
    targetKmi: String,
    ota: Boolean,
    partition: String?,
    allowShell: Boolean,
    enableAdb: Boolean,
    forceBackup: Boolean,
    superKey: String?,
    signatureBypass: Boolean,
    directInstall: Boolean,
    onFinish: (Boolean, Int) -> Unit,
    onStdout: (String) -> Unit,
    onStderr: (String) -> Unit,
    blockModules: String = "",
): Boolean {
    var lkmFile: File? = null
    val result = try {
        if (!targetKmi.matches(kmiPattern)) {
            onStderr("Invalid target KMI: $targetKmi")
            onFinish(false, 1)
            return false
        }
        val blocked = requireNotNull(normalizeBlockModules(blockModules)) { "Invalid kernel module block list" }
        var cmd = "boot-patch"

        cmd += if (bootFile == null) {
            // no boot.img, use -f to force install
            " -f"
        } else {
            " -b ${shellArg(bootFile.absolutePath)}"
        }

        if (ota) {
            cmd += " -u"
        }

        if (forceBackup) {
            cmd += " --backup"
        }

        // Add superkey if specified
        if (!superKey.isNullOrEmpty()) {
            cmd += " --superkey=${shellArg(superKey)}"
            // Add signature bypass flag if enabled
            if (signatureBypass) {
                cmd += " --signature-bypass"
            }
        }

        when (lkm) {
            is LkmSelection.LkmUri -> {
                lkmFile = copyUriToTemporaryFile(lkm.uri, "lkm-", ".ko")
                cmd += " -m ${shellArg(lkmFile.absolutePath)}"
            }

            LkmSelection.KmiNone -> Unit
        }
        cmd += " --kmi ${shellArg(targetKmi)}"

        // output dir
        val downloadsDir =
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
        cmd += " -o ${shellArg(downloadsDir.absolutePath)}"

        partition?.let { part ->
            cmd += " --partition ${shellArg(part)}"
        }

        if (allowShell) {
            cmd += " --allow-shell"
        }

        if (enableAdb) {
            cmd += " --enable-adbd"
        }

        if (blocked.isNotEmpty()) cmd += " --block-module ${shellArg(blocked)}"
        flashWithIO(ksudCmd(cmd), onStdout, onStderr)
    } catch (error: Exception) {
        onStderr(error.message ?: "Failed to patch boot image")
        onFinish(false, 1)
        return false
    } finally {
        bootFile?.delete()
        lkmFile?.delete()
    }
    Log.i("KernelSU", "install boot result: ${result.isSuccess}")
    // if boot uri is empty, it is direct install, when success, we should show reboot button
    onFinish(directInstall && result.isSuccess, result.code)

    if (directInstall && result.isSuccess) {
        install()
    }

    return result.isSuccess
}

private fun copyUriToTemporaryFile(uri: Uri, prefix: String, suffix: String): File {
    return copyPatchInputToTemporaryFile(ksuApp.cacheDir, prefix, suffix) {
        ksuApp.contentResolver.openInputStream(uri)
    }
}

fun patchBootImageV2(
    bootUri: Uri?,
    lkm: LkmSelection,
    ota: Boolean,
    allowShell: Boolean = false,
    enableAdb: Boolean = false,
    superKey: String? = null,
    signatureBypass: Boolean = false,
    onFinish: (Boolean, Int) -> Unit,
    onStdout: (String) -> Unit,
    onStderr: (String) -> Unit,
    blockModules: String = "",
): Boolean {
    var bootFile: File? = null
    var lkmFile: File? = null
    try {
        bootFile = bootUri?.let { copyUriToTemporaryFile(it, "boot-v2-", ".img") }
        if (lkm is LkmSelection.LkmUri) {
            lkmFile = copyUriToTemporaryFile(lkm.uri, "lkm-v2-", ".ko")
        }

        val inputFile = bootFile
        val outputFile = inputFile?.let {
            val outputDirectory =
                Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
            File(
                outputDirectory,
                "kernelsu_patched_v2_${System.currentTimeMillis()}_" +
                    "${UUID.randomUUID().toString().take(8)}.img",
            )
        }
        val blocked = requireNotNull(normalizeBlockModules(blockModules)) { "Invalid kernel module block list" }
        val command = buildString {
            append("boot-patch-v2")
            if (blocked.isNotEmpty()) append(" --block-module ${shellArg(blocked)}")
            if (inputFile != null && outputFile != null) {
                append(" --boot ")
                append(shellArg(inputFile.absolutePath))
                append(" --output ")
                append(shellArg(outputFile.absolutePath))
            } else {
                append(" --flash")
                if (ota) {
                    append(" --ota")
                }
            }
            lkmFile?.let { module ->
                append(" --module ")
                append(shellArg(module.absolutePath))
            }
            if (!superKey.isNullOrEmpty()) {
                append(" --superkey=")
                append(shellArg(superKey))
                if (signatureBypass) {
                    append(" --signature-bypass")
                }
            }
            if (allowShell) {
                append(" --allow-shell")
            }
            if (enableAdb) {
                append(" --enable-adbd")
            }
        }

        val result = flashWithIO(ksudCmd(command), onStdout, onStderr)
        if (result.isSuccess && outputFile != null) {
            MediaScannerConnection.scanFile(
                ksuApp,
                arrayOf(outputFile.absolutePath),
                arrayOf("application/octet-stream"),
                null,
            )
        }
        onFinish(inputFile == null && result.isSuccess, result.code)
        return result.isSuccess
    } catch (error: Exception) {
        onStderr(error.message ?: "Failed to prepare boot-patch-v2 input")
        onFinish(false, 1)
        return false
    } finally {
        bootFile?.delete()
        lkmFile?.delete()
    }
}

fun installBoot(
    bootUri: Uri?,
    lkm: LkmSelection,
    targetKmi: String,
    ota: Boolean,
    partition: String?,
    allowShell: Boolean = false,
    enableAdb: Boolean = false,
    forceBackup: Boolean = false,
    superKey: String? = null,
    signatureBypass: Boolean = false,
    embedLkmInBoot: Boolean = false,
    onFinish: (Boolean, Int) -> Unit,
    onStdout: (String) -> Unit,
    onStderr: (String) -> Unit,
    blockModules: String = "",
): Boolean {
    if (embedLkmInBoot) {
        return patchBootImageV2(
            bootUri = bootUri,
            lkm = lkm,
            ota = ota,
            allowShell = allowShell,
            enableAdb = enableAdb,
            blockModules = blockModules,
            superKey = superKey,
            signatureBypass = signatureBypass,
            onFinish = onFinish,
            onStdout = onStdout,
            onStderr = onStderr,
        )
    }
    val bootFile = try {
        bootUri?.let { copyUriToTemporaryFile(it, "boot-", ".img") }
    } catch (error: Exception) {
        onStderr(error.message ?: "Failed to prepare boot image")
        onFinish(false, 1)
        return false
    }
    return patchBootImage(
        bootFile = bootFile,
        lkm = lkm,
        targetKmi = targetKmi,
        ota = ota,
        partition = partition,
        allowShell = allowShell,
        enableAdb = enableAdb,
        blockModules = blockModules,
        forceBackup = forceBackup,
        superKey = superKey,
        signatureBypass = signatureBypass,
        directInstall = bootUri == null,
        onFinish = onFinish,
        onStdout = onStdout,
        onStderr = onStderr,
    )
}

private fun newDownloadClient(): OkHttpClient = OkHttpClient.Builder()
    .connectTimeout(10, TimeUnit.SECONDS)
    .readTimeout(20, TimeUnit.SECONDS)
    .writeTimeout(20, TimeUnit.SECONDS)
    .build()

private fun validateDownloadUrl(url: String) {
    val uri = url.toUri()
    check(uri.scheme.equals("https", ignoreCase = true) && !uri.host.isNullOrBlank()) {
        "Only HTTPS download URLs are supported"
    }
}

private fun readDownloadMagic(channel: DataSourceChannel): String {
    val buffer = ByteBuffer.allocate(4)
    if (channel.read(buffer) != 4) {
        throw IOException("Downloaded file is shorter than its magic header")
    }
    channel.position(0)
    return String(buffer.array(), StandardCharsets.ISO_8859_1)
}

suspend fun probeRemoteBootPartitions(url: String): ProbeResult = withContext(Dispatchers.IO) {
    validateDownloadUrl(url)
    DataSourceChannel(newDownloadClient(), url).use { channel ->
        when (readDownloadMagic(channel)) {
            "CrAU" -> ExtractImage.probePayload(channel, withKmi = true)
            else -> ExtractImage.probe(channel, withKmi = true)
        }
    }
}

fun downloadBoot(
    url: String,
    partition: String,
    targetKmi: String,
    lkm: LkmSelection,
    allowShell: Boolean,
    enableAdb: Boolean,
    forceBackup: Boolean,
    superKey: String? = null,
    signatureBypass: Boolean = false,
    onFinish: (Boolean, Int) -> Unit,
    onStdout: (String) -> Unit,
    onStderr: (String) -> Unit,
    blockModules: String = "",
): Boolean {
    val bootFile = File(ksuApp.cacheDir, "download-boot.img")
    try {
        validateDownloadUrl(url)
        DataSourceChannel(newDownloadClient(), url).use { channel ->
            val image = ExtractImage(bootFile, onStdout)
            when (readDownloadMagic(channel)) {
                "CrAU" -> image.consumePayload(channel, partition)
                else -> image.consume(channel, partition)
            }
        }
    } catch (error: Exception) {
        bootFile.delete()
        onStderr(error.message ?: "Failed to download boot image")
        onFinish(false, 1)
        return false
    }

    return patchBootImage(
        bootFile = bootFile,
        lkm = lkm,
        targetKmi = targetKmi,
        ota = false,
        partition = partition,
        allowShell = allowShell,
        enableAdb = enableAdb,
        blockModules = blockModules,
        forceBackup = forceBackup,
        superKey = superKey,
        signatureBypass = signatureBypass,
        directInstall = false,
        onFinish = onFinish,
        onStdout = onStdout,
        onStderr = onStderr,
    )
}

fun restartAdbd(): Boolean =
    ShellUtils.fastCmdResult(getRootShell(), "setprop ctl.restart adbd")

fun reboot(reason: String = "") {
    val shell = getRootShell()
    if (reason == "recovery") {
        // KEYCODE_POWER = 26, hide incorrect "Factory data reset" message
        ShellUtils.fastCmd(shell, "/system/bin/input keyevent 26")
    }
    ShellUtils.fastCmd(shell, "/system/bin/svc power reboot $reason || /system/bin/reboot $reason")
}

fun rootAvailable(): Boolean {
    val shell = getRootShell()
    return shell.isRoot
}


private val kmiPattern = Regex("""(?:android\d+-)?\d+\.\d+""")

internal fun parseDetectedKmi(lines: List<String>): String =
    lines.filter { it.matches(kmiPattern) }.singleOrNull().orEmpty()

suspend fun getTargetKmi(ota: Boolean, bootUri: Uri?): String = withContext(Dispatchers.IO) {
    var bootFile: File? = null
    try {
        val args = when {
            ota -> "boot-info target-kmi --ota"
            bootUri != null -> {
                val targetFile = copyUriToTemporaryFile(bootUri, "target-kmi-", ".img")
                bootFile = targetFile
                "boot-info target-kmi --boot ${shellArg(targetFile.absolutePath)}"
            }
            else -> "boot-info target-kmi"
        }
        val command = "TMPDIR=${shellArg(ksuApp.cacheDir.absolutePath)} ${ksudCmd(args)}"
        val lines = getRootShell().newJob().add(command).to(ArrayList(), null).exec().out
            .filter { it.isNotBlank() }.map { it.trim() }
        parseDetectedKmi(lines)
    } finally {
        bootFile?.delete()
    }
}

suspend fun getSupportedKmis(): List<String> = withContext(Dispatchers.IO) {
    ksudReadLines("boot-info supported-kmis")
        .filter { it.matches(kmiPattern) }
        .distinct()
}

internal fun isKmiSupported(currentKmi: String, supportedKmis: List<String>): Boolean =
    currentKmi.isNotBlank() && currentKmi in supportedKmis

internal fun resolveTargetKmi(
    detectedKmi: String,
    supportedKmis: List<String>,
    customLkm: Boolean,
): String? = detectedKmi.takeIf {
    it.isNotBlank() && (customLkm || isKmiSupported(it, supportedKmis))
}

suspend fun isAbDevice(): Boolean = withContext(Dispatchers.IO) {
    ksudReadString("boot-info is-ab-device").toBoolean()
}

suspend fun getDefaultPartition(): String = withContext(Dispatchers.IO) {
    if (getRootShell().isRoot) {
        ksudReadString("boot-info default-partition")
            .takeIf { it == "boot" || it == "init_boot" || it == "vendor_boot" }
            .orEmpty()
    } else {
        // Partition auto-selection requires access to the device boot partitions.
        ""
    }
}

suspend fun getSlotSuffix(ota: Boolean): String = withContext(Dispatchers.IO) {
    val args = if (ota) "boot-info slot-suffix --ota" else "boot-info slot-suffix"
    ksudReadString(args)
}

suspend fun getAvailablePartitions(): List<String> = withContext(Dispatchers.IO) {
    ksudReadLines("boot-info available-partitions")
        .filter { it == "boot" || it == "init_boot" || it == "vendor_boot" }
        .distinct()
}

fun hasMagisk(): Boolean {
    val shell = getRootShell(true)
    val result = shell.newJob().add("which magisk").exec()
    Log.i(TAG, "has magisk: ${result.isSuccess}")
    return result.isSuccess
}

fun isSepolicyValid(rules: String?): Boolean {
    if (rules == null) return true
    return execKsud("sepolicy check ${shellArg(rules)}")
}

fun getSepolicy(pkg: String): String =
    ksudReadLines("profile get-sepolicy ${shellArg(pkg)}").joinToString("\n")

fun setSepolicy(pkg: String, rules: String): Boolean {
    val ok = execKsud("profile set-sepolicy ${shellArg(pkg)} ${shellArg(rules)}")
    Log.i(TAG, "set sepolicy $pkg result: $ok")
    return ok
}

fun listAppProfileTemplates(): List<String> =
    ksudReadLines("profile list-templates")

fun getAppProfileTemplate(id: String): String =
    ksudReadLines("profile get-template ${shellArg(id)}").joinToString("\n")

fun setAppProfileTemplate(id: String, template: String): Boolean =
    execKsud("profile set-template ${shellArg(id)} ${shellArg(template)}")

fun deleteAppProfileTemplate(id: String): Boolean =
    execKsud("profile delete-template ${shellArg(id)}")

fun forceStopApp(packageName: String) {
    val shell = getRootShell()
    val result = shell.newJob().add("am force-stop ${shellArg(packageName)}").exec()
    Log.i(TAG, "force stop $packageName result: $result")
}

fun launchApp(packageName: String) {

    val shell = getRootShell()
    val result =
        shell.newJob()
            .add(
                "cmd package resolve-activity --brief ${shellArg(packageName)} " +
                    "| tail -n 1 | xargs cmd activity start-activity -n"
            )
            .exec()
    Log.i(TAG, "launch $packageName result: $result")
}

fun restartApp(packageName: String) {
    forceStopApp(packageName)
    launchApp(packageName)
}


fun runCmd(shell: Shell, cmd: String): String {
    return shell.newJob()
        .add(cmd)
        .to(mutableListOf<String>(), null)
        .exec().out
        .joinToString("\n")
}


fun getMetaModuleImplement(): String {
    try {
        val metaModuleProp = SuFile.open("/data/adb/metamodule/module.prop")
        if (!metaModuleProp.isFile) {
            Log.i(TAG, "Meta module implement: None")
            return "None"
        }

        val prop = Properties()
        prop.load(metaModuleProp.newInputStream())

        val name = prop.getProperty("name")
        Log.i(TAG, "Meta module implement: $name")
        return name
    } catch (_ : Throwable) {
        Log.i(TAG, "Meta module implement: None")
        return "None"
    }
}

/** Module IDs of known Zygisk implementations; ZygiskNext and NeoZygisk share an ID. */
val ZYGISK_IMPL_MODULE_IDS = listOf("zygisksu", "rezygisk")

suspend fun getZygiskImplement(): String = withContext(Dispatchers.IO) {
    for (moduleId in ZYGISK_IMPL_MODULE_IDS) {
        // skip disabled / pending-removal modules
        if (SuFile.open("/data/adb/modules/$moduleId/disable").isFile || SuFile.open("/data/adb/modules/$moduleId/remove").isFile) continue

        val propFile = SuFile.open("/data/adb/modules/$moduleId/module.prop")
        if (!propFile.isFile) continue

        val prop = Properties()
        prop.load(propFile.newInputStream())

        val name = prop.getProperty("name")
        Log.i(TAG, "Zygisk implement: $name")
        return@withContext name
    }

    Log.i(TAG, "Zygisk implement: None")
    "None"
}
