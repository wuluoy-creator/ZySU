package com.zying.zysu.ui

import android.annotation.SuppressLint
import android.content.Intent
import android.content.pm.PackageInfo
import android.os.*
import android.util.Log
import com.topjohnwu.superuser.ipc.RootService
import com.zying.zysu.BuildConfig
import com.zying.zysu.IKsuInterface
import java.io.File
import java.nio.file.Files
import java.nio.file.NoSuchFileException
import java.util.concurrent.TimeUnit

/**
 * @author ShirkNeko
 * @date 2025/10/17.
 */
class KsuService : RootService() {

    private val TAG = "KsuService"

    private val cacheLock = Any()
    private var _all: List<PackageInfo>? = null
    private val allPackages: List<PackageInfo>
        get() = synchronized(cacheLock) {
            _all ?: loadAllPackages().also { _all = it }
        }

    private fun loadAllPackages(): List<PackageInfo> {
        val users = getSystemService(USER_SERVICE) as UserManager
        return loadUserPackageSnapshot(
            users,
            { users.userProfiles.map { it.getUserIdCompat() } },
            ::getInstalledPackagesAsUser,
        )
    }

    internal inner class Stub : IKsuInterface.Stub() {
        override fun getPackageCount(): Int = allPackages.size

        override fun refreshPackages(): Int = synchronized(cacheLock) {
            loadAllPackages().also { _all = it }.size
        }

        override fun getPackages(start: Int, maxCount: Int): List<PackageInfo> {
            val list = allPackages
            val end = (start + maxCount).coerceAtMost(list.size)
            return if (start >= list.size) emptyList()
            else list.subList(start, end)
        }

        override fun getUidPackagesForBackup(uid: Int): Array<String> {
            require(uid >= 0)
            return allPackages
                .filter { it.applicationInfo?.uid == uid }
                .map { it.packageName }.distinct().sorted().toTypedArray()
        }

        override fun readProfileRulesForBackup(packageName: String): String {
            val file = profileRulesFile(packageName)
            return try {
                require(Files.size(file.toPath()) <= 65536) { "SELinux rules too large" }
                String(Files.readAllBytes(file.toPath()), Charsets.UTF_8)
            } catch (_: NoSuchFileException) {
                ""
            }
        }

        override fun checkProfileRulesForBackup(rules: String): Boolean =
            rules.isEmpty() || runProfileCommand("sepolicy", "check", rules)

        override fun writeProfileRulesForBackup(packageName: String, rules: String): Boolean {
            profileRulesFile(packageName)
            require(rules.toByteArray(Charsets.UTF_8).size <= 65536 && '\u0000' !in rules)
            return runProfileCommand("profile", "set-sepolicy", packageName, rules) &&
                readProfileRulesForBackup(packageName) == rules
        }
    }

    private fun profileRulesFile(packageName: String): File {
        require(packageName.length < 256 &&
            Regex("[a-zA-Z][a-zA-Z0-9_]*(\\.[a-zA-Z][a-zA-Z0-9_]*)*").matches(packageName))
        return File("/data/adb/ksu/profile/selinux", packageName)
    }

    private fun runProfileCommand(vararg arguments: String): Boolean {
        val daemon = File(applicationInfo.nativeLibraryDir, BuildConfig.KSUD_LIBRARY_NAME).absolutePath
        val process = ProcessBuilder(listOf(daemon) + arguments)
            .redirectInput(ProcessBuilder.Redirect.from(File("/dev/null")))
            .redirectOutput(ProcessBuilder.Redirect.to(File("/dev/null")))
            .redirectError(ProcessBuilder.Redirect.to(File("/dev/null")))
            .start()
        if (!process.waitFor(10, TimeUnit.SECONDS)) {
            process.destroyForcibly()
            process.waitFor(1, TimeUnit.SECONDS)
            Log.e(TAG, "ksud command timed out: ${arguments.joinToString(" ")}")
            return false
        }
        return process.exitValue() == 0
    }

    override fun onBind(intent: Intent): IBinder = Stub()

    @SuppressLint("PrivateApi")
    private fun getInstalledPackagesAsUser(userId: Int): List<PackageInfo> {
        return try {
            queryInstalledPackagesAsUser(userId)
        } catch (e: Throwable) {
            Log.e(TAG, "getInstalledPackagesAsUser", e)
            emptyList()
        }
    }

    @SuppressLint("PrivateApi")
    private fun queryInstalledPackagesAsUser(userId: Int): List<PackageInfo> {
        val pm = packageManager
        val method = pm.javaClass.getDeclaredMethod(
            "getInstalledPackagesAsUser", Int::class.java, Int::class.java
        )
        @Suppress("UNCHECKED_CAST")
        return method.invoke(pm, 0, userId) as List<PackageInfo>
    }

    private fun UserHandle.getUserIdCompat(): Int {
        return try {
            javaClass.getDeclaredField("identifier").apply { isAccessible = true }.getInt(this)
        } catch (_: NoSuchFieldException) {
            javaClass.getDeclaredMethod("getIdentifier").invoke(this) as Int
        } catch (e: Throwable) {
            Log.e("KsuService", "getUserIdCompat", e)
            0
        }
    }
}
