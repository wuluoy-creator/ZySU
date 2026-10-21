"""Run the production read-only query worker with injected syscalls.

The portable model runs on Windows too. POSIX additionally runs real fork,
socket/pidfd transfer, timeout and reaping smoke tests (namespace entry stubbed).
No Android device/kernel behavior is inferred from these host checks.
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
    parser.add_argument("--target", help="Optional Zig cross-compile target")
    parser.add_argument("--compile-only", action="store_true")
    args = parser.parse_args()
    if args.target and not args.zig:
        parser.error("--target requires --zig")
    root = Path(__file__).resolve().parents[3]
    source = root / "userspace/ksud/sumhp/src/mount/query_worker.cpp"
    compiler = [args.zig, "c++"] if args.zig else [args.cxx]
    if args.target:
        compiler += ["-target", args.target]
    with tempfile.TemporaryDirectory(prefix="sumh-query-worker-") as directory:
        build = Path(directory)
        body = source.read_text(encoding="utf-8")
        body = body[body.index("namespace sumhp::mount::fsutil {"):]
        # Only redirect syscall names; compile all production control flow,
        # handshake, packet validation, cleanup and timeout code unchanged.
        calls = ("syscall", "getpid", "close", "clock_gettime", "sendmsg", "_exit",
                 "recv", "recvmsg", "waitid", "socketpair", "fork", "send", "nanosleep")
        for call in calls:
            body = body.replace("::" + call + "(", "query_model::" + call + "(")
        (build / "query_worker_under_test.inc").write_text(body, encoding="utf-8")
        model = build / ("query_worker_test.exe" if os.name == "nt" and not args.target else "query_worker_test")
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread"]
        subprocess.run([*compiler, *flags, "-I" + str(build),
            str(root / "userspace/ksud/tests/query_worker_test.cpp"), "-o", str(model)], check=True)
        if not args.compile_only:
            subprocess.run([str(model)], check=True, timeout=30)
        if os.name != "nt" or args.target:
            (build / "utils.hpp").write_text("#pragma once\nnamespace ksud { bool switch_mnt_ns(int); }\n")
            smoke = build / "query_worker_posix_test"
            # Zig's musl CMSG_NXTHDR macro itself compares size_t/ptrdiff_t;
            # leave its warning visible without rejecting the cross-build.
            posix_flags = flags + (["-Wno-error=sign-compare"] if args.target and "musl" in args.target else [])
            subprocess.run([*compiler, *posix_flags, "-I" + str(build),
                "-I" + str(root / "userspace/ksud/sumhp/src"), str(source),
                str(root / "userspace/ksud/tests/query_worker_posix_test.cpp"), "-o", str(smoke)], check=True)
            if not args.compile_only:
                subprocess.run([str(smoke)], check=True, timeout=20)
        else:
            print("POSIX fork/pidfd smoke not run on Windows; syscall-model evidence only.")


if __name__ == "__main__":
    main()
