"""Production hide definition commands/status with controlled store and ioctl boundaries.

The separate store runner checks durable file/lock transitions; this suite
counts selected kernel operations and asserts held serialization and no calls
after unsupported/stale/unconfirmed persistence. It never uses a real kernel.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig')
    parser.add_argument('--cxx',default=os.environ.get('CXX','c++'))
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[3]
    source=root/'userspace/ksud/sumhp'
    compiler=[args.zig,'c++'] if args.zig else [args.cxx]
    with tempfile.TemporaryDirectory(prefix='user-hide-controller-') as temp:
        build=Path(temp)
        body=(source/'src/core/user_hide.cpp').read_text(encoding='utf-8')
        body=body[body.index('namespace sumhp::hide {'):].replace('::getrandom(', 'fixture::getrandom(')
        (build/'user_hide_controller_under_test.inc').write_text(body,encoding='utf-8')
        if os.name=='nt':
            (build/'linux').mkdir(); (build/'sys').mkdir()
            (build/'linux/types.h').write_text('#pragma once\n#include <stdint.h>\ntypedef uint32_t __u32;\ntypedef int32_t __s32;\n#define __aligned_u64 uint64_t __attribute__((aligned(8)))\n')
            (build/'sys/ioctl.h').write_text('#pragma once\n#define _IOC(d,t,n,s) (((unsigned long)(d)<<30)|((unsigned long)(s)<<16)|((unsigned long)(t)<<8)|(n))\n#define _IO(t,n) _IOC(0,t,n,0)\n#define _IOR(t,n,s) _IOC(2,t,n,sizeof(s))\n#define _IOW(t,n,s) _IOC(1,t,n,sizeof(s))\n#define _IOWR(t,n,s) _IOC(3,t,n,sizeof(s))\n')
        binary=build/('user_hide_controller_test.exe' if os.name=='nt' else 'user_hide_controller_test')
        subprocess.run([*compiler,'-std=c++17','-Wall','-Wextra','-Werror',
            '-I'+str(root),'-I'+str(build),'-I'+str(source/'src'),'-I'+str(source/'include'),
            str(root/'userspace/ksud/tests/user_hide_controller_test.cpp'),str(source/'src/core/user_hide_model.cpp'),
            '-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=60)

if __name__=='__main__':
    main()
