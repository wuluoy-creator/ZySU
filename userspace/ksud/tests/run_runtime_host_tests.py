"""Run ksud process and file I/O regressions on a POSIX host.

Linux: python3 userspace/ksud/tests/run_runtime_host_tests.py
Windows cross-check: add --zig /path/to/zig.exe --target x86_64-linux-musl --compile-only
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--zig")
    parser.add_argument("--target", help="Optional Zig cross-compilation target")
    parser.add_argument("--compile-only", action="store_true")
    parser.add_argument("--portable-only", action="store_true",
                        help="Run descriptor fault injection without POSIX processes")
    args = parser.parse_args()
    if args.target and not args.zig:
        parser.error("--target requires --zig")
    compiler = [args.zig, "c++"] if args.zig else [args.cxx]
    if args.target:
        compiler += ["-target", args.target]
    ksud = Path(__file__).resolve().parents[1]
    source = (ksud / "src/utils.cpp").read_text()
    # Compile these production functions directly; the remaining utils.cpp
    # dependencies (SELinux, ioctl, miniz, embedded applets) are unrelated here.
    functions = []
    for name in ["bool copy_file_contents_impl(", "bool copy_file_data(",
                 "bool ensure_dir_exists(", "bool ensure_binary(",
                 "bool write_bytes_impl(", "bool write_file_bytes(", "bool write_file_atomic(",
                 "DurableWriteResult replace_file_durable(", "bool write_file_durable(",
                 "ExecResult exec_command_magiskboot("]:
        begin = source.index(name)
        end = source.index("\n}", begin) + len("\n}")
        functions.append(source[begin:end])
    with tempfile.TemporaryDirectory(prefix="ksud-runtime-tests-") as temporary:
        build = Path(temporary)
        if not args.target:
            portable = build / ("capture_pipe_test.exe" if os.name == "nt" else "capture_pipe_test")
            subprocess.run([
                *compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ksud / "src"), str(ksud / "tests/capture_pipe_test.cpp"),
                "-o", str(portable),
            ], check=True)
            if not args.compile_only:
                subprocess.run([str(portable)], check=True, timeout=30)
        durable_functions = []
        for name in ["bool write_file_atomic(", "DurableWriteResult replace_file_durable(",
                     "bool write_file_durable("]:
            begin = source.index(name)
            end = source.index("\n}", begin) + len("\n}")
            durable_functions.append(source[begin:end])
        (build / "durable_file_under_test.inc").write_text("\n\n".join(durable_functions))
        durable = build / ("durable_file_test.exe" if os.name == "nt" and not args.target
                           else "durable_file_test")
        subprocess.run([
            *compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-I" + str(ksud / "src"), "-I" + str(build),
            str(ksud / "tests/durable_file_test.cpp"), "-o", str(durable),
        ], check=True)
        if not args.compile_only:
            subprocess.run([str(durable)], check=True, timeout=30)
        if args.portable_only:
            return
        (build / "runtime_io_under_test.inc").write_text("\n\n".join(functions))
        binary = build / "runtime_io_test"
        subprocess.run([
            *compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-I" + str(ksud / "src"), "-I" + str(build),
            str(ksud / "tests/runtime_io_test.cpp"), str(ksud / "src/command_exec.cpp"),
            "-o", str(binary),
        ], check=True)
        if args.compile_only:
            print("Runtime I/O regression binary compiled; execution skipped as requested.")
        else:
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
