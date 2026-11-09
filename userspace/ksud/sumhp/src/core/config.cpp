#include "sumhp/config.hpp"

#include "core/json.hpp"
#include "core/log.hpp"
#include "core/runtime.hpp"
#include "uapi/sumh.h"
#include "utils.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
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

    bool acquire(std::string& error) {
        if (!prepare_private_directory(runtime_data_dir(), error)) {
            return false;
        }
        const std::string lock_path = runtime_config_file().string() + ".lock";
        fd_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd_ < 0) {
            error = "open " + lock_path + ": " + std::strerror(errno);
            return false;
        }
        if (::flock(fd_, LOCK_EX) != 0) {
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
  "mountsource": "KSU",
  "work_dir": "/dev/sumhp",
  "mirror_dir": "",
  "mirror_img_size_mb": 2048,
  "overlay_writable": false,
  "debug": false,
  "verbose": false,
  "fs_type": "auto",
  "enable_kernel_debug": false,
  "enable_maps_spoof": false,
  "maps_rules": [],
  "kstat_rules": [],
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

void prune_config_fields(json::Value& config) {
    static const auto defaults = json::parse(default_config_json());
    for (auto it = config.o.begin(); it != config.o.end();) {
        if (!defaults.find(it->first))
            it = config.o.erase(it);
        else
            ++it;
    }
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

json::Value kstat_path_rules_json(const std::vector<KstatPathRule>& rules) {
    JsonValue result;
    result.type = JsonValue::Type::Array;
    for (const auto& rule : rules) {
        JsonValue item = JsonValue::object();
        item.o["target_path"] = rule.target_path;
        item.o["mode"] = rule.mode;
        if (rule.mode == "custom") {
            JsonValue fields = JsonValue::object();
            for (const auto& [key, value] : rule.custom_fields) {
                const auto separator = key.find('.');
                if (separator == std::string::npos)
                    fields.o[key] = value;
                else
                    fields[key.substr(0, separator)][key.substr(separator + 1)] = value;
            }
            item.o["fields"] = std::move(fields);
        } else {
            item.o["source_path"] = rule.source_path;
        }
        result.a.push_back(std::move(item));
    }
    return result;
}

namespace {
bool valid_kstat_digits(const std::string& value, bool allow_negative) {
    if (value.empty())
        return false;
    const bool negative = value.front() == '-';
    if (negative && (!allow_negative || value.size() == 1 || value[1] == '0'))
        return false;
    const std::size_t start = negative ? 1 : 0;
    if (!negative && value.size() > 1 && value.front() == '0')
        return false;
    return std::all_of(value.begin() + start, value.end(),
                       [](char digit) { return digit >= '0' && digit <= '9'; });
}
bool parse_kstat_unsigned(const std::string& value, std::uint64_t limit, std::uint64_t& result) {
    if (!valid_kstat_digits(value, false))
        return false;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
    return ec == std::errc{} && end == value.data() + value.size() && result <= limit;
}
bool parse_kstat_signed(const std::string& value, std::int64_t& result) {
    if (!valid_kstat_digits(value, true))
        return false;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
    return ec == std::errc{} && end == value.data() + value.size();
}
}  // namespace

bool parse_kstat_custom_fields(const std::map<std::string, std::string>& fields,
                               KstatCustomValues& values, std::string& error) {
    if (fields.empty()) {
        error = "custom kstat fields must not be empty";
        return false;
    }
    KstatCustomValues parsed;
    const auto u32_max = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
    const auto i64_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto u64_max = std::numeric_limits<std::uint64_t>::max();
    for (const auto& [key, value] : fields) {
        const bool seconds = key == "atime.sec" || key == "mtime.sec" || key == "ctime.sec";
        const bool nanos = key == "atime.nsec" || key == "mtime.nsec" || key == "ctime.nsec";
        const bool unsigned64 = key == "ino" || key == "blocks";
        const bool unsigned32 = key == "dev" || key == "nlink" || key == "blksize";
        const bool size = key == "size";
        if (!seconds && !nanos && !unsigned64 && !unsigned32 && !size) {
            error = "unknown custom kstat field: " + key;
            return false;
        }
        std::uint64_t unsigned_number = 0;
        std::int64_t signed_number = 0;
        const auto limit = nanos        ? 999999999ULL
                           : unsigned64 ? u64_max
                           : unsigned32 ? u32_max
                                        : i64_max;
        if (seconds ? !parse_kstat_signed(value, signed_number)
                    : !parse_kstat_unsigned(value, limit, unsigned_number)) {
            error = "invalid decimal value for custom kstat field: " + key;
            return false;
        }
        if (key == "dev") {
            parsed.field_mask |= SUMH_KSTAT_FIELD_DEV;
            parsed.dev = static_cast<std::uint32_t>(unsigned_number);
        } else if (key == "ino") {
            parsed.field_mask |= SUMH_KSTAT_FIELD_INO;
            parsed.ino = unsigned_number;
        } else if (key == "nlink") {
            parsed.field_mask |= SUMH_KSTAT_FIELD_NLINK;
            parsed.nlink = static_cast<std::uint32_t>(unsigned_number);
        } else if (key == "size") {
            parsed.field_mask |= SUMH_KSTAT_FIELD_SIZE;
            parsed.size = static_cast<std::int64_t>(unsigned_number);
        } else if (key == "blksize") {
            parsed.field_mask |= SUMH_KSTAT_FIELD_BLKSIZE;
            parsed.blksize = static_cast<std::uint32_t>(unsigned_number);
        } else if (key == "blocks") {
            parsed.field_mask |= SUMH_KSTAT_FIELD_BLOCKS;
            parsed.blocks = unsigned_number;
        } else if (key == "atime.sec")
            parsed.atime_sec = signed_number;
        else if (key == "atime.nsec")
            parsed.atime_nsec = static_cast<std::int64_t>(unsigned_number);
        else if (key == "mtime.sec")
            parsed.mtime_sec = signed_number;
        else if (key == "mtime.nsec")
            parsed.mtime_nsec = static_cast<std::int64_t>(unsigned_number);
        else if (key == "ctime.sec")
            parsed.ctime_sec = signed_number;
        else if (key == "ctime.nsec")
            parsed.ctime_nsec = static_cast<std::int64_t>(unsigned_number);
    }
    for (const auto& [name, bit] :
         {std::pair{"atime", SUMH_KSTAT_FIELD_ATIME}, std::pair{"mtime", SUMH_KSTAT_FIELD_MTIME},
          std::pair{"ctime", SUMH_KSTAT_FIELD_CTIME}}) {
        const bool has_sec = fields.count(std::string(name) + ".sec") != 0;
        const bool has_nsec = fields.count(std::string(name) + ".nsec") != 0;
        if (has_sec != has_nsec) {
            error = std::string("custom kstat ") + name + " requires sec and nsec";
            return false;
        }
        if (has_sec)
            parsed.field_mask |= bit;
    }
    values = parsed;
    return true;
}

bool validate_kstat_path_rules(const std::vector<KstatPathRule>& rules, std::string& error) {
    if (rules.size() > SUMH_MANAGED_KSTAT_MAX_RULES) {
        error = "kstat_rules supports at most 32 rules";
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
        if (!valid_path(rule.target_path, SUMH_MAX_LEN_PATHNAME) ||
            (rule.mode != "custom" && !valid_path(rule.source_path, SUMH_USER_HIDE_PATH_MAX))) {
            error = "invalid kstat rule paths at index " + std::to_string(index) +
                    ": absolute, NUL/CR/LF-free paths required; target < 256 bytes, "
                    "source < 4096 bytes";
            return false;
        }
        if (rule.mode != "identity" && rule.mode != "full" && rule.mode != "custom") {
            error = "invalid kstat rule mode at index " + std::to_string(index);
            return false;
        }
        if (rule.mode == "custom") {
            KstatCustomValues parsed;
            if (!rule.source_path.empty()) {
                error = "custom kstat rules must not set source_path";
                return false;
            }
            if (!parse_kstat_custom_fields(rule.custom_fields, parsed, error)) {
                return false;
            }
        } else if (!rule.custom_fields.empty()) {
            error = "identity/full kstat rules must not set custom fields";
            return false;
        }
        if (!targets.insert(rule.target_path).second) {
            error = "duplicate kstat target path at index " + std::to_string(index);
            return false;
        }
        android_slash_escaping +=
            3 * static_cast<std::size_t>(
                    std::count(rule.target_path.begin(), rule.target_path.end(), '/') +
                    std::count(rule.source_path.begin(), rule.source_path.end(), '/'));
    }
    if (json_quote(stringify_json(kstat_path_rules_json(rules))).size() + android_slash_escaping >
        MAPS_RULES_MAX_ENCODED_BYTES) {
        error = "kstat_rules exceeds the 56 KiB encoded configuration budget";
        return false;
    }
    return true;
}

namespace {
bool parse_kstat_path_rules(const JsonValue& value, std::vector<KstatPathRule>& rules,
                            std::string& error) {
    if (!value.is_array()) {
        error = "configuration field 'kstat_rules' must be an array";
        return false;
    }
    std::vector<KstatPathRule> parsed;
    for (const auto& item : value.a) {
        if (!item.is_object() || item.o.size() != 3) {
            error =
                "kstat_rules entries require exactly target_path/mode and source_path or fields";
            return false;
        }
        const auto* target = item.find("target_path");
        const auto* mode = item.find("mode");
        if (!target || !mode || !target->is_string() || !mode->is_string()) {
            error = "kstat_rules entries require target_path/mode strings";
            return false;
        }
        KstatPathRule rule;
        rule.target_path = target->s;
        rule.mode = mode->s;
        if (rule.mode == "custom") {
            const auto* fields = item.find("fields");
            if (!fields || !fields->is_object()) {
                error = "custom kstat rules require a fields object without source_path";
                return false;
            }
            for (const auto& [key, field] : fields->o) {
                if (key == "atime" || key == "mtime" || key == "ctime") {
                    const auto* sec = field.find("sec");
                    const auto* nsec = field.find("nsec");
                    if (!field.is_object() || field.o.size() != 2 || !sec || !nsec ||
                        !sec->is_string() || !nsec->is_string()) {
                        error = "custom kstat times require sec and nsec strings";
                        return false;
                    }
                    rule.custom_fields[key + ".sec"] = sec->s;
                    rule.custom_fields[key + ".nsec"] = nsec->s;
                } else {
                    if (key != "dev" && key != "ino" && key != "nlink" && key != "size" &&
                        key != "blksize" && key != "blocks") {
                        error = "unknown custom kstat field: " + key;
                        return false;
                    }
                    if (!field.is_string()) {
                        error = "custom kstat scalar fields must be decimal strings";
                        return false;
                    }
                    rule.custom_fields[key] = field.s;
                }
            }
        } else {
            const auto* source = item.find("source_path");
            if (!source || !source->is_string()) {
                error = "identity/full kstat rules require source_path string without fields";
                return false;
            }
            rule.source_path = source->s;
        }
        parsed.push_back(std::move(rule));
    }
    if (!validate_kstat_path_rules(parsed, error))
        return false;
    rules = std::move(parsed);
    return true;
}

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
        if (key == "fs_type" && !value.s.empty() && value.s != "auto" && value.s != "tmpfs" &&
            value.s != "ext4" && value.s != "erofs")
            return reject(key, "must be auto, tmpfs, ext4 or erofs");
        if (key == "mount_backend" && !value.s.empty() && value.s != "auto" && value.s != "sumh" &&
            value.s != "overlay" && value.s != "magic" && value.s != "none")
            return reject(key, "must be auto, sumh, overlay, magic or none");
        if (key == "partitions") {
            for (const auto& partition : value.a) {
                if (!partition.is_string() || partition.s.empty() || partition.s == "." ||
                    partition.s == ".." || partition.s.find('/') != std::string::npos ||
                    partition.s.find('\0') != std::string::npos)
                    return reject(key, "must contain relative partition names without slashes");
            }
        }
        if (key == "maps_rules") {
            std::vector<MapsPathRule> rules;
            if (!parse_maps_path_rules(value, rules, error))
                return false;
        }
        if (key == "kstat_rules") {
            std::vector<KstatPathRule> rules;
            if (!parse_kstat_path_rules(value, rules, error))
                return false;
        }
    }
    return true;
}

namespace {
bool save_config(const std::string& data, std::string& error) {
    if (ksud::write_file_atomic(runtime_config_file(), data))
        return true;
    error = "write config: " + std::string(std::strerror(errno));
    return false;
}
}  // namespace

bool write_default_config(std::string& error) {
    ConfigFileLock lock;
    return lock.acquire(error) && save_config(default_config_json(), error);
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
    errno = 0;
    const auto input = ksud::read_file((runtime_data_dir() / "user_hide_rules.json").string());
    if (!input) {
        if (errno == ENOENT) {
            rules.clear();
            return true;
        }
        error = "read user hide rules: " + std::string(std::strerror(errno ? errno : EIO));
        return false;
    }
    JsonValue root;
    std::string parse_error;
    if (!parse_json(*input, root, parse_error) || !root.is_array()) {
        error = "invalid user hide rules: " +
                (parse_error.empty() ? std::string("expected a JSON array") : parse_error);
        return false;
    }
    std::vector<std::string> parsed;
    parsed.reserve(root.a.size());
    for (const auto& item : root.a) {
        if (!item.is_string() || item.s.empty() || item.s.front() != '/' ||
            item.s.find('\0') != std::string::npos || item.s.size() >= SUMH_USER_HIDE_PATH_MAX) {
            error = "invalid user hide rule at index " + std::to_string(parsed.size());
            return false;
        }
        parsed.push_back(item.s);
    }
    rules = std::move(parsed);
    return true;
}

std::vector<std::string> load_user_hide_rules() {
    std::vector<std::string> rules;
    std::string error;
    if (!load_user_hide_rules(rules, error))
        logging::write(logging::Level::Warning, "config", error);
    return rules;
}

bool save_user_hide_rules(const std::vector<std::string>& rules) {
    JsonValue root;
    root.type = JsonValue::Type::Array;
    for (const auto& rule : rules)
        root.a.emplace_back(rule);
    return ksud::write_file_atomic(runtime_data_dir() / "user_hide_rules.json",
                                   stringify_json(root, 2) + "\n");
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
    JsonValue root;
    if (!parse_json(json, root, error)) {
        return false;
    }
    if (!root.is_object()) {
        error = "config root must be a JSON object";
        return false;
    }

    // Validate these fields even when loading a manually edited config. Invalid
    // definitions must never be applied or silently replaced by an empty set.
    JsonValue build_fields;
    build_fields.type = JsonValue::Type::Object;
    for (const auto* key :
         {"enable_kernel_build_spoof", "kernel_build_release", "kernel_build_version",
          "kernel_build_apply_stage", "enable_maps_spoof"}) {
        if (const auto* value = root.find(key))
            build_fields.o[key] = *value;
    }
    if (!validate_config_patch(build_fields, error))
        return false;
    const bool build_enabled =
        json_bool_or(&root, "enable_kernel_build_spoof", config.enable_kernel_build_spoof);
    const auto build_release =
        json_string_or(&root, "kernel_build_release", config.kernel_build_release);
    const auto build_version =
        json_string_or(&root, "kernel_build_version", config.kernel_build_version);
    if (build_enabled && build_release.empty() && build_version.empty()) {
        error = "kernel build spoof requires a custom kernel_build_release or kernel_build_version";
        return false;
    }
    std::vector<MapsPathRule> maps_rules;
    if (const auto* value = root.find("maps_rules")) {
        if (!parse_maps_path_rules(*value, maps_rules, error))
            return false;
    }
    std::vector<KstatPathRule> kstat_rules;
    if (const auto* value = root.find("kstat_rules")) {
        if (!parse_kstat_path_rules(*value, kstat_rules, error))
            return false;
    }

    config.mount_source = json_string_or(&root, "mountsource", config.mount_source);
    config.work_dir = json_string_or(&root, "work_dir", config.work_dir);
    config.mirror_dir = json_string_or(&root, "mirror_dir", config.mirror_dir);
    config.mirror_img_size_mb = json_int_or(&root, "mirror_img_size_mb", config.mirror_img_size_mb);
    config.overlay_writable = json_bool_or(&root, "overlay_writable", config.overlay_writable);
    config.fs_type = json_string_or(&root, "fs_type", config.fs_type);
    config.debug = json_bool_or(&root, "debug", config.debug);
    config.verbose = json_bool_or(&root, "verbose", config.verbose);
    logging::set_debug_enabled(config.debug || config.verbose);
    config.enable_kernel_debug =
        json_bool_or(&root, "enable_kernel_debug", config.enable_kernel_debug);
    config.enable_maps_spoof = json_bool_or(&root, "enable_maps_spoof", config.enable_maps_spoof);
    config.maps_rules = std::move(maps_rules);
    config.kstat_rules = std::move(kstat_rules);
    config.enable_kernel_build_spoof = build_enabled;
    config.kernel_build_release = build_release;
    config.kernel_build_version = build_version;
    config.kernel_build_apply_stage =
        json_string_or(&root, "kernel_build_apply_stage", config.kernel_build_apply_stage);
    config.overlayfs_enabled = json_bool_or(&root, "overlayfs_enabled", config.overlayfs_enabled);
    config.magic_mount_enabled =
        json_bool_or(&root, "magic_mount_enabled", config.magic_mount_enabled);
    config.mount_backend = json_string_or(&root, "mount_backend", config.mount_backend);
    config.partitions = json_string_array_or_empty(root.find("partitions"));

    return true;
}

bool read_config_file(Config& config, std::string& error) {
    const auto input = ksud::read_file(runtime_config_file().string());
    if (!input) {
        error = "read config: " + std::string(std::strerror(errno));
        return false;
    }
    return parse_config_json(*input, config, error);
}

bool merge_config_json(const std::string& updates, std::string& error) {
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

    const auto input = ksud::read_file(runtime_config_file().string());
    if (!input && errno != ENOENT) {
        error = "read config: " + std::string(std::strerror(errno));
        return false;
    }
    JsonValue root;
    if (!parse_json(input.value_or(default_config_json()), root, error) || !root.is_object()) {
        if (error.empty()) {
            error = "config root must be an object";
        }
        return false;
    }

    prune_config_fields(root);
    prune_config_fields(patch);
    for (const auto& [key, value] : patch.o) {
        root.o[key] = value;
    }
    Config validated;
    if (!parse_config_json(stringify_json(root), validated, error))
        return false;
    const bool saved = save_config(stringify_json(root, 2) + "\n", error);
    if (saved)
        logging::set_debug_enabled(json_bool_or(&root, "debug", false) ||
                                   json_bool_or(&root, "verbose", false));
    return saved;
}

KstatRuleEditResult edit_kstat_path_rule(
    const std::string& target_path, const KstatPathRule* replacement, Config& config,
    std::string& error, const std::function<bool(const Config&, std::string&)>& apply) {
    KstatRuleEditResult result;
    if (replacement && replacement->target_path != target_path) {
        error = "kstat replacement target does not match edited path";
        return result;
    }
    ConfigFileLock lock;
    if (!lock.acquire(error))
        return result;
    const auto input = ksud::read_file(runtime_config_file().string());
    if (!input) {
        error = "read config: " + std::string(std::strerror(errno));
        return result;
    }
    JsonValue root;
    if (!parse_json(*input, root, error) || !root.is_object()) {
        if (error.empty())
            error = "config root must be an object";
        return result;
    }
    Config updated;
    if (!parse_config_json(*input, updated, error))
        return result;

    auto& rules = updated.kstat_rules;
    const auto first = std::find_if(rules.begin(), rules.end(), [&](const KstatPathRule& rule) {
        return rule.target_path == target_path;
    });
    if (replacement) {
        if (first == rules.end())
            rules.push_back(*replacement);
        else
            *first = *replacement;
    } else {
        rules.erase(std::remove_if(
                        rules.begin(), rules.end(),
                        [&](const KstatPathRule& rule) { return rule.target_path == target_path; }),
                    rules.end());
    }
    if (!validate_kstat_path_rules(rules, error))
        return result;
    prune_config_fields(root);
    root.o["kstat_rules"] = kstat_path_rules_json(rules);
    Config validated;
    if (!parse_config_json(stringify_json(root), validated, error))
        return result;
    if (!save_config(stringify_json(root, 2) + "\n", error))
        return result;
    logging::set_debug_enabled(json_bool_or(&root, "debug", false) ||
                               json_bool_or(&root, "verbose", false));
    config = std::move(validated);
    result.saved = true;
    result.applied = apply(config, error);
    if (!result.applied)
        result.apply_errno = errno;
    return result;
}

KstatRuleApplyResult apply_saved_kstat_rules(
    Config& config, std::string& error,
    const std::function<bool(const Config&, std::string&)>& apply) {
    KstatRuleApplyResult result;
    ConfigFileLock lock;
    if (!lock.acquire(error) || !read_config_file(config, error))
        return result;
    result.loaded = true;
    result.applied = apply(config, error);
    if (!result.applied)
        result.apply_errno = errno;
    return result;
}

bool reset_kernel_build_config(std::string& error) {
    ConfigFileLock lock;
    if (!lock.acquire(error))
        return false;
    const auto input = ksud::read_file(runtime_config_file().string());
    if (!input && errno != ENOENT) {
        error = "read config: " + std::string(std::strerror(errno));
        return false;
    }
    JsonValue root;
    if (!parse_json(input.value_or(default_config_json()), root, error) || !root.is_object()) {
        if (error.empty())
            error = "config root must be an object";
        return false;
    }
    // Keep unrelated settings and valid drafts, including unknown keys. Reset
    // is also the recovery path for a manually edited invalid build setting.
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
    return save_config(stringify_json(root, 2) + "\n", error);
}

}  // namespace sumhp
