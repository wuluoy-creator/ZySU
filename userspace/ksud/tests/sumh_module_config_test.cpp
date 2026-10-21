// Production module persistence and CLI dispatch with storage fault injection.
#include "core/json.hpp"
#include "mount/backend.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {
std::map<std::string, std::string> files;
std::string unreadable;
bool write_ok = true;
bool durability_ok = true;
bool lock_ok = true;
int writes = 0;
int reads = 0;
int locks = 0;
const std::string mode_file = "module_mode.json";
const std::string rules_file = "module_rules.json";
}  // namespace

namespace ksud {
std::optional<std::string> read_file(const std::string& path) {
    ++reads;
    const auto name = std::filesystem::path(path).filename().string();
    if (name == unreadable) {
        errno = EACCES;
        return std::nullopt;
    }
    const auto it = files.find(name);
    errno = it == files.end() ? ENOENT : 0;
    return it == files.end() ? std::nullopt : std::optional<std::string>(it->second);
}
bool write_file_durable(const std::filesystem::path& path, const std::string& content,
                        std::string& error) {
    ++writes;
    if (!write_ok) {
        errno = ENOSPC;
        error = "file not replaced: ENOSPC";
        return false;
    }
    files[path.filename().string()] = content;
    if (!durability_ok) {
        errno = EIO;
        error = "file replaced but durability is unconfirmed: EIO";
        return false;
    }
    return true;
}
}  // namespace ksud

namespace sumhp {
std::filesystem::path runtime_data_dir() {
    return "/mock/runtime";
}
namespace logging {
enum class Level { Error };
}
namespace mount {
namespace fsutil {
void mlog(const std::string&, logging::Level) {}
}  // namespace fsutil
class ModuleConfigLock {
public:
    bool acquire(std::string& error) {
        ++locks;
        if (!lock_ok)
            error = "injected lock error";
        return lock_ok;
    }
};
#include "sumh_module_config_under_test.inc"
}  // namespace mount

int run_module_update(const std::vector<std::string>& args) {
    const auto sub = args.at(1);
#include "sumh_module_commands_under_test.inc"
    return 1;
}
}  // namespace sumhp

