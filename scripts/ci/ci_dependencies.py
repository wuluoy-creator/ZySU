#!/usr/bin/env python3
"""Check locked workflow action identities and select a locked DDK image offline."""

import argparse
import json
import os
import re
import sys
from pathlib import Path


ACTION = r"[A-Za-z0-9_-]+/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*"
SHA = r"[0-9a-f]{40}"
DDK_IMAGE_EXPRESSION = "${{ needs.resolve-image.outputs.image }}"


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key: " + key)
        result[key] = value
    return result


def fields(value, names):
    if not isinstance(value, dict) or set(value) != set(names):
        raise ValueError("unexpected lock fields")


def load_lock(path):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 128 * 1024:
        raise ValueError("invalid CI lock file")
    value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object)
    fields(value, ("schema_version", "observed_at", "actions", "unresolved_actions", "ddk_images"))
    if type(value["schema_version"]) is not int or value["schema_version"] != 1:
        raise ValueError("unsupported CI lock schema")
    if not isinstance(value["observed_at"], str) or not value["observed_at"]:
        raise ValueError("missing observation date")
    for name in ("actions", "unresolved_actions", "ddk_images"):
        if not isinstance(value[name], list) or len(value[name]) > 128:
            raise ValueError("invalid lock list: " + name)
    actions, selectors = set(), set()
    for item in value["actions"]:
        fields(item, ("action", "version", "commit"))
        if not all(isinstance(v, str) for v in item.values()) or not re.fullmatch(ACTION, item["action"]) or not re.fullmatch(SHA, item["commit"]):
            raise ValueError("invalid immutable Action identity")
        selector = (item["action"], item["version"])
        if not item["version"] or (item["action"], item["commit"]) in actions or selector in selectors:
            raise ValueError("empty or duplicate Action identity")
        actions.add((item["action"], item["commit"]))
        selectors.add(selector)
    unresolved = set()
    for item in value["unresolved_actions"]:
        fields(item, ("action", "version", "reason"))
        if not all(isinstance(v, str) and v for v in item.values()) or not re.fullmatch(ACTION, item["action"]):
            raise ValueError("invalid unresolved Action")
        key = (item["action"], item["version"])
        if key in unresolved or key in selectors:
            raise ValueError("duplicate unresolved Action")
        unresolved.add(key)
    images = set()
    for item in value["ddk_images"]:
        fields(item, ("kmi", "release", "repository", "digest", "platform"))
        if not all(isinstance(v, str) for v in item.values()) or not re.fullmatch(r"android[0-9]+-[0-9]+\.[0-9]+", item["kmi"]) or not re.fullmatch(r"[0-9]{8}", item["release"]):
            raise ValueError("invalid DDK selector")
        if item["repository"] != "ghcr.io/ylarod/ddk-min" or not re.fullmatch(r"sha256:[0-9a-f]{64}", item["digest"]) or item["platform"] != "linux/amd64":
            raise ValueError("invalid immutable DDK image")
        key = (item["kmi"], item["release"])
        if key in images:
            raise ValueError("duplicate DDK selector")
        images.add(key)
    if not actions or not images:
        raise ValueError("empty immutable dependency inventory")
    return value


def resolve_ddk(lock, kmi, release):
    for item in lock["ddk_images"]:
        if (item["kmi"], item["release"]) == (kmi, release):
            return item["repository"] + "@" + item["digest"]
    raise ValueError("DDK version is not locked; review and add its digest before use")


def check_workflows(repo, lock):
    """Validate simple uses scalars; actionlint remains the YAML/schema validator."""
    pinned = {(a["action"], a["commit"]) for a in lock["actions"]}
    unresolved = {(a["action"], a["version"]): a["reason"] for a in lock["unresolved_actions"]}
    findings, count = [], 0
    for path in sorted(p for p in (repo / ".github").rglob("*") if p.suffix in {".yml", ".yaml"}):
        if path.is_symlink() or path.stat().st_size > 1024 * 1024:
            raise ValueError("invalid workflow file")
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            match = re.match(r"^\s*(?:-\s*)?uses:\s*(.*?)\s*(?:#.*)?$", line)
            if not match:
                # The policy requires a standalone uses scalar. Fail explicitly
                # on inline/quoted YAML keys instead of silently skipping them.
                if not line.lstrip().startswith("#") and re.search(r"(?<![\w])['\"]?uses['\"]?\s*:", line):
                    raise ValueError("unsupported uses layout; use a standalone scalar")
                continue
            use = match[1].strip().strip("\"'")
            location = f"{path.relative_to(repo).as_posix()}:{number}"
            if use.startswith("./"):
                target = repo / use
                if ".." in Path(use).parts or not target.exists() or target.is_symlink() or not target.resolve().is_relative_to(repo.resolve()):
                    raise ValueError("invalid local action at " + location)
                continue
            count += 1
            if use.count("@") != 1:
                raise ValueError("unsupported Action expression at " + location)
            action, ref = use.split("@")
            if (action, ref) in pinned:
                continue
            # Existing issue-comment integration cannot currently be resolved.
            # Exception is exact, visible, and never applies to build/release.
            if (action, ref) in unresolved and path.relative_to(repo).as_posix() == ".github/workflows/add-device.yml":
                findings.append({"location": location, "action": use, "reason": unresolved[(action, ref)]})
                continue
            raise ValueError("unlocked Action at " + location + ": " + use)
    if count == 0:
        raise ValueError("no workflow Actions inspected")
    ddk = repo / ".github/workflows/ddk-lkm.yml"
    images = re.findall(r"^    container:[ \t]*\n      image:[ \t]*(.*?)[ \t]*$",
                        ddk.read_text(encoding="utf-8"), re.M)
    if images != [DDK_IMAGE_EXPRESSION]:
        raise ValueError("DDK job must consume the offline digest resolver")
    return {"schema_version": 1, "checked_uses": count, "pinned_uses": count - len(findings),
            "unresolved": findings, "all_execution_dependencies_pinned": not findings}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("check")
    resolve = sub.add_parser("resolve-ddk")
    resolve.add_argument("--kmi", required=True)
    resolve.add_argument("--release", required=True)
    args = parser.parse_args()
    try:
        lock = load_lock(args.repo / ".github/ci-dependencies.lock.json")
        if args.command == "check":
            result = check_workflows(args.repo, lock)
        else:
            image = resolve_ddk(lock, args.kmi, args.release)
            result = {"schema_version": 1, "image": image, "platform": "linux/amd64"}
            if os.environ.get("GITHUB_OUTPUT"):
                with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as output:
                    output.write("image=" + image + "\n")
        print(json.dumps(result, ensure_ascii=False))
        return 0
    except (ValueError, OSError, UnicodeError) as error:
        print(json.dumps({"schema_version": 1, "error": str(error)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
