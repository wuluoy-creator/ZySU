#!/usr/bin/env python3
"""Execute immutable Maps storage, ioctl and filtering with host API doubles.

Deterministic pin/publication schedules are not Linux locking or device proof.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import sys
sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, ROOT, function, run_test
from test_sumh_proc_memory import memory_source
from test_sumh_mount_views import extract

IOCTL = "kernel/sumh/control/sumh_ioctl.c"
PROC = "kernel/sumh/hooks/sumh_proc_read_hooks.c"
STORE = "kernel/sumh/core/sumh_maps_store.c"


def stripped(path):
    return re.sub(r"^#include[^\n]*\n", "", (ROOT / path).read_text(encoding="utf-8"), flags=re.M)


def fixture(name):
    return (ROOT / "scripts/ci" / name).read_text(encoding="utf-8")


def common_source():
    uapi = (ROOT / "uapi/sumh.h").read_text(encoding="utf-8")
    defines = "\n".join(line for line in uapi.splitlines() if line.startswith(
        ("#define SUMH_MAX_LEN_PATHNAME ", "#define SUMH_MANAGED_MAPS_MAX_RULES ")))
    structs = "\n".join(function("uapi/sumh.h", marker) + ";" for marker in (
        "struct sumh_maps_rule {", "struct sumh_managed_maps_arg {"))
    return COMMON + memory_source() + fixture("sumh_maps_test_model.c") + defines + "\n" + structs + "\n" + stripped(
        "kernel/sumh/core/sumh_maps_store.h") + stripped(STORE)


def mutation_source():
    wrappers = "\n".join(function(IOCTL, marker) for marker in (
        "static long sumh_maps_add_rule(", "static long sumh_maps_replace_managed("))
    clear = function(IOCTL, "if (cmd == SUMH_IOC_CLEAR_MAPS_RULES)")
    wrappers += "\nstatic long clear_rules(unsigned int cmd) {\n" + clear + "\nreturn -EINVAL;\n}\n"
    return common_source() + wrappers + fixture("sumh_maps_mutation_cases.c")


def filter_source():
    methods = "\n".join(extract(PROC, name) for name in (
        "sumh_parse_maps_line", "sumh_maps_line_looks_like_header", "sumh_filter_maps_lines"))
    return common_source() + fixture("sumh_maps_filter_model.c") + methods + fixture("sumh_maps_filter_cases.c")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    for name, factory in (("maps-store-ioctl-lifetime-budget", mutation_source), ("maps-production-filter-snapshots", filter_source)):
        source = factory()
        run_test(compiler, name, source)
        if os.name == "nt":
            lp64 = re.sub(r"\bunsigned long\b(?! long)", "unsigned long long", source)
            run_test(compiler, name + "-lp64", "#define SUMH_TEST_LP64 1\n" + lp64)
