"""Run the actual inactive profile bank with production profile/resource models.

Lock syscalls, checked reads, durable writes and digest generation are counted
fault boundaries. This does not validate POSIX locks, SHA-256 strength, actual
namespace switching or Android power-loss behavior.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from run_profile_model_host_tests import write_validators


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    ksud = root / 'userspace/ksud'
    source = ksud / 'sumhp/src'
    compiler = [args.zig, 'c++'] if args.zig else [args.cxx]
    with tempfile.TemporaryDirectory(prefix='profile-store-tests-') as temp:
        build = Path(temp)
        if os.name == 'nt':
            (build / 'linux').mkdir(); (build / 'sys').mkdir()
            (build / 'linux/types.h').write_text('#pragma once\n#include <stdint.h>\ntypedef uint32_t __u32;\ntypedef int32_t __s32;\n#define __aligned_u64 uint64_t __attribute__((aligned(8)))\n')
            (build / 'sys/ioctl.h').write_text('#pragma once\n#define _IOC(d,t,n,s) (((unsigned long)(d)<<30)|((unsigned long)(s)<<16)|((unsigned long)(t)<<8)|(n))\n#define _IO(t,n) _IOC(0,t,n,0)\n#define _IOR(t,n,s) _IOC(2,t,n,sizeof(s))\n#define _IOW(t,n,s) _IOC(1,t,n,sizeof(s))\n#define _IOWR(t,n,s) _IOC(3,t,n,sizeof(s))\n')
        write_validators(root, build)
        store = (source / 'core/profile_store.cpp').read_text(encoding='utf-8')
        store = store[store.index('namespace sumhp::profiles {'):]
        for name in ('open', 'close', 'fstat', 'flock'):
            store = store.replace('::' + name + '(', 'io_model::' + name + '(')
        store = store.replace('struct stat ', 'struct io_model::Metadata ')
        (build / 'profile_store_under_test.inc').write_text(store, encoding='utf-8')
        binary = build / ('profile_store_test.exe' if os.name == 'nt' else 'profile_store_test')
        subprocess.run([*compiler, '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
            '-I' + str(root), '-I' + str(build), '-I' + str(source),
            '-I' + str(ksud / 'sumhp/include'), '-I' + str(ksud / 'src'),
            str(ksud / 'tests/profile_store_test.cpp'),
            str(source / 'core/profile_model.cpp'), str(source / 'core/user_hide_model.cpp'),
            '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=180)


if __name__ == '__main__':
    main()
