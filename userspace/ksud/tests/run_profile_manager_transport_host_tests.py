"""Production profile transport fault model; POSIX exec smoke only on POSIX.

Windows executes a syscall model. --target --compile-only is compile evidence,
never a claim that pidfd, SELinux, Android or durability checks ran on a device.
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
    compiler = [args.zig, 'c++'] if args.zig else [args.cxx]
    if args.target:
        compiler += ['-target', args.target]
    flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread', '-I' + str(root)]
    with tempfile.TemporaryDirectory(prefix='profile-manager-transport-') as directory:
        build = Path(directory)
        source = root / 'userspace/common/profile_manager_transport.cpp'
        body = source.read_text(encoding='utf-8')
        body = body[body.index('namespace sumhp::profile_transport {'):]
        calls = ('clock_gettime', 'poll', 'socket', 'bind', 'close', 'getsockopt', 'getsockname',
            'fcntl', 'recvmsg', 'sendmsg', 'waitid', 'socketpair', 'posix_spawn_file_actions_init',
            'posix_spawn_file_actions_adddup2', 'posix_spawn_file_actions_addopen',
            'posix_spawn_file_actions_addclose', 'posix_spawn_file_actions_destroy', 'posix_spawn',
            'fstat', 'pread', 'pwrite', 'ftruncate', 'fsync', 'shutdown', 'syscall', 'getpid')
        for name in calls:
            body = body.replace('::' + name + '(', 'transport_model::' + name + '(')
        (build / 'profile_manager_transport_under_test.inc').write_text(body, encoding='utf-8')
        binary = build / ('profile_manager_transport_test' + ('.exe' if os.name == 'nt' and not args.target else ''))
        subprocess.run([*compiler, *flags, '-I' + str(build),
            str(root / 'userspace/ksud/tests/profile_manager_transport_test.cpp'), '-o', str(binary)], check=True)
        if not args.compile_only:
            subprocess.run([str(binary)], check=True, timeout=45)
        if os.name != 'nt' or args.target:
            smoke = build / 'profile_manager_transport_posix_test'
            # Zig's musl CMSG_NXTHDR macro itself mixes signed pointer offsets
            # and size_t; keep that system-header warning visible, non-fatal.
            extra = ['-Wno-error=sign-compare'] if args.target and 'musl' in args.target else []
            subprocess.run([*compiler, *flags, *extra, str(source),
                str(root / 'userspace/ksud/tests/profile_manager_transport_posix_test.cpp'), '-o', str(smoke)], check=True)
            if not args.compile_only:
                subprocess.run([str(smoke)], check=True, timeout=45)
        else:
            print('POSIX exec/pidfd/writer survival not executed on Windows; syscall model only.')
        if args.compile_only:
            print('Compile-only PASS for ' + (args.target or 'host') + '; no executable/runtime tests were run.')


if __name__ == '__main__':
    main()
