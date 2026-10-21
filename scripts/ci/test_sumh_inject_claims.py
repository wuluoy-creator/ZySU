#!/usr/bin/env python3
"""Compile actual claim/store/producer/FOP bodies and behavioral counterexamples.

Kernel path, locking, allocation, RCU and worker services are deterministic
doubles. This is not a kernel build, native concurrency, unmount or device test.
Mutations change generated test input only. Compilation failure is never a
successful counterexample. --baseline-root explicitly tests retained old code.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from test_sumh_vfs_helpers import ROOT
from test_sumh_inject_claims_source import base, INPUTS
from test_sumh_inject_claims_model import fixtures
from test_sumh_source_identity import CASES as IDENTITY_CASES
from test_sumh_inject_claims_cases import CASES, SCENARIOS
import test_sumh_inject_claims_producers as producers
import test_sumh_inject_claims_lifetime as lifetime
from test_sumh_inject_claims_mutations import mutations


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def fingerprints(root, paths):
    return {path: digest(Path(root) / path) for path in paths}


def target_assertion(source, expected):
    """Give exactly one CHECK invocation sentinel 253; all others return 1.

    Several legacy fixtures put multiple assertions on a source line. Exit
    codes based on __LINE__ cannot prove which one failed. Selecting an exact
    invocation and reserving its code removes that ambiguity on POSIX too.
    """
    selected = re.search(r"int main\(void\)\{return (\w+)\(\);\}", source)
    if selected:
        function_name = selected.group(1)
        region_start = source.index(f"static int {function_name}(")
        from test_sumh_inject_claims_source import body
        region_end = region_start + len(body(source, f"static int {function_name}(").rstrip())
    else:
        function_name = "main"
        region_start, region_end = source.rindex("int main(void)"), len(source)
    candidates = []
    for match in re.finditer(r"\bCHECK\(", source):
        if not region_start <= match.start() < region_end:
            continue
        line_start = source.rfind("\n", 0, match.start()) + 1
        if source[line_start:match.start()].lstrip().startswith("#define"):
            continue
        start, end, depth = match.end(), match.end(), 1
        while depth:
            depth += (source[end] == "(") - (source[end] == ")")
            end += 1
        expression = source[start:end - 1]
        if expected in expression:
            candidates.append((match.start(), end, expression))
    if not candidates:
        raise AssertionError(f"no CHECK invocation matches target {expected!r}")
    start, end, expression = candidates[0]
    source = source[:start] + "CHECK_TARGET(" + expression + ")" + source[end:]
    ordinary = "#define CHECK(test) do { if (!(test)) return ((__LINE__ - 1) % 255) + 1; } while (0)"
    if source.count(ordinary) != 1:
        raise AssertionError("CHECK macro changed")
    source = source.replace(ordinary, "#define CHECK(test) do { if (!(test)) return 1; } while (0)\n#define CHECK_TARGET(test) do { if (!(test)) return 253; } while (0)")
    return source, {"expression": expression, "failure_exit_code": 253, "other_assertion_exit_code": 1,
                    "selection": "first matching individual CHECK invocation in the selected scenario function", "function": function_name,
                    "matching_invocations_before_selection": len(candidates)}


def execute(compiler, name, source, output, records, expected=None):
    target = None
    if expected is not None:
        source, target = target_assertion(source, expected)
    directory = output / name
    directory.mkdir(parents=True, exist_ok=True)
    path, obj = directory / "test.c", directory / "test.obj"
    exe = directory / ("test.exe" if os.name == "nt" else "test")
    path.write_text(source, encoding="utf-8")
    flags = ["-std=gnu11", "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-Werror", "-Wno-unused-function", "-Wno-unused-variable"]
    commands = []
    if os.name == "nt":
        commands.append([compiler, "--target=x86_64-pc-windows-msvc", *flags, "-c", str(path), "-o", str(obj)])
        commands.append([str(Path(compiler).with_name("ld.lld.exe")), "-flavor", "link", "/entry:main", "/subsystem:console", "/nodefaultlib", f"/out:{exe}", str(obj)])
    else:
        commands.append([compiler, *flags, str(path), "-o", str(exe)])
    record = {"name": name, "generated_source": str(path), "source_sha256": digest(path), "expected_failure": expected, "target_assertion": target, "commands": []}
    records.append(record)
    for command in commands:
        result = subprocess.run(command, capture_output=True, text=True)
        record["commands"].append({"argv": command, "exit_code": result.returncode, "stdout": result.stdout, "stderr": result.stderr})
        if result.returncode:
            print(result.stdout + result.stderr, end="")
            raise AssertionError(f"{name}: compilation/link failed; not a rejected behavioral mutation")
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    record["commands"].append({"argv": [str(exe)], "exit_code": result.returncode, "stdout": result.stdout, "stderr": result.stderr})
    record["artifacts"] = {"executable": {"path": str(exe), "sha256": digest(exe)}}
    if os.name == "nt":
        record["artifacts"]["object"] = {"path": str(obj), "sha256": digest(obj)}
    candidates = [{"line": n, "text": line.strip()} for n, line in enumerate(source.splitlines(), 1)
                  if "CHECK(" in line and (n - 1) % 255 + 1 == result.returncode]
    record["matching_checks"] = candidates
    if expected is None:
        if result.returncode:
            raise AssertionError(f"{name}: exit {result.returncode}; {candidates}")
        print(f"PASS {name}")
    else:
        if result.returncode != target["failure_exit_code"]:
            raise AssertionError(f"{name}: expected targeted invariant {target['expression']!r} (sentinel 253), got exit {result.returncode}")
        record["matching_checks"] = [{"text": target["expression"], "targeted_sentinel": 253}]
        print(f"PASS rejected {name}: target sentinel 253; {target['expression']}")
    record["passed"] = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    parser.add_argument("--baseline-root", type=Path)
    parser.add_argument("--out-dir", type=Path)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()
    if not args.cc:
        parser.error("a C compiler is required; pass --cc")
    compiler = shutil.which(args.cc) or str(Path(args.cc).resolve())
    inputs = list(INPUTS) + ["kernel/sumh/features/sumh_fop_override.c", "kernel/sumh/features/sumh_fop_override.h", "kernel/sumh/core/sumh_bootstrap.c"]
    tests = sorted(str(p.relative_to(ROOT)).replace("\\", "/") for p in (ROOT / "scripts/ci").glob("test_sumh_inject_claims*.py"))
    tests += ["scripts/ci/test_sumh_source_identity.py", "scripts/ci/test_sumh_merge_install.py", "scripts/ci/test_sumh_merge_install_cases.py", "scripts/ci/test_sumh_merge_install_model.py", "scripts/ci/test_sumh_core.py", "scripts/ci/test_sumh_vfs_helpers.py", "scripts/ci/test_sumh_rules.py", "scripts/ci/test_sumh_ci.py", "scripts/ci/sumh_ci.py"]
    tests += [f".github/workflows/{name}.yml" for name in ("sumh-tests", "build", "release", "ksud", "ksuinit")]
    report = {"argv": sys.argv, "cwd": str(Path.cwd()), "source_before": fingerprints(ROOT, inputs), "tests_before": fingerprints(ROOT, tests), "runs": [], "scenarios": list(SCENARIOS) + list(producers.SCENARIOS) + list(lifetime.SCENARIOS), "reuse_cycles": 1000}
    temporary = tempfile.TemporaryDirectory(prefix="sumh-inject-claims-") if args.out_dir is None else None
    output = (args.out_dir or Path(temporary.name)).resolve()
    try:
        views = {"claims": base(buckets=8) + fixtures(IDENTITY_CASES) + CASES,
                 "producers": producers.source(ROOT), "lifetime": lifetime.source(ROOT)}
        for name, source in views.items():
            execute(compiler, name, source, output, report["runs"])
        for name, changed, expected in mutations(views):
            execute(compiler, "mutant-" + name, changed, output, report["runs"], expected)
        if args.baseline_root:
            import test_sumh_inject_claims_baseline as baseline
            report["baseline_root"] = str(args.baseline_root.resolve())
            report["baseline_hashes"] = fingerprints(args.baseline_root, INPUTS)
            execute(compiler, "retained-baseline", baseline.source(args.baseline_root), output, report["runs"], "!test_bit(AS_FLAGS_SUMH_DIR_HAS_INJECT,&parent_i.i_mapping->flags)")
        report["source_after"] = fingerprints(ROOT, inputs)
        report["tests_after"] = fingerprints(ROOT, tests)
        if report["source_before"] != report["source_after"] or report["tests_before"] != report["tests_after"]:
            raise AssertionError("source or test inputs changed during verification")
        report["passed"] = True
        print(f"PASS injection ownership: {len(report['scenarios'])} named scenarios, 1000 reuse cycles, {sum(r['name'].startswith('mutant-') for r in report['runs'])} compiled mutations rejected")
    finally:
        if args.manifest:
            args.manifest.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        if temporary:
            temporary.cleanup()


if __name__ == "__main__":
    main()
