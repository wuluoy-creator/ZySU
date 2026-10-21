"""Compile semantic controls for tracked Maps input and local reply validation.

This is host execution with the submission fixture's capture counters, not a
kernel/Journal integration test. The state runner covers the actual local flow.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--out-dir', required=True)
    parser.add_argument('--baseline-root', required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output = Path(args.out_dir).resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = root / 'userspace/ksud/sumhp/src'
    baseline = Path(args.baseline_root).resolve() / 'userspace/ksud/sumhp/src/core/operation_submit_input.cpp'
    tracked = [source / 'core/operation_submit.cpp', source / 'core/operation_submit_input.cpp',
               source / 'core/command.hpp', source / 'core/daemon.cpp',
               root / 'userspace/common/temporary_maps_command.hpp',
               root / 'userspace/ksud/tests/operation_submit_test.cpp', Path(__file__).resolve(), baseline]
    manifest = {'commands': [], 'source_before': {str(path): digest(path) for path in tracked},
                'limitations': ['host submission fixture counters; not live kernel or Journal durability',
                                'retained-before control replaces only actual operation input parser'], 'passed': False}
    compiler = [args.zig, 'c++'] if args.zig else [args.cxx]
    if os.name == 'nt':
        compiler += ['-DESTALE=116', '-DESHUTDOWN=108']
    daemon = (source / 'core/daemon.cpp').read_text(encoding='utf-8')
    start = daemon.index('bool exchange_reserved_operation(')
    (output / 'operation_exchange_under_test.inc').write_text(
        daemon[start:daemon.index('\nint run_via_daemon(', start)], encoding='utf-8')
    start = daemon.index('        if (request_args.size() >= 2 && request_args[0] == "operation"')
    (output / 'operation_dispatch_under_test.inc').write_text(
        daemon[start:daemon.index('        const auto result = run_command_capture', start)], encoding='utf-8')
    submit = (source / 'core/operation_submit.cpp').read_text(encoding='utf-8')
    begin = submit.index('            const auto local_reply = stringify_json(operation_execution_json(input, captured));')
    end = submit.index('\n        } else if (!exchange_reserved_operation', begin)
    mutant = output / 'operation_submit_without_local_validation.cpp'
    mutant.write_text(submit[:begin] + '            result = operation_execution_json(input, captured);\n' +
                      '            if (result.o.at("status").s == "command_completed") exit_code = 0;' + submit[end:], encoding='utf-8')
    flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', '-pthread']
    flags += ['-I' + str(path) for path in (root, output, source, source / 'core',
                                           root / 'userspace/ksud/sumhp/include', root / 'userspace/ksud/src')]
    common = [root / 'userspace/ksud/tests/operation_submit_test.cpp', source / 'core/state_ledger_model.cpp',
              root / 'userspace/ksud/src/cli_args.cpp', root / 'userspace/ksud/src/terminal.cpp']

    def run(command, expected=0, sentinel=None):
        result = subprocess.run(list(map(str, command)), text=True, capture_output=True, timeout=120)
        manifest['commands'].append({'argv': list(map(str, command)), 'exit_code': result.returncode,
                                     'stdout': result.stdout, 'stderr': result.stderr})
        print(result.stdout, end='')
        if result.stderr:
            print(result.stderr, end='')
        if result.returncode != expected or (sentinel and sentinel not in result.stdout):
            raise RuntimeError('unexpected compile or semantic-control outcome')

    try:
        for name, submit_source, input_source, cases in (
            ('current', source / 'core/operation_submit.cpp', source / 'core/operation_submit_input.cpp',
             [('tracked-input', 0, 'PASS tracked_input_accepted'), ('local-envelope', 0, 'PASS local_result_normalized')]),
            ('mutant-no-local-validation', mutant, source / 'core/operation_submit_input.cpp',
             [('local-envelope', 253, 'SEMANTIC FAILURE local_result_normalized')]),
            ('retained-before-input', source / 'core/operation_submit.cpp', baseline,
             [('tracked-input', 253, 'SEMANTIC FAILURE tracked_input_accepted')]),
        ):
            binary = output / (name + ('.exe' if os.name == 'nt' else ''))
            run([*compiler, *flags, *common, submit_source, input_source, '-o', binary])
            for case, expected, sentinel in cases:
                run([binary, case], expected, sentinel)
        manifest['source_after'] = {str(path): digest(path) for path in tracked}
        if manifest['source_before'] != manifest['source_after']:
            raise RuntimeError('source changed during controls')
        manifest['passed'] = True
        print('PASS tracked Maps semantic controls (compiled mutants/retained input; exact failures)')
    finally:
        manifest['artifacts'] = {str(path.relative_to(output)): digest(path) for path in output.rglob('*') if path.is_file()}
        (output / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
