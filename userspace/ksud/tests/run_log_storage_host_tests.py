"""Compile/run the production logger against a portable filesystem fault model.

Windows: add --zig /path/to/zig.exe. --target requires --compile-only; a cross
compile does not establish Linux filesystem, process-locking, or crash behavior.
The runner compiles the full production logging namespace, replacing syscall
identifiers only, and links the production repeat filter without alteration.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def model_source(source):
    start = source.index("namespace sumhp::logging {")
    result = source[start:]
    result = re.sub(r"\bstruct\s+stat\b", "struct ::logfs::ModelStat", result)
    result = re.sub(r"\boff_t\b", "::logfs::ModelOffset", result)
    result = re.sub(r"\bpid_t\b", "::logfs::ModelPid", result)
    # Record fallback emission without opening host stderr paths. Windows uses
    # wide filesystem path::c_str(); formatting is outside this POSIX model.
    result = result.replace("std::fprintf(", "::logfs::fallback_fprintf(")
    result = result.replace("std::fputs(", "::logfs::fallback_fputs(")
    constants = {
        "O_RDONLY": "read_only", "O_WRONLY": "write_only", "O_RDWR": "read_write",
        "O_CREAT": "create", "O_EXCL": "exclusive", "O_TRUNC": "truncate",
        "O_APPEND": "append", "O_CLOEXEC": "cloexec", "O_NOFOLLOW": "nofollow",
        "O_NONBLOCK": "nonblock", "O_DIRECTORY": "directory",
        "LOCK_EX": "lock_exclusive", "LOCK_UN": "lock_unlock",
    }
    for original, replacement in constants.items():
        result = re.sub(r"\b" + original + r"\b", "::logfs::" + replacement, result)
    result = re.sub(r"\bS_ISREG\(([^()]*)\)",
                    r"((\1 & ::logfs::type_mask) == ::logfs::regular_type)", result)
    result = re.sub(r"\bS_ISDIR\(([^()]*)\)",
                    r"((\1 & ::logfs::type_mask) == ::logfs::directory_type)", result)
    names = ("open", "read", "pread", "close", "fstat", "lstat", "fchmod", "flock",
             "lseek", "ftruncate", "fsync", "rename", "unlink", "localtime_r", "clock_gettime", "getpid")
    for name in names:
        result = re.sub(r"(?<![\w:])(?:::)?" + name + r"\s*\(",
                        "::logfs::" + name + "(", result)
    # The logger's public write(Level,...) remains untouched. Only qualified
    # POSIX descriptor writes are redirected; fail closed if that seam changes.
    if "::write(" not in result:
        raise ValueError("production descriptor write seam changed; review extraction")
    result = result.replace("::write(", "::logfs::write(")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--zig")
    parser.add_argument("--target")
    parser.add_argument("--compile-only", action="store_true")
    args = parser.parse_args()
    if args.target and (not args.zig or not args.compile_only):
        parser.error("--target requires --zig and --compile-only")
    compiler = [args.zig, "c++"] if args.zig else [args.cxx]
    if args.target:
        compiler += ["-target", args.target]
    ksud = Path(__file__).resolve().parents[1]
    source = (ksud / "sumhp/src/core/log.cpp").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="sumhp-log-storage-tests-") as temporary:
        build = Path(temporary)
        (build / "log_storage_under_test.inc").write_text(model_source(source), encoding="utf-8")
        binary = build / ("log_storage_test.exe" if os.name == "nt" and not args.target
                          else "log_storage_test")
        subprocess.run([
            *compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-I" + str(build), "-I" + str(ksud / "sumhp/src"),
            str(ksud / "tests/log_storage_test.cpp"),
            str(ksud / "sumhp/src/core/log_repeat.cpp"), "-o", str(binary),
        ], check=True)
        if args.compile_only:
            print("Production logger fault model compiled; execution skipped as requested.")
        else:
            subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
