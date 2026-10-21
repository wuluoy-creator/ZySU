"""Run SUMH client, restoration and persistence regressions with a host C++17 compiler.

Linux: python3 userspace/ksud/tests/run_sumh_host_tests.py
Windows: python userspace/ksud/tests/run_sumh_host_tests.py --zig /path/to/zig.exe
The tests use a fake ioctl and do not require a running SUMH kernel or root.
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--zig", help="Use Zig's bundled C++ compiler")
    parser.add_argument("--temporary-maps-fixtures", help="Write actual temporary Maps Manager callback JSON fixtures")
    args = parser.parse_args()
    compiler = [args.zig, "c++"] if args.zig else [args.cxx]
    root = Path(__file__).resolve().parents[3]
    tests = root / "userspace/ksud/tests"
    sumhp = root / "userspace/ksud/sumhp"

    with tempfile.TemporaryDirectory(prefix="sumh-host-tests-") as temporary:
        build = Path(temporary)
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I" + str(root), "-I" + str(sumhp / "include"),
                 "-I" + str(sumhp / "src"), "-I" + str(root / "userspace/ksud/src"),
                 "-I" + str(build)]
        transport = (root / "userspace/common/maps_preview_transport.cpp").read_text(encoding="utf-8")
        begin = transport.index("std::string envelope(")
        (build / "temporary_maps_envelope_under_test.inc").write_text(
            transport[begin:transport.index("\n}  // namespace", begin)], encoding="utf-8")
        if os.name == "nt":
            # Linux-only errno values used by the fake ioctl transport.
            flags.extend(["-DESTALE=116", "-DESHUTDOWN=108"])
            # Only the fake transport uses these headers. Production UAPI and
            # Android builds continue to use their real Linux definitions.
            (build / "linux").mkdir()
            (build / "sys").mkdir()
            (build / "linux/types.h").write_text(
                "#pragma once\n#include <stdint.h>\n"
                "typedef uint32_t __u32;\ntypedef int32_t __s32;\n"
                "#define __aligned_u64 uint64_t __attribute__((aligned(8)))\n")
            (build / "sys/ioctl.h").write_text(
                "#pragma once\n"
                "#define _IOC(d,t,n,s) (((unsigned long)(d)<<30)|((unsigned long)(s)<<16)|"
                "((unsigned long)(t)<<8)|(n))\n"
                "#define _IO(t,n) _IOC(0,t,n,0)\n"
                "#define _IOR(t,n,s) _IOC(2,t,n,sizeof(s))\n"
                "#define _IOW(t,n,s) _IOC(1,t,n,sizeof(s))\n"
                "#define _IOWR(t,n,s) _IOC(3,t,n,sizeof(s))\n")

        source = (sumhp / "src/mount/sumh.cpp").read_text(encoding="utf-8")
        begin = source.index("bool restore_managed_maps_rules(")
        end = source.index("\nbool apply_maps_config(", begin)
        (build / "sumh_maps_restore_under_test.inc").write_text(source[begin:end])
        begin = end + 1
        end = source.index("\nbool apply_feature_config(", begin)
        (build / "sumh_maps_apply_under_test.inc").write_text(source[begin:end])
        maps_source = (sumhp / "src/mount/maps_snapshot.cpp").read_text(encoding="utf-8")
        begin = maps_source.index("MapsFileIdentity read_maps_identity(")
        end = maps_source.index("\nnamespace {", begin)
        (build / "sumh_maps_identity_under_test.inc").write_text(maps_source[begin:end])
        (build / "sumh_maps_snapshot_under_test.inc").write_text(
            maps_source[maps_source.index("namespace sumhp::mount::sumh {"):])
        namespace_source = (sumhp / "src/mount/mount_fs.cpp").read_text(encoding="utf-8")
        begin = namespace_source.index("const std::vector<std::string>& managed_partitions() {")
        end = namespace_source.index("\nvoid mlog(", begin)
        (build / "sumh_partitions_under_test.inc").write_text(namespace_source[begin:end])
        begin = namespace_source.index("bool run_in_init_mount_ns(")
        end = namespace_source.index("\nstd::string partition_mount_point(", begin)
        (build / "sumh_namespace_result_under_test.inc").write_text(namespace_source[begin:end])
        split = namespace_source.index("bool run_in_init_mount_ns(const std::function<bool()>& fn,", begin)
        (build / "sumh_namespace_child_under_test.inc").write_text(namespace_source[begin:split])
        (build / "sumh_namespace_pipe_under_test.inc").write_text(namespace_source[split:end])
        begin = source.index("bool restore_user_hide_rules(")
        end = source.index("\n}  // namespace", begin)
        (build / "sumh_restore_under_test.inc").write_text(source[begin:end])
        begin = source.index("bool disable_kernel_features(")
        end = source.index("\n}  // namespace", begin)
        features = source[begin:end]
        begin = source.index("bool apply_maps_config(")
        end = source.index("\nbool reset_feature_state(", begin)
        (build / "sumh_build_features_under_test.inc").write_text(features + source[begin:end])
        source = (sumhp / "src/mount/backend.cpp").read_text(encoding="utf-8")
        begin = source.index("namespace {\nbool dir_has_direct_files")
        end = source.index("\nMountReport mount_all_enabled(", begin)
        (build / "sumh_backend_selection_under_test.inc").write_text(source[begin:end])
        guard_begin = source.index("bool validate_module_backend_rules(")
        (build / "sumh_backend_rule_guard_under_test.inc").write_text(source[guard_begin:end])
        begin = source.index("namespace {\nstruct ModuleConfigRecord")
        end = source.index("\nnamespace {\nbool dir_has_direct_files", begin)
        (build / "sumh_module_config_under_test.inc").write_text(source[begin:end])
        begin = source.index("bool apply_sumh_features(")
        end = source.index("\n}  // namespace", begin)
        (build / "sumh_backend_features_under_test.inc").write_text(source[begin:end])
        begin = source.index("MountReport mount_all_enabled(")
        end = source.index("\nbool unmount_all(", begin)
        lifecycle = source[begin:end]
        begin = source.index("bool refresh_sumh_modules(")
        end = source.index("\n}  // namespace sumhp::mount", begin)
        (build / "sumh_backend_lifecycle_under_test.inc").write_text(lifecycle + source[begin:end])
        source = (sumhp / "src/mount/recovery.cpp").read_text(encoding="utf-8")
        begin = source.index("namespace sumhp::mount {")
        (build / "sumh_recovery_under_test.inc").write_text(source[begin:])
        source = (sumhp / "src/core/config.cpp").read_text(encoding="utf-8")
        begin = source.index("json::Value maps_path_rules_json(")
        end = source.index("\nnamespace {\nbool parse_maps_path_rules(", begin)
        (build / "sumh_maps_validate_under_test.inc").write_text(source[begin:end])
        begin = source.index("bool load_user_hide_rules(std::vector<std::string>& rules,")
        end = source.index("\nstd::vector<std::string> load_user_hide_rules()", begin)
        (build / "sumh_persistence_under_test.inc").write_text(source[begin:end])
        hide_store = (sumhp / "src/core/user_hide_store.cpp").read_text(encoding="utf-8")
        start = hide_store.index("std::filesystem::path store_path(")
        helpers = hide_store[start:hide_store.index("\nbool current_matches(", start)]
        start = hide_store.index("bool read_store(")
        (build / "sumh_hide_read_under_test.inc").write_text("namespace sumhp::hide {\nnamespace {\n" + helpers +
            "\n}\n" + hide_store[start:hide_store.index("\nstruct Transaction::Impl", start)] + "\n}\n", encoding="utf-8")
        # Hash only is a deterministic boundary double; model/ID framing stays production.
        (build / "user_hide_digest_fixture.inc").write_text('''
namespace sumhp::state {
std::string revision_for_bytes(const std::string& bytes) {
    std::uint64_t hash=1469598103934665603ULL;
    for(const unsigned char c:bytes) hash=(hash^c)*1099511628211ULL;
    static constexpr char hex[]="0123456789abcdef";
    std::string result="sha256-v1:";
    for(unsigned i=0;i<32;++i) { const auto c=static_cast<unsigned char>((hash>>((i%8)*8))^i);
        result+=hex[c>>4]; result+=hex[c&15]; }
    return result;
}
}
''', encoding="utf-8")
        sections = []
        begin = source.index("std::string default_config_json() {")
        end = source.index("\njson::Value maps_path_rules_json(", begin)
        (build / "sumh_config_document_under_test.inc").write_text(source[begin:end])
        end = source.index("\nnamespace {\nbool save_config", begin)
        sections.append(source[begin:end])
        begin = source.index("bool save_config_document(")
        end = source.index("\nnamespace {\nstd::vector<std::string> json_string_array_or_empty(", begin)
        sections.append(source[begin:end])
        for signature in ["std::vector<std::string> json_string_array_or_empty(",
                          "bool json_bool_or("]:
            begin = source.index(signature)
            end = source.index("\n}  // namespace", begin)
            sections.append(source[begin:end])
        begin = source.index("bool parse_config_json(")
        end = source.index("\n}  // namespace sumhp", begin)
        existing_config = source[begin:end]
        # The dedicated CAS workflow has its own production controller suite.
        start_cas = existing_config.index("bool valid_config_revision(")
        end_cas = existing_config.index("bool reset_kernel_build_config(", start_cas)
        sections.append(existing_config[:start_cas] + existing_config[end_cas:])
        (build / "sumh_build_config_under_test.inc").write_text("\n".join(sections))
        source = (root / "userspace/ksud/src/boot/boot_patch.cpp").read_text(encoding="utf-8")
        begin = source.index("std::string read_kernel_release() {")
        end = source.index("\n}  // namespace", begin)
        (build / "sumh_kmi_under_test.inc").write_text(source[begin:end])
        source = (sumhp / "src/core/command.cpp").read_text(encoding="utf-8")
        begin = source.index("int handle_recovery(")
        end = source.index("\n}  // namespace", begin)
        (build / "sumh_recovery_command_under_test.inc").write_text(source[begin:end])
        begin = source.index("bool read_mount_config(")
        end = source.index("\nint print_storage_json(", begin)
        (build / "sumh_hot_mount_under_test.inc").write_text(source[begin:end])
        begin = source.index('    if (sub == "configure") {', source.index("int handle_module("))
        end = source.index('    if (sub == "hot-mount"', begin)
        (build / "sumh_module_commands_under_test.inc").write_text(source[begin:end])
        begin = source.index("int apply_config_file() {")
        end = source.index("\nvoid print_backend_statuses_json()", begin)
        (build / "sumh_config_apply_under_test.inc").write_text(source[begin:end])
        begin = source.index("bool is_query(")
        end = source.index("\nstd::string operation_description(", begin)
        development = (sumhp / "src/mount/development_input.cpp").read_text(encoding="utf-8")
        matcher = development[development.index("bool is_development_prefix("):development.index("\nbool parse_development_arguments(")]
        (build / "sumh_query_under_test.inc").write_text("namespace mount {\n" + matcher + "}\n" + source[begin:end])
        begin = source.index("int handle_sumh(")
        end = source.index("\nint handle_hide(", begin)
        (build / "sumh_build_cli_under_test.inc").write_text(source[begin:end])
        source = (sumhp / "src/embedded.cpp").read_text(encoding="utf-8")
        begin = source.index("void embedded_restore_kernel_build(bool boot_completed) {")
        end = source.index("\nvoid embedded_post_fs_data()", begin)
        (build / "sumh_build_boot_under_test.inc").write_text(source[begin:end])
        source = (root / "userspace/ksud/src/core/ksucalls.cpp").read_text(encoding="utf-8")
        begin = source.index("auto init_driver_fd() -> int {")
        end = source.index("\n}  // namespace", begin)
        (build / "sumh_driver_probe_under_test.inc").write_text(source[begin:end])
        source = (sumhp / "src/core/migration.cpp").read_text(encoding="utf-8")
        begin = source.index("namespace sumhp {") + len("namespace sumhp {")
        end = source.index("\n}  // namespace sumhp", begin)
        (build / "sumh_migration_under_test.inc").write_text(source[begin:end])

        for name, sources in [
            ("sumh_client_test", [tests / "sumh_client_test.cpp",
                                    sumhp / "src/mount/sumh_client.cpp"]),
            ("sumh_runtime_test", [tests / "sumh_runtime_test.cpp",
                                    sumhp / "src/mount/sumh_client.cpp"]),
            ("sumh_inventory_test", [tests / "sumh_inventory_test.cpp",
                                    sumhp / "src/mount/sumh_client.cpp"]),
            ("sumh_maps_temporary_test", [tests / "sumh_maps_temporary_test.cpp",
                                         sumhp / "src/mount/sumh_client.cpp"]),
            ("temporary_maps_manager_test", [tests / "temporary_maps_manager_test.cpp",
                                              sumhp / "src/mount/sumh_client.cpp"]),
            ("sumh_kernel_build_apply_test", [tests / "sumh_kernel_build_apply_test.cpp",
                                               sumhp / "src/mount/sumh_client.cpp"]),
            ("sumh_restore_test", [tests / "sumh_restore_test.cpp", sumhp / "src/core/user_hide_model.cpp"]),
            ("sumh_backend_lifecycle_test", [tests / "sumh_backend_lifecycle_test.cpp"]),
            ("sumh_recovery_test", [tests / "sumh_recovery_test.cpp"]),
            ("sumh_backend_selection_test", [tests / "sumh_backend_selection_test.cpp"]),
            ("sumh_hot_mount_test", [tests / "sumh_hot_mount_test.cpp"]),
            ("sumh_module_config_test", [tests / "sumh_module_config_test.cpp"]),
            ("sumh_maps_restore_test", [tests / "sumh_maps_restore_test.cpp"]),
            ("sumh_maps_snapshot_test", [tests / "sumh_maps_snapshot_test.cpp"]),
            ("sumh_namespace_result_test", [tests / "sumh_namespace_result_test.cpp"]),
            ("sumh_persistence_test", [tests / "sumh_persistence_test.cpp", sumhp / "src/core/user_hide_model.cpp"]),
            ("sumh_migration_test", [tests / "sumh_migration_test.cpp", sumhp / "src/core/user_hide_model.cpp"]),
            ("sumh_driver_probe_test", [tests / "sumh_driver_probe_test.cpp"]),
            ("sumh_build_config_test", [tests / "sumh_build_config_test.cpp",
                                          root / "userspace/ksud/src/cli_args.cpp",
                                          root / "userspace/ksud/src/terminal.cpp"]),
        ]:
            binary = build / (name + (".exe" if os.name == "nt" else ""))
            # The production CLI table deliberately value-initializes omitted
            # option fields; keep unrelated warning policy out of this fixture.
            extra_flags = ["-Wno-missing-field-initializers"] if name == "sumh_build_config_test" else []
            subprocess.run([*compiler, *flags, *extra_flags, *map(str, sources), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            if name == "temporary_maps_manager_test" and args.temporary_maps_fixtures:
                fixture_binary = build / ("temporary_maps_android_fixtures" + (".exe" if os.name == "nt" else ""))
                subprocess.run([*compiler, *flags, "-DSUMH_TEMPORARY_MAPS_ANDROID_ERRNO=1",
                    "-include", str(tests / "temporary_maps_android_errno.hpp"),
                    *map(str, sources), "-o", str(fixture_binary)], check=True)
                subprocess.run([str(fixture_binary), str(Path(args.temporary_maps_fixtures).resolve())], check=True)


if __name__ == "__main__":
    main()
