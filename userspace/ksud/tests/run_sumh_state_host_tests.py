"""Exercise production SUMH ledger, snapshot, capture and operation queries.

No live kernel/daemon or real storage durability is claimed. SHA-256's standard
library implementation is substituted; the production argument/encoding glue
is exercised. Actual atomic replacement faults have a separate R03 runner.
The query's production polling loop runs with injected clock/sleep and real
production parsing/ledger reads against the filesystem model, without waiting.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    sumhp = root / "userspace/ksud/sumhp"
    with tempfile.TemporaryDirectory(prefix="sumh-state-tests-") as temp:
        build = Path(temp)
        source = (sumhp / "src/core/state_ledger.cpp").read_text(encoding="utf-8")
        (build / "state_ledger_under_test.inc").write_text(
            source[source.index("namespace sumhp::state {"):].replace("std::chrono::steady_clock", "fixture_clock::Clock"),
            encoding="utf-8")
        query = (sumhp / "src/core/operation_query.cpp").read_text(encoding="utf-8")
        query = query[query.index("namespace sumhp::state {"):]
        query = query.replace("std::chrono::steady_clock", "fixture_clock::Clock")
        query = query.replace("std::this_thread::sleep_for", "fixture_clock::sleep_for")
        (build / "operation_query_under_test.inc").write_text(query, encoding="utf-8")
        maps_source = (sumhp / "src/mount/maps_snapshot.cpp").read_text(encoding="utf-8")
        (build / "maps_comparison_under_test.inc").write_text(
            maps_source[maps_source.index("MapsComparison compare_maps("):], encoding="utf-8")
        command = (sumhp / "src/core/command.cpp").read_text(encoding="utf-8")
        begin = command.index("CommandResult run_command_capture(")
        end = command.index("\n}  // namespace sumhp", begin)
        (build / "state_capture_under_test.inc").write_text(command[begin:end], encoding="utf-8")
        client = (sumhp / "src/mount/sumh_client.cpp").read_text(encoding="utf-8")
        def function(signature):
            start = client.index(signature)
            end = client.index("\n}", start) + 2
            return client[start:end]
        # Keep the production feature/deletion logic. Only the feature helper's
        # symbol is renamed to coexist with the unrelated snapshot fixture.
        feature = function("FeatureCapabilities feature_capabilities(").replace(
            "feature_capabilities(", "tracked_maps_feature_capabilities(")
        deletion = function("bool delete_temporary_maps_rule(").replace(
            "feature_capabilities()", "tracked_maps_feature_capabilities()")
        (build / "tracked_maps_client_under_test.inc").write_text(
            function("int execute(unsigned long cmd,") + "\n" + feature + "\n" + deletion,
            encoding="utf-8")
        if os.name == "nt":
            (build / "linux").mkdir(); (build / "sys").mkdir()
            (build / "linux/types.h").write_text("#pragma once\n#include <stdint.h>\ntypedef uint32_t __u32;\ntypedef int32_t __s32;\n#define __aligned_u64 uint64_t __attribute__((aligned(8)))\n")
            (build / "sys/ioctl.h").write_text("#pragma once\n#define _IOC(d,t,n,s) (((unsigned long)(d)<<30)|((unsigned long)(s)<<16)|((unsigned long)(t)<<8)|(n))\n#define _IO(t,n) _IOC(0,t,n,0)\n#define _IOR(t,n,s) _IOC(2,t,n,sizeof(s))\n#define _IOW(t,n,s) _IOC(1,t,n,sizeof(s))\n#define _IOWR(t,n,s) _IOC(3,t,n,sizeof(s))\n")
        compiler = [args.zig, "c++"] if args.zig else [args.cxx]
        if os.name == "nt":
            compiler += ["-DESTALE=116", "-DESHUTDOWN=108"]
        binary = build / ("sumh_state_ledger_test.exe" if os.name == "nt" else "sumh_state_ledger_test")
        subprocess.run([*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-I" + str(root), "-I" + str(sumhp / "include"), "-I" + str(sumhp / "src"), "-I" + str(build),
            str(root / "userspace/ksud/tests/sumh_state_ledger_test.cpp"), str(sumhp / "src/core/state_ledger_model.cpp"),
            str(sumhp / "src/core/operation_query_input.cpp"),
            str(sumhp / "src/core/operation_resource_model.cpp"),
            str(sumhp / "src/core/user_hide_model.cpp"),
            "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
