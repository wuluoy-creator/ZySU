#!/usr/bin/env python3
"""Package final artifacts and generate update.json from the signed APK manifest."""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

from resolve_versions import version_code, version_name


def package_release(artifacts, output, apk_name, apk_code, *, ksud_name, ksud_code, kernel_name, kernel_code,
                    expected_commit=None, expected_kmis=None, library_name=None):
    manager_name = "v" + version_name(apk_name, "APK versionName")
    manager_code = version_code(apk_code, "APK versionCode")
    metadata = {
        "schema_version": 1,
        "version_name": manager_name,
        "version_code": manager_code,
        "ksud": {"version_name": version_name(ksud_name, "ksud version"), "version_code": version_code(ksud_code, "ksud code")},
        "kernel": {"version_name": version_name(kernel_name, "kernel version"), "version_code": version_code(kernel_code, "kernel code")},
    }
    # Older installed managers parse _<code>-release; retain that filename.
    filename = f"ZySU_{manager_name}_{manager_code}-release.apk"
    if any(value is not None for value in (expected_commit, expected_kmis, library_name)):
        from release_provenance import collect_release, stage_release
        collection = collect_release(artifacts, expected_commit=expected_commit,
                                     expected_kmis=expected_kmis, library_name=library_name)
        return stage_release(output, collection, filename, metadata)
    # Compatibility helper for historical local callers. The release CLI below
    # always selects the strict byte/evidence chain; this branch emits no such claim.
    apks = list((artifacts / "Manager-arm64-v8a").glob("*.apk"))
    if len(apks) != 1:
        raise ValueError(f"Expected one final Manager APK, found {len(apks)}")
    output.mkdir(parents=True, exist_ok=True)
    apk = output / filename
    shutil.copy2(apks[0], apk)
    signatures = [apks[0].with_suffix(".sig"), Path(str(apks[0]) + ".sig")]
    for signature in signatures:
        if signature.is_file():
            shutil.copy2(signature, output / f"{filename}.sig")
            break
    for directory in sorted(artifacts.glob("ksud-*")):
        daemon = directory / "ksud"
        if daemon.is_file():
            shutil.copy2(daemon, output / directory.name)
            if (directory / "ksud.sig").is_file():
                shutil.copy2(directory / "ksud.sig", output / f"{directory.name}.sig")
    for directory in sorted(artifacts.glob("*-lkm")):
        for module in directory.glob("*_kernelsu.ko*"):
            if module.is_file() and (module.name.endswith(".ko") or module.name.endswith(".ko.sig")):
                shutil.copy2(module, output / module.name)
    metadata["apk"] = {"name": filename, "sha256": hashlib.sha256(apk.read_bytes()).hexdigest(), "size": apk.stat().st_size}
    (output / "update.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path, default=Path("artifacts"))
    parser.add_argument("--output", type=Path, default=Path("release-assets"))
    parser.add_argument("--commit", default=os.environ.get("COMMIT_SHA"))
    parser.add_argument("--kmi", default=os.environ.get("EXPECTED_KMI"))
    args = parser.parse_args()
    if not args.commit or not args.kmi:
        parser.error("--commit/COMMIT_SHA and --kmi/EXPECTED_KMI are required for strict release assembly")
    from plan_build import SUPPORTED_KMIS
    from release_provenance import safe_path, validate_selection
    kmis = list(SUPPORTED_KMIS) if args.kmi == "all" else args.kmi.split(",")
    library_name = (Path(__file__).resolve().parents[2] / "manager/ksud-library-name.txt").read_text(encoding="utf-8").strip()
    validate_selection(args.commit, kmis, library_name)
    apks = list((args.artifacts / "Manager-arm64-v8a").glob("*.apk"))
    if len(apks) != 1:
        raise ValueError("Expected exactly one final Manager APK")
    sdk = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME", "")
    analyzer = shutil.which("apkanalyzer") or str(Path(sdk) / "cmdline-tools/latest/bin/apkanalyzer")
    def manifest(field):
        return subprocess.check_output([analyzer, "manifest", field, str(safe_path(apks[0]))], text=True, timeout=30).strip()
    if manifest("application-id") != "com.zying.zysu":
        raise ValueError("Unexpected Manager package ID")
    name, code = manifest("version-name"), manifest("version-code")
    if name != os.environ["MANAGER_VERSION_NAME"] or code != os.environ["MANAGER_VERSION_CODE"]:
        raise ValueError("Final APK version does not match the resolved build version")
    package_release(args.artifacts, args.output, name, code,
                    ksud_name=os.environ["KSUD_VERSION_NAME"], ksud_code=os.environ["KSUD_VERSION_CODE"],
                    kernel_name=os.environ["KERNEL_VERSION_NAME"], kernel_code=os.environ["KERNEL_VERSION_CODE"],
                    expected_commit=args.commit, expected_kmis=kmis, library_name=library_name)


if __name__ == "__main__":
    main()
