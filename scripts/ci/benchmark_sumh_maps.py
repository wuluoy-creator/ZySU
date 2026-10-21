#!/usr/bin/env python3
"""Compare the actual Maps index with the preserved pre-Batch25 selector.

Single-process host algorithm benchmark, including instrumented key/node
visits. This is not device P99, mutex contention or full proc throughput.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from test_sumh_maps import common_source, fixture, ROOT


def benchmark_source():
    source = common_source()
    marker = "\twhile (first < end) {"
    if source.count(marker) != 1:
        raise ValueError("index instrumentation requires one actual binary-search loop")
    source = source.replace(marker, marker + "\n\t\tindexed_probes++;")
    return "#include <stdio.h>\n#include <time.h>\nstatic unsigned long long indexed_probes;\n" + source + fixture(
        "sumh_maps_linear_baseline.c") + fixture("sumh_maps_benchmark_cases.c")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    compiler = [args.zig, "cc"] if args.zig else [args.cc]
    source = benchmark_source()
    # Windows native unsigned long is32-bit. Match the ARM64 index width for
    # this algorithm benchmark; this does not establish ARM64 ABI behavior.
    if os.name == "nt":
        import re
        source = re.sub(r"\bunsigned long\b(?! long)", "unsigned long long", source)
    with tempfile.TemporaryDirectory(prefix="sumh-maps-benchmark-") as temporary:
        directory = Path(temporary)
        src = directory / "benchmark.c"
        exe = directory / ("benchmark.exe" if os.name == "nt" else "benchmark")
        src.write_text(source, encoding="utf-8")
        subprocess.run([*compiler, "-std=gnu11", "-O2", "-fno-builtin", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-Wno-unused-variable", str(src), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True, check=True, timeout=120)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(result.stdout, encoding="utf-8")
    manifest = {"scope": __doc__, "compiler": compiler, "generated_source_sha256": hashlib.sha256(source.encode()).hexdigest(),
                "source_sha256": {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in (
                    "kernel/sumh/core/sumh_maps_store.c", "kernel/sumh/core/sumh_maps_store.h",
                    "scripts/ci/sumh_maps_linear_baseline.c", "scripts/ci/sumh_maps_benchmark_cases.c")}}
    args.output.with_suffix(".json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(result.stdout, end="")


if __name__ == "__main__":
    main()
