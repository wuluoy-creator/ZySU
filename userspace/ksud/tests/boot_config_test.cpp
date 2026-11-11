#include "boot/ksu_config.hpp"
#include "uapi/imgpatch_config.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>

using ksud::KsuConfigArgs;
#define LOGE(...) ((void)0)
std::vector<uint8_t> module_bytes;
int writes;
bool read_file_bytes(const std::string&, std::vector<uint8_t>* data) {
    *data = module_bytes;
    return true;
}
bool write_file_bytes(const std::string&, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    module_bytes.assign(bytes, bytes + size);
    ++writes;
    return true;
}

#include "imgpatch_config_under_test.inc"
namespace legacy {
#include "boot_config_legacy_parser.inc"
}  // namespace legacy
namespace direct {
#include "boot_config_direct_parser.inc"
}  // namespace direct

bool ramdisk_has_config, fail_config_removal;
std::string ramdisk_config;
unsigned cleanup_count;
bool write_file(const std::string&, const std::string& content) {
    ramdisk_config = content;
    return true;
}
int magiskboot_query(const std::vector<std::string>& args) {
    assert(args.back() == "exists ksu_config");
    return ramdisk_has_config ? 0 : 1;
}
bool do_cpio_cmd(const std::string&, const std::string&, const std::string&,
                 const std::string& cmd) {
    if (cmd == "add 0644 ksu_config ksu_config") {
        ramdisk_has_config = true;
        return true;
    }
    assert(cmd == "rm ksu_config");
    if (fail_config_removal)
        return false;
    ramdisk_has_config = false;
    ramdisk_config.clear();
    return true;
}
void cleanup() {
    ++cleanup_count;
}
int apply_ramdisk_config(const KsuConfigArgs& parsed, bool bundled_lkm = false) {
    const std::string magiskboot, workdir, ramdisk;
#include "boot_config_ramdisk_under_test.inc"
    return 0;
}

ksu_imgpatch_config read_config() {
    ksu_imgpatch_config value;
    memcpy(&value, module_bytes.data(), sizeof(value));
    return value;
}
void reset_module(uint32_t version = KSU_IMGPATCH_CONFIG_VERSION) {
    ksu_imgpatch_config value{};
    value.magic = KSU_IMGPATCH_CONFIG_MAGIC;
    value.version = version;
    value.size = sizeof(value);
    module_bytes.resize(sizeof(value));
    memcpy(module_bytes.data(), &value, sizeof(value));
    writes = 0;
}

int main() {
    KsuConfigArgs config;
    assert(config.module_params().empty());
    assert(config.add_force_feature("sulog=1"));
    assert(config.add_force_feature("6=0x1"));
    assert(config.add_force_feature("2=0"));
    assert(config.force_features() == "2:0,6:1");
    for (const auto* invalid :
         {"", "sulog", "sulog=-1", "sulog=+1", "sulog=1;reboot", "999=1", "su_compat=1", "sumh=0",
          "enhanced_security=0", "kernel_umount=0", "kernel_umount=1", "1=0", "1=1",
          "selinux_hide_status=1", "sulog=18446744073709551616"})
        assert(!config.add_force_feature(invalid));
    assert(config.add_force_feature("109=18446744073709551615"));
    assert(config.add_block_modules(" mkp,vr "));
    assert(config.add_block_modules("vr,oplus_secure_guard"));
    assert(config.block_modules() == "mkp,vr,oplus_secure_guard");
    for (const auto* invalid : {"", ",mkp", "mkp,", "vr,,mkp", "a-b", "$(reboot)", "a\nb"})
        assert(!config.add_block_modules(invalid));
    assert(!config.add_block_modules(std::string(65, 'a')));
    assert(config.block_modules() == "mkp,vr,oplus_secure_guard");
    KsuConfigArgs maximum;
    const auto full = std::string(64, 'a') + ',' + std::string(64, 'b') + ',' +
                      std::string(64, 'c') + ',' + std::string(60, 'd');
    assert(maximum.add_block_modules(full));
    assert(!maximum.add_block_modules("e"));
    assert(maximum.block_modules() == full);

    const std::vector<std::string> args = {"--allow-shell",
                                           "--no-custom-rc",
                                           "--no-preinit",
                                           "--force-feature",
                                           "6=1",
                                           "--force-feature",
                                           "sulog=1",
                                           "--block-module",
                                           "mkp,vr",
                                           "--block-module",
                                           "vr"};
    const auto ramdisk = legacy::parse_boot_patch_args(args);
    const auto image = direct::parse_args(args);
    assert(ramdisk.valid && image.valid);
    assert(ramdisk.module_params() == image.module_params());
    assert(ramdisk.module_params() ==
           "allow_shell=1 norc=1 preinit=0 force_feature=2:1,6:1 block_modules=mkp,vr");
    assert(!apply_ramdisk_config(ramdisk) && ramdisk_has_config);
    assert(ramdisk_config == ramdisk.module_params());
    assert(!apply_ramdisk_config({}) && !ramdisk_has_config);
    assert(!apply_ramdisk_config({}) && !cleanup_count);
    assert(!apply_ramdisk_config(ramdisk));
    fail_config_removal = true;
    assert(apply_ramdisk_config({}) == 1 && cleanup_count == 1 && ramdisk_has_config);
    fail_config_removal = false;
    assert(!apply_ramdisk_config({}, true) && ramdisk_config == "bundled=1");
    for (const auto& invalid :
         std::vector<std::vector<std::string>>{{"--block-module"},
                                               {"--force-feature"},
                                               {"--block-module", "a;reboot"},
                                               {"--force-feature", "sumh=0"},
                                               {"--force-feature", "kernel_umount=0"},
                                               {"--force-feature", "1=0"},
                                               {"--force-feature", "sulog=-1"}}) {
        assert(!legacy::parse_boot_patch_args(invalid).valid);
        assert(!direct::parse_args(invalid).valid);
    }

    reset_module();
    assert(inject_imgpatch_config_to_lkm("module", true, true, true, image));
    auto stored = read_config();
    assert(!strcmp(stored.force_features, "2:1,6:1"));
    assert(!strcmp(stored.block_modules, "mkp,vr"));
    assert(stored.flags == KSU_IMGPATCH_CONFIG_VALID_FLAGS);
    assert(inject_imgpatch_config_to_lkm("module", false, false, false, {}));
    stored = read_config();
    assert(!stored.flags && !stored.force_features[0] && !stored.block_modules[0]);

    reset_module(3);
    assert(!inject_imgpatch_config_to_lkm("module", true, false, false, image));
    assert(writes == 0 && read_config().version == 3);
    auto old_image = image;
    old_image.no_preinit = false;
    assert(inject_imgpatch_config_to_lkm("module", true, false, false, old_image));
    assert(read_config().version == 3);

    reset_module(2);
    assert(inject_imgpatch_config_to_lkm("module", true, false, false, {}));
    assert(read_config().version == 2);
    writes = 0;
    assert(!inject_imgpatch_config_to_lkm("module", true, false, false, image));
    assert(writes == 0 && read_config().version == 2);
    reset_module();
    const auto duplicate = module_bytes;
    module_bytes.insert(module_bytes.end(), duplicate.begin(), duplicate.end());
    assert(!inject_imgpatch_config_to_lkm("module", false, false, false, image));
    assert(writes == 0);
    module_bytes.assign(512, 0);
    assert(!inject_imgpatch_config_to_lkm("module", false, false, false, image));
    assert(writes == 0);
    puts("PASS boot config parsing, ABI compatibility and explicit clearing");
}
