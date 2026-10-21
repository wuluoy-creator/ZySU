#!/usr/bin/env python3
"""Independent temporary Git/CMake fixtures for native provenance boundaries."""

import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location("native_provenance", Path(__file__).with_name("native_provenance.py"))
provenance = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(provenance)


class ProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="native-provenance-")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.repo = self.base / "repo"
        self.repo.mkdir()
        for name in ("userspace/ksuinit", "userspace/ksud", "userspace/common", "uapi", "cmake", "scripts"):
            directory = self.repo / name
            directory.mkdir(parents=True, exist_ok=True)
            (directory / "input.txt").write_bytes(b"source\n")
        (self.repo / "userspace/ksud/assets").mkdir()
        (self.repo / "userspace/ksud/assets/installer.sh").write_bytes(b"#!/bin/sh\r\necho test\r\n")
        (self.repo / ".gitignore").write_text("out/\n**/build/\n**/.env*\n**/*.key\n", encoding="utf-8")
        (self.repo / "LICENSE").write_bytes(b"Fixture license evidence only\n")
        self.git("init", "-q")
        self.git("add", ".")
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture")
        self.build = self.repo / "out/build"
        self.build.mkdir(parents=True)
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_CXX_COMPILER:FILEPATH={Path(sys.executable).as_posix()}\n"
            "CMAKE_SYSTEM_NAME:STRING=Android\nCMAKE_SYSTEM_VERSION:STRING=31\n"
            "CMAKE_ANDROID_ARCH_ABI:STRING=arm64-v8a\n"
            "PRIVATE_SIGNING_SECRET:STRING=must-not-be-copied\n", encoding="utf-8")
        (self.build / "build.ninja").write_bytes(b"# fixture build graph\n")
        self.write_toolchain_metadata("ksuinit")
        self.out = self.repo / "out/evidence"
        self.artifact = self.build / "ksuinit"
        self.target = "ksuinit"
        self.metadata = None

    def write_toolchain_metadata(self, target):
        (self.build / "native-toolchain.json").write_text(json.dumps({"schema_version": 1, "target": target,
            "values": {"CMAKE_CXX_COMPILER": Path(sys.executable).as_posix()}}), encoding="utf-8")

    def git(self, *args, cwd=None):
        return subprocess.run(["git", "-C", str(cwd or self.repo), *args], check=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout

    def arguments(self):
        return argparse.Namespace(repo=str(self.repo), target=self.target, build_dir=str(self.build),
                                  out=str(self.out), artifact=[f"{self.target}={self.artifact}"],
                                  dependencies=str(self.metadata) if self.metadata else None)

    def snapshot(self):
        provenance.snapshot(self.arguments())
        return self.out / "source-snapshot.json"

    def finish(self):
        snapshot = self.snapshot()
        self.artifact.write_bytes(b"independent fixture artifact bytes\x00\xff")
        provenance.finalize(argparse.Namespace(snapshot=str(snapshot)))
        manifest = self.out / "build-manifest.json"
        provenance.verify(argparse.Namespace(manifest=str(manifest)))
        return json.loads(manifest.read_text(encoding="utf-8"))

    def dependency(self, *, kind="archive", override=False):
        self.target = "ksud"
        self.write_toolchain_metadata("ksud")
        self.artifact = self.build / "ksud"
        root = self.base / "dependency"
        root.mkdir()
        (root / "library.c").write_bytes(b"int fixture(void) { return 7; }\n")
        (root / "LICENSE").write_bytes(b"Evidence does not infer SPDX license\n")
        if kind == "git":
            self.git("init", "-q", cwd=root)
            self.git("add", ".", cwd=root)
            self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm", "dep", cwd=root)
            expected = {"repository": "https://example.invalid/source.git",
                        "revision": self.git("rev-parse", "HEAD", cwd=root).decode().strip()}
        else:
            expected = {"url": "https://example.invalid/source.tar.gz", "archive_sha256": "a" * 64}
        self.metadata = self.build / "native-dependencies.json"
        self.metadata.write_text(json.dumps({"schema_version": 1, "dependencies": [{
            "name": "fixture", "source_dir": str(root), "kind": kind, "expected": expected,
            "local_override": override, "license_files": ["LICENSE"]}]}), encoding="utf-8")
        return root

    def test_full_lifecycle_hashes_actual_artifact_and_records_dirty_source(self):
        (self.repo / "userspace/ksuinit/untracked.cpp").write_bytes(b"new source")
        manifest = self.finish()
        expected = hashlib.sha256(b"independent fixture artifact bytes\x00\xff").hexdigest()
        self.assertEqual(manifest["artifacts"][0]["sha256"], expected)
        self.assertTrue(manifest["source"]["dirty"])
        snapshot = json.loads((self.out / "source-snapshot.json").read_text())
        self.assertIn("userspace/ksuinit/untracked.cpp", snapshot["inputs"]["source"]["files"])
        self.assertEqual(snapshot["inputs"]["toolchain"]["tools"]["CMAKE_CXX_COMPILER"]["sha256"],
                         hashlib.sha256(Path(sys.executable).resolve().read_bytes()).hexdigest())
        self.assertNotIn("must-not-be-copied", json.dumps(snapshot))
        self.assertIn("partial", (self.out / "native-sbom.cdx.json").read_text())

    def test_relevant_modified_source_rejected(self):
        path = self.snapshot()
        self.artifact.write_bytes(b"new")
        (self.repo / "uapi/input.txt").write_bytes(b"changed")
        with self.assertRaisesRegex(provenance.EvidenceError, "changed since snapshot"):
            provenance.finalize(argparse.Namespace(snapshot=str(path)))
        self.assertFalse((self.out / "build-manifest.json").exists())

    def test_new_relevant_untracked_source_rejected(self):
        path = self.snapshot()
        self.artifact.write_bytes(b"new")
        (self.repo / "userspace/common/new.hpp").write_bytes(b"changed")
        with self.assertRaises(provenance.EvidenceError):
            provenance.finalize(argparse.Namespace(snapshot=str(path)))

    def test_deleted_relevant_source_rejected(self):
        path = self.snapshot()
        self.artifact.write_bytes(b"new")
        (self.repo / "cmake/input.txt").unlink()
        with self.assertRaises(provenance.EvidenceError):
            provenance.finalize(argparse.Namespace(snapshot=str(path)))

    def test_unrelated_document_edit_is_recorded_and_allowed(self):
        path = self.snapshot()
        self.artifact.write_bytes(b"new")
        (self.repo / "notes.md").write_text("Unrelated documentation", encoding="utf-8")
        provenance.finalize(argparse.Namespace(snapshot=str(path)))
        value = json.loads((self.out / "build-manifest.json").read_text())
        self.assertFalse(value["source"]["dirty"])
        self.assertTrue(value["source_after"]["dirty"])

    def test_preexisting_artifact_rejected(self):
        self.artifact.write_bytes(b"stale")
        with self.assertRaisesRegex(provenance.EvidenceError, "stale artifact"):
            self.snapshot()

    def test_missing_and_empty_required_artifact_rejected(self):
        path = self.snapshot()
        for empty in (False, True):
            if empty:
                self.artifact.touch()
            with self.assertRaises(provenance.EvidenceError):
                provenance.finalize(argparse.Namespace(snapshot=str(path)))
        self.assertFalse((self.out / "build-manifest.json").exists())

    def test_artifact_mutation_rejected_by_verify(self):
        self.finish()
        self.artifact.write_bytes(b"different final executable")
        with self.assertRaisesRegex(provenance.EvidenceError, "artifact checksum"):
            provenance.verify(argparse.Namespace(manifest=str(self.out / "build-manifest.json")))

    def test_build_configuration_change_rejected(self):
        path = self.snapshot()
        self.artifact.write_bytes(b"new")
        (self.build / "build.ninja").write_text("changed compile flags", encoding="utf-8")
        with self.assertRaises(provenance.EvidenceError):
            provenance.finalize(argparse.Namespace(snapshot=str(path)))

    def test_normal_cmake_compiler_variable_metadata_overrides_missing_cache(self):
        cache = self.build / "CMakeCache.txt"
        cache.write_text("CMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")
        self.finish()
        snapshot = json.loads((self.out / "source-snapshot.json").read_text())
        self.assertIn("CMAKE_CXX_COMPILER", snapshot["inputs"]["toolchain"]["tools"])

    def test_configured_metadata_never_falls_back_to_stale_cached_launcher(self):
        with (self.build / "CMakeCache.txt").open("a", encoding="utf-8") as stream:
            stream.write("CMAKE_CXX_COMPILER_LAUNCHER:STRING=nonexistent-stale-compiler-launcher\n")
        self.snapshot()
        snapshot = json.loads((self.out / "source-snapshot.json").read_text())
        self.assertNotIn("CMAKE_CXX_COMPILER_LAUNCHER", snapshot["inputs"]["toolchain"]["tools"])

    def test_actual_compiler_metadata_cannot_be_omitted(self):
        (self.build / "native-toolchain.json").unlink()
        with self.assertRaisesRegex(provenance.EvidenceError, "missing input"):
            self.snapshot()

    def test_ninja_nested_rule_mutation_is_bound(self):
        rules = self.build / "CMakeFiles/rules.ninja"
        rules.parent.mkdir()
        rules.write_text("rule CXX_COMPILER\n  command = clang++ -c\n", encoding="utf-8")
        (self.build / "build.ninja").write_text("include CMakeFiles/rules.ninja\n", encoding="utf-8")
        path = self.snapshot()
        self.artifact.write_bytes(b"new")
        rules.write_text("rule CXX_COMPILER\n  command = different-compiler -c\n", encoding="utf-8")
        with self.assertRaises(provenance.EvidenceError):
            provenance.finalize(argparse.Namespace(snapshot=str(path)))

    def test_dynamic_or_escaping_ninja_include_rejected(self):
        for line in ("include $other/rules.ninja\n", "subninja ../outside.ninja\n"):
            (self.build / "build.ninja").write_text(line, encoding="utf-8")
            with self.assertRaises(provenance.EvidenceError):
                self.snapshot()

    def test_revalidation_does_not_execute_snapshot_selected_tools(self):
        path = self.snapshot()
        self.artifact.write_bytes(b"new")
        original = provenance.command
        def no_version_invocation(args, *positional, **kwargs):
            self.assertNotIn("--version", args)
            return original(args, *positional, **kwargs)
        with patch.object(provenance, "command", side_effect=no_version_invocation):
            provenance.finalize(argparse.Namespace(snapshot=str(path)))
            provenance.verify(argparse.Namespace(manifest=str(self.out / "build-manifest.json")))

    def test_snapshot_cannot_overwrite_prior_evidence(self):
        self.snapshot()
        with self.assertRaises(provenance.EvidenceError):
            self.snapshot()

    def test_finalize_cannot_overwrite_prior_success(self):
        self.finish()
        previous = (self.out / "build-manifest.json").read_bytes()
        with self.assertRaisesRegex(provenance.EvidenceError, "already exists"):
            provenance.finalize(argparse.Namespace(snapshot=str(self.out / "source-snapshot.json")))
        self.assertEqual(previous, (self.out / "build-manifest.json").read_bytes())

    def test_source_output_overlap_rejected(self):
        self.out = self.repo / "userspace/ksuinit/evidence"
        with self.assertRaisesRegex(provenance.EvidenceError, "overlaps"):
            self.snapshot()

    def test_artifact_outside_build_or_duplicate_identity_rejected(self):
        for entries in ([f"ksuinit={self.repo / 'binary'}"],
                        [f"ksuinit={self.artifact}", f"KSUINIT={self.build / 'second'}"],
                        [f"first={self.artifact}", f"second={self.artifact}"]):
            args = self.arguments()
            args.artifact = entries
            with self.assertRaises(provenance.EvidenceError):
                provenance.snapshot(args)

    def test_path_traversal_and_duplicate_json_keys_rejected(self):
        for value in ("../license", "/etc/passwd", "a/../b", "C:/file", "a\\b", "a//b"):
            with self.assertRaises(provenance.EvidenceError):
                provenance.relative_path(value)
        path = self.base / "duplicate.json"
        path.write_text('{"schema_version":1,"schema_version":2}', encoding="utf-8")
        with self.assertRaises(provenance.EvidenceError):
            provenance.load_json(path)

    def test_future_snapshot_format_and_digest_tampering_rejected(self):
        path = self.snapshot()
        value = json.loads(path.read_text())
        value["schema_version"] = 99
        path.write_text(json.dumps(value))
        with self.assertRaisesRegex(provenance.EvidenceError, "unsupported snapshot"):
            provenance.checked_snapshot(path)
        value["schema_version"] = 1
        value["inputs"]["source"]["git"]["commit"] = "a" * 40
        path.write_text(json.dumps(value))
        with self.assertRaisesRegex(provenance.EvidenceError, "digest mismatch"):
            provenance.checked_snapshot(path)

    def test_secret_files_and_build_outputs_are_excluded(self):
        for name in (".env", ".env.local", "signing.key"):
            (self.repo / "userspace/ksuinit" / name).write_text("fixture-secret", encoding="utf-8")
        nested = self.repo / "userspace/ksuinit/build"
        nested.mkdir()
        (nested / "generated.cpp").write_text("generated output", encoding="utf-8")
        self.snapshot()
        text = (self.out / "source-snapshot.json").read_text()
        self.assertNotIn("fixture-secret", text)
        self.assertNotIn("userspace/ksuinit/build/generated.cpp", text)

    def test_source_symlink_rejected(self):
        link = self.repo / "userspace/ksuinit/link.cpp"
        try:
            link.symlink_to(self.repo / "uapi/input.txt")
        except OSError as error:
            self.skipTest(f"host cannot create symlink: {error}")
        with self.assertRaisesRegex(provenance.EvidenceError, "symlink"):
            self.snapshot()

    def test_tool_symlink_resolved_and_hashed(self):
        link = self.base / ("python-link.exe" if os.name == "nt" else "python-link")
        try:
            link.symlink_to(Path(sys.executable))
        except OSError as error:
            self.skipTest(f"host cannot create symlink: {error}")
        record = provenance.tool_record(str(link))
        self.assertEqual(record["resolved_path"], str(Path(sys.executable).resolve()))
        self.assertNotEqual(record["invocation"], record["resolved_path"])

    def test_archive_source_and_license_evidence_never_claim_download_verified(self):
        root = self.dependency()
        manifest = self.finish()
        snapshot = json.loads((self.out / "source-snapshot.json").read_text())
        component = snapshot["inputs"]["dependencies"]["components"][0]
        self.assertEqual(component["comparison"], "archive_expected_only_extracted_tree_fingerprinted")
        self.assertEqual(component["files"]["library.c"]["sha256"], hashlib.sha256((root / "library.c").read_bytes()).hexdigest())
        evidence = manifest["license_evidence"][0]
        self.assertEqual((self.out / evidence["evidence_path"]).read_bytes(), (root / "LICENSE").read_bytes())
        sbom = json.loads((self.out / "native-sbom.cdx.json").read_text())
        self.assertNotIn("licenses", sbom["components"][0])

    def test_missing_or_tampered_license_evidence_rejected(self):
        self.dependency()
        manifest = self.finish()
        evidence_path = self.out / manifest["license_evidence"][0]["evidence_path"]
        evidence_path.write_bytes(b"different license")
        with self.assertRaisesRegex(provenance.EvidenceError, "license evidence checksum"):
            provenance.verify(argparse.Namespace(manifest=str(self.out / "build-manifest.json")))

    def test_manifest_cannot_omit_required_license_or_artifact_identity(self):
        self.dependency()
        manifest = self.finish()
        path = self.out / "build-manifest.json"
        for key, message in (("license_evidence", "license evidence identities"), ("artifacts", "artifact identities")):
            altered = dict(manifest)
            altered[key] = []
            path.write_text(json.dumps(altered))
            with self.assertRaisesRegex(provenance.EvidenceError, message):
                provenance.verify(argparse.Namespace(manifest=str(path)))

    def test_local_override_explicit_and_actual_tree_mutation_rejected(self):
        root = self.dependency(override=True)
        path = self.snapshot()
        value = json.loads(path.read_text())
        self.assertTrue(value["inputs"]["dependencies"]["components"][0]["local_override"])
        self.assertIn("not_verified", value["inputs"]["dependencies"]["components"][0]["comparison"])
        (root / "library.c").write_bytes(b"changed library implementation")
        self.artifact.write_bytes(b"new")
        with self.assertRaises(provenance.EvidenceError):
            provenance.finalize(argparse.Namespace(snapshot=str(path)))

    def test_actual_git_head_must_match_expected_pin_without_override(self):
        self.dependency(kind="git")
        metadata = json.loads(self.metadata.read_text())
        metadata["dependencies"][0]["expected"]["revision"] = "f" * 40
        self.metadata.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(provenance.EvidenceError, "commit mismatch"):
            self.snapshot()

    def test_git_match_record_uses_real_repository_head(self):
        root = self.dependency(kind="git")
        self.snapshot()
        value = json.loads((self.out / "source-snapshot.json").read_text())
        component = value["inputs"]["dependencies"]["components"][0]
        self.assertEqual(component["comparison"], "git_commit_matches")
        self.assertEqual(component["git"]["commit"], self.git("rev-parse", "HEAD", cwd=root).decode().strip())

    def test_submodule_gitlink_and_actual_dirty_source_are_independent_evidence(self):
        root = self.dependency(kind="git")
        self.git("-c", "protocol.file.allow=always", "submodule", "add", "-q", str(root),
                 "userspace/ksud/third_party/fixture-submodule")
        child = self.repo / "userspace/ksud/third_party/fixture-submodule"
        (child / "library.c").write_bytes(b"dirty actual submodule source")
        self.snapshot()
        value = json.loads((self.out / "source-snapshot.json").read_text())
        component = next(item for item in value["inputs"]["dependencies"]["components"]
                         if item["name"] == "submodule-fixture-submodule")
        self.assertEqual(component["expected"]["gitlink"], self.git("rev-parse", "HEAD", cwd=child).decode().strip())
        self.assertTrue(component["git"]["dirty"])
        self.assertEqual(component["files"]["library.c"]["sha256"], hashlib.sha256(b"dirty actual submodule source").hexdigest())

    def test_uninitialized_submodule_is_not_silently_omitted(self):
        root = self.dependency(kind="git")
        self.git("-c", "protocol.file.allow=always", "submodule", "add", "-q", str(root),
                 "userspace/ksud/third_party/fixture-submodule")
        self.git("submodule", "deinit", "--force", "userspace/ksud/third_party/fixture-submodule")
        with self.assertRaisesRegex(provenance.EvidenceError, "uninitialized submodule"):
            self.snapshot()

    def test_missing_dependency_and_duplicate_identity_fail(self):
        root = self.dependency()
        metadata = json.loads(self.metadata.read_text())
        metadata["dependencies"].append(dict(metadata["dependencies"][0]))
        self.metadata.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(provenance.EvidenceError, "duplicate dependency"):
            self.snapshot()
        metadata["dependencies"].pop()
        self.metadata.write_text(json.dumps(metadata))
        shutil.rmtree(root)
        with self.assertRaisesRegex(provenance.EvidenceError, "missing input"):
            self.snapshot()

    def test_metadata_requires_explicit_override_and_license_status(self):
        self.dependency()
        metadata = json.loads(self.metadata.read_text())
        del metadata["dependencies"][0]["local_override"]
        self.metadata.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(provenance.EvidenceError, "override state"):
            self.snapshot()
        metadata["dependencies"][0]["local_override"] = False
        metadata["dependencies"][0]["license_files"] = []
        self.metadata.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(provenance.EvidenceError, "license evidence status"):
            self.snapshot()

    def test_ksud_missing_metadata_fails(self):
        self.target = "ksud"
        with self.assertRaisesRegex(provenance.EvidenceError, "requires configured dependency"):
            self.snapshot()

    def test_embedded_ksuinit_and_kmi_captured_even_if_git_ignored(self):
        self.dependency()
        assets = self.repo / "userspace/ksud/assets"
        (assets / "ksuinit").write_bytes(b"embedded init")
        (assets / "android14-6.1_kernelsu.ko").write_bytes(b"embedded module")
        (assets / "not-embedded.bin").write_bytes(b"other")
        self.snapshot()
        value = json.loads((self.out / "source-snapshot.json").read_text())
        records = {item["name"]: item for item in value["inputs"]["embedded_assets"]}
        self.assertEqual(records["ksuinit"]["sha256"], hashlib.sha256(b"embedded init").hexdigest())
        self.assertEqual(records["android14-6.1_kernelsu.ko"]["kmi"], "android14-6.1")
        self.assertNotIn("not-embedded.bin", records)

    def test_embedded_asset_mutation_rejected(self):
        self.dependency()
        asset = self.repo / "userspace/ksud/assets/ksuinit"
        asset.write_bytes(b"first init")
        path = self.snapshot()
        asset.write_bytes(b"changed init")
        self.artifact.write_bytes(b"new")
        with self.assertRaises(provenance.EvidenceError):
            provenance.finalize(argparse.Namespace(snapshot=str(path)))

    def test_git_describe_tag_changes_rejected_for_generated_version(self):
        self.dependency()
        path = self.snapshot()
        self.artifact.write_bytes(b"new")
        self.git("tag", "v99.1.0")
        with self.assertRaises(provenance.EvidenceError):
            provenance.finalize(argparse.Namespace(snapshot=str(path)))

    def test_out_kernel_version_input_changes_rejected(self):
        self.dependency()
        asset = self.repo / "out/android14-6.1_kernelsu.ko"
        asset.write_bytes(b"kernel build version 1.8.1-nc")
        path = self.snapshot()
        self.artifact.write_bytes(b"new")
        asset.write_bytes(b"changed kernel version 1.8.2-nc")
        with self.assertRaises(provenance.EvidenceError):
            provenance.finalize(argparse.Namespace(snapshot=str(path)))

    def test_ksud_version_environment_changes_rejected(self):
        self.dependency()
        with patch.dict(os.environ, {"ZYSU_KSUD_VERSION_NAME": "1.8.1"}):
            path = self.snapshot()
        self.artifact.write_bytes(b"new")
        with patch.dict(os.environ, {"ZYSU_KSUD_VERSION_NAME": "1.8.2"}):
            with self.assertRaises(provenance.EvidenceError):
                provenance.finalize(argparse.Namespace(snapshot=str(path)))

    def test_bounded_files_and_tree_reject_large_inputs(self):
        path = self.repo / "userspace/ksuinit/input.txt"
        with self.assertRaisesRegex(provenance.EvidenceError, "exceeds limit"):
            provenance.file_record(path, limit=1)
        with patch.object(provenance, "MAX_FILES", 1):
            with self.assertRaisesRegex(provenance.EvidenceError, "exceeds bounds"):
                self.snapshot()

    def test_large_tool_output_is_bounded(self):
        with patch.object(provenance, "MAX_COMMAND", 32):
            with self.assertRaisesRegex(provenance.EvidenceError, "output exceeds limit"):
                provenance.command([sys.executable, "-c", "print('x' * 128)"])

    def test_nonregular_input_rejected(self):
        with self.assertRaisesRegex(provenance.EvidenceError, "not a regular file"):
            provenance.file_record(self.repo)

    def test_cli_json_failure_stderr_and_nonzero_exit(self):
        output, errors = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            result = provenance.main(["verify", "--manifest", str(self.base / "missing.json")])
        self.assertEqual(result, 1)
        self.assertEqual(output.getvalue(), "")
        self.assertEqual(json.loads(errors.getvalue())["status"], "error")


if __name__ == "__main__":
    unittest.main(verbosity=2)
