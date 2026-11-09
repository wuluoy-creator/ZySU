#include "boot_patch_v2.hpp"
#include "boot_backup.hpp"

#include "../assets.hpp"
#include "../log.hpp"
#include "../utils.hpp"
#include "boot_patch.hpp"
#include "lkm_image.hpp"
#include "tools.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>

namespace fs = std::filesystem;

namespace ksud {
namespace {

struct BootPatchV2Args : KsuConfigArgs {
    std::string boot;
    std::string module;
    std::string output;
    std::string magiskboot;
    std::string superkey;
    bool force = false;
    bool flash = false;
    bool ota = false;
    bool enable_adbd = false;
    bool signature_bypass = false;
    bool no_reuse = false;
    bool valid = true;
};

BootPatchV2Args parse_args(const std::vector<std::string>& args) {
    BootPatchV2Args result;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& argument = args[index];
        auto take_value = [&](std::string& destination) {
            if (index + 1 >= args.size()) {
                result.valid = false;
                return;
            }
            destination = args[++index];
            if (destination.empty())
                result.valid = false;
        };
        if (argument == "-b" || argument == "--boot") {
            take_value(result.boot);
        } else if (argument == "-m" || argument == "--module") {
            take_value(result.module);
        } else if (argument == "-o" || argument == "--output" || argument == "--out") {
            take_value(result.output);
        } else if (argument == "--magiskboot") {
            take_value(result.magiskboot);
        } else if (argument == "--superkey") {
            take_value(result.superkey);
        } else if (argument == "--force") {
            result.force = true;
        } else if (argument == "--flash") {
            result.flash = true;
        } else if (argument == "--ota") {
            result.ota = true;
        } else if (argument == "--signature-bypass") {
            result.signature_bypass = true;
        } else if (argument == "--allow-shell") {
            result.allow_shell = true;
        } else if (argument == "--no-preinit") {
            result.no_preinit = true;
        } else if (argument == "--no-custom-rc") {
            result.no_custom_rc = true;
        } else if (argument == "--force-feature" || argument == "--block-module") {
            std::string value;
            take_value(value);
            if (result.valid && !(argument == "--force-feature" ? result.add_force_feature(value)
                                                                : result.add_block_modules(value)))
                result.valid = false;
        } else if (argument == "--enable-adbd") {
            result.enable_adbd = true;
        } else if (argument == "--no-reuse") {
            result.no_reuse = true;
        } else {
            result.valid = false;
        }
    }
    return result;
}

std::optional<std::vector<std::uint8_t>> read_binary(const fs::path& path) {
    std::vector<std::uint8_t> data;
    if (path.empty() || !read_file_bytes(path, &data))
        return std::nullopt;
    return data;
}

bool write_binary(const fs::path& path, const std::vector<std::uint8_t>& data) {
    return !path.empty() && write_file_bytes(path, data.data(), data.size(), 0600);
}

fs::path weak_absolute(const fs::path& path) {
    std::error_code error;
    const fs::path absolute = fs::absolute(path, error);
    if (error)
        return path;
    return absolute.lexically_normal();
}

bool same_path(const fs::path& left, const fs::path& right) {
    if (left.empty() || right.empty())
        return false;
    std::error_code error;
    if (fs::exists(left, error) && !error && fs::exists(right, error) && !error) {
        const bool equivalent = fs::equivalent(left, right, error);
        if (!error && equivalent)
            return true;
    }
    error.clear();
    const fs::path left_canonical = fs::weakly_canonical(left, error);
    if (error)
        return weak_absolute(left) == weak_absolute(right);
    error.clear();
    const fs::path right_canonical = fs::weakly_canonical(right, error);
    if (error)
        return weak_absolute(left) == weak_absolute(right);
    return left_canonical == right_canonical;
}

bool status_is_missing(const fs::file_status& status, const std::error_code& error) {
    if (error)
        return error == std::make_error_code(std::errc::no_such_file_or_directory);
    return status.type() == fs::file_type::not_found;
}

bool looks_like_non_boot_partition(const fs::path& path) {
    std::string name = path.filename().string();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return name.find("init_boot") != std::string::npos ||
           name.find("vendor_boot") != std::string::npos;
}

std::optional<fs::path> make_workdir() {
#ifdef _WIN32
    std::error_code error;
    fs::path base = fs::temp_directory_path(error);
    if (error)
        base = fs::current_path(error);
    for (unsigned int attempt = 0; attempt < 32; ++attempt) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        fs::path candidate =
            base / ("zysu-boot-v2-" + std::to_string(stamp) + "-" + std::to_string(attempt));
        if (fs::create_directories(candidate, error) && !error)
            return candidate;
    }
    return std::nullopt;
#else
    std::array<char, 96> pattern{};
    const char* tmp = std::getenv("TMPDIR");
    const std::string base = (tmp != nullptr && *tmp != '\0') ? tmp : "/data/local/tmp";
    const int written =
        std::snprintf(pattern.data(), pattern.size(), "%s/zysu-boot-v2-XXXXXX", base.c_str());
    if (written < 0 || static_cast<std::size_t>(written) >= pattern.size())
        return std::nullopt;
    const char* created = mkdtemp(pattern.data());
    if (created == nullptr)
        return std::nullopt;
    return fs::path(created);
#endif  // #ifdef _WIN32
}

