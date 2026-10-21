"""Production diagnostic package store + real privacy model, portable I/O faults.

Fresh source-linked doctor fixtures are generated in a temporary directory.
The store's full namespace is compiled with syscall/type/flag substitutions;
the privacy model is linked unchanged. This is not Linux locking, SHA-256
cryptographic, SELinux, namespace, device, or power-loss verification.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def store_source(source):
    result = source[source.index("namespace sumhp::diagnostic_package {"):]
    result = re.sub(r"\bstruct\s+stat\b", "struct ::package_io::Metadata", result)
    for name in ("open", "close", "fstat", "lstat", "read", "write", "flock", "fsync", "rename", "unlink"):
        result = result.replace("::" + name + "(", "::package_io::" + name + "(")
    constants = {"O_RDONLY": "rdonly", "O_WRONLY": "wronly", "O_RDWR": "rdwr", "O_CREAT": "creat",
                 "O_EXCL": "excl", "O_NONBLOCK": "nonblock", "O_CLOEXEC": "cloexec",
                 "O_NOFOLLOW": "nofollow", "O_DIRECTORY": "directory", "LOCK_EX": "lock_ex", "LOCK_NB": "lock_nb"}
    for old, new in constants.items():
        result = re.sub(r"\b" + old + r"\b", "::package_io::" + new, result)
    result = re.sub(r"\bS_ISREG\(([^()]*)\)", r"((\1 & ::package_io::mask) == ::package_io::reg)", result)
    result = re.sub(r"\bS_ISDIR\(([^()]*)\)", r"((\1 & ::package_io::mask) == ::package_io::dir)", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    ksud = root / "userspace/ksud"
    source = ksud / "sumhp/src"
    compiler = [args.zig, "c++"] if args.zig else [args.cxx]
    with tempfile.TemporaryDirectory(prefix="diagnostic-package-store-") as temporary:
        build = Path(temporary)
        fixtures = build / "fixtures"
        subprocess.run([sys.executable, str(ksud / "tests/run_diagnostics_model_host_tests.py"),
                        *( ["--zig", args.zig] if args.zig else ["--cxx", args.cxx] ),
                        "--fixture-directory", str(fixtures)], check=True)
        production = (source / "core/diagnostic_package_store.cpp").read_text(encoding="utf-8")
        (build / "diagnostic_package_store_under_test.inc").write_text(store_source(production), encoding="utf-8")
        binary = build / ("diagnostic_package_store_test.exe" if os.name == "nt" else "diagnostic_package_store_test")
        subprocess.run([*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(root), "-I" + str(source), "-I" + str(build),
                        str(ksud / "tests/diagnostic_package_store_test.cpp"),
                        str(source / "core/diagnostic_package_model.cpp"), "-o", str(binary)], check=True)
        subprocess.run([str(binary), str(fixtures)], check=True, timeout=90)


if __name__ == "__main__":
    main()
