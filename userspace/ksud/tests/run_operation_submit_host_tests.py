"""Exercise production reservation input, receipts and bounded input IO.

Windows executes deterministic syscall/clock fault models. Linux additionally
executes real file/pipe/symlink IO. --target ... --compile-only cross-compiles
those POSIX tests; it does not claim their execution. Journal/capture effects
are counters here; the separate state suite covers durable at-most-once flow.
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
    if args.target and not args.zig:
        parser.error('--target requires --zig')
    root = Path(__file__).resolve().parents[3]
    ksud = root / 'userspace/ksud'
    source = ksud / 'sumhp/src'
    compiler = [args.zig, 'c++'] if args.zig else [args.cxx]
    if args.target:
        compiler += ['-target', args.target]
    elif os.name == 'nt':
        compiler += ['-DESTALE=116', '-DESHUTDOWN=108']
    with tempfile.TemporaryDirectory(prefix='operation-submit-') as temp:
        build = Path(temp)
        body = (source / 'core/operation_submit_io.cpp').read_text(encoding='utf-8')
        body = body[body.index('namespace sumhp::state {'):]
        for name in ('fcntl', 'open', 'fstat', 'poll', 'read', 'close'):
            body = body.replace('::' + name + '(', 'io_model::' + name + '(')
        body = body.replace('std::chrono::steady_clock', 'io_model::Clock')
        body = body.replace('struct stat ', 'struct InputStat ')
        (build / 'operation_input_io_under_test.inc').write_text(body, encoding='utf-8')
        daemon = (source / 'core/daemon.cpp').read_text(encoding='utf-8')
        start = daemon.index('bool exchange_reserved_operation(')
        (build / 'operation_exchange_under_test.inc').write_text(
            daemon[start:daemon.index('\nint run_via_daemon(', start)], encoding='utf-8')
        # Use the exact special dispatch block, with a one-item loop for its
        # continue. Thus tests fail if it ever falls through to legacy capture.
        start = daemon.index('        if (request_args.size() >= 2 && request_args[0] == "operation"')
        (build / 'operation_dispatch_under_test.inc').write_text(
            daemon[start:daemon.index('        const auto result = run_command_capture', start)], encoding='utf-8')
        targets = {
            'operation_submit_test': [ksud / 'tests/operation_submit_test.cpp', source / 'core/operation_submit.cpp',
                source / 'core/operation_submit_input.cpp', source / 'core/state_ledger_model.cpp',
                ksud / 'src/cli_args.cpp', ksud / 'src/terminal.cpp'],
            'operation_input_io_test': [ksud / 'tests/operation_input_io_test.cpp'],
        }
        if os.name != 'nt' or args.target:
            targets['operation_input_posix_test'] = [ksud / 'tests/operation_input_posix_test.cpp', source / 'core/operation_submit_io.cpp']
        for name, files in targets.items():
            binary = build / (name + ('.exe' if os.name == 'nt' and not args.target else ''))
            subprocess.run([*compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', '-pthread',
                '-I' + str(root), '-I' + str(build), '-I' + str(source), '-I' + str(ksud / 'sumhp/include'), '-I' + str(ksud / 'src'),
                *map(str, files), '-o', str(binary)], check=True)
            if not args.compile_only:
                subprocess.run([str(binary)], check=True, timeout=30)
        if args.compile_only:
            print('Submission POSIX binaries compiled; execution skipped.')


if __name__ == '__main__':
    main()