bool find_unpacked_kernel(const fs::path& workdir, fs::path* kernel) {
    for (const char* name : {"kernel", "kernel.img"}) {
        const fs::path candidate = workdir / name;
        std::error_code error;
        if (fs::is_regular_file(candidate, error) && !error) {
            *kernel = candidate;
            return true;
        }
    }
    return false;
}

bool cpio_entry_exists(const std::string& magiskboot, const fs::path& workdir,
                       const fs::path& ramdisk, const char* entry) {
    // Read-only query: runs in-process, and ramdisk is already absolute.
    (void)magiskboot;
    (void)workdir;
    return magiskboot_query({"cpio", ramdisk.string(), "exists " + std::string(entry)}) == 0;
}

bool run_cpio_command(const std::string& magiskboot, const fs::path& workdir,
                      const fs::path& ramdisk, const std::string& command) {
    const auto result =
        exec_command_magiskboot(magiskboot, {"cpio", ramdisk.string(), command}, workdir.string());
    if (result.exit_code == 0)
        return true;
    LOGE("boot-patch-v2: cpio command failed: %s", command.c_str());
    if (!result.stderr_str.empty())
        LOGE("%s", result.stderr_str.c_str());
    return false;
}

bool ramdisk_is_magisk_patched(const std::string& magiskboot, const fs::path& workdir,
                               const fs::path& ramdisk) {
    if (magiskboot_query({"cpio", ramdisk.string(), "test"}) != 1)
        return false;
    return cpio_entry_exists(magiskboot, workdir, ramdisk, "init.magisk.rc") ||
           cpio_entry_exists(magiskboot, workdir, ramdisk, "overlay.d");
}

constexpr std::array<const char*, 9> kLegacyRamdiskEntries = {
    "kernelsu.ko",      "ksuinit",        "ksu_config",       "ksu_allow_shell", "sumh.ko",
    "force_debuggable", "adb_debug.prop", "stock_image.sha1", "init.real",
};

// A ramdisk the legacy patch synthesized for a boot image that ships none holds
// nothing but the loader, its payload and the empty .backup directory.
bool ramdisk_is_synthetic(const std::string& magiskboot, const fs::path& workdir,
                          const fs::path& ramdisk) {
    const auto listing =
        exec_command_magiskboot(magiskboot, {"cpio", ramdisk.string(), "ls -r"}, workdir.string());
    if (listing.exit_code != 0)
        return false;
    const std::string& output = listing.stdout_str;
    for (std::size_t begin = 0; begin < output.size();) {
        std::size_t end = output.find('\n', begin);
        if (end == std::string::npos)
            end = output.size();
        const std::string line = output.substr(begin, end - begin);
        begin = end + 1;
        const std::size_t separator = line.rfind('\t');
        if (separator == std::string::npos)
            continue;
        std::string path = line.substr(separator + 1);
        while (!path.empty() && (path.back() == '\r' || path.back() == ' '))
            path.pop_back();
        if (path.empty() || path == "." || path == "init" || path == ".backup" ||
            path.rfind(".backup/", 0) == 0)
            continue;
        const bool known = std::any_of(kLegacyRamdiskEntries.begin(), kLegacyRamdiskEntries.end(),
                                       [&path](const char* entry) { return path == entry; });
        if (!known) {
            LOGE("boot-patch-v2: unexpected ramdisk entry outside the KernelSU payload: %s",
                 path.c_str());
            return false;
        }
    }
    return true;
}

