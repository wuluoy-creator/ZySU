#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

#define LOGE(...) ((void)0)
#define LOGW(...) ((void)0)

// Windows has no mkdtemp. Preserve exclusive creation and the failure contract
// for the portable path-selection tests; Linux exercises the real syscall.
#ifdef _WIN32
char* mkdtemp(char* pattern) {
    static unsigned counter = 0;
    const std::size_t length = std::strlen(pattern);
    assert(length >= 6);
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        std::snprintf(pattern + length - 6, 7, "%06u", ++counter);
        std::error_code error;
        if (fs::create_directory(pattern, error))
            return pattern;
        if (error && error != std::errc::file_exists)
            return nullptr;
    }
    return nullptr;
}
#endif

std::vector<std::uint8_t> module_bytes;
std::size_t read_count = 0;
std::size_t write_count = 0;
std::size_t hash_count = 0;
std::string hashed_key;
bool write_succeeds = true;
bool entropy_succeeds = true;

bool read_file_bytes(const std::string&, std::vector<std::uint8_t>* result) {
    ++read_count;
    *result = module_bytes;
    return true;
}

bool write_file_bytes(const std::string&, const std::uint8_t* data, std::size_t size) {
    ++write_count;
    if (!write_succeeds)
        return false;
    module_bytes.assign(data, data + size);
    return true;
}

bool fill_random(std::uint8_t* data, std::size_t size) {
    std::fill_n(data, size, 0x12);
    return entropy_succeeds;
}

std::uint64_t hash_superkey(const std::array<std::uint8_t, 16>&, const std::string& key) {
    ++hash_count;
    hashed_key = key;
    return 0x123456789abcdef0ULL;
}

#include "boot_patch_config_under_test.inc"

int configure_legacy_module(const std::string& key, std::size_t* cleanup_count,
                            bool signature_bypass = false) {
    struct {
        std::string superkey;
        bool signature_bypass = false;
    } parsed{key, signature_bypass};
    const std::string kmod_file = "module.ko";
    const auto cleanup = [cleanup_count]() { ++*cleanup_count; };
#include "boot_patch_legacy_config_under_test.inc"
    return 0;
}

int configure_direct_module(const std::string& key, std::size_t* cleanup_count,
                            bool signature_bypass = false) {
    struct {
        std::string superkey;
        bool signature_bypass = false;
    } parsed{key, signature_bypass};
    const fs::path module_for_injection = "module.ko";
    const auto cleanup = [cleanup_count]() { ++*cleanup_count; };
#include "boot_patch_direct_config_under_test.inc"
    return 0;
}

void reset_module() {
    module_bytes.assign(48, 0x56);
    std::memcpy(module_bytes.data() + 4, &SUPERKEY_MAGIC, sizeof(SUPERKEY_MAGIC));
    // The unpatched kernel uses this sentinel to select its compile-time key.
    // Every explicit image configuration, including no key, must replace it.
    const std::uint64_t unpatched_mode = ~std::uint64_t{0};
    std::memcpy(module_bytes.data() + 4 + 32, &unpatched_mode, sizeof(unpatched_mode));
    read_count = write_count = hash_count = 0;
    hashed_key.clear();
    entropy_succeeds = write_succeeds = true;
}

std::uint64_t module_field(std::size_t offset) {
    std::uint64_t field = 0;
    std::memcpy(&field, module_bytes.data() + 4 + offset, sizeof(field));
    return field;
}

void test_superkey_transport_boundaries() {
    const std::vector<std::string> accepted = {
        "",
        " \t ",
        std::string(64, 'a'),
        std::string(60, 'a') + "\xf0\x9f\x94\x91",
    };
    for (const auto& key : accepted) {
        reset_module();
        assert(inject_superkey_to_lkm("module.ko", key, false));
        assert(read_count == 1 && write_count == 1);
        assert(hash_count == (key.empty() ? 0U : 1U));
        assert(hashed_key == key);
        assert(module_field(24) == (key.empty() ? 0ULL : 0x123456789abcdef0ULL));
        assert(module_field(32) == (key.empty() ? SUPERKEY_VERIFICATION_SIGNATURE_ONLY
                                                : SUPERKEY_VERIFICATION_SIGN_AND_KEY));
    }
    std::string unicode_overflow;
    for (unsigned i = 0; i < 22; ++i)
        unicode_overflow += "\xe5\xaf\x86";  // 22 characters, 66 UTF-8 bytes.
    for (const auto& key : {std::string(65, 'a'), unicode_overflow, std::string("a\0b", 3)}) {
        reset_module();
        const auto original = module_bytes;
        assert(!inject_superkey_to_lkm("module.ko", key, false));
        assert(read_count == 0 && write_count == 0 && hash_count == 0);
        assert(module_bytes == original);
        std::size_t cleaned = 0;
        assert(configure_legacy_module(key, &cleaned) != 0);
        assert(cleaned == 1 && module_bytes == original);
    }
    reset_module();
    std::size_t cleaned = 0;
    assert(configure_legacy_module("valid", &cleaned) == 0 && cleaned == 0);
    reset_module();
    write_succeeds = false;
    assert(configure_legacy_module("valid", &cleaned) != 0 && cleaned == 1);
    reset_module();
    entropy_succeeds = false;
    assert(configure_legacy_module("valid", &cleaned) != 0 && cleaned == 2);
    assert(read_count == 0 && write_count == 0);
}

