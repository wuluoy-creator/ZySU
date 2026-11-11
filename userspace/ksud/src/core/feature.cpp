#include "feature.hpp"
#include "../../sumhp/include/sumhp/sumh_client.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../module/module.hpp"
#include "../sulog.hpp"
#include "../terminal.hpp"
#include "../utils.hpp"
#include "ksucalls.hpp"
#include "su_path.hpp"
#include "su_transition.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace ksud {

namespace {

const std::string& get_feature_config_path() {
    static const std::string path = std::string(WORKING_DIR) + ".feature_config";
    return path;
}
constexpr uint32_t FEATURE_MAGIC = 0x7f4b5355;
constexpr uint32_t FEATURE_VERSION = 1;
constexpr const char* LEGACY_HIDE_BOOTLOADER_CONFIG = "/data/adb/ksu/.hide_bootloader";

bool sync_feature_directory() {
    const int fd = open(WORKING_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool synced = fsync(fd) == 0;
    return close(fd) == 0 && synced;
}

const std::map<std::string, uint32_t>& get_feature_map() {
    static const std::map<std::string, uint32_t> map = {
        {"su_compat", KSU_FEATURE_SU_COMPAT},
        {"enhanced_security", KSU_FEATURE_ENHANCED_SECURITY},
        {"adb_root", KSU_FEATURE_ADB_ROOT},
        {"shell_su_adb_only", KSU_FEATURE_SHELL_SU_ADB_ONLY},
        {"selinux_hide", KSU_FEATURE_SELINUX_HIDE},
        {"selinux_hide_status", KSU_FEATURE_SELINUX_HIDE_STATUS},
        {"webview_zygote_umount", KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT},
        {"default_no_new_privs", KSU_FEATURE_DEFAULT_NO_NEW_PRIVS},
        {"sulog", KSU_FEATURE_SULOG},
        {"hide_bootloader", KSU_FEATURE_HIDE_BOOTLOADER},
        {"sumh_sucompat", KSU_FEATURE_SUMH_SUCOMPAT},
        {"sumh", KSU_FEATURE_SUMH},
        {"unshare_mnt", KSU_FEATURE_UNSHARE_MNT},
        {"unicode_path_guard", KSU_FEATURE_UNICODE_PATH_GUARD},
        {"unicode_path_guard_status", KSU_FEATURE_UNICODE_PATH_GUARD_STATUS},
    };
    return map;
}

const std::map<uint32_t, const char*>& get_feature_descriptions() {
    static const std::map<uint32_t, const char*> desc = {
        {KSU_FEATURE_SU_COMPAT,
         "Classic SU Compatibility Mode - permanently disabled; SUMH is the fixed provider"},
        {KSU_FEATURE_ENHANCED_SECURITY,
         "Enhanced Security - always enabled; blocks non-KSU root elevation and unauthorized "
         "UID downgrades"},
        {KSU_FEATURE_SHELL_SU_ADB_ONLY,
         "Shell SU ADB Only - a granted shell must descend from adbd or a ZySU root process"},
        {KSU_FEATURE_ADB_ROOT,
         "ADB Root - run adbd with root privileges via kernel feature injection"},
        {KSU_FEATURE_SELINUX_HIDE,
         "SELinux Hide - requested setting; check selinux_hide_status for runtime readiness"},
        {KSU_FEATURE_SELINUX_HIDE_STATUS, "SELinux Hide runtime state (read-only): 0 disabled, 1 "
                                          "active, 2 pending reboot, 3 failed"},
        {KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT,
         "WebView Zygote Umount - unmounts modules before WebView sandbox processes inherit "
         "the zygote mount namespace"},
        {KSU_FEATURE_DEFAULT_NO_NEW_PRIVS,
         "Default No-New-Privs - always enabled; profiles using the default root profile "
         "block re-escalation (anti-escape)"},
        {KSU_FEATURE_SULOG,
         "SU Log - streams kernel sulog events to userspace and persists them to disk"},
        {KSU_FEATURE_HIDE_BOOTLOADER,
         "Hide Bootloader - rewrites bootloader and verified-boot properties after Android boot "
         "completes (off by default)"},
        {KSU_FEATURE_SUMH_SUCOMPAT,
         "SUMH su compatibility - the fixed, automatically initialized su provider; "
         "cannot be disabled or replaced by classic su compatibility"},
        {KSU_FEATURE_SUMH,
         "SUMH - built-in VFS engine, initialized automatically and always enabled"},
        {KSU_FEATURE_UNSHARE_MNT,
         "Mount View Cleanup - rebuilds the namespace after module unmounting and normalizes "
         "visible propagation IDs (off by default)"},
        {KSU_FEATURE_UNICODE_PATH_GUARD,
         "Unicode Path Guard - opt-in CVE-2025-48567 mitigation for casefold/FUSE paths; "
         "blocks ignorable-character aliases of Android, data or obb on new path lookups. "
         "Restart to clear existing handles/caches; does not replace the MediaProvider update"},
        {KSU_FEATURE_UNICODE_PATH_GUARD_STATUS,
         "Unicode Path Guard runtime state (read-only): 0 disabled, 1 active, 2 unsupported"},
    };
    return desc;
}

bool is_fixed_feature_id(uint32_t id) {
    return id == KSU_FEATURE_SUMH || id == KSU_FEATURE_SU_COMPAT ||
           id == KSU_FEATURE_SUMH_SUCOMPAT || id == KSU_FEATURE_ENHANCED_SECURITY ||
           id == KSU_FEATURE_DEFAULT_NO_NEW_PRIVS;
}

bool is_configurable_feature_id(uint32_t id) {
    if (is_fixed_feature_id(id) || id == KSU_FEATURE_SELINUX_HIDE_STATUS ||
        id == KSU_FEATURE_UNICODE_PATH_GUARD_STATUS)
        return false;
    // Removed feature IDs in old binary configs must never be restored or saved.
    const auto& known_features = get_feature_map();
    return std::any_of(known_features.begin(), known_features.end(),
                       [id](const auto& feature) { return feature.second == id; });
}

bool discard_non_configurable_features(std::map<uint32_t, uint64_t>& features) {
    const auto count = features.size();
    for (auto it = features.begin(); it != features.end();) {
        if (!is_configurable_feature_id(it->first))
            it = features.erase(it);
        else
            ++it;
    }
    return features.size() != count;
}

const char* selinux_hide_status_name(uint64_t value) {
    switch (value) {
    case KSU_SELINUX_HIDE_DISABLED:
        return "disabled";
    case KSU_SELINUX_HIDE_ACTIVE:
        return "active";
    case KSU_SELINUX_HIDE_PENDING_REBOOT:
        return "pending reboot";
    case KSU_SELINUX_HIDE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

void print_selinux_hide_runtime_status() {
    const auto [value, supported] = get_feature(KSU_FEATURE_SELINUX_HIDE_STATUS);
    printf("Runtime status: %s\n",
           supported ? selinux_hide_status_name(value) : "unavailable (kernel does not report it)");
}

const char* unicode_path_guard_status_name(uint64_t value) {
    switch (value) {
    case KSU_UNICODE_PATH_GUARD_DISABLED:
        return "disabled";
    case KSU_UNICODE_PATH_GUARD_ACTIVE:
        return "active (new casefold/FUSE path lookups)";
    case KSU_UNICODE_PATH_GUARD_UNSUPPORTED:
        return "unsupported";
    default:
        return "unknown";
    }
}

void print_unicode_path_guard_runtime_status() {
    const auto [value, supported] = get_feature(KSU_FEATURE_UNICODE_PATH_GUARD_STATUS);
    printf("Runtime status: %s\n",
           supported ? unicode_path_guard_status_name(value) : "unavailable (update the kernel)");
}

int refresh_sucompat_vfs_locked() {
    if (restore_su_path() != 0) {
        LOGE("Saved su path is unavailable; retaining the current SUMH binding and saved path");
        return -1;
    }
    if (!get_feature(KSU_FEATURE_SUMH_SUCOMPAT).second || !sumhp::sumh::is_available()) {
        LOGE("SUMH su compatibility is unavailable; check the kernel log");
        return -1;
    }
    // This is an idempotent rebind request. The kernel owns the fixed mode.
    const int ret = set_feature(KSU_FEATURE_SUMH_SUCOMPAT, 1);
    if (ret < 0) {
        LOGE("Failed to refresh SUMH su compatibility: %d", ret);
        return ret;
    }
    return 0;
}

std::pair<uint32_t, bool> parse_feature_id(const std::string& id) {
    // Try numeric first
    uint32_t num = 0;
    if (parse_uint32(id, &num)) {
        for (const auto& [name, fid] : get_feature_map()) {
            if (fid == num)
                return {num, true};
        }
        return {0, false};
    }

    // Try name lookup
    auto it = get_feature_map().find(id);
    if (it != get_feature_map().end()) {
        return {it->second, true};
    }

    return {0, false};
}

const char* feature_id_to_name(uint32_t id) {
    for (const auto& [name, fid] : get_feature_map()) {
        if (fid == id) {
            return name.c_str();
        }
    }
    return "unknown";
}

const char* feature_id_to_description(uint32_t id) {
    auto it = get_feature_descriptions().find(id);
    if (it != get_feature_descriptions().end()) {
        return it->second;
    }
    return "Unknown feature";
}

std::map<uint32_t, uint64_t> parse_text_feature_config(const std::string& content);

void ensure_forced_sulog_running() {
    if (!is_feature_forced(KSU_FEATURE_SULOG))
        return;
    const auto [value, supported] = get_feature(KSU_FEATURE_SULOG);
    if (supported && value && ensure_sulogd_running() != 0)
        LOGW("Failed to start sulogd for boot-forced sulog");
}

std::map<uint32_t, uint64_t> get_current_feature_values() {
    // Keep the user's saved value of forced features so removing the boot
    // override restores their preference. Never persist the forced runtime value.
    auto saved = load_binary_config();
    std::map<uint32_t, uint64_t> features =
        saved ? *saved : parse_text_feature_config(read_file(FEATURE_CONFIG_PATH).value_or(""));
    discard_non_configurable_features(features);
    for (const auto& [_, id] : get_feature_map()) {
        if (!is_configurable_feature_id(id))
            continue;
        if (is_feature_forced(id))
            continue;
        auto [value, supported] = get_feature(id);
        if (supported) {
            features[id] = value;
        }
    }
    return features;
}

int save_feature_config_files(const std::map<uint32_t, uint64_t>& features) {
    const std::string config_path = std::string(FEATURE_CONFIG_PATH);
    std::string text = "# KernelSU feature configuration\n";
    for (const auto& [name, id] : get_feature_map()) {
        if (!is_configurable_feature_id(id))
            continue;
        auto it = features.find(id);
        if (it == features.end())
            continue;
        text += name;
        text += '=';
        text += std::to_string(it->second);
        text += '\n';
    }
    if (!write_file_atomic(config_path, text) || !sync_feature_directory()) {
        LOGE("Failed to persist text feature configuration");
        return 1;
    }

    if (save_binary_config(features) != 0) {
        LOGE("Failed to save feature binary config");
        return 1;
    }

    LOGI("Saved feature config to %s", config_path.c_str());
    return 0;
}

bool is_sucompat_feature_id(uint32_t feature_id) {
    return feature_id == KSU_FEATURE_SU_COMPAT || feature_id == KSU_FEATURE_SUMH_SUCOMPAT;
}

int feature_save_config_locked() {
    return save_feature_config_files(get_current_feature_values());
}

int feature_set_impl(const std::string& id, uint32_t feature_id, uint64_t value) {
    if (is_feature_forced(feature_id)) {
        LOGE("Feature %s is forced by boot config", id.c_str());
        return 1;
    }
    if (feature_id == KSU_FEATURE_SELINUX_HIDE_STATUS ||
        feature_id == KSU_FEATURE_UNICODE_PATH_GUARD_STATUS) {
        LOGE("Feature %s reports runtime status and is read-only", id.c_str());
        return 1;
    }
    if (feature_id == KSU_FEATURE_ENHANCED_SECURITY ||
        feature_id == KSU_FEATURE_DEFAULT_NO_NEW_PRIVS) {
        LOGE("Feature %s is always enabled and read-only", id.c_str());
        return 1;
    }
    if (is_fixed_feature_id(feature_id)) {
        const uint64_t fixed_value = feature_id == KSU_FEATURE_SU_COMPAT ? 0 : 1;
        if (value != fixed_value) {
            LOGE("Feature %s is fixed at %" PRIu64 " and cannot be changed", id.c_str(),
                 fixed_value);
            return 1;
        }
    }
    if (value != 0 && feature_id == KSU_FEATURE_SUMH_SUCOMPAT && restore_su_path() != 0) {
        LOGE("Failed to restore su path before enabling SUMH");
        return 1;
    }
    if (value != 0 && feature_id == KSU_FEATURE_SUMH_SUCOMPAT &&
        get_feature(KSU_FEATURE_SUMH).second && !sumhp::sumh::is_available()) {
        LOGE("SUMH initialization failed; check the kernel log");
        return 1;
    }
    const int ret = set_feature(feature_id, value);
    if (ret < 0) {
        LOGE("Failed to set feature %s to %" PRIu64, id.c_str(), value);
        return 1;
    }

    if (feature_id == KSU_FEATURE_SULOG && value != 0 && ensure_sulogd_running() != 0) {
        LOGW("Failed to ensure sulogd is running after enabling sulog");
    }

    printf("Feature '%s' set to %" PRIu64 " (%s)\n", feature_id_to_name(feature_id), value,
           value != 0 ? "enabled" : "disabled");
    if (feature_id == KSU_FEATURE_SELINUX_HIDE)
        print_selinux_hide_runtime_status();
    if (feature_id == KSU_FEATURE_UNICODE_PATH_GUARD)
        print_unicode_path_guard_runtime_status();
    return 0;
}

}  // namespace

int feature_get(const std::string& id) {
    auto [feature_id, valid] = parse_feature_id(id);
    if (!valid) {
        LOGE("Unknown feature: %s", id.c_str());
        return 1;
    }

    auto [value, supported] = get_feature(feature_id);

    if (!supported) {
        printf("Feature '%s' is not supported by kernel\n", id.c_str());
        return 0;
    }

    printf("Feature: %s (%u)\n", feature_id_to_name(feature_id), feature_id);
    printf("Description: %s\n", feature_id_to_description(feature_id));
    printf("Value: %" PRIu64 "\n", value);
    if (is_feature_forced(feature_id))
        printf("[FORCED by boot config]\n");
    printf("Status: ");
    if (feature_id == KSU_FEATURE_SELINUX_HIDE_STATUS) {
        terminal::status(stdout, selinux_hide_status_name(value), value == KSU_SELINUX_HIDE_ACTIVE);
    } else if (feature_id == KSU_FEATURE_UNICODE_PATH_GUARD_STATUS) {
        terminal::status(stdout, unicode_path_guard_status_name(value),
                         value == KSU_UNICODE_PATH_GUARD_ACTIVE);
    } else {
        terminal::status(stdout, value != 0 ? "enabled" : "disabled", value != 0);
    }
    printf("\n");
    if (feature_id == KSU_FEATURE_SELINUX_HIDE)
        print_selinux_hide_runtime_status();
    if (feature_id == KSU_FEATURE_UNICODE_PATH_GUARD)
        print_unicode_path_guard_runtime_status();

    return 0;
}

int feature_set(const std::string& id, uint64_t value) {
    auto [feature_id, valid] = parse_feature_id(id);
    if (!valid) {
        LOGE("Unknown feature: %s", id.c_str());
        return 1;
    }

    if (!is_sucompat_feature_id(feature_id) && feature_id != KSU_FEATURE_SUMH) {
        return feature_set_impl(id, feature_id, value);
    }

    const SucompatTransitionLock transition;
    if (!transition.locked()) {
        return 1;
    }
    return feature_set_impl(id, feature_id, value);
}

int feature_set_and_save(const std::string& id, uint64_t value) {
    auto [feature_id, valid] = parse_feature_id(id);
    if (!valid) {
        LOGE("Unknown feature: %s", id.c_str());
        return 1;
    }

    const SucompatTransitionLock transition;
    if (!transition.locked()) {
        return 1;
    }

    const auto [previous_value, previous_supported] = get_feature(feature_id);
    const auto previous_text_config = read_file(FEATURE_CONFIG_PATH);
    const auto previous_binary_config = read_file(get_feature_config_path());
    if (feature_set_impl(id, feature_id, value) != 0) {
        return 1;
    }
    if (feature_save_config_locked() == 0) {
        return 0;
    }

    LOGW("Failed to persist feature %s; restoring previous runtime state", id.c_str());
    if (previous_supported) {
        (void)feature_set_impl(id, feature_id, previous_value);
    }
    const auto restore_file = [](const std::string& path, const auto& previous) {
        if (previous) {
            return write_file_atomic(path, *previous) && sync_feature_directory();
        }
        return unlink(path.c_str()) == 0 || errno == ENOENT;
    };
    const bool text_restored = restore_file(FEATURE_CONFIG_PATH, previous_text_config);
    const bool binary_restored = restore_file(get_feature_config_path(), previous_binary_config);
    if (!text_restored || !binary_restored) {
        LOGW("Failed to restore the previous feature configuration files");
    }
    return 1;
}

void feature_list() {
    terminal::heading(stdout, "Available Features:\n");
    printf("================================================================================\n");

    for (const auto& [name, id] : get_feature_map()) {
        auto [value, supported] = get_feature(id);

        const char* status;
        if (!supported) {
            status = "NOT_SUPPORTED";
        } else if (id == KSU_FEATURE_SELINUX_HIDE_STATUS) {
            status = selinux_hide_status_name(value);
        } else if (id == KSU_FEATURE_UNICODE_PATH_GUARD_STATUS) {
            status = unicode_path_guard_status_name(value);
        } else if (value != 0) {
            status = "ENABLED";
        } else {
            status = "DISABLED";
        }

        printf("[");
        bool active = value != 0;
        if (id == KSU_FEATURE_SELINUX_HIDE_STATUS) {
            active = value == KSU_SELINUX_HIDE_ACTIVE;
        } else if (id == KSU_FEATURE_UNICODE_PATH_GUARD_STATUS) {
            active = value == KSU_UNICODE_PATH_GUARD_ACTIVE;
        }
        terminal::status(stdout, status, supported && active);
        printf("] %s (ID=%u)%s\n", name.c_str(), id, is_feature_forced(id) ? " [FORCED]" : "");
        printf("    %s\n", feature_id_to_description(id));
    }
}

int feature_check(const std::string& id) {
    auto [feature_id, valid] = parse_feature_id(id);
    if (!valid) {
        printf("unsupported\n");
        return 1;
    }

    auto [value, supported] = get_feature(feature_id);
    if (supported) {
        if (is_feature_forced(feature_id)) {
            printf("forced\n");
            return 0;
        }
        if (is_configurable_feature_id(feature_id)) {
            for (const auto& [module, names] : get_managed_features()) {
                for (const auto& name : names) {
                    const auto [managed_id, known] = parse_feature_id(name);
                    if (known && managed_id == feature_id) {
                        printf("managed\n");
                        return 0;
                    }
                }
            }
        }
        printf("supported\n");
        return 0;
    } else {
        printf("unsupported\n");
        return 1;
    }
}

namespace {

std::map<uint32_t, uint64_t> parse_text_feature_config(const std::string& content) {
    std::map<uint32_t, uint64_t> features;
    for_each_line(content, [&](std::string_view raw_line) {
        const std::string_view line = trim_view(raw_line);
        if (line.empty() || line[0] == '#')
            return;
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            return;
        const std::string key(trim_view(line.substr(0, eq)));
        const std::string val(trim_view(line.substr(eq + 1)));
        const auto [feature_id, valid] = parse_feature_id(key);
        if (!valid)
            return;
        uint64_t value = 0;
        if (!parse_uint64(val, &value)) {
            LOGW("Invalid value for feature %s: %s", key.c_str(), val.c_str());
            return;
        }
        features[feature_id] = value;
    });
    return features;
}

}  // namespace

int feature_load_config() {
    const SucompatTransitionLock transition;
    if (!transition.locked()) {
        return 1;
    }

    ensure_forced_sulog_running();
    const std::string config_path = std::string(FEATURE_CONFIG_PATH);
    auto content = read_file(config_path);
    if (!content) {
        LOGI("No feature config file found");
        return refresh_sucompat_vfs_locked() == 0 ? 0 : 1;
    }

    auto loaded_features = parse_text_feature_config(*content);
    discard_non_configurable_features(loaded_features);
    for (const auto& [id, value] : loaded_features) {
        if (is_feature_forced(id)) {
            LOGI("Skip feature %u: forced by boot config", id);
            continue;
        }
        if (set_feature(id, value) < 0) {
            LOGW("Failed to load feature %s", feature_id_to_name(id));
        } else {
            if (id == KSU_FEATURE_SULOG && value && ensure_sulogd_running() != 0)
                LOGW("Failed to start sulogd while loading config");
            LOGI("Loaded feature %s = %" PRIu64, feature_id_to_name(id), value);
        }
    }
    if (refresh_sucompat_vfs_locked() != 0)
        return 1;

    // Rewrite both formats so entries for removed features are discarded.
    const int save_result = save_feature_config_files(loaded_features);
    if (save_result != 0) {
        LOGW("Failed to sync loaded feature configuration");
    }

    return 0;
}

int feature_save_config() {
    const SucompatTransitionLock transition;
    if (!transition.locked()) {
        return 1;
    }
    return feature_save_config_locked();
}

int refresh_sucompat_vfs() {
    const SucompatTransitionLock transition;
    if (!transition.locked()) {
        return 1;
    }

    return refresh_sucompat_vfs_locked() == 0 ? 0 : 1;
}

std::optional<std::map<uint32_t, uint64_t>> load_binary_config() {
    std::map<uint32_t, uint64_t> features;

    const auto blob = read_file(get_feature_config_path());
    if (!blob) {
        LOGI("Feature binary config not found");
        return std::nullopt;
    }

    // Fixed-layout record file: magic, version, count, then count*(u32 id, u64
    // value). Every field is bounds-checked against the blob, which the stream
    // version only did loosely via ifs.good().
    size_t offset = 0;
    const auto take = [&blob, &offset](void* out, size_t size) {
        if (offset + size > blob->size())
            return false;
        std::memcpy(out, blob->data() + offset, size);
        offset += size;
        return true;
    };

    uint32_t magic = 0;
    if (!take(&magic, sizeof(magic)) || magic != FEATURE_MAGIC) {
        LOGW("Invalid feature config magic: expected 0x%08x, got 0x%08x", FEATURE_MAGIC, magic);
        return std::nullopt;
    }

    uint32_t version = 0;
    if (!take(&version, sizeof(version))) {
        LOGW("Feature config truncated before version");
        return std::nullopt;
    }
    if (version != FEATURE_VERSION) {
        LOGW("Feature config version mismatch: expected %u, got %u", FEATURE_VERSION, version);
        return std::nullopt;
    }

    uint32_t count = 0;
    if (!take(&count, sizeof(count))) {
        LOGW("Feature config truncated before count");
        return std::nullopt;
    }

    constexpr size_t record_size = sizeof(uint32_t) + sizeof(uint64_t);
    if (count != (blob->size() - offset) / record_size ||
        (blob->size() - offset) % record_size != 0) {
        LOGW("Feature config has an invalid record count or size");
        return std::nullopt;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t id = 0;
        uint64_t value = 0;
        if (!take(&id, sizeof(id)) || !take(&value, sizeof(value))) {
            LOGW("Feature config truncated at entry %u of %u", i, count);
            return std::nullopt;
        }
        features[id] = value;
    }

    LOGI("Loaded %zu features from binary config", features.size());
    return features;
}

int save_binary_config(const std::map<uint32_t, uint64_t>& features) {
    ensure_dir_exists(WORKING_DIR);

    std::string blob;
    blob.reserve((3 * sizeof(uint32_t)) +
                 (features.size() * (sizeof(uint32_t) + sizeof(uint64_t))));
    const auto put = [&blob](const void* data, size_t size) {
        blob.append(static_cast<const char*>(data), size);
    };

    const uint32_t magic = FEATURE_MAGIC;
    put(&magic, sizeof(magic));
    const uint32_t version = FEATURE_VERSION;
    put(&version, sizeof(version));
    const uint32_t count = static_cast<uint32_t>(std::count_if(
        features.begin(), features.end(),
        [](const auto& feature) { return is_configurable_feature_id(feature.first); }));
    put(&count, sizeof(count));
    for (const auto& [id, value] : features) {
        if (!is_configurable_feature_id(id))
            continue;
        put(&id, sizeof(id));
        put(&value, sizeof(value));
    }

    if (!write_file_atomic(get_feature_config_path(), blob) || !sync_feature_directory()) {
        LOGE("Failed to persist binary feature configuration");
        return -1;
    }

    LOGI("Saved %u features to binary config", count);
    return 0;
}

namespace {

int apply_config_locked(std::map<uint32_t, uint64_t>& features) {
    LOGI("Applying feature configuration to kernel...");
    ensure_forced_sulog_running();

    discard_non_configurable_features(features);
    int applied = 0;
    for (const auto& [id, value] : features) {
        if (is_feature_forced(id)) {
            LOGI("Skip feature %u: forced by boot config", id);
            continue;
        }
        const int ret = set_feature(id, value);
        if (ret >= 0) {
            if (id == KSU_FEATURE_SULOG && value != 0 && ensure_sulogd_running() != 0) {
                LOGW("Failed to ensure sulogd is running while applying config");
            }
            LOGI("Set feature %s to %" PRIu64, feature_id_to_name(id), value);
            applied++;
        } else {
            LOGW("Failed to set feature %u: %d", id, ret);
        }
    }

    if (refresh_sucompat_vfs_locked() != 0)
        return 1;

    LOGI("Applied %d features successfully", applied);
    return 0;
}

}  // namespace

void apply_config(std::map<uint32_t, uint64_t>& features) {
    const SucompatTransitionLock transition;
    if (!transition.locked()) {
        return;
    }
    apply_config_locked(features);
}

int init_features() {
    const SucompatTransitionLock transition;
    if (!transition.locked()) {
        return 1;
    }

    LOGI("Initializing features from config...");

    auto binary_features = load_binary_config();
    bool recovered_from_text = false;
    if (!binary_features) {
        const auto text_config = read_file(FEATURE_CONFIG_PATH);
        if (text_config) {
            auto text_features = parse_text_feature_config(*text_config);
            // A text backup containing only removed features now represents an empty config.
            LOGW("Recovering feature configuration from %s", FEATURE_CONFIG_PATH);
            binary_features = std::move(text_features);
            recovered_from_text = true;
        }
    }
    if (!binary_features) {
        if (access(get_feature_config_path().c_str(), F_OK) == 0) {
            LOGE("Invalid feature config without usable text backup");
            (void)refresh_sucompat_vfs_locked();
            return 1;
        }
        LOGI("No persisted feature configuration found");
        binary_features.emplace();
    }
    auto features = std::move(*binary_features);
    const bool removed_non_configurable_features = discard_non_configurable_features(features);

    // Versions before KSU_FEATURE_HIDE_BOOTLOADER used a standalone marker.
    // Import it only when the running kernel supports the formal feature, then
    // remove it after both feature config formats have been saved successfully.
    const bool has_legacy_hide_bootloader = access(LEGACY_HIDE_BOOTLOADER_CONFIG, F_OK) == 0;
    bool consume_legacy_hide_bootloader = false;
    if (has_legacy_hide_bootloader) {
        const auto [_, supported] = get_feature(KSU_FEATURE_HIDE_BOOTLOADER);
        if (supported) {
            consume_legacy_hide_bootloader = true;
            if (features.find(KSU_FEATURE_HIDE_BOOTLOADER) == features.end()) {
                features[KSU_FEATURE_HIDE_BOOTLOADER] = 1;
                LOGI("Migrating legacy hide-bootloader marker to feature %u",
                     KSU_FEATURE_HIDE_BOOTLOADER);
            }
        } else {
            LOGW("Keeping legacy hide-bootloader marker: feature %u is unsupported",
                 KSU_FEATURE_HIDE_BOOTLOADER);
        }
    }

    // Get managed features from active modules and skip them during init
    auto managed_features_map = get_managed_features();
    if (!managed_features_map.empty()) {
        LOGI("Found %zu modules managing features", managed_features_map.size());

        // Build a set of all managed feature IDs to skip
        for (const auto& [module_id, feature_list] : managed_features_map) {
            LOGI("Module '%s' manages %zu feature(s)", module_id.c_str(), feature_list.size());
            for (const auto& feature_name : feature_list) {
                auto [feature_id, valid] = parse_feature_id(feature_name);
                if (valid) {
                    if (!is_configurable_feature_id(feature_id)) {
                        LOGI("Feature '%s' is not configurable; ignoring module feature management",
                             feature_name.c_str());
                        continue;
                    }
                    if (is_feature_forced(feature_id))
                        continue;  // Keep the user's saved preference under a boot override.
                    // Remove managed features from config, let modules control them
                    auto it = features.find(feature_id);
                    if (it != features.end()) {
                        features.erase(it);
                        LOGI("  - Skipping managed feature '%s' (controlled by module: %s)",
                             feature_name.c_str(), module_id.c_str());
                    } else {
                        LOGI("  - Feature '%s' is managed by module '%s', skipping",
                             feature_name.c_str(), module_id.c_str());
                    }
                } else {
                    LOGW("  - Unknown managed feature '%s' from module '%s', ignoring",
                         feature_name.c_str(), module_id.c_str());
                }
            }
        }
    }
    const auto requested_features = features;
    if (apply_config_locked(features) != 0)
        return 1;

    // Save the configuration (excluding managed features). A legacy migration
    // updates the human-readable config too, so a later `feature load` cannot
    // silently drop the migrated value.
    const int save_result = recovered_from_text || consume_legacy_hide_bootloader ||
                                    removed_non_configurable_features ||
                                    features != requested_features
                                ? save_feature_config_files(features)
                                : save_binary_config(features);
    if (save_result != 0) {
        LOGW("Failed to save initialized feature configuration");
        return 1;
    }
    if (consume_legacy_hide_bootloader) {
        if (unlink(LEGACY_HIDE_BOOTLOADER_CONFIG) != 0 && errno != ENOENT) {
            LOGW("Failed to remove legacy hide-bootloader marker: %s", strerror(errno));
        } else {
            LOGI("Removed legacy hide-bootloader marker");
        }
    }
    LOGI("Saved feature configuration to file");

    return 0;
}

}  // namespace ksud
