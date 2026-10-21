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

    // Existing daemon and module commands still reach their normal dispatch.
    for (const auto& command : std::vector<std::vector<std::string>>{
             {"post-fs-data"}, {"services"}, {"boot-completed"}, {"module", "list"}}) {
        ksud::CliArguments parsed;
        assert(ksud::parse_cli(command, parsed) == -1);
        assert(parsed.command == command.front());
    }
    // Public preview is discoverable by the command parser. Its private framed
    // transport is accepted only by cli_run's exact root-checked direct route.
    ksud::CliArguments preview;
    assert(ksud::parse_cli({"sumhp", "sumh", "maps", "preview"}, preview) == -1);
    assert(preview.path == "sumhp sumh maps preview" &&
           preview.args == std::vector<std::string>({"sumh", "maps", "preview"}));
    assert(ksud::parse_cli({"sumhp", "sumh", "maps", "preview", "--channel"}, preview) == 2);
    for (const auto& command : std::vector<std::vector<std::string>>{
             {"sumhp", "doctor", "--input", "-"},
             {"sumhp", "doctor", "package", "list"},
             {"sumhp", "doctor", "package", "preview", "0123456789abcdef0123456789abcdef"},
             {"--verbose", "sumhp", "doctor", "package", "list"},
             {"daemon", "call", "sumhp", "doctor", "package", "list"},
             {"--verbose", "sumhp", "doctor", "--input", "-"},
             {"daemon", "call", "sumhp", "doctor", "--input", "-"},
             {"sumhp", "doctor", "manager", "{}", "--channel"}}) {
        ksud::CliArguments parsed;
        assert(ksud::parse_cli(command, parsed) == 2);
    }
    std::cout << "cli_args_test: all checks passed\n";
}
