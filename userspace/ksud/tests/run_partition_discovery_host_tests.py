"""Exercise the actual bounded scanner with deterministic POSIX boundary faults.

The scanner/config-name/builtin algorithms are production code. Host execution
does not prove Android descriptor/race/mount semantics; --target --compile-only
also compiles the untouched source for a POSIX target without claiming execution.
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
    src = root / 'userspace/ksud/sumhp/src'
    compiler = [args.zig, 'c++'] if args.zig else [args.cxx]
    if args.target:
        compiler += ['-target', args.target]
    with tempfile.TemporaryDirectory(prefix='sumh-partition-discovery-') as temporary:
        build = Path(temporary)
        flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-DESTALE=116',
                 '-I' + str(root), '-I' + str(root / 'userspace/ksud/sumhp/include'),
                 '-I' + str(src), '-I' + str(build)]
        if args.compile_only:
            subprocess.run([*compiler, *flags, '-c', str(src / 'mount/partition_discovery.cpp'),
                            '-o', str(build / 'partition_discovery.o')], check=True)
            print('untouched production partition scanner POSIX compile passed')
            return
        body = (src / 'mount/partition_discovery.cpp').read_text(encoding='utf-8')
        body = body[body.index('namespace sumhp::mount {'):]
        for call in ('open', 'openat', 'fstatat', 'fstat', 'read', 'close', 'fdopendir', 'closedir', 'readdir', 'readlinkat'):
            body = body.replace('::' + call + '(', 'partition_model::' + call + '(')
        body = body.replace('struct stat', 'partition_model::Metadata').replace('DIR*', 'partition_model::Directory*')
        for name in ('DIR', 'REG', 'LNK'):
            body = body.replace('S_IS' + name + '(', 'partition_model::is_' + name.lower() + '(')
        (build / 'partition_discovery_under_test.inc').write_text(body, encoding='utf-8')
        config = (src / 'core/config.cpp').read_text(encoding='utf-8')
        config = config[config.index('std::string default_config_json() {'):config.index('\nnamespace {\nbool save_config(')]
        (build / 'partition_config_under_test.inc').write_text(config, encoding='utf-8')
        mount = (src / 'mount/mount_fs.cpp').read_text(encoding='utf-8')
        start = mount.index('const std::vector<std::string>& managed_partitions() {')
        (build / 'partition_builtins_under_test.inc').write_text(mount[start:mount.index('\nvoid mlog(', start)], encoding='utf-8')
        if os.name == 'nt':
            (build / 'linux').mkdir(); (build / 'sys').mkdir()
            (build / 'linux/types.h').write_text('#pragma once\n#include <stdint.h>\ntypedef uint32_t __u32;\ntypedef int32_t __s32;\n#define __aligned_u64 uint64_t __attribute__((aligned(8)))\n')
            (build / 'sys/ioctl.h').write_text('#pragma once\n#define _IOC(d,t,n,s) (((unsigned long)(d)<<30)|((unsigned long)(s)<<16)|((unsigned long)(t)<<8)|(n))\n#define _IO(t,n) _IOC(0,t,n,0)\n#define _IOR(t,n,s) _IOC(2,t,n,sizeof(s))\n#define _IOW(t,n,s) _IOC(1,t,n,sizeof(s))\n#define _IOWR(t,n,s) _IOC(3,t,n,sizeof(s))\n')
        output = build / ('partition_discovery_test.exe' if os.name == 'nt' else 'partition_discovery_test')
        subprocess.run([*compiler, *flags, str(root / 'userspace/ksud/tests/partition_discovery_test.cpp'),
                        '-o', str(output)], check=True)
        subprocess.run([str(output)], check=True, timeout=60)


if __name__ == '__main__':
    main()
