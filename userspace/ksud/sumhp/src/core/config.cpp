#include "sumhp/config.hpp"

#include "core/checked_document.hpp"
#include "core/config_document.hpp"
#include "core/json.hpp"
#include "core/log.hpp"
#include "core/partition_config.hpp"
#include "core/rule_inventory.hpp"
#include "core/runtime.hpp"
#include "core/state_ledger.hpp"
#include "core/user_hide_store.hpp"
#include "durable_file.hpp"
#include "mount/partition_discovery.hpp"
#include "uapi/sumh.h"
#include "utils.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace sumhp {

class ConfigFileLock {
public:
    ConfigFileLock() = default;
    ConfigFileLock(const ConfigFileLock&) = delete;
    ConfigFileLock& operator=(const ConfigFileLock&) = delete;
    ConfigFileLock(ConfigFileLock&&) = delete;
    ConfigFileLock& operator=(ConfigFileLock&&) = delete;
    ~ConfigFileLock() {
        if (fd_ >= 0) {
            (void)::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
    }

    bool acquire(std::string& error, bool nonblocking = false) {
        if (!prepare_private_directory(runtime_data_dir(), error)) {
            return false;
        }
        const std::string lock_path = runtime_config_file().string() + ".lock";
        fd_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd_ < 0) {
            error = "open " + lock_path + ": " + std::strerror(errno);
            return false;
        }
        struct stat metadata{};
        if (::fstat(fd_, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 ||
            metadata.st_uid != 0 || (metadata.st_mode & 0077) != 0) {
            error = "invalid private config lock";
            errno = EACCES;
            return false;
        }
        if (::flock(fd_, LOCK_EX | (nonblocking ? LOCK_NB : 0)) != 0) {
            error = "lock " + lock_path + ": " + std::strerror(errno);
            return false;
        }
        return true;
    }

private:
    int fd_ = -1;
};

std::string default_config_json() {
    return R"({
  "schema_version": 1,
  "mountsource": "KSU",
  "work_dir": "/dev/sumhp",
  "mirror_dir": "",
  "mirror_img_size_mb": 2048,
  "overlay_writable": false,
  "debug": false,
  "verbose": false,
  "fs_type": "auto",
  "enable_kernel_debug": false,
  "enable_stealth": true,
  "enable_overlay_xattr_hide": false,
  "enable_mount_hide": false,
  "mount_hide_mode": "normal",
  "enable_maps_spoof": false,
  "maps_rules": [],
  "enable_statfs_spoof": false,
  "enable_kernel_build_spoof": false,
  "kernel_build_release": "",
  "kernel_build_version": "",
  "kernel_build_apply_stage": "post-fs-data",
  "overlayfs_enabled": true,
  "magic_mount_enabled": true,
  "mount_backend": "auto",
  "partitions": []
}
)";
}

bool parse_config_document(const std::string& input, ConfigDocument& document, std::string& error) {
    error.clear();
    ConfigDocument parsed;
    if (!parse_json(input, parsed.fields, error))
        return false;
    if (!parsed.fields.is_object()) {
        error = "config root must be a JSON object";
        return false;
    }
    if (const auto* version = parsed.fields.find("schema_version")) {
        if (!version->is_number() || !std::isfinite(version->n) || version->n < 1 ||
            std::floor(version->n) != version->n) {
            error = "configuration field 'schema_version' must be a positive integer";
            return false;
        }
        if (version->n > CONFIG_SCHEMA_VERSION) {
            error = "unsupported config schema_version; use a compatible controller or "
                    "an explicit downgrade conversion";
            return false;
        }
        parsed.schema_version = static_cast<int>(version->n);
    }
    if (parsed.schema_version == 0) {
        // Older CLI versions accepted these empty values as defaults. Normalize
        // only unversioned documents; explicit schema-1 files and patches keep
        // the strict contract. The writer backs up the untouched input bytes.
        for (const auto* key : {"fs_type", "mount_backend", "mount_hide_mode"}) {
            const auto* value = parsed.fields.find(key);
            if (value && value->is_string() && value->s.empty())
                parsed.fields.o[key] = std::string(key) == "mount_hide_mode" ? "normal" : "auto";
        }
    }
    document = std::move(parsed);
    return true;
}

