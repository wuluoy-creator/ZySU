#include "mount/backend.hpp"
#include <iterator>

#include "core/json.hpp"
#include "core/runtime.hpp"
#include "mount/magic_mount.hpp"
#include "mount/module_rebuild.hpp"
#include "mount/mount_fs.hpp"
#include "mount/overlayfs.hpp"
#include "mount/partition_alias.hpp"
#include "mount/storage.hpp"
#include "mount/sumh.hpp"
#include "sumhp/sumh_client.hpp"
#include "utils.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace sumhp::mount {

namespace fs = std::filesystem;

std::string backend_kind_name(BackendKind kind) {
    switch (kind) {
    case BackendKind::SUMH:
        return "sumh";
    case BackendKind::Overlayfs:
        return "overlayfs";
    case BackendKind::MagicMount:
        return "magic_mount";
    }
    return "unknown";
}

namespace {
bool proc_filesystems_has(const std::string& name, int* error = nullptr) {
    if (error)
        *error = 0;
    errno = 0;
    std::ifstream in("/proc/filesystems");
    if (!in) {
        if (error)
            *error = errno != 0 ? errno : EIO;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream parts(line);
        std::string first;
        std::string second;
        parts >> first >> second;
        if (first == name || second == name) {
            return true;
        }
    }
    if (in.bad() || (!in.eof() && in.fail())) {
        if (error)
            *error = errno != 0 ? errno : EIO;
    }
    return false;
}

int count_committed_mounts() {
    return static_cast<int>(magic::active_mounts(Config{}).size());
}

bool apply_sumh_features(const Config& config, bool& maps_ok) {
    maps_ok = true;
    if (!::sumhp::sumh::is_available()) {
        return true;
    }
    std::string error;
    const bool features_ok = sumh::apply_feature_config(config, error, false, false);
    if (!features_ok) {
        fsutil::mlog("sumh: feature apply failed: " + error, logging::Level::Error);
    }
    const bool restore_maps = ksud::getprop("sys.boot_completed") == "1";
    if (!sumh::apply_maps_config(config, error, restore_maps)) {
        maps_ok = false;
        fsutil::mlog("sumh: maps apply failed: " + error, logging::Level::Error);
        return false;
    }
    return features_ok;
}
}  // namespace

namespace {
fs::path mount_orchestrator_file() {
    return runtime_data_dir() / "run" / "mount_orchestrator_boot";
}

bool claim_mount_orchestrator() {
    return !marker_matches_current_boot(mount_orchestrator_file()) &&
           write_boot_marker(mount_orchestrator_file());
}

}  // namespace

std::vector<BackendStatus> backend_statuses() {
    const auto version = ::sumhp::sumh::version_info();
    const bool sumh_available = version.status == ::sumhp::sumh::Status::Available;
    std::ostringstream sumh_detail;
    sumh_detail << "protocol expected=" << version.expected_protocol
                << " kernel=" << version.kernel_protocol << " uid=" << version.process_uid
                << " euid=" << version.process_euid
                << " status=" << static_cast<int>(version.status);

    const bool overlayfs_available = proc_filesystems_has("overlay");

    const int magic_mounts = count_committed_mounts();
    std::ostringstream magic_detail;
    magic_detail << "Magisk-style systemless mount; live mounts=" << magic_mounts;

    return {
        {BackendKind::SUMH, "SUMH", sumh_available, true, sumh_detail.str()},
        {BackendKind::Overlayfs, "OverlayFS", overlayfs_available, false,
         overlayfs_available ? "overlay filesystem registered"
                             : "overlay filesystem is not registered"},
        {BackendKind::MagicMount, "Magic Mount", true, false, magic_detail.str()},
    };
}

