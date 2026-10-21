#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>
#include "../../../../common/utf8.hpp"
#include "mount/module_rebuild.hpp"

namespace sumhp::mount::sumh {
namespace {
bool keys(const JsonValue& value, std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.o.size() != expected.size())
        return false;
    return std::all_of(expected.begin(), expected.end(),
                       [&](const char* key) { return value.contains(key); });
}
bool count(const JsonValue& value, std::size_t maximum) {
    return value.is_number() && std::isfinite(value.n) && value.n >= 0 &&
           value.n <= static_cast<double>(maximum) && std::floor(value.n) == value.n;
}
bool id_valid(const std::string& id) {
    return id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool module_id_valid(const std::string& id) {
    return !id.empty() && id.size() <= 255 && id != "." && id != ".." &&
           std::all_of(id.begin(), id.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-';
           });
}
bool path_valid(const std::string& path) {
    return !path.empty() && path.size() < 4096 && path.front() == '/' &&
           path.find('\0') == std::string::npos;
}
JsonValue path_json(const std::string& path) {
    auto out = JsonValue::object();
    const bool utf8 = ksud::text::valid_utf8(path);
    out.o["encoding"] = utf8 ? "utf-8" : "hex";
    std::string value;
    if (utf8)
        value = path;
    else {
        static constexpr char hex[] = "0123456789abcdef";
        for (unsigned char const byte : path) {
            value += hex[byte >> 4];
            value += hex[byte & 15];
        }
    }
    out.o["value"] = value;
    out.o["byte_length"] = static_cast<int>(path.size());
    return out;
}
bool path_parse(const JsonValue& value, std::string& out) {
    if (!keys(value, {"encoding", "value", "byte_length"}) || !value.at("encoding").is_string() ||
        !value.at("value").is_string() || !count(value.at("byte_length"), 4095))
        return false;
    std::string result;
    const auto& encoded = value.at("value").s;
    if (value.at("encoding").s == "utf-8") {
        if (!ksud::text::valid_utf8(encoded))
            return false;
        result = encoded;
    } else if (value.at("encoding").s == "hex") {
        if (encoded.size() > 8190 || encoded.size() % 2 ||
            encoded.find_first_not_of("0123456789abcdef") != std::string::npos)
            return false;
        const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        for (std::size_t i = 0; i < encoded.size(); i += 2)
            result += static_cast<char>((digit(encoded[i]) << 4) | digit(encoded[i + 1]));
        if (ksud::text::valid_utf8(result))
            return false;  // one canonical representation
    } else
        return false;
    if (!path_valid(result) || result.size() != static_cast<std::size_t>(value.at("byte_length").n))
        return false;
    out = std::move(result);
    return true;
}
bool record_valid(const ModuleRebuildRecord& r) {
    if (!id_valid(r.id) || r.boot_id.empty() || r.boot_id.size() > 128 ||
        r.boot_id.find_first_not_of(
            "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ._:-") !=
            std::string::npos ||
        r.module_ids.size() > MODULE_REBUILD_MAX_ACTIONS ||
        r.plan.mappings.size() > MODULE_REBUILD_MAX_ACTIONS ||
        r.plan.hide.size() > MODULE_REBUILD_MAX_ACTIONS - r.plan.mappings.size() ||
        r.mappings_acknowledged > r.plan.mappings.size() ||
        r.hides_acknowledged > r.plan.hide.size() || r.error_number < 0 || r.error_number > 4095)
        return false;
    std::set<std::string> ids;
    for (const auto& id : r.module_ids)
        if (!module_id_valid(id) || !ids.insert(id).second)
            return false;
    std::size_t raw_bytes = 0;
    const auto valid_path = [&](const std::string& path) {
        if (!path_valid(path) || path.size() > MODULE_REBUILD_MAX_BYTES - raw_bytes)
            return false;
        raw_bytes += path.size();
        return true;
    };
    for (const auto& mapping : r.plan.mappings)
        if (!valid_path(mapping.path) || !valid_path(mapping.source))
            return false;
    for (const auto& path : r.plan.hide)
        if (!valid_path(path))
            return false;
    if (r.phase == "pending")
        return !r.reset_acknowledged && !r.mappings_acknowledged && !r.hides_acknowledged &&
               r.failed_stage.empty() && r.failed_index == -1 && !r.error_number;
    if (r.phase == "acknowledged")
        return r.reset_acknowledged && r.mappings_acknowledged == r.plan.mappings.size() &&
               r.hides_acknowledged == r.plan.hide.size() && r.failed_stage.empty() &&
               r.failed_index == -1 && !r.error_number;
    if (r.phase != "partial_or_unknown" || !r.error_number)
        return false;
    if (r.failed_stage == "reset")
        return !r.reset_acknowledged && !r.mappings_acknowledged && !r.hides_acknowledged &&
               r.failed_index == -1;
    if (r.failed_stage == "mapping")
        return r.reset_acknowledged && !r.hides_acknowledged && r.failed_index >= 0 &&
               static_cast<std::size_t>(r.failed_index) == r.mappings_acknowledged &&
               r.mappings_acknowledged < r.plan.mappings.size();
    if (r.failed_stage == "hide")
        return r.reset_acknowledged && r.mappings_acknowledged == r.plan.mappings.size() &&
               r.failed_index >= 0 &&
               static_cast<std::size_t>(r.failed_index) == r.hides_acknowledged &&
               r.hides_acknowledged < r.plan.hide.size();
    if (r.failed_stage == "exception")
        return r.failed_index == -1 &&
               (!r.hides_acknowledged ||
                (r.reset_acknowledged && r.mappings_acknowledged == r.plan.mappings.size())) &&
               (r.reset_acknowledged || (!r.mappings_acknowledged && !r.hides_acknowledged));
    return false;
}
bool parse_record(const JsonValue& value, ModuleRebuildRecord& r) {
    if (!keys(value, {"id", "boot_id", "module_ids", "plan", "phase", "reset_acknowledged",
                      "mappings_acknowledged", "hides_acknowledged", "failed_stage", "failed_index",
                      "errno"}) ||
        !value.at("id").is_string() || !value.at("boot_id").is_string() ||
        !value.at("module_ids").is_array() ||
        value.at("module_ids").a.size() > MODULE_REBUILD_MAX_ACTIONS ||
        !value.at("phase").is_string() || !value.at("reset_acknowledged").is_bool() ||
        !count(value.at("mappings_acknowledged"), MODULE_REBUILD_MAX_ACTIONS) ||
        !count(value.at("hides_acknowledged"), MODULE_REBUILD_MAX_ACTIONS) ||
        !value.at("failed_stage").is_string() ||
        (!value.at("failed_index").is_null() &&
         !count(value.at("failed_index"), MODULE_REBUILD_MAX_ACTIONS - 1)) ||
        !count(value.at("errno"), 4095))
        return false;
    r.id = value.at("id").s;
    r.boot_id = value.at("boot_id").s;
    for (const auto& id : value.at("module_ids").a) {
        if (!id.is_string())
            return false;
        r.module_ids.push_back(id.s);
    }
    const auto& plan = value.at("plan");
    if (!keys(plan, {"mappings", "hide"}) || !plan.at("mappings").is_array() ||
        !plan.at("hide").is_array() || plan.at("mappings").a.size() > MODULE_REBUILD_MAX_ACTIONS ||
        plan.at("hide").a.size() > MODULE_REBUILD_MAX_ACTIONS - plan.at("mappings").a.size())
        return false;
    for (const auto& mapping : plan.at("mappings").a) {
        ModuleMountPlan::Mapping m;
        if (!keys(mapping, {"path", "source", "merge"}) || !mapping.at("merge").is_bool() ||
            !path_parse(mapping.at("path"), m.path) || !path_parse(mapping.at("source"), m.source))
            return false;
        m.merge = mapping.at("merge").b;
        r.plan.mappings.push_back(std::move(m));
    }
    std::string previous;
    for (const auto& value_path : plan.at("hide").a) {
        std::string path;
        if (!path_parse(value_path, path) || (!previous.empty() && !(previous < path)))
            return false;
        previous = path;
        r.plan.hide.insert(std::move(path));
    }
    r.phase = value.at("phase").s;
    r.reset_acknowledged = value.at("reset_acknowledged").b;
    r.mappings_acknowledged = static_cast<std::size_t>(value.at("mappings_acknowledged").n);
    r.hides_acknowledged = static_cast<std::size_t>(value.at("hides_acknowledged").n);
    r.failed_stage = value.at("failed_stage").s;
    r.failed_index =
        value.at("failed_index").is_null() ? -1 : static_cast<int>(value.at("failed_index").n);
    r.error_number = static_cast<int>(value.at("errno").n);
    return record_valid(r);
}
}  // namespace