bool config_document_is_writable(const ConfigDocument& document, std::string& error) {
    static const auto defaults = json::parse(default_config_json());
    for (const auto& [key, value] : document.fields.o) {
        (void)value;
        if (!defaults.find(key)) {
            error = "configuration field '" + key +
                    "' is unknown; refusing to rewrite stored configuration";
            return false;
        }
    }
    return true;
}

json::Value maps_path_rules_json(const std::vector<MapsPathRule>& rules) {
    JsonValue result;
    result.type = JsonValue::Type::Array;
    for (const auto& rule : rules) {
        JsonValue item = JsonValue::object();
        item.o["target_path"] = rule.target_path;
        item.o["replacement_path"] = rule.replacement_path;
        result.a.push_back(std::move(item));
    }
    return result;
}

bool validate_maps_path_rules(const std::vector<MapsPathRule>& rules, std::string& error) {
    if (rules.size() > SUMH_MANAGED_MAPS_MAX_RULES) {
        error =
            "maps_rules supports at most " + std::to_string(SUMH_MANAGED_MAPS_MAX_RULES) + " rules";
        return false;
    }
    std::set<std::string> targets;
    std::size_t android_slash_escaping = 0;
    for (std::size_t index = 0; index < rules.size(); ++index) {
        const auto& rule = rules[index];
        const auto valid_path = [](const std::string& path, std::size_t limit) {
            return !path.empty() && path.front() == '/' && path.size() < limit &&
                   path.find('\0') == std::string::npos &&
                   path.find_first_of("\r\n") == std::string::npos;
        };
        if (!valid_path(rule.target_path, SUMH_USER_HIDE_PATH_MAX) ||
            !valid_path(rule.replacement_path, SUMH_MAX_LEN_PATHNAME)) {
            error = "invalid maps rule paths at index " + std::to_string(index) +
                    ": absolute, NUL/CR/LF-free paths required; target < 4096 bytes, "
                    "replacement < 256 bytes";
            return false;
        }
        if (!targets.insert(rule.target_path).second) {
            error = "duplicate maps target path at index " + std::to_string(index);
            return false;
        }
        // Android JSONObject escapes '/' on both serialization passes: each
        // slash uses four bytes rather than our JSON writer's single byte.
        android_slash_escaping +=
            3 * static_cast<std::size_t>(
                    std::count(rule.target_path.begin(), rule.target_path.end(), '/') +
                    std::count(rule.replacement_path.begin(), rule.replacement_path.end(), '/'));
    }
    if (json_quote(stringify_json(maps_path_rules_json(rules))).size() + android_slash_escaping >
        MAPS_RULES_MAX_ENCODED_BYTES) {
        error = "maps_rules exceeds the 56 KiB encoded configuration budget";
        return false;
    }
    return true;
}

namespace {
bool parse_maps_path_rules(const JsonValue& value, std::vector<MapsPathRule>& rules,
                           std::string& error) {
    if (!value.is_array()) {
        error = "configuration field 'maps_rules' must be an array";
        return false;
    }
    std::vector<MapsPathRule> parsed;
    for (const auto& item : value.a) {
        const auto* target = item.find("target_path");
        const auto* replacement = item.find("replacement_path");
        if (!item.is_object() || item.o.size() != 2 || !target || !replacement ||
            !target->is_string() || !replacement->is_string()) {
            error = "configuration field 'maps_rules' must contain only "
                    "target_path/replacement_path string pairs";
            return false;
        }
        parsed.push_back({target->s, replacement->s});
    }
    if (!validate_maps_path_rules(parsed, error))
        return false;
    rules = std::move(parsed);
    return true;
}
}  // namespace