std::optional<bool> clean_legacy_ramdisks(const std::string& magiskboot, const fs::path& workdir) {
    const std::array<fs::path, 3> candidates = {
        workdir / "ramdisk.cpio",
        workdir / "vendor_ramdisk" / "ramdisk.cpio",
        workdir / "vendor_ramdisk" / "init_boot.cpio",
    };
    bool changed = false;
    for (const auto& ramdisk : candidates) {
        std::error_code error;
        if (!fs::is_regular_file(ramdisk, error) || error)
            continue;
        // Debug props and init.real also occur in non-KernelSU ramdisks.
        // Match the legacy patcher's ownership marker before touching any
        // entries or treating a reconstructed image as the stock backup.
        const bool has_kernelsu = cpio_entry_exists(magiskboot, workdir, ramdisk, "kernelsu.ko");
        if (!has_kernelsu)
            continue;
        if (ramdisk_is_magisk_patched(magiskboot, workdir, ramdisk)) {
            LOGE("boot-patch-v2: refusing to rewrite a Magisk-patched ramdisk");
            return std::nullopt;
        }

        const bool has_original_init = cpio_entry_exists(magiskboot, workdir, ramdisk, "init.real");
        if (has_original_init) {
            // magiskboot's cpio mv is fail-closed on destination collisions. The
            // legacy loader owns init, so remove that replacement before restoring
            // the original init.real entry.
            if (cpio_entry_exists(magiskboot, workdir, ramdisk, "init") &&
                !run_cpio_command(magiskboot, workdir, ramdisk, "rm init")) {
                return std::nullopt;
            }
            if (!run_cpio_command(magiskboot, workdir, ramdisk, "mv init.real init")) {
                return std::nullopt;
            }
        } else {
            // The patch only skips the init backup when the image ships no
            // ramdisk, so this whole cpio is synthetic. Drop the file and let
            // repack restore ramdisk_size = 0 instead of leaving a stub behind.
            if (!ramdisk_is_synthetic(magiskboot, workdir, ramdisk)) {
                LOGE("boot-patch-v2: legacy KernelSU ramdisk has no init.real but carries "
                     "unrelated content; refusing cleanup");
                return std::nullopt;
            }
            std::error_code remove_error;
            if (!fs::remove(ramdisk, remove_error) || remove_error) {
                LOGE("boot-patch-v2: failed to drop the synthetic KernelSU ramdisk %s",
                     ramdisk.string().c_str());
                return std::nullopt;
            }
            changed = true;
            printf("- Dropped the synthetic KernelSU ramdisk from %s\n", ramdisk.string().c_str());
            continue;
        }
        for (const char* entry : kLegacyRamdiskEntries) {
            if (std::strcmp(entry, "init.real") == 0)
                continue;
            if (cpio_entry_exists(magiskboot, workdir, ramdisk, entry) &&
                !run_cpio_command(magiskboot, workdir, ramdisk, "rm " + std::string(entry))) {
                return std::nullopt;
            }
        }
        changed = true;
        printf("- Removed legacy KernelSU ramdisk payload from %s\n", ramdisk.string().c_str());
    }
    return changed;
}

struct CleanedRamdiskImage {
    bool changed = false;
    fs::path repacked;
};

std::optional<CleanedRamdiskImage> clean_legacy_ramdisk_image(const std::string& magiskboot,
                                                              const fs::path& source,
                                                              const fs::path& workdir,
                                                              const std::string& output_name) {
    std::error_code error;
    if (!fs::create_directories(workdir, error) && error)
        return std::nullopt;
    const auto unpack =
        exec_command_magiskboot(magiskboot, {"unpack", source.string()}, workdir.string());
    if (unpack.exit_code != 0) {
        LOGE("boot-patch-v2: failed to unpack ramdisk cleanup image: %s", source.string().c_str());
        if (!unpack.stderr_str.empty())
            LOGE("%s", unpack.stderr_str.c_str());
        return std::nullopt;
    }
    const auto changed = clean_legacy_ramdisks(magiskboot, workdir);
    if (!changed)
        return std::nullopt;
    if (!*changed)
        return CleanedRamdiskImage{};

    const fs::path repacked = workdir / output_name;
    const auto repack = exec_command_magiskboot(
        magiskboot, {"repack", source.string(), repacked.string()}, workdir.string());
    error.clear();
    if (repack.exit_code != 0 || !fs::is_regular_file(repacked, error) || error) {
        LOGE("boot-patch-v2: failed to repack cleaned ramdisk image: %s", source.string().c_str());
        if (!repack.stderr_str.empty())
            LOGE("%s", repack.stderr_str.c_str());
        return std::nullopt;
    }
    return CleanedRamdiskImage{true, repacked};
}

bool flash_partition_image(const fs::path& image, const std::string& device) {
    if (image.empty() || device.empty())
        return false;
    if (!set_block_device_rw(device))
        LOGW("boot-patch-v2: continuing to flash %s anyway", device.c_str());
    if (!exec_dd(image.string(), device)) {
        LOGE("boot-patch-v2: failed to flash %s", device.c_str());
        return false;
    }
    return true;
}

