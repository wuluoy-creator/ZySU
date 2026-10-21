"""Actual diagnostic orchestration and bounded I/O with controlled source/OS services.

The pure model, real producer schemas, transport and POSIX lifetime are separate
checks. These tests never enter a real namespace or issue a kernel mutation.
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
    source = root / 'userspace/ksud/sumhp'
    compiler = [args.zig, 'c++'] if args.zig else [args.cxx]
    with tempfile.TemporaryDirectory(prefix='diagnostics-collector-') as temporary:
        build = Path(temporary)
        body = (source / 'src/core/diagnostics.cpp').read_text(encoding='utf-8')
        body = body[body.index('namespace sumhp::diagnostics {'):]
        for name in ('open', 'close', 'fstat', 'stat', 'setns', 'clock_gettime'):
            body = body.replace('::' + name + '(', 'diagnostic_model::' + name + '(')
        body = body.replace('struct stat', 'diagnostic_model::Metadata')
        (build / 'diagnostics_collector_under_test.inc').write_text(body, encoding='utf-8')
        (build / 'diagnostics_hide_collector_under_test.inc').write_text(
            body.replace('diagnostic_model::', 'integration_os::'), encoding='utf-8')
        hide_body = (source / 'src/core/user_hide.cpp').read_text(encoding='utf-8')
        hide_body = hide_body[hide_body.index('namespace sumhp::hide {'):].replace('::getrandom(', 'fixture::getrandom(')
        (build / 'user_hide_controller_under_test.inc').write_text(hide_body, encoding='utf-8')
        services = (root / 'userspace/ksud/tests/user_hide_controller_test.cpp').read_text(encoding='utf-8')
        marker = '#include "user_hide_controller_under_test.inc"'
        assert services.count(marker) == 1
        (build / 'diagnostics_hide_services.inc').write_text(services[:services.index(marker)] + marker + '\n', encoding='utf-8')
        io = (source / 'src/core/diagnostics_io.cpp').read_text(encoding='utf-8')
        io = io[io.index('namespace sumhp::diagnostics {'):]
        for name in ('open', 'close', 'fstat', 'fcntl', 'poll', 'read'):
            io = io.replace('::' + name + '(', 'io_model::' + name + '(')
        io = io.replace('struct stat', 'struct InputStat').replace('std::chrono::steady_clock', 'io_model::Clock')
        (build / 'diagnostics_io_under_test.inc').write_text(io, encoding='utf-8')
        # Reuse the established I/O fault services/assertions against the new
        # actual bounded reader, then add its distinct path and capacity cases.
        test = (root / 'userspace/ksud/tests/operation_input_io_test.cpp').read_text(encoding='utf-8')
        test = test.replace('core/operation_submit.hpp', 'core/diagnostics.hpp')
        test = test.replace('operation_input_io_under_test.inc', 'diagnostics_io_under_test.inc')
        test = test.replace('sumhp::state::read_operation_input', 'sumhp::diagnostics::read_input')
        test = test.replace('sumhp::state::OPERATION_INPUT_BYTES', 'sumhp::diagnostics::input_bytes')
        test = test.replace('read_operation_input', 'read_input').replace('OPERATION_INPUT_BYTES', 'input_bytes')
        markers = list(re.finditer(r'(?m)^    std::cout\s*<<\s*assertions\s*<<', test))
        assert len(markers) == 1
        marker = markers[0].group()
        extra = '''    for (const auto& path : std::vector<std::string>{"", "relative", std::string("/nul\\0x",6), std::string("/bad\\xff",5), "/"+std::string(4095,'a')}) {
        reset(); bytes="unchanged"; check(!read_input(path,bytes,error) && error=="input_path_invalid" && bytes=="unchanged" && opens==0 && dup_count==0);
    }
'''
        test = test.replace(marker, extra + marker).replace('operation input IO assertions', 'diagnostics input IO assertions')
        (build / 'diagnostics_io_test.cpp').write_text(test, encoding='utf-8')
        if os.name == 'nt':
            (build / 'linux').mkdir(); (build / 'sys').mkdir()
            (build / 'linux/types.h').write_text('#pragma once\n#include <stdint.h>\ntypedef uint32_t __u32;\ntypedef int32_t __s32;\n#define __aligned_u64 uint64_t __attribute__((aligned(8)))\n')
            (build / 'sys/ioctl.h').write_text('#pragma once\n#define _IOC(d,t,n,s) (((unsigned long)(d)<<30)|((unsigned long)(s)<<16)|((unsigned long)(t)<<8)|(n))\n#define _IO(t,n) _IOC(0,t,n,0)\n#define _IOR(t,n,s) _IOC(2,t,n,sizeof(s))\n#define _IOW(t,n,s) _IOC(1,t,n,sizeof(s))\n#define _IOWR(t,n,s) _IOC(3,t,n,sizeof(s))\n')
        for name, path in (
            ('collector', root / 'userspace/ksud/tests/diagnostics_collector_test.cpp'),
            ('io', build / 'diagnostics_io_test.cpp'),
            ('hide-integration', root / 'userspace/ksud/tests/diagnostics_hide_integration_test.cpp'),
        ):
            binary = build / (name + ('.exe' if os.name == 'nt' else ''))
            extra_sources = [str(source / ('src/' + part)) for part in
                ('core/user_hide_model.cpp', 'core/diagnostics_model.cpp', 'core/operation_query.cpp')] if name == 'hide-integration' else []
            subprocess.run([*compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-DESTALE=116', '-I' + str(root), '-I' + str(source / 'src'),
                '-I' + str(source / 'include'), '-I' + str(build), str(path), *extra_sources, '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == '__main__':
    main()
