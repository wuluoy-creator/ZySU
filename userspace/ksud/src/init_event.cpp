#include "init_event.hpp"
#include "../sumhp/include/sumhp/embedded.hpp"
#include "assets.hpp"
#include "core/feature.hpp"
#include "core/hide_bootloader.hpp"
#include "core/ksucalls.hpp"
#include "core/restorecon.hpp"
#include "defs.hpp"
#include "integrity_monitor.hpp"
#include "log.hpp"
#include "module/metamodule.hpp"
#include "module/module.hpp"
#include "module/module_config.hpp"
#include "profile/profile.hpp"
#include "sulog.hpp"
#include "umount.hpp"
#include "utils.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ksud {

namespace {

// Catch boot logs (logcat/dmesg) to file
void catch_bootlog(const char* logname, const std::vector<const char*>& command) {
    ensure_dir_exists(LOG_DIR);

    const std::string bootlog = std::string(LOG_DIR) + "/" + logname + ".log";
    const std::string oldbootlog = std::string(LOG_DIR) + "/" + logname + ".old.log";

    // Rotate old log
    if (access(bootlog.c_str(), F_OK) == 0) {
        (void)rename(bootlog.c_str(), oldbootlog.c_str());
    }

    // Fork and exec timeout command
    const pid_t pid = fork();
    if (pid < 0) {
        LOGW("Failed to fork for %s: %s", logname, strerror(errno));
        return;
    }

    if (pid == 0) {
        // Child process
        // Create new process group
        setpgid(0, 0);

        // Switch cgroups
        switch_cgroups();

        // Open log file for stdout
        const int fd = open(bootlog.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            _exit(1);
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);

        // Build argv: timeout -s 9 30s <command...>
        std::vector<const char*> argv;
        argv.push_back("timeout");
        argv.push_back("-s");
        argv.push_back("9");
        argv.push_back(BOOTLOG_TIMEOUT);
        for (const char* arg : command) {
            argv.push_back(arg);
        }
        argv.push_back(nullptr);

        execvp("timeout", const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    // Parent: don't wait, let it run in background
    LOGI("Started %s capture (pid %d)", logname, pid);
}

void run_stage(const std::string& stage, ScriptWait wait) {
    umask(0);

    // Check for Magisk (like Rust version)
    if (has_magisk()) {
        LOGW("Magisk detected, skip %s", stage.c_str());
        return;
    }

    if (is_safe_mode()) {
        LOGW("safe mode, skip %s scripts", stage.c_str());
        return;
    }

    // Execute common scripts first
    exec_common_scripts(stage + ".d", wait);

    // Execute metamodule stage script (priority)
    metamodule_exec_stage_script(stage, wait);

    // Execute regular modules stage scripts
    exec_stage_script(stage, wait);
}

}  // namespace

int on_post_fs_data() {
    LOGI("post-fs-data triggered");
    sumhp::embedded_post_fs_data();

    if (!ensure_uapi_version_matched()) {
        LOGE("Skip post-fs-data due to UAPI version mismatch");
        sumhp::embedded_mount_skipped("UAPI version mismatch");
        return 0;
    }

    if (set_init_pgrp() != 0) {
        LOGW("set init pgrp failed");
    }

    // Report to kernel first
    report_post_fs_data();

    if (!start_ksud_integrity_monitor()) {
        LOGW("Failed to start ksud integrity monitor");
    }

    umask(0);

    // Clear all temporary module configs early (like Rust version)
    clear_all_temp_configs();

    // Catch boot logs
    catch_bootlog("logcat", {"logcat", "-b", "all"});
    catch_bootlog("dmesg", {"dmesg", "-w"});

    const bool safe_mode = is_safe_mode();
    // Check for Magisk (like Rust version)
    if (has_magisk()) {
        LOGW("Magisk detected, skip post-fs-data!");
        sumhp::embedded_mount_skipped("Magisk owns boot handling");
        return 0;
    }

    const auto wait = ScriptWait::until(ScriptWait::Clock::now() + BOOT_STAGE_TIMEOUT);

    if (safe_mode) {
        LOGW("safe mode, skip common post-fs-data.d scripts");
    } else {
        // Execute common post-fs-data scripts
        exec_common_scripts("post-fs-data.d", wait);
    }

    // Ensure directories exist
    ensure_dir_exists(WORKING_DIR);
    ensure_dir_exists(MODULE_DIR);
    ensure_dir_exists(LOG_DIR);
    ensure_dir_exists(PROFILE_DIR);

    // Ensure binaries exist (AFTER safe mode check, like Rust)
    if (ensure_binaries(true) != 0) {
        LOGW("Failed to ensure binaries");
    }

    if (!restorecon())
        LOGW("Failed to migrate KernelSU file contexts");

    // if we are in safe mode, we should disable all modules
    if (safe_mode) {
        LOGW("safe mode, skip post-fs-data scripts and disable all modules!");
        sumhp::embedded_mount_skipped("safe mode");
        disable_all_modules();
        return 0;
    }

    // Handle updated modules
    handle_updated_modules();

    // Prune modules marked for removal
    prune_modules();

    // Refresh custom init rc for the next boot. This also covers manual edits in
    // /data/adb/initrc.d.
    if (regenerate_preinit_rc() != 0) {
        LOGW("regenerate preinit rc failed");
    }

    // Restorecon
    restorecon("/data/adb", true);

    // Load sepolicy rules from modules
    load_sepolicy_rule();

    // Apply profile sepolicies
    apply_profile_sepolies();

    // Load feature config (with init_features handling managed features)
    init_features();
    sumhp::embedded_restore_kernel_build();
    ensure_sulogd_running_if_enabled();

    // KernelSU execution order (https://kernelsu.org/guide/metamodule.html):
    // 1. Common post-fs-data.d, prune, restorecon, sepolicy
    // 2. Metamodule's post-fs-data.sh
    // 3. Regular modules' post-fs-data.sh
    // 4. Load system.prop
    // 5. Metamodule's metamount.sh  <-- MUST run AFTER all post-fs-data
    // 6. post-mount.d

    metamodule_exec_stage_script("post-fs-data", wait);
    exec_stage_script("post-fs-data", wait);
    load_system_prop();

    // Metamodule metamount runs AFTER all post-fs-data.
    metamodule_exec_mount_script();

    umount_apply_config();

    run_stage("post-mount", wait);
    if (refresh_sucompat_vfs() != 0) {
        LOGW("refresh vnode-backed su after final mounts failed");
    }

    // Arm per-app unmounting only after the mount stage and post-mount scripts
    // have settled. A failed mount stage can still leave mounts to detach, so
    // this notification must not depend on the mount controller's exit status.
    report_module_mounted();

    // Finish property changes before init continues to framework startup.
    // Run after module props and scripts so they cannot undo this initial pass.
    hide_bootloader_status();

    chdir("/");

    LOGI("post-fs-data completed");
    return 0;
}

void on_services() {
    if (!ensure_uapi_version_matched()) {
        LOGE("Skip services due to UAPI version mismatch");
        return;
    }

    const int ret = report_services();
    if (ret < 0) {
        LOGE("Failed to report services: %s", strerror(errno));
        return;
    }
    if (ret != 1) {
        LOGI("services already started, skipping");
        return;
    }

    LOGI("services triggered");

    // Retry properties that init or vendor services populated after post-fs-data.
    hide_bootloader_status();

    run_stage("service", ScriptWait::no_wait());

    LOGI("services completed");
}

void on_boot_completed() {
    LOGI("boot-completed triggered");

    if (!ensure_uapi_version_matched()) {
        LOGE("Skip boot-completed due to UAPI version mismatch");
        return;
    }

    // Report to kernel
    report_boot_complete();
    sumhp::embedded_boot_completed();

    // Final retry for late properties and earlier write failures.
    hide_bootloader_status();

    // Run boot-completed stage
    run_stage("boot-completed", ScriptWait::no_wait());

    LOGI("boot-completed completed");
}

}  // namespace ksud
