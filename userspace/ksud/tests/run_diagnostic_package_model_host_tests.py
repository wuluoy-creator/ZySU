"""Compile the production privacy model and four fresh production doctor fixtures.

No device, collector, daemon, disk package store, or live rules are accessed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--fingerprints')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    src = root / 'userspace/ksud/sumhp/src'
    tests = root / 'userspace/ksud/tests'
    inputs = [Path(__file__), tests / 'diagnostic_package_model_test.cpp',
              src / 'core/diagnostic_package_model.cpp', src / 'core/diagnostic_package.hpp',
              src / 'core/json.hpp', root / 'userspace/common/json.hpp',
              root / 'userspace/common/diagnostics_request.hpp', root / 'userspace/common/utf8.hpp',
              tests / 'run_diagnostics_model_host_tests.py', tests / 'diagnostics_model_test.cpp',
              src / 'core/diagnostics_model.cpp', src / 'core/diagnostics_model.hpp',
              src / 'mount/module_preview.cpp', src / 'mount/maps_preview.cpp',
              src / 'core/state_ledger_model.cpp', src / 'mount/recovery.cpp', src / 'mount/backend.cpp']
    before = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    with tempfile.TemporaryDirectory(prefix='sumh-diagnostic-package-model-') as temporary:
        build = Path(temporary)
        fixture_directory = build / 'fixtures'
        options = ['--zig', args.zig] if args.zig else ['--cxx', args.cxx]
        subprocess.run([sys.executable, '-B', str(tests / 'run_diagnostics_model_host_tests.py'),
                        *options, '--fixture-directory', str(fixture_directory)], check=True)
        compiler = [args.zig, 'c++'] if args.zig else [args.cxx]
        executable = build / ('diagnostic_package_model_test.exe' if os.name == 'nt' else 'diagnostic_package_model_test')
        subprocess.run([*compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(root), '-I' + str(src),
                        str(tests / 'diagnostic_package_model_test.cpp'),
                        str(src / 'core/diagnostic_package_model.cpp'),
                        str(src / 'core/diagnostics_model.cpp'), '-o', str(executable)], check=True)
        subprocess.run([str(executable), str(fixture_directory)], check=True, timeout=60)
    after = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    if args.fingerprints:
        Path(args.fingerprints).write_text(json.dumps({'before': before, 'after': after,
            'stable': before == after}, indent=2) + '\n', encoding='utf-8')
    if before != after:
        raise RuntimeError('Diagnostic package model inputs changed during execution')
    print('PASS diagnostic package model inputs stable; host-only evidence')


if __name__ == '__main__':
    main()
