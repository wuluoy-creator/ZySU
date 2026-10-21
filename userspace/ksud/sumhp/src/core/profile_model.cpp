#include "../../../../common/utf8.hpp"
#include "core/config_document.hpp"
#include "core/profile.hpp"
#include "core/user_hide_store.hpp"
#include "mount/backend.hpp"
#include "sumhp/config.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace sumhp::profiles {
namespace {
bool fields(const JsonValue& value, const std::set<std::string>& expected) {
    if (!value.is_object() || value.o.size() != expected.size())
        return false;
    return std::all_of(value.o.begin(), value.o.end(),
                       [&](const auto& item) { return expected.count(item.first) != 0; });
}
bool text(const JsonValue& value, std::size_t maximum, bool nonempty = false) {
    return value.is_string() && (!nonempty || !value.s.empty()) && value.s.size() <= maximum &&
           value.s.find('\0') == std::string::npos && ksud::text::valid_utf8(value.s);
}
bool strict(const std::string& bytes, JsonValue& output, std::string& error,
            std::size_t maximum = PROFILE_MAX_BYTES) {
    if (bytes.size() > maximum || !ksud::text::valid_utf8(bytes)) {
        error = "profile input exceeds capacity or contains invalid UTF-8";
        return false;
    }
    return json::Parser(bytes, true).parse(output, error);
}
bool module_mode(const std::string& mode) {
    return mode == "auto" || mode == "sumh" || mode == "overlay" || mode == "magic" ||
           mode == "none";
}
JsonValue rules_json(const std::vector<mount::ModuleRule>& rules) {
    auto result = JsonValue::array();
    for (const auto& rule : rules) {
        auto row = JsonValue::object();
        row.o["path"] = rule.path;
        row.o["mode"] = rule.mode;
        result.a.push_back(std::move(row));
    }
    return result;
}
JsonValue settings_defaults() {
    auto defaults = json::parse(default_config_json());
    defaults.o.erase("schema_version");
    defaults.o.erase("maps_rules");
    defaults.o.erase("enable_maps_spoof");
    return defaults;
}
bool same(const JsonValue& left, const JsonValue& right) {
    return stringify_json(left) == stringify_json(right);
}
using Entries = std::map<std::string, JsonValue>;
Entries keyed(const JsonValue& array, const char* key) {
    Entries entries;
    for (const auto& row : array.a)
        entries.emplace(row.o.at(key).s, row);
    return entries;
}
JsonValue diff_entries(const Entries& current, const Entries& target) {
    auto result = JsonValue::object();
    result.o["changes"] = JsonValue::array();
    std::set<std::string> keys;
    for (const auto& [key, unused] : current)
        keys.insert(key);
    for (const auto& [key, unused] : target)
        keys.insert(key);
    int added = 0;
    int removed = 0;
    int changed = 0;
    int unchanged = 0;
    for (const auto& key : keys) {
        const auto before = current.find(key);
        const auto after = target.find(key);
        if (before != current.end() && after != target.end() &&
            same(before->second, after->second)) {
            ++unchanged;
            continue;
        }
        auto row = JsonValue::object();
        row.o["key"] = key;
        row.o["before"] = before == current.end() ? JsonValue() : before->second;
        row.o["after"] = after == target.end() ? JsonValue() : after->second;
        if (before == current.end()) {
            row.o["kind"] = "add";
            ++added;
        } else if (after == target.end()) {
            row.o["kind"] = "remove";
            ++removed;
        } else {
            row.o["kind"] = "change";
            ++changed;
        }
        result.o["changes"].a.push_back(std::move(row));
    }
    auto counts = JsonValue::object();
    counts.o["added"] = added;
    counts.o["removed"] = removed;
    counts.o["changed"] = changed;
    counts.o["unchanged"] = unchanged;
    result.o["counts"] = std::move(counts);
    return result;
}
}  // namespace

bool valid_profile_id(const std::string& id) {
    const auto initial = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    return !id.empty() && id.size() <= 64 && initial(static_cast<unsigned char>(id.front())) &&
           std::all_of(id.begin(), id.end(),
                       [&](unsigned char c) { return initial(c) || c == '_' || c == '-'; });
}

