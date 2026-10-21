"""Exercise the boot mount-to-kernel handoff without Android or root.

Linux: python3 userspace/ksud/tests/run_boot_mount_notify_host_tests.py --cxx g++
Windows: python run_boot_mount_notify_host_tests.py --zig /path/to/zig.exe
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
    parser.add_argument("--zig", help="Use Zig's bundled C++ compiler")
    parser.add_argument("--compile-only", action="store_true")
    args = parser.parse_args()
    compiler = [args.zig, "c++"] if args.zig else [args.cxx]
    tests = Path(__file__).resolve().parent
    source_dir = tests.parent / "src"
    source = (source_dir / "init_event.cpp").read_text(encoding="utf-8")

    with tempfile.TemporaryDirectory(prefix="boot-mount-notify-host-tests-") as temporary:
        build = Path(temporary)
        (build / "boot_mount_notify_under_test.inc").write_text(
            function(source, "int on_post_fs_data() {"), encoding="utf-8")
        binary = build / ("boot_mount_notify_test.exe" if os.name == "nt"
                          else "boot_mount_notify_test")
        subprocess.run([*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(build), "-I" + str(source_dir),
                        str(tests / "boot_mount_notify_test.cpp"), "-o", str(binary)],
                       check=True)
        if args.compile_only:
            print("Boot mount notification regression compiled; execution skipped as requested.")
        else:
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
