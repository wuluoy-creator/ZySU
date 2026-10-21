#include <algorithm>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include "core/config_document.hpp"
#include "core/profile.hpp"
#include "core/user_hide_store.hpp"
#include "mount/backend.hpp"
#include "sumhp/config.hpp"
#include "uapi/sumh.h"

namespace sumhp {
#include "profile_config_validators.inc"
namespace mount {
#include "profile_module_validators.inc"
}
namespace state {
// Deterministic test identity only. Production uses its real SHA256 provider.
std::string revision_for_bytes(const std::string& bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto c : bytes) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    std::ostringstream value;
    value << std::hex << std::setfill('0') << std::setw(16) << hash;
    return "sha256-v1:" + value.str() + std::string(48, '0');
}
}  // namespace state
}  // namespace sumhp
using namespace sumhp;
using namespace sumhp::profiles;
namespace {
int checks = 0;
void check(bool value, int line) {
    ++checks;
    if (!value) {
        std::cerr << "check " << checks << " failed at line " << line << "\n";
        std::abort();
    }
}
#define require(value) check((value), __LINE__)
JsonValue base() {
    JsonValue output;
    std::string error;
    require(capture_profile("{}", "{}", "{}", "[]", "默认方案 ", "备注\r\n", output, error));
    return output;
}
void accepted(const JsonValue& value) {
    JsonValue output;
    std::string error;
    require(parse_profile(stringify_json(value), output, error));
    require(error.empty());
}
void rejected_bytes(const std::string& bytes) {
    JsonValue output("sentinel");
    std::string error;
    const bool success = parse_profile(bytes, output, error);
    if (success || error.empty())
        std::cerr << "unexpected rejected result " << success << " input " << bytes.substr(0, 1400)
                  << "\n";
    require(!success);
    require(output.is_string() && output.s == "sentinel");
    require(!error.empty());
}
void rejected(const JsonValue& value) {
    rejected_bytes(stringify_json(value));
}
JsonValue module(const std::string& id, const std::string& mode = "auto") {
    auto row = JsonValue::object();
    row.o["id"] = id;
    row.o["mode"] = mode;
    row.o["rules"] = JsonValue::array();
    return row;
}
JsonValue rule(const std::string& path, const std::string& mode = "sumh") {
    auto row = JsonValue::object();
    row.o["path"] = path;
    row.o["mode"] = mode;
    return row;
}
JsonValue map_rule(const std::string& path, const std::string& replacement = "/display") {
    auto row = JsonValue::object();
    row.o["target_path"] = path;
    row.o["replacement_path"] = replacement;
    return row;
}
JsonValue hide_rule(const std::string& id, const std::string& path) {
    hide::Definition definition{id, path, false, "名称 ", "note\r\n尾 ", "组 "};
    return hide::definition_json(definition);
}
JsonValue& resource(JsonValue& value, const char* name) {
    return value.o.at("resources").o.at(name);
}
void capture_rejected(const std::string& config, const std::string& modes, const std::string& rules,
                      const std::string& hidden) {
    JsonValue output("sentinel");
    std::string error;
    require(!capture_profile(config, modes, rules, hidden, "capture", "", output, error));
    require(output.s == "sentinel" && !error.empty());
}
}  // namespace

