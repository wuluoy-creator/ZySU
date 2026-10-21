"""Compile the real rebuild model and recorded executor with deterministic syscall faults."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    source = root / 'userspace/ksud/sumhp'
    with tempfile.TemporaryDirectory(prefix='module-rebuild-tests-') as temp:
        build = Path(temp)
        content = (source / 'src/mount/module_rebuild.cpp').read_text(encoding='utf-8')
        (build / 'module_rebuild_under_test.inc').write_text(
            content[content.index('namespace sumhp::mount::sumh {'):], encoding='utf-8')
        binary = build / ('module_rebuild_test.exe' if os.name == 'nt' else 'module_rebuild_test')
        compiler = [args.zig, 'c++'] if args.zig else [args.cxx]
        subprocess.run([*compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I' + str(root), '-I' + str(source / 'include'), '-I' + str(source / 'src'), '-I' + str(build),
            str(root / 'userspace/ksud/tests/module_rebuild_test.cpp'),
            str(source / 'src/mount/module_rebuild_model.cpp'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
