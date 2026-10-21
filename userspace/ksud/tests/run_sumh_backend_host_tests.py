"""Run production backend selection/discovery/lifecycle with host fault fixtures.

No mounts, driver calls or device mutation. Injected filesystem failures are
model evidence; real-tree cases exercise host filesystem behavior.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--target")
    parser.add_argument("--compile-only", action="store_true")
    args = parser.parse_args()
    if args.target and not args.zig:
        parser.error("--target requires --zig")
    root = Path(__file__).resolve().parents[3]
    sumhp = root / "userspace/ksud/sumhp"
    tests = root / "userspace/ksud/tests"
    compiler = [args.zig, "c++"] if args.zig else [args.cxx]
    if args.target:
        compiler += ["-target", args.target]
    source = (sumhp / "src/mount/backend.cpp").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="sumh-backend-") as directory:
        build = Path(directory)
        def section(begin, end):
            start = source.index(begin)
            return source[start:source.index(end, start)]
        selection = section("namespace {\nbool dir_has_direct_files", "\nMountReport mount_all_enabled(")
        (build / "sumh_backend_selection_under_test.inc").write_text(selection, encoding="utf-8")
        (build / "sumh_backend_rule_guard_under_test.inc").write_text(
            section("bool validate_module_backend_rules(", "\nMountReport mount_all_enabled("), encoding="utf-8")
        (build / "sumh_module_config_under_test.inc").write_text(
            section("namespace {\nstruct ModuleConfigRecord", "\nnamespace {\nbool dir_has_direct_files"), encoding="utf-8")
        command = (sumhp / "src/core/command.cpp").read_text(encoding="utf-8")
        start = command.index('    if (sub == "configure") {', command.index("int handle_module("))
        (build / "sumh_module_commands_under_test.inc").write_text(
            command[start:command.index('    if (sub == "hot-mount"', start)], encoding="utf-8")
        discovery = section("bool enumerate_mountable_modules_checked(", "\n// Share the config/migration lock")
        for name in ("status", "exists", "directory_iterator"):
            discovery = discovery.replace("fs::" + name, "discovery_model::" + name)
        (build / "sumh_backend_discovery_under_test.inc").write_text(discovery, encoding="utf-8")
        lifecycle = section("MountReport mount_all_enabled(", "\nbool unmount_all(")
        lifecycle += section("bool refresh_sumh_modules(", "\n}  // namespace sumhp::mount")
        (build / "sumh_backend_lifecycle_under_test.inc").write_text(lifecycle, encoding="utf-8")
        features = section("bool apply_sumh_features(", "\n}  // namespace")
        (build / "sumh_backend_features_under_test.inc").write_text(features, encoding="utf-8")
        partitions = (sumhp / "src/mount/mount_fs.cpp").read_text(encoding="utf-8")
        start = partitions.index("const std::vector<std::string>& managed_partitions() {")
        partitions = partitions[start:partitions.index("\nvoid mlog(", start)]
        (build / "sumh_partitions_under_test.inc").write_text(partitions, encoding="utf-8")
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I" + str(root), "-I" + str(sumhp / "include"),
                 "-I" + str(sumhp / "src"), "-I" + str(root / "userspace/ksud/src"),
                 "-I" + str(build)]
        for name in ("sumh_backend_selection_test", "sumh_backend_discovery_test",
                     "sumh_backend_lifecycle_test", "sumh_module_config_test"):
            binary = build / (name + (".exe" if os.name == "nt" and not args.target else ""))
            subprocess.run([*compiler, *flags, str(tests / (name + ".cpp")),
                            "-o", str(binary)], check=True)
            if not args.compile_only:
                subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