void test_superkey_requires_supported_module() {
    using ConfigureModule = int (*)(const std::string&, std::size_t*, bool);
    const std::array<ConfigureModule, 2> configure_paths = {
        configure_legacy_module,
        configure_direct_module,
    };
    for (const auto configure : configure_paths) {
        for (const bool signature_bypass : {false, true}) {
            for (const bool truncated : {false, true}) {
                reset_module();
                if (truncated)
                    module_bytes.resize(4 + 39);  // Marker present, incomplete data block.
                else
                    std::fill(module_bytes.begin(), module_bytes.end(), 0x56);
                const auto original = module_bytes;
                std::size_t cleaned = 0;
                assert(configure("valid", &cleaned, signature_bypass) != 0);
                assert(cleaned == 1 && read_count == 1 && write_count == 0);
                assert(module_bytes == original);

                // Modules without SuperKey remain usable when no key is requested;
                // --signature-bypass without a key retains its ignored behavior.
                cleaned = 0;
                assert(configure("", &cleaned, signature_bypass) == 0);
                assert(cleaned == 0 && write_count == 0 && module_bytes == original);
            }
        }
    }
}

void set_tmpdir(const std::string& value) {
#ifdef _WIN32
    assert(_putenv_s("TMPDIR", value.c_str()) == 0);
#else
    assert(setenv("TMPDIR", value.c_str(), 1) == 0);
#endif
}

fs::path observed_workdir;
std::string observed_target;
bool tool_available = true;
bool kmi_available = true;
std::string target_slot = "_b";

int boot_info_current_kmi() {
    return 0;
}
std::string get_slot_suffix(bool ota) {
    assert(ota);
    return target_slot;
}
std::string find_magiskboot() {
    return tool_available ? "magiskboot" : "";
}
std::string parse_kmi_from_boot(const std::string&, const std::string& workdir,
                                const std::string& target) {
    observed_workdir = workdir;
    assert(observed_workdir.is_absolute() && fs::is_directory(observed_workdir));
    observed_target = target;
    std::ofstream(fs::path(workdir) / "unpacked-kernel").put('x');
    return kmi_available ? "android15-6.6" : "";
}

#include "boot_patch_target_kmi_under_test.inc"

void test_target_kmi_temporary_directories(const fs::path& base) {
    const auto old_cwd = fs::current_path();
    const char* old_env = std::getenv("TMPDIR");
    const std::string old_tmpdir = old_env == nullptr ? "" : old_env;
    const fs::path cwd = base / "writable-cwd";
    const fs::path cache = base / "application-cache-directory-longer-than-32-characters";
    fs::create_directory(cwd);
    fs::create_directory(cache);
    fs::current_path(cwd);
    set_tmpdir(cache.string());
    const auto first = create_boot_workdir();
    const auto second = create_boot_workdir();
    assert(first && second && *first != *second);
    assert(first->is_absolute() && first->parent_path() == cache);
#ifndef _WIN32
    struct stat status{};
    assert(stat(first->c_str(), &status) == 0 && (status.st_mode & 0777) == 0700);
#endif
    fs::remove(*first);
    fs::remove(*second);
    assert(boot_info_target_kmi(false, "/selected/boot.img") == 0);
    assert(observed_workdir.parent_path() == cache);
    assert(observed_target == "/selected/boot.img" && !fs::exists(observed_workdir));
    // A configured but unusable path must fall through to a writable cwd.
    const fs::path non_directory = base / "not-a-directory";
    std::ofstream(non_directory).put('x');
    set_tmpdir(non_directory.string());
    assert(boot_info_target_kmi(false, "/selected/boot.img") == 0);
    assert(observed_workdir.parent_path() == cwd && !fs::exists(observed_workdir));
    set_tmpdir("");
    assert(boot_info_target_kmi(true, "") == 0);
    assert(observed_target == "/dev/block/by-name/boot_b");
    assert(observed_workdir.parent_path() == cwd && !fs::exists(observed_workdir));
    kmi_available = false;
    assert(boot_info_target_kmi(false, "/selected/boot.img") != 0);
    assert(!fs::exists(observed_workdir));
    kmi_available = true;
    tool_available = false;
    assert(boot_info_target_kmi(false, "/selected/boot.img") != 0);
    assert(fs::is_empty(cwd));
    tool_available = true;
    fs::current_path(old_cwd);
    set_tmpdir(old_tmpdir);
}

