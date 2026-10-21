#!/usr/bin/env python3
"""Exercise exact temporary Maps deletion using production C bodies.

Allocation, usercopy and deterministic lock schedules are host service doubles.
ARM32/64 objects compile ABI/extracted models, not a kernel build or device proof.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import COMMON, function, run_test
from test_sumh_maps import IOCTL, STORE, fixture, stripped
from test_sumh_proc_memory import memory_source
from test_sumh_runtime_state import TYPES, uapi


def abi_source():
    return TYPES + uapi() + r"""
_Static_assert(sizeof(struct sumh_maps_delete_arg)==40,"delete width");
_Static_assert(__alignof__(struct sumh_maps_delete_arg)==8,"delete alignment");
_Static_assert(__builtin_offsetof(struct sumh_maps_delete_arg,size)==0,"size offset");
_Static_assert(__builtin_offsetof(struct sumh_maps_delete_arg,version)==4,"version offset");
_Static_assert(__builtin_offsetof(struct sumh_maps_delete_arg,expected_instance_id)==8,"instance offset");
_Static_assert(__builtin_offsetof(struct sumh_maps_delete_arg,expected_generation)==16,"generation offset");
_Static_assert(__builtin_offsetof(struct sumh_maps_delete_arg,target_ino)==24,"inode offset");
_Static_assert(__builtin_offsetof(struct sumh_maps_delete_arg,target_dev)==32,"device offset");
_Static_assert(SUMH_IOC_DELETE_TEMPORARY_MAPS==0x40285335U,"delete command encoding");
_Static_assert(SUMH_FEATURE_TEMPORARY_MAPS_DELETE==(1U<<21),"delete capability");
_Static_assert(SUMH_MAPS_DELETE_VERSION==1,"delete version");
_Static_assert(SUMH_PROTOCOL_VERSION==1,"legacy protocol retained");
"""


def deletion_source():
    source = (COMMON + abi_source() + fixture("sumh_maps_delete_model.c") + memory_source() +
              fixture("sumh_maps_test_model.c") + stripped("kernel/sumh/core/sumh_maps_store.h") +
              stripped(STORE))
    source += "\n".join(function(IOCTL, marker) for marker in (
        "static void sumh_runtime_advance(", "static void sumh_runtime_record_result(",
        "static long sumh_maps_add_rule(", "static long sumh_maps_replace_managed(",
        "static long sumh_maps_delete_temporary(", "long sumh_handle_ioctl("))
    return source + fixture("sumh_maps_delete_cases.c")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    source = deletion_source()
    run_test(compiler, "maps-temporary-delete-control", source)
    if os.name == "nt":
        lp64 = re.sub(r"\bunsigned long\b(?! long)", "unsigned long long", source)
        run_test(compiler, "maps-temporary-delete-control-lp64", "#define SUMH_TEST_LP64 1\n" + lp64)
    # Compile both ABIs without libc or kernel headers/linking. Only clang can
    # cross-compile this optional layer; the executable host regressions above
    # run for other compilers as well.
    version = subprocess.check_output([compiler, "--version"], text=True)
    if "clang" in version.lower():
        with tempfile.TemporaryDirectory(prefix="sumh-maps-delete-abi-") as directory:
            directory = Path(directory)
            for label, target in (("arm32", "armv7a-linux-androideabi"), ("arm64", "aarch64-linux-android")):
                for kind, contents in (("abi", abi_source()), ("model", source)):
                    input_file = directory / f"{label}-{kind}.c"
                    input_file.write_text(contents, encoding="utf-8")
                    command = [compiler, f"--target={target}", "-std=gnu11", "-ffreestanding",
                               "-fno-builtin", "-c", str(input_file), "-o", str(input_file.with_suffix(".o"))]
                    subprocess.run(command, check=True)
                    print(f"PASS: maps-temporary-delete-{label}-{kind}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
