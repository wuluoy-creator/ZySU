"""Exercise production Maps preview routing/observation with injected read-only IO.

Compiles the actual config parser, managed comparison and receipt encoder.
Filesystem, sampler, boot/history reads and transport guard are deterministic
fixtures. No device/kernel behavior or storage durability is claimed.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    src = root / "userspace/ksud/sumhp/src"
    with tempfile.TemporaryDirectory(prefix="sumh-maps-preview-") as temporary:
        build = Path(temporary)
        body = (src / "mount/maps_preview.cpp").read_text(encoding="utf-8")
        body = body[body.index("namespace sumhp::mount::sumh {"):]
        for call in ("open", "fstat", "read", "close"):
            body = body.replace("::" + call + "(", "preview_model::" + call + "(")
        body = body.replace("struct stat metadata", "preview_model::Metadata metadata")
        body = body.replace("S_ISREG(", "preview_model::is_regular(")
        body = body.replace("std::chrono::steady_clock", "preview_model::Clock")
        (build / "maps_preview_under_test.inc").write_text(body, encoding="utf-8")
        config = (src / "core/config.cpp").read_text(encoding="utf-8")
        parts = [config[config.index("std::string default_config_json() {"):
                        config.index("\nnamespace {\nbool save_config(")]]
        for signature in ("std::vector<std::string> json_string_array_or_empty(",
                          "bool json_bool_or("):
            start = config.index(signature)
            parts.append(config[start:config.index("\n}  // namespace", start)])
        start = config.index("bool parse_config_json(")
        parts.append(config[start:config.index("\nbool read_config_file(", start)])
        (build / "maps_preview_config_under_test.inc").write_text("\n".join(parts), encoding="utf-8")
        body = (src / "mount/maps_snapshot.cpp").read_text(encoding="utf-8")
        (build / "maps_preview_comparison_under_test.inc").write_text(
            body[body.index("MapsComparison compare_maps("):], encoding="utf-8")
        body = (src / "embedded.cpp").read_text(encoding="utf-8")
        body = body[body.index("int embedded_command("):body.index("\nint embedded_mount(")]
        body = body.replace("geteuid()", "preview_model::geteuid()")
        (build / "maps_preview_embedded_under_test.inc").write_text(body, encoding="utf-8")
        body = (src / "core/command.cpp").read_text(encoding="utf-8")
        start = body.index('        if (op == "preview") {', body.index("int handle_sumh("))
        end = body.index('        if (op == "list-paths"', start)
        (build / "maps_preview_cli_under_test.inc").write_text(body[start:end], encoding="utf-8")
        start = body.index("bool is_query(")
        (build / "maps_preview_query_under_test.inc").write_text(
            body[start:body.index("\nstd::string operation_description(", start)], encoding="utf-8")
        if os.name == "nt":
            (build / "linux").mkdir(); (build / "sys").mkdir()
            (build / "linux/types.h").write_text("#pragma once\n#include <stdint.h>\ntypedef uint32_t __u32;\ntypedef int32_t __s32;\n#define __aligned_u64 uint64_t __attribute__((aligned(8)))\n")
            (build / "sys/ioctl.h").write_text("#pragma once\n#define _IOC(d,t,n,s) (((unsigned long)(d)<<30)|((unsigned long)(s)<<16)|((unsigned long)(t)<<8)|(n))\n#define _IO(t,n) _IOC(0,t,n,0)\n#define _IOR(t,n,s) _IOC(2,t,n,sizeof(s))\n#define _IOW(t,n,s) _IOC(1,t,n,sizeof(s))\n#define _IOWR(t,n,s) _IOC(3,t,n,sizeof(s))\n")
        compiler = [args.zig, "c++"] if args.zig else [args.cxx]
        if os.name == "nt":
            compiler += ["-DESTALE=116", "-DESHUTDOWN=108"]
        output = build / ("maps_preview_test.exe" if os.name == "nt" else "maps_preview_test")
        subprocess.run([*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-I" + str(root), "-I" + str(root / "userspace/ksud/sumhp/include"),
            "-I" + str(src), "-I" + str(build),
            str(root / "userspace/ksud/tests/maps_preview_test.cpp"),
            str(src / "mount/maps_history_model.cpp"), "-o", str(output)], check=True)
        subprocess.run([str(output)], check=True, timeout=30)


if __name__ == "__main__":
    main()
