"""Compile production preview transport/worker with fault-injected syscalls.

Runs the portable model on Windows too; POSIX also runs real exec/pidfd/guard
smoke tests. Cross-compilation is not device or live POSIX runtime evidence.
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
    parser.add_argument("--target")
    parser.add_argument("--compile-only", action="store_true")
    args = parser.parse_args()
    if args.target and not args.zig:
        parser.error("--target requires --zig")
    root = Path(__file__).resolve().parents[3]
    compiler = [args.zig, "c++"] if args.zig else [args.cxx]
    if args.target:
        compiler += ["-target", args.target]
    elif os.name == "nt":
        # Linux-only errno names used by the shared Maps request helper.
        compiler += ["-DESTALE=116", "-DESHUTDOWN=108"]
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread", "-I" + str(root)]
    with tempfile.TemporaryDirectory(prefix="sumh-preview-transport-") as directory:
        build = Path(directory)
        source = root / "userspace/common/maps_preview_transport.cpp"
        body = source.read_text(encoding="utf-8")
        body = body[body.index("namespace sumhp::preview_transport {"):]
        calls = ("clock_gettime", "nanosleep", "close", "syscall", "waitid", "recvmsg",
                 "sendmsg", "socketpair", "fcntl", "posix_spawn_file_actions_init",
                 "posix_spawn_file_actions_adddup2", "posix_spawn_file_actions_addopen",
                 "posix_spawn_file_actions_addclose", "posix_spawn", "posix_spawn_file_actions_destroy",
                 "getsockopt", "getsockname", "getpid", "socket", "bind")
        for name in calls:
            body = body.replace("::" + name + "(", "transport_model::" + name + "(")
        (build / "maps_preview_transport_under_test.inc").write_text(body, encoding="utf-8")
        jni = (root / "manager/app/src/main/cpp/sumhp_jni.cpp").read_text(encoding="utf-8")
        worker = jni.split("// MAPS_PREVIEW_WORKER_BEGIN", 1)[1]
        worker = worker[worker.index("constexpr auto preview_wait_budget"):]
        worker = worker.split("// MAPS_PREVIEW_WORKER_END", 1)[0]
        worker = worker.replace("std::chrono::seconds(10)", "std::chrono::milliseconds(30)")
        (build / "maps_preview_worker_under_test.inc").write_text(worker, encoding="utf-8")
        for name in ("maps_preview_transport_test", "maps_preview_worker_test"):
            binary = build / (name + (".exe" if os.name == "nt" and not args.target else ""))
            subprocess.run([*compiler, *flags, "-I" + str(build),
                str(root / "userspace/ksud/tests" / (name + ".cpp")), "-o", str(binary)], check=True)
            if not args.compile_only:
                subprocess.run([str(binary)], check=True, timeout=30)
        if os.name != "nt" or args.target:
            smoke = build / "maps_preview_transport_posix_test"
            extra = ["-Wno-error=sign-compare"] if args.target and "musl" in args.target else []
            subprocess.run([*compiler, *flags, *extra, str(source),
                str(root / "userspace/ksud/tests/maps_preview_transport_posix_test.cpp"),
                "-o", str(smoke)], check=True)
            if not args.compile_only:
                subprocess.run([str(smoke)], check=True, timeout=30)
            diagnostic_smoke = build / "diagnostics_transport_posix_test"
            subprocess.run([*compiler, *flags, *extra,
                str(root / "userspace/ksud/tests/diagnostics_transport_posix_test.cpp"),
                "-o", str(diagnostic_smoke)], check=True)
            if not args.compile_only:
                subprocess.run([str(diagnostic_smoke)], check=True, timeout=45)
            package_smoke = build / "diagnostic_package_transport_posix_test"
            subprocess.run([*compiler, *flags, *extra,
                str(root / "userspace/ksud/tests/diagnostic_package_transport_posix_test.cpp"),
                "-o", str(package_smoke)], check=True)
            if not args.compile_only:
                subprocess.run([str(package_smoke)], check=True, timeout=45)
        else:
            print("POSIX exec/pidfd/inherited guard smoke not run on Windows; model evidence only.")


if __name__ == "__main__":
    main()
