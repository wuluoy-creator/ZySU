#include "core/migration.hpp"

#include "core/config_document.hpp"
#include "core/json.hpp"
#include "core/runtime.hpp"
#include "core/user_hide_store.hpp"
#include "utils.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>

namespace sumhp {
namespace {

bool migrate_backend_value(JsonValue& value) {
    // This old spelling is intentionally restricted to persisted upgrade input.
    if (!value.is_string() || value.s != "kasumi")
        return false;
    value.s = "sumh";
    return true;
}

bool migrate_work_path(JsonValue& value) {
    const std::string old_root = "/dev/kagami";
    if (!value.is_string() ||
        (value.s != old_root && value.s.compare(0, old_root.size() + 1, old_root + "/") != 0))
        return false;
    value.s.replace(0, old_root.size(), "/dev/sumhp");
    return true;
}

bool migrate_config_document(const std::string& name, JsonValue& root) {
    bool changed = false;
    if (name == "config.json") {
        // Versioned documents already use the modern contract. In particular,
        // never rewrite a future schema merely because it contains an old
        // spelling: only its owning version can define migration semantics.
        if (root.find("schema_version"))
            return false;
        // An unknown legacy field may carry values this parser cannot round
        // trip. Preserve the original document for an explicit conversion.
        ConfigDocument document;
        document.fields = root;
        std::string error;
        if (!config_document_is_writable(document, error))
            return false;
        if (auto it = root.o.find("mount_backend"); it != root.o.end())
            changed |= migrate_backend_value(it->second);
        for (const auto* key : {"work_dir", "mirror_dir"}) {
            if (auto it = root.o.find(key); it != root.o.end()) {
                if (std::string(key) == "mirror_dir" && it->second.is_string() &&
                    it->second.s == "/dev/kagami_mirror") {
                    it->second.s = "/dev/sumhp_mirror";
                    changed = true;
                } else {
                    changed |= migrate_work_path(it->second);
                }
            }
        }
    } else if (name == "module_mode.json") {
        for (auto& [id, value] : root.o)
            changed |= migrate_backend_value(value);
    } else if (name == "module_rules.json") {
        for (auto& [id, rules] : root.o) {
            JsonValue* entries = &rules;
            if (rules.is_object()) {
                if (auto it = rules.o.find("mode"); it != rules.o.end())
                    changed |= migrate_backend_value(it->second);
                const auto it = rules.o.find("rules");
                if (it == rules.o.end())
                    continue;
                entries = &it->second;
            }
            if (!entries->is_array())
                continue;
            for (auto& rule : entries->a) {
                if (rule.is_object()) {
                    if (auto it = rule.o.find("mode"); it != rule.o.end())
                        changed |= migrate_backend_value(it->second);
                }
            }
        }
    }
    return changed;
}

bool migration_file_exists(const std::filesystem::path& path, bool& exists, std::string& error) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        error = "inspect migration file " + path.string() + ": " + ec.message();
        return false;
    }
    exists = std::filesystem::exists(status);
    if (!exists)
        return true;
    if (!std::filesystem::is_regular_file(status)) {
        error = "not a regular migration file: " + path.string();
        return false;
    }
    const auto links = std::filesystem::hard_link_count(path, ec);
    if (ec || links != 1) {
        error = "not a private migration file: " + path.string();
        return false;
    }
    return prepare_private_file(path, error);
}