JsonValue module_rebuild_record_json(const ModuleRebuildRecord& r) {
    if (!record_valid(r))
        return {};
    auto out = JsonValue::object();
    auto plan = JsonValue::object();
    out.o["id"] = r.id;
    out.o["boot_id"] = r.boot_id;
    out.o["module_ids"] = JsonValue::array();
    for (const auto& id : r.module_ids)
        out.o["module_ids"].a.emplace_back(id);
    plan.o["mappings"] = JsonValue::array();
    plan.o["hide"] = JsonValue::array();
    std::size_t encoded = 0;
    for (const auto& mapping : r.plan.mappings) {
        auto value = JsonValue::object();
        value.o["path"] = path_json(mapping.path);
        value.o["source"] = path_json(mapping.source);
        value.o["merge"] = mapping.merge;
        encoded += stringify_json(value).size() + 1;
        if (encoded > MODULE_REBUILD_MAX_BYTES)
            return {};
        plan.o["mappings"].a.push_back(std::move(value));
    }
    for (const auto& path : r.plan.hide) {
        auto value = path_json(path);
        encoded += stringify_json(value).size() + 1;
        if (encoded > MODULE_REBUILD_MAX_BYTES)
            return {};
        plan.o["hide"].a.push_back(std::move(value));
    }
    out.o["plan"] = std::move(plan);
    out.o["phase"] = r.phase;
    out.o["reset_acknowledged"] = r.reset_acknowledged;
    out.o["mappings_acknowledged"] = static_cast<int>(r.mappings_acknowledged);
    out.o["hides_acknowledged"] = static_cast<int>(r.hides_acknowledged);
    out.o["failed_stage"] = r.failed_stage;
    out.o["failed_index"] = r.failed_index < 0 ? JsonValue{} : JsonValue(r.failed_index);
    out.o["errno"] = r.error_number;
    return out;
}
std::string encode_module_rebuild_history(const ModuleRebuildHistory& history) {
    if (!history.latest)
        return {};
    auto value = JsonValue::object();
    value.o["schema_version"] = 1;
    value.o["resource"] = "module_rebuild_history";
    value.o["latest"] = module_rebuild_record_json(*history.latest);
    value.o["acknowledged"] =
        history.acknowledged ? module_rebuild_record_json(*history.acknowledged) : JsonValue{};
    if (value.at("latest").is_null() ||
        (history.acknowledged && value.at("acknowledged").is_null()))
        return {};
    const auto bytes = stringify_json(value);
    return bytes.size() <= MODULE_REBUILD_MAX_BYTES ? bytes : std::string{};
}
bool parse_module_rebuild_history(const std::string& bytes, ModuleRebuildHistory& out,
                                  std::string& error) {
    const auto fail = [&] {
        error = "module rebuild history is invalid, oversized or unsupported";
        return false;
    };
    if (bytes.empty() || bytes.size() > MODULE_REBUILD_MAX_BYTES || !ksud::text::valid_utf8(bytes))
        return fail();
    JsonValue value;
    if (!json::Parser(bytes, true).parse(value, error) ||
        !keys(value, {"schema_version", "resource", "latest", "acknowledged"}) ||
        !count(value.at("schema_version"), 1) || value.at("schema_version").n != 1 ||
        !value.at("resource").is_string() || value.at("resource").s != "module_rebuild_history")
        return fail();
    ModuleRebuildHistory result;
    ModuleRebuildRecord latest;
    if (!parse_record(value.at("latest"), latest))
        return fail();
    result.latest = std::move(latest);
    if (!value.at("acknowledged").is_null()) {
        ModuleRebuildRecord acknowledged;
        if (!parse_record(value.at("acknowledged"), acknowledged) ||
            acknowledged.phase != "acknowledged")
            return fail();
        result.acknowledged = std::move(acknowledged);
    }
    if (result.latest->phase == "acknowledged") {
        if (!result.acknowledged ||
            stringify_json(value.at("latest")) != stringify_json(value.at("acknowledged")))
            return fail();
    } else if (result.acknowledged && result.acknowledged->id == result.latest->id)
        return fail();
    out = std::move(result);
    error.clear();
    return true;
}
}  // namespace sumhp::mount::sumh
