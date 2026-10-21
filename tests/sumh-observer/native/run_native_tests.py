#!/usr/bin/env python3
"""Compile and execute actual observer codec/collector with bounded host doubles.

Requires a C/C++17 host compiler (or --zig) and the pinned mbedTLS 3.6.2
source already obtained by CMake. This driver does not install or run an APK.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
NATIVE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--mbedtls-source", type=Path)
    parser.add_argument("--export-model", type=Path, help="Export explicitly synthetic model JSON for cross-codec validation")
    args = parser.parse_args()
    candidates = [args.mbedtls_source] if args.mbedtls_source else [
        ROOT / "out/sumh-maps-native/_deps/mbedtls-src",
        ROOT / "out/batch23-native-android/_deps/mbedtls-src",
    ]
    mbed = next((p.resolve() for p in candidates if p and (p / "include/mbedtls/sha256.h").is_file()), None)
    if not mbed:
        parser.error("configure native CMake first or pass --mbedtls-source for pinned 3.6.2")
    version = (mbed / "include/mbedtls/build_info.h").read_text(encoding="utf-8")
    if 'MBEDTLS_VERSION_STRING         "3.6.2"' not in version:
        parser.error("mbedtls source must be version 3.6.2")
    cxx = [args.zig, "c++"] if args.zig else [args.cxx]
    cc = [args.zig, "cc"] if args.zig else [args.cc]
    includes = ["-I" + str(NATIVE), "-I" + str(ROOT / "userspace/common"), "-I" + str(mbed / "include")]
    definitions = ['-DMBEDTLS_CONFIG_FILE="observer_mbedtls_config.h"']
    with tempfile.TemporaryDirectory(prefix="sumh-observer-native-") as tmp:
        work = Path(tmp)
        objects = []
        for name in ("sha256", "platform_util"):
            obj = work / (name + ".o")
            subprocess.run([*cc, "-std=c11", *includes, *definitions, "-c", str(mbed / "library" / (name + ".c")), "-o", str(obj)], check=True)
            objects.append(str(obj))
        programs = [("protocol", NATIVE / "protocol_test.cpp")]
        model = NATIVE / "collector_model_test.cpp"
        model_api = NATIVE / "collector_model_syscalls.hpp"
        if not model.is_file() or not model_api.is_file():
            parser.error("complete collector model source and syscall fixture are required")
        source = (NATIVE / "collector.cpp").read_text(encoding="utf-8")
        forbidden = ("dirent.h", "fcntl.h", "sys/mman.h", "sys/random.h", "sys/stat.h", "sys/syscall.h", "time.h", "unistd.h")
        for name in forbidden:
            source = source.replace("#include <" + name + ">", "")
        source = source.replace("struct stat", "struct model_stat")
        generated = work / "collector_model.cpp"
        generated.write_text('#include "collector_model_syscalls.hpp"\n' + source + '\n#include "collector_model_test.cpp"\n', encoding="utf-8")
        programs.append(("collector-model", generated))
        for name, src in programs:
            exe = work / (name + (".exe" if os.name == "nt" else ""))
            subprocess.run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *includes, *definitions,
                            '-DSUMH_OBSERVER_BUILD_ID="host-test-source"', str(src), str(NATIVE / "protocol.cpp"),
                            *objects, "-o", str(exe)], check=True)
            export = []
            if name == "collector-model" and args.export_model:
                args.export_model.mkdir(parents=True, exist_ok=True)
                export = ["--export", str(args.export_model.resolve())]
            subprocess.run([str(exe), *export], check=True)


if __name__ == "__main__":
    main()
