"""Execute production profile argument/controller and bounded-input helpers.

The host runner replaces filesystem, namespace, persistence and digest boundaries
with counted/fault-injected doubles; model/store have their own production
suites. This does not execute Android namespace changes or prove power-loss safety.
"""
import argparse
import os
import re
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
    with tempfile.TemporaryDirectory(prefix='profile-controller-') as temp:
        build = Path(temp)
        body = (source / 'core/profile_input.cpp').read_text(encoding='utf-8')
        start = body.index('bool input_path_valid(')
        namespace_end = re.search(r'\n}\s*// namespace', body[start:])
        assert namespace_end is not None
        path_helper = body[start:start + namespace_end.start()]
        io = body[body.index('bool read_profile_input('):]
        io = 'namespace sumhp::profiles {\nnamespace {\n' + path_helper + '\n}\n' + io
        for name in ('fcntl', 'open', 'fstat', 'poll', 'read', 'close'):
            io = io.replace('::' + name + '(', 'io_model::' + name + '(')
        io = io.replace('std::chrono::steady_clock', 'io_model::Clock')
        io = io.replace('struct stat ', 'struct InputStat ')
        (build / 'profile_input_io_under_test.inc').write_text(io, encoding='utf-8')
        arguments = body[body.index('namespace sumhp::profiles {'):body.index('bool read_profile_input(')]
        (build / 'profile_arguments_under_test.inc').write_text(arguments + '\n}\n', encoding='utf-8')
        model = (source / 'core/profile_model.cpp').read_text(encoding='utf-8')
        start = model.index('bool valid_profile_id(')
        (build / 'profile_id_under_test.inc').write_text(
            model[start:model.index('\nbool parse_profile(', start)], encoding='utf-8')
        targets = {'profile_input_io_test': [ksud / 'tests/profile_input_io_test.cpp']}
        controller = source / 'core/profile.cpp'
        controller_test = ksud / 'tests/profile_controller_test.cpp'
        body = controller.read_text(encoding='utf-8')
        body = body[body.index('namespace sumhp::profiles {'):]
        for name in ('open', 'close', 'fstat', 'setns'):
            body = body.replace('::' + name + '(', 'controller_model::' + name + '(')
        body = body.replace('struct stat ', 'struct ProfileStat ')
        (build / 'profile_controller_under_test.inc').write_text(body, encoding='utf-8')
        targets['profile_controller_test'] = [controller_test]
        for name, files in targets.items():
            binary = build / (name + ('.exe' if os.name == 'nt' else ''))
            subprocess.run([*compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-Wno-missing-field-initializers', '-I' + str(root), '-I' + str(build),
                '-I' + str(source), '-I' + str(ksud / 'sumhp/include'), '-I' + str(ksud / 'src'),
                *map(str, files), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == '__main__':
    main()
