package com.zying.zysu.ui.util

import android.content.Context
import android.os.Build
import android.system.Os
import com.zying.zysu.Natives
import com.zying.zysu.ui.screen.getManagerVersion
import com.topjohnwu.superuser.Shell
import java.io.File
import java.io.FileWriter
import java.io.PrintWriter
import java.time.LocalDateTime
import java.time.format.DateTimeFormatter
import java.util.UUID

private fun buildBugreportFile(context: Context, bugreportDir: File, shell: Shell): File {
    val processFile = File(bugreportDir, "process.txt")
    val dmesgFile = File(bugreportDir, "dmesg.txt")
    val logcatFile = File(bugreportDir, "logcat.txt")
    val tombstonesFile = File(bugreportDir, "tombstones.tar.gz")
    val dropboxFile = File(bugreportDir, "dropbox.tar.gz")
    val pstoreFile = File(bugreportDir, "pstore.tar.gz")
    val diagFile = File(bugreportDir, "diag.tar.gz")
    val oplusFile = File(bugreportDir, "oplus.tar.gz")
    val bootlogFile = File(bugreportDir, "bootlog.tar.gz")
    val mountsFile = File(bugreportDir, "mounts.txt")
    val fileSystemsFile = File(bugreportDir, "filesystems.txt")
    val adbFileTree = File(bugreportDir, "adb_tree.txt")
    val adbFileDetails = File(bugreportDir, "adb_details.txt")
    val ksuFileSize = File(bugreportDir, "ksu_size.txt")
    val appListFile = File(bugreportDir, "packages.txt")
    val propFile = File(bugreportDir, "props.txt")
    val allowListFile = File(bugreportDir, "allowlist.bin")
    val procModules = File(bugreportDir, "proc_modules.txt")
    val bootConfig = File(bugreportDir, "boot_config.txt")
    val kernelConfig = File(bugreportDir, "defconfig.gz")
    val kallsyms = File(bugreportDir, "kallsyms.txt")

    // busybox ps has very few features for embed devices
    shell.newJob().add("toybox ps -T -A -w -o PID,TID,UID,COMM,CMDLINE,CMD,LABEL,STAT,WCHAN > ${shellArg(processFile.absolutePath)}").exec()
    shell.newJob().add("dmesg -r > ${shellArg(dmesgFile.absolutePath)}").exec()
    shell.newJob().add("logcat -b all -v uid -d > ${shellArg(logcatFile.absolutePath)}").exec()
    shell.newJob()
        .add("tar -czf ${shellArg(tombstonesFile.absolutePath)} -C /data/tombstones .")
        .exec()
    shell.newJob().add("tar -czf ${shellArg(dropboxFile.absolutePath)} -C /data/system/dropbox .").exec()
    shell.newJob()
        .add("tar -czf ${shellArg(pstoreFile.absolutePath)} -C /sys/fs/pstore .")
        .exec()
    shell.newJob().add("tar -czf ${shellArg(diagFile.absolutePath)} -C /data/vendor/diag . --exclude=./minidump.gz").exec()
    shell.newJob().add("tar -czf ${shellArg(oplusFile.absolutePath)} -C /mnt/oplus/op2/media/log/boot_log/ .").exec()
    shell.newJob().add("tar -czf ${shellArg(bootlogFile.absolutePath)} -C /data/adb/ksu/log .").exec()

    shell.newJob().add("cat /proc/1/mountinfo > ${shellArg(mountsFile.absolutePath)}").exec()
    shell.newJob().add("cat /proc/filesystems > ${shellArg(fileSystemsFile.absolutePath)}").exec()
    shell.newJob().add("busybox tree /data/adb > ${shellArg(adbFileTree.absolutePath)}").exec()
    shell.newJob().add("ls -alRZ /data/adb > ${shellArg(adbFileDetails.absolutePath)}").exec()
    shell.newJob().add("du -sh /data/adb/ksu/* > ${shellArg(ksuFileSize.absolutePath)}").exec()
    shell.newJob().add("cp /data/system/packages.list ${shellArg(appListFile.absolutePath)}").exec()
    shell.newJob().add("getprop > ${shellArg(propFile.absolutePath)}").exec()
    shell.newJob().add("cp /data/adb/ksu/.allowlist ${shellArg(allowListFile.absolutePath)}").exec()
    shell.newJob().add("cp /proc/modules ${shellArg(procModules.absolutePath)}").exec()
    shell.newJob().add("cp /proc/bootconfig ${shellArg(bootConfig.absolutePath)}").exec()
    shell.newJob().add("cp /proc/config.gz ${shellArg(kernelConfig.absolutePath)}").exec()
    shell.newJob().add("ORIG=\$(cat /proc/sys/kernel/kptr_restrict); echo 1 > /proc/sys/kernel/kptr_restrict; cat /proc/kallsyms > ${shellArg(kallsyms.absolutePath)}; echo \$ORIG > /proc/sys/kernel/kptr_restrict").exec()

    val selinux = getSELinuxLabel()

    val buildInfo = File(bugreportDir, "basic.txt")
    PrintWriter(FileWriter(buildInfo)).use { pw ->
        pw.println("Kernel: ${System.getProperty("os.version")}")
        pw.println("BRAND: " + Build.BRAND)
        pw.println("MODEL: " + Build.MODEL)
        pw.println("PRODUCT: " + Build.PRODUCT)
        pw.println("MANUFACTURER: " + Build.MANUFACTURER)
        pw.println("SDK: " + Build.VERSION.SDK_INT)
        pw.println("PREVIEW_SDK: " + Build.VERSION.PREVIEW_SDK_INT)
        pw.println("FINGERPRINT: " + Build.FINGERPRINT)
        pw.println("DEVICE: " + Build.DEVICE)
        pw.println("Manager: " + getManagerVersion(context))
        pw.println("SELinux: $selinux")

        val uname = Os.uname()
        pw.println("KernelRelease: ${uname.release}")
        pw.println("KernelVersion: ${uname.version}")
        pw.println("Machine: ${uname.machine}")
        pw.println("Nodename: ${uname.nodename}")
        pw.println("Sysname: ${uname.sysname}")

        val ksuKernel = Natives.version
        pw.println("KernelSU: $ksuKernel")
        val safeMode = Natives.isSafeMode
        pw.println("SafeMode: $safeMode")
        pw.println("LKM: true")
    }

    val modulesFile = File(bugreportDir, "modules.json")
    modulesFile.writeText(listModules())

    val formatter = DateTimeFormatter.ofPattern("yyyy-MM-dd_HH_mm_ss_SSS")
    val current = LocalDateTime.now().format(formatter)

    val targetFile = File.createTempFile("ZySU_bugreport_${current}_", ".tar.gz", context.cacheDir)

    val archive = shell.newJob()
        .add("tar czf ${shellArg(targetFile.absolutePath)} -C ${shellArg(bugreportDir.absolutePath)} . && chmod 0644 ${shellArg(targetFile.absolutePath)}")
        .exec()
    if (!archive.isSuccess || !targetFile.isFile || targetFile.length() == 0L) {
        shell.newJob()
            .add("rm -rf -- ${shellArg(bugreportDir.absolutePath)} ${shellArg(targetFile.absolutePath)}")
            .exec()
        error("Failed to create bugreport archive")
    }
    return targetFile
}

@Synchronized
fun getBugreportFile(context: Context): File = withRootShell(true) { shell ->
    val bugreportDir = File(context.cacheDir, "bugreport_${UUID.randomUUID()}")
    check(bugreportDir.mkdir()) { "Failed to create bugreport staging directory" }
    try {
        buildBugreportFile(context, bugreportDir, shell)
    } finally {
        shell.newJob().add("rm -rf -- ${shellArg(bugreportDir.absolutePath)}").exec()
    }
}
