"""Run the production SUMH planner with host filesystem/namespace doubles.

Virtual Android paths and symlinks are doubled; module contents use temporary
real files, so this suite runs on Windows without creating symlinks or devices.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2] + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--zig")
    args = parser.parse_args()
    compiler = [args.zig, "c++"] if args.zig else [args.cxx]
    root = Path(__file__).resolve().parents[3]
    tests = root / "userspace/ksud/tests"
    source = (root / "userspace/ksud/sumhp/src/mount/sumh.cpp").read_text(encoding="utf-8")
    header = (root / "userspace/ksud/sumhp/src/mount/sumh.hpp").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="sumh-planner-") as temporary:
        build = Path(temporary)
        begin = header.index("struct ModuleMountPlan {")
        end = header.index("\nbool prepare_modules(", begin)
        body = header[begin:end]
        begin = source.index("bool path_matches_rule(")
        end = source.index("\nbool disable_kernel_features(", begin)
        body += source[begin:end]
        begin = source.index("bool prepare_modules_impl(")
        end = source.index("\nbool unmount_all(", begin)
        body += source[begin:end]
        (build / "sumh_planner_under_test.inc").write_text(body, encoding="utf-8")
        common = (root / "userspace/ksud/sumhp/src/mount/mount_fs.cpp").read_text(encoding="utf-8")
        magic = (root / "userspace/ksud/sumhp/src/mount/magic_mount.cpp").read_text(encoding="utf-8")
        (build / "sumh_whiteout_under_test.inc").write_text(
            function(common, "bool stat_is_whiteout(") +
            function(common, "bool path_is_whiteout(") +
            function(magic, "NType type_from_lstat("), encoding="utf-8")
        binary = build / ("sumh_planner_test.exe" if os.name == "nt" else "sumh_planner_test")
        subprocess.run([*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(build), str(tests / "sumh_planner_test.cpp"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
