#!/usr/bin/env python3
"""Independent ELF/byte fixtures against production-generated asset C++.

Synthetic ksuinit/KMI data verifies packaging bytes, not executable/kernel behavior.
"""

import copy
import hashlib
import importlib.util
import json
import lzma
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("embedded_payloads", Path(__file__).with_name("embedded_payloads.py"))
payloads = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(payloads)


def identity(data):
    return {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}


def elf(streams, flags=4, trailer=False):
    body = b"".join(stream + b"\x00\x91\x82\x73\x64" for stream in streams)
    total = 256 + (0 if trailer else len(body))
    header = b"\x7fELF\x02\x01\x01" + b"\x00" * 9
    header += struct.pack("<HHIQQQIHHHHHH", 3, 183, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    program = struct.pack("<IIQQQQQQ", 1, flags, 0, 0, 0, total, total, 4096)
    return (header + program).ljust(256, b"\0") + body


class EmbeddedPayloadTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="embedded-payloads-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.assets = self.root / "assets"
        self.assets.mkdir()
        self.raw = {
            "installer.sh": b"#!/bin/sh\r\necho installer\r\n",
            "shellrc.sh": b"#!/bin/sh\recho shell\r",
            "ksuinit": b"\x7fELF\0synthetic-init-byte-fixture" * 9,
            "android14-6.1_kernelsu.ko": b"\x7fELF\0synthetic-kernel-one" * 18,
            "android15-6.6_kernelsu.ko": b"\x7fELF\0synthetic-kernel-two" * 27,
        }
        self.cpp = self.root / "assets_data.cpp"
        self.generate()
        # Independent expected compression, rather than using parser output to
        # construct the ELF that the parser is tested against.
        ordinary = [zlib.compress(self.normalized(name), 9) for name in sorted(self.raw) if not name.endswith(".ko")]
        kernel_bytes = b"".join(self.raw[name] for name in sorted(self.raw) if name.endswith(".ko"))
        self.streams = ordinary + [lzma.compress(kernel_bytes, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32, preset=6)]
        self.binary = self.root / "ksud"
        self.binary.write_bytes(elf(self.streams))
        self.expected = [{"name": name, "kind": "kernel_module" if name.endswith(".ko") else "ksuinit" if name == "ksuinit" else "runtime_asset",
                          "path": "Z:/nonexistent-original/assets/" + name, **identity(data)} for name, data in sorted(self.raw.items())]
        self.native = self.root / "native"
        self.native.mkdir()
        self.manifest = self.native / "build-manifest.json"
        self.refresh_native()
        self.output = self.native / "embedded"

    def normalized(self, name):
        data = self.raw[name]
        return data.replace(b"\r\n", b"\n").replace(b"\r", b"\n") if name.endswith(".sh") else data

    def generate(self):
        for name, data in self.raw.items():
            (self.assets / name).write_bytes(data)
        result = subprocess.run([sys.executable, str(ROOT / "userspace/ksud/scripts/embed_assets.py"), str(self.assets), str(self.cpp)],
                                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)

    def refresh_native(self):
        snapshot = {"schema_version": 1, "kind": "native_source_snapshot", "target": "ksud", "inputs": {"embedded_assets": self.expected}}
        snapshot_data = (json.dumps(snapshot, sort_keys=True) + "\n").encode()
        (self.native / "source-snapshot.json").write_bytes(snapshot_data)
        self.manifest.write_text(json.dumps({"schema_version": 1, "kind": "native_build_manifest", "target": "ksud",
                                            "snapshot": {"path": "source-snapshot.json", **identity(snapshot_data)},
                                            "artifacts": [{"name": "ksud", "path": "Z:/unavailable/build/ksud", **identity(self.binary.read_bytes())}]}), encoding="utf-8")

    def create(self):
        return payloads.create_proof(self.manifest, self.binary, self.cpp, self.assets, self.output)

    def proof(self):
        return json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))

    def save_proof(self, value):
        (self.output / "manifest.json").write_text(json.dumps(value), encoding="utf-8")

    def verify(self):
        return payloads.verify_proof(self.output / "manifest.json", self.binary, self.expected)

    def test_production_generator_payloads_and_solid_offsets_match_independent_bytes(self):
        result = self.create()
        self.assertEqual(result["scope"], "bytes_present_in_file_backed_read_only_load_segments")
        self.assertEqual(len(result["assets"]), 5)
        self.assertEqual(len(result["groups"]), 4)
        for asset in result["assets"]:
            self.assertEqual(asset["source"], identity(self.raw[asset["name"]]))
            self.assertEqual(asset["normalized"], identity(self.normalized(asset["name"])))
        kmis = [item for item in result["assets"] if item["kind"] == "kernel_module"]
        self.assertEqual([item["offset"] for item in kmis], [0, len(self.raw["android14-6.1_kernelsu.ko"])])
        self.assertEqual(self.verify()["binary"], identity(self.binary.read_bytes()))

    def test_portable_verification_never_needs_original_source_or_generator(self):
        self.create()
        relocated = self.root / "relocated"
        shutil.copytree(self.output, relocated)
        moved_binary = self.root / "moved-ksud"
        shutil.copyfile(self.binary, moved_binary)
        shutil.rmtree(self.native)
        shutil.rmtree(self.assets)
        self.binary.unlink()
        self.cpp.unlink()
        with patch.object(subprocess, "run", side_effect=AssertionError("must not execute anything")):
            result = payloads.verify_proof(relocated / "manifest.json", moved_binary, self.expected)
        self.assertEqual(result["status"], "consistent")

    def test_allocated_only_rejects_trailer_writable_and_executable_matches(self):
        for flags, trailer in ((4, True), (6, False), (5, False)):
            with self.subTest(flags=flags, trailer=trailer):
                self.binary.write_bytes(elf(self.streams, flags, trailer))
                self.refresh_native()
                with self.assertRaises(payloads.PayloadError):
                    self.create()
                self.assertFalse(self.output.exists())

    def test_crossing_readonly_segment_boundary_does_not_count(self):
        data = bytearray(elf(self.streams))
        struct.pack_into("<Q", data, 64 + 32, 256 + len(self.streams[0]) - 1)
        self.binary.write_bytes(data)
        self.refresh_native()
        with self.assertRaises(payloads.PayloadError):
            self.create()

    def test_duplicate_complete_stream_is_ambiguous(self):
        self.binary.write_bytes(elf(self.streams + [self.streams[0]]))
        self.refresh_native()
        with self.assertRaisesRegex(payloads.PayloadError, "ambiguous duplicate"):
            self.create()

    def test_identical_ordinary_streams_can_share_one_folded_group(self):
        self.raw["shellrc.sh"] = self.raw["installer.sh"]
        self.generate()
        ordinary = [zlib.compress(self.normalized(name), 9) for name in sorted(self.raw) if not name.endswith(".ko")]
        self.binary.write_bytes(elf(list(dict.fromkeys(ordinary)) + self.streams[-1:]))
        for expected in self.expected:
            expected.update(identity(self.raw[expected["name"]]))
        self.refresh_native()
        result = self.create()
        ordinary_assets = {item["name"]: item for item in result["assets"]}
        self.assertEqual(ordinary_assets["installer.sh"]["group"], ordinary_assets["shellrc.sh"]["group"])
        self.assertEqual(len(result["groups"]), 3)

    def test_mutated_staged_source_and_missing_extra_assets_rejected(self):
        for change in ("mutate", "missing", "extra"):
            with self.subTest(change=change):
                self.generate()
                if change == "mutate":
                    (self.assets / "ksuinit").write_bytes(b"changed")
                elif change == "missing":
                    (self.assets / "ksuinit").unlink()
                else:
                    (self.assets / "android16-6.12_kernelsu.ko").write_bytes(b"extra")
                with self.assertRaises(payloads.PayloadError):
                    self.create()
                self.assertFalse(self.output.exists())

    def test_generated_array_checksum_or_registry_corruption_rejected(self):
        original = self.cpp.read_text(encoding="utf-8")
        replacements = [
            original.replace("AssetCodec::SolidXz", "AssetCodec::Unknown", 1),
            original.replace("static const std::array<AssetEntry, 6>", "static const std::array<AssetEntry, 7>"),
            original.replace("0x", "0X", 1),
            original + "\nstatic const unsigned char asset_orphan[] = {0x01,};\n",
            original + re.search(r"static const unsigned char asset_[A-Za-z0-9_]+\[\] = \{.*?\};", original, re.S)[0],
        ]
        for changed in replacements:
            self.cpp.write_text(changed, encoding="utf-8")
            with self.assertRaises(payloads.PayloadError):
                self.create()
        self.assertFalse(self.output.exists())

    def test_inconsistent_generated_pack_size_and_checksum_rejected(self):
        original = self.cpp.read_text(encoding="utf-8")
        changed = re.sub(r'(AssetCodec::SolidXz, [0-9]+, )([0-9]+)', lambda match: match[1] + str(int(match[2]) + 1), original, count=1)
        self.cpp.write_text(changed, encoding="utf-8")
        with self.assertRaises((payloads.PayloadError, lzma.LZMAError)):
            self.create()
        self.cpp.write_text(re.sub(r'(asset_android14_6_1_kernelsu_ko_sha256\[\] = \{\s*)0x[0-9a-f]{2}', r'\g<1>0xff', original), encoding="utf-8")
        with self.assertRaisesRegex(payloads.PayloadError, "checksum"):
            self.create()

    def test_corrupted_blob_and_binary_rejected(self):
        result = self.create()
        blob = self.output / result["groups"][0]["path"]
        original = blob.read_bytes()
        blob.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
        with self.assertRaisesRegex(payloads.PayloadError, "checksum"):
            self.verify()
        blob.write_bytes(original)
        self.binary.write_bytes(self.binary.read_bytes() + b"tampered")
        with self.assertRaisesRegex(payloads.PayloadError, "binary binding"):
            self.verify()

    def test_solid_offsets_and_normalized_identity_tampering_rejected(self):
        self.create()
        baseline = self.proof()
        for field, value in (("offset", 1), ("group", "unknown"), ("normalization", "identity")):
            changed = copy.deepcopy(baseline)
            asset = changed["assets"][1] if field != "normalization" else next(item for item in changed["assets"] if item["name"] == "installer.sh")
            asset[field] = value
            self.save_proof(changed)
            with self.assertRaises(payloads.PayloadError):
                self.verify()

    def test_missing_duplicate_or_wrong_expected_asset_rejected(self):
        self.create()
        baseline = copy.deepcopy(self.expected)
        candidates = [baseline[:-1], baseline + baseline[:1], [dict(item, sha256="0" * 64) for item in baseline]]
        for expected in candidates:
            with self.assertRaises(payloads.PayloadError):
                payloads.verify_proof(self.output / "manifest.json", self.binary, expected)

    def test_decode_rejects_bombs_crc_truncation_trailing_and_wrong_check(self):
        data = b"large enough to exercise bounds\0" * 200
        zstream = zlib.compress(data)
        xstream = lzma.compress(data, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32)
        for stream, codec, size in ((zstream, "zlib", 2), (xstream, "xz-crc32", 2),
                                    (zstream + b"extra", "zlib", len(data)), (xstream + xstream, "xz-crc32", len(data)),
                                    (zstream[:-1], "zlib", len(data)), (xstream[:-1], "xz-crc32", len(data)),
                                    (zstream[:-1] + bytes([zstream[-1] ^ 1]), "zlib", len(data)),
                                    (lzma.compress(data, check=lzma.CHECK_CRC64), "xz-crc32", len(data))):
            with self.subTest(codec=codec, size=size):
                with self.assertRaises((payloads.PayloadError, zlib.error, lzma.LZMAError)):
                    payloads.decode(stream, codec, size)

    def test_elf_header_bounds_architecture_and_mapping_conflicts_rejected(self):
        baseline = self.binary.read_bytes()
        mutations = [(18, "<H", 62), (54, "<H", 55), (56, "<H", 65535), (32, "<Q", len(baseline)), (96, "<Q", len(baseline) + 1)]
        for offset, fmt, value in mutations:
            changed = bytearray(baseline)
            struct.pack_into(fmt, changed, offset, value)
            with self.assertRaises(payloads.PayloadError):
                payloads.elf_loads(changed)
        with self.assertRaises(payloads.PayloadError):
            payloads.elf_loads(baseline[:60])

    def test_virtual_load_overlap_bss_and_address_overflow_rejected(self):
        # These headers are constructed independently of the implementation;
        # physical ranges may be disjoint while their virtual mappings collide.
        header = b"\x7fELF\x02\x01\x01" + b"\x00" * 9
        header += struct.pack("<HHIQQQIHHHHHH", 3, 183, 1, 0, 64, 0, 0, 64, 56, 2, 0, 0, 0)
        first = struct.pack("<IIQQQQQQ", 1, 4, 256, 4096, 0, 32, 32, 1)
        cases = (
            (6, 512, 4096, 32, 32),  # different physical bytes and writable
            (4, 512, 4096, 32, 32),  # same permissions, different bytes
            (6, 512, 4096, 0, 32),   # writable BSS covers read-only file bytes
            (4, 256, 4096, 16, 32),  # same translation but file/BSS disagreement
            (4, 512, (1 << 64) - 16, 16, 32),  # address addition wraps uint64
        )
        for flags, start, address, size, memory_size in cases:
            with self.subTest(flags=flags, start=start, address=address, size=size, memory_size=memory_size):
                second = struct.pack("<IIQQQQQQ", 1, flags, start, address, 0, size, memory_size, 1)
                data = (header + first + second).ljust(544, b"\x00")
                with self.assertRaises(payloads.PayloadError):
                    payloads.elf_loads(data)

    def test_identical_overlapping_load_mapping_remains_unambiguous(self):
        stream = zlib.compress(b"one physical byte range with two identical headers")
        header = b"\x7fELF\x02\x01\x01" + b"\x00" * 9
        header += struct.pack("<HHIQQQIHHHHHH", 3, 183, 1, 0, 64, 0, 0, 64, 56, 2, 0, 0, 0)
        program = struct.pack("<IIQQQQQQ", 1, 4, 256, 4096, 0, len(stream), len(stream) + 16, 1)
        data = (header + program + program).ljust(256, b"\x00") + stream
        self.assertEqual(payloads.locate(data, stream, payloads.elf_loads(data)), (256, 0))

    def test_future_bool_schema_unknown_fields_and_duplicate_json_rejected(self):
        self.create()
        baseline = self.proof()
        for mutation in ({"schema_version": 2}, {"schema_version": True}, {"extra": "field"}):
            changed = dict(baseline, **mutation)
            self.save_proof(changed)
            with self.assertRaises(payloads.PayloadError):
                self.verify()
        (self.output / "manifest.json").write_text('{"schema_version":1,"schema_version":1}', encoding="utf-8")
        with self.assertRaisesRegex(payloads.PayloadError, "duplicate JSON"):
            self.verify()

    def test_malformed_nested_types_keep_public_value_error_contract(self):
        self.create()
        baseline = self.proof()
        for section, key, value in (("groups", "codec", []), ("assets", "group", {}),
                                    ("assets", "normalization", []), ("groups", "compressed", None)):
            changed = copy.deepcopy(baseline)
            changed[section][0][key] = value
            self.save_proof(changed)
            with self.assertRaises(payloads.PayloadError):
                self.verify()

    def test_proof_path_escape_extra_files_and_empty_directories_rejected(self):
        self.create()
        baseline = self.proof()
        changed = copy.deepcopy(baseline)
        changed["groups"][0]["path"] = "../outside.zlib"
        self.save_proof(changed)
        with self.assertRaises(payloads.PayloadError):
            self.verify()
        self.save_proof(baseline)
        extra = self.output / "unexpected"
        extra.mkdir()
        with self.assertRaises(payloads.PayloadError):
            self.verify()
        extra.rmdir()
        extra.write_bytes(b"extra")
        with self.assertRaises(payloads.PayloadError):
            self.verify()

    def test_recorded_range_cannot_point_to_a_different_location(self):
        self.create()
        value = self.proof()
        value["groups"][0]["elf_offset"] += 1
        self.save_proof(value)
        with self.assertRaisesRegex(payloads.PayloadError, "location"):
            self.verify()

    def test_preexisting_output_immutable_and_interruption_leaves_no_success(self):
        self.output.mkdir()
        sentinel = self.output / "user-data"
        sentinel.write_bytes(b"preserve")
        with self.assertRaises(payloads.PayloadError):
            self.create()
        self.assertEqual(sentinel.read_bytes(), b"preserve")
        sentinel.unlink()
        self.output.rmdir()
        with patch.object(payloads.os, "rename", side_effect=OSError("interrupted publication")):
            with self.assertRaises(OSError):
                self.create()
        self.assertFalse(self.output.exists())
        self.assertFalse(list(self.native.glob(".embedded-proof-*")))

    def test_source_mutation_during_creation_prevents_publication(self):
        original = payloads.verify_proof
        def mutate(*args):
            result = original(*args)
            (self.assets / "ksuinit").write_bytes(b"changed while finalizing")
            return result
        with patch.object(payloads, "verify_proof", side_effect=mutate):
            with self.assertRaisesRegex(payloads.PayloadError, "source changed"):
                self.create()
        self.assertFalse(self.output.exists())

    def test_limits_and_normalization_semantics(self):
        with patch.object(payloads, "MAX_BINARY", 64):
            with self.assertRaises(payloads.PayloadError):
                self.create()
        self.assertEqual(payloads.normalize("installer.sh", b"a\r\nb\rc")[0], b"a\nb\nc")
        self.assertEqual(payloads.normalize("ksuinit", b"#!a\r\nb")[1], "shell_newlines_lf")
        self.assertEqual(payloads.normalize("ksuinit", b"#!a\r\n\0b")[1], "identity")

    def test_symlink_blob_is_not_followed(self):
        result = self.create()
        blob = self.output / result["groups"][0]["path"]
        target = self.root / "blob"
        shutil.copyfile(blob, target)
        blob.unlink()
        try:
            blob.symlink_to(target)
        except OSError as error:
            self.skipTest(f"host cannot create symlink: {error}")
        with self.assertRaisesRegex(payloads.PayloadError, "symlink"):
            self.verify()

    def test_cli_roundtrip_and_native_binding_failure_are_structured(self):
        create = subprocess.run([sys.executable, str(ROOT / "scripts/ci/embedded_payloads.py"), "create",
                                 "--native-manifest", str(self.manifest), "--binary", str(self.binary),
                                 "--generated-cpp", str(self.cpp), "--assets-dir", str(self.assets), "--output-dir", str(self.output)],
                                capture_output=True, text=True, encoding="utf-8", timeout=30)
        self.assertEqual(create.returncode, 0, create.stderr)
        self.assertEqual(json.loads(create.stdout)["status"], "consistent")
        self.manifest.write_text(self.manifest.read_text().replace('"target": "ksud"', '"target": "ksuinit"'), encoding="utf-8")
        verify = subprocess.run([sys.executable, str(ROOT / "scripts/ci/embedded_payloads.py"), "verify",
                                 "--native-manifest", str(self.manifest), "--binary", str(self.binary), "--proof", str(self.output / "manifest.json")],
                                capture_output=True, text=True, encoding="utf-8", timeout=30)
        self.assertNotEqual(verify.returncode, 0)
        self.assertEqual(verify.stdout, "")
        self.assertEqual(json.loads(verify.stderr)["status"], "error")


if __name__ == "__main__":
    unittest.main(verbosity=2)
