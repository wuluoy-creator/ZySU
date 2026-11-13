"""Compile and run the real Manager helper with dummy authorization and I/O.

Linux only; does not require Android, root, network, or an installed daemon.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    ksud = Path(__file__).resolve().parents[1]
    repo = ksud.parents[1]
    with tempfile.TemporaryDirectory(prefix="manager-helper-tests-") as temporary:
        binary = Path(temporary) / "manager_helper_test"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                   "-I" + str(repo), "-I" + str(ksud / "src"),
                   "-I" + str(ksud / "sumhp/include"),
                   str(ksud / "tests/manager_helper_test.cpp"),
                   str(ksud / "src/manager_helper.cpp"),
                   str(ksud / "sumhp/src/mount/sumh_client.cpp"),
                   "-Wl,--wrap=getuid", "-Wl,--wrap=geteuid", "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
