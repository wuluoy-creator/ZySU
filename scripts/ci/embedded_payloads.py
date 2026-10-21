#!/usr/bin/env python3
"""Verify embedded compressed bytes in a stripped ARM64 ELF without executing it.

This proves bytes present in file-backed read-only PT_LOAD ranges. It does not
prove runtime registry use, extraction success, source authenticity or signing.
"""

from __future__ import annotations

import argparse
from functools import wraps
import hashlib
import json
import lzma
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import sys
import tempfile
import zlib

SCHEMA = 1
MAX_BINARY = 512 * 1024 * 1024
MAX_CPP = 512 * 1024 * 1024
MAX_ASSET = 128 * 1024 * 1024
MAX_DECODED = 512 * 1024 * 1024
MAX_COMPRESSED = 256 * 1024 * 1024
MAX_JSON = 1024 * 1024
MAX_NATIVE_JSON = 64 * 1024 * 1024
MAX_ASSETS = 32
MAX_HEADERS = 128
SCOPE = "bytes_present_in_file_backed_read_only_load_segments"
LIMITATIONS = [
    "Unsigned byte-consistency evidence; no source authentication or trusted build attestation.",
    "Allocated payload bytes are verified; runtime registry use and device extraction are not executed.",
    "Shell source-to-normalized identity is observed at creation; portable verification checks the normalized bytes and bound source claims.",
]
SHA = re.compile(r"[0-9a-f]{64}\Z")
KMI = re.compile(r"(?:android\d+-)?(\d+)\.(\d+)_kernelsu\.ko\Z")
ARRAY = re.compile(r"static const unsigned char (asset_[A-Za-z0-9_]+)\[\] = \{(.*?)\};", re.S)
REGISTRY = re.compile(r"static const std::array<AssetEntry, ([0-9]+)> asset_registry = \{\{(.*?)\}\};", re.S)
ROW = re.compile(r'\{"([A-Za-z0-9_.-]+)", (asset_[A-Za-z0-9_]+), sizeof\((asset_[A-Za-z0-9_]+)\), '
                 r'([0-9]+), AssetCodec::(Zlib|SolidXz), ([0-9]+), ([0-9]+), (nullptr|asset_[A-Za-z0-9_]+)\},')


class PayloadError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise PayloadError(message)


def validation_boundary(function):
    """Keep imported malformed evidence within the public ValueError contract."""
    @wraps(function)
    def checked(*args, **kwargs):
        try:
            return function(*args, **kwargs)
        except PayloadError:
            raise
        except (KeyError, TypeError, ValueError, RecursionError, lzma.LZMAError, zlib.error, struct.error) as error:
            raise PayloadError(f"invalid embedded evidence: {type(error).__name__}") from error
    return checked


def safe_path(value, *, exists=True):
    path = Path(value)
    require(".." not in path.parts and not any(char in str(path) for char in "\x00\r\n"), "ambiguous local path")
    path = Path(os.path.abspath(path))
    for part in (path, *path.parents):
        if part.exists() or part.is_symlink():
            value = part.lstat()
            require(not stat.S_ISLNK(value.st_mode) and not getattr(value, "st_file_attributes", 0) & 0x400,
                    f"symlink/reparse path is unsupported: {part}")
    require(not exists or path.exists(), f"missing file: {path}")
    return path


