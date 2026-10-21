#!/usr/bin/env python3
"""Translate the native lock to CMake and record resolved source locations.

This is configure-time metadata, not an attestation. In particular URL_HASH
checks a downloaded archive, never an arbitrary FETCHCONTENT_SOURCE_DIR override.
Source identities and license file contents must be checked by provenance tooling.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import tempfile


class DependencyError(ValueError):
    pass


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise DependencyError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _text(value, expression, label):
    if not isinstance(value, str) or not re.fullmatch(expression, value):
        raise DependencyError(f"invalid {label}")
    return value


def _relative(value):
    _text(value, r"[A-Za-z0-9_./+-]+", "relative path")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or str(path) != value:
        raise DependencyError(f"ambiguous relative path: {value}")
    return value


def load_lock(path):
    path = Path(path)
    if path.stat().st_size > 65536:
        raise DependencyError("dependency lock exceeds 64 KiB")
    raw = path.read_bytes()
    lock = json.loads(raw, object_pairs_hook=_unique_object)
    if not isinstance(lock, dict) or set(lock) != {"schema_version", "dependencies"}:
        raise DependencyError("invalid dependency lock fields")
    if type(lock["schema_version"]) is not int or lock["schema_version"] != 1:
        raise DependencyError("unsupported dependency lock schema")
    dependencies = lock["dependencies"]
    if not isinstance(dependencies, list) or not 1 <= len(dependencies) <= 16:
        raise DependencyError("invalid dependency lock count")
    names = set()
    for dep in dependencies:
        if not isinstance(dep, dict):
            raise DependencyError("invalid dependency entry")
        name = _text(dep.get("name"), r"[a-z][a-z0-9_]*", "dependency name")
        if name in names:
            raise DependencyError(f"duplicate dependency: {name}")
        names.add(name)
        common = {"name", "kind", "license_files"}
        kind = dep.get("kind")
        if kind == "archive":
            required = common | {"url", "archive_sha256"}
            _text(dep.get("url"), r"https://[A-Za-z0-9/._+-]+", "archive URL")
            _text(dep.get("archive_sha256"), r"[0-9a-f]{64}", "archive SHA256")
        elif kind in {"git", "vendored"}:
            required = common | {"repository", "commit"}
            _text(dep.get("repository"), r"https://[A-Za-z0-9/._+-]+", "Git URL")
            _text(dep.get("commit"), r"[0-9a-f]{40}", "Git commit")
            if kind == "vendored":
                required |= {"gitlink_path", "source_subdir"}
                _relative(dep.get("gitlink_path"))
                _relative(dep.get("source_subdir"))
        else:
            raise DependencyError(f"unsupported dependency kind: {kind}")
        if not required <= set(dep) or set(dep) - required - {"version"}:
            raise DependencyError(f"invalid fields for dependency: {name}")
        if "version" in dep:
            _text(dep["version"], r"[A-Za-z0-9._+-]+", "version")
        licenses = dep["license_files"]
        if not isinstance(licenses, list) or len(licenses) > 32:
            raise DependencyError("invalid license candidates")
        if len(set(licenses)) != len(licenses):
            raise DependencyError("duplicate license candidate")
        for license_path in licenses:
            _relative(license_path)
    return lock, hashlib.sha256(raw).hexdigest()


def _atomic_write(path, content):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(content)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def write_declarations(lock_path, output):
    lock, digest = load_lock(lock_path)
    lines = ["# Generated from native-dependencies.lock.json; do not edit.",
             f"# lock SHA256: {digest}"]
    for dep in lock["dependencies"]:
        if dep["kind"] == "vendored":
            continue
        lines.append(f"FetchContent_Declare({dep['name']}")
        if dep["kind"] == "archive":
            lines += [f"  URL {dep['url']}", f"  URL_HASH SHA256={dep['archive_sha256']}"]
        else:
            # A complete commit identity cannot reliably be fetched with a shallow clone.
            lines += [f"  GIT_REPOSITORY {dep['repository']}",
                      f"  GIT_TAG {dep['commit']}", "  GIT_SHALLOW FALSE"]
        lines.append(")")
    _atomic_write(output, "\n".join(lines) + "\n")


def _path_without_links(value, directory=True):
    path = Path(value)
    if not path.is_absolute() or any(char in str(path) for char in "\r\n;"):
        raise DependencyError("source paths must be unambiguous absolute paths")
    if ".." in path.parts:
        raise DependencyError("source path contains parent traversal")
    for item in (path, *path.parents):
        info = item.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise DependencyError(f"source path uses a symlink/reparse point: {item}")
    if directory and not path.is_dir():
        raise DependencyError(f"missing dependency source directory: {path}")
    if not directory and not path.is_file():
        raise DependencyError(f"missing license evidence file: {path}")
    return path.resolve()


def _assignments(values):
    result = {}
    for value in values:
        name, separator, data = value.partition("=")
        if not separator or name in result:
            raise DependencyError("missing or duplicate dependency assignment")
        result[name] = data
    return result


def _boolean(value):
    if value.upper() not in {"ON", "OFF", "TRUE", "FALSE", "1", "0"}:
        raise DependencyError(f"invalid CMake boolean: {value}")
    return value.upper() in {"ON", "TRUE", "1"}


def write_metadata(lock_path, output, source_values, override_values,
                   fully_disconnected="OFF", updates_values=()):
    lock, digest = load_lock(lock_path)
    sources = _assignments(source_values)
    overrides = _assignments(override_values)
    updates = _assignments(updates_values)
    locked = {dep["name"]: dep for dep in lock["dependencies"]}
    if not sources or set(sources) - set(locked):
        raise DependencyError("missing or unknown dependency sources")
    fetched = {name for name in sources if locked[name]["kind"] != "vendored"}
    if set(overrides) != fetched or set(updates) != fetched:
        raise DependencyError("each FetchContent source needs explicit override/update status")
    metadata = {
        "schema_version": 1,
        "lock_file": str(Path(lock_path).resolve()),
        "lock_sha256": digest,
        "scope": "ksud FetchContent and selected vendored LZ4; submodule graphs are recorded separately",
        "dependencies": [],
        "inactive_declarations": [],
    }
    used_paths = set()
    for name, dep in locked.items():
        expected = {key: value for key, value in dep.items() if key not in {"name", "license_files"}}
        if "commit" in expected:
            expected["revision"] = expected.pop("commit")
        if name not in sources:
            metadata["inactive_declarations"].append({"name": name, "expected": expected})
            continue
        path = _path_without_links(sources[name])
        path_key = os.path.normcase(str(path))
        if path_key in used_paths:
            raise DependencyError("duplicate resolved dependency source directory")
        used_paths.add(path_key)
        licenses = []
        for relative in dep["license_files"]:
            candidate = path / relative
            if candidate.exists() or candidate.is_symlink():
                _path_without_links(str(candidate), directory=False)
                licenses.append(relative)
        is_fetch = name in fetched
        metadata["dependencies"].append({
            "name": name,
            "kind": dep["kind"],
            "source_dir": str(path),
            "resolution": "fetchcontent" if is_fetch else "vendored",
            "expected": expected,
            "local_override": _boolean(overrides[name]) if is_fetch else False,
            "override_variable": "FETCHCONTENT_SOURCE_DIR_" + name.upper() if is_fetch else None,
            "fetchcontent_fully_disconnected": _boolean(fully_disconnected) if is_fetch else False,
            "fetchcontent_updates_disconnected": _boolean(updates[name]) if is_fetch else False,
            "license_files": licenses,
            "license_evidence_status": "files_listed" if licenses else "not_found",
        })
    _atomic_write(output, json.dumps(metadata, indent=2, ensure_ascii=False) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("declarations", "metadata"))
    parser.add_argument("--lock", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--source", action="append", default=[])
    parser.add_argument("--override", action="append", default=[])
    parser.add_argument("--fully-disconnected", default="OFF")
    parser.add_argument("--updates-disconnected", action="append", default=[])
    args = parser.parse_args()
    # Never leave an older success record after a rejected configure invocation.
    output = Path(args.output)
    try:
        if output.exists():
            output.unlink()
        if args.mode == "declarations":
            write_declarations(args.lock, output)
        else:
            write_metadata(args.lock, output, args.source, args.override,
                           args.fully_disconnected, args.updates_disconnected)
        return 0
    except (DependencyError, OSError, ValueError, TypeError) as error:
        print(f"native dependency metadata error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
