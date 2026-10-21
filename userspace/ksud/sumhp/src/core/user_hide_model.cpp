#include <algorithm>
#include <set>
#include "../../../../common/utf8.hpp"
#include "core/state_ledger.hpp"
#include "core/user_hide_store.hpp"

namespace sumhp::hide {
namespace {
bool fields(const JsonValue& value, const std::set<std::string>& expected) {
    if (!value.is_object() || value.o.size() != expected.size())
        return false;
    return std::all_of(value.o.begin(), value.o.end(),
                       [&](const auto& item) { return expected.count(item.first) != 0; });
}
bool text(const JsonValue& value, const char* key, std::string& result, std::size_t maximum) {
    const auto* item = value.find(key);
    if (!item || !item->is_string() || item->s.size() > maximum ||
        item->s.find('\0') != std::string::npos || !ksud::text::valid_utf8(item->s))
        return false;
    result = item->s;
    return true;
}
bool legacy_id(const std::string& path, std::string& result) {
    const auto digest = state::revision_for_bytes("sumhp-user-hide-legacy-v1\n" + json_quote(path));
    if (digest.size() != 74 || digest.compare(0, 10, "sha256-v1:") != 0 ||
        digest.find_first_not_of("0123456789abcdef", 10) != std::string::npos)
        return false;
    result = digest.substr(10, 32);
    return true;
}
}  // namespace

bool valid_id(const std::string& id) {
    return id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool valid_path(const std::string& path) {
    return !path.empty() && path.front() == '/' && path.size() < 4096 &&
           path.find('\0') == std::string::npos && ksud::text::valid_utf8(path);
}
bool parse_document(const std::string& bytes, Document& document, std::string& error) {
    const auto fail = [&](const char* reason) {
        error = reason;
        return false;
    };
    if (bytes.size() > HIDE_MAX_BYTES || !ksud::text::valid_utf8(bytes))
        return fail("user hide document exceeds capacity or contains invalid UTF-8");
    JsonValue root;
    if (!json::Parser(bytes, true).parse(root, error))
        return false;
    Document parsed;
    const JsonValue* rules = nullptr;
    if (root.is_array()) {
        parsed.source_schema = 0;
        rules = &root;
    } else {
        if (!fields(root, {"schema_version", "rules"}) ||
            !root.o.at("schema_version").is_number() || root.o.at("schema_version").n != 1 ||
            !root.o.at("rules").is_array())
            return fail(
                "invalid or unsupported user hide document format; preserving stored definitions");
        rules = &root.o.at("rules");
    }
    // Legacy duplicate paths are one definition. Raw input remains byte-bounded.
    if (parsed.source_schema == 1 && rules->a.size() > HIDE_MAX_RULES)
        return fail("user hide rule count exceeds capacity");
    std::set<std::string> ids;
    std::set<std::string> paths;
    for (const auto& row : rules->a) {
        Definition value;
        if (parsed.source_schema == 0) {
            if (!row.is_string() || !valid_path(row.s))
                return fail("invalid legacy user hide path");
            if (!paths.insert(row.s).second)
                continue;
            if (paths.size() > HIDE_MAX_RULES)
                return fail("user hide rule count exceeds capacity");
            value.path = row.s;
            if (!legacy_id(value.path, value.id))
                return fail("user hide legacy identity digest unavailable");
        } else {
            if (!fields(row, {"id", "path", "enabled", "name", "note", "group"}) ||
                !text(row, "id", value.id, 32) || !valid_id(value.id) ||
                !text(row, "path", value.path, 4095) || !valid_path(value.path) ||
                !text(row, "name", value.name, HIDE_NAME_MAX_BYTES) ||
                !text(row, "note", value.note, HIDE_NOTE_MAX_BYTES) ||
                !text(row, "group", value.group, HIDE_GROUP_MAX_BYTES) ||
                !row.o.at("enabled").is_bool())
                return fail("invalid user hide definition fields");
            value.enabled = row.o.at("enabled").b;
            if (!paths.insert(value.path).second)
                return fail("duplicate user hide definition path");
        }
        if (!ids.insert(value.id).second)
            return fail("duplicate or colliding user hide definition ID");
        parsed.rules.push_back(std::move(value));
    }
    if (encode_document(parsed).size() > HIDE_MAX_BYTES)
        return fail("encoded user hide document exceeds capacity");
    document = std::move(parsed);
    error.clear();
    return true;
}
JsonValue definition_json(const Definition& value) {
    auto row = JsonValue::object();
    row.o["id"] = value.id;
    row.o["path"] = value.path;
    row.o["enabled"] = value.enabled;
    row.o["name"] = value.name;
    row.o["note"] = value.note;
    row.o["group"] = value.group;
    return row;
}
JsonValue document_json(const Document& document) {
    auto root = JsonValue::object();
    root.o["schema_version"] = 1;
    root.o["rules"] = JsonValue::array();
    for (const auto& value : document.rules) {
        root.o["rules"].a.push_back(definition_json(value));
    }
    return root;
}
std::string encode_document(const Document& document) {
    return stringify_json(document_json(document)) + "\n";
}
bool parse_portable_document(const std::string& bytes, Document& document, std::string& error) {
    if (bytes.size() > HIDE_EXCHANGE_MAX_BYTES || !ksud::text::valid_utf8(bytes)) {
        error = "user hide exchange exceeds capacity or contains invalid UTF-8";
        return false;
    }
    JsonValue root;
    if (!json::Parser(bytes, true).parse(root, error))
        return false;
    if (!fields(root, {"schema_version", "kind", "rules"}) || !root.o.at("kind").is_string() ||
        root.o.at("kind").s != "sumh_user_hide_definitions") {
        error = "invalid or unsupported user hide exchange format";
        return false;
    }
    root.o.erase("kind");
    // The persistent codec is the authority for canonical store capacity,
    // exact field types, identity uniqueness and path/metadata validation.
    return parse_document(stringify_json(root) + "\n", document, error);
}
bool encode_portable_document(const Document& document, std::string& bytes, std::string& error) {
    Document validated;
    if (!parse_document(encode_document(document), validated, error))
        return false;
    auto root = document_json(validated);
    root.o["kind"] = "sumh_user_hide_definitions";
    auto encoded = stringify_json(root) + "\n";
    if (encoded.size() > HIDE_EXCHANGE_MAX_BYTES) {
        error = "user hide exchange exceeds capacity";
        return false;
    }
    bytes = std::move(encoded);
    error.clear();
    return true;
}
std::vector<std::string> enabled_paths(const Document& document) {
    std::vector<std::string> result;
    for (const auto& value : document.rules)
        if (value.enabled)
            result.push_back(value.path);
    return result;
}
}  // namespace sumhp::hide