int main() {
    auto original = base();
    auto value = original;
    JsonValue parsed;
    std::string error;
    require(valid_profile_id("a0_-"));
    require(valid_profile_id(std::string(64, 'a')));
    for (const auto& id : {"", "A", "_a", "a/b", "a.b", "é", "a "})
        require(!valid_profile_id(id));
    require(!valid_profile_id(std::string(65, 'a')));
    require(resource(value, "settings").find("maps_rules") == nullptr);
    require(resource(value, "settings").find("enable_maps_spoof") == nullptr);
    for (const auto* mode : {"auto", "sumh", "magic", "overlay", "none"})
        resource(value, "modules").a.push_back(module(mode, mode));
    resource(value, "modules").a[0].o["rules"].a = {rule("/"), rule("/vendor/尾 ", "hide"),
                                                    rule("/system/tab\tname", "none")};
    resource(value, "maps").o["enabled"] = true;
    resource(value, "maps").o["rules"].a.push_back(map_rule("/source/尾 ", "/shown/尾 "));
    resource(value, "user_hide")
        .o["rules"]
        .a.push_back(hide_rule(std::string(32, 'a'), "/隐藏\n换行 "));
    require(parse_profile(stringify_json(value), parsed, error));
    require(parsed.o.at("name").s == "默认方案 ");
    require(resource(parsed, "user_hide").o.at("rules").a[0].o.at("path").s == "/隐藏\n换行 ");
    require(!resource(parsed, "user_hide").o.at("rules").a[0].o.at("enabled").b);
    require(resource(parsed, "modules").a[0].o.at("id").s == "auto");
    JsonValue again;
    require(parse_profile(stringify_json(parsed), again, error));
    require(stringify_json(parsed) == stringify_json(again));
    for (const auto* key : {"schema_version", "kind", "name", "note", "resources"}) {
        auto bad = original;
        bad.o.erase(key);
        rejected(bad);
    }
    for (const auto& field : {"boot_id", "operation_id", "inode", "credentials", "active", "id"}) {
        auto bad = original;
        bad.o[field] = "forbidden";
        rejected(bad);
    }
    for (const auto version : {0.0, 1.5, 2.0}) {
        auto bad = original;
        bad.o["schema_version"] = version;
        rejected(bad);
    }
    for (const auto* key : {"name", "note"}) {
        auto bad = original;
        bad.o[key] = true;
        rejected(bad);
        bad = original;
        bad.o[key] = std::string("x\0y", 3);
        rejected(bad);
    }
    value = original;
    value.o["name"] = "";
    rejected(value);
    value = original;
    value.o["name"] = std::string(128, 'x');
    accepted(value);
    value.o["name"] = std::string(129, 'x');
    rejected(value);
    value = original;
    value.o["note"] = std::string(1024, 'n');
    accepted(value);
    value.o["note"] = std::string(1025, 'n');
    rejected(value);
    for (const auto* scope : {"settings", "modules", "user_hide", "maps"}) {
        auto bad = original;
        bad.o["resources"].o.erase(scope);
        rejected(bad);
    }
    value = original;
    resource(value, "settings").o["future"] = 9007199254740992.0;
    rejected(value);
    value = original;
    resource(value, "settings").o.erase("debug");
    rejected(value);
    value = original;
    resource(value, "settings").o["maps_rules"] = JsonValue::array();
    rejected(value);
    value = original;
    resource(value, "settings").o["enable_kernel_build_spoof"] = true;
    rejected(value);
    resource(value, "settings").o["kernel_build_version"] = "valid version";
    accepted(value);
    resource(value, "settings").o["kernel_build_release"] = std::string(65, 'a');
    rejected(value);
    value = original;
    resource(value, "settings").o["mountsource"] = std::string(64 * 1024, 'a');
    rejected(value);
    value = original;
    resource(value, "settings").o["mirror_img_size_mb"] = 1.5;
    rejected(value);
    for (const auto* mode : {"hide", "unknown"}) {
        value = original;
        resource(value, "modules").a.push_back(module("example", mode));
        rejected(value);
    }
    value = original;
    resource(value, "modules").a = {module(std::string(255, 'm'))};
    accepted(value);
    resource(value, "modules").a[0].o["id"] = std::string(256, 'm');
    rejected(value);
    value = original;
    resource(value, "modules").a = {module("same"), module("same")};
    rejected(value);
    for (const auto* path : {"relative", "/trailing/", "/double//name", "/./name", "/../name",
                             "/line\nname", "/line\rname"}) {
        value = original;
        resource(value, "modules").a = {module("m")};
        resource(value, "modules").a[0].o["rules"].a.push_back(rule(path));
        rejected(value);
    }
    value = original;
    resource(value, "modules").a = {module("m")};
    for (int i = 0; i < 256; ++i)
        resource(value, "modules").a[0].o["rules"].a.push_back(rule("/r" + std::to_string(i)));
    accepted(value);
    resource(value, "modules").a[0].o["rules"].a.push_back(rule("/over"));
    rejected(value);
    value = original;
    for (int i = 0; i < 256; ++i)
        resource(value, "modules").a.push_back(module("m" + std::to_string(i)));
    accepted(value);
    resource(value, "modules").a.push_back(module("overflow"));
    rejected(value);
    value = original;
    for (int i = 0; i < 17; ++i) {
        auto row = module("m" + std::to_string(i));
        for (int j = 0; j < (i == 16 ? 0 : 256); ++j)
            row.o["rules"].a.push_back(rule("/r" + std::to_string(j)));
        resource(value, "modules").a.push_back(row);
    }
    accepted(value);
    resource(value, "modules").a.back().o["rules"].a.push_back(rule("/over"));
    rejected(value);
    value = original;
    resource(value, "modules").a = {module("m")};
    for (int i = 0; i < 256; ++i)
        resource(value, "modules")
            .a[0]
            .o["rules"]
            .a.push_back(rule("/" + std::string(240, 'x') + std::to_string(i)));
    rejected(value);  // Existing 56KiB encoded module budget is unchanged.
    // Independently valid resource groups can still exceed the profile cap.
    value = original;
    for (int i = 0; i < 256; ++i) {
        auto row = module("m" + std::to_string(i));
        for (int j = 0; j < 8; ++j)
            row.o["rules"].a.push_back(rule("/" + std::string(3960, 'x') + std::to_string(j)));
        resource(value, "modules").a.push_back(std::move(row));
    }
    require(stringify_json(value).size() < PROFILE_MAX_BYTES);
    accepted(value);
    for (auto& row : resource(value, "modules").a)
        for (auto& item : row.o["rules"].a)
            item.o["path"].s.insert(1, 120, 'x');
    require(stringify_json(value).size() > PROFILE_MAX_BYTES);
    rejected(value);
    for (const auto* path : {"relative", "/line\nname", "/line\rname"}) {
        value = original;
        resource(value, "maps").o["rules"].a.push_back(map_rule(path));
        rejected(value);
    }
    value = original;
    resource(value, "maps").o["rules"].a = {map_rule("/same"), map_rule("/same")};
    rejected(value);
    value = original;
    resource(value, "maps")
        .o["rules"]
        .a.push_back(map_rule("/target", "/" + std::string(254, 'x')));
    accepted(value);
    resource(value, "maps").o["rules"].a[0].o["replacement_path"] = "/" + std::string(255, 'x');
    rejected(value);
    value = original;
    for (int i = 0; i < 256; ++i)
        resource(value, "maps").o["rules"].a.push_back(map_rule("/t" + std::to_string(i)));
    accepted(value);
    resource(value, "maps").o["rules"].a.push_back(map_rule("/overflow"));
    rejected(value);
    value = original;
    for (int i = 0; i < 256; ++i)
        resource(value, "maps")
            .o["rules"]
            .a.push_back(map_rule("/" + std::string(240, 'x') + std::to_string(i)));
    rejected(value);
    value = original;
    resource(value, "user_hide") = JsonValue::array();
    rejected(value);
    value = original;
    resource(value, "user_hide").o["rules"].a = {hide_rule(std::string(32, 'a'), "/first"),
                                                 hide_rule(std::string(32, 'a'), "/second")};
    rejected(value);
    value = original;
    resource(value, "user_hide").o["rules"].a = {hide_rule(std::string(32, 'a'), "/same"),
                                                 hide_rule(std::string(32, 'b'), "/same")};
    rejected(value);
    value = original;
    for (int i = 0; i < 4096; ++i) {
        std::ostringstream id;
        id << std::hex << std::setfill('0') << std::setw(32) << i;
        resource(value, "user_hide")
            .o["rules"]
            .a.push_back(hide_rule(id.str(), "/h" + std::to_string(i)));
    }
    accepted(value);
    resource(value, "user_hide").o["rules"].a.push_back(hide_rule(std::string(32, 'f'), "/over"));
    rejected(value);
    resource(value, "user_hide").o["rules"].a.pop_back();
    for (auto& row : resource(value, "user_hide").o["rules"].a)
        row.o["note"] = std::string(1024, 'n');
    rejected(value);
    rejected_bytes(std::string(PROFILE_MAX_BYTES + 1, ' '));
    auto padded = stringify_json(original);
    padded.append(PROFILE_MAX_BYTES - padded.size(), ' ');
    require(parse_profile(padded, parsed, error));
    padded += ' ';
    rejected_bytes(padded);
    auto bytes = stringify_json(original);
    rejected_bytes("{\"schema_version\":1," + bytes.substr(1));
    for (const auto* key : {"mountsource", "enabled", "rules"}) {
        auto duplicate = bytes;
        const auto at = duplicate.find(std::string("\"") + key + "\":");
        require(at != std::string::npos);
        duplicate.insert(at, std::string("\"") + key + "\":null,");
        rejected_bytes(duplicate);
    }
    rejected_bytes(std::string("\xff", 1));
    rejected_bytes(std::string(129, '[') + std::string(129, ']'));
    // Semantic capture materializes defaults, respects canonical mode override,
    // and retains explicit auto/empty IDs the production map loader elides.
    require(
        capture_profile(R"({"fs_type":"","mount_backend":""})", R"({"m":"magic","legacy":"none"})",
                        R"({"m":{"mode":"auto","rules":[]},"r":[{"path":"/keep ","mode":"none"}]})",
                        R"(["/legacy\nname ","/legacy\nname "])", "captured", "", parsed, error));
    require(resource(parsed, "settings").o.at("mount_backend").s == "auto");
    require(resource(parsed, "modules").a.size() == 3);
    require(resource(parsed, "modules").a[1].o.at("id").s == "m");
    require(resource(parsed, "modules").a[1].o.at("mode").s == "auto");
    require(resource(parsed, "user_hide").o.at("rules").a.size() == 1);
    capture_rejected(R"({"future":true})", "{}", "{}", "[]");
    capture_rejected(R"({"debug":true,"debug":false})", "{}", "{}", "[]");
    capture_rejected("{}", R"({"m":"sumh","m":"none"})", "{}", "[]");
    capture_rejected("{}", "{}", R"({"m":[{"path":"/bad/","mode":"sumh"}]})", "[]");
    capture_rejected("{}", "{}",
                     R"({"m":[{"path":"/x","mode":"sumh"},{"path":"/x","mode":"none"}]})", "[]");
    capture_rejected("{}", "{}", "{}", R"({"schema_version":2,"rules":[]})");
    // Exact scoped diff; same path with new hide identity is explicit conflict.
    value = original;
    resource(value, "settings").o["debug"] = true;
    resource(value, "modules").a = {module("new")};
    resource(value, "maps").o["enabled"] = true;
    resource(value, "maps").o["rules"].a = {map_rule("/new")};
    resource(original, "user_hide").o["rules"].a = {hide_rule(std::string(32, 'a'), "/shared")};
    resource(value, "user_hide").o["rules"].a = {hide_rule(std::string(32, 'b'), "/shared")};
    auto diff = diff_profiles(original, value, {"settings", "modules", "maps", "user_hide"});
    require(diff.o.at("resources").o.at("settings").o.at("counts").o.at("changed").n == 1);
    require(diff.o.at("resources").o.at("modules").o.at("counts").o.at("added").n == 1);
    require(diff.o.at("resources").o.at("maps").o.at("enabled").o.at("counts").o.at("changed").n ==
            1);
    require(diff.o.at("resources").o.at("user_hide").o.at("identity_conflicts").a.size() == 1);
    auto other_path = original;
    resource(other_path, "user_hide").o["rules"].a[0].o["path"] = "/new-path";
    auto identity_diff = diff_profiles(original, other_path, {"user_hide"});
    require(identity_diff.o.at("resources")
                .o.at("user_hide")
                .o.at("identity_conflicts")
                .a[0]
                .o.at("kind")
                .s == "same_id_different_path");
    diff = diff_profiles(original, value, {"settings"});
    require(diff.o.at("resources").o.size() == 1);
    diff = diff_profiles(value, original, {"modules", "maps"});
    require(diff.o.at("resources").o.at("modules").o.at("counts").o.at("removed").n == 1);
    diff = diff_profiles(value, value, {"settings", "modules", "maps", "user_hide"});
    for (const auto& [key, result] : diff.o.at("resources").o) {
        (void)key;
        require(result.o.at("changes").a.empty());
    }
    std::cout << "profile model: " << checks << " checks passed\n";
}