bool validate_config_patch(const json::Value& patch, std::string& error) {
    error.clear();
    if (!patch.is_object()) {
        error = "config updates must be a JSON object";
        return false;
    }
    static const auto defaults = json::parse(default_config_json());
    const auto reject = [&error](const std::string& key, const char* reason) {
        error = "configuration field '" + key + "' " + reason;
        return false;
    };
    for (const auto& [key, value] : patch.o) {
        if (key == "schema_version")
            return reject(key, "is managed by the controller and cannot be patched");
        const auto* expected = defaults.find(key);
        if (!expected)
            return reject(key, "is unknown");
        if (value.type != expected->type) {
            const char* type = "must be a positive integer";
            if (expected->is_bool())
                type = "must be a boolean";
            else if (expected->is_string())
                type = "must be a string";
            else if (expected->is_array())
                type = "must be an array";
            return reject(key, type);
        }
        if (value.is_string() && value.s.find('\0') != std::string::npos)
            return reject(key, "must not contain NUL");
        if (key == "kernel_build_release" || key == "kernel_build_version") {
            if (value.s.size() >= SUMH_KERNEL_BUILD_MAX)
                return reject(key, "must be at most 64 UTF-8 bytes");
            if (std::any_of(value.s.begin(), value.s.end(),
                            [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; }))
                return reject(key, "must not contain control characters");
            if (key == "kernel_build_release" && value.s.find(' ') != std::string::npos)
                return reject(key, "must not contain whitespace");
        }
        if (key == "kernel_build_apply_stage" && value.s != "post-fs-data" &&
            value.s != "boot-completed")
            return reject(key, "must be post-fs-data or boot-completed");
        if (key == "mirror_img_size_mb" && (!std::isfinite(value.n) || value.n < 1 ||
                                            value.n > INT_MAX || std::floor(value.n) != value.n))
            return reject(key, "must be an integer between 1 and 2147483647");
        if ((key == "work_dir" && (value.s.empty() || value.s.front() != '/')) ||
            (key == "mirror_dir" && !value.s.empty() && value.s.front() != '/'))
            return reject(key, "must be an absolute path");
        if ((key == "work_dir" || key == "mirror_dir") && value.s.size() >= 4096)
            return reject(key, "must be shorter than 4096 UTF-8 bytes");
        if (key == "mount_hide_mode" && value.s != "normal" && value.s != "aggressive")
            return reject(key, "must be normal or aggressive");
        if (key == "fs_type" && value.s != "auto" && value.s != "tmpfs" && value.s != "ext4" &&
            value.s != "erofs")
            return reject(key, "must be auto, tmpfs, ext4 or erofs");
        if (key == "mount_backend" && value.s != "auto" && value.s != "sumh" &&
            value.s != "overlay" && value.s != "magic" && value.s != "none")
            return reject(key, "must be auto, sumh, overlay, magic or none");
        if (key == "partitions") {
            for (const auto& partition : value.a) {
                if (!partition.is_string() || partition.s.empty() || partition.s.size() > 255 ||
                    !std::all_of(partition.s.begin(), partition.s.end(), [](unsigned char c) {
                        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9') || c == '_' || c == '-';
                    }))
                    return reject(key, "must contain partition names using letters, digits, '_' "
                                       "or '-' (at most 255 bytes)");
            }
        }
        if (key == "maps_rules") {
            std::vector<MapsPathRule> rules;
            if (!parse_maps_path_rules(value, rules, error))
                return false;
        }
    }
    return true;
}

namespace {
bool save_config(const std::string& data, std::string& error) {
    if (ksud::write_file_durable(runtime_config_file(), data, error))
        return true;
    error = "write config: " + error;
    return false;
}
}  // namespace

bool save_config_document(ConfigDocument& document, const std::optional<std::string>& original,
                          std::string& error) {
    // Parsing/read-only queries never migrate. Keep the exact original bytes
    // before the first explicit write that introduces the version marker.
    if (document.schema_version == 0 && original &&
        !ksud::write_file_durable(runtime_config_file().string() + ".pre-schema-1.bak", *original,
                                  error)) {
        error = "backup config before schema migration: " + error;
        return false;
    }
    document.fields.o["schema_version"] = CONFIG_SCHEMA_VERSION;
    return save_config(stringify_json(document.fields, 2) + "\n", error);
}

bool write_default_config(std::string& error) {
    ConfigFileLock lock;
    if (!lock.acquire(error))
        return false;
    errno = 0;
    const auto input = ksud::read_file(runtime_config_file().string());
    if (!input && errno != ENOENT) {
        error = "read config: " + std::string(std::strerror(errno ? errno : EIO));
        return false;
    }
    ConfigDocument document;
    if (!parse_config_document(input.value_or(default_config_json()), document, error) ||
        !config_document_is_writable(document, error))
        return false;
    document.fields = json::parse(default_config_json());
    return save_config_document(document, input, error);
}

