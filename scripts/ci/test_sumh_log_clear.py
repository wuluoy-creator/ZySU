#!/usr/bin/env python3
"""Run actual Manager clear_log/Fd/failure bodies against modeled syscalls.

No real log, filesystem or process lock is touched. Cross compilation is not a
Linux/Android runtime result. Every evidence destination must be fresh.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PRODUCT = "manager/app/src/main/cpp/sumhp_jni.cpp"
FIXTURE = "scripts/ci/sumh_log_clear_model.cpp"


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def block(text, marker):
    if text.count(marker) != 1:
        raise ValueError(f"expected exactly one definition: {marker}")
    start = text.index(marker)
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def extract(text):
    body = block(text, "class Fd {") + ";\n\n" + block(text, "[[noreturn]] void failure(") + "\n\n" + block(text, "void clear_log(")
    body = re.sub(r"\bstruct\s+stat\b", "struct ::logclear::ModelStat", body)
    for original, replacement in {
        "O_WRONLY":"write_only", "O_RDWR":"read_write", "O_CREAT":"create",
        "O_TRUNC":"trunc", "O_NONBLOCK":"nonblock", "O_CLOEXEC":"cloexec",
        "O_NOFOLLOW":"nofollow", "LOCK_EX":"lock_ex", "LOCK_NB":"lock_nb", "S_ISREG":"is_regular",
    }.items():
        body = re.sub(r"\b" + original + r"\b", "::logclear::" + replacement, body)
    for call in ("open", "close", "fstat", "flock", "fchmod", "ftruncate", "strerror"):
        replacement = "error_message" if call == "strerror" else call
        body = re.sub(r"(?<![\w:])" + call + r"\s*\(", "::logclear::" + replacement + "(", body)
    return body


def wiring_guard(text):
    """Fail closed if the exercised helper is disconnected from its JNI entry."""
    entry=block(text,"Java_com_zying_zysu_Natives_sumhClearLog(")
    entry=re.sub(r"//[^\n]*|/\*.*?\*/","",entry,flags=re.S)
    compact=re.sub(r"\s+","",entry)
    expected="result(env,[]{clear_log(log_path);returnstd::string{};})"
    if expected not in compact or compact.count("clear_log(")!=1:
        raise ValueError("sumhClearLog must call clear_log(log_path) through result")
    return {"entry":"Java_com_zying_zysu_Natives_sumhClearLog", "helper":"clear_log(log_path)", "wrapper":"result"}


def run(command, report, timeout=120):
    result = subprocess.run([str(arg) for arg in command], capture_output=True, text=True, timeout=timeout)
    record = {"argv":[str(arg) for arg in command], "exit_code":result.returncode,
              "stdout":result.stdout, "stderr":result.stderr}
    report.append(record)
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    return result


def build(compiler, root, output, name, records, changed=None):
    directory=output/name
    directory.mkdir()
    source=(root/PRODUCT).read_text(encoding="utf-8")
    generated=extract(source)
    if changed:
        old,new=changed
        if generated.count(old)!=1:
            raise ValueError(f"fault seam changed: {old}")
        generated=generated.replace(old,new,1)
    include=directory/"sumh_log_clear_under_test.inc"
    include.write_text(generated+"\n",encoding="utf-8")
    executable=directory/("clear_log.exe" if os.name=="nt" else "clear_log")
    result=run([*compiler,"-std=c++17","-Wall","-Wextra","-Werror","-I"+str(directory),ROOT/FIXTURE,"-o",executable],records)
    if result.returncode:
        raise RuntimeError(f"{name}: compile failure is not a behavioral rejection")
    return executable, {"source":str(include),"source_sha256":sha(include),
                        "binary":str(executable),"binary_sha256":sha(executable)}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig")
    parser.add_argument("--cxx",default=os.environ.get("CXX","c++"))
    parser.add_argument("--target")
    parser.add_argument("--compile-only",action="store_true")
    parser.add_argument("--out-dir",type=Path)
    parser.add_argument("--baseline-root",type=Path)
    args=parser.parse_args()
    if args.target and not(args.zig and args.compile_only):
        parser.error("--target requires --zig and --compile-only")
    if args.out_dir and args.out_dir.exists():
        parser.error("evidence directory exists; choose a fresh destination")
    temporary=tempfile.TemporaryDirectory(prefix="sumh-log-clear-") if args.out_dir is None else None
    output=(args.out_dir or Path(temporary.name)/"evidence").resolve()
    output.mkdir(parents=True)
    compiler=[args.zig,"c++"] if args.zig else [args.cxx]
    if args.target:
        compiler += ["-target",args.target]
    paths=[PRODUCT,FIXTURE,"scripts/ci/test_sumh_log_clear.py"]
    report={"argv":sys.argv,"source_before":{p:sha(ROOT/p) for p in paths},"commands":[],"artifacts":{},
            "compile_only":args.compile_only,"target":args.target,
            "limitations":["actual extracted production bodies; modeled syscalls only","no device, real filesystem, actual flock contention or hostile-root race test"]}
    try:
        text=(ROOT/PRODUCT).read_text(encoding="utf-8")
        report["production_wiring"]=wiring_guard(text)
        disconnected=text.replace("clear_log(log_path);","(void)log_path;",1)
        try:
            wiring_guard(disconnected)
        except ValueError:
            report["wiring_disconnect_guard_rejected"]=True
        else:
            raise RuntimeError("JNI wiring guard accepted disconnected helper")
        binary,artifact=build(compiler,ROOT,output,"current",report["commands"])
        report["artifacts"]["current"]=artifact
        if not args.compile_only:
            result=run([binary],report["commands"])
            if result.returncode:
                raise RuntimeError("current production regression failed")
            count=re.search(r"PASS clear_log: (\d+) checks",result.stdout)
            if not count:
                raise RuntimeError("missing runtime check count")
            report["checks"]=int(count.group(1))
        # A targeted compiled fault proves validation guards the destructive call.
        fault,artifact=build(compiler,ROOT,output,"mutant-target-link-check",report["commands"],
            (" || info.st_nlink != 1", ""))
        report["artifacts"]["mutant-target-link-check"]=artifact
        if not args.compile_only:
            result=run([fault,"links","target_link_count_rejected"],report["commands"])
            if result.returncode!=253 or "CHECK FAILED [links]: target_link_count_rejected" not in result.stdout:
                raise RuntimeError("link-validation fault not rejected at exact semantic assertion")
        if args.baseline_root:
            report["baseline_before"]={PRODUCT:sha(args.baseline_root/PRODUCT)}
            report["baseline_wiring"]=wiring_guard((args.baseline_root/PRODUCT).read_text(encoding="utf-8"))
            old,artifact=build(compiler,args.baseline_root,output,"retained-old",report["commands"])
            report["artifacts"]["retained-old"]=artifact
            controls={"links":"target_link_count_rejected","negative_size":"negative_size_rejected",
                      "failures":"lock_open_errno","contention":"contention_nonblocking_before_target",
                      "types":"special_file_open_never_waits","success":"permissions_private"}
            report["retained_old_controls"]=controls
            if not args.compile_only:
                for scenario,assertion in controls.items():
                    result=run([old,scenario,assertion],report["commands"])
                    if result.returncode!=253 or f"CHECK FAILED [{scenario}]: {assertion}" not in result.stdout:
                        raise RuntimeError(f"old body did not fail at exact {assertion} assertion")
        report["passed"]=True
    finally:
        report["source_after"]={p:sha(ROOT/p) for p in paths}
        if report["source_before"]!=report["source_after"]:
            report["passed"]=False
            report["inputs_changed"]=True
        if args.baseline_root and "baseline_before" in report:
            report["baseline_after"]={PRODUCT:sha(args.baseline_root/PRODUCT)}
            if report["baseline_before"]!=report["baseline_after"]:
                report["passed"]=False
                report["baseline_changed"]=True
        (output/"manifest.json").write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
        if temporary:
            temporary.cleanup()
    if not report.get("passed"):
        raise RuntimeError("verification inputs changed")
    print("PASS clear_log compiled" if args.compile_only else "PASS clear_log regression and semantic controls")


if __name__=="__main__":
    main()