struct PartitionImage {
    fs::path patched;
    fs::path original;
    std::string device;
};

enum class PartitionFlashResult : std::uint8_t { kSuccess, kRolledBack, kRollbackFailed };

PartitionFlashResult flash_partition_images(const std::vector<PartitionImage>& partitions) {
    for (std::size_t index = 0; index < partitions.size(); ++index) {
        const auto& partition = partitions[index];
        if (flash_partition_image(partition.patched, partition.device))
            continue;

        // A failed write may already have changed the target. Restore it as
        // well as every earlier partition, even if one restoration also fails.
        LOGE("boot-patch-v2: write failed; restoring affected partitions");
        bool rolled_back = true;
        for (std::size_t attempted = index + 1; attempted > 0; --attempted) {
            const auto& previous = partitions[attempted - 1];
            if (!flash_partition_image(previous.original, previous.device)) {
                rolled_back = false;
                LOGE("boot-patch-v2: rollback failed for %s; original snapshot: %s",
                     previous.device.c_str(), previous.original.string().c_str());
            } else {
                LOGW("boot-patch-v2: restored the original image to %s", previous.device.c_str());
            }
        }
        if (rolled_back) {
            LOGW("boot-patch-v2: patch failed; all affected partitions were restored");
            return PartitionFlashResult::kRolledBack;
        }
        LOGE("boot-patch-v2: patch failed and rollback is incomplete");
        return PartitionFlashResult::kRollbackFailed;
    }
    return PartitionFlashResult::kSuccess;
}

std::optional<std::vector<std::uint8_t>> load_module(const BootPatchV2Args& args,
                                                     const std::vector<std::uint8_t>& kernel,
                                                     const fs::path& workdir) {
    if (!args.module.empty()) {
        auto module = read_binary(args.module);
        if (!module)
            LOGE("boot-patch-v2: failed to read module: %s", args.module.c_str());
        return module;
    }
    const auto kmi = boot::lkm_image::detect_kmi(kernel);
    if (!kmi) {
        LOGE("boot-patch-v2: failed to detect KMI from linux_banner");
        return std::nullopt;
    }
    const std::string asset_name = *kmi + "_kernelsu.ko";
    const fs::path extracted_module = workdir / "embedded-kernelsu.ko";
    if (!copy_asset_to_file(asset_name, extracted_module.string())) {
        LOGE("boot-patch-v2: failed to extract embedded module for KMI %s", kmi->c_str());
        return std::nullopt;
    }
    auto module = read_binary(extracted_module);
    if (!module) {
        LOGE("boot-patch-v2: failed to read extracted module for KMI %s", kmi->c_str());
        return std::nullopt;
    }
    printf("- Using embedded LKM: %s\n", asset_name.c_str());
    return module;
}

void print_report(const boot::lkm_image::InjectionReport& report) {
    if (report.reused_metadata) {
        printf("- Reused the existing bootstrap and capsule metadata; skipped kallsyms and BTF "
               "recovery\n");
    } else {
        if (!report.reuse_skipped_reason.empty())
            printf("- Full re-analysis: %s\n", report.reuse_skipped_reason.c_str());
        printf("- Kernel: %s\n", report.kernel_release.c_str());
        printf("- Kallsyms: %s (%zu symbols)\n", report.kallsyms_layout.c_str(),
               report.kallsyms_count);
        if (report.btf_offset) {
            printf("- vmlinux BTF: offset 0x%zx, size 0x%zx, %zu types\n", *report.btf_offset,
                   report.btf_size.value_or(0), report.btf_type_count);
        } else {
            printf("- vmlinux BTF: not selected; using built-in GKI ABI\n");
        }
    }
    printf("- load_info: storage=%llu, hdr=%llu, len=%llu\n",
           static_cast<unsigned long long>(report.gki_abi.load_info_storage_size),
           static_cast<unsigned long long>(report.gki_abi.load_info_hdr_offset),
           static_cast<unsigned long long>(report.gki_abi.load_info_len_offset));
    if (report.code_size != 0) {
        printf("- Bootstrap: offset 0x%zx, %zu bytes\n", report.code_offset, report.code_size);
        printf("- Memblock patch: offset 0x%zx\n", report.memblock_call_offset);
    } else {
        printf("- Bootstrap: existing direct-LKM bootstrap retained\n");
    }
    if (report.page_offset != 0)
        printf("- PAGE_OFFSET: 0x%llx\n", static_cast<unsigned long long>(report.page_offset));
    printf("- Module fixups: %zu, unresolved: %zu\n", report.fixup_count, report.unresolved.size());
    if (!report.unresolved.empty()) {
        printf("- Native resolver symbols: ");
        for (std::size_t index = 0; index < report.unresolved.size(); ++index)
            printf("%s%s", index == 0 ? "" : ", ", report.unresolved[index].c_str());
        printf("\n");
    }
    printf("- Patched Image size: 0x%zx\n", report.image_size);
}

}  // namespace

