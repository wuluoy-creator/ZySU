"""Test production module preview input/controller with deterministic boundary doubles.

Compiles actual config and module record/rule parsers. IO, hashing, namespace
worker, backend probes and SUMH compilation are injected; their separate suites
cover production internals. No Android/kernel/real POSIX behavior is claimed.
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
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    src = root / 'userspace/ksud/sumhp/src'
    with tempfile.TemporaryDirectory(prefix='sumh-module-preview-') as temporary:
        build = Path(temporary)
        body = (src / 'mount/module_preview.cpp').read_text(encoding='utf-8')
        body = body[body.index('namespace sumhp::mount {'):]
        for call in ('open', 'fstat', 'read', 'close', 'dup', 'stat', 'flock', 'mmap', 'munmap'):
            body = body.replace('::' + call + '(', 'preview_model::' + call + '(')
        body = body.replace('struct stat ', 'preview_model::Metadata ')
        body = body.replace('S_ISREG(', 'preview_model::is_regular(').replace('S_ISDIR(', 'preview_model::is_directory(')
        body = body.replace('std::chrono::steady_clock', 'preview_model::Clock')
        (build / 'module_preview_under_test.inc').write_text(body, encoding='utf-8')
        config = (src / 'core/config.cpp').read_text(encoding='utf-8')
        parts = [config[config.index('std::string default_config_json() {'):config.index('\nnamespace {\nbool save_config(')]]
        for signature in ('std::vector<std::string> json_string_array_or_empty(', 'bool json_bool_or('):
            start = config.index(signature)
            parts.append(config[start:config.index('\n}  // namespace', start)])
        start = config.index('bool parse_config_json(')
        parts.append(config[start:config.index('\nbool read_config_file(', start)])
        parts.append(function(config, 'bool parse_partition_config('))
        (build / 'module_preview_config_under_test.inc').write_text('\n'.join(parts), encoding='utf-8')
        backend = (src / 'mount/backend.cpp').read_text(encoding='utf-8')
        start = backend.index('struct ModuleConfigRecord {')
        parts = [backend[start:backend.index('\nbool valid_module_mode(', start)]]
        for signature in ('bool valid_module_mode(', 'bool valid_stored_module_rule_path(',
                          'JsonValue module_rules_json(', 'bool validate_module_rules(',
                          'bool parse_module_rules_value(', 'bool parse_module_document(',
                          'bool parse_module_configuration_records(', 'void export_module_configuration(',
                          'bool valid_module_id(', 'bool valid_module_rule_path(',
                          'bool parse_module_rules(', 'bool parse_module_configuration_documents(',
                          'bool validate_module_backend_rules('):
            parts.append(function(backend, signature))
        (build / 'module_preview_backend_under_test.inc').write_text('\n'.join(parts), encoding='utf-8')
        if os.name == 'nt':
            (build / 'linux').mkdir(); (build / 'sys').mkdir()
            (build / 'linux/types.h').write_text('#pragma once\n#include <stdint.h>\ntypedef uint32_t __u32;\ntypedef int32_t __s32;\n#define __aligned_u64 uint64_t __attribute__((aligned(8)))\n')
            (build / 'sys/ioctl.h').write_text('#pragma once\n#define _IOC(d,t,n,s) (((unsigned long)(d)<<30)|((unsigned long)(s)<<16)|((unsigned long)(t)<<8)|(n))\n#define _IO(t,n) _IOC(0,t,n,0)\n#define _IOR(t,n,s) _IOC(2,t,n,sizeof(s))\n#define _IOW(t,n,s) _IOC(1,t,n,sizeof(s))\n#define _IOWR(t,n,s) _IOC(3,t,n,sizeof(s))\n')
        compiler = [args.zig, 'c++'] if args.zig else [args.cxx]
        output = build / ('module_preview_test.exe' if os.name == 'nt' else 'module_preview_test')
        subprocess.run([*compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DESTALE=116',
            '-I' + str(root), '-I' + str(root / 'userspace/ksud/sumhp/include'),
            '-I' + str(src), '-I' + str(build),
            str(root / 'userspace/ksud/tests/module_preview_test.cpp'),
            str(src / 'mount/module_preview_input.cpp'), '-o', str(output)], check=True)
        subprocess.run([str(output)], check=True, timeout=60)


if __name__ == '__main__':
    main()
