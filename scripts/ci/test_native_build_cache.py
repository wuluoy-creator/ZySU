"""Exercise optional compiler-cache setup without package installs or network access.

Windows: python test_native_build_cache.py --bash "C:/Program Files/Git/bin/bash.exe"
"""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SETUP = ROOT / ".github/actions/setup-native-build/setup.sh"
BASH = shutil.which("bash")


def shell_path(path):
    path = Path(path).resolve()
    if os.name == "nt":
        return "/" + path.drive[0].lower() + path.as_posix()[2:]
    return str(path)


class NativeBuildCacheTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not BASH:
            raise unittest.SkipTest("Bash is required; use --bash to select it")
        result = subprocess.run(
            [BASH, "--noprofile", "--norc", "-c",
             "command -v bash; command -v sha256sum; command -v cut; command -v chmod"],
            check=True, capture_output=True, text=True,
        )
        cls.shell, cls.sha256sum, cls.cut, cls.chmod = result.stdout.splitlines()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="native-cache-test-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.bin = self.directory / "bin"
        self.bin.mkdir()
        self.ndk = self.directory / "ndk"
        compiler = self.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
        compiler.parent.mkdir(parents=True)
        self.write_executable(compiler, "exit 0\n")
        (self.ndk / "source.properties").write_text("Pkg.Revision = 30.0.0\n")
        self.output = self.directory / "output"
        self.github_env = self.directory / "environment"
        self.apt_log = self.directory / "apt-calls"
        self.output.touch()
        self.github_env.touch()
        self.apt_log.touch()
        for name, executable in (("sha256sum", self.sha256sum), ("cut", self.cut)):
            self.write_executable(self.bin / name, f"exec {shlex.quote(executable)} \"$@\"\n")
        self.write_executable(self.bin / "sudo", 'exec "$@"\n')
        self.write_executable(self.bin / "timeout", """
while [[ "$1" == -* || "$1" =~ ^[0-9]+[smhd]?$ ]]; do shift; done
exec "$@"
""")
        self.write_executable(self.bin / "apt-get", """
operation=''
for argument; do
    case "$argument" in update|install) operation="$argument";; esac
done
printf '%s\\n' "$operation" >> "$TEST_APT_LOG"
if [[ "$TEST_APT_MODE" == "update-failure" && "$operation" == "update" ]] ||
   [[ "$TEST_APT_MODE" == "install-failure" && "$operation" == "install" ]]; then
    echo 'Temporary failure resolving package mirror' >&2
    exit 100
fi
if [[ "$operation" == "install" ]]; then
    printf '#!%s\\nexit 0\\n' "$TEST_SHELL" > "$TEST_BIN/ccache"
    "$TEST_CHMOD" +x "$TEST_BIN/ccache"
fi
""")

    def write_executable(self, path, body):
        path.write_text(f"#!{self.shell}\nset -eu\n{body}", encoding="utf-8", newline="\n")
        path.chmod(0o755)

    def run_setup(self, mode="success", installed=False):
        if installed:
            self.write_executable(self.bin / "ccache", "exit 0\n")
        environment = os.environ.copy()
        environment.update({
            "PATH": shell_path(self.bin),
            "ANDROID_NDK_HOME": shell_path(self.ndk),
            "GITHUB_OUTPUT": shell_path(self.output),
            "GITHUB_ENV": shell_path(self.github_env),
            "GITHUB_WORKSPACE": shell_path(ROOT),
            "RUNNER_TEMP": shell_path(self.directory),
            "TEST_APT_MODE": mode,
            "TEST_APT_LOG": shell_path(self.apt_log),
            "TEST_BIN": shell_path(self.bin),
            "TEST_SHELL": self.shell,
            "TEST_CHMOD": self.chmod,
        })
        result = subprocess.run(
            [BASH, "--noprofile", "--norc", shell_path(SETUP)],
            env=environment, capture_output=True, text=True, timeout=10,
        )
        outputs = dict(line.split("=", 1) for line in self.output.read_text().splitlines())
        return result, outputs, self.apt_log.read_text().splitlines()

    def assert_cache_enabled(self, result, outputs):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(outputs["available"], "true")
        self.assertEqual(outputs["launcher"], shell_path(self.bin / "ccache"))
        self.assertIn("CCACHE_DIR=", self.github_env.read_text())

    def test_preinstalled_cache_needs_no_package_manager(self):
        result, outputs, calls = self.run_setup(installed=True)
        self.assert_cache_enabled(result, outputs)
        self.assertEqual(calls, [])

    def test_successful_install_enables_cache(self):
        result, outputs, calls = self.run_setup()
        self.assert_cache_enabled(result, outputs)
        self.assertIn("install", calls)

    def assert_cache_unavailable(self, mode):
        result, outputs, calls = self.run_setup(mode)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(outputs["available"], "false")
        self.assertEqual(outputs["launcher"], "")
        self.assertEqual(self.github_env.read_text(), "")
        self.assertIn("::warning::", result.stdout + result.stderr)
        self.assertTrue(calls)
        return calls

    def test_failed_package_index_update_still_allows_uncached_build(self):
        self.assert_cache_unavailable("update-failure")

    def test_failed_package_install_still_allows_uncached_build(self):
        calls = self.assert_cache_unavailable("install-failure")
        self.assertIn("install", calls)

    def test_missing_ndk_metadata_remains_fatal(self):
        (self.ndk / "source.properties").unlink()
        result, outputs, calls = self.run_setup(installed=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotEqual(outputs.get("available"), "true")
        self.assertEqual(calls, [])

    def test_missing_ndk_compiler_remains_fatal(self):
        (self.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/clang").unlink()
        result, outputs, calls = self.run_setup(installed=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotEqual(outputs.get("available"), "true")
        self.assertEqual(calls, [])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bash", default=BASH)
    arguments, remaining = parser.parse_known_args()
    BASH = arguments.bash
    unittest.main(argv=[__file__, *remaining])