int boot_patch_v2(const std::vector<std::string>& args) {
    const BootPatchV2Args parsed = parse_args(args);
    const bool device_mode = parsed.flash;
    if (!parsed.valid || (parsed.ota && !device_mode) ||
        (!device_mode && (parsed.boot.empty() || parsed.output.empty())) ||
        (device_mode && (!parsed.boot.empty() || !parsed.output.empty()))) {
        LOGE("Usage: ksud boot-patch-v2 --boot <boot.img> [--module <kernelsu.ko>] "
             "--output <patched.img> [--superkey <key>] [--signature-bypass] "
             "[--allow-shell] [--enable-adbd] [--force] [--no-custom-rc] [--no-preinit] "
             "[--force-feature FEATURE=VALUE] [--block-module NAME[,NAME...]]\n"
             "       ksud boot-patch-v2 --flash [--ota] [--module <kernelsu.ko>] "
             "[--superkey <key>] [--signature-bypass] [--allow-shell] "
             "[--enable-adbd] [--no-custom-rc] [--no-preinit] [--force-feature FEATURE=VALUE] "
             "[--block-module NAME[,NAME...]]");
        return 1;
    }
    const fs::path module_path(parsed.module);
    std::error_code error;
    if (!parsed.module.empty()) {
        error.clear();
        if (!fs::is_regular_file(module_path, error)) {
            if (error) {
                LOGE("boot-patch-v2: failed to inspect module %s: %s", parsed.module.c_str(),
                     error.message().c_str());
            } else {
                LOGE("boot-patch-v2: module does not exist: %s", parsed.module.c_str());
            }
            return 1;
        }
    }

    auto workdir = make_workdir();
    if (!workdir) {
        LOGE("boot-patch-v2: failed to create a temporary directory");
        return 1;
    }
    const fs::path& work = *workdir;
    bool preserve_recovery_snapshots = false;
    const auto cleanup = [&work, &preserve_recovery_snapshots]() {
        if (preserve_recovery_snapshots) {
            LOGW("boot-patch-v2: preserving original partition snapshots in %s for recovery",
                 work.string().c_str());
            return;
        }
        std::error_code cleanup_error;
        fs::remove_all(work, cleanup_error);
        if (cleanup_error)
            LOGW("boot-patch-v2: failed to remove temporary directory %s: %s",
                 work.string().c_str(), cleanup_error.message().c_str());
    };

    fs::path input_path;
    fs::path requested_input_path;
    const fs::path output_path(parsed.output);
    std::string boot_device;
    std::string init_boot_device;
    if (device_mode) {
        const std::string slot = get_slot_suffix(parsed.ota);
        if (parsed.ota && slot.empty()) {
            LOGE("boot-patch-v2: failed to resolve the inactive slot");
            cleanup();
            return 1;
        }
        boot_device = "/dev/block/by-name/boot" + slot;
        const fs::path current_boot = work / "boot-current.img";
        if (access(boot_device.c_str(), R_OK) != 0 ||
            !exec_dd(boot_device, current_boot.string())) {
            LOGE("boot-patch-v2: failed to read %s", boot_device.c_str());
            cleanup();
            return 1;
        }
        input_path = current_boot;
        init_boot_device = "/dev/block/by-name/init_boot" + slot;
    } else {
        // Magiskboot runs from the temporary workdir, so preserve an absolute
        // source path when the caller supplied a relative boot image path.
        requested_input_path = weak_absolute(fs::path(parsed.boot));
        input_path = requested_input_path;
        error.clear();
        if (!fs::is_regular_file(input_path, error)) {
            if (error) {
                LOGE("boot-patch-v2: failed to inspect boot image %s: %s", parsed.boot.c_str(),
                     error.message().c_str());
            } else {
                LOGE("boot-patch-v2: boot image does not exist: %s", parsed.boot.c_str());
            }
            cleanup();
            return 1;
        }
        if (looks_like_non_boot_partition(input_path)) {
            LOGE("boot-patch-v2: refusing init_boot/vendor_boot; this command only patches "
                 "boot.img");
            cleanup();
            return 1;
        }
    }

    if (!device_mode) {
        error.clear();
        const fs::file_status output_status = fs::symlink_status(output_path, error);
        if (error && !status_is_missing(output_status, error)) {
            LOGE("boot-patch-v2: failed to inspect output path %s: %s", parsed.output.c_str(),
                 error.message().c_str());
            cleanup();
            return 1;
        }
        const bool output_exists = !status_is_missing(output_status, error);
        if (output_exists) {
            error.clear();
            if (!fs::is_regular_file(output_path, error)) {
                if (error) {
                    LOGE("boot-patch-v2: failed to inspect output path %s: %s",
                         parsed.output.c_str(), error.message().c_str());
                } else {
                    LOGE("boot-patch-v2: output exists but is not a regular file: %s",
                         parsed.output.c_str());
                }
                cleanup();
                return 1;
            }
            if (same_path(requested_input_path.empty() ? input_path : requested_input_path,
                          output_path) ||
                (!parsed.module.empty() && same_path(module_path, output_path))) {
                LOGE("boot-patch-v2: refusing to overwrite an input file");
                cleanup();
                return 1;
            }
            if (!parsed.force) {
                LOGE("boot-patch-v2: output exists; use --force: %s", parsed.output.c_str());
                cleanup();
                return 1;
            }
        }
    }

    if (!device_mode) {
        const fs::path snapshot = work / "boot-input.img";
        if (!copy_file_data(requested_input_path, snapshot, 0600, false)) {
            LOGE("boot-patch-v2: failed to snapshot the selected boot image");
            cleanup();
            return 1;
        }
        input_path = snapshot;
    }

    {
        const auto source_data = read_binary(input_path);
        if (!source_data) {
            LOGE("boot-patch-v2: failed to read boot image: %s", input_path.string().c_str());
            cleanup();
            return 1;
        }
        const auto boot_info = boot::lkm_image::parse_boot_image(*source_data);
        if (!boot_info) {
            LOGE("boot-patch-v2: input must be a valid AOSP boot image; init_boot and "
                 "vendor_boot are not supported: %s",
                 boot_info.error().message.c_str());
            cleanup();
            return 1;
        }
        if (boot_info.value().kernel.empty()) {
            LOGE("boot-patch-v2: boot image does not contain a kernel");
            cleanup();
            return 1;
        }
    }

    const std::string magiskboot = find_magiskboot(parsed.magiskboot, work.string());
    if (magiskboot.empty()) {
        cleanup();
        return 1;
    }
    printf("- Reading boot image: %s\n", input_path.string().c_str());
    const auto unpack =
        exec_command_magiskboot(magiskboot, {"unpack", input_path.string()}, work.string());
    if (unpack.exit_code != 0) {
        LOGE("boot-patch-v2: magiskboot unpack failed (%d)", unpack.exit_code);
        if (!unpack.stderr_str.empty())
            LOGE("%s", unpack.stderr_str.c_str());
        cleanup();
        return 1;
    }
    const auto legacy_cleanup = clean_legacy_ramdisks(magiskboot, work);
    if (!legacy_cleanup) {
        LOGE("boot-patch-v2: failed to clean legacy KernelSU ramdisk payload");
        cleanup();
        return 1;
    }

    std::optional<CleanedRamdiskImage> cleaned_init_boot;
    if (device_mode) {
        struct stat init_boot_stat{};
        if (stat(init_boot_device.c_str(), &init_boot_stat) == 0) {
            const fs::path init_boot_source = work / "init_boot-input.img";
            if (!exec_dd(init_boot_device, init_boot_source.string())) {
                LOGE("boot-patch-v2: failed to read %s", init_boot_device.c_str());
                cleanup();
                return 1;
            }
            cleaned_init_boot = clean_legacy_ramdisk_image(
                magiskboot, init_boot_source, work / "init_boot_cleanup", "new-init_boot.img");
            if (!cleaned_init_boot) {
                LOGE("boot-patch-v2: failed to clean %s", init_boot_device.c_str());
                cleanup();
                return 1;
            }
        } else if (errno != ENOENT) {
            LOGE("boot-patch-v2: failed to inspect %s: %s", init_boot_device.c_str(),
                 std::strerror(errno));
            cleanup();
            return 1;
        }
    }
    fs::path kernel_path;
    if (!find_unpacked_kernel(work, &kernel_path)) {
        LOGE("boot-patch-v2: unpacked boot image has no kernel");
        cleanup();
        return 1;
    }
    auto kernel = read_binary(kernel_path);
    if (!kernel) {
        LOGE("boot-patch-v2: failed to read unpacked kernel");
        cleanup();
        return 1;
    }
    const bool already_patched = boot::lkm_image::contains_capsule(*kernel);
    std::optional<boot_backup::Backup> previous_backup;
    if (already_patched)
        previous_backup = boot_backup::find_imgpatch(input_path, work);
    fs::path stock_image = previous_backup ? previous_backup->image : input_path;
    if (!previous_backup && (already_patched || *legacy_cleanup)) {
        stock_image = work / "stock-boot.img";
        if (already_patched) {
            const auto restored = boot::lkm_image::remove_capsule(*kernel);
            if (!restored || !write_binary(kernel_path, restored.value())) {
                LOGE("boot-patch-v2: failed to recover the stock kernel for backup");
                cleanup();
                return 1;
            }
        }
        const auto stock_repack = exec_command_magiskboot(
            magiskboot, {"repack", input_path.string(), stock_image.string()}, work.string());
        if ((already_patched && !write_binary(kernel_path, *kernel)) ||
            stock_repack.exit_code != 0) {
            LOGE("boot-patch-v2: failed to prepare the stock image backup");
            cleanup();
            return 1;
        }
    }

    const bool bundled_lkm = parsed.module.empty();
    auto module = load_module(parsed, *kernel, work);
    if (!module) {
        cleanup();
        return 1;
    }
    const fs::path module_for_injection = work / "module-for-injection.ko";
    if (!write_binary(module_for_injection, *module)) {
        LOGE("boot-patch-v2: failed to stage module for injection");
        cleanup();
        return 1;
    }
    if (!parsed.superkey.empty())
        printf("- Injecting SuperKey into LKM\n");
    else if (parsed.signature_bypass)
        LOGW("boot-patch-v2: --signature-bypass requires --superkey; ignoring");
    if (!inject_superkey_into_lkm(module_for_injection.string(), parsed.superkey,
                                  parsed.signature_bypass)) {
        LOGE("boot-patch-v2: failed to inject SuperKey data into LKM");
        cleanup();
        return 1;
    }
    if (!inject_imgpatch_config_into_lkm(module_for_injection.string(), parsed.allow_shell,
                                         parsed.enable_adbd, bundled_lkm, parsed)) {
        LOGE("boot-patch-v2: failed to inject ImgPatch configuration into LKM");
        cleanup();
        return 1;
    }
    module = read_binary(module_for_injection);
    if (!module) {
        LOGE("boot-patch-v2: failed to read SuperKey-patched LKM");
        cleanup();
        return 1;
    }
    const auto marked_module = boot::lkm_image::mark_module_image_patch(&*module, bundled_lkm);
    if (!marked_module) {
        LOGE("boot-patch-v2: failed to mark the LKM load mode: %s",
             marked_module.error().message.c_str());
        cleanup();
        return 1;
    }
    if (already_patched)
        printf("- Existing direct-LKM bootstrap found; replacing its module capsule\n");
    else
        printf("- Recovering kallsyms/BTF and injecting LKM\n");
    auto injected = already_patched ? boot::lkm_image::replace_capsule_module(*kernel, *module,
                                                                              !parsed.no_reuse)
                                    : boot::lkm_image::inject_image(*kernel, *module);
    if (!injected) {
        LOGE("boot-patch-v2: injection failed: %s", injected.error().message.c_str());
        cleanup();
        return 1;
    }
    if (!write_binary(kernel_path, injected.value().image)) {
        LOGE("boot-patch-v2: failed to write patched kernel");
        cleanup();
        return 1;
    }
    const fs::path repacked = work / "new-boot.img";
    // Always let MagiskbootAlone recompress the modified kernel in its source format.
    const auto repack = exec_command_magiskboot(
        magiskboot, {"repack", input_path.string(), repacked.string()}, work.string());
    error.clear();
    const bool repacked_regular = fs::is_regular_file(repacked, error);
    if (repack.exit_code != 0 || !repacked_regular || error) {
        LOGE("boot-patch-v2: magiskboot repack failed (%d)", repack.exit_code);
        if (!repack.stderr_str.empty())
            LOGE("%s", repack.stderr_str.c_str());
        if (error)
            LOGE("boot-patch-v2: failed to inspect repacked image: %s", error.message().c_str());
        cleanup();
        return 1;
    }
    printf("- Repacked boot with MagiskbootAlone\n");
    const auto stock_backup = boot_backup::save_stock(stock_image);
    const auto patched_backup =
        stock_backup ? boot_backup::bind_imgpatch(*stock_backup, repacked) : std::nullopt;
    if (!patched_backup) {
        LOGE("boot-patch-v2: failed to bind the stock backup to the patched image");
        cleanup();
        return 1;
    }

    if (device_mode) {
        const bool init_boot_changed = cleaned_init_boot.has_value() && cleaned_init_boot->changed;
        std::vector<PartitionImage> partitions{{repacked, input_path, boot_device}};
        if (init_boot_changed)
            partitions.push_back(
                {cleaned_init_boot->repacked, work / "init_boot-input.img", init_boot_device});
        const auto flash_result = flash_partition_images(partitions);
        if (flash_result != PartitionFlashResult::kSuccess) {
            preserve_recovery_snapshots = flash_result == PartitionFlashResult::kRollbackFailed;
            cleanup();
            return 1;
        }
        if (previous_backup && previous_backup->reference != patched_backup->reference)
            boot_backup::consume(*previous_backup);
        print_report(injected.value().report);
        printf("- Flashed direct-kernel patch to %s\n", boot_device.c_str());
        if (init_boot_changed)
            printf("- Cleared legacy KernelSU ramdisk from %s\n", init_boot_device.c_str());
        printf("- Patch successfully\n");
        printf("- Done!\n");
        cleanup();
        return 0;
    }

    const fs::path parent =
        output_path.parent_path().empty() ? fs::path(".") : output_path.parent_path();
    error.clear();
    fs::create_directories(parent, error);
    if (error) {
        LOGE("boot-patch-v2: failed to create output directory: %s", error.message().c_str());
        cleanup();
        return 1;
    }
    error.clear();
    if (!fs::is_directory(parent, error)) {
        if (error) {
            LOGE("boot-patch-v2: failed to inspect output directory: %s", error.message().c_str());
        } else {
            LOGE("boot-patch-v2: output parent is not a directory: %s", parent.string().c_str());
        }
        cleanup();
        return 1;
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path temporary = parent / (".zysu-boot-v2-" + output_path.filename().string() + "." +
                                         std::to_string(stamp) + ".tmp");
    error.clear();
    const fs::file_status temporary_status = fs::symlink_status(temporary, error);
    if (!status_is_missing(temporary_status, error)) {
        if (error) {
            LOGE("boot-patch-v2: failed to inspect temporary output: %s", error.message().c_str());
        } else {
            LOGE("boot-patch-v2: temporary output already exists: %s", temporary.string().c_str());
        }
        cleanup();
        return 1;
    }
    const bool copied = copy_file_data(repacked, temporary, 0644, false);
    if (!copied) {
        LOGE("boot-patch-v2: failed to stage output image: %s", strerror(errno));
        std::error_code remove_error;
        fs::remove(temporary, remove_error);
        cleanup();
        return 1;
    }
    error.clear();
    const fs::file_status final_output_status = fs::symlink_status(output_path, error);
    if (error && !status_is_missing(final_output_status, error)) {
        LOGE("boot-patch-v2: failed to recheck output path: %s", error.message().c_str());
        std::error_code remove_error;
        fs::remove(temporary, remove_error);
        cleanup();
        return 1;
    }
    if (!status_is_missing(final_output_status, error)) {
        if (same_path(requested_input_path, output_path) ||
            (!parsed.module.empty() && same_path(module_path, output_path))) {
            LOGE("boot-patch-v2: refusing to overwrite an input file");
            std::error_code remove_error;
            fs::remove(temporary, remove_error);
            cleanup();
            return 1;
        }
        if (!parsed.force) {
            LOGE("boot-patch-v2: output appeared while patching; use --force: %s",
                 parsed.output.c_str());
            std::error_code remove_error;
            fs::remove(temporary, remove_error);
            cleanup();
            return 1;
        }
        if (final_output_status.type() != fs::file_type::regular &&
            final_output_status.type() != fs::file_type::symlink) {
            LOGE("boot-patch-v2: refusing to replace a non-file output: %s", parsed.output.c_str());
            std::error_code remove_error;
            fs::remove(temporary, remove_error);
            cleanup();
            return 1;
        }
        error.clear();
        fs::remove(output_path, error);
        if (error) {
            LOGE("boot-patch-v2: failed to replace output: %s", error.message().c_str());
            std::error_code remove_error;
            fs::remove(temporary, remove_error);
            cleanup();
            return 1;
        }
    }
    error.clear();
    fs::rename(temporary, output_path, error);
    if (error) {
        LOGE("boot-patch-v2: failed to install output image: %s", error.message().c_str());
        std::error_code remove_error;
        fs::remove(temporary, remove_error);
        cleanup();
        return 1;
    }
    print_report(injected.value().report);
    printf("- Output: %s\n", output_path.string().c_str());
    printf("- Patch successfully\n");
    printf("- Done!\n");
    cleanup();
    return 0;
}

}  // namespace ksud
