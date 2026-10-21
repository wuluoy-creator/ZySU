"""Exercise the production user-hide definition codec, transaction and compatibility wrappers.

Filesystem reads, locks, durable replacement, directory preparation and hashing
are deterministic fault boundaries. No live kernel, root filesystem mutation,
POSIX locking, power-loss or SHA-256 implementation validation is claimed.
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
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    ksud = root / 'userspace/ksud'
    source = ksud / 'sumhp/src'
    compiler = [args.zig, 'c++'] if args.zig else [args.cxx]
    with tempfile.TemporaryDirectory(prefix='user-hide-store-') as temp:
        build = Path(temp)
        store = (source / 'core/user_hide_store.cpp').read_text(encoding='utf-8')
        store = store[store.index('namespace sumhp::hide {'):]
        for name in ('open', 'close', 'fstat', 'flock'):
            store = store.replace('::' + name + '(', 'io_model::' + name + '(')
        store = store.replace('struct stat ', 'struct io_model::Metadata ')
        (build / 'user_hide_store_under_test.inc').write_text(store, encoding='utf-8')
        config = (source / 'core/config.cpp').read_text(encoding='utf-8')
        start = config.index('bool load_user_hide_rules(')
        (build / 'user_hide_wrappers_under_test.inc').write_text(
            config[start:config.index('\nnamespace {', start)], encoding='utf-8')
        binary = build / ('user_hide_store_test.exe' if os.name == 'nt' else 'user_hide_store_test')
        subprocess.run([*compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I' + str(build), '-I' + str(source), '-I' + str(ksud / 'src'),
            str(ksud / 'tests/user_hide_store_test.cpp'), str(source / 'core/user_hide_model.cpp'),
            '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == '__main__':
    main()
