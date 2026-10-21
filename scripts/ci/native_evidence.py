#!/usr/bin/env python3
"""Verify relocated native evidence consistency, without source/tool execution.

This is deliberately separate from native_provenance.verify: imported original
paths are inert observations, not filesystem inputs. Evidence is unsigned.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys

import native_provenance as native


MAX_JSON = 64 * 1024 * 1024
MAX_FILE = 512 * 1024 * 1024
MAX_TREE_BYTES = 2 * 1024 * 1024 * 1024
MAX_TREE_FILES = 50000
MAX_TOTAL_FILES = 300000
MAX_LICENSE_BYTES = 128 * 1024 * 1024
MAX_NODES = 2_000_000
MAX_DEPTH = 32
MAX_STRING = 64 * 1024
HASH = re.compile(r"[0-9a-f]{64}\Z")
COMMIT = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})\Z")
NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,95}\Z")
KMI = re.compile(r"(?:android\d+-)?(\d+)\.(\d+)\Z")
PORTABLE_LIMITATIONS = [
    "Unsigned portable evidence consistency; not authenticated provenance or a signed attestation.",
    "Original source/dependency/tool paths are inert claims; their bytes and tools are not revalidated or executed.",
    "Embedded asset records describe staged source identities; final ELF payload presence requires separate byte proof.",
    "Partial native component coverage does not establish full Gradle, Android sysroot or kernel build lineage.",
]


class EvidenceError(ValueError):
    pass


def need(condition, message):
    if not condition:
        raise EvidenceError(message)


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")


def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def obj(value, required, optional=(), label="object"):
    need(type(value) is dict, f"{label} must be an object")
    need(set(required) <= value.keys() and value.keys() <= set(required) | set(optional),
         f"{label} has missing or unknown fields")
    return value


def seq(value, maximum, label, minimum=0):
    need(type(value) is list and minimum <= len(value) <= maximum, f"invalid {label} list capacity")
    return value


def text_value(value, label, maximum=MAX_STRING, *, empty=False):
    need(type(value) is str and (empty or bool(value)) and len(value) <= maximum and "\x00" not in value,
         f"invalid {label} string")
    return value


def number(value, label, maximum=MAX_FILE, minimum=0):
    need(type(value) is int and minimum <= value <= maximum, f"invalid {label} integer")
    return value


def sha(value, label="SHA-256"):
    need(type(value) is str and HASH.fullmatch(value), f"invalid {label}")
    return value


def commit(value):
    need(type(value) is str and COMMIT.fullmatch(value), "invalid full Git commit")
    return value


def name(value, label="identity"):
    need(type(value) is str and NAME.fullmatch(value), f"invalid {label}")
    return value


def relative(value):
    text_value(value, "relative path", 2048)
    need(not any(ord(char) < 32 or ord(char) == 127 for char in value), "control character in path")
    path = PurePosixPath(value)
    need(value != "." and "\\" not in value and ":" not in value and not path.is_absolute() and str(path) == value
         and all(part not in {".", "..", ""} for part in path.parts), "noncanonical relative path")
    return value


def observation_path(value):
    # Windows-created evidence must remain valid on POSIX and vice versa. These
    # strings are NEVER passed to Path, open, resolve, subprocess, or import.
    text_value(value, "observed path", 16384)
    need(not any(ord(char) < 32 or ord(char) == 127 for char in value), "control character in observed path")
    need(value.startswith("/") or re.match(r"[A-Za-z]:[\\/]", value), "observed path is not absolute")
    return value


def no_aliases(values, label):
    seen = set()
    for value in values:
        key = value.replace("\\", "/").casefold().rstrip("/")
        need(key not in seen, f"duplicate or ambiguous {label}")
        seen.add(key)


def opened_path(value):
    raw = Path(value)
    need(".." not in raw.parts, "parent traversal in supplied path")
    path = Path(os.path.abspath(raw))
    for part in (path, *path.parents):
        info = part.lstat()
        need(not stat.S_ISLNK(info.st_mode) and not getattr(info, "st_file_attributes", 0) & 0x400,
             "supplied path uses symlink/reparse component")
    return path


def read_file(path, maximum=MAX_FILE, *, retain=False):
    path = opened_path(path)
    before = path.stat()
    need(stat.S_ISREG(before.st_mode) and before.st_size <= maximum, "missing/nonregular/oversized evidence file")
    hasher, size, blocks = hashlib.sha256(), 0, []
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        need((before.st_dev, before.st_ino) == (opened.st_dev, opened.st_ino), "input replaced before read")
        while chunk := stream.read(1024 * 1024):
            size += len(chunk)
            need(size <= maximum, "file grew beyond capacity")
            hasher.update(chunk)
            if retain:
                blocks.append(chunk)
    after = path.stat()
    need((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
         (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns) and size == after.st_size,
         "file changed during verification")
    record = {"sha256": hasher.hexdigest(), "size": size}
    return (record, b"".join(blocks)) if retain else record


def unique_pairs(pairs):
    value = {}
    for key, item in pairs:
        need(key not in value, "duplicate JSON key")
        value[key] = item
    return value


def json_bounds(value):
    pending, count = [(value, 0)], 0
    while pending:
        item, depth = pending.pop()
        count += 1
        need(count <= MAX_NODES and depth <= MAX_DEPTH, "JSON structural capacity exceeded")
        if type(item) is dict:
            pending.extend((key, depth + 1) for key in item)
            pending.extend((child, depth + 1) for child in item.values())
        elif type(item) is list:
            pending.extend((child, depth + 1) for child in item)
        elif type(item) is str:
            need(len(item) <= MAX_STRING, "JSON string capacity exceeded")
        elif type(item) is int:
            need(abs(item) <= (1 << 63) - 1, "JSON integer capacity exceeded")
        else:
            need(item is None or type(item) is bool, "unsupported JSON scalar")


def json_file(path):
    record, raw = read_file(path, MAX_JSON, retain=True)
    try:
        value = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=unique_pairs,
                           parse_constant=lambda _: (_ for _ in ()).throw(EvidenceError("nonfinite JSON number")))
    except (UnicodeError, RecursionError, json.JSONDecodeError) as error:
        raise EvidenceError("invalid/bounded JSON input") from error
    json_bounds(value)
    return record, value


def hash_record(value, label="file record", *, minimum=0, maximum=MAX_FILE):
    obj(value, ("sha256", "size"), label=label)
    sha(value["sha256"])
    number(value["size"], "file size", maximum, minimum)
    return value


def git_observation(value):
    obj(value, ("commit", "dirty", "status_sha256"), label="Git observation")
    commit(value["commit"])
    need(type(value["dirty"]) is bool, "Git dirty state must be boolean")
    sha(value["status_sha256"])


def file_table(value, *, original_paths=False):
    need(type(value) is dict and 1 <= len(value) <= MAX_TREE_FILES, "invalid source file table")
    total = 0
    for key, record in value.items():
        (observation_path if original_paths else relative)(key)
        hash_record(record)
        total += record["size"]
    no_aliases(value, "file table path")
    need(total <= MAX_TREE_BYTES, "source tree byte capacity exceeded")
    return len(value)


def licenses_in_table(value, files):
    seq(value, 64, "license evidence")
    for entry in value:
        obj(entry, ("path", "sha256", "size"), label="source license evidence")
        relative(entry["path"])
        hash_record({key: entry[key] for key in ("sha256", "size")}, maximum=16 * 1024 * 1024)
        need(files.get(entry["path"]) == {key: entry[key] for key in ("sha256", "size")},
             "license source identity is absent from file table")
    no_aliases([entry["path"] for entry in value], "license source path")


def expected_identity(value, kind):
    allowed = {"kind", "revision", "repository", "version", "url", "archive_sha256",
               "gitlink", "path", "gitlink_path", "source_subdir"}
    need(type(value) is dict and value and value.keys() <= allowed, "invalid expected dependency identity")
    for key, item in value.items():
        text_value(item, "expected dependency identity", 4096)
        if key in {"revision", "gitlink"}:
            commit(item)
        elif key == "archive_sha256":
            sha(item)
        elif key in {"path", "gitlink_path", "source_subdir"}:
            relative(item)
    if "kind" in value:
        need(value["kind"] == kind, "dependency expected kind mismatch")
    if kind == "git":
        need("revision" in value, "Git dependency requires immutable revision")
    elif kind == "archive":
        need("archive_sha256" in value, "archive dependency requires expected SHA-256")
    elif kind == "submodule":
        need({"gitlink", "path"} <= value.keys(), "submodule expected identity incomplete")


def dependency(value):
    obj(value, ("name", "source_dir", "kind", "expected", "resolution", "local_override", "comparison",
                "git", "files", "tree_sha256", "license_evidence", "license_evidence_status"), label="dependency")
    name(value["name"])
    observation_path(value["source_dir"])
    need(value["kind"] in {"git", "archive", "submodule", "vendored"}, "unknown dependency kind")
    expected_identity(value["expected"], value["kind"])
    need(type(value["local_override"]) is bool, "override state must be boolean")
    text_value(value["resolution"], "dependency resolution", 64)
    if value["git"] is not None:
        git_observation(value["git"])
    if value["local_override"]:
        expected_comparison = "local_override_not_verified_against_expected_identity"
    elif value["expected"].get("revision") and value["git"] is not None:
        need(value["expected"]["revision"] == value["git"]["commit"], "recorded Git commit does not match claimed pin")
        expected_comparison = "git_commit_matches"
    elif value["expected"].get("archive_sha256"):
        expected_comparison = "archive_expected_only_extracted_tree_fingerprinted"
    else:
        expected_comparison = "expected_identity_recorded_actual_tree_fingerprinted"
    need(value["comparison"] == expected_comparison, "dependency comparison claim is inconsistent")
    if value["kind"] == "submodule":
        need(value["git"] is not None and value["local_override"] ==
             (value["expected"]["gitlink"] != value["git"]["commit"]), "submodule gitlink/override claim is inconsistent")
    count = file_table(value["files"])
    need(sha(value["tree_sha256"]) == digest(value["files"]), "dependency file-table digest mismatch")
    licenses_in_table(value["license_evidence"], value["files"])
    need(value["license_evidence_status"] == ("files_recorded" if value["license_evidence"] else "not_found"),
         "dependency license evidence status mismatch")
    return count


def toolchain(value):
    obj(value, ("settings", "tools", "files"), ("ndk_source_properties",), label="toolchain observation")
    need(type(value["settings"]) is dict and value["settings"].keys() <= set(native.SETTINGS), "unknown configured settings")
    for setting in value["settings"].values():
        text_value(setting, "configured setting", empty=True)
    need(type(value["tools"]) is dict and value["tools"].keys() <= set(native.TOOL_KEYS) | {"provenance_python"}
         and {"CMAKE_CXX_COMPILER", "provenance_python"} <= value["tools"].keys(), "invalid configured tools")
    for tool in value["tools"].values():
        obj(tool, ("invocation", "resolved_path", "sha256", "size", "symlink_chain", "version"), label="tool observation")
        observation_path(tool["invocation"])
        observation_path(tool["resolved_path"])
        hash_record({key: tool[key] for key in ("sha256", "size")})
        text_value(tool["version"], "tool version", empty=True)
        for link in seq(tool["symlink_chain"], 32, "tool symlinks"):
            obj(link, ("path", "target"), label="tool symlink observation")
            observation_path(link["path"])
            text_value(link["target"], "tool link target", 16384)
    file_table(value["files"], original_paths=True)
    if "ndk_source_properties" in value:
        text_value(value["ndk_source_properties"], "NDK metadata", empty=True)


def embedded(value, source_files, target):
    seq(value, 32, "embedded source assets")
    need(target == "ksud" or not value, "ksuinit cannot declare embedded ksud assets")
    for entry in value:
        obj(entry, ("name", "path", "sha256", "size", "kind"), ("kmi",), label="embedded source record")
        name(entry["name"], "embedded asset name")
        observation_path(entry["path"])
        hash_record({key: entry[key] for key in ("sha256", "size")})
        if entry["name"].endswith("_kernelsu.ko"):
            kmi = entry["name"].removesuffix("_kernelsu.ko")
            match = KMI.fullmatch(kmi)
            need(match and tuple(map(int, match.groups())) >= (6, 1), "invalid embedded KMI")
            need(entry.get("kmi") == kmi and entry["kind"] == "kernel_module", "embedded KMI identity mismatch")
        else:
            need(entry["name"] in {"installer.sh", "shellrc.sh", "ksuinit"} and "kmi" not in entry,
                 "unknown embedded runtime asset")
            need(entry["kind"] == ("ksuinit" if entry["name"] == "ksuinit" else "runtime_asset"), "embedded runtime kind mismatch")
        relative_name = "userspace/ksud/assets/" + entry["name"]
        need(source_files.get(relative_name) == {key: entry[key] for key in ("sha256", "size")},
             "embedded source record does not match source file table")
    no_aliases([entry["name"] for entry in value], "embedded asset name")
    no_aliases([entry["path"] for entry in value], "embedded asset path")
    if target == "ksud":
        prefix = "userspace/ksud/assets/"
        selected = {path[len(prefix):] for path in source_files if path.startswith(prefix)
                    and "/" not in path[len(prefix):] and
                    (path[len(prefix):] in {"installer.sh", "shellrc.sh", "ksuinit"} or path.endswith("_kernelsu.ko"))}
        need({entry["name"] for entry in value} == selected, "embedded source asset set is incomplete")


def version_inputs(value, target):
    if target == "ksuinit":
        need(value is None, "unexpected ksuinit version inputs")
        return
    obj(value, ("environment", "describe", "commit_count", "kernel_status", "ksud_status", "additional_kernel_version_inputs"),
        label="version inputs")
    obj(value["environment"], ("ZYSU_KSUD_VERSION_NAME", "ZYSU_KSUD_VERSION_CODE"), label="version environment")
    for entry in value["environment"].values():
        text_value(entry, "version override", 128, empty=True)
    text_value(value["describe"], "Git describe", 4096)
    need(type(value["commit_count"]) is str and re.fullmatch(r"[0-9]{1,20}", value["commit_count"]), "invalid commit count")
    for key in ("kernel_status", "ksud_status"):
        obj(value[key], ("dirty", "status_sha256"), label="version Git status")
        need(type(value[key]["dirty"]) is bool, "invalid version dirty status")
        sha(value[key]["status_sha256"])
    additional = value["additional_kernel_version_inputs"]
    need(type(additional) is dict and len(additional) <= 32, "too many kernel version inputs")
    for key, entry in additional.items():
        name(key)
        need(key.endswith("_kernelsu.ko"), "invalid additional kernel version input")
        hash_record(entry)
    no_aliases(additional, "additional kernel input")


def validate_snapshot(value, target, expected_commit):
    obj(value, ("schema_version", "kind", "target", "repo", "build_dir", "output_dir", "created_ns",
                "artifacts", "inputs", "inputs_sha256", "limitations"), label="source snapshot")
    need(type(value["schema_version"]) is int and value["schema_version"] == 1 and value["kind"] == "native_source_snapshot",
         "unsupported snapshot schema/kind")
    need(value["target"] == target, "snapshot target mismatch")
    for key in ("repo", "build_dir", "output_dir"):
        observation_path(value[key])
    number(value["created_ns"], "snapshot time", (1 << 63) - 1, 1)
    artifacts = seq(value["artifacts"], 32, "snapshot artifacts", 1)
    for entry in artifacts:
        obj(entry, ("name", "path"), label="snapshot artifact")
        name(entry["name"])
        observation_path(entry["path"])
    no_aliases([entry["name"] for entry in artifacts], "snapshot artifact name")
    no_aliases([entry["path"] for entry in artifacts], "snapshot artifact path")
    need(target in {entry["name"] for entry in artifacts}, "target executable role absent")
    inputs = obj(value["inputs"], ("source", "dependencies", "toolchain", "embedded_assets", "version_inputs"), label="snapshot inputs")
    source = obj(inputs["source"], ("git", "roots", "files", "license_evidence", "tree_sha256", "exclusions"), label="source observation")
    git_observation(source["git"])
    need(source["git"]["commit"] == expected_commit, "source commit differs from externally expected commit")
    for root in seq(source["roots"], 32, "source roots", 1):
        relative(root)
    no_aliases(source["roots"], "source root")
    count = file_table(source["files"])
    need(sha(source["tree_sha256"]) == digest(source["files"]), "source file-table digest mismatch")
    licenses_in_table(source["license_evidence"], source["files"])
    obj(source["exclusions"], ("directories", "files"), label="source exclusions")
    for entries in source["exclusions"].values():
        for item in seq(entries, 128, "source exclusions"):
            text_value(item, "source exclusion", 2048)
    deps = obj(inputs["dependencies"], ("metadata_file", "metadata_sha256", "inactive_declarations", "components"), label="dependencies")
    if target == "ksud":
        observation_path(deps["metadata_file"])
        sha(deps["metadata_sha256"])
    else:
        need(deps["metadata_file"] is None and deps["metadata_sha256"] is None and not deps["components"]
             and not deps["inactive_declarations"], "unexpected ksuinit dependencies")
    for item in seq(deps["inactive_declarations"], 128, "inactive declarations"):
        obj(item, ("name", "expected"), label="inactive dependency")
        name(item["name"])
        need(type(item["expected"]) is dict and item["expected"].get("kind") in {"git", "archive", "vendored"},
             "invalid inactive dependency kind")
        expected_identity(item["expected"], item["expected"]["kind"])
    no_aliases([item["name"] for item in deps["inactive_declarations"]], "inactive dependency name")
    for item in seq(deps["components"], 128, "native components", 1 if target == "ksud" else 0):
        count += dependency(item)
    need(count <= MAX_TOTAL_FILES, "aggregate source-file capacity exceeded")
    no_aliases([item["name"] for item in deps["components"]], "native component name")
    no_aliases([item["source_dir"] for item in deps["components"]], "native source root")
    need(not ({item["name"].casefold() for item in deps["components"]} &
              {item["name"].casefold() for item in deps["inactive_declarations"]}), "active/inactive component overlap")
    toolchain(inputs["toolchain"])
    embedded(inputs["embedded_assets"], source["files"], target)
    version_inputs(inputs["version_inputs"], target)
    need(value["limitations"] == native.LIMITATIONS, "unsupported snapshot scope/limitations")
    need(sha(value["inputs_sha256"]) == digest(inputs), "snapshot inputs digest mismatch")


def _verify_bundle(manifest_path, artifacts, *, expected_target, expected_commit):
    need(expected_target in {"ksud", "ksuinit"}, "unsupported externally expected target")
    commit(expected_commit)
    need(type(artifacts) is dict and 1 <= len(artifacts) <= 32, "explicit artifact mappings are required")
    manifest_path = opened_path(manifest_path)
    need(manifest_path.name == "build-manifest.json", "native manifest must have its canonical filename")
    evidence_dir = manifest_path.parent
    manifest_record, manifest = json_file(manifest_path)
    obj(manifest, ("schema_version", "kind", "target", "created_ns", "snapshot", "inputs_sha256", "source",
                   "source_after", "artifacts", "license_evidence", "sbom", "limitations"), label="native manifest")
    need(type(manifest["schema_version"]) is int and manifest["schema_version"] == 1 and manifest["kind"] == "native_build_manifest",
         "unsupported manifest schema/kind")
    need(manifest["target"] == expected_target, "manifest target mismatch")
    number(manifest["created_ns"], "manifest time", (1 << 63) - 1, 1)
    for key in ("source", "source_after"):
        git_observation(manifest[key])
        need(manifest[key]["commit"] == expected_commit, "manifest commit differs from externally expected commit")
    evidence_files = {"build-manifest.json": manifest_record}
    loaded = {}
    for key, filename in (("snapshot", "source-snapshot.json"), ("sbom", "native-sbom.cdx.json")):
        entry = obj(manifest[key], ("path", "sha256", "size"), label=f"{key} binding")
        need(entry["path"] == filename, f"noncanonical {key} evidence path")
        hash_record({field: entry[field] for field in ("sha256", "size")}, maximum=MAX_JSON)
        record, loaded[key] = json_file(evidence_dir / filename)
        need(record == {field: entry[field] for field in ("sha256", "size")}, f"{key} evidence checksum mismatch")
        evidence_files[filename] = record
    snapshot = loaded["snapshot"]
    validate_snapshot(snapshot, expected_target, expected_commit)
    need(manifest["source"] == snapshot["inputs"]["source"]["git"] and
         manifest["inputs_sha256"] == snapshot["inputs_sha256"], "manifest/snapshot source binding mismatch")
    need(manifest["limitations"] == snapshot["limitations"], "manifest/snapshot limitations mismatch")
    declared = seq(manifest["artifacts"], 32, "manifest artifacts", 1)
    for entry in declared:
        obj(entry, ("name", "path", "sha256", "size"), label="manifest artifact")
        name(entry["name"])
        observation_path(entry["path"])
        hash_record({key: entry[key] for key in ("sha256", "size")}, minimum=1)
    need([{key: item[key] for key in ("name", "path")} for item in declared] == snapshot["artifacts"],
         "manifest artifact identities differ from snapshot")
    need(set(artifacts) == {item["name"] for item in declared}, "all declared artifact roles must be supplied exactly")
    no_aliases(artifacts, "supplied artifact role")
    expected_licenses = native.license_output_records(snapshot)
    evidence_paths = {evidence_dir / filename for filename in
                      [*evidence_files, *(entry["evidence_path"] for entry in expected_licenses)]}
    evidence_identities = set()
    for path in evidence_paths:
        info = opened_path(path).stat()
        identity = (info.st_dev, info.st_ino)
        need(identity not in evidence_identities, "evidence files alias the same filesystem identity")
        evidence_identities.add(identity)
    actual_paths, inode_keys = {}, set()
    for key, path in artifacts.items():
        name(key)
        path = opened_path(path)
        # A portable release can deliberately ship symbols under artifacts/ in
        # the evidence directory. They must never alias a JSON/license evidence
        # entry, and remain explicit mappings rather than inferred old paths.
        need(path not in evidence_paths, "supplied artifact overlaps evidence file")
        info = path.stat()
        inode = (info.st_dev, info.st_ino)
        need(inode not in inode_keys and inode not in evidence_identities, "supplied artifact files alias the same identity")
        inode_keys.add(inode)
        actual_paths[key] = path
    no_aliases([str(path) for path in actual_paths.values()], "supplied artifact path")
    verified = []
    for entry in declared:
        actual = read_file(actual_paths[entry["name"]])
        need(actual == {key: entry[key] for key in ("sha256", "size")}, "relocated artifact checksum mismatch")
        verified.append({"name": entry["name"], **actual})
    seq(manifest["license_evidence"], 128 * 64, "manifest license evidence")
    need(canonical(manifest["license_evidence"]) == canonical(expected_licenses),
         "manifest license identities differ from source tables")
    total_license_bytes = 0
    for entry in expected_licenses:
        path = relative(entry["evidence_path"])
        need(path not in evidence_files, "duplicate evidence path")
        record = read_file(evidence_dir / path, 16 * 1024 * 1024)
        need(record == {key: entry[key] for key in ("sha256", "size")}, "license evidence checksum mismatch")
        total_license_bytes += record["size"]
        need(total_license_bytes <= MAX_LICENSE_BYTES, "license evidence aggregate capacity exceeded")
        evidence_files[path] = record
    no_aliases(evidence_files, "evidence file path")
    need(canonical(loaded["sbom"]) == canonical(native.sbom_for(snapshot, declared, expected_licenses)),
         "SBOM does not match native evidence projection")
    components = [{key: item[key] for key in ("name", "kind", "expected", "git", "local_override", "comparison",
                                             "tree_sha256", "license_evidence_status")}
                  for item in snapshot["inputs"]["dependencies"]["components"]]
    return {"schema_version": 1, "status": "consistent", "target": expected_target,
            "source": manifest["source"], "source_after": manifest["source_after"],
            "source_tree_sha256": snapshot["inputs"]["source"]["tree_sha256"], "components": components,
            "inputs_sha256": snapshot["inputs_sha256"], "manifest": manifest_record, "artifacts": verified,
            "embedded_assets": snapshot["inputs"]["embedded_assets"], "evidence_files": evidence_files,
            "coverage": "portable_evidence_consistency", "authentication": "unsigned_unverified",
            "source_revalidated": False, "toolchain_revalidated": False, "embedded_bytes_verified": False,
            "limitations": [*native.LIMITATIONS, *PORTABLE_LIMITATIONS], "snapshot": snapshot}


def verify_bundle(manifest_path, artifacts, *, expected_target, expected_commit):
    """Validate all declared roles; malformed imported data has a stable error type."""
    try:
        return _verify_bundle(manifest_path, artifacts, expected_target=expected_target, expected_commit=expected_commit)
    except (EvidenceError, OSError):
        raise
    except (TypeError, KeyError, ValueError, RecursionError) as error:
        raise EvidenceError("malformed native evidence structure") from error


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    verify = commands.add_parser("verify")
    verify.add_argument("--manifest", required=True, type=Path)
    verify.add_argument("--target", choices=("ksud", "ksuinit"), required=True)
    verify.add_argument("--commit", required=True)
    verify.add_argument("--artifact", action="append", required=True)
    args = parser.parse_args(argv)
    try:
        mappings = {}
        for item in args.artifact:
            key, separator, path = item.partition("=")
            need(separator and key not in mappings and path, "invalid/duplicate artifact mapping")
            mappings[key] = Path(path)
        result = verify_bundle(args.manifest, mappings, expected_target=args.target, expected_commit=args.commit)
        result.pop("snapshot", None)  # Keep the CLI report small; consumers use the function for validated data.
        print(json.dumps(result, ensure_ascii=False, sort_keys=True))
        return 0
    except (EvidenceError, OSError, TypeError, ValueError, KeyError, RecursionError) as error:
        print(json.dumps({"schema_version": 1, "status": "error", "error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
