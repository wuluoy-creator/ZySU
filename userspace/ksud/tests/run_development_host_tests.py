"""Production development queries and conditional config save with injected OS boundaries.

Exercises actual parsing, locks, bounded reads, durable outcome handling and
namespace/controller flow. Does not establish Android/POSIX or power-loss behavior.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from run_module_preview_host_tests import function


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    src = root / 'userspace/ksud/sumhp/src'
    with tempfile.TemporaryDirectory(prefix='sumh-development-') as temporary:
        build = Path(temporary)
        def instrument(body):
            for call in ('open', 'fstat', 'read', 'close', 'stat', 'flock', 'mmap', 'munmap', 'setns'):
                body = body.replace('::' + call + '(', 'model::' + call + '(')
            body = body.replace('struct stat', 'model::Metadata')
            body = body.replace('S_ISREG(', 'model::is_regular(').replace('S_ISDIR(', 'model::is_directory(')
            return body.replace('std::chrono::steady_clock', 'model::Clock')
        config = (src / 'core/config.cpp').read_text(encoding='utf-8')
        parts = [config[config.index('std::string default_config_json() {'):config.index('\nnamespace {\nbool save_config(')]]
        for signature in ('std::vector<std::string> json_string_array_or_empty(', 'bool json_bool_or('):
            start = config.index(signature)
            parts.append(config[start:config.index('\n}  // namespace', start)])
        start = config.index('bool parse_config_json(')
        parts.append(config[start:config.index('\nbool read_config_file(', start)])
        parts += [function(config, name) for name in ('bool valid_config_revision(', 'bool parse_partition_config(', 'JsonValue save_partitions_conditional(')]
        start = config.index('class ConfigFileLock {')
        (build / 'development_config.inc').write_text(instrument(config[start:config.index('\nstd::string default_config_json()', start)]) + '\n' + '\n'.join(parts), encoding='utf-8')
        checked = (src / 'core/checked_document.hpp').read_text(encoding='utf-8')
        (build / 'development_checked.inc').write_text(instrument(checked[checked.index('namespace sumhp::checked {'):]), encoding='utf-8')
        body = (src / 'mount/development.cpp').read_text(encoding='utf-8')
        (build / 'development_controller.inc').write_text(instrument(body[body.index('namespace sumhp::mount {'):]), encoding='utf-8')
        backend = (src / 'mount/backend.cpp').read_text(encoding='utf-8')
        (build / 'development_paths.inc').write_text(function(backend, 'bool valid_stored_module_rule_path(') + function(backend, 'bool valid_module_rule_path('), encoding='utf-8')
        if os.name == 'nt':
            (build / 'linux').mkdir(); (build / 'sys').mkdir()
            (build / 'linux/types.h').write_text('#pragma once\n#include <stdint.h>\ntypedef uint32_t __u32;\ntypedef int32_t __s32;\n#define __aligned_u64 uint64_t __attribute__((aligned(8)))\n')
            (build / 'sys/ioctl.h').write_text('#pragma once\n#define _IOC(d,t,n,s) (((unsigned long)(d)<<30)|((unsigned long)(s)<<16)|((unsigned long)(t)<<8)|(n))\n#define _IO(t,n) _IOC(0,t,n,0)\n#define _IOR(t,n,s) _IOC(2,t,n,sizeof(s))\n#define _IOW(t,n,s) _IOC(1,t,n,sizeof(s))\n#define _IOWR(t,n,s) _IOC(3,t,n,sizeof(s))\n')
        compiler = [args.zig, 'c++'] if args.zig else [args.cxx]
        binary = build / ('development_test.exe' if os.name == 'nt' else 'development_test')
        subprocess.run([*compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DESTALE=116',
            '-I' + str(root), '-I' + str(root / 'userspace/ksud/sumhp/include'), '-I' + str(src), '-I' + str(build),
            str(root / 'userspace/ksud/tests/development_test.cpp'), str(src / 'mount/development_input.cpp'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == '__main__':
    main()
