#!/usr/bin/env python3
"""Regression-test the checker against mutations of actual built Android ELFs.

Usage: python test_native_hardening.py --build-dir <CMake build> --target ksud
       --readelf <NDK llvm-readelf> --linkage dynamic
Use --target ksuinit --linkage static for the early-init executable.
All altered files live in a temporary directory; no Android binary is executed.
"""

import argparse
import copy
import json
import shlex
import struct
import tempfile
from pathlib import Path

import check_native_hardening as check


def program_headers(data):
    assert data[:6] == b"\x7fELF\x02\x01", "ELF64 little-endian fixture required"
    offset = struct.unpack_from("<Q", data, 32)[0]
    size, count = struct.unpack_from("<HH", data, 54)
    for index in range(count):
        at = offset + index * size
        yield at, struct.unpack_from("<IIQQQQQQ", data, at)


def change_header(data, kind, flags=None):
    mutated = bytearray(data)
    for at, header in program_headers(mutated):
        if header[0] == kind:
            struct.pack_into("<I", mutated, at if flags is None else at + 4,
                             0 if flags is None else header[1] | flags)
            return mutated
    raise AssertionError(f"fixture has no program header {kind:x}")


def remove_now(data):
    mutated = bytearray(data)
    for _, header in program_headers(mutated):
        if header[0] != 2:  # PT_DYNAMIC
            continue
        for at in range(header[2], header[2] + header[5], 16):
            tag, value = struct.unpack_from("<QQ", mutated, at)
            if tag == 24:  # DT_BIND_NOW -> DT_DEBUG, which conveys no binding flags
                struct.pack_into("<QQ", mutated, at, 21, 0)
            elif tag == 30:
                struct.pack_into("<Q", mutated, at + 8, value & ~8)
            elif tag == 0x6FFFFFFB:
                struct.pack_into("<Q", mutated, at + 8, value & ~1)
    return mutated


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--target", choices=("ksud", "ksuinit"), required=True)
    parser.add_argument("--readelf", type=Path, required=True)
    parser.add_argument("--linkage", choices=("dynamic", "static"), required=True)
    args = parser.parse_args()
    binary_path = args.build_dir / args.target
    symbols_path = args.build_dir / "symbols" / (args.target + ".unstripped")
    binary = binary_path.read_bytes()
    symbols = symbols_path.read_bytes()
    b_text = check.inspect(args.readelf, binary_path)
    s_text = check.inspect(args.readelf, symbols_path)
    build_id = check.check_elf(b_text, s_text, args.linkage, True)
    db_path = args.build_dir / "compile_commands.json"
    check.check_compile_commands(db_path, True)
    tests = 1

    def rejected(name, expected, function):
        nonlocal tests
        try:
            function()
        except check.CheckError as error:
            assert expected in str(error), f"{name}: wrong rejection: {error}"
        else:
            raise AssertionError(f"{name}: checker accepted a regression")
        tests += 1

    with tempfile.TemporaryDirectory(prefix="native-hardening-") as scratch:
        scratch = Path(scratch)

        def check_changed(data, symbol_data=False):
            path = scratch / ("symbols" if symbol_data else "binary")
            path.write_bytes(data)
            text = check.inspect(args.readelf, path)
            return check.check_elf(b_text if symbol_data else text,
                                   text if symbol_data else s_text, args.linkage, True)

        rejected("RELRO removed", "GNU_RELRO", lambda: check_changed(change_header(binary, 0x6474E552)))
        rejected("executable stack", "GNU_STACK", lambda: check_changed(change_header(binary, 0x6474E551, 1)))
        # Set both W and X on a real LOAD segment.
        rejected("RWX segment", "LOAD", lambda: check_changed(change_header(binary, 1, 3)))
        if args.linkage == "dynamic":
            rejected("lazy binding", "NOW", lambda: check_changed(remove_now(binary)))
        identity = bytes.fromhex(build_id)
        different = bytes([identity[0] ^ 1]) + identity[1:]
        assert symbols.count(identity) >= 1
        rejected("stale symbols", "build IDs differ", lambda: check_changed(symbols.replace(identity, different, 1), True))
        rejected("removed build ID", "build ID", lambda: check_changed(binary.replace(b"GNU\0" + identity, b"BAD\0" + identity, 1)))
        stripped_reason = "already stripped" if args.linkage == "dynamic" else "canary failure implementation"
        rejected("stripped symbols", stripped_reason,
                 lambda: check.check_elf(b_text, b_text, args.linkage, True))
        rejected("shipping debug data", "shipping executable", lambda: check.check_elf(s_text, s_text, args.linkage, True))
        rejected("missing line tables", "line tables", lambda: check_changed(symbols.replace(b".debug_line\0", b".nodeb_line\0"), True))

        entries = json.loads(db_path.read_text(encoding="utf-8"))
        index = next(i for i, entry in enumerate(entries) if Path(entry["file"]).suffix.lower() in {".c", ".cpp", ".cc", ".cxx"})
        db_copy = scratch / "compile_commands.json"

        def changed_command(suffix, argument_array=False):
            data = copy.deepcopy(entries)
            command = data[index].get("command") or " ".join(data[index]["arguments"])
            data[index].pop("arguments", None)
            if argument_array:
                data[index]["arguments"] = shlex.split(command) + shlex.split(suffix)
                data[index].pop("command", None)
            else:
                data[index]["command"] = command + " " + suffix
            db_copy.write_text(json.dumps(data), encoding="utf-8")
            return check.check_compile_commands(db_copy, True)

        rejected("last canary flag wins", "stack protector", lambda: changed_command("-fno-stack-protector"))
        rejected("weak canary flag", "stack protector", lambda: changed_command("-fstack-protector"))
        rejected("FORTIFY disabled", "FORTIFY", lambda: changed_command("-D_FORTIFY_SOURCE=0"))
        rejected("FORTIFY undefined", "FORTIFY", lambda: changed_command("-U_FORTIFY_SOURCE"))
        rejected("split FORTIFY undef", "FORTIFY", lambda: changed_command("-U _FORTIFY_SOURCE"))
        rejected("split FORTIFY disable", "FORTIFY", lambda: changed_command("-D _FORTIFY_SOURCE=0"))
        rejected("quoted FORTIFY disable", "FORTIFY", lambda: changed_command('"-D_FORTIFY_SOURCE=0"'))
        rejected("quoted canary disable", "stack protector", lambda: changed_command('"-fno-stack-protector"'))
        rejected("argument-array FORTIFY disable", "FORTIFY", lambda: changed_command("-D _FORTIFY_SOURCE=0", True))
        rejected("argument-array canary disable", "stack protector", lambda: changed_command("-fno-stack-protector", True))
        rejected("unexpanded response", "response file", lambda: changed_command("@unexpanded-options.rsp"))
        rejected("FORTIFY unoptimized", "optimized code", lambda: changed_command("-O0"))
        # The effective final flag matters; an earlier disable must not produce
        # a false failure when the production policy overrides it.
        changed_command("-fno-stack-protector -fstack-protector-strong -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -O2")
        tests += 1
        changed_command('-U _FORTIFY_SOURCE -D "_FORTIFY_SOURCE=2" "-fstack-protector-strong" -O', True)
        tests += 1
    print(f"{args.target}: {tests} real-artifact/compile-command hardening checks passed")


if __name__ == "__main__":
    main()
