#!/usr/bin/env python3
"""Run actual DH all-producer ownership, lifetime and fault regressions.

Native VFS, scheduling and allocation are deterministic host services. This is
not a native kernel/KMI, device, concurrency, cache-coherence or performance test.
"""
import argparse
import ast
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import ROOT
from test_sumh_inject_claims import execute, fingerprints, target_assertion
from test_sumh_dh_ownership_source import core, FIXTURES, INPUTS, function, replace_once
from test_sumh_dh_ownership_cases import HELPERS, CASES
import test_sumh_dh_ownership_managed as managed
import test_sumh_dh_ownership_su as su
import test_sumh_dh_ownership_lifetime as lifetime
import test_sumh_dh_ownership_producers as producers
import test_sumh_dh_ownership_materializer as materializer
import test_sumh_dh_ownership_bootstrap as bootstrap
import test_sumh_dh_ownership_extended as extended
from test_sumh_dh_ownership_mutations import mutations


def test_inputs():
    """Fingerprint every local Python dependency, including unchanged services."""
    pending = list((ROOT / "scripts/ci").glob("test_sumh_dh_ownership*.py"))
    pending += [ROOT / "scripts/ci" / f"{name}.py" for name in (
        "test_sumh_inject_claims", "test_sumh_source_identity", "test_sumh_merge_install",
        "test_sumh_core", "test_sumh_rules", "test_sumh_merge_snapshot", "sumh_ci", "test_sumh_ci")]
    seen = set()
    while pending:
        path = pending.pop().resolve()
        if path in seen:
            continue
        seen.add(path)
        for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"))):
            if isinstance(node, ast.ImportFrom) and node.module:
                names = [node.module]
            elif isinstance(node, ast.Import):
                names = [entry.name for entry in node.names]
            else:
                continue
            for name in names:
                dependency = path.parent / (name.split(".")[0] + ".py")
                if dependency.exists():
                    pending.append(dependency)
    return sorted(str(p.relative_to(ROOT)).replace("\\", "/") for p in seen) + [
        f".github/workflows/{name}.yml" for name in ("sumh-tests", "build", "release", "ksud", "ksuinit")]


def views():
    return {"owners": core() + FIXTURES + HELPERS + CASES,
            **{module.__name__.split("_")[-1]: module.source() for module in
               (managed, su, lifetime, producers, materializer, bootstrap, extended)}}


def extractor_guards():
    checks = {
        "missing-definition": lambda: function("int absent(void);", "int absent("),
        "duplicate-definition": lambda: function("int f(void){return 0;}\nint f(void){return 1;}", "int f("),
        "missing-instrumentation": lambda: replace_once("other source", "instrumentation", "changed"),
        "duplicate-instrumentation": lambda: replace_once("marker marker", "marker", "changed"),
    }
    for name, check in checks.items():
        try:
            check()
        except AssertionError:
            continue
        raise AssertionError(f"extractor did not fail closed: {name}")
    assert function("int f(void);\nint f(void){return 1;}", "int f(") == "int f(void){return 1;}"
    return [*checks, "prototype-excluded"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    parser.add_argument("--out-dir", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--baseline-root", type=Path)
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required")
    if args.manifest and args.manifest.exists():
        parser.error("manifest already exists; use a fresh evidence destination")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    inputs = sorted(set(INPUTS + managed.INPUTS + su.INPUTS + lifetime.INPUTS +
                        producers.INPUTS + materializer.INPUTS + bootstrap.INPUTS))
    tests = test_inputs()
    report = dict(argv=sys.argv, cwd=str(Path.cwd()), source_before=fingerprints(ROOT, inputs),
                  tests_before=fingerprints(ROOT, tests), runs=[], reuse_cycles=1000,
                  limitations=["deterministic native-service model", "no native kernel or device execution",
                               "D1/D2/D5 cache freshness deferred"])
    temporary = tempfile.TemporaryDirectory(prefix="sumh-dh-ownership-") if args.out_dir is None else None
    output = (args.out_dir or Path(temporary.name)).resolve()
    try:
        report["extractor_guards"] = extractor_guards()
        sources = views()
        report["scenario_functions"] = {name: re.findall(r"static int (\w+)\(void\)", text[text.index(FIXTURES):])
                                        for name, text in sources.items()}
        for name, text in sources.items():
            execute(compiler, name, text, output, report["runs"])
        for name, text, expected in mutations(sources):
            _, assertion = target_assertion(text, expected)
            if assertion["matching_invocations_before_selection"] != 1:
                raise AssertionError(f"ambiguous mutation assertion: {name}: {assertion}")
            execute(compiler, "mutant-" + name, text, output, report["runs"], expected)
        if args.baseline_root:
            import test_sumh_dh_ownership_baseline as baseline
            report["baseline_root"] = str(args.baseline_root.resolve())
            report["baseline_hashes"] = fingerprints(args.baseline_root, inputs)
            for name, text, expected in baseline.views(args.baseline_root):
                _, assertion = target_assertion(text, expected)
                if assertion["matching_invocations_before_selection"] != 1:
                    raise AssertionError(f"ambiguous baseline assertion: {name}")
                execute(compiler, "retained-old-" + name, text, output, report["runs"], expected)
        report["passed"] = True
        print(f"PASS DH ownership: {len(sources)} actual-body views, 1000 modeled reuse cycles, "
              f"{sum(r['name'].startswith('mutant-') for r in report['runs'])} targeted compiled faults")
    finally:
        report["source_after"] = fingerprints(ROOT, inputs)
        report["tests_after"] = fingerprints(ROOT, tests)
        if args.baseline_root:
            report["baseline_after"] = fingerprints(args.baseline_root, inputs)
            if report.get("baseline_hashes") != report["baseline_after"]:
                report["passed"] = False
                report["baseline_change"] = True
        if report["source_before"] != report["source_after"] or report["tests_before"] != report["tests_after"]:
            report["passed"] = False
            report["input_change"] = True
        if args.manifest:
            args.manifest.parent.mkdir(parents=True, exist_ok=True)
            args.manifest.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        if temporary:
            temporary.cleanup()
    if not report.get("passed"):
        raise AssertionError("verification inputs changed")


if __name__ == "__main__":
    main()
