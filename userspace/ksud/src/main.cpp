#include <cstring>
#include <optional>
#include "cli.hpp"
#include "manager_helper.hpp"

#if defined(MAGISKBOOT_ALONE_AVAILABLE) && MAGISKBOOT_ALONE_AVAILABLE
extern int magiskboot_main(int argc, char** argv);
#endif  // #if defined(MAGISKBOOT_ALONE_AVAILABLE)...
#if defined(BOOTCTL_ALONE_AVAILABLE) && BOOTCTL_ALONE_AVAILABLE
extern "C" int bootctl_main(int argc, char** argv);
#endif  // #if defined(BOOTCTL_ALONE_AVAILABLE) &&...
#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
extern "C" int resetprop_main(int argc, char** argv);
#endif  // #if defined(RESETPROP_ALONE_AVAILABLE) ...
#if defined(MKBOOTFS_ALONE_AVAILABLE) && MKBOOTFS_ALONE_AVAILABLE
#include <mkbootfs/mkbootfs.h>
#endif  // #if defined(MKBOOTFS_ALONE_AVAILABLE) ...
#if defined(NDK_BUSYBOX_AVAILABLE) && NDK_BUSYBOX_AVAILABLE
extern "C" int busybox_main(int argc, char** argv);
#endif  // #if defined(NDK_BUSYBOX_AVAILABLE) && N...
#if defined(TOYBOX_READELF_AVAILABLE) && TOYBOX_READELF_AVAILABLE
extern "C" int zysu_toybox_main(int argc, char** argv);
#endif  // #if defined(TOYBOX_READELF_AVAILABLE) &&...
namespace {
const char* path_basename(const char* path) {
    const char* base = path;
    for (; *path; ++path)
        if (*path == '/')
            base = path + 1;
    return base;
}
}  // namespace

int main(int argc, char** argv) {
    // Dispatch by argv[0] basename (e.g. when invoked as /data/adb/ksu/bin/magiskboot ->
    // magiskboot)
    const char* base = (argc >= 1 && argv[0]) ? path_basename(argv[0]) : nullptr;
    // When invoked via the Manager's packaged .so path, argv[0] is that path; app passes tool
    // name as argv[1]
    const char* first_arg = (argc >= 2 && argv[1]) ? argv[1] : nullptr;

    const bool dispatcher = base && (std::strcmp(base, "ksud") == 0 || std::strstr(base, ".so"));
    if (dispatcher && first_arg && std::strcmp(first_arg, "manager-helper") == 0)
        return argc == 2 ? ksud::manager_helper_main() : 2;
    auto dispatch = [base, first_arg, dispatcher, argc, argv](const char* name,
                                                              auto main_fn) -> std::optional<int> {
        if (base && std::strcmp(base, name) == 0) {
            return main_fn(argc, argv);
        }
        if (dispatcher && first_arg && std::strcmp(first_arg, name) == 0) {
            return main_fn(argc - 1, argv + 1);
        }
        return std::nullopt;
    };

#if defined(MAGISKBOOT_ALONE_AVAILABLE) && MAGISKBOOT_ALONE_AVAILABLE
    {
        const auto r = dispatch("magiskboot", magiskboot_main);
        if (r)
            return *r;
    }
#endif  // #if defined(MAGISKBOOT_ALONE_AVAILABLE)...
#if defined(BOOTCTL_ALONE_AVAILABLE) && BOOTCTL_ALONE_AVAILABLE
    {
        const auto r = dispatch("bootctl", bootctl_main);
        if (r)
            return *r;
    }
#endif  // #if defined(BOOTCTL_ALONE_AVAILABLE) &&...
#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
    {
        const auto r = dispatch("resetprop", resetprop_main);
        if (r)
            return *r;
    }
#endif  // #if defined(RESETPROP_ALONE_AVAILABLE) ...
#if defined(MKBOOTFS_ALONE_AVAILABLE) && MKBOOTFS_ALONE_AVAILABLE
    {
        const auto r = dispatch("mkbootfs", mkbootfs_main);
        if (r)
            return *r;
    }
#endif  // #if defined(MKBOOTFS_ALONE_AVAILABLE) ...
#if defined(TOYBOX_READELF_AVAILABLE) && TOYBOX_READELF_AVAILABLE
    {
        const auto r = dispatch("readelf", zysu_toybox_main);
        if (r)
            return *r;
    }
#endif  // #if defined(TOYBOX_READELF_AVAILABLE) &&...
#if defined(NDK_BUSYBOX_AVAILABLE) && NDK_BUSYBOX_AVAILABLE
    {
        const auto r = dispatch("busybox", busybox_main);
        if (r)
            return *r;
    }
    // If invoked via a symlink whose name matches a busybox applet (e.g. "ls"),
    // and it's not one of ksud's own tools or a .so path, delegate to busybox.
    // Exclude "su": sucompat hijacks root shell to ksud; must not be delegated to busybox.
    if (base && base[0] && std::strcmp(base, "ksud") != 0 && std::strcmp(base, "magiskboot") != 0 &&
        std::strcmp(base, "bootctl") != 0 && std::strcmp(base, "resetprop") != 0 &&
        std::strcmp(base, "mkbootfs") != 0 && std::strcmp(base, "su") != 0 &&
        std::strstr(base, ".so") == nullptr) {
        return busybox_main(argc, argv);
    }
#endif  // #if defined(NDK_BUSYBOX_AVAILABLE) && N...

    return ksud::cli_run(argc, argv);
}