bool import_json_record(const std::filesystem::path& legacy_dir,
                        const std::filesystem::path& data_dir, const std::string& name,
                        bool legacy_exists, std::string& error) {
    const auto target = data_dir / name;
    bool target_exists = false;
    if (!migration_file_exists(target, target_exists, error))
        return false;
    bool source_exists = false;
    if (!target_exists && legacy_exists &&
        !migration_file_exists(legacy_dir / name, source_exists, error))
        return false;
    if (!target_exists && !source_exists)
        return true;
    const auto source = target_exists ? target : legacy_dir / name;
    const auto input = ksud::read_file(source.string());
    if (!input) {
        error = "read migration file " + source.string() + ": " + std::strerror(errno);
        return false;
    }
    JsonValue root;
    hide::Document hide_document;
    if (!parse_json(*input, root, error) ||
        (name == "user_hide_rules.json" ? !hide::parse_document(*input, hide_document, error)
                                        : !root.is_object())) {
        if (target_exists) {
            // Leave damaged current settings to the existing read/repair paths.
            // Blocking startup here would also block `config init` recovery.
            error.clear();
            return true;
        }
        error = "invalid migration file " + source.string() + ": " +
                (error.empty() ? "unexpected JSON root" : error);
        return false;
    }
    const bool changed = migrate_config_document(name, root);
    if (!changed && target_exists)
        return true;
    if (!ksud::write_file_durable(target, changed ? stringify_json(root, 2) + "\n" : *input,
                                  error)) {
        error = "write migration file: " + error;
        return false;
    }
    if (!target_exists) {
        std::error_code ec;
        std::filesystem::remove(source, ec);
        if (ec) {
            error = "remove imported migration file " + source.string() + ": " + ec.message();
            return false;
        }
        if (!sync_private_directory(legacy_dir, error))
            return false;
    }
    return true;
}

bool import_data_record(const std::filesystem::path& legacy_dir,
                        const std::filesystem::path& data_dir, const char* name,
                        std::string& error) {
    const auto target = data_dir / name;
    bool target_exists = false;
    if (!migration_file_exists(target, target_exists, error))
        return false;
    if (target_exists)
        return true;
    const auto source = legacy_dir / name;
    bool source_exists = false;
    if (!migration_file_exists(source, source_exists, error))
        return false;
    if (!source_exists)
        return true;
    std::error_code ec;
    std::filesystem::rename(source, target, ec);
    if (ec) {
        error = "move migration file " + source.string() + ": " + ec.message();
        return false;
    }
    return sync_private_directory(target.parent_path(), error) &&
           sync_private_directory(source.parent_path(), error);
}

}  // namespace

bool migrate_legacy_state(const std::filesystem::path& legacy_dir,
                          const std::filesystem::path& data_dir, std::string& error) {
    error.clear();
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(legacy_dir, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        error = "inspect legacy state: " + ec.message();
        return false;
    }
    const bool legacy_exists = std::filesystem::exists(status);
    if (legacy_exists &&
        (!std::filesystem::is_directory(status) || !prepare_private_directory(legacy_dir, error))) {
        if (error.empty())
            error = "not a private legacy state directory: " + legacy_dir.string();
        return false;
    }
    if (legacy_exists &&
        marker_matches_current_boot(legacy_dir / "run" / "mount_orchestrator_boot")) {
        error = "previous controller mounted modules during this boot; restart before migrating";
        return false;
    }
    for (const auto* name :
         {"config.json", "module_mode.json", "module_rules.json", "user_hide_rules.json"}) {
        if (!import_json_record(legacy_dir, data_dir, name, legacy_exists, error))
            return false;
    }
    if (!legacy_exists)
        return true;
    for (const auto* name : {"mirror.img", "mirror.erofs"}) {
        if (!import_data_record(legacy_dir, data_dir, name, error))
            return false;
    }
    const auto run_status = std::filesystem::symlink_status(legacy_dir / "run", ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        error = "inspect legacy recovery state: " + ec.message();
        return false;
    }
    if (std::filesystem::exists(run_status)) {
        if (!std::filesystem::is_directory(run_status)) {
            error = "not a private legacy recovery directory";
            return false;
        }
        // Preserve boot-loop protection; other run files describe old mounts
        // and sockets and must not become the new controller's live state.
        for (const auto* name : {"run/boot_attempts", "run/mount_disabled"}) {
            if (!import_data_record(legacy_dir, data_dir, name, error))
                return false;
        }
        std::filesystem::remove(legacy_dir / "run", ec);  // Removes only an empty directory.
    }
    std::filesystem::remove(legacy_dir, ec);  // Conflicting or unknown data stays intact.
    return true;
}

}  // namespace sumhp
