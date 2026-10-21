#!/usr/bin/env python3
"""Independent ZIP, inventory and staged-release byte-binding regressions."""

import copy
import hashlib
import json
import lzma
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import warnings
from zipfile import BadZipFile, ZIP_DEFLATED, ZIP_STORED, ZipFile
import zlib

import release_provenance as release
import native_provenance
import embedded_payloads


ROOT = Path(__file__).resolve().parents[2]
COMMIT = "1234567890abcdef" * 2 + "12345678"
KMI = "android14-6.1"
LIBRARY = "libruntime.so"
PAYLOAD = "lib/arm64-v8a/" + LIBRARY
MARKER = "assets/runtime/payload.txt"


def record(data):
    return {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False, allow_nan=False).encode()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False).encode() + b"\n")


def write_apk(path, native, *, marker=LIBRARY.encode() + b"\n", entries=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    with warnings.catch_warnings(), ZipFile(path, "w", compression=ZIP_DEFLATED) as archive:
        warnings.simplefilter("ignore", UserWarning)
        archive.writestr("classes.dex", b"synthetic Manager byte fixture, not an APK runtime test")
        if marker is not None:
            archive.writestr(MARKER, marker)
        if native is not None:
            archive.writestr(PAYLOAD, native)
        for name, content in entries or []:
            archive.writestr(name, content)


def fixture_native_bundle(bundle, target, binary, symbols, assets):
    """Hand-authored schema 1 fixture; production verification/builders not called."""
    license_bytes = b"Synthetic license evidence; no upstream licensing assertion.\n"
    git = {"commit": COMMIT, "dirty": True, "status_sha256": hashlib.sha256(b"dirty fixture").hexdigest()}
    files = {"LICENSE": record(license_bytes), f"userspace/{target}/main.cpp": record(b"fixture source")}
    for asset_name, data in assets.items():
        files["userspace/ksud/assets/" + asset_name] = record(data)
    source = {
        "git": git, "roots": [f"userspace/{target}"], "files": files,
        "tree_sha256": hashlib.sha256(canonical(files)).hexdigest(),
        "license_evidence": [{"path": "LICENSE", **record(license_bytes)}],
        "exclusions": {"directories": ["out"], "files": ["*.key"]},
    }
    dep_files = {"small.c": record(b"synthetic library")}
    dependencies = {
        "metadata_file": "/unavailable/configured/dependencies.json" if target == "ksud" else None,
        "metadata_sha256": hashlib.sha256(b"configured fixture").hexdigest() if target == "ksud" else None,
        "inactive_declarations": [], "components": [],
    }
    if target == "ksud":
        dependencies["components"].append({
            "name": "fixture-dependency", "source_dir": "/unavailable/dependency", "kind": "git",
            "expected": {"revision": COMMIT, "repository": "https://example.invalid/fixture.git"},
            "resolution": "git", "local_override": False, "comparison": "git_commit_matches", "git": git,
            "files": dep_files, "tree_sha256": hashlib.sha256(canonical(dep_files)).hexdigest(),
            "license_evidence": [], "license_evidence_status": "not_found",
        })
    tool = {"invocation": "/unavailable/tool", "resolved_path": "/unavailable/tool", **record(b"inert program"),
            "symlink_chain": [], "version": "synthetic observation; never execute"}
    embedded = []
    for asset_name, data in sorted(assets.items()):
        kind = "kernel_module" if asset_name.endswith("_kernelsu.ko") else "ksuinit" if asset_name == "ksuinit" else "runtime_asset"
        entry = {"name": asset_name, "path": "/unavailable/assets/" + asset_name, **record(data), "kind": kind}
        if kind == "kernel_module":
            entry["kmi"] = asset_name.removesuffix("_kernelsu.ko")
        embedded.append(entry)
    status = {"dirty": True, "status_sha256": hashlib.sha256(b"status").hexdigest()}
    inputs = {
        "source": source, "dependencies": dependencies,
        "toolchain": {"settings": {"CMAKE_BUILD_TYPE": "Release"},
                      "tools": {"CMAKE_CXX_COMPILER": tool, "provenance_python": tool},
                      "files": {"/unavailable/build/build.ninja": record(b"fixture graph")}},
        "embedded_assets": embedded,
        "version_inputs": {"environment": {"ZYSU_KSUD_VERSION_NAME": "", "ZYSU_KSUD_VERSION_CODE": ""},
                           "describe": "fixture-v1", "commit_count": "1", "kernel_status": status,
                           "ksud_status": status, "additional_kernel_version_inputs": {}} if target == "ksud" else None,
    }
    artifacts = [{"name": name, "path": "/unavailable/build/" + name, **record(data)}
                 for name, data in ((target, binary), (target + ".unstripped", symbols))]
    snapshot = {
        "schema_version": 1, "kind": "native_source_snapshot", "target": target,
        "repo": "/unavailable/source", "build_dir": "/unavailable/build", "output_dir": "/unavailable/evidence",
        "created_ns": 1, "artifacts": [{"name": item["name"], "path": item["path"]} for item in artifacts],
        "inputs": inputs, "inputs_sha256": hashlib.sha256(canonical(inputs)).hexdigest(),
        "limitations": native_provenance.LIMITATIONS,
    }
    license_record = {"component": "ZySU-" + target, "source_relative_path": "LICENSE",
                      "evidence_path": f"licenses/ZySU-{target}/00-LICENSE", **record(license_bytes)}
    evidence_license = bundle / license_record["evidence_path"]
    evidence_license.parent.mkdir(parents=True, exist_ok=True)
    evidence_license.write_bytes(license_bytes)
    app = {"type": "application", "bom-ref": "zysu:" + target, "name": "ZySU " + target, "version": COMMIT,
           "properties": [{"name": "zysu:source-tree-sha256", "value": source["tree_sha256"]},
                          {"name": "zysu:checkout-dirty", "value": "true"},
                          {"name": "zysu:license-evidence", "value": canonical([license_record]).decode()}]}
    components = []
    if target == "ksud":
        dep = dependencies["components"][0]
        components.append({"type": "library", "bom-ref": "native:" + dep["name"], "name": dep["name"], "version": COMMIT,
                           "properties": [{"name": "zysu:expected-identity", "value": canonical(dep["expected"]).decode()},
                                          {"name": "zysu:comparison", "value": "git_commit_matches"},
                                          {"name": "zysu:local-override", "value": "false"},
                                          {"name": "zysu:source-tree-sha256", "value": dep["tree_sha256"]},
                                          {"name": "zysu:license-evidence", "value": "[]"}]})
    components.extend({"type": "file", "bom-ref": "artifact:" + item["name"], "name": item["name"],
                       "hashes": [{"alg": "SHA-256", "content": item["sha256"]}]} for item in artifacts)
    sbom = {"bomFormat": "CycloneDX", "specVersion": "1.5", "version": 1,
            "metadata": {"component": app, "properties": [{"name": "zysu:coverage", "value": "partial-native-source-components"},
                                                         {"name": "zysu:limitations", "value": canonical(native_provenance.LIMITATIONS).decode()}]},
            "components": components}
    write_json(bundle / "source-snapshot.json", snapshot)
    write_json(bundle / "native-sbom.cdx.json", sbom)
    manifest = {"schema_version": 1, "kind": "native_build_manifest", "target": target, "created_ns": 2,
                "snapshot": {"path": "source-snapshot.json", **record((bundle / "source-snapshot.json").read_bytes())},
                "inputs_sha256": snapshot["inputs_sha256"], "source": git, "source_after": git,
                "artifacts": artifacts, "license_evidence": [license_record],
                "sbom": {"path": "native-sbom.cdx.json", **record((bundle / "native-sbom.cdx.json").read_bytes())},
                "limitations": native_provenance.LIMITATIONS}
    write_json(bundle / "build-manifest.json", manifest)


def chain_fixture(root, kmis=(KMI,)):
    """Construct independent minimal ARM64 ELF + proof + original-format native evidence."""
    artifacts = root / "artifacts"
    ksuinit = b"synthetic standalone ksuinit\x00"
    raw = {"installer.sh": b"#!/bin/sh\r\necho installer\r\n", "shellrc.sh": b"echo shell\n", "ksuinit": ksuinit}
    raw.update({kmi + "_kernelsu.ko": b"synthetic KMI byte fixture, not a built module: " + kmi.encode() for kmi in kmis})
    groups, assets, blobs = [], [], []
    kmis_sorted = sorted(kmis)
    kmi_bytes = b"".join(raw[kmi + "_kernelsu.ko"] for kmi in kmis_sorted)
    packed = lzma.compress(kmi_bytes, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32, preset=6)
    group_inputs = [("xz-crc32", packed, kmi_bytes)]
    for name in ("installer.sh", "ksuinit", "shellrc.sh"):
        data = raw[name].replace(b"\r\n", b"\n").replace(b"\r", b"\n") if name.endswith(".sh") else raw[name]
        group_inputs.append(("zlib", zlib.compress(data, level=9), data))
    offset = 256
    for index, (codec, compressed, decoded) in enumerate(group_inputs):
        groups.append({"id": f"g{index:02d}", "codec": codec, "path": f"payloads/{index:02d}." + ("xz" if codec.startswith("xz") else "zlib"),
                       "compressed": record(compressed), "decoded": record(decoded), "elf_offset": offset, "load_index": 0})
        offset += len(compressed)
        blobs.append(compressed)
    binary = bytearray(offset)
    binary[:16] = b"\x7fELF\x02\x01\x01" + b"\0" * 9
    struct.pack_into("<HHIQQQIHHHHHH", binary, 16, 3, 183, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    struct.pack_into("<IIQQQQQQ", binary, 64, 1, 4, 0, 0, 0, len(binary), len(binary), 1)
    binary[256:] = b"".join(blobs)
    member_offset = 0
    ordinary = {"installer.sh": 1, "ksuinit": 2, "shellrc.sh": 3}
    for name, data in sorted(raw.items()):
        is_kmi = name.endswith("_kernelsu.ko")
        index = 0 if is_kmi else ordinary[name]
        normalized = data.replace(b"\r\n", b"\n").replace(b"\r", b"\n") if name.endswith(".sh") else data
        assets.append({"name": name, "kind": "kernel_module" if is_kmi else "ksuinit" if name == "ksuinit" else "runtime_asset",
                       "source": record(data), "normalized": record(normalized), "normalization": "shell_newlines_lf" if name.endswith(".sh") else "identity",
                       "group": f"g{index:02d}", "offset": member_offset if is_kmi else 0})
        if is_kmi:
            member_offset += len(data)
    paths = {}
    for target, data in (("ksud", bytes(binary)), ("ksuinit", ksuinit)):
        path = artifacts / f"{target}-aarch64-linux-android" / target
        path.parent.mkdir(parents=True)
        path.write_bytes(data)
        symbols = artifacts / f"{target}-symbols-aarch64-linux-android" / (target + ".unstripped")
        symbols.parent.mkdir(parents=True)
        symbol_bytes = b"independent synthetic symbols for " + target.encode()
        symbols.write_bytes(symbol_bytes)
        bundle = artifacts / f"{target}-provenance-aarch64-linux-android"
        fixture_native_bundle(bundle, target, data, symbol_bytes, raw if target == "ksud" else {})
        paths[target] = path
        paths[target + "_bundle"] = bundle
    for kmi in kmis:
        path = artifacts / (kmi + "-lkm") / (kmi + "_kernelsu.ko")
        path.parent.mkdir(parents=True)
        path.write_bytes(raw[path.name])
    bundle = paths["ksud_bundle"]
    proof = {"schema_version": 1, "kind": "embedded_payload_proof", "target": "ksud", "scope": embedded_payloads.SCOPE,
             "native_manifest": record((bundle / "build-manifest.json").read_bytes()),
             "native_snapshot": record((bundle / "source-snapshot.json").read_bytes()), "binary": record(bytes(binary)),
             "generated_cpp": record(b"independent synthetic registry observation"),
             "assets": assets, "groups": groups, "limitations": embedded_payloads.LIMITATIONS}
    for group, blob in zip(groups, blobs):
        path = bundle / "embedded" / group["path"]
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(blob)
    write_json(bundle / "embedded/manifest.json", proof)
    paths["apk"] = artifacts / "Manager-arm64-v8a/final.apk"
    write_apk(paths["apk"], bytes(binary))
    return artifacts, paths


class ApkByteBindingTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="release-apk-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "Unicode 路径 with spaces"
        self.root.mkdir()
        self.binary = self.root / "ksud"
        self.binary.write_bytes(b"independent canonical native bytes\0" * 20)
        self.apk = self.root / "final.apk"
        write_apk(self.apk, self.binary.read_bytes())

    def inspect(self):
        return release.inspect_apk(self.apk, self.binary, LIBRARY)

    def test_final_zip_payload_matches_canonical_file(self):
        result = self.inspect()
        encoded = json.dumps(result, sort_keys=True)
        self.assertIn(hashlib.sha256(self.binary.read_bytes()).hexdigest(), encoded)
        self.assertIn(PAYLOAD, encoded)

    def test_wrong_missing_or_empty_payload_is_rejected(self):
        for native in (b"different bytes", None, b""):
            with self.subTest(native=native):
                write_apk(self.apk, native)
                with self.assertRaises((ValueError, OSError)):
                    self.inspect()

    def test_duplicate_critical_entries_are_rejected_even_when_equal(self):
        for name, data in ((MARKER, LIBRARY.encode() + b"\n"), (PAYLOAD, self.binary.read_bytes())):
            with self.subTest(name=name):
                write_apk(self.apk, self.binary.read_bytes(), entries=[(name, data)])
                with self.assertRaises((ValueError, OSError)):
                    self.inspect()

    def test_missing_wrong_or_invalid_utf8_marker_is_rejected(self):
        for marker in (None, b"libksud.so", b"libother.so", b"\xff", b""):
            with self.subTest(marker=marker):
                write_apk(self.apk, self.binary.read_bytes(), marker=marker)
                with self.assertRaises((ValueError, OSError)):
                    self.inspect()

    def test_foreign_abi_and_legacy_payload_entries_are_rejected(self):
        for name in ("lib/x86_64/libruntime.so", "lib/arm64-v8a/libksud.so", "lib/x86_64/libksud.so"):
            with self.subTest(name=name):
                write_apk(self.apk, self.binary.read_bytes(), entries=[(name, b"unexpected payload")])
                with self.assertRaises((ValueError, OSError)):
                    self.inspect()

    def test_invalid_library_identity_cannot_choose_arbitrary_zip_entry(self):
        for name in ("", "../libruntime.so", "lib/path.so", "libbad-name.so", "libruntime.so\n", "classes.dex"):
            with self.subTest(name=name), self.assertRaises((ValueError, OSError)):
                release.inspect_apk(self.apk, self.binary, name)

    def test_crc_corruption_is_a_structured_validation_failure(self):
        with ZipFile(self.apk, "w", compression=ZIP_STORED) as archive:
            archive.writestr(MARKER, LIBRARY.encode())
            archive.writestr(PAYLOAD, self.binary.read_bytes())
        with ZipFile(self.apk) as archive:
            entry = archive.getinfo(PAYLOAD)
            offset = entry.header_offset
        content = bytearray(self.apk.read_bytes())
        filename_size, extra_size = struct.unpack_from("<HH", content, offset + 26)
        content[offset + 30 + filename_size + extra_size] ^= 1
        self.apk.write_bytes(content)
        with self.assertRaises((ValueError, OSError, BadZipFile)):
            self.inspect()

    def test_truncated_or_nonzip_input_is_rejected(self):
        data = self.apk.read_bytes()
        for invalid in (b"not a ZIP", data[:-24]):
            with self.subTest(size=len(invalid)):
                self.apk.write_bytes(invalid)
                with self.assertRaises((ValueError, OSError, BadZipFile)):
                    self.inspect()

    def test_encrypted_critical_zip_entry_is_rejected_without_password(self):
        content = bytearray(self.apk.read_bytes())
        with ZipFile(self.apk) as archive:
            offset = archive.getinfo(PAYLOAD).header_offset
        flags = struct.unpack_from("<H", content, offset + 6)[0]
        struct.pack_into("<H", content, offset + 6, flags | 1)
        cursor = 0
        while True:
            cursor = content.find(b"PK\x01\x02", cursor)
            if cursor < 0:
                break
            size = struct.unpack_from("<H", content, cursor + 28)[0]
            name = content[cursor + 46:cursor + 46 + size].decode()
            if name == PAYLOAD:
                flags = struct.unpack_from("<H", content, cursor + 8)[0]
                struct.pack_into("<H", content, cursor + 8, flags | 1)
                break
            cursor += 4
        self.apk.write_bytes(content)
        with self.assertRaises((ValueError, OSError)):
            self.inspect()

    def test_canonical_change_after_repack_is_rejected(self):
        self.binary.write_bytes(self.binary.read_bytes() + b"later change")
        with self.assertRaises((ValueError, OSError)):
            self.inspect()

    def test_noncanonical_dot_zip_member_rejected(self):
        write_apk(self.apk, self.binary.read_bytes(), entries=[(".", b"not a canonical file name")])
        with self.assertRaises(ValueError):
            self.inspect()


class BoundedReleaseJsonTests(unittest.TestCase):
    def test_change_between_size_check_and_json_parse_cannot_bypass_budget(self):
        with tempfile.TemporaryDirectory(prefix="release-json-") as temporary:
            path = Path(temporary) / "manifest.json"
            path.write_bytes(b"{}")
            original_open = Path.open
            reads = 0
            changed = False

            def replace_on_second_read(source, mode="r", *args, **kwargs):
                nonlocal reads, changed
                if source == path and "r" in mode:
                    reads += 1
                    if reads == 2:
                        with original_open(path, "wb") as stream:
                            stream.write(json.dumps({"claim": "x" * 512}).encode())
                        changed = True
                return original_open(source, mode, *args, **kwargs)

            rejected = False
            with patch.object(release, "MAX_JSON", 64), patch.object(Path, "open", replace_on_second_read):
                try:
                    result = release.load_json(path)
                except ValueError:
                    rejected = True
            if changed:
                self.assertTrue(rejected, "replacement JSON bypassed the checked byte budget")
            else:
                self.assertFalse(rejected)
                self.assertEqual(result, {})  # A single bounded read needs no second open.

    def test_static_json_above_byte_budget_rejected(self):
        with tempfile.TemporaryDirectory(prefix="release-json-") as temporary:
            path = Path(temporary) / "manifest.json"
            path.write_bytes(json.dumps({"claim": "x" * 512}).encode())
            with patch.object(release, "MAX_JSON", 64), self.assertRaises(ValueError):
                release.load_json(path)


class ReleaseChainTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="release-chain-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "Unicode 路径 with spaces"
        self.root.mkdir()
        self.artifacts, self.paths = chain_fixture(self.root)
        self.output = self.root / "release"

    def collect(self, **overrides):
        options = {"expected_commit": COMMIT, "expected_kmis": [KMI], "library_name": LIBRARY}
        options.update(overrides)
        return release.collect_release(self.artifacts, **options)

    def stage(self, collection=None):
        metadata = {"schema_version": 1, "version_name": "v4.2.1", "version_code": 4242,
                    "ksud": {"version_name": "4.2.0", "version_code": 4240},
                    "kernel": {"version_name": "4.1.9", "version_code": 4239}}
        return release.stage_release(self.output, collection or self.collect(), "ZySU_v4.2.1_4242-release.apk", metadata)

    def load_manifest(self):
        return json.loads((self.output / "release-manifest.json").read_text(encoding="utf-8"))

    def refresh_outer_records(self, manifest):
        """Rehash untrusted outer records to exercise independent inner evidence checks."""
        metadata = json.loads((self.output / "update.json").read_text(encoding="utf-8"))
        metadata["provenance"].update(record((self.output / "native-evidence.zip").read_bytes()))
        write_json(self.output / "update.json", metadata)
        for name in manifest["artifacts"]:
            manifest["artifacts"][name] = record((self.output / name).read_bytes())
        write_json(self.output / "release-manifest.json", manifest)

    def rewrite_archive(self, mutate):
        path = self.output / "native-evidence.zip"
        with ZipFile(path) as archive:
            entries = [(info.filename, archive.read(info)) for info in archive.infolist()]
        entries = mutate(entries)
        with warnings.catch_warnings(), ZipFile(path, "w", compression=ZIP_DEFLATED) as archive:
            warnings.simplefilter("ignore", UserWarning)
            for name, data in entries:
                archive.writestr(name, data)

    def assert_no_stage(self):
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.root.glob(".zysu-release-*")), [])

    def test_complete_single_kmi_chain_and_public_inventory(self):
        collection = self.collect()
        self.assertNotEqual(collection["native"]["ksud"]["inputs_sha256"], collection["native"]["ksuinit"]["inputs_sha256"])
        metadata = self.stage(collection)
        self.assertEqual(metadata["schema_version"], 1)
        self.assertEqual(metadata["apk"]["name"], "ZySU_v4.2.1_4242-release.apk")
        self.assertEqual(metadata["ksud"], {"version_name": "4.2.0", "version_code": 4240})
        expected = {"ZySU_v4.2.1_4242-release.apk", "ksud-aarch64-linux-android", "ksuinit-aarch64-linux-android",
                    KMI + "_kernelsu.ko", "update.json", "release-manifest.json", "native-evidence.zip"}
        self.assertEqual({path.name for path in self.output.iterdir()}, expected)
        with ZipFile(self.output / "native-evidence.zip") as archive:
            self.assertIn("native/ksud/artifacts/ksud.unstripped", archive.namelist())
            self.assertIn("native/ksuinit/artifacts/ksuinit.unstripped", archive.namelist())
            self.assertIn("native/ksud/embedded/manifest.json", archive.namelist())
            self.assertEqual(archive.namelist(), sorted(archive.namelist()))
        verified = release.verify_release(self.output)
        self.assertEqual(verified["status"], "consistent")
        self.assertEqual(verified["source_commit"], COMMIT)
        relations = self.load_manifest()["relationships"]
        self.assertEqual(relations["manager_source_build_evidence"], "not_available")
        self.assertEqual(relations["ddk_source_build_evidence"], "not_available")

    def test_all_selected_kmis_are_bound_exactly(self):
        root = self.root / "all selected"
        artifacts, _ = chain_fixture(root, tuple(release.SUPPORTED_KMIS))
        collected = release.collect_release(artifacts, expected_commit=COMMIT,
                                           expected_kmis=list(reversed(release.SUPPORTED_KMIS)), library_name=LIBRARY)
        self.assertEqual(collected["expected_kmis"], sorted(release.SUPPORTED_KMIS))
        self.stage(collected)
        self.assertEqual(release.verify_release(self.output)["selected_kmis"], sorted(release.SUPPORTED_KMIS))

    def test_relocated_release_verifies_without_original_paths_or_processes(self):
        self.stage()
        relocated = self.root / "relocated portable release"
        self.output.rename(relocated)
        # This is a TemporaryDirectory-owned synthetic tree, never the checkout.
        self.assertEqual(self.artifacts.resolve().parent, self.root.resolve())
        shutil.rmtree(self.artifacts)
        with patch.object(subprocess, "Popen", side_effect=AssertionError("portable verifier must not execute tools")), \
             patch.object(subprocess, "run", side_effect=AssertionError("portable verifier must not execute tools")):
            self.assertEqual(release.verify_release(relocated)["status"], "consistent")

    def test_missing_required_artifacts_or_evidence_reject_before_output(self):
        required = [self.paths["apk"], self.paths["ksud"], self.paths["ksuinit"],
                    self.artifacts / (KMI + "-lkm") / (KMI + "_kernelsu.ko")]
        for target in ("ksud", "ksuinit"):
            required.append(self.artifacts / f"{target}-symbols-aarch64-linux-android" / (target + ".unstripped"))
            required.extend(self.paths[target + "_bundle"] / name
                            for name in ("build-manifest.json", "source-snapshot.json", "native-sbom.cdx.json",
                                         f"licenses/ZySU-{target}/00-LICENSE"))
        required.extend(self.paths["ksud_bundle"] / "embedded" / name
                        for name in ("manifest.json", "payloads/00.xz"))
        for path in required:
            with self.subTest(path=path.relative_to(self.artifacts)):
                contents = path.read_bytes()
                path.unlink()
                try:
                    with self.assertRaises((ValueError, OSError)):
                        self.collect()
                    self.assert_no_stage()
                finally:
                    path.write_bytes(contents)

    def test_extra_apk_and_ambiguous_signatures_rejected(self):
        extra = self.paths["apk"].with_name("second.apk")
        extra.write_bytes(self.paths["apk"].read_bytes())
        with self.assertRaises(ValueError):
            self.collect()
        extra.unlink()
        self.paths["apk"].with_suffix(".sig").write_bytes(b"signature A")
        Path(str(self.paths["apk"]) + ".sig").write_bytes(b"signature B")
        with self.assertRaises(ValueError):
            self.collect()

    def test_unexpected_native_or_kmi_directories_rejected(self):
        for directory, filename in (("ksud-foreign-target", "ksud"), ("extra-lkm", "module.ko")):
            with self.subTest(directory=directory):
                extra = self.artifacts / directory
                extra.mkdir()
                (extra / filename).write_bytes(b"unexpected")
                try:
                    with self.assertRaises(ValueError):
                        self.collect()
                finally:
                    (extra / filename).unlink()
                    extra.rmdir()

    def test_unexpected_file_inside_artifact_directory_rejected(self):
        path = self.paths["ksud"].parent / "ksud.old"
        path.write_bytes(self.paths["ksud"].read_bytes())
        with self.assertRaises(ValueError):
            self.collect()

    def test_invalid_external_commit_kmi_or_library_context_rejected(self):
        options = [{"expected_commit": "a" * 40}, {"expected_commit": "short"}, {"expected_kmis": []},
                   {"expected_kmis": [KMI, KMI]}, {"expected_kmis": ["unknown"]}, {"library_name": "libksud.so"}]
        for option in options:
            with self.subTest(option=option), self.assertRaises(ValueError):
                self.collect(**option)

    def test_wrong_native_target_rejected(self):
        path = self.paths["ksud_bundle"] / "build-manifest.json"
        value = json.loads(path.read_text(encoding="utf-8"))
        value["target"] = "ksuinit"
        write_json(path, value)
        with self.assertRaises(ValueError):
            self.collect()

    def test_downloadable_ksuinit_kmi_or_canonical_daemon_mutation_rejected(self):
        paths = [self.paths["ksuinit"], self.paths["ksud"], self.artifacts / (KMI + "-lkm") / (KMI + "_kernelsu.ko")]
        for path in paths:
            original = path.read_bytes()
            with self.subTest(name=path.name):
                path.write_bytes(original + b"mutation")
                try:
                    with self.assertRaises(ValueError):
                        self.collect()
                finally:
                    path.write_bytes(original)

    def test_bad_embedded_offset_rejected_even_with_valid_blob_hash(self):
        path = self.paths["ksud_bundle"] / "embedded/manifest.json"
        proof = json.loads(path.read_text(encoding="utf-8"))
        proof["groups"][0]["elf_offset"] += 1
        write_json(path, proof)
        with self.assertRaises(ValueError):
            self.collect()

    def test_corrupted_source_sbom_or_license_rejected(self):
        bundle = self.paths["ksud_bundle"]
        for name in ("source-snapshot.json", "native-sbom.cdx.json", "licenses/ZySU-ksud/00-LICENSE"):
            path = bundle / name
            original = path.read_bytes()
            with self.subTest(name=name):
                path.write_bytes(original + b"corruption")
                try:
                    with self.assertRaises(ValueError):
                        self.collect()
                finally:
                    path.write_bytes(original)

    def test_future_native_schema_and_duplicate_json_rejected(self):
        path = self.paths["ksud_bundle"] / "build-manifest.json"
        original = path.read_bytes()
        value = json.loads(original)
        value["schema_version"] = 999
        for invalid in (json.dumps(value).encode(), original.replace(b'"schema_version": 1', b'"schema_version": 1, "schema_version": 1')):
            with self.subTest(invalid=invalid[:80]):
                path.write_bytes(invalid)
                with self.assertRaises(ValueError):
                    self.collect()
        path.write_bytes(original)

    def test_existing_output_preserved_without_stage_creation(self):
        self.output.mkdir()
        sentinel = self.output / "keep.bin"
        sentinel.write_bytes(b"prior release")
        with self.assertRaises(ValueError):
            self.stage()
        self.assertEqual(sentinel.read_bytes(), b"prior release")
        self.assertEqual({p.name for p in self.output.iterdir()}, {"keep.bin"})
        self.assertEqual(list(self.root.glob(".zysu-release-*")), [])

    def test_output_inside_input_artifacts_rejected(self):
        self.output = self.artifacts / "not-allowed"
        with self.assertRaises(ValueError):
            self.stage()
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.artifacts.glob(".zysu-release-*")), [])

    def test_copy_failure_cleans_only_created_stage(self):
        sibling = self.root / ".zysu-release-unrelated"
        sibling.mkdir()
        sentinel = sibling / "keep.txt"
        sentinel.write_bytes(b"unrelated sibling")
        original = release.copy_checked
        calls = 0

        def fail_second(source, destination, expected):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError("synthetic interrupted copy")
            original(source, destination, expected)

        with patch.object(release, "copy_checked", side_effect=fail_second), self.assertRaisesRegex(OSError, "interrupted copy"):
            self.stage()
        self.assertEqual(calls, 2)
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.root.glob(".zysu-release-*")), [sibling])
        self.assertEqual(sentinel.read_bytes(), b"unrelated sibling")

    def test_rename_failure_leaves_no_partial_release(self):
        with patch.object(release.os, "rename", side_effect=OSError("synthetic rename failure")), \
             self.assertRaisesRegex(OSError, "rename failure"):
            self.stage()
        self.assert_no_stage()

    def test_input_change_after_collection_rejects_staging(self):
        collection = self.collect()
        self.paths["ksud"].write_bytes(self.paths["ksud"].read_bytes() + b"late change")
        with self.assertRaisesRegex(ValueError, "changed after validation"):
            self.stage(collection)
        self.assert_no_stage()

    def test_evidence_change_after_collection_rejects_staging(self):
        collection = self.collect()
        path = self.paths["ksud_bundle"] / "licenses/ZySU-ksud/00-LICENSE"
        path.write_bytes(path.read_bytes() + b"late evidence change")
        with self.assertRaises(ValueError):
            self.stage(collection)
        self.assert_no_stage()

    def test_strict_package_helper_keeps_legacy_name_and_adds_evidence(self):
        from prepare_release import package_release
        metadata = package_release(self.artifacts, self.output, "v4.2.1", "4242", ksud_name="v4.2.0", ksud_code="4240",
                                   kernel_name="4.1.9", kernel_code="4239", expected_commit=COMMIT,
                                   expected_kmis=[KMI], library_name=LIBRARY)
        self.assertEqual(metadata["version_name"], "v4.2.1")
        self.assertEqual(metadata["apk"]["name"], "ZySU_v4.2.1_4242-release.apk")
        self.assertIn("provenance", metadata)
        self.assertEqual(release.verify_release(self.output)["status"], "consistent")

    def test_partial_strict_context_never_falls_back_to_legacy_packaging(self):
        from prepare_release import package_release
        for options in ({"expected_commit": COMMIT}, {"expected_kmis": [KMI]}, {"library_name": LIBRARY}):
            with self.subTest(options=options), self.assertRaises(ValueError):
                package_release(self.artifacts, self.output, "4.2.1", 4242, ksud_name="4.2.0", ksud_code=4240,
                                kernel_name="4.1.9", kernel_code=4239, **options)
            self.assert_no_stage()

    def test_signatures_are_copied_as_bytes_without_authentication_claim(self):
        self.paths["apk"].with_suffix(".sig").write_bytes(b"synthetic APK signature bytes")
        self.paths["ksud"].with_suffix(".sig").write_bytes(b"synthetic native signature bytes")
        self.stage()
        self.assertEqual((self.output / "ZySU_v4.2.1_4242-release.apk.sig").read_bytes(), b"synthetic APK signature bytes")
        self.assertEqual((self.output / "ksud-aarch64-linux-android.sig").read_bytes(), b"synthetic native signature bytes")
        self.assertEqual(release.verify_release(self.output)["scope"], "unsigned_byte_evidence_consistency")

    def test_release_extra_file_and_artifact_mutation_rejected(self):
        self.stage()
        extra = self.output / "unexpected.txt"
        extra.write_bytes(b"extra")
        with self.assertRaises(ValueError):
            release.verify_release(self.output)
        extra.unlink()
        path = self.output / "ksuinit-aarch64-linux-android"
        path.write_bytes(path.read_bytes() + b"tamper")
        with self.assertRaises(ValueError):
            release.verify_release(self.output)

    def test_release_future_schema_duplicate_json_and_nonfinite_values_rejected(self):
        self.stage()
        path = self.output / "release-manifest.json"
        original = path.read_bytes()
        value = json.loads(original)
        value["schema_version"] = 2
        invalids = [json.dumps(value).encode(), original.replace(b'"schema_version": 1', b'"schema_version": 1, "schema_version": 1'),
                    original.replace(b'"schema_version": 1', b'"schema_version": NaN')]
        for invalid in invalids:
            with self.subTest(invalid=invalid[:80]):
                path.write_bytes(invalid)
                with self.assertRaises(ValueError):
                    release.verify_release(self.output)

    def test_false_release_relations_rejected_with_outer_hashes_intact(self):
        self.stage()
        original = self.load_manifest()
        for field in ("relationships", "native", "bool-alias"):
            manifest = copy.deepcopy(original)
            if field == "relationships":
                manifest[field]["manager_source_build_evidence"] = "verified"
            elif field == "bool-alias":
                manifest["native"]["ksud"]["source"]["dirty"] = 1
            else:
                manifest[field]["ksud"]["inputs_sha256"] = "0" * 64
            with self.subTest(field=field):
                write_json(self.output / "release-manifest.json", manifest)
                with self.assertRaises(ValueError):
                    release.verify_release(self.output)

    def test_archive_extra_duplicate_and_traversal_entries_rejected_after_rehash(self):
        self.stage()
        zip_path = self.output / "native-evidence.zip"
        original_zip = zip_path.read_bytes()
        original_manifest = self.load_manifest()
        for name in ("native/ksud/extra.txt", "native/ksud/build-manifest.json", "../escape.txt", "native/ksud/CON"):
            with self.subTest(name=name):
                zip_path.write_bytes(original_zip)
                self.rewrite_archive(lambda entries: entries + [(name, b"extra bytes")])
                self.refresh_outer_records(copy.deepcopy(original_manifest))
                with self.assertRaises((ValueError, OSError, BadZipFile)):
                    release.verify_release(self.output)
                self.assertFalse((self.root / "escape.txt").exists())

    def test_archive_symbol_tamper_rejected_even_with_member_and_outer_rehash(self):
        self.stage()
        name = "native/ksud/artifacts/ksud.unstripped"
        altered = b"different symbol bytes"
        self.rewrite_archive(lambda entries: [(key, altered if key == name else data) for key, data in entries])
        manifest = self.load_manifest()
        manifest["evidence_files"][name] = record(altered)
        self.refresh_outer_records(manifest)
        with self.assertRaises(ValueError):
            release.verify_release(self.output)

    def test_archive_unregistered_evidence_rejected_even_with_full_inventory(self):
        self.stage()
        name, altered = "native/ksud/extra-evidence.txt", b"unregistered evidence"
        self.rewrite_archive(lambda entries: entries + [(name, altered)])
        manifest = self.load_manifest()
        manifest["evidence_files"][name] = record(altered)
        self.refresh_outer_records(manifest)
        with self.assertRaises(ValueError):
            release.verify_release(self.output)

    def test_update_metadata_version_and_name_validation_after_outer_rehash(self):
        self.stage()
        path = self.output / "update.json"
        original = json.loads(path.read_text(encoding="utf-8"))
        manifest = self.load_manifest()
        for field, value in (("version_code", True), ("version_name", "v9.9.9"), ("ksud", {"version_name": "4.0", "version_code": -1})):
            metadata = copy.deepcopy(original)
            metadata[field] = value
            with self.subTest(field=field):
                write_json(path, metadata)
                self.refresh_outer_records(copy.deepcopy(manifest))
                with self.assertRaises(ValueError):
                    release.verify_release(self.output)

    def test_publication_and_archive_read_budgets_fail_closed(self):
        for constant, limit in (("MAX_EVIDENCE_ENTRIES", 1), ("MAX_EVIDENCE_BYTES", 1)):
            with self.subTest(constant=constant), patch.object(release, constant, limit), self.assertRaises(ValueError):
                self.collect()
        with patch.object(release, "MAX_APK_ENTRIES", 1), self.assertRaises(ValueError):
            release.inspect_apk(self.paths["apk"], self.paths["ksud"], LIBRARY)
        with patch.object(release, "MAX_APK_EXPANDED", 1), self.assertRaises(ValueError):
            release.inspect_apk(self.paths["apk"], self.paths["ksud"], LIBRARY)
        self.stage()
        with patch.object(release, "MAX_JSON", 10), self.assertRaises(ValueError):
            release.verify_release(self.output)

    def test_verify_cli_success_and_structured_failure(self):
        self.stage()
        command = [sys.executable, "-B", str(ROOT / "scripts/ci/release_provenance.py"), "verify", "--release-dir", str(self.output)]
        success = subprocess.run(command, capture_output=True, encoding="utf-8", timeout=30)
        self.assertEqual(success.returncode, 0, success.stderr)
        self.assertEqual(success.stderr, "")
        self.assertEqual(json.loads(success.stdout)["status"], "consistent")
        (self.output / "ksuinit-aarch64-linux-android").write_bytes(b"changed")
        failure = subprocess.run(command, capture_output=True, encoding="utf-8", timeout=30)
        self.assertNotEqual(failure.returncode, 0)
        self.assertEqual(failure.stdout, "")
        self.assertEqual(json.loads(failure.stderr)["status"], "error")
        self.assertNotIn("Traceback", failure.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