def read_bytes(path, limit):
    path = safe_path(path)
    before = path.stat()
    require(stat.S_ISREG(before.st_mode) and before.st_size <= limit, "not a bounded regular file")
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        require((before.st_dev, before.st_ino) == (opened.st_dev, opened.st_ino), "input replaced before read")
        data = stream.read(limit + 1)
    after = path.stat()
    require(len(data) <= limit and len(data) == before.st_size and
            (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
            (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns), "input changed or exceeded bound")
    return data


def record(data):
    return {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key")
        result[key] = value
    return result


def load_json(path, limit=MAX_JSON):
    data = read_bytes(path, limit)
    result = json.loads(data, object_pairs_hook=unique_object,
                        parse_constant=lambda value: (_ for _ in ()).throw(PayloadError("non-finite JSON number")))
    pending = [(result, 0)]
    count = 0
    while pending:
        value, depth = pending.pop()
        count += 1
        require(depth <= 32 and count <= 1000000, "JSON structure exceeds bound")
        if isinstance(value, dict):
            pending.extend((item, depth + 1) for item in value.values())
        elif isinstance(value, list):
            pending.extend((item, depth + 1) for item in value)
        elif isinstance(value, str):
            require(len(value) <= 65536, "JSON string exceeds bound")
    return result, record(data)


def exact(value, fields, label):
    require(isinstance(value, dict) and set(value) == set(fields), f"invalid {label} fields")


def integer(value, maximum, label, minimum=0):
    require(type(value) is int and minimum <= value <= maximum, f"invalid {label}")
    return value


def file_identity(value, limit=MAX_BINARY):
    exact(value, ("sha256", "size"), "file identity")
    require(isinstance(value["sha256"], str) and SHA.fullmatch(value["sha256"]), "invalid SHA256")
    integer(value["size"], limit, "file size")
    return value


def asset_kind(name):
    require(isinstance(name, str) and len(name) <= 96, "invalid asset name")
    if name in {"installer.sh", "shellrc.sh"}:
        return "runtime_asset"
    if name == "ksuinit":
        return "ksuinit"
    match = KMI.fullmatch(name)
    require(match is not None and tuple(map(int, match.groups())) >= (6, 1), "unsupported asset/KMI name")
    return "kernel_module"


def expected_asset_map(expected_assets):
    require(isinstance(expected_assets, list) and 1 <= len(expected_assets) <= MAX_ASSETS, "invalid expected asset count")
    result = {}
    total = 0
    for item in expected_assets:
        require(isinstance(item, dict), "invalid expected asset")
        name = item.get("name")
        kind = asset_kind(name)
        require(name not in result and item.get("kind") == kind, "duplicate asset or mismatched kind")
        identity = file_identity({key: item[key] for key in ("sha256", "size")}, MAX_ASSET)
        total += identity["size"]
        require(total <= MAX_DECODED, "expected asset bytes exceed bound")
        result[name] = {"kind": kind, "source": identity}
    return dict(sorted(result.items()))


def normalize(name, data):
    if name.endswith((".sh", ".bash")) or data.startswith(b"#!") and b"\0" not in data:
        return data.replace(b"\r\n", b"\n").replace(b"\r", b"\n"), "shell_newlines_lf"
    return data, "identity"


def elf_loads(data):
    require(64 <= len(data) <= MAX_BINARY and data[:7] == b"\x7fELF\x02\x01\x01", "expected ELF64 little-endian binary")
    header = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    etype, machine, version, _, offset, _, _, ehsize, entsize, count, _, _, _ = header
    require(etype == 3 and machine == 183 and version == 1 and ehsize == 64 and entsize == 56,
            "expected ARM64 ET_DYN ELF")
    require(1 <= count <= MAX_HEADERS and offset >= 64 and offset + count * entsize <= len(data), "invalid ELF program headers")
    loads = []
    for index in range(count):
        kind, flags, start, vaddr, _, size, memory_size, alignment = struct.unpack_from("<IIQQQQQQ", data, offset + index * entsize)
        if kind != 1:
            continue
        require(size <= memory_size and start + size <= len(data) and flags & ~7 == 0, "invalid ELF load range")
        require(vaddr + memory_size <= (1 << 64) - 1, "overflowed ELF virtual load range")
        require(alignment in {0, 1} or alignment & (alignment - 1) == 0, "invalid ELF load alignment")
        require(alignment <= 1 or start % alignment == vaddr % alignment, "incongruent ELF load alignment")
        load = {"index": index, "offset": start, "size": size, "memory_size": memory_size, "vaddr": vaddr, "flags": flags}
        for previous in loads:
            if size and previous["size"] and max(start, previous["offset"]) < min(start + size, previous["offset"] + previous["size"]):
                require(flags == previous["flags"] and vaddr - start == previous["vaddr"] - previous["offset"],
                        "ambiguous overlapping ELF load mappings")
            overlap_start = max(vaddr, previous["vaddr"])
            overlap_end = min(vaddr + memory_size, previous["vaddr"] + previous["memory_size"])
            if overlap_start < overlap_end:
                # Disjoint file ranges can still map over the same virtual
                # bytes. Include zero-filled memory: BSS must not cover a
                # supposedly file-backed payload in another load segment.
                file_end = min(overlap_end, max(overlap_start, vaddr + size))
                previous_file_end = min(overlap_end, max(overlap_start, previous["vaddr"] + previous["size"]))
                require(flags == previous["flags"] and vaddr - start == previous["vaddr"] - previous["offset"]
                        and file_end == previous_file_end, "ambiguous overlapping ELF virtual mappings")
        loads.append(load)
    require(any(item["flags"] == 4 and item["size"] for item in loads), "no read-only file-backed ELF load segment")
    return loads


def locate(data, compressed, loads):
    require(compressed, "empty compressed stream")
    matches = {}
    for load in loads:
        if load["flags"] != 4:
            continue
        start, end = load["offset"], load["offset"] + load["size"]
        while True:
            found = data.find(compressed, start, end)
            if found < 0:
                break
            matches.setdefault(found, load["index"])
            require(len(matches) <= 1, "ambiguous duplicate compressed bytes in read-only ELF loads")
            start = found + 1
    require(len(matches) == 1, "compressed bytes missing from read-only file-backed ELF loads")
    offset, load_index = next(iter(matches.items()))
    return offset, load_index


def decode(data, codec, size):
    integer(size, MAX_DECODED, "decoded size")
    require(0 < len(data) <= MAX_COMPRESSED, "invalid compressed size")
    if codec == "zlib":
        decoder = zlib.decompressobj()
        result = decoder.decompress(data, size + 1)
        require(decoder.eof and not decoder.unused_data and not decoder.unconsumed_tail,
                "truncated/trailing/oversized zlib stream")
    elif codec == "xz-crc32":
        decoder = lzma.LZMADecompressor(format=lzma.FORMAT_XZ, memlimit=64 * 1024 * 1024)
        result = decoder.decompress(data, max_length=size + 1)
        require(decoder.eof and not decoder.unused_data and decoder.check == lzma.CHECK_CRC32,
                "truncated/trailing/oversized/non-CRC32 XZ stream")
    else:
        raise PayloadError("unsupported compression codec")
    require(len(result) == size, "decoded length mismatch")
    return result


def parse_generated(data):
    text = data.decode("utf-8")
    arrays = {}
    matches = list(ARRAY.finditer(text))
    require(1 <= len(matches) <= MAX_ASSETS * 2 + 1 and len(matches) == text.count("static const unsigned char "),
            "missing/malformed/generated array count")
    total = 0
    for match in matches:
        name, body = match.groups()
        require(name not in arrays, "duplicate generated array")
        require(re.fullmatch(r"\s*(?:0x[0-9a-f]{2},\s*)*", body), "invalid generated byte token")
        payload = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", body))
        total += len(payload)
        require(total <= MAX_COMPRESSED, "generated compressed arrays exceed bound")
        arrays[name] = payload
    registries = list(REGISTRY.finditer(text))
    require(len(registries) == 1 and text.count("static const std::array<AssetEntry,") == 1, "missing/duplicate/malformed generated registry")
    count, body = registries[0].groups()
    lines = [line.strip() for line in body.splitlines() if line.strip()]
    require(lines and lines[-1] == "{nullptr, nullptr, 0, 0, AssetCodec::Zlib, 0, 0, nullptr}", "invalid registry sentinel")
    require(int(count) == len(lines) and 2 <= len(lines) <= MAX_ASSETS + 1, "invalid registry count")
    rows = []
    names = set()
    used = set()
    for line in lines[:-1]:
        match = ROW.fullmatch(line)
        require(match is not None, "unsupported generated registry row")
        name, symbol, size_symbol, size, codec, offset, total_size, checksum = match.groups()
        asset_kind(name)
        require(name not in names and symbol == size_symbol and symbol in arrays, "duplicate/dangling generated registry entry")
        names.add(name)
        used.add(symbol)
        row = {"name": name, "symbol": symbol, "size": int(size), "codec": codec, "offset": int(offset),
               "total_size": int(total_size), "checksum": checksum}
        integer(row["size"], MAX_ASSET, "asset size")
        integer(row["offset"], MAX_DECODED, "asset offset")
        integer(row["total_size"], MAX_DECODED, "pack size")
        if codec == "Zlib":
            require(row["offset"] == 0 and row["total_size"] == 0 and checksum == "nullptr" and
                    asset_kind(name) != "kernel_module", "invalid ordinary asset layout")
        else:
            require(asset_kind(name) == "kernel_module" and symbol == "asset_lkm_pack_xz" and
                    checksum in arrays and len(arrays[checksum]) == 32, "invalid solid XZ registry layout")
            used.add(checksum)
        rows.append(row)
    require([row["name"] for row in rows] == sorted(names), "noncanonical generated asset order")
    require(used == set(arrays), "orphan generated array")
    return arrays, rows


def native_binding(manifest_path, binary):
    manifest_path = safe_path(manifest_path)
    manifest, manifest_identity = load_json(manifest_path, MAX_NATIVE_JSON)
    require(isinstance(manifest, dict) and type(manifest.get("schema_version")) is int and manifest.get("schema_version") == 1 and manifest.get("kind") == "native_build_manifest" and
            manifest.get("target") == "ksud", "expected native ksud manifest")
    snapshot_entry = manifest["snapshot"]
    require(snapshot_entry["path"] == "source-snapshot.json", "unsupported native snapshot path")
    snapshot, snapshot_identity = load_json(manifest_path.parent / "source-snapshot.json", MAX_NATIVE_JSON)
    require(snapshot_identity == {key: snapshot_entry[key] for key in ("sha256", "size")}, "native snapshot binding mismatch")
    require(isinstance(snapshot, dict) and snapshot.get("target") == "ksud" and type(snapshot.get("schema_version")) is int and snapshot.get("schema_version") == 1 and
            snapshot.get("kind") == "native_source_snapshot", "expected native ksud snapshot")
    artifacts = [item for item in manifest["artifacts"] if item["name"] == "ksud"]
    require(len(artifacts) == 1 and record(binary) == {key: artifacts[0][key] for key in ("sha256", "size")},
            "binary does not match native manifest ksud")
    return snapshot["inputs"]["embedded_assets"], manifest_identity, snapshot_identity


@validation_boundary
def create_proof(native_manifest_path, ksud_path, generated_cpp, assets_dir, output_dir):
    binary = read_bytes(ksud_path, MAX_BINARY)
    expected_assets, native_manifest, native_snapshot = native_binding(native_manifest_path, binary)
    expected = expected_asset_map(expected_assets)
    assets_dir = safe_path(assets_dir)
    require(assets_dir.is_dir(), "assets directory required")
    selected = sorted(path.name for path in assets_dir.iterdir() if path.name in {"installer.sh", "shellrc.sh", "ksuinit"} or path.name.endswith("_kernelsu.ko"))
    require(selected == list(expected), "actual staged asset set differs from native snapshot")
    normalized = {}
    for name, item in expected.items():
        raw = read_bytes(assets_dir / name, MAX_ASSET)
        require(record(raw) == item["source"], "staged source differs from native snapshot")
        payload, transformation = normalize(name, raw)
        # Actual ELF/module binary assets must remain byte-identical; arbitrary
        # shebang content would make the release's standalone relation ambiguous.
        require(item["kind"] == "runtime_asset" or transformation == "identity", "binary asset unexpectedly requires text normalization")
        normalized[name] = (payload, transformation)
    cpp = read_bytes(generated_cpp, MAX_CPP)
    arrays, rows = parse_generated(cpp)
    require([row["name"] for row in rows] == list(expected), "generated registry asset set differs from snapshot")
    loads = elf_loads(binary)
    groups, assets, blobs = [], [], {}
    shared = {}
    for row in rows:
        name = row["name"]
        compressed = arrays[row["symbol"]]
        codec = "zlib" if row["codec"] == "Zlib" else "xz-crc32"
        group_key = (codec, compressed)
        if group_key not in shared:
            group_id = f"g{len(groups):02d}"
            length = row["size"] if codec == "zlib" else row["total_size"]
            decoded = decode(compressed, codec, length)
            offset, load_index = locate(binary, compressed, loads)
            filename = f"payloads/{len(groups):02d}.{'zlib' if codec == 'zlib' else 'xz'}"
            groups.append({"id": group_id, "codec": codec, "path": filename, "compressed": record(compressed),
                           "decoded": record(decoded), "elf_offset": offset, "load_index": load_index})
            shared[group_key] = (group_id, decoded)
            blobs[filename] = compressed
        group_id, decoded = shared[group_key]
        require(codec == "zlib" or row["total_size"] == len(decoded), "inconsistent generated solid pack size")
        data, transformation = normalized[name]
        require(row["size"] == len(data) and decoded[row["offset"]:row["offset"] + row["size"]] == data,
                "generated compressed content differs from normalized staged source")
        if codec == "xz-crc32":
            require(arrays[row["checksum"]] == hashlib.sha256(data).digest(), "generated KMI checksum differs")
        assets.append({"name": name, "kind": expected[name]["kind"], "source": expected[name]["source"],
                       "normalized": record(data), "normalization": transformation, "group": group_id, "offset": row["offset"]})
    proof = {"schema_version": SCHEMA, "kind": "embedded_payload_proof", "target": "ksud", "scope": SCOPE,
             "native_manifest": native_manifest, "native_snapshot": native_snapshot, "binary": record(binary),
             "generated_cpp": record(cpp), "assets": assets, "groups": groups, "limitations": LIMITATIONS}
    output = safe_path(output_dir, exists=False)
    require(not output.exists(), "embedded evidence destination already exists")
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".embedded-proof-", dir=output.parent))
    try:
        (staging / "payloads").mkdir()
        for filename, data in blobs.items():
            (staging / filename).write_bytes(data)
        (staging / "manifest.json").write_text(json.dumps(proof, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        result = verify_proof(staging / "manifest.json", ksud_path, expected_assets)
        require(read_bytes(generated_cpp, MAX_CPP) == cpp, "generated input changed during proof creation")
        for name, item in expected.items():
            require(record(read_bytes(assets_dir / name, MAX_ASSET)) == item["source"], "source changed during proof creation")
        require(not output.exists(), "embedded evidence destination appeared during creation")
        os.rename(staging, output)
        return result
    finally:
        if staging.exists():
            shutil.rmtree(staging)


@validation_boundary
def verify_proof(proof_path, ksud_path, expected_assets):
    proof_path = safe_path(proof_path)
    require(proof_path.name == "manifest.json", "embedded proof must be manifest.json")
    proof, proof_identity = load_json(proof_path)
    exact(proof, ("schema_version", "kind", "target", "scope", "native_manifest", "native_snapshot", "binary", "generated_cpp", "assets", "groups", "limitations"), "embedded proof")
    require(type(proof["schema_version"]) is int and proof["schema_version"] == SCHEMA and
            proof["kind"] == "embedded_payload_proof" and proof["target"] == "ksud" and proof["scope"] == SCOPE,
            "unsupported embedded proof schema/scope")
    require(proof["limitations"] == LIMITATIONS, "unsupported proof limitations")
    for key in ("native_manifest", "native_snapshot", "binary", "generated_cpp"):
        file_identity(proof[key], MAX_CPP if key == "generated_cpp" else MAX_BINARY)
    expected = expected_asset_map(expected_assets)
    require(isinstance(proof["assets"], list) and len(proof["assets"]) == len(expected), "proof asset count differs")
    require(isinstance(proof["groups"], list) and 1 <= len(proof["groups"]) <= MAX_ASSETS, "invalid proof group count")
    binary = read_bytes(ksud_path, MAX_BINARY)
    require(record(binary) == proof["binary"], "ELF binary binding mismatch")
    loads = elf_loads(binary)
    groups, decoded_groups, evidence = {}, {}, {"manifest.json": proof_identity}
    stream_identities = set()
    total_compressed = total_decoded = 0
    for index, group in enumerate(proof["groups"]):
        exact(group, ("id", "codec", "path", "compressed", "decoded", "elf_offset", "load_index"), "compressed group")
        require(group["id"] == f"g{index:02d}" and group["codec"] in {"zlib", "xz-crc32"}, "noncanonical group identity")
        extension = "zlib" if group["codec"] == "zlib" else "xz"
        require(group["path"] == f"payloads/{index:02d}.{extension}", "noncanonical proof blob path")
        file_identity(group["compressed"], MAX_COMPRESSED)
        file_identity(group["decoded"], MAX_DECODED)
        stream_key = (group["codec"], group["compressed"]["sha256"], group["compressed"]["size"])
        require(stream_key not in stream_identities, "duplicate compressed group identity")
        stream_identities.add(stream_key)
        total_compressed += group["compressed"]["size"]
        total_decoded += group["decoded"]["size"]
        require(total_compressed <= MAX_COMPRESSED and total_decoded <= MAX_DECODED, "proof payload aggregate exceeds bound")
        integer(group["elf_offset"], len(binary), "ELF payload offset")
        integer(group["load_index"], MAX_HEADERS - 1, "ELF load index")
        compressed = read_bytes(proof_path.parent / group["path"], MAX_COMPRESSED)
        require(record(compressed) == group["compressed"], "compressed proof checksum mismatch")
        found, load_index = locate(binary, compressed, loads)
        require(found == group["elf_offset"] and load_index == group["load_index"], "ELF compressed location mismatch")
        decoded = decode(compressed, group["codec"], group["decoded"]["size"])
        require(record(decoded) == group["decoded"], "decoded proof checksum mismatch")
        groups[group["id"]] = group
        decoded_groups[group["id"]] = decoded
        evidence[group["path"]] = group["compressed"]
    names, references = [], {key: [] for key in groups}
    for asset in proof["assets"]:
        exact(asset, ("name", "kind", "source", "normalized", "normalization", "group", "offset"), "asset")
        name = asset["name"]
        kind = asset_kind(name)
        require(name in expected and asset["kind"] == kind and asset["source"] == expected[name]["source"], "asset source identity differs")
        file_identity(asset["source"], MAX_ASSET)
        file_identity(asset["normalized"], MAX_ASSET)
        require(asset["normalization"] in {"identity", "shell_newlines_lf"} and
                (kind == "runtime_asset" or asset["normalization"] == "identity"), "unsupported normalization")
        if asset["normalization"] == "identity":
            require(asset["source"] == asset["normalized"], "identity normalization changed bytes")
        else:
            require(asset["normalized"]["size"] <= asset["source"]["size"], "newline normalization grew input")
        require(asset["group"] in groups, "unknown asset group")
        integer(asset["offset"], MAX_DECODED, "asset offset")
        group = groups[asset["group"]]
        require((kind == "kernel_module") == (group["codec"] == "xz-crc32"), "asset codec/kind mismatch")
        if group["codec"] == "zlib":
            require(asset["offset"] == 0 and asset["normalized"] == group["decoded"], "ordinary asset group differs")
        data = decoded_groups[asset["group"]][asset["offset"]:asset["offset"] + asset["normalized"]["size"]]
        require(record(data) == asset["normalized"], "normalized member bytes differ")
        references[asset["group"]].append(asset)
        names.append(name)
    require(names == list(expected), "duplicate/noncanonical/missing asset names")
    xz_count = 0
    for key, group in groups.items():
        require(references[key], "orphan compressed group")
        if group["codec"] == "xz-crc32":
            xz_count += 1
            offset = 0
            for asset in references[key]:
                require(asset["offset"] == offset, "solid XZ member gap/overlap")
                offset += asset["normalized"]["size"]
            require(offset == group["decoded"]["size"], "solid XZ pack size differs")
    require(xz_count <= 1, "multiple solid XZ groups")
    actual = set()
    for current, dirs, files in os.walk(proof_path.parent, followlinks=False):
        require(set(dirs) <= ({"payloads"} if Path(current) == proof_path.parent else set()), "extra embedded evidence directory")
        for name in dirs:
            safe_path(Path(current) / name)
        for name in files:
            path = safe_path(Path(current) / name)
            actual.add(path.relative_to(proof_path.parent).as_posix())
            require(len(actual) <= MAX_ASSETS + 1, "extra embedded evidence files")
    require(actual == set(evidence), "missing/extra embedded evidence files")
    return {"schema_version": SCHEMA, "status": "consistent", "scope": SCOPE, "binary": proof["binary"],
            "native_manifest": proof["native_manifest"], "native_snapshot": proof["native_snapshot"],
            "assets": proof["assets"], "groups": proof["groups"], "evidence_files": evidence,
            "limitations": LIMITATIONS}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("create")
    create.add_argument("--native-manifest", required=True)
    create.add_argument("--binary", required=True)
    create.add_argument("--generated-cpp", required=True)
    create.add_argument("--assets-dir", required=True)
    create.add_argument("--output-dir", required=True)
    verify = commands.add_parser("verify")
    verify.add_argument("--proof", required=True)
    verify.add_argument("--binary", required=True)
    verify.add_argument("--native-manifest", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "create":
            result = create_proof(args.native_manifest, args.binary, args.generated_cpp, args.assets_dir, args.output_dir)
        else:
            expected, manifest, snapshot = native_binding(args.native_manifest, read_bytes(args.binary, MAX_BINARY))
            result = verify_proof(args.proof, args.binary, expected)
            require(result["native_manifest"] == manifest and result["native_snapshot"] == snapshot, "native proof binding mismatch")
        print(json.dumps(result, sort_keys=True))
        return 0
    except (PayloadError, OSError, ValueError, KeyError, TypeError, RecursionError, lzma.LZMAError, zlib.error, struct.error) as error:
        print(json.dumps({"schema_version": SCHEMA, "status": "error", "error": str(error)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
