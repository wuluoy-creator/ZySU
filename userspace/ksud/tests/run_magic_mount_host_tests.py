"""Exercise Magic Mount transactions and traversal without root or Android.

Linux: python3 userspace/ksud/tests/run_magic_mount_host_tests.py --cxx g++
Windows: python run_magic_mount_host_tests.py --zig /path/to/zig.exe
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
    tests = Path(__file__).resolve().parent
    source_dir = tests.parent / "sumhp/src"
    with tempfile.TemporaryDirectory(prefix="magic-mount-host-tests-") as temporary:
        build = Path(temporary)
        source = (source_dir / "mount/magic_mount.cpp").read_text(encoding="utf-8")
        signatures = [
            "std::string state_file(", "bool ksu_umount_add(", "bool read_mounts(",
            "bool rollback(", "bool unmount_all(", "bool save_mounts(",
            "bool mount_modules(",
        ]
        (build / "magic_mount_under_test.inc").write_text(
            "\n".join(function(source, signature) for signature in signatures),
            encoding="utf-8")
        (build / "magic_traversal_under_test.inc").write_text(
            function(source, "bool do_directory(") +
            function(source, "bool do_mount(Node& node, const std::string& real, "
                     "const std::string& work, bool has_tmpfs,\n              Walk& w) {"),
            encoding="utf-8")
        source = (source_dir / "embedded.cpp").read_text(encoding="utf-8")
        (build / "embedded_umount_under_test.inc").write_text(
            function(source, "bool embedded_register_umount(") +
            function(source, "bool embedded_unregister_umount("), encoding="utf-8")
        for test in ("magic_mount_lifecycle_test", "magic_mount_traversal_test"):
            binary = build / (test + ".exe" if os.name == "nt" else test)
            subprocess.run([*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(build), str(tests / (test + ".cpp")),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
