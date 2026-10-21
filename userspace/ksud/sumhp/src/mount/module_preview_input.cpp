#include <algorithm>
#include <set>
#include "core/rule_inventory.hpp"
#include "mount/module_preview.hpp"

namespace sumhp::mount {
namespace {
bool keys(const JsonValue& value, const std::set<std::string>& allowed) {
    if (!value.is_object())
        return false;
    return std::all_of(value.o.begin(), value.o.end(),
                       [&](const auto& entry) { return allowed.count(entry.first); });
}
bool valid_mode(const std::string& mode) {
    return mode == "auto" || mode == "sumh" || mode == "none" || mode == "magic" ||
           mode == "overlay";
}
}  // namespace

bool is_module_preview_prefix(const std::vector<std::string>& args) {
    return args.size() >= 2 && args[0] == "module" && (args[1] == "plan" || args[1] == "explain");
}
bool parse_module_preview_arguments(const std::vector<std::string>& args,
                                    ModulePreviewRequest& request, std::string& error) {
    error = "usage: ksud sumhp module plan [--input <absolute-file|->] or module explain <path> "
            "[--input <absolute-file|->]";
    if (!is_module_preview_prefix(args))
        return false;
    ModulePreviewRequest parsed;
    std::size_t at = 2;
    if (args[1] == "explain") {
        if (at == args.size() || !valid_module_rule_path(args[at]) ||
            !inventory::valid_utf8(args[at]))
            return false;
        parsed.explain_path = args[at++];
    }
    if (at < args.size()) {
        if (args.size() != at + 2 || args[at] != "--input")
            return false;
        parsed.input_path = args[at + 1];
        if (parsed.input_path != "-" && (!valid_module_rule_path(parsed.input_path) ||
                                         !inventory::valid_utf8(parsed.input_path)))
            return false;
    }
    request = std::move(parsed);
    error.clear();
    return true;
}

bool apply_module_preview_input(const std::string& bytes, Config& config, ModuleModeMap& modes,
                                ModuleRuleMap& rules, std::vector<std::string>& changed_ids,
                                std::string& error) {
    error.clear();
    const auto fail = [&](const std::string& reason) {
        error = reason;
        return false;
    };
    if (bytes.size() > MODULE_PREVIEW_INPUT_BYTES || !inventory::valid_utf8(bytes))
        return fail("candidate exceeds 64 KiB or is not valid UTF-8");
    JsonValue input;
    // New candidate documents must have one unambiguous value per key.
    // The shared parser's default remains compatible with saved legacy JSON.
    if (!json::Parser(bytes, true).parse(input, error))
        return false;
    if (!keys(input, {"schema_version", "kind", "config", "modules"}))
        return fail("unknown candidate fields or non-object");
    const auto* version = input.find("schema_version");
    const auto* kind = input.find("kind");
    if (!version || !version->is_number() || version->n != 1 || !kind || !kind->is_string() ||
        kind->s != "sumh.module.plan.input")
        return fail("unsupported module plan input format");
    Config candidate = config;
    auto candidate_modes = modes;
    auto candidate_rules = rules;
    std::vector<std::string> ids;
    if (const auto* patch = input.find("config")) {
        if (!keys(*patch,
                  {"mount_backend", "overlayfs_enabled", "magic_mount_enabled", "partitions"}))
            return fail("candidate config contains fields outside module planning scope");
        if (!validate_config_patch(*patch, error))
            return false;
        if (const auto* item = patch->find("mount_backend"))
            candidate.mount_backend = item->s;
        if (const auto* item = patch->find("overlayfs_enabled"))
            candidate.overlayfs_enabled = item->b;
        if (const auto* item = patch->find("magic_mount_enabled"))
            candidate.magic_mount_enabled = item->b;
        if (const auto* item = patch->find("partitions")) {
            candidate.partitions.clear();
            for (const auto& part : item->a)
                candidate.partitions.push_back(part.s);
        }
    }
    if (const auto* modules = input.find("modules")) {
        if (!modules->is_array() || modules->a.size() > 256)
            return fail("candidate modules must be an array of at most 256 replacements");
        std::set<std::string> seen;
        for (const auto& module : modules->a) {
            if (!keys(module, {"id", "mode", "rules"}) || module.o.size() != 3)
                return fail("module replacement requires exactly id, mode and rules");
            const auto& id = module.o.at("id");
            const auto& mode = module.o.at("mode");
            if (!id.is_string() || !valid_module_id(id.s) || !seen.insert(id.s).second ||
                !mode.is_string() || !valid_mode(mode.s))
                return fail("invalid or duplicate module id/mode");
            std::vector<ModuleRule> parsed;
            if (!parse_module_rules(stringify_json(module.o.at("rules")), parsed, error))
                return false;
            candidate_modes[id.s] = mode.s;
            candidate_rules[id.s] = std::move(parsed);
            ids.push_back(id.s);
        }
    }
    config = std::move(candidate);
    modes = std::move(candidate_modes);
    rules = std::move(candidate_rules);
    changed_ids = std::move(ids);
    return true;
}
}  // namespace sumhp::mount
