#!/usr/bin/env python3
"""Run SUMH native entrypoints and enforce the same-commit release gate."""

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path


# These programs compile and execute their native fixtures from main(). Merely
# importing them through unittest discovery does not run those regressions.
NATIVE_CHECKS = (
    ("manager/tests/run_profile_transfer_worker_tests.py", "--cxx", "g++"),
    ("scripts/ci/test_native_evidence.py",),
    ("scripts/ci/test_embedded_payloads.py",),
    ("scripts/ci/test_release_provenance.py",),
    ("scripts/ci/test_ci_dependencies.py",),
    ("scripts/ci/ci_dependencies.py", "check"),
    ("scripts/ci/test_native_dependencies.py",),
    ("scripts/ci/test_native_provenance.py",),
    ("scripts/ci/test_sumh_observer_evidence.py",),
    ("scripts/ci/test_sumh_log_clear.py", "--cxx", "g++"),
    *[(f"scripts/ci/{name}.py", "--cc", "clang") for name in (
        "test_sepolicy_avtab", "test_sumh_core", "test_sumh_host_harness", "test_sumh_vfs_helpers",
        "test_sumh_directory_names", "test_sumh_directory_stream", "test_sumh_merge_snapshot",
        "test_sumh_lookup_compat",
        "test_sumh_mount_views", "test_sumh_proc_memory", "test_sumh_native_namespace",
        "test_selinux_hide_status", "test_selinux_hide_context",
        "test_selinux_hide_setprocattr", "test_sumh_guards",
        "test_sumh_unicode_guard", "test_sumh_kernel_build", "test_sumh_rules",
        "test_sumh_maps", "test_sumh_maps_delete", "test_sumh_kstat", "test_sumh_feature_readiness",
        "test_sumh_runtime_state",
        "test_sumh_rule_inventory", "test_sumh_source_identity", "test_sumh_source_capture", "test_sumh_merge_install",
        "test_sumh_fop_lifetime", "test_sumh_inject_claims", "test_sumh_dh_ownership", "test_sumh_dh_cache",
    )],
    *[(f"userspace/ksud/tests/{name}.py", "--cxx", "g++") for name in (
        "run_sumh_host_tests", "run_boot_mount_notify_host_tests",
        "run_runtime_host_tests",
        "run_log_policy_host_tests", "run_log_storage_host_tests",
        "run_sumh_state_host_tests",
        "run_operation_submit_host_tests",
        "run_user_hide_host_tests", "run_user_hide_controller_host_tests",
        "run_query_worker_host_tests",
        "run_maps_history_host_tests",
        "run_module_rebuild_host_tests", "run_module_rebuild_namespace_host_tests",
        "run_maps_preview_host_tests",
        "run_maps_preview_transport_host_tests",
        "run_maps_preview_cli_host_tests",
        "run_diagnostics_model_host_tests", "run_diagnostics_collector_host_tests",
        "run_diagnostic_package_model_host_tests", "run_diagnostic_package_store_host_tests",
        "run_diagnostic_package_controller_host_tests",
        "run_capability_snapshot_host_tests",
        "run_sumh_backend_host_tests", "run_module_preview_host_tests",
        "run_module_normalize_host_tests", "run_partition_discovery_host_tests",
        "run_development_host_tests",
        "run_profile_model_host_tests", "run_profile_store_host_tests", "run_profile_controller_host_tests",
        "run_profile_manager_transport_host_tests",
        "run_sumhp_daemon_host_tests", "run_mount_alias_host_tests",
        "run_sumh_planner_host_tests", "run_magic_mount_host_tests",
        "run_overlay_mount_host_tests",
    )],
)


def run_native_checks(root, commit_sha, checks=NATIVE_CHECKS):
    results = []
    for check in checks:
        command = [sys.executable, *check]
        print(f"::group::{check[0]}", flush=True)
        started = time.monotonic()
        try:
            completed = subprocess.run(command, cwd=root, check=False)
            result = {"result": "success" if completed.returncode == 0 else "failure",
                      "exit_code": completed.returncode}
        except OSError as error:
            result = {"result": "failure", "error": str(error)}
        result.update(name=check[0], command=command,
                      duration_seconds=round(time.monotonic() - started, 3))
        results.append(result)
        print(f"::endgroup::\n{check[0]}: {result['result']}", flush=True)
    return {"schema_version": 1, "commit_sha": commit_sha, "checks": results,
            "result": "success" if results and all(
                result["result"] == "success" for result in results) else "failure"}


def evaluate_gate(jobs, commit_sha):
    """A skipped, cancelled, missing or different-commit job cannot release."""
    checks = []
    for name in ("host-tests", "manager-tests"):
        job = jobs.get(name, {})
        tested_sha = job.get("outputs", {}).get("commit_sha", "")
        checks.append({"name": name, "result": job.get("result", "missing"),
                       "commit_sha": tested_sha,
                       "matches_commit": bool(commit_sha) and tested_sha == commit_sha})
    return {"schema_version": 1, "commit_sha": commit_sha, "checks": checks,
            "result": "success" if all(check["result"] == "success" and
                check["matches_commit"] for check in checks) else "failure"}


def write_report(report, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with Path(summary).open("a", encoding="utf-8") as stream:
            stream.write(f"## SUMH regression results\n\nCommit: `{report['commit_sha']}`\n\n")
            stream.write("| Check | Result | Commit matches |\n| --- | --- | --- |\n")
            for check in report["checks"]:
                stream.write(f"| {check['name']} | {check['result']} | "
                             f"{check.get('matches_commit', 'current checkout')} |\n")
            stream.write(f"\nGate result: **{report['result']}**\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("host", "gate"))
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    commit_sha = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    if args.mode == "host":
        report = run_native_checks(root, commit_sha)
    else:
        report = evaluate_gate(json.loads(os.environ["SUMH_JOB_RESULTS"]), commit_sha)
    write_report(report, args.report)
    if report["result"] != "success":
        return 1
    if output := os.environ.get("GITHUB_OUTPUT"):
        with Path(output).open("a", encoding="utf-8") as stream:
            stream.write(f"commit_sha={commit_sha}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
