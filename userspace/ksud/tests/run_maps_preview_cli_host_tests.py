"""Run the production main -> cli_run -> embedded preview routing chain.

Uses the actual generic parser for adjacent commands. Android command actions,
query execution, log/signal setup, root identity and exec calls are counted
fixtures; the transport and sampler have separate fault-model suites.
"""
import argparse
import os
import re
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    src = root / "userspace/ksud/src"
    sumhp = root / "userspace/ksud/sumhp/src"
    with tempfile.TemporaryDirectory(prefix="maps-preview-cli-") as temporary:
        build = Path(temporary)
        body = (src / "cli.cpp").read_text(encoding="utf-8")
        body = body[body.index("int cli_run("):]
        body = body[:body.index("\n}  // namespace ksud")]
        for name in ("setenv", "execv", "_exit"):
            body = body.replace(name + "(", "cli_model::" + name + "(")
        (build / "cli_run_under_test.inc").write_text(body, encoding="utf-8")
        body = (sumhp / "embedded.cpp").read_text(encoding="utf-8")
        body = body[body.index("int embedded_command("):body.index("\nint embedded_mount(")]
        body = body.replace("geteuid()", "cli_model::geteuid()")
        (build / "cli_embedded_under_test.inc").write_text(body, encoding="utf-8")
        hide = (sumhp / "core/user_hide.cpp").read_text(encoding="utf-8")
        start = hide.index("std::string manager_dispatch_json(")
        (build / "cli_hide_dispatch_under_test.inc").write_text(hide[start:hide.index("\nnamespace {",start)],encoding="utf-8")
        body = (sumhp / "mount/maps_preview.cpp").read_text(encoding="utf-8")
        body = body[body.index("bool is_maps_preview_command("):body.index("\nstd::string maps_preview_json(")]
        (build / "cli_preview_match_under_test.inc").write_text(body, encoding="utf-8")
        body = (sumhp / "mount/module_preview_input.cpp").read_text(encoding="utf-8")
        body = body[body.index("bool is_module_preview_prefix("):body.index("\nbool apply_module_preview_input(")]
        backend = (sumhp / "mount/backend.cpp").read_text(encoding="utf-8")
        start = backend.index("bool valid_stored_module_rule_path(")
        paths = backend[start:backend.index("\nJsonValue module_rules_json(", start)]
        start = backend.index("bool valid_module_rule_path(")
        paths += backend[start:backend.index("\nbool parse_module_rules(", start)]
        (build / "cli_module_preview_match_under_test.inc").write_text(paths + body, encoding="utf-8")
        config = (sumhp / "core/config.cpp").read_text(encoding="utf-8")
        start = config.index("bool valid_config_revision(")
        (build / "cli_revision_under_test.inc").write_text(config[start:config.index("\nbool parse_partition_config(", start)], encoding="utf-8")
        development = (sumhp / "mount/development_input.cpp").read_text(encoding="utf-8")
        namespace_end = list(re.finditer(r"^}\s*// namespace", development, re.M))[-1].start()
        (build / "cli_development_under_test.inc").write_text(development[development.index("bool is_development_prefix("):namespace_end], encoding="utf-8")
        inventory = (sumhp / "core/rule_inventory.hpp").read_text(encoding="utf-8")
        start = inventory.index("bool valid_utf8(")
        end = inventory.index("\ninline std::string", start)
        (build / "cli_utf8_under_test.inc").write_text(inventory[start:end], encoding="utf-8")
        submission = (sumhp / "core/operation_submit_input.cpp").read_text(encoding="utf-8")
        start = submission.index("bool input_path_valid(")
        helper = submission[start:submission.index("\nbool validate_command(", start)]
        start = submission.index("bool parse_operation_submit_arguments(")
        (build / "cli_operation_submit_arguments_under_test.inc").write_text(
            helper + submission[start:submission.index("\nbool parse_operation_input(", start)], encoding="utf-8")
        profile = (sumhp / "core/profile_input.cpp").read_text(encoding="utf-8")
        start = profile.index("namespace sumhp::profiles {") + len("namespace sumhp::profiles {")
        (build / "cli_profile_arguments_under_test.inc").write_text(
            profile[start:profile.index("\nbool read_profile_input(", start)], encoding="utf-8")
        model = (sumhp / "core/profile_model.cpp").read_text(encoding="utf-8")
        start = model.index("bool valid_profile_id(")
        (build / "cli_profile_id_under_test.inc").write_text(
            model[start:model.index("\nbool parse_profile(", start)], encoding="utf-8")
        command = (sumhp / "core/command.cpp").read_text(encoding="utf-8")
        start = command.index("int run_command(")
        end = command.index('    if (args.empty()', start)
        (build / "cli_operation_daemon_rejection_under_test.inc").write_text(
            command[start:end] + "    return 83;\n}\n", encoding="utf-8")
        daemon = (sumhp / "core/daemon.cpp").read_text(encoding="utf-8")
        start = daemon.index("std::string response_json(")
        (build / "cli_operation_envelope_under_test.inc").write_text(
            daemon[start:daemon.index("\nstd::string status_json(", start)], encoding="utf-8")
        start = command.index("int handle_module(")
        prefix = command[start:command.index('    if (sub == "list") {', start)]
        (build / "cli_module_daemon_rejection_under_test.inc").write_text(prefix + "    return 83;\n}\n", encoding="utf-8")
        start = command.index("int print_meta_json(")
        (build / "cli_module_meta_under_test.inc").write_text(
            command[start:command.index("\nint print_mount_status_json(", start)], encoding="utf-8")
        body = (src / "main.cpp").read_text(encoding="utf-8").replace("int main(", "int production_main(")
        (build / "main_under_test.inc").write_text(body, encoding="utf-8")
        compiler = [args.zig, "c++"] if args.zig else [args.cxx]
        if os.name == "nt":
            compiler += ["-DESTALE=116", "-DESHUTDOWN=108"]
        binary = build / ("maps_preview_cli_test.exe" if os.name == "nt" else "maps_preview_cli_test")
        subprocess.run([*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
            "-I" + str(root), "-I" + str(build), "-I" + str(sumhp), "-I" + str(src),
            "-I" + str(sumhp.parent / "include"),
            str(root / "userspace/ksud/tests/maps_preview_cli_test.cpp"),
            str(sumhp / "core/state_ledger_model.cpp"), str(sumhp / "core/operation_query_input.cpp"),
            str(src / "cli_args.cpp"), str(src / "terminal.cpp"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)
        parser_binary = build / ("cli_args_test.exe" if os.name == "nt" else "cli_args_test")
        subprocess.run([*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
            "-I" + str(src), str(root / "userspace/ksud/tests/cli_args_test.cpp"),
            str(src / "cli_args.cpp"), str(src / "terminal.cpp"), "-o", str(parser_binary)], check=True)
        subprocess.run([str(parser_binary)], check=True, timeout=30)
        log_source = (src / "log.cpp").read_text(encoding="utf-8").replace("fopen(", "preview_log_open(")
        (build / "log_probe.hpp").write_text("#include <cstdio>\nFILE* preview_log_open(const char*, const char*);\n", encoding="utf-8")
        log_source = '#include "log_probe.hpp"\n' + log_source
        (build / "log_under_test.cpp").write_text(log_source, encoding="utf-8")
        log_binary = build / ("maps_preview_log_test.exe" if os.name == "nt" else "maps_preview_log_test")
        # Existing production logger computes an unused Android numeric level;
        # keep that warning visible without treating it as a new test failure.
        subprocess.run([*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-error=unused-but-set-variable",
            "-I" + str(src), "-I" + str(build),
            str(root / "userspace/ksud/tests/maps_preview_log_test.cpp"),
            str(build / "log_under_test.cpp"), str(src / "terminal.cpp"), "-o", str(log_binary)], check=True)
        subprocess.run([str(log_binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
