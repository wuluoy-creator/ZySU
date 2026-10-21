#!/usr/bin/env python3
"""Offline CLI boundary tests for the CI dependency lock and workflow checks."""

import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/ci/ci_dependencies.py"
SHA = "1" * 40
OTHER_SHA = "2" * 40
IMAGE = "ghcr.io/ylarod/ddk-min@sha256:" + "a" * 64
IMAGE_EXPRESSION = "${{ needs.resolve-image.outputs.image }}"


def fixture_lock():
    return {
        "schema_version": 1,
        "observed_at": "2026-10-19T00:00:00Z",
        "actions": [{"action": "example/checkout", "version": "v1", "commit": SHA}],
        "unresolved_actions": [{
            "action": "Kernel-SU/actions-comment-on-issue", "version": "master",
            "reason": "Synthetic unavailable reference; restricted issue workflow only",
        }],
        "ddk_images": [{
            "kmi": "android14-6.1", "release": "20260828",
            "repository": "ghcr.io/ylarod/ddk-min", "digest": "sha256:" + "a" * 64,
            "platform": "linux/amd64",
        }],
    }


class CiDependencyTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ci-dependencies-")
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name) / "repository with spaces 测试"
        self.repo.mkdir()
        self.lock_path = self.repo / ".github/ci-dependencies.lock.json"
        self.output = self.repo / "github output.txt"
        self.lock = fixture_lock()
        self.write_lock(self.lock)
        self.workflow("ddk-lkm.yml", f"""name: Fixture
on: workflow_dispatch
jobs:
  build:
    runs-on: ubuntu-latest
    container:
      image: {IMAGE_EXPRESSION}
    steps:
      - uses: example/checkout@{SHA} # v1
""")

    def write_lock(self, value):
        self.lock_path.parent.mkdir(parents=True, exist_ok=True)
        self.lock_path.write_text(json.dumps(value), encoding="utf-8")

    def workflow(self, name, source):
        path = self.repo / ".github/workflows" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source, encoding="utf-8")
        return path

    def add_uses(self, reference, name="build.yml"):
        return self.workflow(name, "name: Fixture\non: workflow_dispatch\njobs:\n"
                             "  build:\n    runs-on: ubuntu-latest\n    steps:\n"
                             f"      - uses: {reference}\n")

    def run_cli(self, *arguments, success=True):
        environment = dict(os.environ)
        environment.update(GITHUB_OUTPUT=str(self.output), PYTHONIOENCODING="utf-8")
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--repo", str(self.repo), *arguments],
            cwd=self.repo, env=environment, capture_output=True, text=True,
            encoding="utf-8", errors="strict", timeout=15,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
            payload = json.loads(result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout, "")
            payload = json.loads(result.stderr)
            self.assertIsInstance(payload.get("error"), str)
            self.assertTrue(payload["error"])
        self.assertEqual(payload["schema_version"], 1)
        return payload

    def test_resolve_exact_pair_and_append_single_github_output_record(self):
        self.output.write_text("existing=value\n", encoding="utf-8")
        result = self.run_cli("resolve-ddk", "--kmi", "android14-6.1", "--release", "20260828")
        self.assertEqual(result, {"schema_version": 1, "image": IMAGE, "platform": "linux/amd64"})
        self.assertEqual(self.output.read_text(encoding="utf-8"), "existing=value\nimage=" + IMAGE + "\n")

    def test_all_current_kmi_selectors_have_distinct_exact_images(self):
        self.lock["ddk_images"] = []
        expected = {}
        for index, kmi in enumerate(("android14-6.1", "android15-6.6", "android16-6.12", "android17-6.18")):
            item = copy.deepcopy(fixture_lock()["ddk_images"][0])
            item.update(kmi=kmi, digest="sha256:" + str(index + 1) * 64)
            self.lock["ddk_images"].append(item)
            expected[kmi] = item["repository"] + "@" + item["digest"]
        self.write_lock(self.lock)
        for kmi, image in expected.items():
            with self.subTest(kmi=kmi):
                self.assertEqual(self.run_cli("resolve-ddk", "--kmi", kmi, "--release", "20260828")["image"], image)

    def test_unknown_future_and_injected_selectors_leave_output_unchanged(self):
        for kmi, release in (
            ("android18-6.20", "20260828"), ("android14-6.1", "20270101"),
            ("android14-6.1 ", "20260828"), ("android14-6.1\nimage=evil", "20260828"),
            ("$(touch sentinel)", "20260828"), ("android14-6.1", "20260828\nimage=evil"),
        ):
            with self.subTest(kmi=kmi, release=release):
                self.output.write_text("untouched=value\n", encoding="utf-8")
                self.run_cli("resolve-ddk", "--kmi", kmi, "--release", release, success=False)
                self.assertEqual(self.output.read_text(encoding="utf-8"), "untouched=value\n")
                self.assertFalse((self.repo / "sentinel").exists())

    def test_missing_lock_returns_structured_failure_without_output_file(self):
        self.lock_path.unlink()
        self.run_cli("resolve-ddk", "--kmi", "android14-6.1", "--release", "20260828", success=False)
        self.assertFalse(self.output.exists())

    def test_output_write_failure_is_nonzero_and_not_success_json(self):
        self.output.mkdir()
        self.run_cli("resolve-ddk", "--kmi", "android14-6.1", "--release", "20260828", success=False)

    def test_future_boolean_schema_missing_unknown_and_empty_fields_rejected(self):
        for mutate in (
            lambda lock: lock.update(schema_version=2),
            lambda lock: lock.update(schema_version=True),
            lambda lock: lock.pop("ddk_images"),
            lambda lock: lock.update(unrecognized=True),
            lambda lock: lock.update(actions=[]),
            lambda lock: lock.update(ddk_images=[]),
            lambda lock: lock.update(observed_at=""),
            lambda lock: lock.update(actions={}),
            lambda lock: lock["actions"][0].update(unrecognized=True),
        ):
            lock = fixture_lock()
            mutate(lock)
            with self.subTest(lock=lock):
                self.write_lock(lock)
                self.run_cli("check", success=False)

    def test_malformed_duplicate_keys_and_oversized_lock_rejected(self):
        for source in ("not json", '{"schema_version":1,"schema_version":1}', " " * (128 * 1024 + 1)):
            with self.subTest(length=len(source)):
                self.lock_path.write_text(source, encoding="utf-8")
                self.run_cli("check", success=False)

    def test_duplicate_action_ddk_and_unresolved_entries_rejected(self):
        for name in ("actions", "ddk_images", "unresolved_actions"):
            lock = fixture_lock()
            lock[name].append(copy.deepcopy(lock[name][0]))
            with self.subTest(name=name):
                self.write_lock(lock)
                self.run_cli("check", success=False)

    def test_same_action_version_cannot_authorize_two_different_commits(self):
        self.lock["actions"].append({"action": "example/checkout", "version": "v1", "commit": OTHER_SHA})
        self.write_lock(self.lock)
        self.run_cli("check", success=False)

    def test_resolved_action_version_cannot_also_be_unresolved(self):
        self.lock["unresolved_actions"].append({"action": "example/checkout", "version": "v1", "reason": "ambiguous"})
        self.write_lock(self.lock)
        self.run_cli("check", success=False)

    def test_malicious_or_mutable_action_identities_rejected(self):
        for field, value in (
            ("commit", "v1"), ("commit", "g" * 40), ("commit", SHA + "\nrun=evil"),
            ("action", "../checkout"), ("action", "example/checkout;echo evil"),
            ("action", "example/checkout\nrun=evil"), ("version", ""),
        ):
            with self.subTest(field=field, value=value):
                lock = fixture_lock()
                lock["actions"][0][field] = value
                self.write_lock(lock)
                self.run_cli("check", success=False)

    def test_ddk_lock_rejects_other_registry_platform_digest_and_injection(self):
        for field, value in (
            ("repository", "ghcr.io/other/ddk-min"), ("repository", "ghcr.io/ylarod/ddk-min\nimage=evil"),
            ("platform", "linux/arm64"), ("digest", "latest"), ("digest", "sha256:" + "g" * 64),
            ("digest", "sha256:" + "a" * 64 + "\nimage=evil"), ("kmi", "android14-6.1\nimage=evil"),
            ("release", "20260828\nimage=evil"), ("release", 20260828),
        ):
            with self.subTest(field=field, value=value):
                lock = fixture_lock()
                lock["ddk_images"][0][field] = value
                self.write_lock(lock)
                self.run_cli("resolve-ddk", "--kmi", "android14-6.1", "--release", "20260828", success=False)
                self.assertFalse(self.output.exists())

    def test_pinned_comments_quoted_scalars_and_subpath_are_accepted(self):
        self.lock["actions"].append({"action": "example/actions/setup", "version": "v2", "commit": OTHER_SHA})
        self.write_lock(self.lock)
        for reference in (f'"example/actions/setup@{OTHER_SHA}" # v2', f"'example/actions/setup@{OTHER_SHA}' # v2"):
            with self.subTest(reference=reference):
                self.add_uses(reference)
                result = self.run_cli("check")
                self.assertEqual(result["checked_uses"], 2)
                self.assertEqual(result["pinned_uses"], 2)
                self.assertEqual(result["unresolved"], [])

    def test_exact_unresolved_exception_is_visible_only_in_issue_workflow(self):
        self.add_uses("Kernel-SU/actions-comment-on-issue@master", "add-device.yml")
        result = self.run_cli("check")
        self.assertFalse(result["all_execution_dependencies_pinned"])
        self.assertEqual(result["checked_uses"], 2)
        self.assertEqual(result["pinned_uses"], 1)
        self.assertEqual(len(result["unresolved"]), 1)
        self.assertIn(".github/workflows/add-device.yml:", result["unresolved"][0]["location"])

    def test_unresolved_exception_cannot_authorize_build_or_release(self):
        for filename in ("build.yml", "release.yml", "ksud.yml"):
            with self.subTest(filename=filename):
                path = self.add_uses("Kernel-SU/actions-comment-on-issue@master", filename)
                self.run_cli("check", success=False)
                path.unlink()

    def test_unexpected_mutable_wrong_sha_or_expression_rejected(self):
        for reference in ("example/checkout@v1", "example/checkout@main", f"example/checkout@{OTHER_SHA}",
                          "${{ inputs.action }}", "docker://example/image:latest",
                          "Kernel-SU/actions-comment-on-issue@main"):
            with self.subTest(reference=reference):
                self.add_uses(reference)
                self.run_cli("check", success=False)

    def test_mutable_action_in_yaml_workflow_is_not_skipped(self):
        self.add_uses("example/checkout@main", "release.yaml")
        self.run_cli("check", success=False)

    def test_mutable_action_in_yaml_composite_is_not_skipped(self):
        path = self.repo / ".github/actions/helper/action.yaml"
        path.parent.mkdir(parents=True)
        path.write_text("name: helper\nruns:\n  using: composite\n  steps:\n    - uses: example/checkout@main\n", encoding="utf-8")
        self.add_uses("./.github/actions/helper")
        self.run_cli("check", success=False)

    def test_unsupported_inline_and_quoted_uses_keys_fail_explicitly(self):
        for step in ('- {uses: example/checkout@main}',
                     '- "uses": example/checkout@main',
                     "- 'uses': example/checkout@main"):
            with self.subTest(step=step):
                self.workflow("build.yml", "name: Fixture\non: workflow_dispatch\njobs:\n"
                              "  build:\n    runs-on: ubuntu-latest\n    steps:\n"
                              f"      {step}\n")
                result = self.run_cli("check", success=False)
                self.assertIn("unsupported uses layout", result["error"])

    def test_commented_mutable_reference_does_not_become_an_action(self):
        self.workflow("build.yml", "# uses: example/checkout@main\n"
                      "# - {uses: example/checkout@main}\n")
        result = self.run_cli("check")
        self.assertEqual(result["checked_uses"], 1)
        self.assertEqual(result["unresolved"], [])

    def test_existing_local_action_and_reusable_workflow_are_allowed(self):
        action = self.repo / ".github/actions/helper/action.yml"
        action.parent.mkdir(parents=True)
        action.write_text("name: helper\nruns:\n  using: composite\n  steps: []\n", encoding="utf-8")
        self.add_uses("./.github/actions/helper")
        self.workflow("caller.yml", "jobs:\n  call:\n    uses: ./.github/workflows/ddk-lkm.yml\n")
        result = self.run_cli("check")
        self.assertEqual(result["checked_uses"], 1)
        self.assertFalse(self.output.exists())

    def test_missing_and_traversing_local_actions_rejected(self):
        for reference in ("./.github/actions/missing", "./../outside", "./.github/../.github/workflows/ddk-lkm.yml"):
            with self.subTest(reference=reference):
                self.add_uses(reference)
                self.run_cli("check", success=False)

    def test_ddk_image_cannot_bypass_resolver(self):
        path = self.repo / ".github/workflows/ddk-lkm.yml"
        original = path.read_text(encoding="utf-8")
        for expression in ("ghcr.io/ylarod/ddk-min:latest", IMAGE, "${{ inputs.image }}"):
            with self.subTest(expression=expression):
                path.write_text(original.replace(IMAGE_EXPRESSION, expression), encoding="utf-8")
                self.run_cli("check", success=False)

    def test_repository_lock_uses_previously_resolved_release_identities(self):
        actual = json.loads((ROOT / ".github/ci-dependencies.lock.json").read_text(encoding="utf-8"))
        actions = {entry["action"]: entry["commit"] for entry in actual["actions"]}
        self.assertEqual(actions["gradle/actions/setup-gradle"], "3f5f9adaf7d9fecd50b5935e54106014257a94e6")
        self.assertEqual(actions["actions/checkout"], "3d3c42e5aac5ba805825da76410c181273ba90b1")
        expected = {
            "android14-6.1": "1dd6ac340b627a90a4031d6d0df6b129d8cc949b139fb56032c898f023d3d5d3",
            "android15-6.6": "51159bc935c75a282fc65bc87e02f16bd098d1cdf55dc558c4fdd0fcf7a132e2",
            "android16-6.12": "fd3ec5cc71d7f2716212e9df8d5674a9afe68fb769282fcac87b83f207f949ee",
            "android17-6.18": "fb8b66200bf402cf65a751060fc1aa29524b9fa8840feef16c7b196a4f3f0e6e",
        }
        observed = {entry["kmi"]: entry["digest"].removeprefix("sha256:")
                    for entry in actual["ddk_images"] if entry["release"] == "20260828"}
        self.assertEqual(observed, expected)


if __name__ == "__main__":
    unittest.main(verbosity=2)
