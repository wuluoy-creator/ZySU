"""Exercise production normalization planning/execution against a deterministic FS model.

Every mutation boundary is instrumented. This is not real Linux filesystem,
SELinux, renameat2, crash-durability or Android namespace execution evidence.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--target')
    parser.add_argument('--compile-only', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    src = root / 'userspace/ksud/sumhp/src/mount'
    with tempfile.TemporaryDirectory(prefix='sumh-normalize-') as temporary:
        build = Path(temporary)
        header = (src / 'magic_mount.hpp').read_text(encoding='utf-8')
        body = header[header.index('struct NormalizationObservation {'):header.index('// Read-only checked plan.')]
        source = (src / 'magic_mount.cpp').read_text(encoding='utf-8')
        start = source.index('namespace {\nconst std::vector<std::string>& normalization_partitions()')
        body += source[start:source.rindex('\n}  // namespace sumhp::mount::magic')]
        for call in ('readlink', 'open', 'close', 'fstat', 'mkdirat', 'openat', 'syscall', 'symlinkat', 'fsync'):
            body = body.replace('::' + call + '(', 'model::' + call + '(')
        # Filesystem root syntax differs on Windows. The fixture path facade
        # below implements POSIX absolute-path semantics for virtual paths.
        (build / 'module_normalize_under_test.inc').write_text(body, encoding='utf-8')
        backend = (src / 'backend.cpp').read_text(encoding='utf-8')
        validation = []
        for signature in ('bool valid_stored_module_rule_path(', 'bool valid_module_rule_path('):
            start = backend.index(signature)
            validation.append(backend[start:backend.index('\n}', start) + 2])
        (build / 'module_normalize_path_under_test.inc').write_text('\n'.join(validation), encoding='utf-8')
        compiler = [args.zig, 'c++'] if args.zig else [args.cxx]
        if args.target: compiler += ['-target', args.target]
        output = build / ('normalize.o' if args.compile_only else 'normalize.exe' if os.name == 'nt' else 'normalize')
        subprocess.run([*compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(build),
            *(['-c'] if args.compile_only else []), str(root / 'userspace/ksud/tests/module_normalize_test.cpp'),
            '-o', str(output)], check=True)
        if not args.compile_only: subprocess.run([str(output)], check=True, timeout=30)


if __name__ == '__main__':
    main()
