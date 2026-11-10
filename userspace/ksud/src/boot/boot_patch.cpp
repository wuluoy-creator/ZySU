#include "boot_patch.hpp"
#include "../../sumhp/include/sumhp/sumh_client.hpp"
#include "../assets.hpp"
#include "../defs.hpp"
#include "../kernel_version.hpp"
#include "../log.hpp"
#include "../utils.hpp"
#include "boot_backup.hpp"
#include "lkm_image.hpp"
#include "tools.hpp"
#include "uapi/imgpatch_config.h"
#include "uapi/sumh.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include <mbedtls/sha256.h>

namespace fs = std::filesystem;

namespace ksud {

// SuperKey magic marker (must match kernel's SUPERKEY_MAGIC)
constexpr uint64_t SUPERKEY_MAGIC = 0x5355504552;  // "SUPER" in hex

// SuperKey verification mode definitions (must match kernel)
// 0: signature-only (no superkey)
// 1: signature authentication, or SuperKey with a trusted signature
// 2: superkey-only (signature bypass)
constexpr uint64_t SUPERKEY_VERIFICATION_SIGNATURE_ONLY = 0;
constexpr uint64_t SUPERKEY_VERIFICATION_SIGN_AND_KEY = 1;
constexpr uint64_t SUPERKEY_VERIFICATION_KEY_ONLY = 2;

namespace {

// LZ4 legacy ramdisk magic (reject before cpio to avoid huge cache/hang).
constexpr std::array<unsigned char, 4> LZ4_LEGACY_MAGIC = {0x02, 0x21, 0x4c, 0x18};

constexpr size_t SUPERKEY_SALT_LEN = 16;
// uapi/supercall.h transports at most 64 key bytes plus the terminating NUL.
constexpr size_t SUPERKEY_MAX_LEN = 64;

std::optional<fs::path> create_boot_workdir() {
    std::vector<fs::path> candidates;
    const char* configured = std::getenv("TMPDIR");
    if (configured != nullptr && *configured != '\0')
        candidates.emplace_back(configured);
    std::error_code error;
    const fs::path current = fs::current_path(error);
    if (!error)
        candidates.push_back(current);
    candidates.emplace_back("/data/local/tmp");
    for (const auto& base : candidates) {
        error.clear();
        const fs::path absolute = fs::absolute(base, error);
        if (error)
            continue;
        std::string pattern = (absolute / "KernelSU_XXXXXX").string();
        // mkdtemp creates a private directory atomically; a failed candidate
        // must not prevent an unprivileged caller trying its writable cwd.
        if (mkdtemp(pattern.data()) != nullptr)
            return fs::path(pattern);
    }
    return std::nullopt;
}

// SHA-256(salt || key), truncated to first 8 bytes (little-endian u64). Must
// match the kernel-side hash_superkey() in kernel/superkey.c.
uint64_t hash_superkey(const std::array<uint8_t, SUPERKEY_SALT_LEN>& salt, const std::string& key) {
    unsigned char digest[32] = {0};
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, salt.data(), salt.size());
    mbedtls_sha256_update(&ctx, reinterpret_cast<const unsigned char*>(key.data()), key.size());
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    uint64_t out = 0;
    for (size_t i = 0; i < 8; ++i) {
        out |= static_cast<uint64_t>(digest[i]) << (i * 8);
    }
    return out;
}

bool fill_random(uint8_t* buf, size_t len) {
    size_t filled = 0;
    while (filled < len) {
        const ssize_t count = getrandom(buf + filled, len - filled, 0);
        if (count > 0) {
            filled += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

bool has_lz4_legacy_magic(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    std::array<unsigned char, 4> magic{};
    size_t filled = 0;
    while (filled < magic.size()) {
        const ssize_t count = read(fd, magic.data() + filled, magic.size() - filled);
        if (count > 0) {
            filled += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    close(fd);
    return filled == magic.size() &&
           memcmp(magic.data(), LZ4_LEGACY_MAGIC.data(), magic.size()) == 0;
}

// Inject superkey salt+hash and verification mode into LKM file.
// Layout in the LKM .data section (matches struct superkey_data in kernel):
//   [+0]  u64 magic   (SUPERKEY_MAGIC)
//   [+8]  u8  salt[16]
//   [+24] u64 hash    (first 8 bytes of SHA-256(salt || key))
//   [+32] u64 flags   (verification mode)
// The verification mode is always injected, even when superkey is empty.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) path then superkey
bool inject_superkey_to_lkm(const std::string& lkm_path, const std::string& superkey,
                            bool signature_bypass) {
    if (superkey.size() > SUPERKEY_MAX_LEN || superkey.find('\0') != std::string::npos) {
        LOGE("SuperKey must contain at most 64 UTF-8 bytes and no NUL characters");
        return false;
    }
    std::array<uint8_t, SUPERKEY_SALT_LEN> salt{};
    if (!superkey.empty()) {
        if (!fill_random(salt.data(), salt.size())) {
            LOGE("Failed to obtain SuperKey salt entropy");
            return false;
        }
    }
    uint64_t hash = superkey.empty() ? 0 : hash_superkey(salt, superkey);

    uint64_t flags;
    if (superkey.empty()) {
        // User did not configure a SuperKey: pure signature mode.
        flags = SUPERKEY_VERIFICATION_SIGNATURE_ONLY;
    } else {
        // SuperKey is set; choose between "sign+key" and "key only".
        flags =
            signature_bypass ? SUPERKEY_VERIFICATION_KEY_ONLY : SUPERKEY_VERIFICATION_SIGN_AND_KEY;
    }

    printf("- SuperKey hash: 0x%016llx\n", static_cast<unsigned long long>(hash));
    const char* mode_str;
    if (superkey.empty()) {
        mode_str = "signature-only";
    } else {
        mode_str = signature_bypass ? "key-only" : "sign+key";
    }
    printf("- Verification mode: %llu (%s)\n", static_cast<unsigned long long>(flags), mode_str);

    std::vector<uint8_t> content;
    if (!read_file_bytes(lkm_path, &content)) {
        LOGE("Failed to open LKM file: %s", lkm_path.c_str());
        return false;
    }
    const size_t size = content.size();

    // Search for SUPERKEY_MAGIC in the binary
    std::array<uint8_t, 8> magic_bytes{};
    memcpy(magic_bytes.data(), &SUPERKEY_MAGIC, magic_bytes.size());

    constexpr size_t SUPERKEY_BLOCK_LEN = 40;  // magic(8) + salt(16) + hash(8) + flags(8)
    bool found = false;
    for (size_t i = 0; i + SUPERKEY_BLOCK_LEN <= size; i++) {
        if (memcmp(&content[i], magic_bytes.data(), magic_bytes.size()) == 0) {
            memcpy(&content[i + 8], salt.data(), salt.size());
            memcpy(&content[i + 24], &hash, sizeof(hash));
            memcpy(&content[i + 32], &flags, sizeof(flags));
            found = true;
            printf("- Injected SuperKey data at offset 0x%zx (40 bytes)\n", i);
            break;
        }
    }

    if (!found) {
        if (!superkey.empty()) {
            LOGE("Selected LKM does not support SuperKey; requested key was not applied");
            return false;
        }
        printf("- Warning: SUPERKEY_MAGIC not found in LKM, SuperKey may not work\n");
        printf("- Make sure the kernel module is compiled with SuperKey support\n");
    } else if (!write_file_bytes(lkm_path, content.data(), content.size())) {
        LOGE("Failed to write patched LKM: %s", lkm_path.c_str());
        return false;
    }

    return true;
}

bool inject_imgpatch_config_to_lkm(const std::string& lkm_path, bool allow_shell, bool enable_adbd,
                                   bool bundled_lkm, const KsuConfigArgs& args) {
    static_assert(sizeof(ksu_imgpatch_config) == 512, "ImgPatch config ABI drift");
    static_assert(offsetof(ksu_imgpatch_config, force_features) == 24, "ImgPatch config ABI drift");

    ksu_imgpatch_config config{};
    config.magic = KSU_IMGPATCH_CONFIG_MAGIC;
    config.version = KSU_IMGPATCH_CONFIG_VERSION;
    config.size = sizeof(config);
    if (bundled_lkm)
        config.flags |= KSU_IMGPATCH_CONFIG_BUNDLED;
    if (allow_shell)
        config.flags |= KSU_IMGPATCH_CONFIG_ALLOW_SHELL;
    if (enable_adbd)
        config.flags |= KSU_IMGPATCH_CONFIG_ENABLE_ADBD;

    if (args.no_custom_rc)
        config.flags |= KSU_IMGPATCH_CONFIG_NO_CUSTOM_RC;
    if (args.no_preinit)
        config.flags |= KSU_IMGPATCH_CONFIG_NO_PREINIT;
    const auto forced = args.force_features();
    const auto blocked = args.block_modules();
    if (forced.size() >= sizeof(config.force_features) ||
        blocked.size() >= sizeof(config.block_modules))
        return false;
    memcpy(config.force_features, forced.c_str(), forced.size() + 1);
    memcpy(config.block_modules, blocked.c_str(), blocked.size() + 1);
    const bool extended_config =
        args.no_custom_rc || args.no_preinit || !forced.empty() || !blocked.empty();

    std::vector<uint8_t> content;
    if (!read_file_bytes(lkm_path, &content)) {
        LOGE("Failed to open LKM file: %s", lkm_path.c_str());
        return false;
    }

    std::array<uint8_t, sizeof(config.magic)> magic_bytes{};
    memcpy(magic_bytes.data(), &config.magic, magic_bytes.size());
    std::optional<size_t> config_offset;
    bool incompatible_marker = false;
    for (size_t offset = 0; offset + sizeof(config) <= content.size(); ++offset) {
        if (memcmp(content.data() + offset, magic_bytes.data(), magic_bytes.size()) != 0)
            continue;

        ksu_imgpatch_config candidate{};
        memcpy(&candidate, content.data() + offset, sizeof(candidate));
        if ((candidate.version != KSU_IMGPATCH_CONFIG_VERSION && candidate.version != 3 &&
             candidate.version != 2) ||
            (candidate.version < 4 && args.no_preinit) ||
            (candidate.version == 2 && extended_config) || candidate.size != sizeof(candidate)) {
            incompatible_marker = true;
            continue;
        }
        if (config_offset.has_value()) {
            LOGE("LKM contains duplicate ImgPatch config markers");
            return false;
        }
        config.version = candidate.version;
        config_offset = offset;
    }

    if (!config_offset.has_value()) {
        if (incompatible_marker || config.flags != 0 || extended_config) {
            LOGE("Selected LKM does not support embedded ImgPatch configuration%s",
                 incompatible_marker ? " (incompatible marker version)" : "");
            return false;
        }
        return true;
    }

    memcpy(content.data() + *config_offset, &config, sizeof(config));
    if (!write_file_bytes(lkm_path, content.data(), content.size())) {
        LOGE("Failed to write ImgPatch configuration to LKM: %s", lkm_path.c_str());
        return false;
    }
    printf("- Injected ImgPatch config at offset 0x%zx: allow_shell=%d enable_adbd=%d\n",
           *config_offset, allow_shell, enable_adbd);
    return true;
}

// Execute magiskboot cpio command (runs in workdir for relative path resolution).
bool do_cpio_cmd(const std::string& magiskboot, const std::string& workdir,
                 const std::string& cpio_path, const std::string& cmd) {
    auto result = exec_command_magiskboot(magiskboot, {"cpio", cpio_path, cmd}, workdir);
    if (result.exit_code != 0) {
        LOGE("magiskboot cpio %s failed", cmd.c_str());
        if (!result.stdout_str.empty()) {
            LOGE("magiskboot cpio stdout: %s", result.stdout_str.c_str());
        }
        if (!result.stderr_str.empty()) {
            LOGE("magiskboot cpio stderr: %s", result.stderr_str.c_str());
        }
        return false;
    }
    return true;
}

// Check if boot image is patched by Magisk
bool is_magisk_patched(const std::string& magiskboot, const std::string& workdir,
                       const std::string& cpio_path) {
    // Built-in magiskboot runs in-process for read-only queries, and cpio_path is
    // absolute, so neither the tool path nor a cwd is needed here.
    (void)magiskboot;
    (void)workdir;
    // magiskboot cpio test: 0 = stock, 1 = magisk, 2 = unsupported.
    if (magiskboot_query({"cpio", cpio_path, "test"}) != 1) {
        return false;
    }
    // Confirm with a Magisk-specific entry so an unrelated exit status of 1
    // cannot be read as "Magisk-patched".
    return magiskboot_query({"cpio", cpio_path, "exists init.magisk.rc"}) == 0 ||
           magiskboot_query({"cpio", cpio_path, "exists overlay.d"}) == 0;
}

// Check if boot image is patched by KernelSU
bool is_kernelsu_patched(const std::string& magiskboot, const std::string& workdir,
                         const std::string& cpio_path) {
    // Built-in magiskboot runs in-process for read-only queries, and cpio_path is
    // absolute, so neither the tool path nor a cwd is needed here.
    (void)magiskboot;
    (void)workdir;
    return magiskboot_query({"cpio", cpio_path, "exists kernelsu.ko"}) == 0;
}

// Flash boot image
bool flash_boot(const std::string& bootdevice, const std::string& new_boot) {
    if (bootdevice.empty()) {
        LOGE("Boot device not found");
        return false;
    }

    // Set device to read-write. Some devices/partitions do not require or allow
    // this ioctl; continue either way and let the write itself decide.
    if (!set_block_device_rw(bootdevice)) {
        LOGW("Continuing to flash %s anyway", bootdevice.c_str());
    }

    if (!exec_dd(new_boot, bootdevice)) {
        LOGE("Failed to flash boot image");
        return false;
    }

    return true;
}

// Backup stock boot image
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool do_backup(const std::string& magiskboot, const std::string& workdir,
               const std::string& cpio_path, const std::string& image) {
    const auto backup = boot_backup::save_stock(image);
    if (!backup)
        return false;
    const std::string sha1_file = workdir + "/" + BACKUP_FILENAME;
    if (!write_file(sha1_file, backup->sha1))
        return false;

    // Add backup info to ramdisk
    if (!do_cpio_cmd(
            magiskboot, workdir, cpio_path,
            "add 0644 " + std::string(BACKUP_FILENAME) + " " + std::string(BACKUP_FILENAME))) {
        return false;
    }

    printf("- Stock image has been backup to\n");
    printf("- %s\n", backup->stored_image.c_str());
    return true;
}

std::string parse_kmi_from_kernel_file(const std::string& kernel_path) {
    std::vector<uint8_t> data;
    if (!read_file_bytes(kernel_path, &data)) {
        LOGE("Failed to read kernel from %s", kernel_path.c_str());
        return "";
    }
    if (data.empty()) {
        LOGE("Kernel image is empty: %s", kernel_path.c_str());
        return "";
    }

    for (size_t i = 0; i + 4 <= data.size(); ++i) {
        if (data[i] < '5' || data[i] > '9' || data[i + 1] != '.' || !std::isdigit(data[i + 2]) ||
            (data[i] == '5' && !std::isdigit(data[i + 3]))) {
            continue;
        }

        const size_t limit = std::min(data.size(), i + 100);
        size_t end = i;
        while (end < limit && data[end] != '\0') {
            ++end;
        }
        if (end == limit) {
            continue;
        }

        const std::string_view candidate(reinterpret_cast<const char*>(data.data() + i), end - i);
        if (const auto kmi = parse_kmi_string(candidate))
            return *kmi;
    }

    return "";
}

std::string parse_kmi_from_boot(const std::string& magiskboot, const std::string& workdir,
                                const std::string& boot_path) {
    std::error_code path_error;
    const auto absolute_boot = fs::absolute(boot_path, path_error);
    if (path_error) {
        LOGE("Failed to resolve target boot image: %s", boot_path.c_str());
        return "";
    }
    const std::string detect_dir = workdir + "/target_kmi";
    if (!ensure_clean_dir(detect_dir)) {
        LOGE("Failed to create target KMI detection directory");
        return "";
    }

    auto unpack_result =
        exec_command_magiskboot(magiskboot, {"unpack", absolute_boot.string()}, detect_dir);
    if (unpack_result.exit_code != 0) {
        // Kernel is extracted before ramdisk. Some legacy ramdisks can still make magiskboot
        // abort after a valid kernel file has already been written, so try that file first.
        LOGW("Target boot unpack exited with %d: %s", unpack_result.exit_code, boot_path.c_str());
        if (!unpack_result.stderr_str.empty()) {
            LOGW("magiskboot stderr: %s", unpack_result.stderr_str.c_str());
        }
    }

    const std::string kernel_path = detect_dir + "/kernel";
    const std::string kmi = parse_kmi_from_kernel_file(kernel_path);
    if (kmi.empty()) {
        LOGE("Failed to get KMI from target boot image");
    }
    return kmi;
}

bool restore_legacy_ramdisk(const std::string& magiskboot, const std::string& workdir,
                            const std::string& ramdisk) {
    const auto exists = [&](const char* entry) {
        return magiskboot_query({"cpio", ramdisk, "exists " + std::string(entry)}) == 0;
    };
    constexpr std::array<const char*, 8> payload = {
        "kernelsu.ko",     "sumh.ko",          "ksuinit",        "ksu_config",
        "ksu_allow_shell", "force_debuggable", "adb_debug.prop", "stock_image.sha1",
    };
    if (exists("init.real")) {
        // cpio mv refuses to overwrite an existing destination.
        if (exists("init") && !do_cpio_cmd(magiskboot, workdir, ramdisk, "rm init"))
            return false;
        if (!do_cpio_cmd(magiskboot, workdir, ramdisk, "mv init.real init"))
            return false;
        return std::all_of(payload.begin(), payload.end(), [&](const char* entry) {
            return !exists(entry) ||
                   do_cpio_cmd(magiskboot, workdir, ramdisk, "rm " + std::string(entry));
        });
    }

    // A formerly ramdisk-free boot gets a synthetic ramdisk with no init.real.
    // Remove it only if every entry belongs to the patch; otherwise stock init
    // cannot be recovered safely without the full-image backup.
    const auto listing = exec_command_magiskboot(magiskboot, {"cpio", ramdisk, "ls -r"}, workdir);
    if (listing.exit_code != 0)
        return false;
    bool saw_entry = false;
    for (size_t begin = 0; begin < listing.stdout_str.size();) {
        const size_t end = listing.stdout_str.find('\n', begin);
        const std::string line = listing.stdout_str.substr(begin, end - begin);
        begin = end == std::string::npos ? listing.stdout_str.size() : end + 1;
        if (line.empty())
            continue;
        const size_t separator = line.rfind('\t');
        if (separator == std::string::npos)
            return false;
        std::string path = line.substr(separator + 1);
        if (!path.empty() && path.back() == '\r')
            path.pop_back();
        if (path.empty())
            return false;
        saw_entry = true;
        if (path == "." || path == "init" || path == ".backup" || path.rfind(".backup/", 0) == 0)
            continue;
        if (std::find(payload.begin(), payload.end(), path) == payload.end()) {
            LOGE("Cannot restore init without init.real or a stock backup (entry: %s)",
                 path.c_str());
            return false;
        }
    }
    std::error_code error;
    return saw_entry && fs::remove(ramdisk, error) && !error;
}

}  // namespace

bool inject_superkey_into_lkm(const std::string& lkm_path, const std::string& superkey,
                              bool signature_bypass) {
    return inject_superkey_to_lkm(lkm_path, superkey, signature_bypass);
}

bool inject_imgpatch_config_into_lkm(const std::string& lkm_path, bool allow_shell,
                                     bool enable_adbd, bool bundled_lkm,
                                     const KsuConfigArgs& config) {
    return inject_imgpatch_config_to_lkm(lkm_path, allow_shell, enable_adbd, bundled_lkm, config);
}

// Parse boot patch arguments
struct BootPatchArgs : KsuConfigArgs {
    std::string boot_image;         // -b, --boot
    std::string kernel;             // -k, --kernel
    std::string module;             // -m, --module (LKM path)
    std::string init;               // -i, --init
    std::string superkey;           // -s, --superkey
    bool signature_bypass = false;  // --signature-bypass
    bool ota = false;               // -u, --ota
    bool flash = false;             // -f, --flash
    bool backup = false;            // --backup
    std::string out;                // -o, --out
    std::string magiskboot;         // --magiskboot
    std::string kmi;                // --kmi
    std::string partition;          // --partition
    std::string out_name;           // --out-name
    bool enable_adbd = false;       // --enable-adbd
    std::string adb_debug_prop;     // --adb-debug-prop
    bool help = false;              // -h, --help
    bool valid = true;
    std::string invalid_reason;  // names the offending argument when valid is false
};

namespace {

// Without -b, boot-patch detects the boot partition itself, reads it, and -- with
// -f -- writes it back. That makes an argument this parser does not understand
// dangerous rather than merely useless: dropping a mistyped --boot silently turns
// "patch this file" into "patch the partition I picked". Every unrecognized or
// incomplete argument therefore stops the run.
void print_boot_patch_usage() {
    printf("Usage: ksud boot-patch [-b <boot.img>] [-o <dir>] [options]\n"
           "\n"
           "Without -b the boot partition is detected and read.\n"
           "Without -f nothing is written to any partition.\n"
           "\n"
           "  -b, --boot <img>       patch this image instead of the boot partition\n"
           "  -f, --flash            write the result back to the boot partition\n"
           "  -o, --out <dir>        directory to write the patched image into\n"
           "      --out-name <name>  filename for the patched image\n"
           "  -m, --module <ko>      use this LKM instead of the embedded one\n"
           "  -k, --kernel <img>     replace the kernel\n"
           "  -i, --init <bin>       replace init\n"
           "  -u, --ota              target the inactive slot\n"
           "      --partition <name> boot partition to use (boot, init_boot)\n"
           "      --kmi <kmi>        override the detected KMI\n"
           "      --backup           back up the stock image\n"
           "  -s, --superkey <key>   set the SuperKey\n"
           "      --signature-bypass relax LKM signature checking\n"
           "      --allow-shell      keep a root shell available\n"
           "      --no-preinit       skip module preinit actions\n"
           "      --no-custom-rc     skip the custom init.rc injection\n"
           "      --force-feature FEATURE=VALUE  force a configurable feature (repeatable)\n"
           "      --block-module NAME[,NAME...]  block kernel modules (repeatable; default none)\n"
           "      --enable-adbd      run adbd as root\n"
           "      --adb-debug-prop <file>  adb debug properties to embed\n"
           "      --magiskboot <path>      accepted for compatibility; ignored\n"
           "  -h, --help             show this message\n");
}

BootPatchArgs parse_boot_patch_args(const std::vector<std::string>& args) {
    BootPatchArgs result;

    // Keep the first failure. Once the command line is known to be wrong there is
    // nothing to gain from diagnosing the rest of it.
    auto reject = [&result](std::string reason) {
        if (result.valid) {
            result.valid = false;
            result.invalid_reason = std::move(reason);
        }
    };

    for (size_t i = 0; i < args.size(); i++) {
        const std::string& arg = args[i];

        // arg stays bound to the flag itself across the ++i below, so it still
        // names the right thing in the messages here.
        auto take_value = [&](std::string& destination) {
            if (i + 1 >= args.size()) {
                reject(arg + " requires a value");
                return;
            }
            destination = args[++i];
            if (destination.empty())
                reject(arg + " requires a non-empty value");
        };

        if (arg == "-h" || arg == "--help") {
            result.help = true;
        } else if (arg == "-b" || arg == "--boot") {
            take_value(result.boot_image);
        } else if (arg == "-k" || arg == "--kernel") {
            take_value(result.kernel);
        } else if (arg == "-m" || arg == "--module") {
            take_value(result.module);
        } else if (arg == "-i" || arg == "--init") {
            take_value(result.init);
        } else if (arg == "-s" || arg == "--superkey") {
            take_value(result.superkey);
        } else if (arg == "--signature-bypass") {
            result.signature_bypass = true;
        } else if (arg == "-u" || arg == "--ota") {
            result.ota = true;
        } else if (arg == "-f" || arg == "--flash") {
            result.flash = true;
        } else if (arg == "--backup") {
            result.backup = true;
        } else if (arg == "-o" || arg == "--out") {
            take_value(result.out);
        } else if (arg == "--magiskboot") {
            take_value(result.magiskboot);
        } else if (arg == "--kmi") {
            take_value(result.kmi);
        } else if (arg == "--partition") {
            take_value(result.partition);
        } else if (arg == "--out-name") {
            take_value(result.out_name);
        } else if (arg == "--allow-shell") {
            result.allow_shell = true;
        } else if (arg == "--no-preinit") {
            result.no_preinit = true;
        } else if (arg == "--no-custom-rc") {
            result.no_custom_rc = true;
        } else if (arg == "--enable-adbd") {
            result.enable_adbd = true;
        } else if (arg == "--force-feature" || arg == "--block-module") {
            std::string value;
            take_value(value);
            if (result.valid && !(arg == "--force-feature" ? result.add_force_feature(value)
                                                           : result.add_block_modules(value)))
                reject(std::string("invalid or oversized ")
                           .append(arg)
                           .append(" value: ")
                           .append(value));
        } else if (arg == "--adb-debug-prop") {
            take_value(result.adb_debug_prop);
        } else {
            reject("unknown argument: " + arg);
        }
    }

    return result;
}

enum class DirectLkmRestoreStatus : std::uint8_t {
    kNotPresent,
    kRestored,
    kFailed,
};

enum class DirectLkmImageStatus : std::uint8_t {
    kNoCapsule,
    kContainsCapsule,
    kUnverified,
};

bool has_direct_lkm_magic(const std::vector<std::uint8_t>& kernel) {
    constexpr std::array<std::uint8_t, 8> kCapsuleMagic = {'K', 'S', 'U', 'L', 'K', 'M', '1', 0};
    constexpr std::size_t kCapsuleHeaderSize = 96;
    const auto image_info = boot::lkm_image::parse_arm64_image(kernel);
    if (!image_info || image_info.value().image_size > kernel.size() ||
        image_info.value().image_size < kCapsuleHeaderSize)
        return false;

    const std::size_t image_size = image_info.value().image_size;
    for (std::size_t offset = 64; offset <= image_size - kCapsuleHeaderSize; offset += 16) {
        if (std::equal(kCapsuleMagic.begin(), kCapsuleMagic.end(),
                       kernel.begin() + static_cast<std::ptrdiff_t>(offset)))
            return true;
    }
    return false;
}

DirectLkmImageStatus inspect_direct_lkm_image(const fs::path& image, const std::string& magiskboot,
                                              const std::string& workdir) {
    if (image.empty())
        return DirectLkmImageStatus::kUnverified;

    const std::string check_workdir = workdir + "/direct-lkm-backup-check";
    if (!ensure_clean_dir(check_workdir)) {
        LOGW("Failed to create direct-LKM image check directory");
        return DirectLkmImageStatus::kUnverified;
    }

    auto unpack = exec_command_magiskboot(magiskboot, {"unpack", image.string()}, check_workdir);
    if (unpack.exit_code != 0) {
        LOGW("Failed to fully unpack image for direct-LKM validation: %s", image.string().c_str());
        if (!unpack.stderr_str.empty())
            LOGW("magiskboot stderr: %s", unpack.stderr_str.c_str());

        // A ramdisk decompression failure can happen after magiskboot has already
        // extracted the kernel. Retry without decompression before giving up.
        unpack = exec_command_magiskboot(magiskboot, {"unpack", image.string(), "--skip-decomp"},
                                         check_workdir);
    }

    const std::vector<std::string> kernel_candidates = {check_workdir + "/kernel",
                                                        check_workdir + "/kernel.img"};
    for (const auto& candidate : kernel_candidates) {
        std::vector<std::uint8_t> kernel_bytes;
        if (!read_file_bytes(candidate, &kernel_bytes))
            continue;

        if (!boot::lkm_image::parse_arm64_image(kernel_bytes))
            continue;

        if (!boot::lkm_image::contains_capsule(kernel_bytes)) {
            return has_direct_lkm_magic(kernel_bytes) ? DirectLkmImageStatus::kUnverified
                                                      : DirectLkmImageStatus::kNoCapsule;
        }

        auto restored = boot::lkm_image::remove_capsule(kernel_bytes);
        if (restored)
            return DirectLkmImageStatus::kContainsCapsule;

        LOGW("Could not validate direct-LKM capsule state for %s: %s", image.string().c_str(),
             restored.error().message.c_str());
        return DirectLkmImageStatus::kUnverified;
    }

    if (unpack.exit_code != 0)
        LOGW("No unpacked ARM64 kernel available for direct-LKM validation: %s",
             image.string().c_str());
    else
        LOGW("Image has no recognizable ARM64 kernel for direct-LKM validation: %s",
             image.string().c_str());
    return DirectLkmImageStatus::kUnverified;
}

DirectLkmRestoreStatus restore_direct_lkm_kernel(const std::string& workdir) {
    const std::vector<std::string> kernel_candidates = {workdir + "/kernel",
                                                        workdir + "/kernel.img"};
    for (const auto& candidate : kernel_candidates) {
        std::vector<std::uint8_t> kernel_bytes;
        if (!read_file_bytes(candidate, &kernel_bytes))
            continue;
        if (!boot::lkm_image::contains_capsule(kernel_bytes))
            continue;

        auto restored = boot::lkm_image::remove_capsule(kernel_bytes);
        if (!restored) {
            LOGE("Direct-LKM restore failed: %s", restored.error().message.c_str());
            return DirectLkmRestoreStatus::kFailed;
        }

        if (!write_file_bytes(candidate, restored.value().data(), restored.value().size())) {
            LOGE("Failed to write restored kernel: %s", candidate.c_str());
            return DirectLkmRestoreStatus::kFailed;
        }
        printf("- Existing direct-LKM image detected\n");
        printf("- Removed direct-LKM capsule; continuing with traditional ramdisk patch\n");
        return DirectLkmRestoreStatus::kRestored;
    }
    return DirectLkmRestoreStatus::kNotPresent;
}

struct DirectLkmBootRestore {
    std::string device;
    std::string original_image;
    std::string restored_image;
};

DirectLkmRestoreStatus prepare_direct_lkm_boot_restore(const std::string& workdir,
                                                       const std::string& magiskboot,
                                                       const std::string& bootdevice,
                                                       DirectLkmBootRestore* prepared) {
    if (prepared == nullptr)
        return DirectLkmRestoreStatus::kFailed;

    const std::string original_image = workdir + "/imgpatch-boot.img";
    if (!exec_dd(bootdevice, original_image)) {
        LOGE("Failed to read boot image from %s", bootdevice.c_str());
        return DirectLkmRestoreStatus::kFailed;
    }

    const std::string restore_workdir = workdir + "/imgpatch-boot-restore";
    if (!ensure_clean_dir(restore_workdir)) {
        LOGE("Failed to create direct-LKM restore workdir: %s", restore_workdir.c_str());
        return DirectLkmRestoreStatus::kFailed;
    }

    const auto unpack =
        exec_command_magiskboot(magiskboot, {"unpack", original_image}, restore_workdir);
    if (unpack.exit_code != 0) {
        LOGE("Failed to unpack direct-LKM boot image from %s", bootdevice.c_str());
        if (!unpack.stdout_str.empty())
            LOGE("magiskboot stdout: %s", unpack.stdout_str.c_str());
        if (!unpack.stderr_str.empty())
            LOGE("magiskboot stderr: %s", unpack.stderr_str.c_str());
        return DirectLkmRestoreStatus::kFailed;
    }

    const std::vector<std::string> kernel_candidates = {restore_workdir + "/kernel",
                                                        restore_workdir + "/kernel.img"};
    bool restored_kernel = false;
    for (const auto& candidate : kernel_candidates) {
        std::vector<std::uint8_t> kernel_bytes;
        if (!read_file_bytes(candidate, &kernel_bytes))
            continue;
        if (!boot::lkm_image::contains_capsule(kernel_bytes))
            continue;

        auto restored = boot::lkm_image::remove_capsule(kernel_bytes);
        if (!restored) {
            LOGE("Direct-LKM boot restore failed: %s", restored.error().message.c_str());
            return DirectLkmRestoreStatus::kFailed;
        }

        if (!write_file_bytes(candidate, restored.value().data(), restored.value().size())) {
            LOGE("Failed to write restored kernel: %s", candidate.c_str());
            return DirectLkmRestoreStatus::kFailed;
        }
        restored_kernel = true;
        break;
    }
    if (!restored_kernel)
        return DirectLkmRestoreStatus::kNotPresent;

    const std::string restored_image = workdir + "/imgpatch-boot-restored.img";
    const auto repack = exec_command_magiskboot(
        magiskboot, {"repack", original_image, restored_image}, restore_workdir);
    std::error_code repacked_error;
    if (repack.exit_code != 0 || !fs::is_regular_file(restored_image, repacked_error) ||
        repacked_error) {
        LOGE("Failed to repack direct-LKM boot image from %s", bootdevice.c_str());
        if (!repack.stdout_str.empty())
            LOGE("magiskboot stdout: %s", repack.stdout_str.c_str());
        if (!repack.stderr_str.empty())
            LOGE("magiskboot stderr: %s", repack.stderr_str.c_str());
        return DirectLkmRestoreStatus::kFailed;
    }

    *prepared = {bootdevice, original_image, restored_image};
    printf("- Existing direct-LKM image detected in %s\n", bootdevice.c_str());
    printf("- Repacked boot image after removing the direct-LKM capsule\n");
    return DirectLkmRestoreStatus::kRestored;
}

int boot_patch_impl(const std::vector<std::string>& args) {
    auto parsed = parse_boot_patch_args(args);
    if (parsed.help) {
        print_boot_patch_usage();
        return 0;
    }
    if (!parsed.valid) {
        LOGE("boot-patch: %s", parsed.invalid_reason.c_str());
        print_boot_patch_usage();
        return 1;
    }
    const std::string ota_slot = parsed.ota ? get_slot_suffix(true) : "";
    if (parsed.ota && ota_slot.empty()) {
        LOGE("Inactive-slot install requires a valid current slot suffix (_a or _b)");
        return 1;
    }
    (void)setvbuf(stdout, nullptr, _IONBF, 0);
    (void)setvbuf(stderr, nullptr, _IONBF, 0);
    printf("\n");
    printf(" _____  __   __  ____   _   _\n");
    printf("|__  /  \\ \\ / / / ___| | | | |\n");
    printf("  / /    \\ V /  \\___ \\ | | | |\n");
    printf(" / /_     | |    ___) || |_| |\n");
    printf("/____|    |_|   |____/  \\___/\n");
    printf("\n");

    const auto temporary = create_boot_workdir();
    if (!temporary) {
        LOGE("Failed to create temp directory");
        LOGE("Try setting TMPDIR environment variable to a writable directory");
        return 1;
    }
    const std::string workdir = temporary->string();

    // Keep the original partition snapshots if a rollback cannot finish.
    bool preserve_workdir = false;
    auto cleanup = [&workdir, &preserve_workdir]() {
        if (preserve_workdir) {
            LOGE("Recovery images retained in %s", workdir.c_str());
            return;
        }
        std::error_code ec;
        fs::remove_all(workdir, ec);
        if (ec) {
            LOGW("Failed to remove temporary directory %s: %s", workdir.c_str(),
                 ec.message().c_str());
        }
    };

    // Find magiskboot
    const std::string magiskboot = find_magiskboot(parsed.magiskboot, workdir);
    if (magiskboot.empty()) {
        cleanup();
        return 1;
    }
    printf("- Using magiskboot: %s\n", magiskboot.c_str());

    // Get or detect KMI
    std::string kmi = parsed.kmi;
    if (kmi.empty() && (!parsed.kernel.empty() || !parsed.boot_image.empty())) {
        // Asset selection must follow the kernel that will actually boot.
        kmi = !parsed.kernel.empty() ? parse_kmi_from_kernel_file(parsed.kernel)
                                     : parse_kmi_from_boot(magiskboot, workdir, parsed.boot_image);
        if (kmi.empty()) {
            LOGE("Cannot detect the selected kernel KMI; specify the target KMI with --kmi "
                 "(required for an init_boot image without a kernel)");
            cleanup();
            return 1;
        }
    } else if (kmi.empty() && parsed.ota) {
        const std::string target_boot = "/dev/block/by-name/boot" + ota_slot;
        printf("- Trying to auto detect KMI version from %s\n", target_boot.c_str());
        kmi = parse_kmi_from_boot(magiskboot, workdir, target_boot);
        if (kmi.empty()) {
            printf("! Failed to detect KMI from inactive slot boot image\n");
            printf("! Please specify the target kernel with --kmi\n");
            cleanup();
            return 1;
        }
    }
    if (kmi.empty()) {
        kmi = get_current_kmi();
    }
    if (!is_supported_kmi(kmi)) {
        printf("! ZySU requires Linux 6.1 or newer (target KMI: %s)\n",
               kmi.empty() ? "unknown" : kmi.c_str());
        if (kmi.empty())
            printf("! Specify the target kernel with --kmi\n");
        cleanup();
        return 1;
    }
    printf("- KMI: %s\n", kmi.c_str());

    // Determine boot image path
    std::string bootimage;
    std::string bootdevice;
    std::string rollback_bootimage;
    const bool patch_file = !parsed.boot_image.empty();

    if (patch_file) {
        std::error_code path_error;
        bootimage = fs::absolute(parsed.boot_image, path_error).string();
        if (path_error) {
            LOGE("Failed to resolve boot image path");
            cleanup();
            return 1;
        }
        if (access(bootimage.c_str(), R_OK) != 0) {
            LOGE("Boot image not found: %s", bootimage.c_str());
            cleanup();
            return 1;
        }
    } else {
        // Auto-detect boot partition
        std::string partition_name;

        // Determine if we're in replace kernel mode (when --kernel is specified)
        const bool is_replace_kernel = !parsed.kernel.empty();

        if (!parsed.partition.empty()) {
            // User specified partition name (e.g., "init_boot" or "boot")
            partition_name =
                choose_boot_partition(parsed.ota, &parsed.partition, is_replace_kernel);
        } else {
            // Auto-detect: choose_boot_partition returns full path with slot
            partition_name = choose_boot_partition(parsed.ota, nullptr, is_replace_kernel);
        }

        if (partition_name.empty()) {
            LOGE("Failed to resolve a safe boot partition");
            cleanup();
            return 1;
        }
        printf("- Bootdevice: %s\n", partition_name.c_str());

        bootimage = workdir + "/boot.img";
        if (!exec_dd(partition_name, bootimage)) {
            LOGE("Failed to read boot image from %s", partition_name.c_str());
            cleanup();
            return 1;
        }
        bootdevice = partition_name;
        rollback_bootimage = bootimage;
    }

    // Prepare LKM module
    printf("- Preparing assets\n");
    const std::string kmod_file = workdir + "/kernelsu.ko";
    bool bundled_lkm = false;

    if (!parsed.module.empty()) {
        // Use specified module
        if (!copy_file_data(parsed.module, kmod_file)) {
            LOGE("Failed to copy kernel module from %s", parsed.module.c_str());
            cleanup();
            return 1;
        }
    } else {
        // Try to extract LKM from embedded assets first
        const std::string kmi_lkm_name = kmi + "_kernelsu.ko";
        printf("- KMI: %s\n", kmi.c_str());

        if (copy_asset_to_file(kmi_lkm_name, kmod_file)) {
            bundled_lkm = true;
            printf("- Using embedded LKM: %s\n", kmi_lkm_name.c_str());
        } else {
            // Fallback: try to find LKM from known locations
            const std::vector<std::string> search_paths = {
                std::string(BINARY_DIR) + kmi_lkm_name,  std::string(BINARY_DIR) + "kernelsu.ko",
                std::string(WORKING_DIR) + kmi_lkm_name, std::string(WORKING_DIR) + "kernelsu.ko",
                "/data/local/tmp/" + kmi_lkm_name,       "/data/local/tmp/kernelsu.ko",
            };

            bool found = false;
            for (const auto& path : search_paths) {
                if (access(path.c_str(), R_OK) == 0) {
                    printf("- Found LKM at %s\n", path.c_str());
                    if (copy_file_data(path, kmod_file)) {
                        found = true;
                        break;
                    }
                }
            }

            if (!found) {
                // List available KMIs from embedded assets
                auto supported = list_supported_kmi();

                printf("\n");
                printf("! No LKM module found for KMI: %s\n", kmi.c_str());
                printf("!\n");
                if (!supported.empty()) {
                    printf("! Supported KMIs in this build:\n");
                    for (const auto& k : supported) {
                        printf("!   - %s\n", k.c_str());
                    }
                    printf("!\n");
                }
                printf("! Please select an LKM file in Manager, or place it at:\n");
                printf("!   %s%s\n", BINARY_DIR, kmi_lkm_name.c_str());
                printf("!\n");
                printf("! You can download LKM from:\n");
                printf("!   https://github.com/wuluoy-creator/ZySU/releases\n");
                printf("\n");
                cleanup();
                return 1;
            }
        }
    }

    // Always inject verification mode (and SuperKey hash if set).
    // Pure signature mode (no superkey) still needs flags=0 to be explicitly written,
    // otherwise the LKM may have stale/wrong values and signature verification fails.
    if (!parsed.superkey.empty()) {
        printf("- Injecting SuperKey into LKM\n");
    } else if (parsed.signature_bypass) {
        printf("- Warning: signature_bypass requires superkey to be set, ignoring\n");
    }
    if (!inject_superkey_to_lkm(kmod_file, parsed.superkey, parsed.signature_bypass)) {
        LOGE("Failed to inject SuperKey data into LKM");
        cleanup();
        return 1;
    }

    // Prepare init if specified
    const std::string init_file = workdir + "/init";
    if (!parsed.init.empty()) {
        if (!copy_file_data(parsed.init, init_file, 0755)) {
            LOGE("Failed to copy init from %s", parsed.init.c_str());
            cleanup();
            return 1;
        }
    } else {
        // Try to extract ksuinit from embedded assets first.
        if (copy_asset_to_file("ksuinit", init_file)) {
            printf("- Using embedded ksuinit\n");
        } else {
            // Fallback: check standard location
            const std::string ksuinit_path = std::string(BINARY_DIR) + "ksuinit";
            if (access(ksuinit_path.c_str(), R_OK) == 0 &&
                copy_file_data(ksuinit_path, init_file, 0755)) {
                printf("- Using ksuinit from %s\n", ksuinit_path.c_str());
            } else {
                LOGE("ksuinit not found in embedded assets or %s", ksuinit_path.c_str());
                LOGE("Please install KernelSU Manager or rebuild ksud with ksuinit embedded");
                cleanup();
                return 1;
            }
        }
    }
    chmod(init_file.c_str(), 0755);

    // Unpack boot image (must run in workdir so output files go there)
    printf("- Unpacking boot image\n");
    (void)fflush(stdout);
    printf("- magiskboot: %s\n", magiskboot.c_str());
    printf("- bootimage: %s\n", bootimage.c_str());
    printf("- workdir: %s\n", workdir.c_str());

    // Verify boot image exists and is readable
    struct stat boot_stat{};
    if (stat(bootimage.c_str(), &boot_stat) != 0) {
        LOGE("Boot image not accessible: %s (errno=%d)", bootimage.c_str(), errno);
        cleanup();
        return 1;
    }
    printf("- Boot image size: %ld bytes\n", static_cast<long>(boot_stat.st_size));

    // Try normal unpack first (with decompress). On some devices LZ4_LEGACY in-process decompress
    // crashes (SIGSEGV, exit 139); then we retry with --skip-decomp and tell user to use PC.
    std::vector<std::string> unpack_args = {"unpack", bootimage};  // NOLINT(misc-const-correctness)
    auto unpack_result = exec_command_magiskboot(magiskboot, unpack_args, workdir);
    printf("- unpack exit code: %d\n", unpack_result.exit_code);
    if (!unpack_result.stdout_str.empty()) {
        printf("- stdout: %s\n", unpack_result.stdout_str.c_str());
    }
    if (!unpack_result.stderr_str.empty()) {
        printf("- stderr: %s\n", unpack_result.stderr_str.c_str());
    }

#ifdef __ANDROID__
    constexpr int SIGSEGV_EXIT = 128 + 11;  // 139
    if (unpack_result.exit_code == SIGSEGV_EXIT) {
        printf(
            "- Unpack crashed (SIGSEGV); retrying with --skip-decomp to avoid LZ4 decompress.\n");
        unpack_args.push_back("--skip-decomp");
        unpack_result = exec_command_magiskboot(magiskboot, unpack_args, workdir);
        printf("- unpack (skip-decomp) exit code: %d\n", unpack_result.exit_code);
        if (unpack_result.exit_code != 0) {
            LOGE("magiskboot unpack failed with exit code %d", unpack_result.exit_code);
            cleanup();
            return 1;
        }
        const std::string ramdisk_cpio = workdir + "/ramdisk.cpio";
        if (has_lz4_legacy_magic(ramdisk_cpio)) {
            LOGE("Ramdisk is LZ4_LEGACY; on-device decompress crashed (SIGSEGV), so it was "
                 "skipped.");
            LOGE("Cannot continue without decompressed ramdisk. Patch the boot image on a PC "
                 "instead.");
            cleanup();
            return 1;
        }
    } else
#endif  // #ifdef __ANDROID__
        if (unpack_result.exit_code != 0) {
            LOGE("magiskboot unpack failed with exit code %d", unpack_result.exit_code);
            cleanup();
            return 1;
        }

    const auto direct_lkm_status = restore_direct_lkm_kernel(workdir);
    if (direct_lkm_status == DirectLkmRestoreStatus::kFailed) {
        cleanup();
        return 1;
    }
    std::string backup_source = bootimage;
    if (direct_lkm_status == DirectLkmRestoreStatus::kRestored) {
        backup_source = workdir + "/direct-lkm-restored.img";
        auto restore_repack =
            exec_command_magiskboot(magiskboot, {"repack", bootimage, backup_source}, workdir);
        std::error_code restore_repack_error;
        if (restore_repack.exit_code != 0 ||
            !fs::is_regular_file(backup_source, restore_repack_error) || restore_repack_error) {
            LOGE("Failed to repack boot image after removing the direct-LKM capsule");
            if (!restore_repack.stdout_str.empty())
                LOGE("magiskboot stdout: %s", restore_repack.stdout_str.c_str());
            if (!restore_repack.stderr_str.empty())
                LOGE("magiskboot stderr: %s", restore_repack.stderr_str.c_str());
            cleanup();
            return 1;
        }
    }

    // Replace only after reconstructing the stock backup, so that restoration
    // keeps the original kernel rather than the user-supplied replacement.
    if (!parsed.kernel.empty()) {
        const std::string kernel_file = workdir + "/kernel";
        std::error_code kernel_error;
        if (!fs::is_regular_file(kernel_file, kernel_error) || kernel_error ||
            fs::file_size(kernel_file, kernel_error) == 0 || kernel_error) {
            LOGE("The selected image has no kernel to replace; use a boot image");
            cleanup();
            return 1;
        }
        if (!fs::is_regular_file(parsed.kernel, kernel_error) || kernel_error ||
            fs::file_size(parsed.kernel, kernel_error) == 0 || kernel_error ||
            !copy_file_data(parsed.kernel, kernel_file)) {
            LOGE("Failed to copy replacement kernel from %s", parsed.kernel.c_str());
            cleanup();
            return 1;
        }
        printf("- Replaced kernel from %s\n", parsed.kernel.c_str());
    }

    // Find ramdisk
    std::string ramdisk;
    const std::vector<std::string> ramdisk_candidates_do_patch = {
        workdir + "/ramdisk.cpio", workdir + "/vendor_ramdisk/init_boot.cpio",
        workdir + "/vendor_ramdisk/ramdisk.cpio"};

    for (const auto& candidate : ramdisk_candidates_do_patch) {
        if (access(candidate.c_str(), R_OK) == 0) {
            ramdisk = candidate;
            break;
        }
    }

    if (ramdisk.empty()) {
        printf("- No ramdisk found, creating default\n");
        ramdisk = workdir + "/ramdisk.cpio";
        // Create empty ramdisk (use a valid entry name; "." is invalid for some magiskboot builds)
        if (!do_cpio_cmd(magiskboot, workdir, ramdisk, "mkdir 000 .backup")) {
            LOGE("Failed to create default ramdisk");
            cleanup();
            return 1;
        }
    } else {
        // Unconditionally reject LZ4 ramdisk before any cpio (avoids cpio parsing LZ4 as cpio →
        // huge cache/hang).
        if (has_lz4_legacy_magic(ramdisk)) {
            LOGE("Ramdisk is LZ4 compressed; cannot patch. Use PC to patch this boot image.\n");
            cleanup();
            return 1;
        }
    }

    // Check for Magisk
    if (is_magisk_patched(magiskboot, workdir, ramdisk)) {
        LOGE("Cannot work with Magisk patched image");
        cleanup();
        return 1;
    }

    printf("- Adding KernelSU LKM\n");
    const bool already_patched = is_kernelsu_patched(magiskboot, workdir, ramdisk);

    if (!already_patched) {
        // Backup init if it exists
        if (magiskboot_query({"cpio", ramdisk, "exists init"}) == 0) {
            do_cpio_cmd(magiskboot, workdir, ramdisk, "mv init init.real");
        }
    }

    // Add init and kernelsu.ko (use workdir for relative paths in cpio add)
    if (!do_cpio_cmd(magiskboot, workdir, ramdisk, "add 0755 init init")) {
        cleanup();
        return 1;
    }
    if (!do_cpio_cmd(magiskboot, workdir, ramdisk, "add 0755 kernelsu.ko kernelsu.ko")) {
        cleanup();
        return 1;
    }

    const std::string config_text = parsed.module_params(bundled_lkm);
    if (!config_text.empty()) {
        if (!write_file(workdir + "/ksu_config", config_text)) {
            LOGE("Failed to create ksu_config");
            cleanup();
            return 1;
        }
        if (!do_cpio_cmd(magiskboot, workdir, ramdisk, "add 0644 ksu_config ksu_config")) {
            cleanup();
            return 1;
        }
    } else if (magiskboot_query({"cpio", ramdisk, "exists ksu_config"}) == 0 &&
               !do_cpio_cmd(magiskboot, workdir, ramdisk, "rm ksu_config")) {
        // Omitting the options must clear old overrides, including a block list.
        cleanup();
        return 1;
    }

    if (magiskboot_query({"cpio", ramdisk, "exists ksu_allow_shell"}) == 0) {
        printf("- Removing legacy allow shell config\n");
        do_cpio_cmd(magiskboot, workdir, ramdisk, "rm ksu_allow_shell");
    }

    if (parsed.enable_adbd || !parsed.adb_debug_prop.empty()) {
        printf("- Adding adb debug props\n");
        if (!write_file(workdir + "/force_debuggable", "")) {
            LOGE("Failed to create force_debuggable");
            cleanup();
            return 1;
        }
        if (!do_cpio_cmd(magiskboot, workdir, ramdisk,
                         "add 0644 force_debuggable force_debuggable")) {
            cleanup();
            return 1;
        }

        std::string adb_props;
        if (parsed.enable_adbd) {
            printf("- Enabling adbd debug props\n");
            adb_props = "ro.debuggable=1\nro.force.debuggable=1\nro.adb.secure=0\n";
        }
        if (!parsed.adb_debug_prop.empty()) {
            printf("- Appending custom adb props\n");
            const auto custom_props = read_file(parsed.adb_debug_prop);
            if (!custom_props) {
                LOGE("Failed to read adb debug properties from %s", parsed.adb_debug_prop.c_str());
                cleanup();
                return 1;
            }
            adb_props += *custom_props;
            if (!adb_props.empty() && adb_props.back() != '\n')
                adb_props += '\n';
        }
        if (!write_file(workdir + "/adb_debug.prop", adb_props)) {
            LOGE("Failed to create adb_debug.prop");
            cleanup();
            return 1;
        }
        if (!do_cpio_cmd(magiskboot, workdir, ramdisk, "add 0644 adb_debug.prop adb_debug.prop")) {
            cleanup();
            return 1;
        }
    } else {
        if (magiskboot_query({"cpio", ramdisk, "exists force_debuggable"}) == 0) {
            printf("- Removing /force_debuggable\n");
            do_cpio_cmd(magiskboot, workdir, ramdisk, "rm force_debuggable");
        }

        if (magiskboot_query({"cpio", ramdisk, "exists adb_debug.prop"}) == 0) {
            printf("- Removing /adb_debug.prop\n");
            do_cpio_cmd(magiskboot, workdir, ramdisk, "rm adb_debug.prop");
        }
    }

    // Remove the legacy embedded module when repatching an image created by an older manager.
    if (magiskboot_query({"cpio", ramdisk, "exists sumh.ko"}) == 0) {
        do_cpio_cmd(magiskboot, workdir, ramdisk, "rm sumh.ko");
    }

    // Direct flashing automatically backs up stock images. --backup also allows an explicitly
    // selected file to be treated as stock, even when it appears to have been patched already.
    if (parsed.backup || (!already_patched && parsed.flash)) {
        if (!do_backup(magiskboot, workdir, ramdisk, backup_source)) {
            printf("- Warning: Backup stock image failed\n");
        }
    }

    // Repack boot image (must run in workdir where unpack output files are)
    // Pass explicit output path for compatibility with older magiskboot variants
    // that require: repack <in-boot.img> <out-boot.img>.
    // Output filename must match input format: boot -> new-boot.img, init_boot -> new-init_boot.img
    // (aligned with Magisk payload extract: partition.partition_name -> {partition}.img)
    const bool is_init_boot =
        (patch_file && bootimage.find("init_boot") != std::string::npos) ||
        (!bootdevice.empty() && bootdevice.find("init_boot") != std::string::npos);
    const std::string new_boot =
        workdir + "/" + (is_init_boot ? "new-init_boot.img" : "new-boot.img");
    printf("- Repacking boot image\n");
    (void)fflush(stdout);
    // MagiskbootAlone supports LZ4_LEGACY, GZIP, and LZ4 compression natively.
    // Do NOT use --skip-comp: the ramdisk must be recompressed to fit the
    // partition. Without recompression the uncompressed ramdisk makes the output
    // image far larger than the original, exhausting cache space and overflowing
    // the boot partition.
    auto repack_result =
        exec_command_magiskboot(magiskboot, {"repack", bootimage, new_boot}, workdir);
    if (repack_result.exit_code != 0) {
        LOGE("magiskboot repack failed (exit code %d)", repack_result.exit_code);
        if (!repack_result.stdout_str.empty()) {
            LOGE("magiskboot repack stdout: %s", repack_result.stdout_str.c_str());
        }
        if (!repack_result.stderr_str.empty()) {
            LOGE("magiskboot repack stderr: %s", repack_result.stderr_str.c_str());
        }
        cleanup();
        return 1;
    }
    std::error_code output_error;
    if (!fs::is_regular_file(new_boot, output_error) || output_error ||
        fs::file_size(new_boot, output_error) == 0 || output_error) {
        LOGE("magiskboot repack reported success but output is missing or empty: %s",
             new_boot.c_str());
        LOGE("Current magiskboot may be an incomplete build (unpack/repack stubs).");
        LOGE("Please use a full magiskboot implementation from upstream Magisk.");
        cleanup();
        return 1;
    }

    std::optional<DirectLkmBootRestore> companion_boot_restore;
    if (parsed.flash && !bootdevice.empty()) {
        const std::string companion_bootdevice =
            "/dev/block/by-name/boot" + get_slot_suffix(parsed.ota);
        if (companion_bootdevice != bootdevice) {
            DirectLkmBootRestore prepared;
            const auto companion_status = prepare_direct_lkm_boot_restore(
                workdir, magiskboot, companion_bootdevice, &prepared);
            if (companion_status == DirectLkmRestoreStatus::kFailed) {
                cleanup();
                return 1;
            }
            if (companion_status == DirectLkmRestoreStatus::kRestored)
                companion_boot_restore = std::move(prepared);
        }
    }

    // Output patched image
    if (patch_file) {
        const std::string output_dir = parsed.out.empty() ? "." : parsed.out;
        std::string name = parsed.out_name;
        if (name.empty()) {
            const time_t now = time(nullptr);
            const struct tm* tm_info = localtime(&now);
            std::array<char, 32> time_str{};
            if (tm_info != nullptr &&
                strftime(time_str.data(), time_str.size(), "%Y%m%d_%H%M%S", tm_info) != 0) {
                name = std::string("kernelsu_patched_") + time_str.data() + ".img";
            } else {
                name = "kernelsu_patched.img";
            }
        }

        const std::string output_image = output_dir + "/" + name;

        if (!copy_file_data(new_boot, output_image)) {
            LOGE("Failed to write output file");
            cleanup();
            return 1;
        }

        printf("- Output file is written to\n");
        printf("- %s\n", output_image.c_str());
    }

    // Flash if requested
    if (parsed.flash && !bootdevice.empty()) {
        printf("- Flashing new boot image\n");
        if (!flash_boot(bootdevice, new_boot)) {
            LOGE("Failed to flash boot image; restoring the original image");
            const std::string& rollback_image =
                rollback_bootimage.empty() ? bootimage : rollback_bootimage;
            if (!flash_boot(bootdevice, rollback_image)) {
                LOGE("Rollback failed; original image: %s", rollback_image.c_str());
                preserve_workdir = true;
            }
            cleanup();
            return 1;
        }
        if (companion_boot_restore &&
            !flash_boot(companion_boot_restore->device, companion_boot_restore->restored_image)) {
            LOGE("Failed to restore direct-LKM boot image; rolling back both partitions");
            if (!flash_boot(companion_boot_restore->device,
                            companion_boot_restore->original_image)) {
                LOGE("Failed to roll back direct-LKM boot image");
                preserve_workdir = true;
            }
            const std::string& rollback_image =
                rollback_bootimage.empty() ? bootimage : rollback_bootimage;
            if (!flash_boot(bootdevice, rollback_image)) {
                LOGE("Failed to roll back traditional boot patch");
                preserve_workdir = true;
            }
            cleanup();
            return 1;
        }
        if (companion_boot_restore)
            printf("- Removed direct-LKM capsule from %s\n",
                   companion_boot_restore->device.c_str());
    }

    cleanup();

    printf("- Done!\n");
    return 0;
}

// Parse boot restore arguments
struct BootRestoreArgs {
    std::string boot_image;  // -b, --boot
    bool flash = false;      // -f, --flash
    std::string magiskboot;  // --magiskboot
    std::string out_name;    // --out-name
    bool help = false;       // -h, --help
    bool valid = true;
    std::string invalid_reason;
};

// boot-restore flashes and prunes the stock backups, so it gets the same
// treatment as boot-patch: an argument this parser cannot place stops the run
// instead of leaving -b empty and letting the partition path take over.
void print_boot_restore_usage() {
    printf("Usage: ksud boot-restore [-b <boot.img>] [options]\n"
           "\n"
           "Without -b the boot partition is detected and read.\n"
           "Without -f nothing is written to any partition.\n"
           "\n"
           "  -b, --boot <img>       restore from this image instead of the partition\n"
           "  -f, --flash            write the restored image back to the partition\n"
           "      --out-name <name>  filename for the restored image\n"
           "      --magiskboot <path>  accepted for compatibility; ignored\n"
           "  -h, --help             show this message\n");
}

BootRestoreArgs parse_boot_restore_args(const std::vector<std::string>& args) {
    BootRestoreArgs result;

    auto reject = [&result](std::string reason) {
        if (result.valid) {
            result.valid = false;
            result.invalid_reason = std::move(reason);
        }
    };

    for (size_t i = 0; i < args.size(); i++) {
        const std::string& arg = args[i];

        auto take_value = [&](std::string& destination) {
            if (i + 1 >= args.size()) {
                reject(arg + " requires a value");
                return;
            }
            destination = args[++i];
            if (destination.empty())
                reject(arg + " requires a non-empty value");
        };

        if (arg == "-h" || arg == "--help") {
            result.help = true;
        } else if (arg == "-b" || arg == "--boot") {
            take_value(result.boot_image);
        } else if (arg == "-f" || arg == "--flash") {
            result.flash = true;
        } else if (arg == "--magiskboot") {
            take_value(result.magiskboot);
        } else if (arg == "--out-name") {
            take_value(result.out_name);
        } else {
            reject("unknown argument: " + arg);
        }
    }

    return result;
}

std::string make_restore_output_name(const std::string& requested) {
    if (!requested.empty())
        return requested;
    const time_t now = time(nullptr);
    const struct tm* tm_info = localtime(&now);
    std::array<char, 32> time_str{};
    if (tm_info != nullptr &&
        strftime(time_str.data(), time_str.size(), "%Y%m%d_%H%M%S", tm_info) != 0)
        return std::string("kernelsu_restore_") + time_str.data() + ".img";
    return "kernelsu_restore.img";
}

}  // namespace

int boot_patch(const std::vector<std::string>& args) {
    return boot_patch_impl(args);
}

int boot_restore(const std::vector<std::string>& args) {
    auto parsed = parse_boot_restore_args(args);
    if (parsed.help) {
        print_boot_restore_usage();
        return 0;
    }
    if (!parsed.valid) {
        LOGE("boot-restore: %s", parsed.invalid_reason.c_str());
        print_boot_restore_usage();
        return 1;
    }
    // Create temp working directory
    std::array<char, 32> tmpdir_buf{};
    (void)strncpy(tmpdir_buf.data(), "/data/local/tmp/KernelSU_XXXXXX", tmpdir_buf.size() - 1);
    tmpdir_buf[tmpdir_buf.size() - 1] = '\0';
    const char* tmpdir = mkdtemp(tmpdir_buf.data());
    if (!tmpdir) {
        LOGE("Failed to create temp directory");
        return 1;
    }
    std::string workdir = tmpdir;

    bool preserve_workdir = false;
    auto cleanup = [&workdir, &preserve_workdir]() {
        if (preserve_workdir) {
            LOGE("Recovery images retained in %s", workdir.c_str());
            return;
        }
        std::error_code ec;
        fs::remove_all(workdir, ec);
        if (ec) {
            LOGW("Failed to remove temporary directory %s: %s", workdir.c_str(),
                 ec.message().c_str());
        }
    };

    // Find magiskboot
    const std::string magiskboot = find_magiskboot(parsed.magiskboot, workdir);
    if (magiskboot.empty()) {
        cleanup();
        return 1;
    }

    bool prefer_boot_partition = false;
    std::string restore_slot;
    if (parsed.boot_image.empty()) {
        restore_slot = get_slot_suffix(false);
        const std::string current_boot_device = "/dev/block/by-name/boot" + restore_slot;
        const fs::path probe = fs::path(workdir) / "boot-probe.img";
        if (!exec_dd(current_boot_device, probe.string())) {
            LOGE("Failed to inspect the current boot image before restoration");
            cleanup();
            return 1;
        }
        const auto status = inspect_direct_lkm_image(probe, magiskboot, workdir);
        if (status == DirectLkmImageStatus::kUnverified) {
            LOGE("Cannot safely determine whether the current boot image contains ImgPatch");
            cleanup();
            return 1;
        }
        prefer_boot_partition = status == DirectLkmImageStatus::kContainsCapsule;
    }

    // Determine boot image path
    std::string bootimage;
    std::string bootdevice;

    if (!parsed.boot_image.empty()) {
        std::error_code path_error;
        bootimage = fs::absolute(parsed.boot_image, path_error).string();
        if (path_error) {
            LOGE("Failed to resolve boot image path");
            cleanup();
            return 1;
        }
        if (access(bootimage.c_str(), R_OK) != 0) {
            LOGE("Boot image not found: %s", bootimage.c_str());
            cleanup();
            return 1;
        }
    } else {
        // Auto-detect boot partition (restore doesn't replace kernel)
        const std::string partition_name = prefer_boot_partition
                                               ? "/dev/block/by-name/boot" + restore_slot
                                               : choose_boot_partition(false, nullptr, false);
        if (partition_name.empty()) {
            LOGE("Failed to resolve a safe boot partition");
            cleanup();
            return 1;
        }
        printf("- Bootdevice: %s\n", partition_name.c_str());

        bootimage = workdir + "/boot.img";
        if (!exec_dd(partition_name, bootimage)) {
            LOGE("Failed to read boot image");
            cleanup();
            return 1;
        }
        bootdevice = partition_name;
    }

    // Unpack boot image (must run in workdir so output files go there)
    printf("- Unpacking boot image\n");
    std::vector<std::string> unpack_args_restore = {"unpack",
                                                    bootimage};  // NOLINT(misc-const-correctness)
    auto unpack_result = exec_command_magiskboot(magiskboot, unpack_args_restore, workdir);
#ifdef __ANDROID__
    constexpr int SIGSEGV_EXIT_R = 128 + 11;  // 139
    if (unpack_result.exit_code == SIGSEGV_EXIT_R) {
        printf("- Unpack crashed (SIGSEGV); retrying with --skip-decomp.\n");
        unpack_args_restore.push_back("--skip-decomp");
        unpack_result = exec_command_magiskboot(magiskboot, unpack_args_restore, workdir);
    }
    if (unpack_result.exit_code == 0) {
        const std::string rd_cpio = workdir + "/ramdisk.cpio";
        if (has_lz4_legacy_magic(rd_cpio)) {
            LOGE("Ramdisk is LZ4_LEGACY; on-device decompress crashed (SIGSEGV), so it was "
                 "skipped.");
            LOGE("Restore or patch the boot image on a PC instead.");
            cleanup();
            return 1;
        }
    }
#endif  // #ifdef __ANDROID__
    if (unpack_result.exit_code != 0) {
        LOGE("magiskboot unpack failed");
        if (!unpack_result.stderr_str.empty()) {
            LOGE("stderr: %s", unpack_result.stderr_str.c_str());
        }
        cleanup();
        return 1;
    }

    std::string new_boot;
    bool from_backup = false;
    bool direct_restore = false;
    std::optional<boot_backup::Backup> used_backup;

    // Direct-LKM images have no ramdisk payload to restore. Remove the capsule
    // from the unpacked kernel and repack the complete boot container below.
    const std::vector<std::string> kernel_candidates = {workdir + "/kernel",
                                                        workdir + "/kernel.img"};
    for (const auto& candidate : kernel_candidates) {
        std::vector<std::uint8_t> kernel_bytes;
        if (!read_file_bytes(candidate, &kernel_bytes))
            continue;
        if (!boot::lkm_image::contains_capsule(kernel_bytes))
            continue;
        auto backup = boot_backup::find_imgpatch(bootimage, workdir);
        if (backup && inspect_direct_lkm_image(backup->image, magiskboot, workdir) ==
                          DirectLkmImageStatus::kNoCapsule) {
            new_boot = backup->image.string();
            used_backup = std::move(backup);
            from_backup = true;
            direct_restore = true;
            printf("- Restoring the stock backup matched to the current ImgPatch image\n");
            break;
        }
        auto restored = boot::lkm_image::remove_capsule(kernel_bytes);
        if (!restored) {
            LOGE("Direct-LKM restore failed: %s", restored.error().message.c_str());
            cleanup();
            return 1;
        }
        if (!write_file_bytes(candidate, restored.value().data(), restored.value().size())) {
            LOGE("Failed to write restored kernel");
            cleanup();
            return 1;
        }
        const std::string out_img = workdir + "/new-boot.img";
        printf("- Removing direct-LKM capsule\n");
        auto repack_result =
            exec_command_magiskboot(magiskboot, {"repack", bootimage, out_img}, workdir);
        std::error_code repacked_error;
        if (repack_result.exit_code != 0 || !fs::is_regular_file(out_img, repacked_error) ||
            repacked_error) {
            LOGE("magiskboot repack failed");
            if (!repack_result.stdout_str.empty())
                LOGE("magiskboot stdout: %s", repack_result.stdout_str.c_str());
            if (!repack_result.stderr_str.empty())
                LOGE("magiskboot stderr: %s", repack_result.stderr_str.c_str());
            cleanup();
            return 1;
        }
        new_boot = out_img;
        direct_restore = true;
        break;
    }

    if (!direct_restore) {
        // Find ramdisk
        std::string ramdisk;
        const std::vector<std::string> ramdisk_candidates = {
            workdir + "/ramdisk.cpio", workdir + "/vendor_ramdisk/init_boot.cpio",
            workdir + "/vendor_ramdisk/ramdisk.cpio"};

        for (const auto& candidate : ramdisk_candidates) {
            if (access(candidate.c_str(), R_OK) == 0) {
                ramdisk = candidate;
                break;
            }
        }

        if (ramdisk.empty()) {
            LOGE("No compatible ramdisk found");
            cleanup();
            return 1;
        }

        // Check if patched by KernelSU
        if (!is_kernelsu_patched(magiskboot, workdir, ramdisk)) {
            LOGE("Boot image is not patched by KernelSU");
            cleanup();
            return 1;
        }

        // Try to find backup
        if (magiskboot_query({"cpio", ramdisk, "exists " + std::string(BACKUP_FILENAME)}) == 0) {
            // Extract backup sha1
            const std::string backup_file = workdir + "/" + BACKUP_FILENAME;
            exec_command_magiskboot(
                magiskboot,
                {"cpio", ramdisk, "extract " + std::string(BACKUP_FILENAME) + " " + backup_file},
                workdir);

            auto sha_content = read_file(backup_file);
            if (sha_content) {
                const std::string sha = trim(*sha_content);
                if (auto backup = boot_backup::find_stock(sha, workdir)) {
                    new_boot = backup->image.string();
                    used_backup = std::move(backup);
                    from_backup = true;
                } else {
                    printf("- No valid stock backup matched the ramdisk hash\n");
                }
            }
        } else {
            printf("- Backup info is absent!\n");
        }

        // If no backup, manually remove KernelSU
        if (!from_backup) {
            if (!restore_legacy_ramdisk(magiskboot, workdir, ramdisk)) {
                LOGE("Failed to restore the original ramdisk; no image will be flashed");
                cleanup();
                return 1;
            }

            // Repack (must run in workdir where unpack output files are)
            // Output filename must match input format: boot -> new-boot.img, init_boot ->
            // new-init_boot.img
            const bool is_init_boot_restore =
                (!parsed.boot_image.empty() &&
                 parsed.boot_image.find("init_boot") != std::string::npos) ||
                (!bootdevice.empty() && bootdevice.find("init_boot") != std::string::npos);
            const std::string out_img =
                workdir + "/" + (is_init_boot_restore ? "new-init_boot.img" : "new-boot.img");
            printf("- Repacking boot image\n");
            auto repack_result =
                exec_command_magiskboot(magiskboot, {"repack", bootimage, out_img}, workdir);
            if (repack_result.exit_code != 0) {
                LOGE("magiskboot repack failed");
                cleanup();
                return 1;
            }
            new_boot = out_img;
        }
    }

    // Output restored image
    if (!parsed.boot_image.empty()) {
        std::string name = parsed.out_name;
        if (name.empty()) {
            name = make_restore_output_name("");
        }

        const std::string output_image = "./" + name;

        if (!copy_file_data(new_boot, output_image)) {
            LOGE("Failed to write output file");
            cleanup();
            return 1;
        }

        printf("- Output file is written to\n");
        printf("- %s\n", output_image.c_str());
    }

    // Flash if requested
    if (parsed.flash && !bootdevice.empty()) {
        if (used_backup) {
            printf("- Flashing verified stock backup: %s\n", used_backup->stored_image.c_str());
        } else {
            printf("- Flashing new boot image\n");
        }
        if (!flash_boot(bootdevice, new_boot)) {
            LOGE("Failed to flash restored boot image; rolling back");
            if (!flash_boot(bootdevice, bootimage)) {
                LOGE("Rollback failed; original image: %s", bootimage.c_str());
                preserve_workdir = true;
            }
            cleanup();
            return 1;
        }
    }

    if (used_backup && (!parsed.boot_image.empty() || (parsed.flash && !bootdevice.empty())))
        boot_backup::consume(*used_backup);

    cleanup();
    printf("- Done!\n");
    return 0;
}

namespace {

std::string read_kernel_release_from_sysfs() {
    const auto release = read_file("/proc/sys/kernel/osrelease");
    return release ? std::string(trim_view(*release)) : std::string{};
}

std::string parse_kmi_from_release(const std::string& full_version) {
    // Extract major.minor (e.g. "6.6")
    const size_t dot1 = full_version.find('.');
    if (dot1 == std::string::npos)
        return "";
    size_t dot2 = full_version.find('.', dot1 + 1);
    if (dot2 == std::string::npos)
        dot2 = full_version.length();

    std::string major_minor = full_version.substr(0, dot2);

    // Try to find android version (e.g. "android15")
    const size_t android_pos = full_version.find("-android");
    if (android_pos != std::string::npos) {
        const size_t ver_start = android_pos + 8;
        size_t ver_end = full_version.find('-', ver_start);
        if (ver_end == std::string::npos)
            ver_end = full_version.length();

        const std::string android_ver = full_version.substr(ver_start, ver_end - ver_start);
        return "android" + android_ver + "-" + major_minor;
    }

    return major_minor;
}

std::string read_kernel_release() {
    const auto capabilities = sumhp::sumh::feature_capabilities();
    if (!capabilities.ok || (capabilities.bitmask & SUMH_FEATURE_KERNEL_BUILD_SPOOF)) {
        sumhp::sumh::KernelBuild original;
        if (sumhp::sumh::original_kernel_build(original))
            return original.release;
        const int original_error = errno;
        const auto unsupported = [](int error) {
            return error == ENOTTY || error == EOPNOTSUPP || error == ENOSYS || error == ENODEV;
        };
        if (capabilities.ok || !unsupported(capabilities.last_errno) ||
            !unsupported(original_error)) {
            // Query/access failures must never select a KMI using a potentially
            // spoofed release. Old/absent drivers still support stock patching.
            LOGE("Failed to get original kernel build for KMI selection");
            return "";
        }
    }
    std::string full_version = read_kernel_release_from_sysfs();
    if (!full_version.empty())
        return full_version;

    struct utsname uts{};
    if (uname(&uts) != 0) {
        LOGE("Failed to get uname");
        return "";
    }
    return uts.release;
}

}  // namespace

std::string get_current_kmi() {
    return parse_kmi_from_release(read_kernel_release());
}

int boot_info_current_kmi() {
    const std::string kmi = get_current_kmi();
    if (kmi.empty()) {
        printf("Failed to get current KMI\n");
        return 1;
    }
    printf("%s\n", kmi.c_str());
    return 0;
}

int boot_info_target_kmi(bool ota, const std::string& boot_image) {
    if (!ota && boot_image.empty())
        return boot_info_current_kmi();

    std::string target_boot = boot_image;
    if (ota) {
        const std::string slot = get_slot_suffix(true);
        if (slot.empty()) {
            printf("Failed to obtain a valid inactive slot suffix\n");
            return 1;
        }
        target_boot = "/dev/block/by-name/boot" + slot;
    }

    const auto temporary = create_boot_workdir();
    if (!temporary) {
        LOGE("Failed to create temp directory for target KMI detection");
        return 1;
    }

    const std::string workdir = temporary->string();
    const std::string magiskboot = find_magiskboot("", workdir);
    std::string kmi;
    if (!magiskboot.empty()) {
        kmi = parse_kmi_from_boot(magiskboot, workdir, target_boot);
    }

    std::error_code ec;
    fs::remove_all(workdir, ec);
    if (ec) {
        LOGW("Failed to remove target KMI directory %s: %s", workdir.c_str(), ec.message().c_str());
    }

    if (kmi.empty()) {
        printf("Failed to get target KMI\n");
        return 1;
    }
    printf("%s\n", kmi.c_str());
    return 0;
}

int boot_info_supported_kmis() {
    auto supported = list_supported_kmi();
    if (supported.empty()) {
        printf("No embedded LKMs found\n");
        return 1;
    }
    for (const auto& kmi : supported) {
        printf("%s\n", kmi.c_str());
    }
    return 0;
}

int boot_info_is_ab_device() {
    auto ab_update = getprop("ro.build.ab_update");
    const bool is_ab = ab_update && trim(*ab_update) == "true" && !get_slot_suffix(false).empty();
    printf("%s\n", is_ab ? "true" : "false");
    return 0;
}

std::string get_slot_suffix(bool ota) {
    auto suffix = getprop("ro.boot.slot_suffix");
    if (!suffix) {
        return "";
    }
    *suffix = trim(*suffix);
    if (*suffix != "_a" && *suffix != "_b")
        return "";

    if (ota) {
        // Toggle to other slot
        if (*suffix == "_a")
            return "_b";
        if (*suffix == "_b")
            return "_a";
    }

    return *suffix;
}

int boot_info_slot_suffix(bool ota) {
    const std::string suffix = get_slot_suffix(ota);
    if (ota && suffix.empty()) {
        printf("Failed to obtain a valid inactive slot suffix\n");
        return 1;
    }
    printf("%s\n", suffix.c_str());
    return 0;
}

std::string choose_boot_partition(bool ota, const std::string* override_partition,
                                  bool is_replace_kernel) {
    const std::string slot = get_slot_suffix(ota);
    if (ota && slot.empty())
        return "";

    // If specific partition is specified, use it
    if (override_partition && !override_partition->empty()) {
        // Validate partition name
        if (*override_partition == "boot" || *override_partition == "init_boot" ||
            *override_partition == "vendor_boot") {
            return "/dev/block/by-name/" + *override_partition + slot;
        }
        // Invalid partition name, fallback to auto-detect
    }

    // Check if init_boot exists
    std::string init_boot = "/dev/block/by-name/init_boot" + slot;
    struct stat st{};
    const bool init_boot_exist = (stat(init_boot.c_str(), &st) == 0);

    // Use init_boot if:
    // - Not replacing kernel (LKM mode)
    // - init_boot partition exists
    if (!is_replace_kernel && init_boot_exist) {
        return init_boot;
    }

    // Fallback to boot
    return "/dev/block/by-name/boot" + slot;
}

// Return partition name only (without path and slot suffix)
// Used by boot-info default-partition command for manager
std::string get_default_partition_name(bool is_replace_kernel) {
    const std::string slot = get_slot_suffix(false);

    // Check if init_boot exists
    const std::string init_boot = "/dev/block/by-name/init_boot" + slot;
    struct stat st{};
    const bool init_boot_exist = (stat(init_boot.c_str(), &st) == 0);

    // Use init_boot if:
    // - Not replacing kernel (LKM mode)
    // - init_boot partition exists
    if (!is_replace_kernel && init_boot_exist) {
        return "init_boot";
    }

    return "boot";
}

int boot_info_default_partition() {
    // Return partition name only, not full path.
    const std::string partition = get_default_partition_name(false);
    printf("%s\n", partition.c_str());
    return 0;
}

int boot_info_available_partitions() {
    // Return base partition names without the slot suffix.
    // Manager will add slot suffix based on user's choice
    const std::string slot = get_slot_suffix(false);

    const std::array<const char*, 3> candidates = {"boot", "init_boot", "vendor_boot"};

    for (const char* name : candidates) {
        const std::string full_path = std::string("/dev/block/by-name/") + name + slot;
        struct stat st{};
        if (stat(full_path.c_str(), &st) == 0) {
            printf("%s\n", name);
        }
    }

    return 0;
}

}  // namespace ksud
