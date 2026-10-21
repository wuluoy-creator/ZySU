#!/usr/bin/env python3
"""Bounded, local evidence for configured ksud/ksuinit builds (not attestation).

Configure -> snapshot -> build -> finalize -> verify. Outputs must be new; a
snapshot does not accept existing artifact files. No private key, environment
dump, network access, package installation, or product build is performed here.
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
import subprocess
import sys
import tempfile
import time


SCHEMA = 1
MAX_FILE = 512 * 1024 * 1024
MAX_TREE = 2 * 1024 * 1024 * 1024
MAX_FILES = 50000
MAX_JSON = 64 * 1024 * 1024
MAX_COMMAND = 16 * 1024 * 1024
NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,95}\Z")
SHA = re.compile(r"[0-9a-f]{64}\Z")
GIT_SHA = re.compile(r"[0-9a-f]{40,64}\Z")
SKIP_DIRS = {".git", ".gradle", ".cache", "__pycache__", "node_modules", "out", "dist", "CMakeFiles"}
SECRET_SUFFIXES = {".pem", ".key", ".keystore", ".jks", ".p12", ".pfx"}
SETTINGS = (
    "CMAKE_BUILD_TYPE", "CMAKE_SYSTEM_NAME", "CMAKE_SYSTEM_VERSION", "CMAKE_ANDROID_ARCH_ABI",
    "CMAKE_ANDROID_NDK", "CMAKE_GENERATOR", "CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS",
    "CMAKE_C_FLAGS_RELEASE", "CMAKE_CXX_FLAGS_RELEASE", "CMAKE_EXE_LINKER_FLAGS",
    "CMAKE_EXE_LINKER_FLAGS_RELEASE", "CMAKE_TOOLCHAIN_FILE", "KSUD_DISABLE_LTO",
    "KSUD_UPX", "KSUINIT_UPX", "ZYSU_ENABLE_CLANG_TIDY", "CMAKE_SYSROOT",
)
TOOL_KEYS = (
    "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "CMAKE_LINKER", "CMAKE_AR", "CMAKE_RANLIB",
    "CMAKE_STRIP", "CMAKE_READELF", "CMAKE_MAKE_PROGRAM", "CMAKE_COMMAND",
    "CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER", "_Python3_EXECUTABLE",
    "Python3_EXECUTABLE",
)
LIMITATIONS = [
    "Local unsigned evidence, not a trusted CI attestation or proof of reproducibility.",
    "Partial native source-component SBOM; excludes the complete Android sysroot, Gradle/Manager and kernel dependency graphs.",
    "Tool executable hashes/version text and NDK metadata identify configured tools; the entire sysroot/toolchain installation is not hashed.",
    "Archive URL hashes describe expected downloads; extracted or overridden source trees are fingerprinted separately and are not certified by that hash.",
    "Embedded assets are staged inputs; their presence in the final executable is not independently decoded by this collector.",
    "License files/headers are evidence, not project-wide license conclusions or legal clearance.",
]


class EvidenceError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise EvidenceError(message)


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")


def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def safe_path(value, *, exists=True):
    """Reject symlink/reparse components for source, output and evidence paths."""
    raw = Path(value).expanduser()
    require("\x00" not in str(raw), "NUL in path")
    require(".." not in raw.parts, f"parent traversal is not allowed: {raw}")
    path = Path(os.path.abspath(raw))
    for part in (path, *path.parents):
        if part.exists() or part.is_symlink():
            info = part.lstat()
            require(not stat.S_ISLNK(info.st_mode), f"symlink is not allowed: {part}")
            require(not getattr(info, "st_file_attributes", 0) & 0x400,
                    f"reparse point is not allowed: {part}")
    if exists:
        require(path.exists(), f"missing input: {path}")
    return path


def within(path, root):
    return path == root or root in path.parents


def relative_path(value):
    require(isinstance(value, str) and value and "\\" not in value and "\x00" not in value,
            "invalid relative path")
    path = PurePosixPath(value)
    require(not path.is_absolute() and all(part not in {".", "..", ""} for part in path.parts)
            and str(path) == value and not re.match(r"^[A-Za-z]:", value), "ambiguous relative path")
    return path


def file_record(path, *, limit=MAX_FILE):
    path = safe_path(path)
    before = path.stat()
    require(stat.S_ISREG(before.st_mode), f"not a regular file: {path}")
    require(before.st_size <= limit, f"file exceeds limit: {path}")
    hasher = hashlib.sha256()
    count = 0
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        require((before.st_dev, before.st_ino) == (opened.st_dev, opened.st_ino),
                f"input replaced before read: {path}")
        while chunk := stream.read(1024 * 1024):
            count += len(chunk)
            require(count <= limit, f"file exceeds limit during read: {path}")
            hasher.update(chunk)
    after = path.stat()
    require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
            (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns) and count == after.st_size,
            f"input changed during read: {path}")
    return {"sha256": hasher.hexdigest(), "size": count}


def load_json(path):
    file_record(path, limit=MAX_JSON)
    try:
        return json.loads(Path(path).read_text(encoding="utf-8-sig"), object_pairs_hook=unique_object,
                          parse_constant=lambda value: (_ for _ in ()).throw(EvidenceError(f"invalid JSON number {value}")))
    except (ValueError, UnicodeError) as error:
        raise EvidenceError(f"invalid JSON: {path}: {error}") from error


def write_json_new(path, value):
    path = safe_path(path, exists=False)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False).encode("utf-8") + b"\n")


def command(args, cwd=None, *, allow_failure=False):
    # Temporary files bound memory; poll bounds disk output and execution time.
    with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
        process = subprocess.Popen(args, cwd=cwd, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                   shell=False)
        deadline = time.monotonic() + 30
        try:
            while process.poll() is None:
                require(time.monotonic() < deadline, f"command timed out: {args[0]}")
                require(os.fstat(stdout.fileno()).st_size + os.fstat(stderr.fileno()).st_size <= MAX_COMMAND,
                        f"command output exceeds limit: {args[0]}")
                time.sleep(0.01)
            stdout.seek(0)
            stderr.seek(0)
            result, errors = stdout.read(MAX_COMMAND + 1), stderr.read(MAX_COMMAND + 1)
            require(len(result) + len(errors) <= MAX_COMMAND, f"command output exceeds limit: {args[0]}")
            require(allow_failure or process.returncode == 0,
                    f"command failed ({process.returncode}): {args[0]}: {errors.decode('utf-8', 'replace')[:2000]}")
            return process.returncode, result
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()


def git(repo, *args, allow_failure=False):
    return command(["git", "-C", str(repo), *args], allow_failure=allow_failure)


def git_identity(repo):
    code, raw = git(repo, "rev-parse", "--show-toplevel", allow_failure=True)
    if code:
        return None
    # git invoked inside a plain extracted directory can find the parent repo.
    if Path(os.path.abspath(raw.decode("utf-8").strip())) != repo:
        return None
    _, raw = git(repo, "rev-parse", "HEAD")
    revision = raw.decode("ascii").strip()
    require(GIT_SHA.fullmatch(revision), f"invalid git revision: {repo}")
    _, raw = git(repo, "status", "--porcelain=v1", "-z", "--untracked-files=all")
    require(len(raw) <= MAX_COMMAND, "Git status exceeds limit")
    return {"commit": revision, "dirty": bool(raw), "status_sha256": hashlib.sha256(raw).hexdigest()}


def excluded(relative, *, source=True):
    parts = relative.parts
    if any(part in SKIP_DIRS or part.startswith("build") or part.startswith("cmake-build-") for part in parts[:-1]):
        return True
    name = relative.name
    return (name == ".git" or name == ".env" or name.startswith(".env.") or
            relative.suffix.lower() in SECRET_SUFFIXES or name.endswith((".pyc", ".pyo")) or
            (source and any(part == "third_party" for part in parts)))


def tree_records(root, *, source=False):
    root = safe_path(root)
    require(root.is_dir(), f"not a directory: {root}")
    result = {}
    total = 0
    seen = set()
    for current, dirs, files in os.walk(root, followlinks=False):
        current = Path(current)
        kept = []
        for name in sorted(dirs):
            relative = (current / name).relative_to(root)
            # Append a dummy file so directory exclusions apply to the final part.
            if excluded(relative / "_", source=source):
                continue
            safe_path(current / name)
            kept.append(name)
        dirs[:] = kept
        for name in sorted(files):
            relative = (current / name).relative_to(root)
            if excluded(relative, source=source):
                continue
            key = relative.as_posix()
            require(key.casefold() not in seen, f"case-ambiguous file identity: {key}")
            seen.add(key.casefold())
            record = file_record(current / name)
            total += record["size"]
            require(len(result) < MAX_FILES and total <= MAX_TREE, f"source tree exceeds bounds: {root}")
            result[key] = record
    require(result, f"empty input tree: {root}")
    return dict(sorted(result.items()))


def source_state(repo, target):
    identity = git_identity(repo)
    require(identity is not None, "repository root must be a Git worktree with HEAD")
    roots = [f"userspace/{target}", "userspace/common", "uapi", "cmake", "scripts"]
    optional = [".github/workflows", ".github/actions"]
    files = {}
    for name in roots + optional:
        root = repo / name
        if name in optional and not root.exists():
            continue
        for key, value in tree_records(root, source=True).items():
            files[f"{name}/{key}"] = value
    root_licenses = discover_licenses(repo)
    for name in (".gitmodules", ".github/ci-dependencies.lock.json", "CMakeLists.txt", *root_licenses):
        if (repo / name).exists():
            files[name] = file_record(repo / name)
    require(len(files) <= MAX_FILES and sum(v["size"] for v in files.values()) <= MAX_TREE,
            "first-party source exceeds bounds")
    return {"git": identity, "roots": roots + optional, "files": dict(sorted(files.items())),
            "license_evidence": [{"path": name, **files[name]} for name in root_licenses],
            "tree_sha256": digest(files), "exclusions": {
                "directories": sorted(SKIP_DIRS | {"build*", "cmake-build-*", "third_party (reported separately)"}),
                "files": [".git", ".env", ".env.*", "*.pyc", "*.pyo", *sorted("*" + v for v in SECRET_SUFFIXES)]}}


def discover_licenses(root):
    result = []
    # Exact root-level evidence; do not infer dependency-wide licensing.
    for path in sorted(root.iterdir()):
        if path.is_file() and re.match(r"^(?:LICENSE|COPYING|COPYRIGHT|NOTICE)(?:[._-].*)?$", path.name, re.I):
            result.append(path.name)
    return result


def dependency_record(item):
    require(isinstance(item, dict), "dependency must be an object")
    name = item.get("name")
    require(isinstance(name, str) and NAME.fullmatch(name), "invalid dependency name")
    source_dir = item.get("source_dir")
    require(isinstance(source_dir, str) and Path(source_dir).is_absolute(), "dependency source_dir must be absolute")
    root = safe_path(source_dir)
    require(root.is_dir(), f"missing dependency source: {name}")
    expected = item.get("expected")
    require(isinstance(expected, dict) and expected, f"missing expected identity: {name}")
    kind = item.get("kind")
    require(kind in {"git", "archive", "submodule", "vendored"}, f"unsupported dependency kind: {name}")
    if kind == "git":
        require(isinstance(expected.get("revision"), str) and GIT_SHA.fullmatch(expected["revision"]),
                f"immutable Git revision required: {name}")
    elif kind == "archive":
        require(isinstance(expected.get("archive_sha256"), str) and SHA.fullmatch(expected["archive_sha256"]),
                f"archive SHA-256 required: {name}")
    override = item.get("local_override")
    require(type(override) is bool, f"missing explicit override state: {name}")
    files = tree_records(root)
    identity = git_identity(root)
    licenses = item.get("license_files")
    require(isinstance(licenses, list) and len(licenses) <= 64, "invalid license evidence list")
    require(licenses or item.get("license_evidence_status") == "not_found", "missing explicit license evidence status")
    evidence = []
    for entry in licenses:
        # A helper may include the precomputed checksum; independently read it.
        path_text = entry.get("path") if isinstance(entry, dict) else entry
        relative = relative_path(path_text)
        record = file_record(root / str(relative))
        require(record["size"] <= 16 * 1024 * 1024, "license evidence exceeds bound")
        require(not any(previous["path"].casefold() == path_text.casefold() for previous in evidence),
                "duplicate license evidence")
        evidence.append({"path": path_text, **record})
    commit = expected.get("commit", expected.get("revision"))
    if override:
        comparison = "local_override_not_verified_against_expected_identity"
    elif commit and identity:
        comparison = "git_commit_matches" if commit == identity["commit"] else "git_commit_mismatch"
        require(commit == identity["commit"], f"dependency commit mismatch: {name}")
    elif expected.get("archive_sha256") or expected.get("sha256"):
        comparison = "archive_expected_only_extracted_tree_fingerprinted"
    else:
        comparison = "expected_identity_recorded_actual_tree_fingerprinted"
    return {"name": name, "source_dir": str(root), "kind": kind, "expected": expected,
            "resolution": item.get("resolution", item.get("kind", "unspecified")),
            "local_override": override, "comparison": comparison, "git": identity,
            "files": files, "tree_sha256": digest(files), "license_evidence": evidence,
            "license_evidence_status": "files_recorded" if evidence else "not_found"}


def dependencies_state(repo, target, metadata_path):
    records = []
    metadata = None
    if target == "ksud":
        require(metadata_path is not None, "ksud requires configured dependency metadata")
        metadata = load_json(metadata_path)
        require(isinstance(metadata, dict) and metadata.get("schema_version") == SCHEMA,
                "unsupported dependency metadata schema")
        items = metadata.get("dependencies")
        require(isinstance(items, list) and 1 <= len(items) <= 64, "missing/bounded dependency records required")
        records.extend(dependency_record(item) for item in items)
        _, raw = git(repo, "ls-files", "--stage", "-z", "--", "userspace/ksud/third_party")
        for entry in raw.split(b"\0"):
            if not entry:
                continue
            header, name = entry.split(b"\t", 1)
            mode, revision, stage = header.decode("ascii").split()
            if mode != "160000":
                continue
            require(stage == "0", "unmerged submodule entry")
            relative = relative_path(name.decode("utf-8"))
            root = safe_path(repo / str(relative))
            identity = git_identity(root)
            require(identity is not None, f"uninitialized submodule: {relative}")
            records.append(dependency_record({"name": "submodule-" + root.name, "source_dir": str(root),
                "kind": "submodule", "resolution": "submodule", "expected": {"gitlink": revision,
                "path": str(relative)}, "local_override": identity["commit"] != revision,
                "license_files": discover_licenses(root), "license_evidence_status": "not_found"}))
    else:
        require(metadata_path is None, "ksuinit has no configured third-party dependency metadata")
    names, paths = set(), set()
    for record in records:
        require(record["name"].casefold() not in names, "duplicate dependency name")
        require(record["source_dir"].casefold() not in paths, "duplicate dependency source root")
        names.add(record["name"].casefold())
        paths.add(record["source_dir"].casefold())
    return {"metadata_file": str(metadata_path) if metadata_path else None,
            "metadata_sha256": file_record(metadata_path)["sha256"] if metadata_path else None,
            "inactive_declarations": metadata.get("inactive_declarations", []) if metadata else [],
            "components": sorted(records, key=lambda item: item["name"])}


def cache_values(build):
    path = build / "CMakeCache.txt"
    file_record(path, limit=4 * 1024 * 1024)
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith(("#", "//")) or "=" not in line or ":" not in line.split("=", 1)[0]:
            continue
        key, value = line.split("=", 1)
        key = key.split(":", 1)[0]
        require(key not in result, f"duplicate CMake cache key: {key}")
        result[key] = value
    return result


def tool_record(value, previous=None):
    require(isinstance(value, str) and value and ";" not in value, "ambiguous configured tool command")
    located = shutil.which(value) if not Path(value).is_absolute() else value
    require(located is not None, f"configured tool not found: {value}")
    invocation = Path(os.path.abspath(located))
    # NDK/Unix tool names often intentionally symlink to one executable. Resolve
    # these explicitly and hash the final target; source/output links are rejected.
    resolved = invocation.resolve(strict=True)
    record = file_record(resolved)
    links = []
    current = invocation
    for _ in range(32):
        if not current.is_symlink():
            break
        target = os.readlink(current)
        links.append({"path": str(current), "target": target})
        current = Path(os.path.abspath(current.parent / target)) if not Path(target).is_absolute() else Path(target)
    else:
        raise EvidenceError("tool symlink chain exceeds bound")
    if previous is None:
        _, version = command([str(invocation), "--version"])
        require(len(version) <= 64 * 1024, "tool version output exceeds bound")
        version_text = version.decode("utf-8", "replace").strip()
    else:
        # Revalidation never executes a tool selected by an imported snapshot.
        # Changed executable bytes/path/link identity are rejected by comparison.
        version_text = previous["version"]
    return {"invocation": str(invocation), "resolved_path": str(resolved), "symlink_chain": links, **record,
            "version": version_text}


def toolchain_state(build, target, previous_tools=None):
    cache_values(build)  # Validate the separately fingerprinted cache structure.
    metadata_path = build / "native-toolchain.json"
    metadata = load_json(metadata_path)
    require(isinstance(metadata, dict) and metadata.get("schema_version") == SCHEMA and metadata.get("target") == target,
            "unsupported/mismatched configured toolchain metadata")
    values = metadata.get("values")
    require(isinstance(values, dict) and all(key in SETTINGS + TOOL_KEYS and isinstance(value, str)
                                           for key, value in values.items()), "invalid configured toolchain values")
    require(values.get("CMAKE_CXX_COMPILER"), "actual configured C++ compiler metadata is required")
    # All reported settings/tools come from actual configured variables. An
    # absent/empty normal launcher must not fall back to a stale cache entry.
    cache = values
    require(cache.get("CMAKE_CXX_COMPILER"), "configured C++ compiler is required")
    result = {"settings": {key: cache[key] for key in SETTINGS if key in cache}, "tools": {}, "files": {}}
    for key in TOOL_KEYS:
        if cache.get(key):
            result["tools"][key] = tool_record(cache[key], previous_tools.get(key, {"version": ""}) if previous_tools is not None else None)
    result["tools"]["provenance_python"] = tool_record(sys.executable, previous_tools.get("provenance_python") if previous_tools is not None else None)
    for name in ("CMakeCache.txt", "native-toolchain.json", "compile_commands.json"):
        path = build / name
        if path.exists():
            result["files"][str(path)] = file_record(path)
    result["files"].update(ninja_graph(build))
    for path in sorted((build / "CMakeFiles").glob("*/CMake*Compiler.cmake")):
        result["files"][str(path)] = file_record(path)
    for key in ("CMAKE_TOOLCHAIN_FILE",):
        if cache.get(key):
            path = Path(cache[key])
            if not path.is_absolute():
                path = build / path
            result["files"][str(safe_path(path))] = file_record(path)
    if cache.get("CMAKE_ANDROID_NDK"):
        root = safe_path(cache["CMAKE_ANDROID_NDK"])
        path = root / "source.properties"
        result["files"][str(path)] = file_record(path)
        require(path.stat().st_size <= 65536, "NDK identity metadata exceeds bound")
        result["ndk_source_properties"] = path.read_text(encoding="utf-8")
    return result


def ninja_graph(build):
    """Bind the actual included compile/link rules, without executing Ninja."""
    pending = [build / "build.ninja"]
    result = {}
    while pending:
        path = safe_path(pending.pop())
        require(within(path, build), "Ninja include escapes build directory")
        if str(path) in result:
            continue
        require(len(result) < 128, "Ninja include count exceeds bound")
        result[str(path)] = file_record(path, limit=16 * 1024 * 1024)
        for line in path.read_text(encoding="utf-8").splitlines():
            match = re.match(r"^(?:include|subninja)\s+(.+)$", line)
            if not match:
                continue
            raw = match.group(1).rstrip()
            # Generated CMake Ninja includes use literal paths. Support Ninja's
            # literal dollar/space/colon escapes; refuse variable-expanded or
            # continued include expressions instead of silently omitting them.
            decoded = []
            index = 0
            while index < len(raw):
                char = raw[index]
                if char == "$":
                    require(index + 1 < len(raw) and raw[index + 1] in "$ :", "unsupported dynamic Ninja include")
                    index += 1
                    char = raw[index]
                decoded.append(char)
                index += 1
            include = Path("".join(decoded))
            require(not include.is_absolute() and ".." not in include.parts, "ambiguous Ninja include path")
            pending.append(build / include)
    return dict(sorted(result.items()))


def embedded_assets(repo, target):
    if target != "ksud":
        return []
    root = repo / "userspace/ksud/assets"
    require(root.is_dir(), "ksud assets directory is required")
    result = []
    for path in sorted(root.iterdir()):
        if path.name in {"installer.sh", "shellrc.sh", "ksuinit"} or path.name.endswith("_kernelsu.ko"):
            item = {"name": path.name, "path": str(path), **file_record(path), "kind": "runtime_asset"}
            if path.name.endswith("_kernelsu.ko"):
                kmi = path.name.removesuffix("_kernelsu.ko")
                match = re.fullmatch(r"(?:android\d+-)?(\d+)\.(\d+)", kmi)
                require(match is not None and tuple(map(int, match.groups())) >= (6, 1), "unsupported embedded KMI")
                item.update(kind="kernel_module", kmi=kmi)
            elif path.name == "ksuinit":
                item["kind"] = "ksuinit"
            result.append(item)
    return result


def capture(repo, target, build, metadata, previous_tools=None):
    return {"source": source_state(repo, target), "dependencies": dependencies_state(repo, target, metadata),
            "toolchain": toolchain_state(build, target, previous_tools), "embedded_assets": embedded_assets(repo, target),
            "version_inputs": version_inputs(repo, target)}


def version_inputs(repo, target):
    if target != "ksud":
        return None
    result = {"environment": {name: os.environ.get(name, "") for name in
              ("ZYSU_KSUD_VERSION_NAME", "ZYSU_KSUD_VERSION_CODE")}}
    require(all(len(value) <= 128 for value in result["environment"].values()), "version override exceeds bound")
    for label, args in (("describe", ("describe", "--tags", "--always", "--abbrev=8")),
                        ("commit_count", ("rev-list", "--count", "HEAD"))):
        _, raw = git(repo, *args)
        result[label] = raw.decode("utf-8").strip()
    # generate_version.py uses these statuses, including kernel dirty state, and
    # scans top-level out/*.ko as well as staged assets for the -nc marker.
    for label, paths in (("kernel_status", ("kernel", "uapi")), ("ksud_status", ("userspace/ksud",))):
        _, raw = git(repo, "-c", "core.autocrlf=true", "status", "--porcelain", "--untracked-files=normal", "--", *paths)
        result[label] = {"dirty": bool(raw.strip()), "status_sha256": hashlib.sha256(raw).hexdigest()}
    result["additional_kernel_version_inputs"] = {
        path.name: file_record(path) for path in sorted((repo / "out").glob("*_kernelsu.ko"))}
    return result


def compare_inputs(expected, actual):
    # Unrelated docs/Manager edits may change global Git dirty status. Record the
    # actual before/after status while guarding relevant content and root commit.
    before, after = json.loads(json.dumps(expected)), json.loads(json.dumps(actual))
    for item in (before, after):
        item["source"]["git"].pop("dirty", None)
        item["source"]["git"].pop("status_sha256", None)
    require(before == after, "relevant source, dependency, build configuration, assets or toolchain changed since snapshot")


def snapshot(args):
    repo, build = safe_path(args.repo), safe_path(args.build_dir)
    require(repo.is_dir() and build.is_dir(), "repo and build-dir must be directories")
    out = safe_path(args.out, exists=False)
    metadata = safe_path(args.dependencies) if args.dependencies else None
    # Output under an excluded build directory is fine, under source is not.
    for name in (f"userspace/{args.target}", "userspace/common", "uapi", "cmake", "scripts", ".github"):
        root = repo / name
        if within(out, root):
            require(any(part.startswith("build") or part in SKIP_DIRS for part in out.relative_to(root).parts),
                    "evidence output overlaps snapshotted sources")
    require(not out.exists() or not any(out.iterdir()), "evidence output directory must be empty")
    artifacts = []
    names, paths = set(), set()
    for item in args.artifact:
        require("=" in item, "artifact must be NAME=PATH")
        name, value = item.split("=", 1)
        require(NAME.fullmatch(name) and name.casefold() not in names, "invalid or duplicate artifact name")
        path = safe_path(value, exists=False)
        require(within(path, build) and not within(path, out), "artifact must be inside build-dir and outside evidence output")
        require(not path.exists(), f"stale artifact exists before snapshot: {path}")
        require(str(path).casefold() not in paths, "duplicate artifact path")
        names.add(name.casefold())
        paths.add(str(path).casefold())
        artifacts.append({"name": name, "path": str(path)})
    require(artifacts and len(artifacts) <= 32, "one to 32 required artifacts must be declared")
    inputs = capture(repo, args.target, build, metadata)
    created = time.time_ns()
    value = {"schema_version": SCHEMA, "kind": "native_source_snapshot", "target": args.target,
             "repo": str(repo), "build_dir": str(build), "output_dir": str(out),
             "created_ns": created, "artifacts": artifacts, "inputs": inputs,
             "inputs_sha256": digest(inputs), "limitations": LIMITATIONS}
    write_json_new(out / "source-snapshot.json", value)
    return {"schema_version": SCHEMA, "status": "snapshotted", "snapshot": str(out / "source-snapshot.json"),
            "inputs_sha256": value["inputs_sha256"]}


def checked_snapshot(path):
    snapshot_path = safe_path(path)
    value = load_json(snapshot_path)
    require(isinstance(value, dict), "snapshot must be an object")
    require(value.get("schema_version") == SCHEMA and value.get("kind") == "native_source_snapshot",
            "unsupported snapshot schema/kind")
    require(value.get("target") in {"ksud", "ksuinit"}, "invalid snapshot target")
    require(value.get("inputs_sha256") == digest(value["inputs"]), "snapshot input digest mismatch")
    require(safe_path(value["output_dir"]) == snapshot_path.parent, "snapshot moved from declared output directory")
    return value


def current_for(value):
    metadata = value["inputs"]["dependencies"]["metadata_file"]
    return capture(safe_path(value["repo"]), value["target"], safe_path(value["build_dir"]),
                   safe_path(metadata) if metadata else None, value["inputs"]["toolchain"]["tools"])


def sbom_for(snapshot_value, artifacts, licenses):
    source = snapshot_value["inputs"]["source"]
    components = [{"type": "application", "bom-ref": "zysu:" + snapshot_value["target"],
        "name": "ZySU " + snapshot_value["target"], "version": source["git"]["commit"],
        "properties": [{"name": "zysu:source-tree-sha256", "value": source["tree_sha256"]},
                       {"name": "zysu:checkout-dirty", "value": str(source["git"]["dirty"]).lower()},
                       {"name": "zysu:license-evidence", "value": canonical([
                           item for item in licenses if item["component"] == "ZySU-" + snapshot_value["target"]]).decode()}]}]
    for dependency in snapshot_value["inputs"]["dependencies"]["components"]:
        properties = [{"name": "zysu:expected-identity", "value": canonical(dependency["expected"]).decode()},
                      {"name": "zysu:comparison", "value": dependency["comparison"]},
                      {"name": "zysu:local-override", "value": str(dependency["local_override"]).lower()},
                      {"name": "zysu:source-tree-sha256", "value": dependency["tree_sha256"]},
                      {"name": "zysu:license-evidence", "value": canonical([item for item in licenses if item["component"] == dependency["name"]]).decode()}]
        item = {"type": "library", "bom-ref": "native:" + dependency["name"], "name": dependency["name"],
                "properties": properties}
        if dependency["git"]:
            item["version"] = dependency["git"]["commit"]
        components.append(item)
    for artifact in artifacts:
        components.append({"type": "file", "bom-ref": "artifact:" + artifact["name"], "name": artifact["name"],
                           "hashes": [{"alg": "SHA-256", "content": artifact["sha256"]}]})
    return {"bomFormat": "CycloneDX", "specVersion": "1.5", "version": 1,
            "metadata": {"component": components[0], "properties": [
                {"name": "zysu:coverage", "value": "partial-native-source-components"},
                {"name": "zysu:limitations", "value": canonical(LIMITATIONS).decode()}]},
            "components": components[1:]}


def license_components(value):
    result = list(value["inputs"]["dependencies"]["components"])
    if value["inputs"]["source"]["license_evidence"]:
        result.append({"name": "ZySU-" + value["target"], "source_dir": value["repo"],
                       "license_evidence": value["inputs"]["source"]["license_evidence"]})
    return result


def license_output_records(value):
    result = []
    for component in license_components(value):
        for index, evidence in enumerate(component["license_evidence"]):
            result.append({"component": component["name"], "source_relative_path": evidence["path"],
                           "evidence_path": f"licenses/{component['name']}/{index:02d}-{Path(evidence['path']).name}",
                           "sha256": evidence["sha256"], "size": evidence["size"]})
    return result


def finalize(args):
    value = checked_snapshot(args.snapshot)
    out = safe_path(value["output_dir"])
    for name in ("build-manifest.json", "native-sbom.cdx.json", "licenses"):
        require(not (out / name).exists(), f"final output already exists: {name}")
    actual = current_for(value)
    compare_inputs(value["inputs"], actual)
    artifacts = []
    for item in value["artifacts"]:
        path = safe_path(item["path"])
        require(within(path, safe_path(value["build_dir"])), "artifact escaped build directory")
        record = file_record(path)
        require(record["size"] > 0, f"empty artifact: {path}")
        # Snapshot absence is the freshness guard; mtime is extra evidence only,
        # since copied artifacts may intentionally preserve an earlier mtime.
        artifacts.append({**item, **record})
    licenses = []
    # First-party license texts are also exact evidence, with no inferred SPDX
    # expression. Source files under each dependency retain their own evidence.
    for dependency in license_components(value):
        for index, evidence in enumerate(dependency["license_evidence"]):
            source = safe_path(Path(dependency["source_dir"]) / str(relative_path(evidence["path"])))
            require(file_record(source) == {key: evidence[key] for key in ("sha256", "size")}, "license evidence changed")
            relative = f"licenses/{dependency['name']}/{index:02d}-{Path(evidence['path']).name}"
            destination = safe_path(out / relative, exists=False)
            destination.parent.mkdir(parents=True, exist_ok=True)
            with destination.open("xb") as output, source.open("rb") as input_file:
                shutil.copyfileobj(input_file, output, 1024 * 1024)
            require(file_record(destination) == file_record(source), "license evidence copy mismatch")
            licenses.append({"component": dependency["name"], "source_relative_path": evidence["path"],
                             "evidence_path": relative, "sha256": evidence["sha256"], "size": evidence["size"]})
    sbom = sbom_for(value, artifacts, licenses)
    write_json_new(out / "native-sbom.cdx.json", sbom)
    manifest = {"schema_version": SCHEMA, "kind": "native_build_manifest", "target": value["target"],
                "created_ns": time.time_ns(), "snapshot": {"path": "source-snapshot.json", **file_record(args.snapshot)},
                "inputs_sha256": value["inputs_sha256"], "source": value["inputs"]["source"]["git"],
                "source_after": actual["source"]["git"], "artifacts": artifacts, "license_evidence": licenses,
                "sbom": {"path": "native-sbom.cdx.json", **file_record(out / "native-sbom.cdx.json")},
                "limitations": LIMITATIONS}
    # Validate inputs a second time after copying evidence. A build running
    # concurrently with finalization is a caller error, never a silent success.
    compare_inputs(value["inputs"], current_for(value))
    write_json_new(out / "build-manifest.json", manifest)
    return {"schema_version": SCHEMA, "status": "finalized", "manifest": str(out / "build-manifest.json"),
            "artifacts": len(artifacts), "native_components": len(value["inputs"]["dependencies"]["components"])}


def verify(args):
    path = safe_path(args.manifest)
    manifest = load_json(path)
    require(isinstance(manifest, dict), "manifest must be an object")
    require(manifest.get("schema_version") == SCHEMA and manifest.get("kind") == "native_build_manifest",
            "unsupported manifest schema/kind")
    out = path.parent
    for key in ("snapshot", "sbom"):
        entry = manifest[key]
        file_path = out / str(relative_path(entry["path"]))
        require(file_record(file_path) == {name: entry[name] for name in ("sha256", "size")}, f"{key} checksum mismatch")
    value = checked_snapshot(out / manifest["snapshot"]["path"])
    require(manifest["target"] == value["target"] and manifest["inputs_sha256"] == value["inputs_sha256"],
            "manifest/snapshot mismatch")
    require(manifest["source"] == value["inputs"]["source"]["git"], "manifest source mismatch")
    require([{key: item[key] for key in ("name", "path")} for item in manifest["artifacts"]] == value["artifacts"],
            "manifest artifact identities do not match snapshot")
    for item in manifest["artifacts"]:
        require(file_record(item["path"]) == {key: item[key] for key in ("sha256", "size")}, "artifact checksum mismatch")
    require(manifest["license_evidence"] == license_output_records(value), "manifest license evidence identities differ from snapshot")
    for item in manifest["license_evidence"]:
        evidence = out / str(relative_path(item["evidence_path"]))
        require(file_record(evidence) == {key: item[key] for key in ("sha256", "size")}, "license evidence checksum mismatch")
    expected_sbom = sbom_for(value, manifest["artifacts"], manifest["license_evidence"])
    require(load_json(out / manifest["sbom"]["path"]) == expected_sbom, "SBOM content does not match manifest inputs")
    compare_inputs(value["inputs"], current_for(value))
    return {"schema_version": SCHEMA, "status": "verified", "manifest": str(path),
            "sha256": file_record(path)["sha256"], "coverage": "partial-native-source-components"}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    first = commands.add_parser("snapshot")
    first.add_argument("--repo", required=True)
    first.add_argument("--target", choices=("ksud", "ksuinit"), required=True)
    first.add_argument("--build-dir", required=True)
    first.add_argument("--out", required=True)
    first.add_argument("--artifact", action="append", required=True)
    first.add_argument("--dependencies")
    final = commands.add_parser("finalize")
    final.add_argument("--snapshot", required=True)
    check = commands.add_parser("verify")
    check.add_argument("--manifest", required=True)
    args = parser.parse_args(argv)
    try:
        result = {"snapshot": snapshot, "finalize": finalize, "verify": verify}[args.command](args)
        print(json.dumps(result, ensure_ascii=False, sort_keys=True))
        return 0
    except (EvidenceError, OSError, KeyError, TypeError, ValueError, RecursionError) as error:
        print(json.dumps({"schema_version": SCHEMA, "status": "error", "error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
