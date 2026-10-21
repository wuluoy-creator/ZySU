#!/usr/bin/env python3
"""Exercise the lock parser and real CMake parent/child resolution without network."""

import copy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("native_dependencies", ROOT / "cmake/native_dependencies.py")
DEPENDENCIES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DEPENDENCIES)
LOCK = ROOT / "cmake/native-dependencies.lock.json"
CMAKE = os.environ.get("CMAKE_COMMAND") or shutil.which("cmake")


class NativeDependencyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="native-dependencies-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.lock = json.loads(LOCK.read_text(encoding="utf-8"))

    def write_lock(self, lock):
        path = self.root / "lock.json"
        path.write_text(json.dumps(lock), encoding="utf-8")
        return path

    def source(self, name, license_file="LICENSE"):
        path = self.root / name
        path.mkdir()
        if license_file:
            (path / license_file).write_text("synthetic evidence; no licensing conclusion\n", encoding="utf-8")
        return path

    def test_pins_keep_existing_release_identities(self):
        dependencies = {dep["name"]: dep for dep in self.lock["dependencies"]}
        self.assertEqual(dependencies["miniz"]["commit"], "293d4db1b7d0ffee9756d035b9ac6f7431ef8492")
        self.assertEqual(dependencies["lz4"]["commit"], "ebb370ca83af193212df4dcbadcc5d87bc0de2f0")
        self.assertEqual(dependencies["mbedtls"]["archive_sha256"],
                         "8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca")

    def test_lock_rejects_mutable_or_injected_identities(self):
        mutations = [
            (1, "commit", "3.0.2"), (1, "commit", "main"),
            (1, "commit", "a" * 39), (0, "archive_sha256", "0" * 63),
            (1, "repository", 'https://example.invalid/repo")\nmessage(FATAL_ERROR injected)'),
            (1, "name", "miniz;bad"), (1, "license_files", ["../LICENSE"]),
            (1, "license_files", ["LICENSE", "LICENSE"]),
            (1, "license_files", ["LICENSE\\other"]),
        ]
        for index, key, value in mutations:
            with self.subTest(key=key, value=value):
                lock = copy.deepcopy(self.lock)
                lock["dependencies"][index][key] = value
                with self.assertRaises(DEPENDENCIES.DependencyError):
                    DEPENDENCIES.load_lock(self.write_lock(lock))

    def test_lock_rejects_future_duplicate_and_unknown_fields(self):
        for mutate in (
            lambda value: value.update(schema_version=2),
            lambda value: value.update(schema_version=True),
            lambda value: value["dependencies"].append(value["dependencies"][0]),
            lambda value: value["dependencies"][0].update(unrecognized="value"),
        ):
            lock = copy.deepcopy(self.lock)
            mutate(lock)
            with self.assertRaises(DEPENDENCIES.DependencyError):
                DEPENDENCIES.load_lock(self.write_lock(lock))
        path = self.root / "duplicate.json"
        path.write_text('{"schema_version":1,"schema_version":1,"dependencies":[]}', encoding="utf-8")
        with self.assertRaises(DEPENDENCIES.DependencyError):
            DEPENDENCIES.load_lock(path)

    def test_source_overrides_are_observations_not_archive_verification(self):
        source = self.source("arbitrary archive override")
        output = self.root / "metadata.json"
        DEPENDENCIES.write_metadata(LOCK, output, [f"mbedtls={source}"], ["mbedtls=ON"], "ON", ["mbedtls=OFF"])
        result = json.loads(output.read_text(encoding="utf-8"))
        dep = result["dependencies"][0]
        self.assertTrue(dep["local_override"])
        self.assertTrue(dep["fetchcontent_fully_disconnected"])
        self.assertEqual(dep["override_variable"], "FETCHCONTENT_SOURCE_DIR_MBEDTLS")
        self.assertEqual(dep["source_dir"], str(source))
        self.assertEqual(dep["license_files"], ["LICENSE"])
        self.assertEqual(dep["license_evidence_status"], "files_listed")
        self.assertNotIn("verified", dep)
        self.assertEqual(len(result["inactive_declarations"]), 5)

    def test_missing_license_is_explicit_and_headers_are_evidence(self):
        source = self.source("vendored", "lz4.h")
        output = self.root / "metadata.json"
        DEPENDENCIES.write_metadata(LOCK, output, [f"vendored_lz4={source}"], [])
        dep = json.loads(output.read_text(encoding="utf-8"))["dependencies"][0]
        self.assertEqual(dep["license_files"], ["lz4.h"])
        self.assertEqual(dep["resolution"], "vendored")
        self.assertEqual(dep["expected"]["revision"], "fe5794c9db82348f404e91bc7665258585880bd9")
        (source / "lz4.h").unlink()
        DEPENDENCIES.write_metadata(LOCK, output, [f"vendored_lz4={source}"], [])
        dep = json.loads(output.read_text(encoding="utf-8"))["dependencies"][0]
        self.assertEqual(dep["license_files"], [])
        self.assertEqual(dep["license_evidence_status"], "not_found")

    def test_metadata_rejects_unknown_duplicate_missing_and_ambiguous_inputs(self):
        source = self.source("source")
        output = self.root / "metadata.json"
        cases = [
            ([f"unknown={source}"], [], []),
            ([f"miniz={source}", f"miniz={source}"], ["miniz=ON"], ["miniz=OFF"]),
            ([f"miniz={source}", f"mbedtls={source}"], ["miniz=ON", "mbedtls=ON"], ["miniz=OFF", "mbedtls=OFF"]),
            ([f"miniz={source}"], [], ["miniz=OFF"]),
            ([f"miniz={source}"], ["miniz=perhaps"], ["miniz=OFF"]),
            (["miniz=relative/path"], ["miniz=ON"], ["miniz=OFF"]),
            ([f"miniz={source}/../source"], ["miniz=ON"], ["miniz=OFF"]),
            ([f"miniz={source}\n"], ["miniz=ON"], ["miniz=OFF"]),
            ([f"miniz={source / 'missing'}"], ["miniz=ON"], ["miniz=OFF"]),
        ]
        for sources, overrides, updates in cases:
            with self.subTest(sources=sources, overrides=overrides):
                with self.assertRaises((DEPENDENCIES.DependencyError, OSError)):
                    DEPENDENCIES.write_metadata(LOCK, output, sources, overrides, "OFF", updates)

    def test_cli_failed_metadata_invalidates_previous_success(self):
        output = self.root / "metadata.json"
        output.write_text('{"previous":"success"}', encoding="utf-8")
        completed = subprocess.run([sys.executable, str(ROOT / "cmake/native_dependencies.py"),
                                    "metadata", "--lock", str(LOCK), "--output", str(output),
                                    "--source", "miniz=relative/path", "--override", "miniz=ON", "--updates-disconnected", "miniz=OFF"],
                                   capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
        self.assertNotEqual(completed.returncode, 0)
        self.assertFalse(output.exists())
        self.assertIn("absolute paths", completed.stderr)

    def test_source_symlink_is_rejected(self):
        source = self.source("actual")
        alias = self.root / "alias"
        try:
            alias.symlink_to(source, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"host cannot create source symlink: {error}")
        with self.assertRaises(DEPENDENCIES.DependencyError):
            DEPENDENCIES.write_metadata(LOCK, self.root / "metadata.json", [f"miniz={alias}"], ["miniz=ON"], "OFF", ["miniz=OFF"])

    @unittest.skipUnless(CMAKE, "CMake unavailable (set CMAKE_COMMAND to exercise real parent/child resolution)")
    def test_cmake_parent_pins_win_and_resolved_overrides_are_recorded(self):
        for name in ("mbedtls", "miniz", "lz4"):
            source = self.source(name)
            (source / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.14)\n", encoding="utf-8")
        vendored = self.source("vendored", "lz4.h")
        project = self.root / "project"
        project.mkdir()
        lines = ["cmake_minimum_required(VERSION 3.14)", "project(pin_fixture NONE)",
                 f'set(Python3_EXECUTABLE "{Path(sys.executable).as_posix()}")',
                 f'include("{(ROOT / "cmake/NativeDependencies.cmake").as_posix()}")',
                 "set(FETCHCONTENT_FULLY_DISCONNECTED ON)"]
        for name in ("mbedtls", "miniz", "lz4", "magisk_lz4", "magisk_xz"):
            # Child tries to replace every parent declaration with mutable data.
            lines += [f"FetchContent_Declare({name} GIT_REPOSITORY https://invalid.invalid/child GIT_TAG master)",
                      f"get_property(saved GLOBAL PROPERTY _FetchContent_{name}_savedDetails)",
                      'if(saved MATCHES "invalid.invalid|master")',
                      '  message(FATAL_ERROR "mutable child declaration escaped parent lock")', "endif()"]
        for name in ("mbedtls", "miniz", "lz4"):
            lines += [f'set(FETCHCONTENT_SOURCE_DIR_{name.upper()} "{(self.root / name).as_posix()}")',
                      f"FetchContent_MakeAvailable({name})"]
        lines += [f'zysu_record_native_dependencies(vendored "{vendored.as_posix()}")']
        (project / "CMakeLists.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        build = self.root / "build"
        command = [CMAKE, "-S", str(project), "-B", str(build), "-G", "Ninja"]
        bundled_ninja = Path(CMAKE).parent / ("ninja.exe" if os.name == "nt" else "ninja")
        if bundled_ninja.is_file():
            command.append(f"-DCMAKE_MAKE_PROGRAM={bundled_ninja}")
        completed = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        metadata = json.loads((build / "native-dependencies.json").read_text(encoding="utf-8"))
        by_name = {dep["name"]: dep for dep in metadata["dependencies"]}
        self.assertEqual(set(by_name), {"mbedtls", "miniz", "lz4", "vendored_lz4"})
        for name in ("mbedtls", "miniz", "lz4"):
            self.assertTrue(by_name[name]["local_override"])
            self.assertTrue(by_name[name]["fetchcontent_fully_disconnected"])
        self.assertFalse(by_name["vendored_lz4"]["local_override"])
        self.assertEqual({dep["name"] for dep in metadata["inactive_declarations"]}, {"magisk_lz4", "magisk_xz"})
        declarations = (build / "native-dependency-declarations.cmake").read_text(encoding="utf-8")
        self.assertEqual(declarations.count("GIT_SHALLOW FALSE"), 4)
        self.assertNotIn("GIT_SHALLOW TRUE", declarations)

    @unittest.skipUnless(CMAKE, "CMake unavailable (set CMAKE_COMMAND to exercise configured tool metadata)")
    def test_cmake_normal_tool_values_and_json_escaping(self):
        project = self.root / "tool-project"
        project.mkdir()
        build = self.root / "tool-build"
        lines = ["cmake_minimum_required(VERSION 3.14)", "project(tool_fixture NONE)",
                 f'set(Python3_EXECUTABLE "{Path(sys.executable).as_posix()}")',
                 'set(CMAKE_CXX_COMPILER "/incorrect/cached/compiler" CACHE FILEPATH "")',
                 f'set(CMAKE_CXX_COMPILER "{Path(sys.executable).as_posix()}")',
                 'set(CMAKE_CXX_COMPILER_LAUNCHER "ccache")',
                 'set(PRIVATE_SIGNING_SECRET "do-not-copy")',
                 'set(CMAKE_CXX_FLAGS [=[-DNAME="quoted";C:\\tool\tline\nnext]=])',
                 f'include("{(ROOT / "cmake/NativeToolchainMetadata.cmake").as_posix()}")',
                 'zysu_record_native_toolchain(ksuinit)']
        (project / "CMakeLists.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        command = [CMAKE, "-S", str(project), "-B", str(build), "-G", "Ninja"]
        bundled_ninja = Path(CMAKE).parent / ("ninja.exe" if os.name == "nt" else "ninja")
        if bundled_ninja.is_file():
            command.append(f"-DCMAKE_MAKE_PROGRAM={bundled_ninja}")
        completed = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        result = json.loads((build / "native-toolchain.json").read_text(encoding="utf-8"))
        self.assertEqual(result["target"], "ksuinit")
        self.assertEqual(result["values"]["CMAKE_CXX_COMPILER"], Path(sys.executable).as_posix())
        self.assertEqual(result["values"]["CMAKE_CXX_COMPILER_LAUNCHER"], "ccache")
        self.assertEqual(result["values"]["CMAKE_CXX_FLAGS"], '-DNAME="quoted";C:\\tool\tline\nnext')
        self.assertNotIn("PRIVATE_SIGNING_SECRET", result["values"])
        self.assertNotIn("do-not-copy", (build / "native-toolchain.json").read_text(encoding="utf-8"))
        self.assertIn("CMAKE_CXX_COMPILER:FILEPATH=/incorrect/cached/compiler", (build / "CMakeCache.txt").read_text(encoding="utf-8"))
        lines.insert(-1, "string(ASCII 1 CMAKE_CXX_FLAGS)")
        (project / "CMakeLists.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        rejected = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("Unsupported control character", rejected.stderr)
        self.assertFalse((build / "native-toolchain.json").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
