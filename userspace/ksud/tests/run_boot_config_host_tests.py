"""Exercise the production C++ boot config, argument parsers and embedded LKM writer."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def between(source, begin, end):
    start = source.index(begin)
    return source[start:source.index(end, start)]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--zig", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    compiler = [str(args.zig), "c++"] if args.zig else [args.cxx]
    tests = Path(__file__).resolve().parent
    root = tests.parents[2]
    src = tests.parent / "src"
    legacy = (src / "boot/boot_patch.cpp").read_text(encoding="utf-8")
    direct = (src / "boot/boot_patch_v2.cpp").read_text(encoding="utf-8")
    features = (src / "core/feature.cpp").read_text(encoding="utf-8")
    utilities = (src / "utils.cpp").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="ksud-boot-config-") as temporary:
        build = Path(temporary)
        extracts = {
            "imgpatch_config_under_test.inc": between(legacy, "bool inject_imgpatch_config_to_lkm(", "// Execute magiskboot"),
            "boot_config_legacy_parser.inc": between(legacy, "struct BootPatchArgs", "enum class DirectLkmRestoreStatus") + "\n}\n",
            "boot_config_direct_parser.inc": between(direct, "struct BootPatchV2Args", "std::optional<std::vector<std::uint8_t>> read_binary"),
            "boot_config_ramdisk_under_test.inc": between(legacy, '    const std::string config_text = parsed.module_params(', '    if (magiskboot_query({"cpio", ramdisk, "exists ksu_allow_shell"})'),
            "feature_persistence_under_test.inc": function(utilities, 'bool parse_uint32(') + '\n'.join(function(features, signature) for signature in (
                'const std::map<std::string, uint32_t>& get_feature_map()',
                'std::pair<uint32_t, bool> parse_feature_id(',
                'bool is_fixed_feature_id(', 'bool is_configurable_feature_id(',
                'bool discard_non_configurable_features(', 'void ensure_forced_sulog_running()',
                'std::map<uint32_t, uint64_t> get_current_feature_values()',
                'bool is_sucompat_feature_id(', 'int feature_set_impl(', 'int feature_set(',
                'int apply_config_locked(', 'int feature_load_config()',
            )),
        }
        for name, content in extracts.items():
            (build / name).write_text(content, encoding="utf-8")
        if os.name == "nt":
            (build / "linux").mkdir()
            (build / "linux/types.h").write_text("#include <stdint.h>\ntypedef uint64_t __u64;\ntypedef uint32_t __u32;\n")
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        for name in ('boot_config_test', 'feature_forced_test'):
            binary = build / (name + (".exe" if os.name == "nt" else ""))
            subprocess.run([*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", *flags,
                            "-I" + str(build), "-I" + str(src), "-I" + str(root),
                            str(tests / (name + '.cpp')), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
