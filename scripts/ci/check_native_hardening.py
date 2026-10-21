#!/usr/bin/env python3
"""Check real Android artifacts and compile commands; never execute the binary.

Static ksuinit has no dynamic loader or lazy PLT binding: NOW is not a runtime
property there. A GNU_RELRO segment records linker intent, not proof that early
bionic startup mprotects it. Device startup/permissions remain separate checks.
"""

import argparse
import json
import re
import shlex
import subprocess
import sys
from pathlib import Path


class CheckError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise CheckError(message)


def inspect(readelf, path):
    result = subprocess.run(
        [str(readelf), "--wide", "--file-header", "--program-headers",
         "--section-headers", "--dynamic", "--notes", "--symbols", str(path)],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    require(result.returncode == 0, f"readelf failed for {path}: {result.stderr}")
    return result.stdout


def check_elf(binary, symbols, linkage, optimized):
    require(re.search(r"Class:\s+ELF64", binary), "artifact is not ELF64")
    require(re.search(r"Machine:\s+AArch64", binary), "artifact is not ARM64")
    require(re.search(r"^\s+GNU_RELRO\s", binary, re.M), "missing GNU_RELRO segment")
    stack = re.search(r"^\s+GNU_STACK\s+(.+)$", binary, re.M)
    require(stack and not re.search(r"\bE\b|RWE", stack[1]), "missing or executable GNU_STACK")
    require(not re.search(r"^\s+LOAD\s+.*\bRWE\b", binary, re.M), "writable executable LOAD segment")
    if linkage == "dynamic":
        require(re.search(r"Type:\s+DYN\b", binary), "dynamic executable is not PIE")
        require(re.search(r"^\s+INTERP\s", binary, re.M), "dynamic executable has no interpreter")
        require(re.search(r"\(FLAGS_1\).*\bPIE\b", binary), "missing dynamic PIE flag")
        require(re.search(r"\(FLAGS\).*\bBIND_NOW\b|\(FLAGS_1\).*\bNOW\b", binary), "missing immediate binding (NOW)")
        require(re.search(r"\bUND\s+__stack_chk_fail(?:@|\s|$)", binary, re.M), "no stack canary failure import")
    else:
        require(not re.search(r"^\s+INTERP\s|\(NEEDED\)", binary, re.M), "static init unexpectedly needs a dynamic loader")
        require(re.search(r"\b(?!UND\b)\d+\s+__stack_chk_fail(?:\s|$)", symbols, re.M), "static init has no stack canary failure implementation")
    build_ids = re.findall(r"Build ID:\s+([0-9a-fA-F]+)", binary)
    symbol_ids = re.findall(r"Build ID:\s+([0-9a-fA-F]+)", symbols)
    require(len(build_ids) == 1 and len(build_ids[0]) == 40, "missing or non-SHA1 build ID")
    require(build_ids == symbol_ids, "executable and symbol artifact build IDs differ")
    require(re.search(r"\s\.symtab\s", symbols), "symbol artifact is already stripped")
    require(not re.search(r"\s\.symtab\s|\s\.debug_\w+\s", binary), "shipping executable contains debug symbols")
    if optimized:
        require(re.search(r"\s\.debug_line\s", symbols), "symbol artifact has no source line tables")
        require(re.search(r"\b__(?:memcpy|memmove|memset|strcpy|strcat|strlcpy|strlcat|strlen|strchr|strrchr|snprintf|vsnprintf|sprintf|vsprintf|read|write|pread|pwrite|fgets|fread|fwrite|open|openat|poll|ppoll|recvfrom|recv|sendto|fdelt)_chk(?:@|\s|$)", symbols, re.M), "optimized artifact has no FORTIFY runtime checks")
    return build_ids[0]


def check_compile_commands(path, optimized):
    entries = json.loads(Path(path).read_text(encoding="utf-8"))
    require(isinstance(entries, list) and entries, "empty compilation database")
    checked = 0
    for entry in entries:
        source = entry.get("file", "")
        if Path(source).suffix.lower() not in {".c", ".cc", ".cpp", ".cxx"}:
            continue
        arguments = entry.get("arguments")
        if arguments is None:
            command = entry.get("command")
            require(isinstance(command, str) and command, f"missing compilation command: {source}")
            # CMake's compilation database quotes/escapes individual arguments.
            # Parse those without running a shell; raw-string regex misses quoted
            # options and split -D/-U arguments accepted by the compiler.
            arguments = shlex.split(command)
        require(isinstance(arguments, list) and arguments and
                all(isinstance(arg, str) for arg in arguments), f"invalid compilation arguments: {source}")
        require(not any(arg.startswith("@") for arg in arguments),
                f"unexpanded response file cannot be audited: {source}")
        protectors = [arg for arg in arguments if re.fullmatch(
            r"-f(?:no-)?stack-protector(?:-strong|-all|-explicit)?", arg)]
        require(protectors and protectors[-1] in {"-fstack-protector-strong", "-fstack-protector-all"}, f"stack protector is disabled/missing: {source}")
        if optimized:
            fortify = []
            index = 0
            while index < len(arguments):
                arg = arguments[index]
                index += 1
                if not (arg.startswith("-D") or arg.startswith("-U")):
                    continue
                operation, value = arg[:2], arg[2:]
                if not value:
                    require(index < len(arguments), f"missing preprocessor argument: {source}")
                    value = arguments[index]
                    index += 1
                name, _, level = value.partition("=")
                if name == "_FORTIFY_SOURCE":
                    fortify.append(level if operation == "-D" else "")
            require(fortify and fortify[-1] in {"2", "3"}, f"FORTIFY level 2+ is disabled/missing: {source}")
            opts = [arg[2:] or "1" for arg in arguments if re.fullmatch(r"-O(?:[0-3szg]|fast)?", arg)]
            require(opts and opts[-1] != "0", f"FORTIFY requires optimized code: {source}")
        checked += 1
    require(checked > 0, "no C/C++ compilation commands checked")
    return checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--symbols", type=Path, required=True)
    parser.add_argument("--readelf", type=Path, required=True)
    parser.add_argument("--compile-commands", type=Path, required=True)
    parser.add_argument("--linkage", choices=("dynamic", "static"), required=True)
    parser.add_argument("--configuration", default="Release")
    args = parser.parse_args()
    optimized = args.configuration.lower() in {"release", "minsizerel", "relwithdebinfo"}
    count = check_compile_commands(args.compile_commands, optimized)
    identity = check_elf(inspect(args.readelf, args.binary), inspect(args.readelf, args.symbols), args.linkage, optimized)
    print(json.dumps({"artifact": str(args.binary), "build_id": identity,
                      "bytes": args.binary.stat().st_size, "symbols_bytes": args.symbols.stat().st_size,
                      "compile_commands_checked": count, "linkage": args.linkage,
                      "relro": "segment present" if args.linkage == "static" else "RELRO + NOW",
                      "static_runtime_relro_verified": False if args.linkage == "static" else None,
                      "device_startup_tested": False}, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (CheckError, OSError, ValueError) as error:
        print(f"native hardening check failed: {error}", file=sys.stderr)
        sys.exit(1)
