#!/usr/bin/env python3
"""Verify actual DH publication/open bodies with deterministic native services.

No Linux/KMI/device or true concurrency claim. Output manifests are immutable.
Old-source controls must compile and fail at the named semantic CHECK only.
"""
import argparse
import ast
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

sys.dont_write_bytecode = True
from test_sumh_dh_cache_source import ROOT, INPUTS, base
from test_sumh_dh_cache_cases import HELPERS, CASES, SCENARIOS, IDENTITY_SCENARIOS, OLD_FAILURES, main as case_main
from test_sumh_inject_claims import execute, fingerprints, target_assertion
from test_sumh_dh_cache_mutations import mutations


def dependencies():
    pending = list((ROOT / "scripts/ci").glob("test_sumh_dh_cache*.py"))
    seen = set()
    while pending:
        path = pending.pop().resolve()
        if path in seen:
            continue
        seen.add(path)
        for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"))):
            names = [node.module] if isinstance(node, ast.ImportFrom) and node.module else (
                [x.name for x in node.names] if isinstance(node, ast.Import) else [])
            for name in names:
                local = path.parent / (name.split(".")[0] + ".py")
                if local.exists():
                    pending.append(local)
    return sorted(str(p.relative_to(ROOT)).replace("\\", "/") for p in seen)


def source(root=ROOT, version="6,18", scenario=None):
    return base(root, version) + HELPERS + CASES + case_main(scenario)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    parser.add_argument("--out-dir", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--baseline-root", type=Path)
    parser.add_argument("--scenario", choices=SCENARIOS + IDENTITY_SCENARIOS, help="Development subset; not a full pass")
    parser.add_argument("--version", choices=("6,12", "6,18"), help="Development API subset")
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required")
    if args.manifest and args.manifest.exists():
        parser.error("manifest already exists; choose a fresh evidence path")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    inputs, tests = sorted(INPUTS), dependencies()
    report = dict(argv=sys.argv, cwd=str(Path.cwd()), source_before=fingerprints(ROOT, inputs),
                  tests_before=fingerprints(ROOT, tests), runs=[], scenarios=([args.scenario] if args.scenario else SCENARIOS + IDENTITY_SCENARIOS),
                  full_suite=not(args.scenario or args.version),
                  limitations=["actual product bodies with deterministic native services", "no kernel/KMI build",
                               "no real concurrency, device or filesystem execution", "FUSE READDIRPLUS, pure virtual subtree and transient fast-reader windows remain open"])
    tmp = tempfile.TemporaryDirectory(prefix="sumh-dh-cache-") if not args.out_dir else None
    output = (args.out_dir or Path(tmp.name)).resolve()
    try:
        for version in ([args.version] if args.version else ["6,12", "6,18"]):
            execute(compiler, f"cache-{version.replace(',', '-')}", source(version=version, scenario=args.scenario), output, report["runs"])
        if not args.scenario:
            for name, text, expected in mutations(source):
                _, target = target_assertion(text, expected)
                if target["matching_invocations_before_selection"] != 1:
                    raise AssertionError(f"ambiguous fault {name}")
                execute(compiler, "mutant-" + name, text, output, report["runs"], expected)
        if args.baseline_root:
            report["baseline_root"] = str(args.baseline_root.resolve())
            report["baseline_before"] = fingerprints(args.baseline_root, inputs)
            for scenario, expected in OLD_FAILURES.items():
                if args.scenario and scenario != args.scenario:
                    continue
                text = source(args.baseline_root, scenario=scenario)
                _, target = target_assertion(text, expected)
                if target["matching_invocations_before_selection"] != 1:
                    raise AssertionError(f"ambiguous old control {scenario}")
                execute(compiler, "retained-old-" + scenario, text, output, report["runs"], expected)
        report["passed"] = True
        print("PASS DH cache publication/open suite" + (" (development subset)" if not report["full_suite"] else ""))
    finally:
        report["source_after"] = fingerprints(ROOT, inputs)
        report["tests_after"] = fingerprints(ROOT, tests)
        if report["source_before"] != report["source_after"] or report["tests_before"] != report["tests_after"]:
            report["passed"] = False
            report["inputs_changed"] = True
        if args.baseline_root and "baseline_before" in report:
            report["baseline_after"] = fingerprints(args.baseline_root, inputs)
            if report["baseline_before"] != report["baseline_after"]:
                report["passed"] = False
                report["baseline_changed"] = True
        if args.manifest:
            args.manifest.parent.mkdir(parents=True, exist_ok=True)
            args.manifest.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        if tmp:
            tmp.cleanup()
    if not report.get("passed"):
        raise AssertionError("verification input changed")


if __name__ == "__main__":
    main()
