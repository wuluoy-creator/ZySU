#!/usr/bin/env python3
"""Validate and package portable APK/native/embedded byte consistency evidence.

This does not authenticate a builder, verify signing keys, or prove runtime use.
Imported original build paths are never used to locate release artifacts.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import sys
import tempfile
import zipfile

from plan_build import SUPPORTED_KMIS


TARGET = "aarch64-linux-android"
ABI = "arm64-v8a"
MARKER = "assets/runtime/payload.txt"
MAX_FILE = 512 * 1024 * 1024
MAX_JSON = 16 * 1024 * 1024
MAX_APK_ENTRIES = 8192
MAX_APK_EXPANDED = 2 * 1024 * 1024 * 1024
MAX_EVIDENCE_ENTRIES = 1024
MAX_EVIDENCE_BYTES = 512 * 1024 * 1024
SHA = re.compile(r"[0-9a-f]{64}\Z")
COMMIT = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})\Z")
LIBRARY = re.compile(r"lib[A-Za-z0-9_]+\.so\Z")
LIMITATIONS = [
    "Unsigned byte/evidence consistency; builder identity and signatures are not authenticated by this verifier.",
    "Recorded native source and tool identities are not revalidated on the release consumer's machine.",
    "Embedded compressed bytes are present in read-only file-backed ELF load segments; runtime registry use and device behavior are not proven.",
    "Manager/Gradle and DDK kernel source-build provenance are not available in this release manifest.",
    "Not a complete dependency graph, signed attestation, device acceptance or reproducible-build proof.",
]


class ReleaseError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ReleaseError(message)


def safe_path(value, *, exists=True):
    raw = Path(value)
    require(".." not in raw.parts and "\0" not in str(raw), "ambiguous filesystem path")
    path = Path(os.path.abspath(raw))
    for component in (path, *path.parents):
        if component.exists() or component.is_symlink():
            info = component.lstat()
            require(not stat.S_ISLNK(info.st_mode) and not getattr(info, "st_file_attributes", 0) & 0x400,
                    f"symlink/reparse path is not supported: {component}")
    if exists:
        require(path.exists(), f"missing input: {path}")
    return path


def relative_name(name):
    require(isinstance(name, str) and name and len(name) <= 1024, "invalid relative name")
    require(not any(ord(char) < 32 for char in name) and "\\" not in name and ":" not in name,
            "invalid relative path characters")
    path = PurePosixPath(name)
    require(bool(path.parts) and not path.is_absolute() and str(path) == name and all(part not in {"", ".", ".."} for part in path.parts),
            "relative path traversal or alias")
    for part in path.parts:
        require(not part.endswith((".", " ")) and not re.fullmatch(r"(?i:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?", part),
                "relative path is ambiguous on Windows")
    return name


def record(path, *, limit=MAX_FILE):
    path = safe_path(path)
    before = path.stat()
    require(stat.S_ISREG(before.st_mode) and before.st_size <= limit, f"invalid or oversized file: {path}")
    hasher = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        require((opened.st_dev, opened.st_ino) == (before.st_dev, before.st_ino), "file replaced before read")
        while chunk := stream.read(1024 * 1024):
            size += len(chunk)
            require(size <= limit, "file exceeded read budget")
            hasher.update(chunk)
    after = path.stat()
    require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
            (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns) and size == after.st_size,
            "input changed during read")
    return {"sha256": hasher.hexdigest(), "size": size}


def checked_record(value):
    require(isinstance(value, dict) and set(value) == {"sha256", "size"}, "invalid file record")
    require(isinstance(value["sha256"], str) and SHA.fullmatch(value["sha256"]), "invalid SHA-256")
    require(type(value["size"]) is int and 0 <= value["size"] <= MAX_FILE, "invalid file size")
    return value


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        require(key not in value, "duplicate JSON key")
        value[key] = item
    return value


def same_json(left, right):
    # Derived, imported documents must not exploit Python's True == 1 equality.
    def canonical(value):
        return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False)
    return canonical(left) == canonical(right)


def load_json(path):
    expected = record(path, limit=MAX_JSON)
    # Parse only bounded bytes bound to the checked identity, even if the file
    # grows or is replaced between the initial stable read and this open.
    with safe_path(path).open("rb") as stream:
        raw = stream.read(MAX_JSON + 1)
    require(len(raw) <= MAX_JSON, "JSON exceeded read budget")
    require({"sha256": hashlib.sha256(raw).hexdigest(), "size": len(raw)} == expected,
            "JSON changed before parsing")
    value = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object,
                       parse_constant=lambda _: (_ for _ in ()).throw(ReleaseError("nonfinite JSON number")))
    pending = [(value, 0)]
    count = 0
    while pending:
        item, depth = pending.pop()
        count += 1
        require(count <= 200000 and depth <= 32, "JSON structure budget exceeded")
        if isinstance(item, dict):
            pending.extend((entry, depth + 1) for entry in item.values())
        elif isinstance(item, list):
            pending.extend((entry, depth + 1) for entry in item)
        elif isinstance(item, str):
            require(len(item) <= 65536, "JSON string budget exceeded")
    return value


def write_json(path, value):
    with Path(path).open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False)
        stream.write("\n")


def validate_selection(expected_commit, expected_kmis, library_name):
    require(isinstance(expected_commit, str) and COMMIT.fullmatch(expected_commit), "expected commit must be a full Git identity")
    require(isinstance(expected_kmis, list) and expected_kmis and len(expected_kmis) <= len(SUPPORTED_KMIS),
            "expected KMI list must be nonempty and bounded")
    require(all(isinstance(kmi, str) and kmi in SUPPORTED_KMIS for kmi in expected_kmis)
            and len(set(expected_kmis)) == len(expected_kmis), "unknown or duplicate expected KMI")
    require(isinstance(library_name, str) and LIBRARY.fullmatch(library_name) and library_name != "libksud.so",
            "invalid current payload library name")
    return sorted(expected_kmis)


def zip_inventory(archive, *, entries, total_limit):
    infos = archive.infolist()
    require(len(infos) <= entries, "ZIP entry count exceeds budget")
    found, folded, total = {}, set(), 0
    for info in infos:
        name = info.filename
        require(info.orig_filename == name, "ZIP filename contains NUL")
        relative_name(name[:-1] if info.is_dir() else name)
        require(name not in found and name.casefold() not in folded, "duplicate ZIP entry or case alias")
        require(not info.flag_bits & 1, "encrypted ZIP entry is unsupported")
        require(info.compress_type in {zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED}, "unsupported ZIP compression")
        mode = info.external_attr >> 16
        require(not stat.S_ISLNK(mode) and not (stat.S_IFMT(mode) and not (stat.S_ISREG(mode) or stat.S_ISDIR(mode))),
                "nonregular ZIP entry")
        require(0 <= info.file_size <= MAX_FILE, "ZIP entry exceeds size budget")
        total += info.file_size
        require(total <= total_limit, "ZIP expanded size exceeds budget")
        found[name] = info
        folded.add(name.casefold())
    return found


def stream_zip(archive, info, *, limit, destination=None, collect=False):
    require(info.file_size <= limit, "ZIP member exceeds declared read budget")
    size, hasher, pieces = 0, hashlib.sha256(), []
    with archive.open(info) as stream:
        while chunk := stream.read(min(1024 * 1024, limit - size + 1)):
            size += len(chunk)
            require(size <= limit, "ZIP member exceeded streaming budget")
            hasher.update(chunk)
            if destination is not None:
                destination.write(chunk)
            if collect:
                pieces.append(chunk)
    require(size == info.file_size, "ZIP member length differs from header")
    return {"sha256": hasher.hexdigest(), "size": size}, b"".join(pieces) if collect else None


def inspect_apk(apk_path, ksud_path, library_name):
    require(isinstance(library_name, str) and LIBRARY.fullmatch(library_name) and library_name != "libksud.so",
            "invalid current payload library name")
    apk_path, ksud_path = safe_path(apk_path), safe_path(ksud_path)
    before, daemon = record(apk_path), record(ksud_path)
    require(before["size"] and daemon["size"], "empty APK or daemon")
    entry = f"lib/{ABI}/{library_name}"
    with zipfile.ZipFile(apk_path) as archive:
        infos = zip_inventory(archive, entries=MAX_APK_ENTRIES, total_limit=MAX_APK_EXPANDED)
        require(MARKER in infos and entry in infos, "missing APK runtime marker or native payload")
        for name, info in infos.items():
            parts = name.split("/")
            if not info.is_dir() and parts[0] == "lib":
                require(len(parts) == 3 and parts[1] == ABI, "unsupported ABI or malformed APK native entry")
                require(parts[2] != "libksud.so", "legacy APK payload is not supported")
        _, raw = stream_zip(archive, infos[MARKER], limit=256, collect=True)
        marker = raw.decode("utf-8")
        require(marker in {library_name, library_name + "\n", library_name + "\r\n"}, "APK runtime marker mismatch")
        payload, _ = stream_zip(archive, infos[entry], limit=daemon["size"])
        require(payload == daemon, "APK payload differs from canonical ksud bytes")
    require(record(apk_path) == before and record(ksud_path) == daemon, "APK or daemon changed during inspection")
    return {"entry": entry, "marker": MARKER, "library_name": library_name, **payload}


def exact_directory(directory, allowed):
    directory = safe_path(directory)
    require(directory.is_dir(), f"not a directory: {directory}")
    for path in directory.iterdir():
        require(path.name in allowed and safe_path(path).is_file(), f"unexpected artifact entry: {path}")
    return directory


def collect_release(artifacts, *, expected_commit, expected_kmis, library_name):
    kmis = validate_selection(expected_commit, expected_kmis, library_name)
    root = safe_path(artifacts)
    require(root.is_dir(), "artifacts root must be a directory")
    for path in root.iterdir():
        safe_path(path)  # Do not follow an unexpected artifact-root symlink.
    manager = safe_path(root / f"Manager-{ABI}")
    require(manager.is_dir(), "Manager artifact directory is missing")
    apks = [path for path in manager.iterdir() if path.suffix == ".apk"]
    require(len(apks) == 1, "expected exactly one final Manager APK")
    apk = safe_path(apks[0])
    signatures = [apk.with_suffix(".sig"), Path(str(apk) + ".sig")]
    exact_directory(manager, {apk.name, *(path.name for path in signatures)})
    present = [path for path in signatures if path.exists()]
    require(len(present) <= 1, "ambiguous APK signature names")
    files = {"apk": apk}
    if present:
        files["apk.sig"] = present[0]
    native = {}
    for target in ("ksud", "ksuinit"):
        binary_dir = exact_directory(root / f"{target}-{TARGET}", {target, target + ".sig"})
        files[target] = safe_path(binary_dir / target)
        if (binary_dir / (target + ".sig")).exists():
            files[target + ".sig"] = binary_dir / (target + ".sig")
        symbol_dir = exact_directory(root / f"{target}-symbols-{TARGET}", {target + ".unstripped"})
        native[target] = {
            "bundle": safe_path(root / f"{target}-provenance-{TARGET}"),
            "symbols": safe_path(symbol_dir / (target + ".unstripped")),
        }
    expected_dirs = {kmi + "-lkm" for kmi in kmis}
    actual_dirs = {path.name for path in root.iterdir() if path.name.endswith("-lkm")}
    require(actual_dirs == expected_dirs, "downloaded KMI artifact directories differ from expected selection")
    for kmi in kmis:
        name = kmi + "_kernelsu.ko"
        directory = exact_directory(root / (kmi + "-lkm"), {name, name + ".sig"})
        files[name] = safe_path(directory / name)
        if (directory / (name + ".sig")).exists():
            files[name + ".sig"] = directory / (name + ".sig")
    for path in root.iterdir():
        if path.is_dir() and path.name not in {f"ksud-{TARGET}", f"ksuinit-{TARGET}"}:
            require(not (path / "ksud").exists() and not (path / "ksuinit").exists(), "unexpected duplicate native artifact directory")
    records = {role: record(path) for role, path in files.items()}
    require(all(item["size"] > 0 for item in records.values()), "empty release binary or signature")
    verified = verify_chain(files, native, expected_commit, kmis, library_name)
    return {"input_root": root, "files": files, "records": records, "native_paths": native,
            "expected_commit": expected_commit, "expected_kmis": kmis, "library_name": library_name, **verified}


def verify_chain(files, native_paths, expected_commit, kmis, library_name):
    # Lazy imports keep the ZIP-only command usable without build-time helpers.
    from native_evidence import verify_bundle
    from embedded_payloads import verify_proof
    bundles, evidence_files, evidence_sources = {}, {}, {}
    for target in ("ksud", "ksuinit"):
        paths = native_paths[target]
        bundle = paths["bundle"]
        mappings = {target: files[target], target + ".unstripped": paths["symbols"]}
        bundles[target] = verify_bundle(bundle / "build-manifest.json", mappings,
                                        expected_target=target, expected_commit=expected_commit)
        for name, identity in bundles[target]["evidence_files"].items():
            relative_name(name)
            dest = f"native/{target}/{name}"
            require(dest not in evidence_files, "duplicate evidence role")
            evidence_files[dest] = checked_record(identity)
            evidence_sources[dest] = safe_path(bundle / name)
        symbol_name = f"native/{target}/artifacts/{target}.unstripped"
        require(symbol_name not in evidence_files, "symbol evidence role collision")
        evidence_files[symbol_name] = record(paths["symbols"])
        evidence_sources[symbol_name] = paths["symbols"]
    bundle = native_paths["ksud"]["bundle"]
    proof = verify_proof(bundle / "embedded/manifest.json", files["ksud"], bundles["ksud"]["embedded_assets"])
    require(proof["native_manifest"] == bundles["ksud"]["manifest"]
            and proof["native_snapshot"] == bundles["ksud"]["evidence_files"]["source-snapshot.json"],
            "embedded proof native evidence binding mismatch")
    for name, identity in proof["evidence_files"].items():
        relative_name(name)
        dest = "native/ksud/embedded/" + name
        require(dest not in evidence_files, "duplicate embedded evidence role")
        evidence_files[dest] = checked_record(identity)
        evidence_sources[dest] = safe_path(bundle / "embedded" / name)
    require(proof["binary"] == record(files["ksud"]), "embedded proof binary mismatch")
    assets = {asset["name"]: asset for asset in proof["assets"]}
    require(len(assets) == len(proof["assets"]), "duplicate embedded asset")
    required = {"installer.sh", "shellrc.sh", "ksuinit", *(kmi + "_kernelsu.ko" for kmi in kmis)}
    require(set(assets) == required, "embedded asset set differs from selected release components")
    relations = []
    for name in ["ksuinit", *(kmi + "_kernelsu.ko" for kmi in kmis)]:
        actual = record(files[name])
        require(assets[name]["source"] == actual and assets[name]["normalized"] == actual,
                f"embedded component differs from downloadable bytes: {name}")
        relations.append({"name": name, **actual})
    native_summary = {}
    for target, value in bundles.items():
        native_summary[target] = {key: value[key] for key in
                                  ("target", "source", "inputs_sha256", "manifest", "artifacts", "coverage", "limitations")}
    relationships = {"apk_payload": inspect_apk(files["apk"], files["ksud"], library_name),
                     "embedded_components": relations, "embedded_proof_scope": proof["scope"],
                     "manager_source_build_evidence": "not_available", "ddk_source_build_evidence": "not_available"}
    require(len(evidence_files) <= MAX_EVIDENCE_ENTRIES and sum(item["size"] for item in evidence_files.values()) <= MAX_EVIDENCE_BYTES,
            "evidence bundle exceeds publication budget")
    return {"native": native_summary, "relationships": relationships,
            "evidence_files": evidence_files, "evidence_sources": evidence_sources}


def copy_checked(source, destination, expected):
    require(record(source) == expected, "input changed after validation")
    with safe_path(source).open("rb") as incoming, destination.open("xb") as outgoing:
        size = 0
        while chunk := incoming.read(1024 * 1024):
            size += len(chunk)
            require(size <= expected["size"], "input grew during copy")
            outgoing.write(chunk)
    require(record(destination) == expected and record(source) == expected, "copied bytes differ from validated input")


def write_evidence_zip(path, sources, records):
    require(set(sources) == set(records), "evidence source inventory mismatch")
    with zipfile.ZipFile(path, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name in sorted(sources):
            relative_name(name)
            expected = checked_record(records[name])
            require(record(sources[name]) == expected, "evidence changed before archiving")
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (stat.S_IFREG | 0o644) << 16
            hasher, size = hashlib.sha256(), 0
            with safe_path(sources[name]).open("rb") as incoming, archive.open(info, "w", force_zip64=True) as outgoing:
                while chunk := incoming.read(1024 * 1024):
                    size += len(chunk)
                    require(size <= expected["size"], "evidence grew while archiving")
                    hasher.update(chunk)
                    outgoing.write(chunk)
            require({"sha256": hasher.hexdigest(), "size": size} == expected, "evidence changed while archiving")
    require(record(path)["size"] <= MAX_FILE, "compressed evidence archive exceeds budget")


def role_names(apk_filename, kmis, roles):
    relative_name(apk_filename)
    require("/" not in apk_filename and apk_filename.endswith(".apk"), "invalid release APK filename")
    names = {"apk": apk_filename, "ksud": f"ksud-{TARGET}", "ksuinit": f"ksuinit-{TARGET}"}
    names.update({kmi + "_kernelsu.ko": kmi + "_kernelsu.ko" for kmi in kmis})
    allowed = set(names) | {role + ".sig" for role in names}
    require(set(roles) <= allowed and set(names) <= set(roles), "release role set mismatch")
    names.update({role + ".sig": name + ".sig" for role, name in list(names.items()) if role + ".sig" in roles})
    return names


def stage_release(output, collection, apk_filename, update_metadata):
    output = safe_path(output, exists=False)
    require(not output.exists(), "release output must be a new directory")
    root = collection["input_root"]
    require(output != root and root not in output.parents and output not in root.parents, "output overlaps input artifacts")
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".zysu-release-", dir=output.parent))
    try:
        names = role_names(apk_filename, collection["expected_kmis"], collection["files"])
        for role, name in names.items():
            copy_checked(collection["files"][role], stage / name, collection["records"][role])
        write_evidence_zip(stage / "native-evidence.zip", collection["evidence_sources"], collection["evidence_files"])
        metadata = json.loads(json.dumps(update_metadata))
        metadata["apk"] = {"name": apk_filename, **collection["records"]["apk"]}
        metadata["provenance"] = {"name": "native-evidence.zip", **record(stage / "native-evidence.zip"),
                                  "manifest": "release-manifest.json", "scope": "unsigned_byte_evidence_consistency"}
        write_json(stage / "update.json", metadata)
        manifest = {"schema_version": 1, "kind": "release_artifact_manifest", "source_commit": collection["expected_commit"],
                    "abi": ABI, "library_name": collection["library_name"], "selected_kmis": collection["expected_kmis"],
                    "roles": names, "artifacts": {path.name: record(path) for path in sorted(stage.iterdir())},
                    "native": collection["native"], "relationships": collection["relationships"],
                    "evidence_files": collection["evidence_files"], "limitations": LIMITATIONS}
        write_json(stage / "release-manifest.json", manifest)
        verify_release(stage)
        require(not output.exists(), "release output appeared during staging")
        os.rename(stage, output)
        return metadata
    finally:
        if stage.exists():
            require(stage.resolve().parent == output.parent.resolve() and stage.name.startswith(".zysu-release-"),
                    "refusing cleanup outside created staging directory")
            shutil.rmtree(stage)


def materialize_evidence(archive_path, records, output):
    require(isinstance(records, dict) and len(records) <= MAX_EVIDENCE_ENTRIES, "invalid evidence inventory")
    require(sum(checked_record(item)["size"] for item in records.values()) <= MAX_EVIDENCE_BYTES, "evidence budget exceeded")
    with zipfile.ZipFile(archive_path) as archive:
        infos = zip_inventory(archive, entries=MAX_EVIDENCE_ENTRIES, total_limit=MAX_EVIDENCE_BYTES)
        require(set(infos) == set(records), "evidence ZIP inventory mismatch")
        for name in sorted(records):
            relative_name(name)
            require(name.startswith(("native/ksud/", "native/ksuinit/")) and not infos[name].is_dir(), "unexpected evidence namespace")
            destination = output / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            with destination.open("xb") as stream:
                actual, _ = stream_zip(archive, infos[name], limit=records[name]["size"], destination=stream)
            require(actual == records[name], "evidence member checksum mismatch")


def verify_release(release_dir):
    root = safe_path(release_dir)
    value = load_json(root / "release-manifest.json")
    keys = {"schema_version", "kind", "source_commit", "abi", "library_name", "selected_kmis", "roles", "artifacts",
            "native", "relationships", "evidence_files", "limitations"}
    require(isinstance(value, dict) and set(value) == keys and type(value["schema_version"]) is int
            and value["schema_version"] == 1 and value["kind"] == "release_artifact_manifest", "unsupported release manifest")
    kmis = validate_selection(value["source_commit"], value["selected_kmis"], value["library_name"])
    require(kmis == value["selected_kmis"] and value["abi"] == ABI and value["limitations"] == LIMITATIONS,
            "release scope or selection mismatch")
    require(isinstance(value["roles"], dict) and isinstance(value["artifacts"], dict), "invalid release file inventory")
    names = role_names(value["roles"].get("apk"), kmis, value["roles"])
    require(names == value["roles"], "noncanonical release artifact roles")
    expected_names = set(names.values()) | {"native-evidence.zip", "update.json"}
    require(set(value["artifacts"]) == expected_names, "release manifest asset set mismatch")
    require({path.name for path in root.iterdir()} == expected_names | {"release-manifest.json"}, "release directory inventory mismatch")
    for name, identity in value["artifacts"].items():
        relative_name(name)
        require(record(root / name) == checked_record(identity) and identity["size"] > 0, "release artifact checksum mismatch")
    metadata = load_json(root / "update.json")
    from resolve_versions import version_code, version_name
    require(isinstance(metadata, dict) and set(metadata) == {"schema_version", "version_name", "version_code", "ksud", "kernel", "apk", "provenance"}
            and type(metadata["schema_version"]) is int and metadata["schema_version"] == 1, "unsupported update metadata")
    require(metadata["version_name"] == "v" + version_name(metadata["version_name"], "Manager version")
            and type(metadata["version_code"]) is int
            and version_code(metadata["version_code"], "Manager code") == metadata["version_code"], "noncanonical Manager version metadata")
    require(names["apk"] == f"ZySU_{metadata['version_name']}_{metadata['version_code']}-release.apk", "APK filename/version mismatch")
    for component in ("ksud", "kernel"):
        item = metadata[component]
        require(isinstance(item, dict) and set(item) == {"version_name", "version_code"}
                and item["version_name"] == version_name(item["version_name"], component + " version")
                and type(item["version_code"]) is int
                and item["version_code"] == version_code(item["version_code"], component + " code"), "invalid component version metadata")
    require(same_json(metadata.get("apk"), {"name": names["apk"], **value["artifacts"][names["apk"]]}),
            "update metadata APK identity mismatch")
    require(same_json(metadata.get("provenance"), {"name": "native-evidence.zip", **value["artifacts"]["native-evidence.zip"],
            "manifest": "release-manifest.json", "scope": "unsigned_byte_evidence_consistency"}), "update evidence identity mismatch")
    with tempfile.TemporaryDirectory(prefix="zysu-release-evidence-") as temporary:
        scratch = Path(temporary)
        materialize_evidence(root / "native-evidence.zip", value["evidence_files"], scratch)
        native = {target: {"bundle": scratch / "native" / target,
                           "symbols": scratch / "native" / target / "artifacts" / (target + ".unstripped")}
                  for target in ("ksud", "ksuinit")}
        actual = verify_chain({role: root / name for role, name in names.items()}, native,
                              value["source_commit"], kmis, value["library_name"])
        for key in ("native", "relationships", "evidence_files"):
            require(same_json(actual[key], value[key]), f"release {key} differs from supplied evidence")
    # Hash again after portable checks so an intervening input mutation is not accepted.
    for name, identity in value["artifacts"].items():
        require(record(root / name) == identity, "release changed during verification")
    return {"schema_version": 1, "status": "consistent", "scope": "unsigned_byte_evidence_consistency",
            "source_commit": value["source_commit"], "selected_kmis": kmis, "artifacts": len(value["artifacts"]),
            "limitations": LIMITATIONS}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    apk = commands.add_parser("apk")
    apk.add_argument("--apk", required=True, type=Path)
    apk.add_argument("--ksud", required=True, type=Path)
    apk.add_argument("--library-name", required=True)
    verify = commands.add_parser("verify")
    verify.add_argument("--release-dir", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        result = inspect_apk(args.apk, args.ksud, args.library_name) if args.command == "apk" else verify_release(args.release_dir)
        print(json.dumps(result, ensure_ascii=False, sort_keys=True))
        return 0
    except (ValueError, OSError, KeyError, TypeError, RecursionError, zipfile.BadZipFile, RuntimeError) as error:
        print(json.dumps({"schema_version": 1, "status": "error", "error": str(error)[:2000]}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
