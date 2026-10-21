"""Test the production Maps submission model and storage/ioctl ordering with fault injection."""
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
    with tempfile.TemporaryDirectory(prefix='maps-history-tests-') as tmp:
        build = Path(tmp)
        if os.name == 'nt':
            (build/'linux').mkdir(); (build/'sys').mkdir()
            (build/'linux/types.h').write_text('#pragma once\n#include <stdint.h>\ntypedef uint32_t __u32;\ntypedef int32_t __s32;\n#define __aligned_u64 uint64_t __attribute__((aligned(8)))\n')
            (build/'sys/ioctl.h').write_text('#pragma once\n#define _IOC(d,t,n,s) (((unsigned long)(d)<<30)|((unsigned long)(s)<<16)|((unsigned long)(t)<<8)|(n))\n#define _IO(t,n) _IOC(0,t,n,0)\n#define _IOR(t,n,s) _IOC(2,t,n,sizeof(s))\n#define _IOW(t,n,s) _IOC(1,t,n,sizeof(s))\n#define _IOWR(t,n,s) _IOC(3,t,n,sizeof(s))\n')
        content = (source / 'src/mount/maps_history.cpp').read_text(encoding='utf-8')
        (build / 'maps_history_under_test.inc').write_text(content[content.index('namespace sumhp::mount::sumh {'):], encoding='utf-8')
        config = (source / 'src/core/config.cpp').read_text(encoding='utf-8')
        begin = config.index('json::Value maps_path_rules_json(')
        end = config.index('\nnamespace {\nbool parse_maps_path_rules(', begin)
        (build / 'maps_validation_under_test.inc').write_text(config[begin:end], encoding='utf-8')
        compiler = [args.zig, 'c++'] if args.zig else [args.cxx]
        binary = build / ('maps_history_test.exe' if os.name == 'nt' else 'maps_history_test')
        subprocess.run([*compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I'+str(root), '-I'+str(source/'include'), '-I'+str(source/'src'), '-I'+str(build),
            str(root/'userspace/ksud/tests/maps_history_test.cpp'),
            str(source/'src/mount/maps_history_model.cpp'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