std::map<std::string, std::string> cpio_entries;
std::size_t cpio_mutations = 0;
bool magisk_patched = false;
bool cpio_succeeds = true;

bool cpio_entry_exists(const std::string&, const fs::path&, const fs::path&, const char* entry) {
    return cpio_entries.count(entry) != 0;
}
bool ramdisk_is_magisk_patched(const std::string&, const fs::path&, const fs::path&) {
    return magisk_patched;
}
bool run_cpio_command(const std::string&, const fs::path&, const fs::path&,
                      const std::string& cmd) {
    ++cpio_mutations;
    if (!cpio_succeeds)
        return false;
    if (cmd.rfind("rm ", 0) == 0)
        return cpio_entries.erase(cmd.substr(3)) == 1;
    if (cmd == "mv init.real init") {
        if (cpio_entries.count("init") != 0 || cpio_entries.count("init.real") == 0)
            return false;
        cpio_entries["init"] = cpio_entries.at("init.real");
        cpio_entries.erase("init.real");
        return true;
    }
    return false;
}
struct CommandResult {
    int exit_code = 0;
    std::string stdout_str;
};
CommandResult exec_command_magiskboot(const std::string&, const std::vector<std::string>& args,
                                      const std::string&) {
    assert(args.size() == 3 && args[0] == "cpio" && args[2] == "ls -r");
    CommandResult result;
    for (const auto& entry : cpio_entries)
        result.stdout_str += "100644\t" + entry.first + "\n";
    return result;
}

#include "boot_patch_cleanup_under_test.inc"

void test_ramdisk_ownership_and_conversion(const fs::path& base) {
    const fs::path workdir = base / "cleanup";
    fs::create_directory(workdir);
    const fs::path ramdisk = workdir / "ramdisk.cpio";
    std::ofstream(ramdisk).put('x');
    cpio_entries = {{"init", "stock init"},
                    {"adb_debug.prop", "stock debug props"},
                    {"force_debuggable", ""},
                    {"init.real", "unrelated init backup"}};
    const auto stock_entries = cpio_entries;
    auto changed = clean_legacy_ramdisks("", workdir);
    assert(changed && !*changed && cpio_entries == stock_entries);
    assert(cpio_mutations == 0 && fs::exists(ramdisk));
    // Genuine legacy ramdisk: restore the original init before deleting KSU.
    cpio_entries = {{"init", "ksuinit"},
                    {"init.real", "stock init"},
                    {"kernelsu.ko", "module"},
                    {"ksu_config", "bundled=1"},
                    {"adb_debug.prop", "ksu props"},
                    {"force_debuggable", ""},
                    {"stock_image.sha1", "backup digest"},
                    {"unrelated.rc", "keep"}};
    changed = clean_legacy_ramdisks("", workdir);
    assert(changed && *changed);
    assert(cpio_entries ==
           (std::map<std::string, std::string>{{"init", "stock init"}, {"unrelated.rc", "keep"}}));
    // Genuine synthetic ramdisk for a formerly ramdisk-free boot is removed.
    cpio_entries = {{"init", "ksuinit"}, {"kernelsu.ko", "module"}, {".backup", ""}};
    changed = clean_legacy_ramdisks("", workdir);
    assert(changed && *changed && !fs::exists(ramdisk));
    std::ofstream(ramdisk).put('x');
    cpio_entries["unrelated.rc"] = "must not lose";
    const auto ambiguous_entries = cpio_entries;
    assert(!clean_legacy_ramdisks("", workdir));
    assert(cpio_entries == ambiguous_entries && fs::exists(ramdisk));
    magisk_patched = true;
    assert(!clean_legacy_ramdisks("", workdir));
    assert(cpio_entries == ambiguous_entries);
    magisk_patched = false;
    cpio_entries = {{"init", "ksuinit"}, {"init.real", "stock init"}, {"kernelsu.ko", "module"}};
    cpio_succeeds = false;
    assert(!clean_legacy_ramdisks("", workdir));
    assert(cpio_entries.at("init") == "ksuinit" && cpio_entries.count("init.real") == 1);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const fs::path base = fs::absolute(argv[1]);
    test_superkey_transport_boundaries();
    test_superkey_requires_supported_module();
    test_target_kmi_temporary_directories(base);
    test_ramdisk_ownership_and_conversion(base);
    std::cout << "boot_patch_flow_test: all tests passed\n";
}