bool enumerate_mountable_modules_checked(std::vector<ModuleEntry>& modules, std::string& error,
                                         std::size_t max_entries) {
    std::vector<ModuleEntry> out;
    const fs::path root = runtime_modules_dir();
    std::error_code ec;
    const auto fail = [&](const std::string& phase, const fs::path& path, int code) {
        errno = code != 0 ? code : EIO;
        error = "module discovery " + phase + " at " + path.string() + ": " +
                std::error_code(errno, std::generic_category()).message();
        return false;
    };
    const auto root_status = fs::status(root, ec);
    if (ec)
        return fail("root metadata", root, ec.value());
    if (!fs::is_directory(root_status))
        return fail("root is not a directory", root, fs::exists(root_status) ? ENOTDIR : ENOENT);
    fs::directory_iterator it(root, ec);
    if (ec)
        return fail("open directory", root, ec.value());
    std::size_t scanned = 0;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec)
            return fail("iterate directory", root, ec.value());
        if (scanned++ == max_entries)
            return fail("entry budget exceeded", root, E2BIG);
        const auto& entry = *it;
        const fs::path& p = entry.path();
        const bool directory = entry.is_directory(ec);
        if (ec)
            return fail("entry metadata", p, ec.value());
        if (!directory)
            continue;
        const auto prop = p / "module.prop";
        const bool has_prop = fs::exists(prop, ec);
        if (ec)
            return fail("module.prop metadata", prop, ec.value());
        if (!has_prop)
            continue;
        bool excluded = false;
        for (const auto& marker : {p / "disable", p / "remove", p / "skip_mount",
                                   runtime_data_dir() / "run" / "hot_unmounted" / p.filename()}) {
            const bool exists = fs::exists(marker, ec);
            if (ec)
                return fail("marker metadata", marker, ec.value());
            if (exists) {
                excluded = true;
                break;
            }
        }
        if (!excluded)
            out.push_back({p.filename().string(), p});
    }
    // increment(error_code) can both set ec and move to end.
    if (ec)
        return fail("iterate directory", root, ec.value());
    std::sort(out.begin(), out.end(),
              [](const ModuleEntry& a, const ModuleEntry& b) { return a.id < b.id; });
    modules = std::move(out);
    error.clear();
    return true;
}

std::vector<ModuleEntry> enumerate_mountable_modules() {
    std::vector<ModuleEntry> out;
    std::string error;
    (void)enumerate_mountable_modules_checked(out, error);
    return out;
}

// Share the config/migration lock so legacy two-file imports and direct CLI
// mutations cannot race the daemon's module configuration snapshots.
class ModuleConfigLock {
public:
    ModuleConfigLock() = default;
    ModuleConfigLock(const ModuleConfigLock&) = delete;
    ModuleConfigLock& operator=(const ModuleConfigLock&) = delete;
    ModuleConfigLock(ModuleConfigLock&&) = delete;
    ModuleConfigLock& operator=(ModuleConfigLock&&) = delete;
    ~ModuleConfigLock() {
        if (fd_ >= 0) {
            (void)::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
    }
    bool acquire(std::string& error) {
        if (!prepare_private_directory(runtime_data_dir(), error))
            return false;
        const auto path = runtime_config_file().string() + ".lock";
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd_ < 0 || ::flock(fd_, LOCK_EX) != 0) {
            error = "lock module configuration: " + std::string(std::strerror(errno));
            return false;
        }
        return true;
    }

private:
    int fd_ = -1;
};