namespace {
std::vector<std::string> json_string_array_or_empty(const JsonValue* value) {
    std::vector<std::string> out;
    if (!value || !value->is_array()) {
        return out;
    }
    for (const auto& item : value->a) {
        if (item.is_string()) {
            out.push_back(item.s);
        }
    }
    return out;
}
}  // namespace

bool load_user_hide_rules(std::vector<std::string>& rules, std::string& error) {
    hide::Stored source;
    if (!hide::read_store(source, error))
        return false;
    rules = hide::enabled_paths(source.document);
    return true;
}

std::vector<std::string> load_user_hide_rules() {
    std::vector<std::string> rules;
    std::string error;
    if (!load_user_hide_rules(rules, error))
        logging::write(logging::Level::Warning, "config", error);
    return rules;
}

bool save_user_hide_rules(const std::vector<std::string>& rules, std::string& error) {
    // This compatibility API has no way to distinguish disabling from deleting
    // a rich definition. New controller writers use the full transaction model.
    hide::Transaction transaction;
    if (!transaction.begin(error))
        return false;
    auto values = JsonValue::array();
    for (const auto& path : rules)
        values.a.emplace_back(path);
    hide::Document candidate;
    if (!hide::parse_document(stringify_json(values), candidate, error))
        return false;
    const auto& source = transaction.stored();
    if (!source.missing && source.document.source_schema == 1) {
        auto before = hide::enabled_paths(source.document);
        auto after = hide::enabled_paths(candidate);
        std::sort(before.begin(), before.end());
        std::sort(after.begin(), after.end());
        if (before != after) {
            error = "structured user hide definitions require definition-aware edits";
            errno = EOPNOTSUPP;
            return false;
        }
        candidate = source.document;
    }
    const auto result = transaction.save(candidate, error);
    return result == hide::SaveResult::Saved || result == hide::SaveResult::Unchanged;
}

bool save_user_hide_rules(const std::vector<std::string>& rules) {
    std::string error;
    const bool saved = save_user_hide_rules(rules, error);
    if (!saved)
        logging::write(logging::Level::Error, "config", error);
    return saved;
}

namespace {
bool json_bool_or(const JsonValue* root, const char* key, bool fallback) {
    const auto* value = root ? root->find(key) : nullptr;
    return value ? value->bool_or(fallback) : fallback;
}

std::string json_string_or(const JsonValue* root, const char* key, const std::string& fallback) {
    const auto* value = root ? root->find(key) : nullptr;
    return value ? value->string_or(fallback) : fallback;
}

int json_int_or(const JsonValue* root, const char* key, int fallback) {
    const auto* value = root ? root->find(key) : nullptr;
    return value ? static_cast<int>(value->u32_or(static_cast<std::uint32_t>(fallback))) : fallback;
}
}  // namespace

