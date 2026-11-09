"""Test OverlayFS attribute registration using production mount orchestration.

Linux: python3 userspace/ksud/tests/run_overlay_mount_host_tests.py --cxx g++
Windows: python run_overlay_mount_host_tests.py --zig /path/to/zig.exe
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
    source = (tests.parent / "sumhp/src/mount/overlayfs.cpp").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="overlay-mount-host-tests-") as temporary:
        build = Path(temporary)
        start = source.index("struct AttachedMount {")
        end = source.index("\n};", start) + 3
        (build / "overlay_record_under_test.inc").write_text(
            source[start:end] + "\n" + function(source, "bool record_mount("), encoding="utf-8")
        start = source.index("struct MountPointIndex {")
        end = source.index("\n};", start) + 3
        (build / "overlay_index_under_test.inc").write_text(
            function(source, "bool starts_with(") + source[start:end] + "\n", encoding="utf-8")
        common = (tests.parent / "sumhp/src/mount/mount_fs.cpp").read_text(encoding="utf-8")
        (build / "mount_decode_under_test.inc").write_text(
            function(common, "std::string decode_mount_path("), encoding="utf-8")
        (build / "mount_journal_under_test.inc").write_text(
            function(common, "bool write_mount_journal(const std::string& journal, const std::vector<MountRecord>&") +
            function(common, "bool write_mount_journal(const std::string& journal, const std::vector<std::string>&"),
            encoding="utf-8")
        (build / "overlay_mount_under_test.inc").write_text(
            function(source, "bool rollback_mounts(") +
            function(source, "bool mount_modules(") + function(source, "bool unmount_all(") +
            function(source, "bool restore_xattr_hiding("),
            encoding="utf-8")
        start = source.index("struct LayerSelection {")
        end = source.index("\n};", start) + 3
        (build / "overlay_layers_under_test.inc").write_text(
            source[start:end] + "\n" + "\n".join(function(source, signature) for signature in [
                "bool select_layers(", "bool mount_overlayfs(", "bool mount_overlay_child(",
                "bool mount_overlay("]), encoding="utf-8")
        for name in ["overlay_mount_test", "overlay_layer_selection_test"]:
            binary = build / (name + (".exe" if os.name == "nt" else ""))
            subprocess.run([*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(build), str(tests / (name + ".cpp")),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