namespace {
struct ModuleConfigRecord {
    std::string mode = "auto";
    std::vector<ModuleRule> rules;
};
using ModuleConfiguration = std::map<std::string, ModuleConfigRecord>;

bool valid_module_mode(const std::string& mode, bool path_rule = false) {
    return mode == "auto" || mode == "sumh" || mode == "overlay" || mode == "magic" ||
           mode == "none" || (path_rule && mode == "hide");
}

bool valid_stored_module_rule_path(const std::string& path) {
    return !path.empty() && path.front() == '/' && path.size() < 4096 &&
           path.find('\0') == std::string::npos && path.find_first_of("\r\n") == std::string::npos;
}

JsonValue module_rules_json(const std::vector<ModuleRule>& rules) {
    JsonValue array;
    array.type = JsonValue::Type::Array;
    for (const auto& rule : rules) {
        JsonValue item = JsonValue::object();
        item.o["path"] = JsonValue(rule.path);
        item.o["mode"] = JsonValue(rule.mode);
        array.a.push_back(std::move(item));
    }
    return array;
}

bool validate_module_rules(const std::vector<ModuleRule>& rules, std::string& error) {
    if (rules.size() > 256) {
        error = "module supports at most 256 path rules";
        return false;
    }
    std::set<std::string> paths;
    std::size_t slash_escaping = 0;
    for (std::size_t index = 0; index < rules.size(); ++index) {
        const auto& rule = rules[index];
        if (!valid_module_rule_path(rule.path) || !valid_module_mode(rule.mode, true)) {
            error = "invalid module path or mode at index " + std::to_string(index);
            return false;
        }
        if (!paths.insert(rule.path).second) {
            error = "duplicate module path at index " + std::to_string(index);
            return false;
        }
        // Android JSONObject escapes '/' during both serialization passes.
        slash_escaping +=
            3 * static_cast<std::size_t>(std::count(rule.path.begin(), rule.path.end(), '/'));
    }
    if (json_quote(stringify_json(module_rules_json(rules))).size() + slash_escaping >
        56UL * 1024) {
        error = "module rules exceed the 56 KiB encoded configuration budget";
        return false;
    }
    return true;
}

bool parse_module_rules_value(const JsonValue& value, std::vector<ModuleRule>& rules,
                              std::string& error, bool strict = true) {
    if (!value.is_array()) {
        error = "module rules must be a JSON array";
        return false;
    }
    std::vector<ModuleRule> parsed;
    for (const auto& entry : value.a) {
        const auto* path = entry.find("path");
        const auto* mode = entry.find("mode");
        if (!entry.is_object() || entry.o.size() != 2 || !path || !path->is_string() || !mode ||
            !mode->is_string()) {
            error = "module rules require path and mode strings";
            return false;
        }
        if (!valid_stored_module_rule_path(path->s) || !valid_module_mode(mode->s, true)) {
            error = "invalid saved module path or mode";
            return false;
        }
        parsed.push_back({path->s, mode->s});
    }
    if (strict && !validate_module_rules(parsed, error))
        return false;
    rules = std::move(parsed);
    return true;
}

bool parse_module_document(const std::string& input, const char* name, JsonValue& root,
                           std::string& error) {
    if (!parse_json(input, root, error) || !root.is_object()) {
        error = std::string("invalid ") + name + ": " +
                (error.empty() ? "expected a JSON object" : error);
        return false;
    }
    return true;
}

bool read_module_document(const char* name, JsonValue& root, std::string& error) {
    errno = 0;
    const auto input = ksud::read_file((runtime_data_dir() / name).string());
    if (!input) {
        if (errno == ENOENT) {
            root = JsonValue::object();
            return true;
        }
        error = std::string("read ") + name + ": " + std::strerror(errno ? errno : EIO);
        return false;
    }
    return parse_module_document(*input, name, root, error);
}

bool parse_module_configuration_records(const JsonValue& modes, const JsonValue& rules,
                                        ModuleConfiguration& records, std::string& error) {
    ModuleConfiguration parsed;
    for (const auto& [id, value] : modes.o) {
        // Canonical entries own their mode, including explicit auto.
        const auto* configured = rules.find(id);
        if (configured && configured->is_object())
            continue;
        if (!valid_module_id(id) || !value.is_string() || !valid_module_mode(value.s)) {
            error = "invalid legacy module mode for " + id;
            return false;
        }
        parsed[id].mode = value.s;
    }
    for (const auto& [id, value] : rules.o) {
        if (!valid_module_id(id)) {
            error = "invalid module id in module_rules.json";
            return false;
        }
        auto& record = parsed[id];
        const JsonValue* entries = &value;
        if (value.is_object()) {
            const auto* mode = value.find("mode");
            entries = value.find("rules");
            if (value.o.size() != 2 || !mode || !mode->is_string() || !valid_module_mode(mode->s) ||
                !entries) {
                error = "invalid module configuration for " + id;
                return false;
            }
            record.mode = mode->s;
        }
        // Existing controllers accepted noncanonical prefixes and unbounded
        // arrays. Preserve their exact matching behavior so users can repair
        // them through configure/removal; new rule sets use strict validation.
        if (!parse_module_rules_value(*entries, record.rules, error, false)) {
            error.insert(0, "invalid module configuration for " + id + ": ");
            return false;
        }
    }
    records = std::move(parsed);
    return true;
}

bool read_module_configuration(ModuleConfiguration& records, std::string& error) {
    JsonValue modes;
    JsonValue rules;
    return read_module_document("module_mode.json", modes, error) &&
           read_module_document("module_rules.json", rules, error) &&
           parse_module_configuration_records(modes, rules, records, error);
}

void export_module_configuration(const ModuleConfiguration& records, ModuleModeMap& modes,
                                 ModuleRuleMap& rules) {
    ModuleModeMap parsed_modes;
    ModuleRuleMap parsed_rules;
    for (const auto& [id, record] : records) {
        if (record.mode != "auto")
            parsed_modes[id] = record.mode;
        if (!record.rules.empty())
            parsed_rules[id] = record.rules;
    }
    modes = std::move(parsed_modes);
    rules = std::move(parsed_rules);
}

bool write_module_configuration(const ModuleConfiguration& records, std::string& error) {
    JsonValue root = JsonValue::object();
    for (const auto& [id, record] : records) {
        JsonValue value = JsonValue::object();
        value.o["mode"] = JsonValue(record.mode);
        value.o["rules"] = module_rules_json(record.rules);
        root.o[id] = std::move(value);
    }
    if (!ksud::write_file_durable(runtime_data_dir() / "module_rules.json",
                                  stringify_json(root, 2) + "\n", error)) {
        error = "write module configuration: " + error;
        return false;
    }
    return true;
}

bool update_module_configuration(const std::string& id,
                                 const std::function<void(ModuleConfigRecord&)>& update,
                                 std::string& error, bool validate_rules = true) {
    error.clear();
    if (!valid_module_id(id)) {
        error = "invalid module id";
        return false;
    }
    ModuleConfigLock lock;
    ModuleConfiguration records;
    if (!lock.acquire(error) || !read_module_configuration(records, error))
        return false;
    auto& record = records[id];
    update(record);
    if (!valid_module_mode(record.mode) ||
        (validate_rules && !validate_module_rules(record.rules, error))) {
        if (error.empty())
            error = "invalid module mode";
        return false;
    }
    // Keep auto/empty records so old module_mode.json values never reappear.
    return write_module_configuration(records, error);
}
}  // namespace

