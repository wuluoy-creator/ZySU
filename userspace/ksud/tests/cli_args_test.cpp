#include "cli_args.hpp"
#include "core/json.hpp"
#include "terminal.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace sumhp {
// The commands under test never validate a SUMH configuration patch.
bool validate_config_patch(const json::Value&, std::string&) {
    assert(false && "unexpected SUMH configuration validation");
    return false;
}
}  // namespace sumhp

int main() {
    assert(ksud::terminal::set_color("never"));

    for (const auto* command : {"boot-patch", "boot-patch-v2"}) {
        ksud::CliArguments parsed;
        assert(ksud::parse_cli({command, "--force-feature", "sulog=1", "--force-feature=6=1",
                                "--block-module", "mkp", "--block-module=vr", "--no-custom-rc"},
                               parsed) == -1);
        const std::vector<std::string> expected = {
            "--force-feature", "sulog=1", "--force-feature", "6=1", "--block-module", "mkp",
            "--block-module",  "vr",      "--no-custom-rc"};
        assert(parsed.option_args == expected);
        assert(ksud::parse_cli({command, "--block-module"}, parsed) == 2);
        assert(ksud::parse_cli({command, "--force-feature"}, parsed) == 2);
        assert(ksud::parse_cli({command, "--allow-shell", "--allow-shell"}, parsed) == 2);
    }

    // Embedded boot tools no longer accept an ignored external-tool path.
    for (const auto* command : {"boot-patch", "boot-patch-v2", "boot-restore", "uninstall"}) {
        ksud::CliArguments parsed;
        assert(ksud::parse_cli({command, "--magiskboot", "/tmp/magiskboot"}, parsed) == 2);
        assert(ksud::parse_cli({command, "--magiskboot=/tmp/magiskboot"}, parsed) == 2);
        assert(ksud::parse_cli({command, "--magiskboot"}, parsed) == 2);
    }

    // Retired aliases must fail before dispatch, including nested daemon requests.
    for (const auto& command : std::vector<std::vector<std::string>>{
             {"debug", "insmod", "module.ko"},
             {"debug", "insmod", "--help"},
             {"boot-patch-v2", "--boot", "boot.img", "--out", "patched.img"},
             {"boot-patch-v2", "--out=patched.img"},
             {"sumhp", "module", "add", "example.module"},
             {"sumhp", "module", "delete", "example.module"},
             {"sumhp", "module", "add", "--help"},
             {"sumhp", "module", "delete", "--help"},
             {"sumhp", "daemon", "call", "module", "add", "example.module"},
             {"sumhp", "daemon", "call", "module", "delete", "example.module"},
             {"sumhp", "daemon", "call", "daemon", "call", "module", "add", "example.module"}}) {
        ksud::CliArguments parsed;
        assert(ksud::parse_cli(command, parsed) == 2);
    }

    // Keep the supported spellings, short options, and the installer's real copy option.
    {
        ksud::CliArguments parsed;
        assert(ksud::parse_cli({"install", "--magiskboot", "/tmp/magiskboot"}, parsed) == -1);
        assert(parsed.value("--magiskboot") == "/tmp/magiskboot");
        assert(ksud::parse_cli({"boot-patch", "--out", "/tmp/output"}, parsed) == -1);
        assert(parsed.value("--out") == "/tmp/output");
        for (const auto* option : {"--output", "-o"}) {
            assert(ksud::parse_cli({"boot-patch-v2", "-b", "boot.img", option, "patched.img"},
                                   parsed) == -1);
            assert(parsed.value("--output") == "patched.img");
            assert(parsed.value("--boot") == "boot.img");
        }
        assert(ksud::parse_cli({"insmod", "module.ko", "key=value", "--module-param"}, parsed) ==
               -1);
        assert(
            (parsed.args == std::vector<std::string>{"module.ko", "key=value", "--module-param"}));
        for (const auto* operation : {"hot-mount", "hot-unmount"}) {
            assert(ksud::parse_cli({"sumhp", "module", operation, "example.module"}, parsed) == -1);
            assert(
                (parsed.args == std::vector<std::string>{"module", operation, "example.module"}));
            assert(
                ksud::parse_cli({"sumhp", "daemon", "call", "module", operation, "example.module"},
                                parsed) == -1);
            assert((parsed.args == std::vector<std::string>{"daemon", "call", "module", operation,
                                                            "example.module"}));
        }
    }

    // All former signature-registration entry points must fail during parsing,
    // before command dispatch can touch persisted trust or kernel state.
    const std::string digest(64, 'a');
    const std::vector<std::vector<std::string>> retired_commands = {
        {"dynamic"},
        {"dynamic", "get-sign", "/data/app/base.apk"},
        {"dynamic", "get-sign", "--uid", "10001", "--json"},
        {"dynamic", "set-hash", "100", digest},
        {"dynamic", "set-apk", "/data/app/base.apk"},
        {"dynamic", "set-uid", "10001"},
        {"dynamic", "list"},
        {"dynamic", "del", "100", digest},
        {"dynamic", "clear"},
        {"dynamic", "--help"},
    };
    for (const auto& command : retired_commands) {
        ksud::CliArguments parsed;
        assert(ksud::parse_cli(command, parsed) == 2);
    }

    // Manual unmount commands must never reach the SUMHP-owned mount registry.
    for (const auto& command :
         std::vector<std::vector<std::string>>{{"umount"},
                                               {"umount", "--help"},
                                               {"umount", "add", "/system/app", "--flags", "2"},
                                               {"umount", "del", "/system/app"},
                                               {"umount", "remove", "/system/app"},
                                               {"umount", "list"},
                                               {"umount", "save"},
                                               {"umount", "apply"},
                                               {"umount", "clear-custom"},
                                               {"kernel", "umount", "--help"},
                                               {"kernel", "umount", "add", "/system/app"},
                                               {"kernel", "umount", "del", "/system/app"},
                                               {"kernel", "umount", "wipe"}}) {
        ksud::CliArguments parsed;
        assert(ksud::parse_cli(command, parsed) == 2);
    }

    // Existing daemon and module commands still reach their normal dispatch.
    for (const auto& command :
         std::vector<std::vector<std::string>>{{"post-fs-data"},
                                               {"services"},
                                               {"boot-completed"},
                                               {"module", "list"},
                                               {"sumhp", "module", "unmount"},
                                               {"kernel", "notify-module-mounted"}}) {
        ksud::CliArguments parsed;
        assert(ksud::parse_cli(command, parsed) == -1);
        assert(parsed.command == command.front());
    }
    std::cout << "cli_args_test: all checks passed\n";
}
