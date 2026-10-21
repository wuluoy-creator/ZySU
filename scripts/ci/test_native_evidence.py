#!/usr/bin/env python3
"""Independent portable evidence fixtures; no build tools or original paths."""

import contextlib
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import native_evidence as evidence
import native_provenance


COMMIT = "1" * 40


def record(data):
    return {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}


def encoded(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()


def table_hash(value):
    return hashlib.sha256(encoded(value)).hexdigest()


class Fixture:
    def __init__(self, root, target="ksud"):
        self.root = root
        self.target = target
        self.bundle = root / "relocated/native" / target
        self.bundle.mkdir(parents=True)
        self.binaries = {target: b"native fixture\x00" + target.encode(),
                         target + ".unstripped": b"symbol fixture\x00" + target.encode()}
        self.artifacts = {}
        for key, raw in self.binaries.items():
            path = self.bundle / "artifacts" / key
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(raw)
            self.artifacts[key] = path
        observed = "C:/already-deleted-original-workspace"
        git = {"commit": COMMIT, "dirty": False, "status_sha256": hashlib.sha256(b"").hexdigest()}
        source_files = {"LICENSE": record(b"root license"), f"userspace/{target}/src/main.cpp": record(b"source")}
        licenses = [{"path": "LICENSE", **record(b"root license")}]
        self.license_bytes = {f"licenses/ZySU-{target}/00-LICENSE": b"root license"}
        components, assets = [], []
        if target == "ksud":
            for asset_name, raw in (("installer.sh", b"#!/bin/sh\r\n"), ("ksuinit", b"init executable"),
                                    ("android14-6.1_kernelsu.ko", b"synthetic module")):
                relative = "userspace/ksud/assets/" + asset_name
                source_files[relative] = record(raw)
                asset = {"name": asset_name, "path": observed + "/" + relative, **record(raw),
                         "kind": "ksuinit" if asset_name == "ksuinit" else "runtime_asset"}
                if asset_name.endswith(".ko"):
                    asset.update(kind="kernel_module", kmi="android14-6.1")
                assets.append(asset)
            files = {"LICENSE": record(b"dependency license"), "library.c": record(b"library source")}
            components = [{"name": "fixture", "source_dir": "C:/already-deleted-dependency", "kind": "archive",
                           "expected": {"kind": "archive", "url": "https://example.invalid/dependency.tar.gz",
                                        "archive_sha256": "a" * 64},
                           "resolution": "fetchcontent", "local_override": False,
                           "comparison": "archive_expected_only_extracted_tree_fingerprinted", "git": None,
                           "files": files, "tree_sha256": table_hash(files),
                           "license_evidence": [{"path": "LICENSE", **record(b"dependency license")}],
                           "license_evidence_status": "files_recorded"}]
            self.license_bytes["licenses/fixture/00-LICENSE"] = b"dependency license"
        tool = {"invocation": "C:/not-executed/tool.exe", "resolved_path": "C:/not-executed/tool.exe",
                **record(b"claimed tool bytes"), "symlink_chain": [], "version": "recorded version"}
        self.snapshot = {"schema_version": 1, "kind": "native_source_snapshot", "target": target,
                         "repo": observed, "build_dir": observed + "/out/build", "output_dir": observed + "/out/evidence",
                         "created_ns": 100, "artifacts": [{"name": name, "path": observed + "/out/build/" + name}
                                                         for name in self.binaries],
                         "inputs": {"source": {"git": git, "roots": [f"userspace/{target}"], "files": source_files,
                                              "license_evidence": licenses, "tree_sha256": table_hash(source_files),
                                              "exclusions": {"directories": ["build*"], "files": ["*.key"]}},
                                    "dependencies": {"metadata_file": observed + "/out/build/native-dependencies.json" if target == "ksud" else None,
                                                     "metadata_sha256": "d" * 64 if target == "ksud" else None,
                                                     "inactive_declarations": [], "components": components},
                                    "toolchain": {"settings": {"CMAKE_SYSTEM_NAME": "Android"},
                                                  "tools": {"CMAKE_CXX_COMPILER": copy.deepcopy(tool), "provenance_python": copy.deepcopy(tool)},
                                                  "files": {observed + "/out/build/build.ninja": record(b"claimed graph")}},
                                    "embedded_assets": assets,
                                    "version_inputs": {"environment": {"ZYSU_KSUD_VERSION_NAME": "", "ZYSU_KSUD_VERSION_CODE": ""},
                                                       "describe": "1.8.1", "commit_count": "1234",
                                                       "kernel_status": {"dirty": False, "status_sha256": "c" * 64},
                                                       "ksud_status": {"dirty": False, "status_sha256": "c" * 64},
                                                       "additional_kernel_version_inputs": {}} if target == "ksud" else None},
                         "inputs_sha256": "", "limitations": list(native_provenance.LIMITATIONS)}
        self.manifest = {"schema_version": 1, "kind": "native_build_manifest", "target": target, "created_ns": 101,
                         "snapshot": {}, "inputs_sha256": "", "source": copy.deepcopy(git), "source_after": copy.deepcopy(git),
                         "artifacts": [{**entry, **record(self.binaries[entry["name"]])} for entry in self.snapshot["artifacts"]],
                         "license_evidence": [], "sbom": {}, "limitations": list(native_provenance.LIMITATIONS)}
        self.refresh()

    def refresh(self, *, source_hash=True):
        value = self.snapshot
        if source_hash:
            value["inputs"]["source"]["tree_sha256"] = table_hash(value["inputs"]["source"]["files"])
            for component in value["inputs"]["dependencies"]["components"]:
                component["tree_sha256"] = table_hash(component["files"])
        value["inputs_sha256"] = table_hash(value["inputs"])
        self.manifest["inputs_sha256"] = value["inputs_sha256"]
        license_records = []
        components = list(value["inputs"]["dependencies"]["components"])
        components.append({"name": "ZySU-" + self.target, "license_evidence": value["inputs"]["source"]["license_evidence"]})
        for component in components:
            for index, entry in enumerate(component["license_evidence"]):
                license_records.append({"component": component["name"], "source_relative_path": entry["path"],
                                        "evidence_path": f"licenses/{component['name']}/{index:02d}-{entry['path'].rsplit('/', 1)[-1]}",
                                        "sha256": entry["sha256"], "size": entry["size"]})
        self.manifest["license_evidence"] = license_records
        source = value["inputs"]["source"]
        application = {"type": "application", "bom-ref": "zysu:" + self.target, "name": "ZySU " + self.target,
                       "version": source["git"]["commit"], "properties": [
                           {"name": "zysu:source-tree-sha256", "value": source["tree_sha256"]},
                           {"name": "zysu:checkout-dirty", "value": str(source["git"]["dirty"]).lower()},
                           {"name": "zysu:license-evidence", "value": encoded([v for v in license_records if v["component"] == "ZySU-" + self.target]).decode()}]}
        libraries = []
        for dep in value["inputs"]["dependencies"]["components"]:
            item = {"type": "library", "bom-ref": "native:" + dep["name"], "name": dep["name"], "properties": [
                {"name": "zysu:expected-identity", "value": encoded(dep["expected"]).decode()},
                {"name": "zysu:comparison", "value": dep["comparison"]},
                {"name": "zysu:local-override", "value": str(dep["local_override"]).lower()},
                {"name": "zysu:source-tree-sha256", "value": dep["tree_sha256"]},
                {"name": "zysu:license-evidence", "value": encoded([v for v in license_records if v["component"] == dep["name"]]).decode()}]}
            if dep["git"]:
                item["version"] = dep["git"]["commit"]
            libraries.append(item)
        self.sbom = {"bomFormat": "CycloneDX", "specVersion": "1.5", "version": 1,
                     "metadata": {"component": application, "properties": [
                         {"name": "zysu:coverage", "value": "partial-native-source-components"},
                         {"name": "zysu:limitations", "value": encoded(native_provenance.LIMITATIONS).decode()}]},
                     "components": libraries + [{"type": "file", "bom-ref": "artifact:" + item["name"],
                                                  "name": item["name"], "hashes": [{"alg": "SHA-256", "content": item["sha256"]}]}
                                                 for item in self.manifest["artifacts"]]}
        for path, raw in self.license_bytes.items():
            destination = self.bundle / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(raw)
        for key, filename, document in (("snapshot", "source-snapshot.json", self.snapshot),
                                        ("sbom", "native-sbom.cdx.json", self.sbom)):
            raw = encoded(document)
            (self.bundle / filename).write_bytes(raw)
            self.manifest[key] = {"path": filename, **record(raw)}
        self.write_manifest()

    def write_manifest(self):
        (self.bundle / "build-manifest.json").write_bytes(encoded(self.manifest))

    def verify(self, **kwargs):
        return evidence.verify_bundle(self.bundle / "build-manifest.json", self.artifacts,
                                      expected_target=kwargs.get("target", self.target), expected_commit=kwargs.get("commit", COMMIT))


class PortableTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="native-evidence-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.fixture = Fixture(self.root)

    def test_portable_full_roles_and_explicit_scope(self):
        result = self.fixture.verify()
        self.assertEqual(result["coverage"], "portable_evidence_consistency")
        self.assertEqual(result["authentication"], "unsigned_unverified")
        self.assertFalse(result["source_revalidated"])
        self.assertFalse(result["toolchain_revalidated"])
        self.assertFalse(result["embedded_bytes_verified"])
        self.assertEqual({item["name"] for item in result["artifacts"]}, {"ksud", "ksud.unstripped"})
        self.assertEqual(result["artifacts"][0]["sha256"], hashlib.sha256(self.fixture.binaries["ksud"]).hexdigest())
        self.assertEqual(set(result["evidence_files"]), {"build-manifest.json", "source-snapshot.json", "native-sbom.cdx.json",
                                                      "licenses/fixture/00-LICENSE", "licenses/ZySU-ksud/00-LICENSE"})

    def test_ksuinit_zero_dependencies_valid(self):
        fixture = Fixture(self.root / "init", "ksuinit")
        result = fixture.verify()
        self.assertEqual(result["embedded_assets"], [])
        self.assertEqual(result["components"], [])

    def test_entire_bundle_relocated_and_original_removed(self):
        destination = self.root / "moved"
        shutil.copytree(self.fixture.bundle, destination)
        shutil.rmtree(self.fixture.bundle)
        artifacts = {name: destination / "artifacts" / name for name in self.fixture.artifacts}
        result = evidence.verify_bundle(destination / "build-manifest.json", artifacts,
                                        expected_target="ksud", expected_commit=COMMIT)
        self.assertEqual(result["status"], "consistent")

    def test_original_paths_never_opened_or_executed(self):
        original = Path.open
        calls = []
        def guarded(path, *args, **kwargs):
            calls.append(str(path))
            self.assertIn(str(self.root), str(path))
            return original(path, *args, **kwargs)
        with patch.object(Path, "open", guarded), patch.object(subprocess, "Popen", side_effect=AssertionError("execution forbidden")), \
             patch.object(native_provenance, "capture", side_effect=AssertionError("source access forbidden")), \
             patch.object(native_provenance, "checked_snapshot", side_effect=AssertionError("local semantics forbidden")):
            self.fixture.verify()
        self.assertTrue(calls)

    def test_external_commit_and_target_required(self):
        for kwargs in ({"commit": "2" * 40}, {"target": "ksuinit"}, {"commit": "1" * 39}, {"target": "unknown"}):
            with self.assertRaises(evidence.EvidenceError):
                self.fixture.verify(**kwargs)

    def test_missing_extra_and_wrong_artifact_roles_rejected(self):
        original = dict(self.fixture.artifacts)
        for artifacts in ({"ksud": original["ksud"]}, {**original, "unexpected": original["ksud"]},
                          {"ksuinit": original["ksud"], "ksud.unstripped": original["ksud.unstripped"]}):
            self.fixture.artifacts = artifacts
            with self.assertRaisesRegex(evidence.EvidenceError, "all declared artifact roles"):
                self.fixture.verify()

    def test_mutated_empty_or_missing_artifact_rejected(self):
        path = self.fixture.artifacts["ksud"]
        for data in (b"different bytes", b""):
            path.write_bytes(data)
            with self.assertRaisesRegex(evidence.EvidenceError, "artifact checksum"):
                self.fixture.verify()
        path.unlink()
        with self.assertRaises(OSError):
            self.fixture.verify()

    def test_artifact_aliases_rejected(self):
        self.fixture.artifacts["ksud.unstripped"] = self.fixture.artifacts["ksud"]
        with self.assertRaisesRegex(evidence.EvidenceError, "alias"):
            self.fixture.verify()

    def test_duplicate_json_keys_rejected(self):
        path = self.fixture.bundle / "build-manifest.json"
        path.write_text('{"schema_version":1,"schema_version":1}')
        with self.assertRaisesRegex(evidence.EvidenceError, "duplicate JSON"):
            self.fixture.verify()

    def test_future_unknown_and_bool_schema_rejected(self):
        for key, value in (("schema_version", 2), ("schema_version", True), ("unknown", "field")):
            original = copy.deepcopy(self.fixture.manifest)
            self.fixture.manifest[key] = value
            self.fixture.write_manifest()
            with self.assertRaises(evidence.EvidenceError):
                self.fixture.verify()
            self.fixture.manifest = original

    def test_missing_snapshot_and_changed_snapshot_checksum_rejected(self):
        path = self.fixture.bundle / "source-snapshot.json"
        path.write_bytes(path.read_bytes() + b" ")
        with self.assertRaisesRegex(evidence.EvidenceError, "snapshot evidence checksum"):
            self.fixture.verify()
        path.unlink()
        with self.assertRaises(OSError):
            self.fixture.verify()

    def test_internally_rehashed_wrong_source_digest_rejected(self):
        self.fixture.snapshot["inputs"]["source"]["tree_sha256"] = "b" * 64
        self.fixture.refresh(source_hash=False)
        with self.assertRaisesRegex(evidence.EvidenceError, "source file-table digest"):
            self.fixture.verify()

    def test_internally_rehashed_wrong_dependency_digest_rejected(self):
        self.fixture.snapshot["inputs"]["dependencies"]["components"][0]["tree_sha256"] = "b" * 64
        self.fixture.refresh(source_hash=False)
        with self.assertRaisesRegex(evidence.EvidenceError, "dependency file-table digest"):
            self.fixture.verify()

    def test_dependency_override_comparison_cannot_claim_authentication(self):
        component = self.fixture.snapshot["inputs"]["dependencies"]["components"][0]
        component["local_override"] = True
        self.fixture.refresh()
        with self.assertRaisesRegex(evidence.EvidenceError, "comparison claim"):
            self.fixture.verify()
        component["comparison"] = "local_override_not_verified_against_expected_identity"
        self.fixture.refresh()
        self.assertTrue(self.fixture.verify()["components"][0]["local_override"])

    def test_claimed_git_match_checks_recorded_expected_and_head(self):
        component = self.fixture.snapshot["inputs"]["dependencies"]["components"][0]
        component.update(kind="git", expected={"kind": "git", "revision": "2" * 40},
                         git={"commit": "3" * 40, "dirty": False, "status_sha256": "d" * 64}, comparison="git_commit_matches")
        self.fixture.refresh()
        with self.assertRaisesRegex(evidence.EvidenceError, "recorded Git commit"):
            self.fixture.verify()

    def test_duplicate_component_or_source_table_alias_rejected(self):
        source = self.fixture.snapshot["inputs"]["source"]["files"]
        source["license"] = source["LICENSE"]
        self.fixture.refresh()
        with self.assertRaisesRegex(evidence.EvidenceError, "ambiguous file table"):
            self.fixture.verify()

    def test_duplicate_native_component_name_rejected(self):
        components = self.fixture.snapshot["inputs"]["dependencies"]["components"]
        duplicate = copy.deepcopy(components[0])
        duplicate["source_dir"] = "C:/different-observed-source"
        components.append(duplicate)
        self.fixture.refresh()
        with self.assertRaisesRegex(evidence.EvidenceError, "native component name"):
            self.fixture.verify()

    def test_submodule_gitlink_override_and_head_consistency(self):
        component = self.fixture.snapshot["inputs"]["dependencies"]["components"][0]
        component.update(kind="submodule", expected={"gitlink": "2" * 40, "path": "userspace/ksud/third_party/fixture"},
                         git={"commit": "3" * 40, "dirty": True, "status_sha256": "d" * 64},
                         comparison="expected_identity_recorded_actual_tree_fingerprinted")
        self.fixture.refresh()
        with self.assertRaisesRegex(evidence.EvidenceError, "gitlink/override"):
            self.fixture.verify()
        component["local_override"] = True
        component["comparison"] = "local_override_not_verified_against_expected_identity"
        self.fixture.refresh()
        self.assertTrue(self.fixture.verify()["components"][0]["git"]["dirty"])

    def test_license_source_identity_and_copied_evidence_are_checked(self):
        path = self.fixture.bundle / "licenses/fixture/00-LICENSE"
        path.write_bytes(b"wrong copied evidence")
        with self.assertRaisesRegex(evidence.EvidenceError, "license evidence checksum"):
            self.fixture.verify()
        component = self.fixture.snapshot["inputs"]["dependencies"]["components"][0]
        component["license_evidence"][0]["sha256"] = "b" * 64
        self.fixture.refresh()
        with self.assertRaisesRegex(evidence.EvidenceError, "license source identity"):
            self.fixture.verify()

    def test_license_omission_cannot_hide_required_evidence(self):
        self.fixture.manifest["license_evidence"] = []
        self.fixture.write_manifest()
        with self.assertRaisesRegex(evidence.EvidenceError, "license identities"):
            self.fixture.verify()

    def test_sbom_self_consistent_file_hash_does_not_skip_projection(self):
        self.fixture.sbom["components"][0]["name"] = "wrong-library"
        raw = encoded(self.fixture.sbom)
        (self.fixture.bundle / "native-sbom.cdx.json").write_bytes(raw)
        self.fixture.manifest["sbom"].update(record(raw))
        self.fixture.write_manifest()
        with self.assertRaisesRegex(evidence.EvidenceError, "SBOM does not match"):
            self.fixture.verify()

    def test_sbom_boolean_cannot_alias_integer_projection(self):
        self.fixture.sbom["version"] = True
        raw = encoded(self.fixture.sbom)
        (self.fixture.bundle / "native-sbom.cdx.json").write_bytes(raw)
        self.fixture.manifest["sbom"].update(record(raw))
        self.fixture.write_manifest()
        with self.assertRaisesRegex(evidence.EvidenceError, "SBOM does not match"):
            self.fixture.verify()

    def test_boolean_license_size_cannot_alias_zero_or_one(self):
        component = self.fixture.snapshot["inputs"]["dependencies"]["components"][0]
        for raw, boolean in ((b"", False), (b"x", True)):
            component["files"]["LICENSE"] = record(raw)
            component["license_evidence"][0] = {"path": "LICENSE", **record(raw)}
            self.fixture.license_bytes["licenses/fixture/00-LICENSE"] = raw
            self.fixture.refresh()
            self.fixture.manifest["license_evidence"][0]["size"] = boolean
            self.fixture.write_manifest()
            with self.assertRaisesRegex(evidence.EvidenceError, "license identities"):
                self.fixture.verify()

    def test_staged_embedded_asset_identity_and_complete_set_required(self):
        assets = self.fixture.snapshot["inputs"]["embedded_assets"]
        saved = copy.deepcopy(assets)
        assets[0]["sha256"] = "b" * 64
        self.fixture.refresh()
        with self.assertRaisesRegex(evidence.EvidenceError, "embedded source record"):
            self.fixture.verify()
        self.fixture.snapshot["inputs"]["embedded_assets"] = saved[:-1]
        self.fixture.refresh()
        with self.assertRaisesRegex(evidence.EvidenceError, "asset set is incomplete"):
            self.fixture.verify()

    def test_duplicate_and_wrong_kmi_source_records_rejected(self):
        assets = self.fixture.snapshot["inputs"]["embedded_assets"]
        assets[-1]["kmi"] = "android15-6.6"
        self.fixture.refresh()
        with self.assertRaisesRegex(evidence.EvidenceError, "KMI identity"):
            self.fixture.verify()
        assets[-1]["kmi"] = "android14-6.1"
        assets.append(copy.deepcopy(assets[0]))
        self.fixture.refresh()
        with self.assertRaisesRegex(evidence.EvidenceError, "embedded asset name"):
            self.fixture.verify()

    def test_source_path_traversal_rejected_without_opening_it(self):
        source = self.fixture.snapshot["inputs"]["source"]["files"]
        source["../outside.cpp"] = record(b"never opened")
        self.fixture.refresh()
        with self.assertRaisesRegex(evidence.EvidenceError, "noncanonical relative"):
            self.fixture.verify()

    def test_all_noncanonical_relative_paths_rejected(self):
        for value in ("../file", "a/../b", "a//b", "a\\b", "C:/file", "/file", "a\x00b", "a\nb", "a/", "."):
            with self.assertRaises(evidence.EvidenceError):
                evidence.relative(value)

    def test_json_depth_nodes_numbers_and_bytes_bounded(self):
        with patch.object(evidence, "MAX_DEPTH", 2):
            with self.assertRaisesRegex(evidence.EvidenceError, "structural capacity"):
                self.fixture.verify()
        with patch.object(evidence, "MAX_NODES", 2):
            with self.assertRaises(evidence.EvidenceError):
                self.fixture.verify()
        with patch.object(evidence, "MAX_JSON", 8):
            with self.assertRaises(evidence.EvidenceError):
                self.fixture.verify()
        for value in (float("nan"), 1 << 64, 1.25):
            with self.assertRaises(evidence.EvidenceError):
                evidence.json_bounds(value)

    def test_bool_file_size_and_file_capacity_rejected(self):
        self.fixture.manifest["artifacts"][0]["size"] = True
        self.fixture.write_manifest()
        with self.assertRaisesRegex(evidence.EvidenceError, "file size integer"):
            self.fixture.verify()

    def test_malformed_unhashable_types_use_valueerror_contract(self):
        component = self.fixture.snapshot["inputs"]["dependencies"]["components"][0]
        component["kind"] = []
        self.fixture.refresh()
        with self.assertRaises(evidence.EvidenceError):
            self.fixture.verify()

    def test_nonfinite_json_and_large_string_rejected(self):
        path = self.fixture.bundle / "build-manifest.json"
        path.write_text('{"bad":NaN}')
        with self.assertRaises(evidence.EvidenceError):
            self.fixture.verify()
        with patch.object(evidence, "MAX_STRING", 2):
            with self.assertRaises(evidence.EvidenceError):
                evidence.json_bounds("long")

    def test_input_changed_during_read_rejected(self):
        path = self.fixture.artifacts["ksud"]
        original = Path.stat
        calls = 0
        def changed_stat(candidate, *args, **kwargs):
            nonlocal calls
            if candidate == path and kwargs.get("follow_symlinks", True):
                calls += 1
                # opened_path lstat does not call this override; change the
                # bytes immediately before the final stat after the read.
                if calls == 2:
                    with path.open("ab") as stream:
                        stream.write(b"changed")
            return original(candidate, *args, **kwargs)
        with patch.object(Path, "stat", changed_stat):
            with self.assertRaisesRegex(evidence.EvidenceError, "changed during"):
                evidence.read_file(path)

    def test_hardlink_alias_to_evidence_file_rejected(self):
        source = self.fixture.bundle / "licenses/fixture/00-LICENSE"
        artifact = self.fixture.artifacts["ksud"]
        artifact.unlink()
        os.link(source, artifact)
        with self.assertRaisesRegex(evidence.EvidenceError, "alias"):
            self.fixture.verify()

    def test_source_tree_and_license_aggregate_bounds(self):
        with patch.object(evidence, "MAX_TREE_FILES", 1):
            with self.assertRaises(evidence.EvidenceError):
                self.fixture.verify()
        with patch.object(evidence, "MAX_LICENSE_BYTES", 1):
            with self.assertRaisesRegex(evidence.EvidenceError, "aggregate capacity"):
                self.fixture.verify()

    def test_nonregular_input_rejected(self):
        self.fixture.artifacts["ksud"] = self.root
        with self.assertRaises(evidence.EvidenceError):
            self.fixture.verify()

    def test_evidence_symlink_rejected(self):
        path = self.fixture.bundle / "licenses/fixture/00-LICENSE"
        target = self.root / "license-copy"
        target.write_bytes(path.read_bytes())
        path.unlink()
        try:
            path.symlink_to(target)
        except OSError as error:
            self.skipTest(f"host cannot create symlink: {error}")
        with self.assertRaisesRegex(evidence.EvidenceError, "symlink/reparse"):
            self.fixture.verify()

    def test_cli_stdout_stderr_and_duplicate_mapping(self):
        base = ["verify", "--manifest", str(self.fixture.bundle / "build-manifest.json"), "--target", "ksud", "--commit", COMMIT]
        for name, path in self.fixture.artifacts.items():
            base += ["--artifact", f"{name}={path}"]
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = evidence.main(base)
        self.assertEqual(result, 0)
        self.assertNotIn("snapshot", json.loads(stdout.getvalue()))
        self.assertEqual(stderr.getvalue(), "")
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = evidence.main(base + ["--artifact", f"ksud={self.fixture.artifacts['ksud']}"])
        self.assertEqual(result, 1)
        self.assertEqual(stdout.getvalue(), "")
        self.assertEqual(json.loads(stderr.getvalue())["status"], "error")


if __name__ == "__main__":
    unittest.main(verbosity=2)