bool parse_profile(const std::string& bytes, JsonValue& output, std::string& error) {
    const auto fail = [&](const char* reason) {
        error = reason;
        return false;
    };
    JsonValue root;
    if (!strict(bytes, root, error))
        return false;
    if (!fields(root, {"schema_version", "kind", "name", "note", "resources"}) ||
        !root.o.at("schema_version").is_number() || root.o.at("schema_version").n != 1 ||
        !root.o.at("kind").is_string() || root.o.at("kind").s != "sumh.profile" ||
        !text(root.o.at("name"), 128, true) || !text(root.o.at("note"), 1024))
        return fail("invalid or unsupported profile format/name/note");
    auto& resources = root.o.at("resources");
    if (!fields(resources, {"settings", "modules", "user_hide", "maps"}))
        return fail("profile requires exactly settings, modules, user_hide and maps resources");
    const auto defaults = settings_defaults();
    auto& settings = resources.o.at("settings");
    if (!settings.is_object() || settings.o.size() != defaults.o.size() ||
        stringify_json(settings).size() > 64UL * 1024UL)
        return fail("profile settings must contain every supported field within 64 KiB");
    for (const auto& [key, unused] : defaults.o)
        if (!settings.find(key))
            return fail("profile settings is missing a supported field");
    if (!validate_config_patch(settings, error))
        return false;
    auto& maps = resources.o.at("maps");
    if (!fields(maps, {"enabled", "rules"}) || !maps.o.at("enabled").is_bool())
        return fail("profile maps requires enabled and rules");
    auto config = settings;
    config.o["schema_version"] = 1;
    config.o["enable_maps_spoof"] = maps.o.at("enabled");
    config.o["maps_rules"] = maps.o.at("rules");
    Config validated_config;
    if (!parse_config_json(stringify_json(config), validated_config, error))
        return false;
    auto& modules = resources.o.at("modules");
    if (!modules.is_array() || modules.a.size() > 256)
        return fail("profile supports at most 256 module configurations");
    std::set<std::string> ids;
    std::size_t total_rules = 0;
    for (const auto& module : modules.a) {
        if (!fields(module, {"id", "mode", "rules"}) || !text(module.o.at("id"), 255, true) ||
            !mount::valid_module_id(module.o.at("id").s) ||
            !ids.insert(module.o.at("id").s).second || !module.o.at("mode").is_string() ||
            !module_mode(module.o.at("mode").s))
            return fail("invalid or duplicate profile module identity/mode");
        std::vector<mount::ModuleRule> parsed;
        if (!mount::parse_module_rules(stringify_json(module.o.at("rules")), parsed, error))
            return false;
        total_rules += parsed.size();
        if (total_rules > 4096)
            return fail("profile supports at most 4096 total module path rules");
    }
    std::sort(modules.a.begin(), modules.a.end(),
              [](const JsonValue& left, const JsonValue& right) {
                  return left.o.at("id").s < right.o.at("id").s;
              });
    hide::Document hidden;
    const auto& hide_input = resources.o.at("user_hide");
    if (!hide_input.is_object())
        return fail("profile user_hide requires a versioned definition object");
    if (!hide::parse_document(stringify_json(hide_input), hidden, error))
        return false;
    resources.o["user_hide"] = hide::document_json(hidden);
    if (stringify_json(root).size() + 1 > PROFILE_MAX_BYTES)
        return fail("canonical profile exceeds 8 MiB capacity");
    output = std::move(root);
    error.clear();
    return true;
}

