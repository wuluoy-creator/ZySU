"""Pure diagnostic model and shared strict grammar against production serializer excerpts.

This suite performs no device, kernel, namespace or transport actions. Producer
serializers are compiled from current source; boundary data are deterministic.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--fixture-directory')
    parser.add_argument('--fingerprints')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    src = root / 'userspace/ksud/sumhp/src'
    sources = {
        'module': src / 'mount/module_preview.cpp',
        'maps': src / 'mount/maps_preview.cpp',
        'ledger': src / 'core/state_ledger_model.cpp',
        'recovery': src / 'mount/recovery.cpp',
        'backend': src / 'mount/backend.cpp',
    }
    inputs = [*sources.values(), Path(__file__), root / 'userspace/ksud/tests/diagnostics_model_test.cpp',
              root / 'userspace/common/diagnostics_request.hpp', root / 'userspace/common/json.hpp',
              root / 'userspace/common/utf8.hpp', src / 'core/diagnostics_model.hpp', src / 'core/diagnostics_model.cpp', src / 'core/json.hpp']
    before = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    with tempfile.TemporaryDirectory(prefix='sumh-diagnostics-model-') as temporary:
        build = Path(temporary)
        bodies = {key: path.read_text(encoding='utf-8') for key, path in sources.items()}
        (build / 'diagnostic_module_serializers.inc').write_text(
            function(bodies['module'], 'JsonValue probe_json(') + function(bodies['module'], 'JsonValue decision_json('), encoding='utf-8')
        (build / 'diagnostic_maps_serializers.inc').write_text(
            function(bodies['maps'], 'JsonValue identity_json(') + function(bodies['maps'], 'JsonValue envelope('), encoding='utf-8')
        (build / 'diagnostic_operation_serializers.inc').write_text(
            function(bodies['ledger'], 'JsonValue operation_json(') + function(bodies['ledger'], 'JsonValue operation_record_json('), encoding='utf-8')
        (build / 'diagnostic_recovery_serializer.inc').write_text(function(bodies['recovery'], 'std::string recovery_status_json('), encoding='utf-8')
        (build / 'diagnostic_module_validator.inc').write_text(function(bodies['backend'], 'bool valid_module_id('), encoding='utf-8')
        compiler = [args.zig, 'c++'] if args.zig else [args.cxx]
        output = build / ('diagnostics_model_test.exe' if os.name == 'nt' else 'diagnostics_model_test')
        subprocess.run([*compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I' + str(root), '-I' + str(src), '-I' + str(build),
            str(root / 'userspace/ksud/tests/diagnostics_model_test.cpp'), str(src / 'core/diagnostics_model.cpp'),
            '-o', str(output)], check=True)
        if args.fixture_directory:
            Path(args.fixture_directory).mkdir(parents=True, exist_ok=True)
        subprocess.run([str(output), *([str(Path(args.fixture_directory).resolve())] if args.fixture_directory else [])], check=True, timeout=60)
    after = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    if args.fingerprints:
        Path(args.fingerprints).write_text(json.dumps({'before': before, 'after': after, 'stable': before == after}, indent=2) + '\n', encoding='utf-8')
    if before != after:
        raise RuntimeError('Model suite input changed during execution')
    print('PASS diagnostics model inputs stable; host-only evidence')


if __name__ == '__main__':
    main()
