"""Exercise the SUMH runner and fail-closed release gate with real processes."""

import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from sumh_ci import NATIVE_CHECKS, evaluate_gate, run_native_checks


class SumhCiTests(unittest.TestCase):
    SHA = "a" * 40

    def successful_jobs(self):
        return {name: {"result": "success", "outputs": {"commit_sha": self.SHA}}
                for name in ("host-tests", "manager-tests")}

    def test_both_same_commit_jobs_allow_release(self):
        self.assertEqual(evaluate_gate(self.successful_jobs(), self.SHA)["result"], "success")

    def test_native_or_ui_failure_skip_cancellation_or_missing_job_block_release(self):
        for name in ("host-tests", "manager-tests"):
            for status in ("failure", "skipped", "cancelled", None):
                with self.subTest(name=name, status=status):
                    jobs = self.successful_jobs()
                    if status is None:
                        del jobs[name]
                    else:
                        jobs[name]["result"] = status
                    self.assertEqual(evaluate_gate(jobs, self.SHA)["result"], "failure")

    def test_other_or_missing_commit_cannot_approve_current_build(self):
        for name in ("host-tests", "manager-tests"):
            for sha in ("b" * 40, ""):
                with self.subTest(name=name, sha=sha):
                    jobs = self.successful_jobs()
                    jobs[name]["outputs"]["commit_sha"] = sha
                    self.assertEqual(evaluate_gate(jobs, self.SHA)["result"], "failure")
        self.assertEqual(evaluate_gate(self.successful_jobs(), "")["result"], "failure")

    def test_runner_executes_main_records_failure_and_continues(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, exit_code in (("pass", 0), ("fail", 7), ("later", 0)):
                (root / f"{name}.py").write_text(
                    f"from pathlib import Path\nif __name__ == '__main__':\n"
                    f"    Path('{name}.ran').write_text('executed')\n"
                    f"    raise SystemExit({exit_code})\n", encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()):
                report = run_native_checks(root, self.SHA,
                    [(f"{name}.py",) for name in ("pass", "fail", "later")])
            self.assertEqual(report["result"], "failure")
            self.assertEqual([check["exit_code"] for check in report["checks"]], [0, 7, 0])
            self.assertTrue(all((root / f"{name}.ran").exists() for name in ("pass", "fail", "later")))
            self.assertEqual(report["commit_sha"], self.SHA)

    def test_empty_or_missing_suite_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run_native_checks(directory, self.SHA, [])["result"], "failure")
            self.assertEqual(run_native_checks(directory, self.SHA, [("missing.py",)])["result"], "failure")

    def test_all_native_entrypoints_exist_and_have_no_duplicates(self):
        root = Path(__file__).resolve().parents[2]
        paths = [check[0] for check in NATIVE_CHECKS]
        self.assertEqual(len(paths), len(set(paths)))
        for path in paths:
            self.assertTrue((root / path).is_file(), path)

    def test_gate_command_writes_evidence_and_only_exports_successful_commit(self):
        script = Path(__file__).with_name("sumh_ci.py")
        root = script.resolve().parents[2]
        sha = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
        for failed_job in (None, "host-tests", "manager-tests"):
            with self.subTest(failed_job=failed_job), tempfile.TemporaryDirectory() as directory:
                directory = Path(directory)
                jobs = {name: {"result": "success", "outputs": {"commit_sha": sha}}
                        for name in ("host-tests", "manager-tests")}
                if failed_job:
                    jobs[failed_job]["result"] = "failure"
                report, output, summary = (directory / name for name in ("report.json", "output", "summary"))
                result = subprocess.run(
                    [sys.executable, str(script), "gate", "--report", str(report)],
                    env=dict(os.environ, SUMH_JOB_RESULTS=json.dumps(jobs),
                             GITHUB_OUTPUT=str(output), GITHUB_STEP_SUMMARY=str(summary)),
                    capture_output=True, text=True)
                self.assertEqual(result.returncode, 1 if failed_job else 0, result.stderr)
                data = json.loads(report.read_text(encoding="utf-8"))
                self.assertEqual(data["commit_sha"], sha)
                self.assertEqual(data["result"], "failure" if failed_job else "success")
                self.assertIn(sha, summary.read_text(encoding="utf-8"))
                if failed_job:
                    self.assertFalse(output.exists())
                else:
                    self.assertEqual(output.read_text(encoding="utf-8"), f"commit_sha={sha}\n")


if __name__ == "__main__":
    unittest.main()