int main() {
    using namespace sumhp;
    using namespace sumhp::mount;
    std::string error;
    ModuleModeMap modes;
    ModuleRuleMap rules;
    assert(load_module_configuration(modes, rules, error));
    assert(modes.empty() && rules.empty());

    // First commit imports both legacy files and commits mode plus replacement
    // rules with one write; unrelated modules and literal path text survive.
    files[mode_file] = R"({"selected":"magic","other":"overlay","mode_only":"none"})";
    files[rules_file] =
        R"({"selected":[{"path":"/old","mode":"sumh"}],"other":[{"path":"/vendor/keep ","mode":"none"}],"rules_only":[{"path":"/literal\tpath","mode":"hide"}]})";
    const auto legacy_modes = files[mode_file];
    assert(run_module_update({"module", "configure", "selected", "sumh",
                              R"([{"path":"/new path ","mode":"hide"}])"}) == 0);
    assert(writes == 1 && files[mode_file] == legacy_modes);
    assert(load_module_configuration(modes, rules, error));
    assert(modes.at("selected") == "sumh" && modes.at("other") == "overlay");
    assert(modes.at("mode_only") == "none" && modes.count("rules_only") == 0);
    assert(rules.at("selected").size() == 1 && rules.at("selected")[0].path == "/new path ");
    assert(rules.at("other")[0].path == "/vendor/keep ");
    assert(rules.at("rules_only")[0].path == "/literal\tpath");
    JsonValue document;
    assert(parse_json(files[rules_file], document, error));
    assert(document.o.size() == 4 && document.find("mode_only")->find("rules")->a.empty());

    // Explicit auto is retained even if the old file still requests magic.
    assert(run_module_update({"module", "configure", "selected", "auto", "[]"}) == 0);
    assert(load_module_configuration(modes, rules, error));
    assert(modes.count("selected") == 0 && rules.count("selected") == 0);
    assert(parse_json(files[rules_file], document, error));
    assert(document.find("selected")->find("mode")->s == "auto");
    assert(run_module_update({"module", "add-rule", "selected", "/kept ", "hide"}) == 0);
    assert(run_module_update({"module", "set-mode", "selected", "sumh"}) == 0);
    assert(run_module_update({"module", "add-rule", "selected", "/kept ", "none"}) == 0);
    assert(load_module_configuration(modes, rules, error));
    assert(modes.at("selected") == "sumh" && rules.at("selected").size() == 1);
    assert(rules.at("selected")[0].mode == "none");
    assert(run_module_update({"module", "remove-rule", "selected", "/kept "}) == 0);
    assert(load_module_configuration(modes, rules, error));
    assert(modes.at("selected") == "sumh" && rules.count("selected") == 0);

    // Save, lock, parse and read failures preserve the complete previous file.
    const auto saved = files;
    write_ok = false;
    assert(!configure_module("selected", "none", {{"/change", "hide"}}, error));
    assert(!error.empty() && files == saved);
    write_ok = true;
    durability_ok = false;
    assert(!configure_module("selected", "none", {{"/change", "hide"}}, error));
    assert(error.find("file replaced but durability is unconfirmed") != std::string::npos);
    assert(files != saved);  // Never report a post-rename failure as unchanged.
    durability_ok = true;
    files = saved;
    lock_ok = false;
    assert(!set_module_mode("selected", "none", error));
    assert(files == saved);
    lock_ok = true;
    unreadable = mode_file;
    assert(!configure_module("selected", "none", {}, error) && files == saved);
    unreadable.clear();
    for (const auto* malformed :
         {"", "[]", R"({"selected":null})", R"({"selected":{"mode":"hide","rules":[]}})",
          R"({"selected":{"mode":"sumh","rules":[{"path":"/bad\npath","mode":"hide"}]}})"}) {
        files[rules_file] = malformed;
        const auto before = files;
        assert(!configure_module("selected", "sumh", {}, error));
        assert(!error.empty() && files == before);
        modes = {{"untouched", "none"}};
        rules = {{"untouched", {{"/kept", "hide"}}}};
        assert(!load_module_configuration(modes, rules, error));
        assert(modes.count("untouched") && rules.count("untouched"));
    }
    files = saved;

    // Previously permitted prefixes, duplicates and oversized arrays remain
    // readable and untouched when configuring another module. They can be
    // repaired as a complete configure operation or removed incrementally.
    std::vector<ModuleRule> legacy_rules{
        {"/system/lib/", "hide"}, {"/same", "none"}, {"/same", "hide"}};
    for (int index = 0; index < 257; ++index)
        legacy_rules.push_back({"/" + std::to_string(index) + "/" + std::string(230, 'x'), "none"});
    JsonValue legacy = JsonValue::object();
    legacy.o["old"] = module_rules_json(legacy_rules);
    files[rules_file] = stringify_json(legacy);
    files[mode_file] = R"({"old":"sumh"})";
    assert(load_module_configuration(modes, rules, error));
    assert(rules.at("old").size() == 260 && rules.at("old")[0].path == "/system/lib/");
    assert(configure_module("new", "sumh", {{"/safe", "hide"}}, error));
    assert(load_module_configuration(modes, rules, error));
    assert(stringify_json(module_rules_json(rules.at("old"))) ==
           stringify_json(module_rules_json(legacy_rules)));
    assert(set_module_mode("old", "magic", error));
    assert(remove_module_rule("old", "/system/lib/", error));
    assert(load_module_configuration(modes, rules, error));
    assert(rules.at("old").size() == 259 && modes.at("old") == "magic");
    assert(!configure_module("old", "sumh", rules.at("old"), error));
    assert(configure_module("old", "sumh", {{"/system/lib", "hide"}}, error));
    assert(load_module_configuration(modes, rules, error));
    assert(rules.at("old").size() == 1 && rules.at("new")[0].path == "/safe");
    files = saved;

    // Validate exact bytes and components, without normalizing literal spaces.
    for (const auto* bad : {"", "relative", "//", "/a//b", "/a/", "/a/./b", "/a/../b", "/.", "/..",
                            "/line\nbreak", "/carriage\rreturn"})
        assert(!valid_module_rule_path(bad));
    assert(!valid_module_rule_path(std::string("/nul\0suffix", 11)));
    assert(valid_module_rule_path("/") && valid_module_rule_path("/space /last "));
    assert(valid_module_rule_path("/" + std::string(4094, 'x')));
    assert(!valid_module_rule_path("/" + std::string(4095, 'x')));
    assert(!valid_module_id(".") && !valid_module_id("..") && !valid_module_id("a/b"));
    assert(valid_module_id("module.name-1"));
    std::vector<ModuleRule> parsed{{"/unchanged", "hide"}};
    for (const auto* bad : {"[] garbage", "{}", R"([{"path":"/x","mode":"unknown"}])",
                            R"([{"path":"/x","mode":"hide","extra":1}])",
                            R"([{"path":"/same","mode":"hide"},{"path":"/same","mode":"none"}])"}) {
        assert(!parse_module_rules(bad, parsed, error));
        assert(parsed.size() == 1 && parsed[0].path == "/unchanged");
    }
    assert(!configure_module("selected", "hide", {}, error) && files == saved);
    std::vector<ModuleRule> many;
    for (int index = 0; index < 256; ++index)
        many.push_back({"/" + std::to_string(index), "hide"});
    assert(validate_module_rules(many, error));
    many.push_back({"/extra", "hide"});
    assert(!configure_module("selected", "sumh", many, error) && files == saved);
    many.clear();
    for (int index = 0; index < 30; ++index)
        many.push_back({"/" + std::to_string(index) + "/" + std::string(2000, 'x'), "hide"});
    assert(!configure_module("selected", "sumh", many, error) && files == saved);
    // Cross-language budget includes both quoting passes and Android '/' escaping.
    const std::vector<ModuleRule> sample{{"/system/a", "hide"}};
    assert(json_quote(stringify_json(module_rules_json(sample))).size() + 6 == 53);
    assert(run_module_update({"module", "configure", "selected", "sumh"}) == 1);
    assert(files == saved);

    // Preview parses the exact same records without acquiring a lock, reading
    // files or mutating them. Canonical auto suppresses an old legacy mode.
    const std::vector<std::pair<std::string, std::string>> documents{
        {"{}", "{}"},
        {R"({"same":"magic","legacy":"overlay"})",
         R"({"same":{"mode":"auto","rules":[]},"legacy":[{"path":"/keep ","mode":"none"}]})"},
        {R"({"same":null})", R"({"same":{"mode":"sumh","rules":[]}})"},
        {R"({"old":"sumh"})", stringify_json(legacy)},
        {"[]", "{}"},
        {"{}", "null"},
        {R"({"bad":"invalid"})", "{}"},
        {"{}", R"({"new":{"mode":"sumh","rules":[],"future":true}})"},
        {"{}", R"({"new":[{"path":"/bad\npath","mode":"hide"}]})"},
    };
    for (const auto& [mode_json, rule_json] : documents) {
        files = {{mode_file, mode_json}, {rules_file, rule_json}};
        ModuleModeMap loaded_modes{{"untouched", "none"}};
        ModuleRuleMap loaded_rules{{"untouched", {{"/old", "hide"}}}};
        ModuleModeMap pure_modes = loaded_modes;
        ModuleRuleMap pure_rules = loaded_rules;
        std::string loaded_error;
        const bool loaded = load_module_configuration(loaded_modes, loaded_rules, loaded_error);
        const auto original_files = files;
        const int old_reads = reads;
        const int old_locks = locks;
        const int old_writes = writes;
        lock_ok = false;
        unreadable = mode_file;
        const bool pure = parse_module_configuration_documents(mode_json, rule_json, pure_modes,
                                                               pure_rules, error);
        lock_ok = true;
        unreadable.clear();
        assert(pure == loaded && error == loaded_error && pure_modes == loaded_modes);
        assert(pure_rules.size() == loaded_rules.size());
        for (const auto& [id, entries] : loaded_rules)
            assert(stringify_json(module_rules_json(entries)) ==
                   stringify_json(module_rules_json(pure_rules.at(id))));
        assert(reads == old_reads && locks == old_locks && writes == old_writes &&
               files == original_files);
    }
    files = saved;
    std::cout << "sumh_module_config_test: all checks passed\n";
}
