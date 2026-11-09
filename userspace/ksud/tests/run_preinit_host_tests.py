"""Exercise production preinit synchronization using temporary directories."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--zig")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    compiler = [args.zig, "c++"] if args.zig else [args.cxx]
    tests = Path(__file__).resolve().parent
    source = tests.parent / "src"
    with tempfile.TemporaryDirectory(prefix="preinit-tests-") as temporary:
        build = Path(temporary)
        binary = build / ("preinit_test.exe" if os.name == "nt" else "preinit_test")
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        subprocess.run([*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I" + str(source), str(source / "module/preinit.cpp"),
                        str(tests / "preinit_test.cpp"), "-o", str(binary)], check=True)
        subprocess.run([str(binary), str(build / "data")], check=True, timeout=60)


if __name__ == "__main__":
    main()
