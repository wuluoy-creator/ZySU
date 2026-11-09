#include "sumhp/embedded.hpp"
#include <sys/mount.h>
#include <unistd.h>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include "core/command.hpp"
#include "core/daemon.hpp"
#include "core/ksucalls.hpp"
#include "core/log.hpp"
#include "core/runtime.hpp"
#include "module/metamodule.hpp"
#include "mount/backend.hpp"
#include "sumhp/config.hpp"
#include "sumhp/sumh_client.hpp"
#include "terminal.hpp"
#include "utils.hpp"

namespace sumhp {
int embedded_command(const std::vector<std::string>& args) try {
    if (!args.empty() && (args[0] == "version" || args[0] == "--version"))
        return run_command(args);
    if (geteuid() != 0) {
        return ksud::terminal::error("SUMHP requires root",
                                     "Run this command from an authorized root shell.");
    }
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h" ||
        args[0] == "version" || args[0] == "--version" || args[0] == "daemon")
        return run_command(args);
    return run_via_daemon(args, true);
} catch (const std::exception& error) {
    return ksud::terminal::error(std::string("SUMHP: ") + error.what());
}

int embedded_mount() try {
    std::string error;
    if (!prepare_runtime(error)) {
        logging::write(logging::Level::Error, "boot", error);
        return 1;
    }
    if (!std::filesystem::exists(runtime_config_file()) && !write_default_config(error)) {
        std::cerr << "SUMHP: " << error << '\n';
        return 1;
    }
    Config config;
    if (!read_config_file(config, error)) {
        std::cerr << "SUMHP: " << error << '\n';
        return 1;
    }
    logging::write(logging::Level::Info, "boot",
                   "starting built-in mount plan; no external LKM lookup");
    return run_via_daemon({"module", "mount-all"});
} catch (const std::exception& error) {
    std::cerr << "SUMHP mount: " << error.what() << '\n';
    return 1;
}

void embedded_restore_kernel_build(bool boot_completed) {
    if (ksud::is_safe_mode()) {
        return;
    }
    // Kernel identity is independent of which backend owns module mounts.
    if (sumh::is_available()) {
        Config config;
        std::string error;
        if (!prepare_runtime(error)) {
            logging::write(logging::Level::Error, "boot",
                           "prepare kernel build settings: " + error);
            return;
        }
        errno = 0;
        if (read_config_file(config, error)) {
            if (config.enable_kernel_build_spoof && !boot_completed &&
                config.kernel_build_apply_stage == "boot-completed")
                return;
            if (!sumh::set_kernel_build(config.enable_kernel_build_spoof,
                                        config.kernel_build_release, config.kernel_build_version)) {
                logging::write(logging::Level::Error, "boot",
                               "failed to restore kernel build spoof");
            }
        } else if (errno != ENOENT) {
            logging::write(logging::Level::Error, "boot", "read kernel build settings: " + error);
        }
    }
}

void embedded_boot_completed() {
    logging::write(logging::Level::Info, "boot", "boot completed");
    mount::recovery_boot_completed();
    // Retry an early restoration that could not reach the kernel yet.
    embedded_restore_kernel_build(true);
    if (ksud::is_safe_mode()) {
        return;
    }
    // Path identities are resolved only after module mounts have settled.
    // Maps filtering is independent of which controller owns those mounts.
    if (sumh::is_available() && run_via_daemon({"sumh", "maps", "apply"}) != 0) {
        logging::write(logging::Level::Error, "boot", "failed to restore maps path rules");
    }
    if (sumh::is_available() && run_via_daemon({"sumh", "kstat", "apply"}) != 0) {
        logging::write(logging::Level::Error, "boot", "failed to restore kstat path rules");
    }
    if (!embedded_external_mount_owner().empty()) {
        return;
    }
    if (run_via_daemon({"hide", "apply"}) != 0) {
        logging::write(logging::Level::Error, "boot", "failed to restore user hide rules");
    }
}

void embedded_post_fs_data() {
    std::string error;
    if (!prepare_runtime(error) || !logging::prepare_boot_log(error)) {
        logging::write(logging::Level::Error, "boot", "prepare built-in controller: " + error);
        return;
    }
    logging::write(logging::Level::Info, "boot",
                   "post-fs-data; state=" + runtime_data_dir().string());
}

void embedded_mount_skipped(const std::string& reason) {
    logging::write(logging::Level::Info, "boot", "built-in mount skipped: " + reason);
}

bool embedded_register_umount(const std::string& path) {
    // Skeletons contain child binds and files may already be open in zygote.
    // Match the mountinfo scan's detach semantics when the kernel falls back
    // to this list; ordinary umount can leave the whole tree behind with EBUSY.
    const bool ok = ksud::umount_list_add(path, MNT_DETACH) == 0;
    if (!ok)
        logging::write(logging::Level::Error, "mount", "failed to register unmount path " + path);
    return ok;
}

bool embedded_unregister_umount(const std::string& path) {
    return ksud::umount_list_del(path) == 0;
}

std::string embedded_external_mount_owner() {
    return ksud::metamodule_mount_owner();
}
}  // namespace sumhp

extern "C" int ksu_sumh_ioctl(unsigned long cmd, void* arg) {
    return ksud::ksuctl(static_cast<int>(cmd), arg);
}
