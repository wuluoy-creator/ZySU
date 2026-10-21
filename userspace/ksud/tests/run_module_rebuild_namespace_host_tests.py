"""Execute production module rebuild namespace/outcome and reset functions with host doubles.

No fork, mount, kernel ioctl, or device mutation is performed. The extracted
production functions are unchanged; only OS/backend boundaries are doubled.
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
    source = (root / "userspace/ksud/sumhp/src/mount/sumh.cpp").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="sumh-rebuild-namespace-") as temporary:
        build = Path(temporary)
        extracted = "\n".join(function(source, signature) for signature in (
            "bool disable_kernel_features(", "bool reset_feature_state(",
            "bool reset_runtime_state(", "bool rebuild_modules_in_init_ns("))
        (build / "sumh_rebuild_namespace_under_test.inc").write_text(extracted, encoding="utf-8")
        binary = build / ("sumh_rebuild_namespace_test.exe" if os.name == "nt" else "sumh_rebuild_namespace_test")
        subprocess.run([*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(root / "userspace/ksud/sumhp/include"),
                        "-I" + str(root / "userspace/ksud/sumhp/src"),
                        "-I" + str(build),
                        str(root / "userspace/ksud/tests/sumh_rebuild_namespace_test.cpp"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
