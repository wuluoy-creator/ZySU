"""Production capability serializer and file reads with deterministic OS/kernel boundaries.

Uses production config parsing and runtime JSON. Not device/real namespace proof.
"""
import argparse
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
    parser.add_argument('--fixture')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    src = root / 'userspace/ksud/sumhp/src'
    with tempfile.TemporaryDirectory(prefix='sumh-capabilities-') as temporary:
        build = Path(temporary)
        body = (src / 'core/capability_snapshot.cpp').read_text(encoding='utf-8')
        body = body[body.index('namespace sumhp {'):]
        for call in ('open', 'fstat', 'read', 'close', 'stat', 'mmap', 'munmap'):
            body = body.replace('::' + call + '(', 'capability_model::' + call + '(')
        body = body.replace('struct stat ', 'capability_model::Metadata ')
        body = body.replace('S_ISREG(', 'capability_model::is_regular(')
        (build / 'capability_snapshot_under_test.inc').write_text(body, encoding='utf-8')
        config = (src / 'core/config.cpp').read_text(encoding='utf-8')
        parts = [config[config.index('std::string default_config_json() {'):config.index('\nnamespace {\nbool save_config(')]]
        for signature in ('std::vector<std::string> json_string_array_or_empty(', 'bool json_bool_or('):
            start = config.index(signature)
            parts.append(config[start:config.index('\n}  // namespace', start)])
        start = config.index('bool parse_config_json(')
        parts.append(config[start:config.index('\nbool read_config_file(', start)])
        parts.append(function(config, 'bool parse_partition_config('))
        (build / 'capability_config_under_test.inc').write_text('\n'.join(parts), encoding='utf-8')
        if os.name == 'nt':
            (build / 'linux').mkdir(); (build / 'sys').mkdir()
            (build / 'linux/types.h').write_text('#pragma once\n#include <stdint.h>\ntypedef uint32_t __u32;\ntypedef int32_t __s32;\n#define __aligned_u64 uint64_t __attribute__((aligned(8)))\n')
            (build / 'sys/ioctl.h').write_text('#pragma once\n#define _IOC(d,t,n,s) (((unsigned long)(d)<<30)|((unsigned long)(s)<<16)|((unsigned long)(t)<<8)|(n))\n#define _IO(t,n) _IOC(0,t,n,0)\n#define _IOR(t,n,s) _IOC(2,t,n,sizeof(s))\n#define _IOW(t,n,s) _IOC(1,t,n,sizeof(s))\n#define _IOWR(t,n,s) _IOC(3,t,n,sizeof(s))\n')
        compiler = [args.zig, 'c++'] if args.zig else [args.cxx]
        output = build / ('capability_snapshot_test.exe' if os.name == 'nt' else 'capability_snapshot_test')
        subprocess.run([*compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DESTALE=116',
            '-I' + str(root), '-I' + str(root / 'userspace/ksud/sumhp/include'),
            '-I' + str(src), '-I' + str(build),
            str(root / 'userspace/ksud/tests/capability_snapshot_test.cpp'), '-o', str(output)], check=True)
        subprocess.run([str(output), *([str(Path(args.fixture).resolve())] if args.fixture else [])], check=True, timeout=30)


if __name__ == '__main__':
    main()
