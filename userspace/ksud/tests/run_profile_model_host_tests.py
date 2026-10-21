"""Run actual portable profile model and extracted production resource validators.

Only the legacy-hide identity hash is a deterministic boundary. No filesystem,
kernel, cryptographic strength or cross-resource atomicity is tested here.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def extract_function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    cursor = brace + 1
    # Selected production functions have balanced braces in string literals.
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[start:cursor] + '\n'


def write_validators(root, build):
    source = root / 'userspace/ksud/sumhp/src'
    config = (source / 'core/config.cpp').read_text(encoding='utf-8')
    start = config.index('std::string default_config_json()')
    end = config.index('\nnamespace {\nbool save_config(', start)
    code = config[start:end]
    code += extract_function(config, 'std::vector<std::string> json_string_array_or_empty(')
    start = config.index('namespace {\nbool json_bool_or(')
    end = config.index('\nbool read_config_file(', start)
    code += config[start:end]
    (build / 'profile_config_validators.inc').write_text(code, encoding='utf-8')
    backend = (source / 'mount/backend.cpp').read_text(encoding='utf-8')
    code = 'struct ModuleConfigRecord { std::string mode = "auto"; std::vector<ModuleRule> rules; };\n'
    code += 'using ModuleConfiguration = std::map<std::string, ModuleConfigRecord>;\n'
    for signature in ('bool valid_module_mode(', 'bool valid_stored_module_rule_path(',
                      'JsonValue module_rules_json(', 'bool validate_module_rules(',
                      'bool parse_module_rules_value(', 'bool parse_module_document(',
                      'bool parse_module_configuration_records(', 'void export_module_configuration(',
                      'bool valid_module_id(', 'bool valid_module_rule_path(',
                      'bool parse_module_rules(', 'bool parse_module_configuration_documents('):
        code += extract_function(backend, signature)
    (build / 'profile_module_validators.inc').write_text(code, encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    source = root / 'userspace/ksud/sumhp'
    with tempfile.TemporaryDirectory(prefix='profile-model-tests-') as tmp:
        build = Path(tmp)
        if os.name == 'nt':
            (build / 'linux').mkdir(); (build / 'sys').mkdir()
            (build / 'linux/types.h').write_text('#pragma once\n#include <stdint.h>\ntypedef uint32_t __u32;\ntypedef int32_t __s32;\n#define __aligned_u64 uint64_t __attribute__((aligned(8)))\n')
            (build / 'sys/ioctl.h').write_text('#pragma once\n#define _IOC(d,t,n,s) (((unsigned long)(d)<<30)|((unsigned long)(s)<<16)|((unsigned long)(t)<<8)|(n))\n#define _IO(t,n) _IOC(0,t,n,0)\n#define _IOR(t,n,s) _IOC(2,t,n,sizeof(s))\n#define _IOW(t,n,s) _IOC(1,t,n,sizeof(s))\n#define _IOWR(t,n,s) _IOC(3,t,n,sizeof(s))\n')
        write_validators(root, build)
        compiler = [args.zig, 'c++'] if args.zig else [args.cxx]
        binary = build / ('profile_model_test.exe' if os.name == 'nt' else 'profile_model_test')
        subprocess.run([*compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I' + str(root), '-I' + str(source / 'include'), '-I' + str(source / 'src'), '-I' + str(build),
            str(root / 'userspace/ksud/tests/profile_model_test.cpp'),
            str(source / 'src/core/profile_model.cpp'), str(source / 'src/core/user_hide_model.cpp'),
            '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == '__main__':
    main()