bool capture_profile(const std::string& config, const std::string& modes, const std::string& rules,
                     const std::string& hidden, const std::string& name, const std::string& note,
                     JsonValue& output, std::string& error) {
    JsonValue config_input;
    JsonValue modes_input;
    JsonValue rules_input;
    JsonValue hide_input;
    if (!strict(config, config_input, error, 4UL * 1024UL * 1024UL) ||
        !strict(modes, modes_input, error, 4UL * 1024UL * 1024UL) ||
        !strict(rules, rules_input, error, 4UL * 1024UL * 1024UL) ||
        !strict(hidden, hide_input, error, hide::HIDE_MAX_BYTES))
        return false;
    ConfigDocument document;
    Config validated;
    if (!parse_config_document(config, document, error) ||
        !config_document_is_writable(document, error) ||
        !parse_config_json(config, validated, error))
        return false;
    auto full_config = json::parse(default_config_json());
    for (const auto& [key, value] : document.fields.o)
        full_config.o[key] = value;
    auto maps_resource = JsonValue::object();
    maps_resource.o["enabled"] = full_config.o.at("enable_maps_spoof");
    maps_resource.o["rules"] = full_config.o.at("maps_rules");
    full_config.o.erase("schema_version");
    full_config.o.erase("maps_rules");
    full_config.o.erase("enable_maps_spoof");
    mount::ModuleModeMap parsed_modes;
    mount::ModuleRuleMap parsed_rules;
    if (!mount::parse_module_configuration_documents(modes, rules, parsed_modes, parsed_rules,
                                                     error))
        return false;
    std::set<std::string> ids;
    for (const auto& [id, unused] : modes_input.o)
        ids.insert(id);
    for (const auto& [id, unused] : rules_input.o)
        ids.insert(id);
    auto modules = JsonValue::array();
    for (const auto& id : ids) {
        auto row = JsonValue::object();
        row.o["id"] = id;
        const auto mode = parsed_modes.find(id);
        row.o["mode"] = mode == parsed_modes.end() ? "auto" : mode->second;
        const auto list = parsed_rules.find(id);
        row.o["rules"] = list == parsed_rules.end() ? JsonValue::array() : rules_json(list->second);
        modules.a.push_back(std::move(row));
    }
    hide::Document definitions;
    if (!hide::parse_document(hidden, definitions, error))
        return false;
    auto root = JsonValue::object();
    root.o["schema_version"] = 1;
    root.o["kind"] = "sumh.profile";
    root.o["name"] = name;
    root.o["note"] = note;
    auto resources = JsonValue::object();
    resources.o["settings"] = std::move(full_config);
    resources.o["modules"] = std::move(modules);
    resources.o["maps"] = std::move(maps_resource);
    resources.o["user_hide"] = hide::document_json(definitions);
    root.o["resources"] = std::move(resources);
    // The same strict new-write validators apply to captured legacy content.
    // Rejection is explicit: never repair, truncate or normalize module rules.
    return parse_profile(stringify_json(root), output, error);
}

JsonValue diff_profiles(const JsonValue& current, const JsonValue& target,
                        const std::vector<std::string>& scopes) {
    auto result = JsonValue::object();
    result.o["resources"] = JsonValue::object();
    const auto& before = current.o.at("resources");
    const auto& after = target.o.at("resources");
    for (const auto& scope : scopes) {
        if (scope == "settings") {
            result.o["resources"].o[scope] =
                diff_entries(before.o.at(scope).o, after.o.at(scope).o);
        } else if (scope == "modules") {
            result.o["resources"].o[scope] =
                diff_entries(keyed(before.o.at(scope), "id"), keyed(after.o.at(scope), "id"));
        } else if (scope == "maps") {
            auto maps = diff_entries(keyed(before.o.at(scope).o.at("rules"), "target_path"),
                                     keyed(after.o.at(scope).o.at("rules"), "target_path"));
            maps.o["enabled"] = diff_entries({{"enabled", before.o.at(scope).o.at("enabled")}},
                                             {{"enabled", after.o.at(scope).o.at("enabled")}});
            result.o["resources"].o[scope] = std::move(maps);
        } else if (scope == "user_hide") {
            const auto& prior = before.o.at(scope).o.at("rules");
            const auto& next = after.o.at(scope).o.at("rules");
            auto hidden = diff_entries(keyed(prior, "id"), keyed(next, "id"));
            hidden.o["identity_conflicts"] = JsonValue::array();
            const auto prior_ids = keyed(prior, "id");
            const auto next_ids = keyed(next, "id");
            for (const auto& [id, row] : prior_ids) {
                const auto found = next_ids.find(id);
                if (found == next_ids.end() || row.o.at("path").s == found->second.o.at("path").s)
                    continue;
                auto conflict = JsonValue::object();
                conflict.o["kind"] = "same_id_different_path";
                conflict.o["id"] = id;
                conflict.o["before_path"] = row.o.at("path");
                conflict.o["after_path"] = found->second.o.at("path");
                hidden.o["identity_conflicts"].a.push_back(std::move(conflict));
            }
            const auto prior_paths = keyed(prior, "path");
            const auto next_paths = keyed(next, "path");
            for (const auto& [path, row] : prior_paths) {
                const auto found = next_paths.find(path);
                if (found == next_paths.end() || row.o.at("id").s == found->second.o.at("id").s)
                    continue;
                auto conflict = JsonValue::object();
                conflict.o["kind"] = "same_path_different_id";
                conflict.o["path"] = path;
                conflict.o["before_id"] = row.o.at("id");
                conflict.o["after_id"] = found->second.o.at("id");
                hidden.o["identity_conflicts"].a.push_back(std::move(conflict));
            }
            result.o["resources"].o[scope] = std::move(hidden);
        }
    }
    return result;
}
}  // namespace sumhp::profiles