bool parse_config_json(const std::string& json, Config& config, std::string& error) {
    ConfigDocument document;
    if (!parse_config_document(json, document, error))
        return false;
    const auto& root = document.fields;
    // Every known field uses the same contract as a patch. Unknown fields are
    // left in the document for inspection and make the document non-writable.
    static const auto defaults = json::parse(default_config_json());
    JsonValue fields = JsonValue::object();
    for (const auto& [key, value] : root.o) {
        if (key != "schema_version" && defaults.find(key))
            fields.o[key] = value;
    }
    if (!validate_config_patch(fields, error))
        return false;
    Config parsed;
    const bool build_enabled =
        json_bool_or(&root, "enable_kernel_build_spoof", parsed.enable_kernel_build_spoof);
    const auto build_release =
        json_string_or(&root, "kernel_build_release", parsed.kernel_build_release);
    const auto build_version =
        json_string_or(&root, "kernel_build_version", parsed.kernel_build_version);
    if (build_enabled && build_release.empty() && build_version.empty()) {
        error = "kernel build spoof requires a custom kernel_build_release or kernel_build_version";
        return false;
    }
    std::vector<MapsPathRule> maps_rules;
    if (const auto* value = root.find("maps_rules")) {
        if (!parse_maps_path_rules(*value, maps_rules, error))
            return false;
    }

    parsed.mount_source = json_string_or(&root, "mountsource", parsed.mount_source);
    parsed.work_dir = json_string_or(&root, "work_dir", parsed.work_dir);
    parsed.mirror_dir = json_string_or(&root, "mirror_dir", parsed.mirror_dir);
    parsed.mirror_img_size_mb = json_int_or(&root, "mirror_img_size_mb", parsed.mirror_img_size_mb);
    parsed.overlay_writable = json_bool_or(&root, "overlay_writable", parsed.overlay_writable);
    parsed.fs_type = json_string_or(&root, "fs_type", parsed.fs_type);
    parsed.debug = json_bool_or(&root, "debug", parsed.debug);
    parsed.verbose = json_bool_or(&root, "verbose", parsed.verbose);
    parsed.enable_kernel_debug =
        json_bool_or(&root, "enable_kernel_debug", parsed.enable_kernel_debug);
    parsed.enable_stealth = json_bool_or(&root, "enable_stealth", parsed.enable_stealth);
    parsed.enable_overlay_xattr_hide =
        json_bool_or(&root, "enable_overlay_xattr_hide", parsed.enable_overlay_xattr_hide);
    parsed.enable_mount_hide = json_bool_or(&root, "enable_mount_hide", parsed.enable_mount_hide);
    parsed.mount_hide_mode = json_string_or(&root, "mount_hide_mode", parsed.mount_hide_mode);
    parsed.enable_maps_spoof = json_bool_or(&root, "enable_maps_spoof", parsed.enable_maps_spoof);
    parsed.maps_rules = std::move(maps_rules);
    parsed.enable_statfs_spoof =
        json_bool_or(&root, "enable_statfs_spoof", parsed.enable_statfs_spoof);
    parsed.enable_kernel_build_spoof = build_enabled;
    parsed.kernel_build_release = build_release;
    parsed.kernel_build_version = build_version;
    parsed.kernel_build_apply_stage =
        json_string_or(&root, "kernel_build_apply_stage", parsed.kernel_build_apply_stage);
    parsed.overlayfs_enabled = json_bool_or(&root, "overlayfs_enabled", parsed.overlayfs_enabled);
    parsed.magic_mount_enabled =
        json_bool_or(&root, "magic_mount_enabled", parsed.magic_mount_enabled);
    parsed.mount_backend = json_string_or(&root, "mount_backend", parsed.mount_backend);
    parsed.partitions = json_string_array_or_empty(root.find("partitions"));
    config = std::move(parsed);

    return true;
}

bool read_config_file(Config& config, std::string& error) {
    errno = 0;
    const auto input = ksud::read_file(runtime_config_file().string());
    if (!input) {
        error = "read config: " + std::string(std::strerror(errno ? errno : EIO));
        return false;
    }
    if (!parse_config_json(*input, config, error))
        return false;
    logging::set_debug_enabled(config.debug || config.verbose);
    return true;
}

bool merge_config_json(const std::string& updates, std::string& error) {
    error.clear();
    JsonValue patch;
    if (!parse_json(updates, patch, error) || !patch.is_object()) {
        if (error.empty()) {
            error = "config updates must be a JSON object";
        }
        return false;
    }

    if (!validate_config_patch(patch, error))
        return false;
    ConfigFileLock lock;
    if (!lock.acquire(error)) {
        return false;
    }

    errno = 0;
    const auto input = ksud::read_file(runtime_config_file().string());
    if (!input && errno != ENOENT) {
        error = "read config: " + std::string(std::strerror(errno));
        return false;
    }
    ConfigDocument document;
    if (!parse_config_document(input.value_or(default_config_json()), document, error) ||
        !config_document_is_writable(document, error)) {
        return false;
    }
    auto& root = document.fields;
    for (const auto& [key, value] : patch.o) {
        root.o[key] = value;
    }
    Config validated;
    if (!parse_config_json(stringify_json(root), validated, error))
        return false;
    const bool saved = save_config_document(document, input, error);
    if (saved)
        logging::set_debug_enabled(json_bool_or(&root, "debug", false) ||
                                   json_bool_or(&root, "verbose", false));
    return saved;
}