bool valid_module_id(const std::string& id) {
    return !id.empty() && id != "." && id != ".." &&
           std::all_of(id.begin(), id.end(), [](unsigned char byte) {
               return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                      (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' || byte == '.';
           });
}

bool valid_module_rule_path(const std::string& path) {
    if (!valid_stored_module_rule_path(path) || (path.size() > 1 && path.back() == '/'))
        return false;
    if (path == "/")
        return true;
    std::size_t start = 1;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto segment = path.substr(start, end == std::string::npos ? end : end - start);
        if (segment.empty() || segment == "." || segment == "..")
            return false;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}

bool parse_module_rules(const std::string& json, std::vector<ModuleRule>& rules,
                        std::string& error) {
    error.clear();
    JsonValue value;
    return parse_json(json, value, error) && parse_module_rules_value(value, rules, error);
}

bool load_module_configuration(ModuleModeMap& modes, ModuleRuleMap& rules, std::string& error) {
    error.clear();
    ModuleConfigLock lock;
    ModuleConfiguration records;
    if (!lock.acquire(error) || !read_module_configuration(records, error))
        return false;
    export_module_configuration(records, modes, rules);
    return true;
}

bool parse_module_configuration_documents(const std::string& modes_json,
                                          const std::string& rules_json, ModuleModeMap& modes,
                                          ModuleRuleMap& rules, std::string& error) {
    error.clear();
    JsonValue modes_document;
    JsonValue rules_document;
    ModuleConfiguration records;
    if (!parse_module_document(modes_json, "module_mode.json", modes_document, error) ||
        !parse_module_document(rules_json, "module_rules.json", rules_document, error) ||
        !parse_module_configuration_records(modes_document, rules_document, records, error))
        return false;
    export_module_configuration(records, modes, rules);
    return true;
}

bool configure_module(const std::string& id, const std::string& mode,
                      const std::vector<ModuleRule>& rules, std::string& error) {
    return update_module_configuration(
        id,
        [&](auto& record) {
            record.mode = mode;
            record.rules = rules;
        },
        error);
}

bool set_module_mode(const std::string& id, const std::string& mode, std::string& error) {
    return update_module_configuration(id, [&](auto& record) { record.mode = mode; }, error, false);
}

bool upsert_module_rule(const std::string& id, const ModuleRule& rule, std::string& error) {
    return update_module_configuration(
        id,
        [&](auto& record) {
            const auto found =
                std::find_if(record.rules.begin(), record.rules.end(),
                             [&](const auto& existing) { return existing.path == rule.path; });
            if (found == record.rules.end())
                record.rules.push_back(rule);
            else
                found->mode = rule.mode;
        },
        error);
}

bool remove_module_rule(const std::string& id, const std::string& path, std::string& error) {
    if (!valid_stored_module_rule_path(path)) {
        error = "invalid module path";
        return false;
    }
    return update_module_configuration(
        id,
        [&](auto& record) {
            record.rules.erase(std::remove_if(record.rules.begin(), record.rules.end(),
                                              [&](const auto& rule) { return rule.path == path; }),
                               record.rules.end());
        },
        error, false);
}

namespace {
bool dir_has_direct_files(const fs::path& dir, const std::vector<std::string>& parts,
                          bool system_root, std::string& error) {
    std::error_code ec;
    const auto status = fs::symlink_status(dir, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        (!ec && status.type() == fs::file_type::not_found)) {
        return false;
    }
    const auto record_error = [&](const fs::path& path) {
        if (error.empty())
            error = "module backend metadata at " + path.string() + ": " + ec.message();
    };
    const auto installer_alias = [&](const fs::path& path) {
        if (path.parent_path().filename() != "system" || path.filename() == "system")
            return false;
        const auto link = fs::read_symlink(path, ec);
        if (ec) {
            record_error(path);
            return false;
        }
        if (link != fs::path("..") / path.filename())
            return false;
        const auto target = path.parent_path().parent_path() / path.filename();
        const auto target_status = fs::symlink_status(target, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            record_error(target);
        return !ec && fs::is_directory(target_status);
    };
    if (ec) {
        record_error(dir);
        return true;
    }
    if (!fs::is_directory(status))
        return !fs::is_symlink(status) || !installer_alias(dir);
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        const auto& entry = *it;
        const auto entry_status = entry.symlink_status(ec);
        if (ec) {
            record_error(entry.path());
            return true;
        }
        if (system_root &&
            std::find(parts.begin(), parts.end(), entry.path().filename().string()) !=
                parts.end() &&
            fs::is_symlink(entry_status) && installer_alias(entry.path())) {
            continue;  // canonical installer alias is scanned through the top-level tree
        }
        if (!fs::is_directory(entry_status)) {
            return true;  // files and arbitrary symlinks cannot form overlay leaves
        }
    }
    if (ec)
        record_error(dir);
    return static_cast<bool>(ec);
}

// A module needs magic mount when it places files directly at a partition root
// (e.g. system/build.prop): overlay only stacks on leaf subdirs, never on a
// partition root. Directory trees remain subject to OverlayFS target validation.
bool module_needs_magic(const ModuleEntry& m, const Config& config, std::string& error) {
    const auto parts = fsutil::managed_partitions(config.partitions);
    if (dir_has_direct_files(m.path / "system", parts, true, error)) {
        return true;
    }
    return std::any_of(parts.begin(), parts.end(), [&](const auto& part) {
        return part != "system" &&
               (dir_has_direct_files(m.path / part, parts, false, error) ||
                dir_has_direct_files(m.path / "system" / part, parts, false, error));
    });
}

// True if the module has any managed-partition tree (i.e. contributes mounts).
bool module_has_content(const ModuleEntry& m, const Config& config, std::string& error) {
    std::error_code root_error;
    if (!fs::is_directory(m.path, root_error)) {
        error = "module backend root at " + m.path.string() + ": " +
                (root_error ? root_error.message() : "not a directory");
        return false;
    }
    for (const auto& part : fsutil::managed_partitions(config.partitions)) {
        std::error_code ec;
        const auto path = m.path / part;
        if (fs::is_directory(path, ec)) {
            return true;
        }
        if (ec && ec != std::errc::no_such_file_or_directory && error.empty())
            error = "module backend content at " + path.string() + ": " + ec.message();
    }
    return false;
}

bool sumh_usable() {
    const auto version = ::sumhp::sumh::version_info();
    return version.status == ::sumhp::sumh::Status::Available;
}

std::string fallback_backend(const ModuleEntry& m, const Config& config,
                             ModuleBackendDecision& decision) {
    auto& overlay = decision.overlay;
    if (config.overlayfs_enabled && !overlay.checked) {
        overlay.checked = true;
        overlay.available = proc_filesystems_has("overlay", &overlay.error_number);
        overlay.known = overlay.error_number == 0;
        if (!overlay.known)
            overlay.detail = "filesystem_query_failed";
        else
            overlay.detail =
                overlay.available ? "filesystem_registered" : "filesystem_not_registered";
        if (!overlay.known)
            decision.complete = false;
    }
    if (config.overlayfs_enabled && overlay.available &&
        !module_needs_magic(m, config, decision.observation_error)) {
        return "overlay";
    }
    if (config.overlayfs_enabled && overlay.available)
        decision.requires_magic = true;
    return config.magic_mount_enabled ? "magic" : "none";
}
}  // namespace

ModuleBackendDecision resolve_module_backend_decision(const ModuleEntry& m, const Config& config,
                                                      const ModuleModeMap& modes, bool allow_sumh) {
    ModuleBackendDecision decision;
    decision.allow_sumh = allow_sumh;
    decision.global_mode = config.mount_backend;
    const auto configured = modes.find(m.id);
    if (configured != modes.end() && !configured->second.empty()) {
        decision.configured_mode = configured->second;
        decision.requested = configured->second;
        decision.source = "module";
    }
    if (!config.mount_backend.empty() && config.mount_backend != "auto") {
        decision.requested = config.mount_backend;
        decision.source = "global";
    }
    const auto finish = [&]() {
        decision.complete = decision.complete && decision.observation_error.empty();
        return decision;
    };
    decision.has_content = module_has_content(m, config, decision.observation_error);
    if (!decision.has_content) {
        decision.reason = decision.observation_error.empty() ? "no_managed_partition_tree"
                                                             : "content_observation_failed";
        return finish();
    }
    const auto& mode = decision.requested;
    // SUMH is available independently of the fallback backends. `auto`
    // preserves SUMHP's established OverlayFS -> Magic Mount path; an
    // explicit module/global "sumh" selection reaches the LKM backend.
    if (mode == "sumh") {
        if (allow_sumh) {
            const auto version = ::sumhp::sumh::version_info();
            decision.sumh.checked = true;
            decision.sumh.available = version.status == ::sumhp::sumh::Status::Available;
            decision.sumh.known = version.status != ::sumhp::sumh::Status::NotPresent;
            decision.sumh.error_number = version.last_errno;
            if (decision.sumh.available)
                decision.sumh.detail = "protocol_compatible";
            else
                decision.sumh.detail =
                    decision.sumh.known ? "protocol_mismatch" : "version_query_failed";
            if (!decision.sumh.known)
                decision.complete = false;
        }
        if (decision.sumh.available) {
            decision.selected = "sumh";
            decision.reason = "explicit_sumh";
            return finish();
        }
        if (!allow_sumh)
            decision.reason = "sumh_not_allowed_for_boot_owner";
        else
            decision.reason =
                decision.sumh.known ? "sumh_protocol_mismatch" : "sumh_readiness_unknown";
        decision.used_fallback = true;
    } else if (mode == "none") {
        decision.reason = "explicit_none";
        return finish();
    } else if (mode == "magic" && config.magic_mount_enabled) {
        decision.selected = "magic";
        decision.reason = "explicit_magic";
        return finish();
    } else {
        decision.used_fallback = mode != "auto" && mode != "overlay";
        if (mode == "auto")
            decision.reason = "automatic_fallback_order";
        else if (mode == "magic")
            decision.reason = "magic_disabled";
        else
            decision.reason = mode == "overlay" ? "explicit_overlay" : "unknown_requested_mode";
    }
    decision.selected = fallback_backend(m, config, decision);
    if (mode == "overlay" && decision.selected != "overlay") {
        decision.used_fallback = true;
        if (!config.overlayfs_enabled)
            decision.reason = "overlay_disabled";
        else if (!decision.overlay.known)
            decision.reason = "overlay_readiness_unknown";
        else if (!decision.overlay.available)
            decision.reason = "overlay_not_registered";
        else
            decision.reason = !decision.observation_error.empty() ? "overlay_layout_unknown"
                                                                  : "partition_root_requires_magic";
    }
    return finish();
}

std::string resolve_module_backend(const ModuleEntry& m, const Config& config,
                                   const ModuleModeMap& modes, bool allow_sumh) {
    const auto decision = resolve_module_backend_decision(m, config, modes, allow_sumh);
    if (decision.requested == "overlay" && decision.used_fallback) {
        fsutil::mlog(
            "module=" + m.id + " cannot use leaf OverlayFS; selected backend=" + decision.selected,
            logging::Level::Warning);
    }
    return decision.selected;
}

bool validate_module_backend_rules(const std::string& requested, const std::string& selected,
                                   const std::vector<ModuleRule>& rules, std::string& error) {
    if (requested == "sumh" && selected != "sumh" &&
        std::any_of(rules.begin(), rules.end(),
                    [](const auto& rule) { return rule.mode == "hide" || rule.mode == "none"; })) {
        error = "requires SUMH for hidden/excluded paths; refusing fallback backend " + selected;
        return false;
    }
    error.clear();
    return true;
}

MountReport mount_all_enabled(const Config& config) {
    MountReport report;
    fsutil::mlog("mount plan begin config=" + runtime_config_file().string());
    ModuleModeMap modes;
    ModuleRuleMap rules;
    if (!load_module_configuration(modes, rules, report.detail)) {
        fsutil::mlog(report.detail, logging::Level::Error);
        return report;
    }

    std::vector<ModuleEntry> modules;
    if (!enumerate_mountable_modules_checked(modules, report.detail)) {
        fsutil::mlog(report.detail, logging::Level::Error);
        return report;
    }
    const bool manage_sumh = ::sumhp::sumh::is_available();
    report.modules = static_cast<int>(modules.size());

    // Orchestrate per module: a global override (config.mount_backend != "auto")
    // forces every module; otherwise each module's saved mode (default
    // "auto") decides, with auto following OverlayFS -> Magic Mount -> none.
    // SUMH is selected only by an explicit module/global setting.
    std::vector<ModuleEntry> overlay_set;
    std::vector<ModuleEntry> magic_set;
    std::vector<ModuleEntry> sumh_set;
    for (const auto& m : modules) {
        const auto decision = resolve_module_backend_decision(m, config, modes);
        if (!decision.observation_error.empty()) {
            report.detail = decision.observation_error;
            fsutil::mlog(report.detail, logging::Level::Error);
            return report;
        }
        const auto& mode = decision.selected;
        if (decision.requested == "overlay" && decision.used_fallback)
            fsutil::mlog("module=" + m.id + " cannot use leaf OverlayFS; selected backend=" + mode,
                         logging::Level::Warning);
        const auto path_rules = rules.find(m.id);
        if (path_rules != rules.end() &&
            !validate_module_backend_rules(decision.requested, mode, path_rules->second,
                                           report.detail)) {
            report.detail.insert(0, "module " + m.id + " ");
            fsutil::mlog(report.detail, logging::Level::Error);
            return report;
        }
        fsutil::mlog("selected module=" + m.id + " backend=" + mode);
        if (mode == "overlay") {
            overlay_set.push_back(m);
        } else if (mode == "magic") {
            magic_set.push_back(m);
        } else if (mode == "sumh") {
            sumh_set.push_back(m);
        }
        // "none" → skip
    }

    fsutil::mlog("orchestrator: overlay=" + std::to_string(overlay_set.size()) + " magic=" +
                 std::to_string(magic_set.size()) + " sumh=" + std::to_string(sumh_set.size()));

    if (manage_sumh && !fsutil::run_in_init_mount_ns(
                           [&]() {
                               sumh::ModuleMountPlan plan;
                               return sumh::prepare_modules(sumh_set, config, rules, plan,
                                                            report.detail);
                           },
                           report.detail)) {
        fsutil::mlog("sumh: mount preflight failed: " + report.detail, logging::Level::Error);
        return report;
    }

    // Refuse before the boot claim or any backend mutation if a previous
    // legacy module rebuild is unresolved. The executor checks again locked.
    if (manage_sumh && !sumh::module_rebuild_allowed(report.detail)) {
        fsutil::mlog(report.detail, logging::Level::Error);
        return report;
    }

    if (!claim_mount_orchestrator()) {
        report.detail = "module mount-all already ran this boot or its state could not be recorded";
        fsutil::mlog(report.detail, logging::Level::Warning);
        return report;
    }
    sumh::invalidate_active_state();
    sumh::clear_replayable_mappings();

    // Bootloop protection: refuse to mount if previous boots never confirmed
    // completion, so a bad mount cannot brick boot. The counter is bumped here
    // and cleared by recovery_boot_completed() once boot_completed is reached.
    bool recovery_disabled = true;
    std::string recovery_error;
    const bool recovery_ok = recovery_begin_mount(recovery_disabled, recovery_error);
    if (!recovery_ok || recovery_disabled) {
        bool cleanup_ok = true;
        if (manage_sumh) {
            std::string error;
            cleanup_ok = sumh::reset_runtime_state(error);
            if (!cleanup_ok) {
                fsutil::mlog("sumh: recovery cleanup failed: " + error, logging::Level::Error);
            }
        }
        report.ok = recovery_ok && cleanup_ok;
        report.detail =
            recovery_ok ? recovery_error : "automatic mounting stopped: " + recovery_error;
        if (!cleanup_ok)
            report.detail += "; SUMH cleanup failed (see controller log)";
        fsutil::mlog(report.detail, report.ok ? logging::Level::Warning : logging::Level::Error);
        return report;
    }

    bool non_sumh_mounts_ok = true;
    bool sumh_rules_ok = true;
    bool xattrs_ok = true;
    if (!overlay_set.empty() || !magic_set.empty()) {
        non_sumh_mounts_ok = fsutil::run_in_init_mount_ns([&]() {
            bool r = true;
            // Preserve backend layering: OverlayFS is applied above Magic Mount.
            if (!magic_set.empty()) {
                r = magic::mount_modules(magic_set, config) && r;
            }
            if (!overlay_set.empty()) {
                r = overlay::mount_modules(overlay_set, config) && r;
            }
            return r;
        });
    }
    if (manage_sumh) {
        std::string error;
        sumh::ModuleRebuildOutcome outcome = sumh::ModuleRebuildOutcome::Unchanged;
        sumh_rules_ok = sumh::rebuild_modules_in_init_ns(sumh_set, config, rules, outcome, error);
        if (!sumh_rules_ok)
            fsutil::mlog("sumh: rebuild failed: " + error, logging::Level::Error);
        // The native reset also clears OverlayFS attribute-hiding state. The
        // fallback mounts above are already attached, so rebind their markers.
        if (sumh_rules_ok && !fsutil::run_in_init_mount_ns(
                                 [&]() { return overlay::restore_xattr_hiding(config); })) {
            xattrs_ok = false;
            fsutil::mlog("sumh: overlay attribute restore failed; module mappings retained",
                         logging::Level::Error);
        }
        if (sumh_rules_ok && !sumh_set.empty() && !sumh::record_replayable_mappings(sumh_set)) {
            sumh_rules_ok = false;
            fsutil::mlog(
                "sumh: rules acknowledged, boot mapping marker unconfirmed; mappings retained",
                logging::Level::Error);
        }
        if (!sumh_rules_ok || sumh_set.empty()) {
            sumh::clear_replayable_mappings();
        }
    } else {
        sumh::clear_replayable_mappings();
    }

    bool features_ok = true;
    bool maps_ok = true;
    if (manage_sumh && sumh_rules_ok) {
        features_ok = apply_sumh_features(config, maps_ok);
    } else if (manage_sumh) {
        features_ok = false;
    }
    const bool sumh_ok = features_ok && sumh_rules_ok;
    // Independent feature failures never erase successfully committed mappings.
    const bool ok = non_sumh_mounts_ok && xattrs_ok && features_ok && sumh_ok;

    report.backend = "hybrid(overlay=" + std::to_string(overlay_set.size()) +
                     ",magic=" + std::to_string(magic_set.size()) +
                     ",sumh=" + std::to_string(sumh_set.size()) + ")";
    report.ok = ok;
    report.mounts = static_cast<int>(magic::active_mounts(config).size() +
                                     overlay::active_mounts(config).size());
    report.detail = ok ? "ok" : "some backends reported errors (see controller log)";
    fsutil::mlog("mount plan complete backend=" + report.backend + " modules=" +
                     std::to_string(report.modules) + " result=" + (ok ? "ok" : "failed"),
                 ok ? logging::Level::Info : logging::Level::Error);
    return report;
}

bool unmount_all(const Config& config) {
    // Unwind the hybrid plan from its topmost backend.
    return fsutil::run_in_init_mount_ns([&]() {
        bool r = true;
        r = sumh::unmount_all(config) && r;
        r = overlay::unmount_all(config) && r;
        r = magic::unmount_all(config) && r;
        r = storage::teardown_shared(config) && r;
        return r;
    });
}

bool refresh_sumh_modules(const Config& config, bool* mappings_applied,
                          sumh::ModuleRebuildOutcome* outcome) {
    if (mappings_applied)
        *mappings_applied = false;
    if (outcome)
        *outcome = sumh::ModuleRebuildOutcome::Unchanged;
    if (!sumh_usable()) {
        return false;
    }
    const auto replayable = sumh::replayable_module_ids();
    if (replayable.empty()) {
        fsutil::mlog("sumh: refusing post-boot mapping replay without a "
                     "SUMH-owned boot plan",
                     logging::Level::Error);
        return false;
    }
    const std::set<std::string> replayable_set(replayable.begin(), replayable.end());
    std::vector<ModuleEntry> all;
    std::string error;
    if (!enumerate_mountable_modules_checked(all, error)) {
        fsutil::mlog(error, logging::Level::Error);
        return false;
    }
    std::vector<ModuleEntry> selected;
    for (const auto& module : all) {
        if (replayable_set.count(module.id) != 0) {
            selected.push_back(module);
        }
    }
    ModuleModeMap modes;
    ModuleRuleMap rules;
    if (!load_module_configuration(modes, rules, error)) {
        fsutil::mlog(error, logging::Level::Error);
        return false;
    }
    sumh::ModuleRebuildOutcome rebuilt = sumh::ModuleRebuildOutcome::Unchanged;
    const bool mount_ok = sumh::rebuild_modules_in_init_ns(selected, config, rules, rebuilt, error);
    if (outcome)
        *outcome = rebuilt;
    if (mappings_applied)
        *mappings_applied = rebuilt == sumh::ModuleRebuildOutcome::Applied;
    if (!mount_ok) {
        fsutil::mlog("sumh: refresh failed: " + error, logging::Level::Error);
        return false;
    }
    const bool xattrs_ok =
        fsutil::run_in_init_mount_ns([&]() { return overlay::restore_xattr_hiding(config); });
    if (!xattrs_ok)
        fsutil::mlog("sumh: overlay attribute restore failed; module mappings retained",
                     logging::Level::Error);
    bool maps_ok = true;
    const bool features_ok = apply_sumh_features(config, maps_ok);
    if (mappings_applied)
        *mappings_applied = true;
    if (!sumh::restore_persisted_hide_rules(error, RestoreTrigger::UserRequested)) {
        fsutil::mlog("sumh: module mappings applied; user hide restore failed: " + error,
                     logging::Level::Error);
        return false;
    }
    return xattrs_ok && features_ok;
}

}  // namespace sumhp::mount
