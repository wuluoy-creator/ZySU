#include "cli.hpp"
#include "../sumhp/include/sumhp/embedded.hpp"
#include "assets.hpp"
#include "boot/boot_patch.hpp"
#include "boot/boot_patch_v2.hpp"
#include "boot/ramdisk_editor.hpp"
#include "cli_args.hpp"
#include "core/feature.hpp"
#include "core/ksucalls.hpp"
#include "core/restorecon.hpp"
#include "core/su_path.hpp"
#include "debug.hpp"
#include "defs.hpp"
#include "flash/flash_ak3.hpp"
#include "flash/flash_partition.hpp"
#include "init_event.hpp"
#include "log.hpp"
#include "module/module.hpp"
#include "module/module_config.hpp"
#include "profile/profile.hpp"
#include "sepolicy/sepolicy.hpp"
#include "su.hpp"
#include "sulog.hpp"
#include "terminal.hpp"
#include "umount.hpp"
#include "utils.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ksud {

namespace {

int check_input_file(const std::string& path, bool allow_block = false) {
    struct stat status{};
    if (stat(path.c_str(), &status) != 0)
        return terminal::file_error("cannot read input", path, errno);
    const auto usable = [allow_block](mode_t mode) {
        return S_ISREG(mode) || (allow_block && S_ISBLK(mode));
    };
    if (!usable(status.st_mode))
        return terminal::error("input '" + path + "' is not a regular file" +
                               (allow_block ? " or block device" : ""));
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return terminal::file_error("cannot read input", path, errno);
    const int result = fstat(fd, &status);
    const int error = errno;
    close(fd);
    if (result != 0)
        return terminal::file_error("cannot inspect input", path, error);
    if (!usable(status.st_mode))
        return terminal::error("input file type changed: '" + path + "'");
    return 0;
}

int check_input_files(const CliArguments& cli) {
    const auto& path = cli.path;
    if (cli.command == "install") {
        for (const auto* option : {"--libadbroot", "--magiskboot"}) {
            if (!cli.has(option))
                continue;
            const int result = check_input_file(cli.value(option));
            if (result != 0)
                return result;
        }
    }
    const bool module = path == "module install";
    if (module || path == "insmod" || path == "debug insmod" || path == "flash image" ||
        path == "flash ak3" || path == "flash ak3-info" || path == "sepolicy apply" ||
        path == "dynamic set-apk" || (path == "dynamic get-sign" && !cli.has("--uid"))) {
        const size_t offset = path == "insmod" ? 0 : 1;
        const int result = check_input_file(cli.args[offset], path == "flash image");
        if (result != 0)
            return result;
    }
    if (cli.command == "boot-patch" || cli.command == "boot-patch-v2" ||
        cli.command == "boot-restore" || path == "boot-info target-kmi") {
        for (const auto* option :
             {"--boot", "--module", "--kernel", "--init", "--adb-debug-prop"}) {
            if (!cli.has(option))
                continue;
            const int result =
                check_input_file(cli.value(option), std::string_view(option) == "--boot");
            if (result != 0)
                return result;
        }
    }
    return 0;
}

void print_version() {
    printf("ksud version %s (code: %s, uapi: %u)\n", VERSION_NAME, VERSION_CODE, uapi_version());
}

int cmd_module(const std::vector<std::string>& args) {
    // Switch to init mount namespace
    if (!switch_mnt_ns(1)) {
        LOGE("Failed to switch mount namespace");
        return 1;
    }

    const std::string& subcmd = args[0];

    if (subcmd == "install" && args.size() > 1) {
        return module_install(args[1]);
    } else if (subcmd == "uninstall" && args.size() > 1) {
        return module_uninstall(args[1]);
    } else if (subcmd == "undo-uninstall" && args.size() > 1) {
        return module_undo_uninstall(args[1]);
    } else if (subcmd == "enable" && args.size() > 1) {
        return module_enable(args[1]);
    } else if (subcmd == "disable" && args.size() > 1) {
        return module_disable(args[1]);
    } else if (subcmd == "action" && args.size() > 1) {
        return module_run_action(args[1]);
    } else if (subcmd == "list") {
        return module_list();
    } else if (subcmd == "config") {
        // Handle module config subcommands
        if (args.size() < 2) {
            printf("USAGE: ksud module config <get|set|list|delete|clear> ...\n");
            return 1;
        }
        return module_config_handle(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    terminal::errorf("Unknown module subcommand: %s\n", subcmd.c_str());
    return 1;
}

int cmd_initrc(const std::vector<std::string>& args) {
    const std::string& subcmd = args[0];
    if (subcmd == "refresh") {
        return regenerate_preinit_rc();
    }

    terminal::errorf("Unknown initrc subcommand: %s\n", subcmd.c_str());
    return 1;
}

int cmd_feature(const std::vector<std::string>& args) {
    const std::string& subcmd = args[0];

    if (subcmd == "get" && args.size() > 1) {
        return feature_get(args[1]);
    } else if (subcmd == "set" && args.size() > 2) {
        uint64_t value = 0;
        if (!parse_uint64(args[2], &value))
            return terminal::usage_error(
                "invalid value '" + args[2] + "': expected an unsigned 64-bit integer",
                "ksud feature set <ID> <VALUE>");
        return feature_set(args[1], value);
    } else if (subcmd == "set-save" && args.size() > 2) {
        uint64_t value = 0;
        if (!parse_uint64(args[2], &value))
            return terminal::usage_error(
                "invalid value '" + args[2] + "': expected an unsigned 64-bit integer",
                "ksud feature set-save <ID> <VALUE>");
        return feature_set_and_save(args[1], value);
    } else if (subcmd == "list") {
        feature_list();
        return 0;
    } else if (subcmd == "check" && args.size() > 1) {
        return feature_check(args[1]);
    } else if (subcmd == "load") {
        return feature_load_config();
    } else if (subcmd == "save") {
        return feature_save_config();
    }

    terminal::errorf("Unknown feature subcommand: %s\n", subcmd.c_str());
    return 1;
}

int cmd_debug(const std::vector<std::string>& args) {
    const std::string& subcmd = args[0];

    if (subcmd == "set-manager") {
        const std::string pkg = args.size() > 1 ? args[1] : "com.zying.zysu";
        return debug_set_manager(pkg);
    } else if (subcmd == "insmod" && args.size() > 1) {
        return debug_insmod(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
    } else if (subcmd == "info") {
        printf("version: %d\n", get_version());
        printf("uapi_version: %u\n", get_uapi_version());
        printf("flags: 0x%x\n", get_flags());
        printf("lkm: %s\n", is_lkm() ? "true" : "false");
        printf("bundled: %s\n", is_lkm_bundled() ? "true" : "false");
        printf("late_load: %s\n", is_late_load() ? "true" : "false");
        printf("runtime_mode: %s\n", runtime_mode());
        return 0;
    } else if (subcmd == "version") {
        printf("Kernel Version: %d\n", get_version());
        return 0;
    } else if (subcmd == "su") {
        const bool global_mnt = args.size() > 1 && args[1] == "-g";
        return grant_root_shell(global_mnt);
    } else if (subcmd == "mark" && args.size() > 1) {
        return debug_mark(std::vector<std::string>(args.begin() + 1, args.end()));
    } else if (subcmd == "sulogd") {
        return ensure_sulogd_running();
    }

    terminal::errorf("Unknown debug subcommand: %s\n", subcmd.c_str());
    return 1;
}

int cmd_insmod(const std::vector<std::string>& args) {
    return debug_insmod(args[0], std::vector<std::string>(args.begin() + 1, args.end()));
}

int cmd_umount(const std::vector<std::string>& args, const CliArguments& cli) {
    const std::string& subcmd = args[0];

    if (subcmd == "add") {
        const std::string& path = args[1];
        uint32_t flags = 0;
        if (cli.has("--flags") && !parse_uint32(cli.value("--flags"), &flags))
            return terminal::usage_error("invalid flags: expected an unsigned 32-bit integer",
                                         "ksud umount add <MOUNT> [--flags <UINT32>]");
        return umount_list_add(path, flags) < 0 ? 1 : 0;
    } else if ((subcmd == "del" || subcmd == "remove") && args.size() > 1) {
        return umount_del_entry(args[1]);
    } else if (subcmd == "list") {
        auto list = umount_list_list();
        if (list) {
            printf("%s", list->c_str());
        }
        return 0;
    } else if (subcmd == "save") {
        return umount_save_config();
    } else if (subcmd == "apply") {
        return umount_apply_config();
    } else if (subcmd == "clear-custom") {
        return umount_clear_config();
    }

    terminal::errorf("Unknown umount subcommand: %s\n", subcmd.c_str());
    return 1;
}

int cmd_kernel(const std::vector<std::string>& args, const CliArguments& cli) {
    const std::string& subcmd = args[0];

    if (subcmd == "nuke-ext4-sysfs" && args.size() > 1) {
        return nuke_ext4_sysfs(args[1]);
    } else if (subcmd == "umount" && args.size() > 1) {
        const std::string& op = args[1];
        if (op == "add" && args.size() > 2) {
            const std::string& path = args[2];
            uint32_t flags = 0;
            if (cli.has("--flags") && !parse_uint32(cli.value("--flags"), &flags))
                return terminal::usage_error("invalid flags: expected an unsigned 32-bit integer",
                                             "ksud kernel umount add <MOUNT> [--flags <UINT32>]");
            return umount_list_add(path, flags);
        } else if (op == "del" && args.size() > 2) {
            return umount_list_del(args[2]);
        } else if (op == "wipe") {
            return umount_list_wipe();
        }
    } else if (subcmd == "notify-module-mounted") {
        report_module_mounted();
        return 0;
    }

    terminal::errorf("Unknown kernel subcommand: %s\n", subcmd.c_str());
    return 1;
}

int cmd_sepolicy(const std::vector<std::string>& args) {
    const std::string& subcmd = args[0];

    if (subcmd == "patch" && args.size() > 1) {
        return sepolicy_live_patch(args[1]);
    } else if (subcmd == "apply" && args.size() > 1) {
        return sepolicy_apply_file(args[1]);
    } else if (subcmd == "check" && args.size() > 1) {
        return sepolicy_check_rule(args[1]);
    }

    terminal::errorf("Unknown sepolicy subcommand: %s\n", subcmd.c_str());
    return 1;
}

int cmd_profile(const std::vector<std::string>& args) {
    const std::string& subcmd = args[0];

    if (subcmd == "get-sepolicy" && args.size() > 1) {
        return profile_get_sepolicy(args[1]);
    } else if (subcmd == "set-sepolicy" && args.size() > 2) {
        return profile_set_sepolicy(args[1], args[2]);
    } else if (subcmd == "get-template" && args.size() > 1) {
        return profile_get_template(args[1]);
    } else if (subcmd == "set-template" && args.size() > 2) {
        return profile_set_template(args[1], args[2]);
    } else if (subcmd == "delete-template" && args.size() > 1) {
        return profile_delete_template(args[1]);
    } else if (subcmd == "list-templates") {
        return profile_list_templates();
    }

    terminal::errorf("Unknown profile subcommand: %s\n", subcmd.c_str());
    return 1;
}

int cmd_boot_info(const std::vector<std::string>& args, const CliArguments& cli) {
    const std::string& subcmd = args[0];

    if (subcmd == "current-kmi") {
        return boot_info_current_kmi();
    } else if (subcmd == "target-kmi") {
        if (cli.has("--ota") && cli.has("--boot"))
            return terminal::usage_error("--ota and --boot cannot be used together",
                                         "ksud boot-info target-kmi [--ota | --boot <PATH>]");
        return boot_info_target_kmi(cli.has("--ota"), cli.value("--boot"));
    } else if (subcmd == "supported-kmis") {
        return boot_info_supported_kmis();
    } else if (subcmd == "is-ab-device") {
        return boot_info_is_ab_device();
    } else if (subcmd == "default-partition") {
        return boot_info_default_partition();
    } else if (subcmd == "available-partitions") {
        return boot_info_available_partitions();
    } else if (subcmd == "slot-suffix") {
        const bool ota = cli.has("--ota");
        return boot_info_slot_suffix(ota);
    }

    terminal::errorf("Unknown boot-info subcommand: %s\n", subcmd.c_str());
    return 1;
}

int cmd_ramdisk_editor(const std::vector<std::string>& args, bool boot_image) {
    const std::size_t expected_args = boot_image ? 2U : 1U;
    if (args.size() != expected_args) {
        (void)fprintf(stderr, boot_image
                                  ? "USAGE: ksud boot-ramdisk-editor <SOURCE.IMG> <OUTPUT.IMG>\n"
                                  : "USAGE: ksud ramdisk-editor <RAMDISK.CPIO>\n");
        return 1;
    }

    if (fflush(stdout) != 0) {
        return 1;
    }
    const int protocol_output = dup(STDOUT_FILENO);
    if (protocol_output < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        if (protocol_output >= 0) {
            close(protocol_output);
        }
        return 1;
    }
    const int result =
        boot_image ? run_boot_ramdisk_editor(args[0], args[1], STDIN_FILENO, protocol_output)
                   : run_ramdisk_editor(args[0], STDIN_FILENO, protocol_output);
    close(protocol_output);
    return result;
}

int cmd_flash_new(const std::vector<std::string>& args, const CliArguments& cli) {
    using namespace flash;

    const std::string& subcmd = args[0];

    if (subcmd == "ak3-info") {
        if (args.size() != 2) {
            (void)fprintf(stderr, "USAGE: ksud flash ak3-info <ZIP>\n");
            return 1;
        }
        const auto info = inspect_ak3_package(args[1]);
        if (!info.valid) {
            (void)terminal::errorf("Invalid AnyKernel3 package: %s", info.error.c_str());
            return 1;
        }
        printf("valid=1\n");
        printf("kernel=%s\n", info.kernel_name.c_str());
        printf("devices=");
        for (size_t i = 0; i < info.devices.size(); ++i) {
            if (i != 0)
                printf("|");
            printf("%s", info.devices[i].c_str());
        }
        printf("\n");
        printf("slot_policy=%s\n", info.package_slot_policy.c_str());
        return 0;
    }

    if (subcmd == "ak3") {
        if (args.size() < 2) {
            (void)fprintf(stderr, "USAGE: ksud flash ak3 <ZIP> [--slot <a|b|_a|_b>] [--log <FILE>] "
                                  "[--use-mkbootfs]\n");
            return 1;
        }
        Ak3FlashConfig config;
        config.zip_path = args[1];
        config.target_slot = cli.value("--slot");
        config.log_path = cli.value("--log");
        config.use_mkbootfs = cli.has("--use-mkbootfs");
        return flash_ak3_package(config);
    }

    std::string target_slot = cli.value("--slot");
    if (!target_slot.empty() && target_slot[0] != '_')
        target_slot.insert(0, 1, '_');
    const bool scan_all = cli.has("--all");
    const auto& filtered_args = args;

    if (filtered_args[0] == "image" && filtered_args.size() >= 3) {
        const std::string& image_path = filtered_args[1];
        const std::string& partition = filtered_args[2];

        std::string progress = "Flashing " + image_path + " to " + partition;
        if (!target_slot.empty())
            progress += " (slot: " + target_slot + ")";
        terminal::message(stderr, "info", progress);

        if (ksud::flash::flash_partition(image_path, partition, target_slot)) {
            terminal::message(stderr, "success", "Flash successful!");
            return 0;
        } else {
            terminal::error("Flash failed!");
            return 1;
        }

    } else if (filtered_args[0] == "backup" && filtered_args.size() >= 3) {
        const std::string& partition = filtered_args[1];
        const std::string& output = filtered_args[2];

        std::string progress = "Backing up " + partition + " to " + output;
        if (!target_slot.empty())
            progress += " (slot: " + target_slot + ")";
        terminal::message(stderr, "info", progress);

        if (ksud::flash::backup_partition(partition, output, target_slot)) {
            terminal::message(stderr, "success", "Backup successful!");
            return 0;
        } else {
            terminal::error("Backup failed!");
            return 1;
        }

    } else if (filtered_args[0] == "list") {
        const std::string slot =
            target_slot.empty() ? ksud::flash::get_current_slot_suffix() : target_slot;
        auto partitions = ksud::flash::get_available_partitions(scan_all, slot);

        if (scan_all) {
            printf("All partitions");
        } else {
            printf("Common partitions");
        }
        if (ksud::flash::is_ab_device() && !slot.empty()) {
            printf(" (slot: %s)", slot.c_str());
        }
        printf(":\n");

        for (const auto& p : partitions) {
            auto info = ksud::flash::get_partition_info(p, slot);
            const char* type = info.is_logical ? "logical" : "physical";
            const char* marker = "";
            if (ksud::flash::is_dangerous_partition(p)) {
                marker = " [DANGEROUS]";
            }
            printf("  %-20s [%s, %lu bytes]%s\n", p.c_str(), type,
                   static_cast<unsigned long>(info.size), marker);
        }
        return 0;

    } else if (filtered_args[0] == "info" && filtered_args.size() >= 2) {
        const std::string& partition = filtered_args[1];
        const std::string slot =
            target_slot.empty() ? ksud::flash::get_current_slot_suffix() : target_slot;
        auto info = ksud::flash::get_partition_info(partition, slot);

        if (!info.exists) {
            terminal::errorf("Partition %s not found", partition.c_str());
            return 1;
        }

        printf("Partition: %s\n", info.name.c_str());
        printf("Block device: %s\n", info.block_device.c_str());
        printf("Type: %s\n", info.is_logical ? "logical" : "physical");
        printf("Size: %lu bytes (%.2f MB)\n", static_cast<unsigned long>(info.size),
               info.size / 1024.0 / 1024.0);

        if (ksud::flash::is_ab_device()) {
            printf("Slot: %s\n", info.slot_suffix.empty() ? "/" : info.slot_suffix.c_str());
        }
        return 0;

    } else if (filtered_args[0] == "slots") {
        // Show slot information for A/B devices
        if (!ksud::flash::is_ab_device()) {
            printf("This device is not A/B partitioned\n");
            return 0;
        }

        const std::string current_slot = ksud::flash::get_current_slot_suffix();
        const std::string other_slot = (current_slot == "_a") ? "_b" : "_a";

        printf("Slot Information:\n");
        printf("  Current slot: %s\n", current_slot.c_str());
        printf("  Other slot:   %s\n", other_slot.c_str());

        // __system_property_get is what getprop itself calls; no process needed.
        if (const auto slot_suffix = getprop("ro.boot.slot_suffix")) {
            printf("  Property ro.boot.slot_suffix: %s\n", slot_suffix->c_str());
        }

        return 0;

    } else if (filtered_args[0] == "map" && filtered_args.size() >= 2) {
        std::string slot = filtered_args[1];
        // Normalize slot format
        if (!slot.empty() && slot[0] != '_') {
            slot = "_" + slot;
        }

        terminal::message(stderr, "info", "Mapping logical partitions for slot " + slot);
        if (ksud::flash::map_logical_partitions(slot)) {
            terminal::message(stderr, "success", "Mapping successful!");
            terminal::message(
                stderr, "hint",
                "Use 'ksud flash list --slot " + slot + " --all' to see mapped partitions.");
            return 0;
        } else {
            terminal::error("Mapping failed or no partitions to map");
            return 1;
        }

    } else if (filtered_args[0] == "avb") {
        if (filtered_args.size() >= 2 && filtered_args[1] == "disable") {
            terminal::message(stderr, "info", "Disabling AVB/dm-verity");
            if (ksud::flash::patch_vbmeta_disable_verification()) {
                terminal::message(stderr, "success", "AVB/dm-verity disabled successfully!");
                terminal::message(stderr, "info", "Reboot required for changes to take effect.");
                return 0;
            } else {
                terminal::errorf("Failed to disable AVB/dm-verity\n");
                return 1;
            }
        } else {
            const std::string status = ksud::flash::get_avb_status();
            if (status.empty()) {
                terminal::errorf("Failed to get AVB status\n");
                return 1;
            }
            printf("AVB/dm-verity status: %s\n", status.c_str());
            return 0;
        }

    } else if (filtered_args[0] == "kernel") {
        const std::string version = ksud::flash::get_kernel_version(target_slot);
        if (version.empty()) {
            terminal::errorf("Failed to get kernel version\n");
            return 1;
        }
        printf("Kernel version: %s\n", version.c_str());
        return 0;

    } else if (filtered_args[0] == "boot-info") {
        const std::string info = ksud::flash::get_boot_slot_info();
        printf("%s\n", info.c_str());
        return 0;
    }

    terminal::errorf("Unknown flash subcommand: %s\n", subcmd.c_str());
    printf("Run 'ksud flash' for usage\n");
    return 1;
}

}  // namespace

int cli_run(int argc, char** argv) {
    // Preserve argv[0] dispatch before recognizing the dedicated query: an
    // invocation as su/sh must keep its original argument interpretation.
    const std::string arg0 = argv[0];
    const size_t last_slash = arg0.rfind('/');
    const std::string basename =
        (last_slash != std::string::npos) ? arg0.substr(last_slash + 1) : arg0;

    // The public queries and private Maps transport deliberately bypass the
    // generic parser/logging path. That parser has no private --channel option,
    // and ordinary SUMHP dispatch may cold-start a restoring daemon. Forward
    // the whole preview prefix, including invalid tails, to the root-checked
    // embedded handler so malformed requests also fail without side effects.
    const bool maps_preview = argc >= 5 && std::strcmp(argv[2], "sumh") == 0 &&
                              std::strcmp(argv[3], "maps") == 0 &&
                              std::strcmp(argv[4], "preview") == 0;
    const bool maps_temporary =
        argc >= 5 && std::strcmp(argv[2], "sumh") == 0 && std::strcmp(argv[3], "maps") == 0 &&
        (std::strcmp(argv[4], "remove") == 0 || std::strcmp(argv[4], "list-temporary") == 0 ||
         std::strcmp(argv[4], "manager") == 0);
    const bool module_preview =
        argc >= 4 && std::strcmp(argv[2], "module") == 0 &&
        (std::strcmp(argv[3], "plan") == 0 || std::strcmp(argv[3], "manager-preview") == 0 ||
         std::strcmp(argv[3], "explain") == 0 || std::strcmp(argv[3], "rebuild-status") == 0 ||
         std::strcmp(argv[3], "inspect") == 0 ||
         (std::strcmp(argv[3], "normalize") == 0 &&
          std::any_of(argv + 4, argv + argc,
                      [](const char* word) { return std::strcmp(word, "--dry-run") == 0; })));
    const bool partitions = argc >= 4 && std::strcmp(argv[2], "config") == 0 &&
                            (std::strcmp(argv[3], "discover-partitions") == 0 ||
                             std::strcmp(argv[3], "sync-partitions") == 0 ||
                             std::strcmp(argv[3], "set-partitions") == 0);
    const bool operations = argc >= 3 && std::strcmp(argv[2], "operation") == 0;
    const bool sumh_profiles = argc >= 3 && std::strcmp(argv[2], "profile") == 0;
    const bool diagnostics = argc >= 3 && std::strcmp(argv[2], "doctor") == 0;
    const bool hide_status =
        argc >= 4 && std::strcmp(argv[2], "hide") == 0 &&
        (std::strcmp(argv[3], "status") == 0 || std::strcmp(argv[3], "definitions") == 0 ||
         std::strcmp(argv[3], "export") == 0);
    const bool hide_channel = argc >= 4 && std::strcmp(argv[2], "hide") == 0 &&
                              std::strcmp(argv[argc - 1], "--channel") == 0;
    const bool capabilities_channel = argc >= 4 && std::strcmp(argv[2], "api") == 0 &&
                                      std::strcmp(argv[3], "meta") == 0 &&
                                      std::strcmp(argv[argc - 1], "--channel") == 0;
    const bool capability_snapshot =
        argc >= 4 && std::strcmp(argv[2], "api") == 0 && std::strcmp(argv[3], "capabilities") == 0;
    if (basename != "su" && basename != "sh" && argc >= 2 && std::strcmp(argv[1], "sumhp") == 0 &&
        (maps_preview || maps_temporary || module_preview || partitions || operations ||
         sumh_profiles || diagnostics || hide_status || hide_channel || capabilities_channel ||
         capability_snapshot)) {
        // Driver discovery and failed ioctls otherwise write /dev/log/main.
        // The dedicated child and its sampler inherit this process-local
        // silence; restore the prior level if an embedding caller continues.
        class RestoreLogLevel {
        public:
            explicit RestoreLogLevel(LogLevel previous) : level_(previous) {}
            RestoreLogLevel(const RestoreLogLevel&) = delete;
            RestoreLogLevel& operator=(const RestoreLogLevel&) = delete;
            RestoreLogLevel(RestoreLogLevel&&) = delete;
            RestoreLogLevel& operator=(RestoreLogLevel&&) = delete;
            ~RestoreLogLevel() { log_set_level(level_); }

        private:
            LogLevel level_;
        };
        const RestoreLogLevel restore_log_level{log_get_level()};
        log_set_level(LogLevel::OFF);
        return sumhp::embedded_command(std::vector<std::string>(argv + 2, argv + argc));
    }

    // Existing commands retain logging and SIGSYS setup, including su/sh.
    log_init("KernelSU");
    log_set_cli_mode(false);
    setup_sigsys_handler();

    if (basename == "su") {
        return su_main(argc, argv);
    }

    // If invoked as "sh", forward to busybox sh with all arguments
    // This handles the case where /system/bin/sh is a hardlink to ksud
    if (basename == "sh") {
        // Use busybox to handle shell operations
        const char* busybox = "/data/adb/ksu/bin/busybox";

        // Build argv for busybox: busybox sh [original args...]
        std::vector<char*> new_argv;
        new_argv.push_back(const_cast<char*>("sh"));
        for (int i = 1; i < argc; i++) {
            new_argv.push_back(argv[i]);
        }
        new_argv.push_back(nullptr);

        // Set ASH_STANDALONE to make busybox ash work properly
        setenv("ASH_STANDALONE", "1", 1);

        execv(busybox, new_argv.data());
        // If busybox fails, try system sh as fallback
        execv("/system/bin/toybox", new_argv.data());
        _exit(127);
    }

    CliArguments cli;
    const std::vector<std::string> input(argv + 1, argv + argc);
    const int parse_result = parse_cli(input, cli);
    if (parse_result >= 0)
        return parse_result;
    log_set_cli_mode(cli.verbose);
    const int input_result = check_input_files(cli);
    if (input_result != 0)
        return input_result;
    const auto& cmd = cli.command;
    auto& args = cli.args;
    if (cli.has("--temp"))
        args.emplace_back("--temp");
    if (cli.path == "debug su" && cli.has("-g"))
        args.emplace_back("-g");
    if (cmd == "su-path" && cli.has("--json"))
        args.insert(args.begin() + 1, "--json");
    if (cli.path == "dynamic get-sign") {
        const std::string target = cli.has("--uid") ? cli.value("--uid") : args[1];
        args = {"get-sign"};
        if (cli.has("--json"))
            args.emplace_back("--json");
        args.emplace_back(cli.has("--uid") ? "--uid" : "--");
        args.push_back(target);
    }
    if (cli.forward_options || cmd == "boot-patch-v2")
        args.insert(args.end(), cli.option_args.begin(), cli.option_args.end());
    LOGD("command: %s", cmd.c_str());

    // Dispatch commands
    if (cmd == "version") {
        print_version();
        return 0;
    } else if (cmd == "insmod") {
        return cmd_insmod(args);
    } else if (cmd == "post-fs-data") {
        return on_post_fs_data();
    } else if (cmd == "services") {
        on_services();
        return 0;
    } else if (cmd == "boot-completed") {
        on_boot_completed();
        return 0;
    } else if (cmd == "module") {
        return cmd_module(args);
    } else if (cmd == "install") {
        const auto magiskboot = cli.has("--magiskboot")
                                    ? std::optional<std::string>(cli.value("--magiskboot"))
                                    : std::nullopt;
        const auto libadbroot = cli.has("--libadbroot")
                                    ? std::optional<std::string>(cli.value("--libadbroot"))
                                    : std::nullopt;
        return install(magiskboot, libadbroot);
    } else if (cmd == "uninstall") {
        const auto magiskboot = cli.has("--magiskboot")
                                    ? std::optional<std::string>(cli.value("--magiskboot"))
                                    : std::nullopt;
        return uninstall(magiskboot);
    } else if (cmd == "sepolicy") {
        return cmd_sepolicy(args);
    } else if (cmd == "profile") {
        return cmd_profile(args);
    } else if (cmd == "feature") {
        return cmd_feature(args);
    } else if (cmd == "sumhp") {
        return sumhp::embedded_command(args);
    } else if (cmd == "su-path") {
        return su_path_command(args);
    } else if (cmd == "initrc") {
        return cmd_initrc(args);
    } else if (cmd == "sulogd") {
        return run_sulogd();
    } else if (cmd == "boot-patch") {
        return boot_patch(args);
    } else if (cmd == "boot-patch-v2") {
        return boot_patch_v2(args);
    } else if (cmd == "boot-restore") {
        return boot_restore(args);
    } else if (cmd == "boot-info") {
        return cmd_boot_info(args, cli);
    } else if (cmd == "ramdisk-editor") {
        return cmd_ramdisk_editor(args, false);
    } else if (cmd == "boot-ramdisk-editor") {
        return cmd_ramdisk_editor(args, true);
    } else if (cmd == "umount") {
        return cmd_umount(args, cli);
    } else if (cmd == "kernel") {
        return cmd_kernel(args, cli);
    } else if (cmd == "debug") {
        return cmd_debug(args);
    } else if (cmd == "flash") {
        return cmd_flash_new(args, cli);
    }

    return terminal::usage_error("unknown command '" + cmd + "'", "ksud <COMMAND>");
}

}  // namespace ksud