bool valid_config_revision(const std::string& revision) {
    return revision.size() == 74 && revision.compare(0, 10, "sha256-v1:") == 0 &&
           std::all_of(revision.begin() + 10, revision.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool parse_partition_config(const std::string& bytes, Config& config, std::string& error) {
    JsonValue strict;
    ConfigDocument document;
    return bytes.size() <= checked::document_limit && inventory::valid_utf8(bytes) &&
           json::Parser(bytes, true).parse(strict, error) &&
           parse_config_document(bytes, document, error) &&
           config_document_is_writable(document, error) && parse_config_json(bytes, config, error);
}

JsonValue save_partitions_conditional(const std::string& revision, const std::string& array_json) {
    auto out = JsonValue::object();
    out.o["schema_version"] = 1;
    out.o["kind"] = "sumh.partitions.save";
    out.o["read_only"] = false;
    out.o["saved"] = false;
    out.o["applied"] = false;
    out.o["requires_reboot"] = true;
    out.o["readback_confirmed"] = false;
    out.o["durability_confirmed"] = false;
    out.o["config_revision"] = JsonValue{};
    out.o["previous_revision"] = JsonValue{};
    out.o["saved_partitions"] = JsonValue::array();
    const auto finish = [&](const char* status, const char* reason, const std::string& detail,
                            int code) {
        out.o["status"] = status;
        out.o["reason"] = reason;
        out.o["detail"] = json::parse(inventory::bytes_json(detail));
        out.o["errno"] = code;
        return out;
    };
    JsonValue names;
    std::string error;
    if (!valid_config_revision(revision) || array_json.size() > PARTITIONS_INPUT_BYTES ||
        !inventory::valid_utf8(array_json) || !json::Parser(array_json, true).parse(names, error) ||
        !names.is_array() || names.a.size() > PARTITIONS_MAX_NAMES)
        return finish("rejected", "invalid_input", error, EINVAL);
    auto patch = JsonValue::object();
    patch.o["partitions"] = names;
    if (!validate_config_patch(patch, error))
        return finish("rejected", "invalid_partitions", error, EINVAL);
    std::set<std::string> requested;
    for (const auto& name : names.a)
        if (!requested.insert(name.s).second)
            return finish("rejected", "duplicate_partition", name.s, EINVAL);
    ConfigFileLock lock;
    if (!lock.acquire(error, true))
        return finish("failed", "config_lock_unavailable", error, errno ? errno : EIO);
    const auto original = checked::read_document(runtime_config_file());
    if (original.status != "valid")
        return finish("rejected", "config_unavailable", original.status, original.error);
    const auto previous = state::revision_for_bytes(original.bytes);
    out.o["previous_revision"] = previous.empty() ? JsonValue{} : JsonValue(previous);
    if (previous.empty())
        return finish("failed", "revision_unavailable", {}, EIO);
    if (previous != revision)
        return finish("conflict", "config_revision_changed", {}, ESTALE);
    Config current;
    ConfigDocument document;
    if (!parse_partition_config(original.bytes, current, error) ||
        !parse_config_document(original.bytes, document, error))
        return finish("rejected", "config_not_writable", error, EINVAL);
    const std::set<std::string> existing(current.partitions.begin(), current.partitions.end());
    const bool additions = std::any_of(requested.begin(), requested.end(),
                                       [&](const auto& name) { return !existing.count(name); });
    if (additions) {
        mount::PartitionDiscovery scan;
        if (!mount::discover_extra_partitions(current.partitions, scan, error) || !scan.complete)
            return finish("rejected", "discovery_incomplete", error, EIO);
        std::set<std::string> eligible;
        const auto* candidates = scan.output.find("candidates");
        if (candidates && candidates->is_array())
            for (const auto& candidate : candidates->a) {
                const auto* name = candidate.find("name");
                const auto* applicable = candidate.find("applicable");
                if (name && name->is_string() && applicable && applicable->is_bool() &&
                    applicable->b)
                    eligible.insert(name->s);
            }
        for (const auto& name : requested)
            if (!existing.count(name) && !eligible.count(name))
                return finish("rejected", "partition_not_eligible", name, EINVAL);
    }
    // Recheck after potentially expensive discovery, including non-cooperating writers.
    if (!(original == checked::read_document(runtime_config_file())))
        return finish("conflict", "config_changed_during_preflight", {}, ESTALE);
    const auto record_readback = [&](const checked::Document& readback) {
        Config observed;
        std::string read_error;
        if (readback.status != "valid" ||
            !parse_partition_config(readback.bytes, observed, read_error))
            return false;
        const auto hash = state::revision_for_bytes(readback.bytes);
        if (hash.empty())
            return false;
        out.o["config_revision"] = hash;
        auto parts = JsonValue::array();
        for (const auto& name : observed.partitions)
            parts.a.emplace_back(name);
        out.o["saved_partitions"] = std::move(parts);
        out.o["readback_confirmed"] = true;
        return true;
    };
    std::vector<std::string> wanted;
    for (const auto& name : names.a)
        wanted.push_back(name.s);
    if (current.partitions == wanted) {
        if (!record_readback(original))
            return finish("unknown", "unchanged_revision_unavailable", {}, EIO);
        out.o["saved"] = true;
        return finish("unchanged", "already_saved_no_write", {}, 0);
    }
    document.fields.o["partitions"] = names;
    if (document.schema_version == 0) {
        const auto backup = ksud::replace_file_durable(
            runtime_config_file().string() + ".pre-schema-1.bak", original.bytes);
        if (!backup.complete())
            return finish("failed", "migration_backup_unconfirmed",
                          "config unchanged; backup may exist", backup.error_number);
    }
    document.fields.o["schema_version"] = CONFIG_SCHEMA_VERSION;
    const auto bytes = stringify_json(document.fields, 2) + "\n";
    Config validated;
    if (!parse_partition_config(bytes, validated, error))
        return finish("rejected", "result_invalid", error, EINVAL);
    if (!(original == checked::read_document(runtime_config_file())))
        return finish("conflict", "config_changed_before_commit", {}, ESTALE);
    const auto saved = ksud::replace_file_durable(runtime_config_file(), bytes);
    out.o["durability_confirmed"] = saved.complete();
    const auto readback = checked::read_document(runtime_config_file());
    const bool observed = record_readback(readback);
    if (!saved.replaced())
        return finish("failed", "config_not_replaced",
                      "do not automatically replay; inspect current configuration",
                      saved.error_number);
    if (!observed || readback.bytes != bytes)
        return finish("unknown", "save_readback_not_matched",
                      "query current configuration before another explicit save", EIO);
    if (!saved.complete())
        return finish("partial", "replaced_durability_unconfirmed",
                      "new bytes observed; persistence across power loss unconfirmed",
                      saved.error_number);
    out.o["saved"] = true;
    return finish("saved", "saved_only_reboot_required", {}, 0);
}

bool reset_kernel_build_config(std::string& error) {
    ConfigFileLock lock;
    if (!lock.acquire(error))
        return false;
    errno = 0;
    const auto input = ksud::read_file(runtime_config_file().string());
    if (!input && errno != ENOENT) {
        error = "read config: " + std::string(std::strerror(errno));
        return false;
    }
    ConfigDocument document;
    if (!parse_config_document(input.value_or(default_config_json()), document, error) ||
        !config_document_is_writable(document, error)) {
        return false;
    }
    auto& root = document.fields;
    // Keep unrelated settings and valid drafts. Unknown fields/version block
    // writes; reset only repairs known Kernel Build fields.
    const auto defaults = json::parse(default_config_json());
    for (const auto* key :
         {"kernel_build_release", "kernel_build_version", "kernel_build_apply_stage"}) {
        if (const auto* value = root.find(key)) {
            JsonValue field = JsonValue::object();
            field.o[key] = *value;
            std::string validation_error;
            if (!validate_config_patch(field, validation_error))
                root.o[key] = *defaults.find(key);
        }
    }
    root.o["enable_kernel_build_spoof"] = false;
    Config validated;
    if (!parse_config_json(stringify_json(root), validated, error))
        return false;
    return save_config_document(document, input, error);
}

}  // namespace sumhp
