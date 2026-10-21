#pragma once
#include <algorithm>
#include <set>
#include "../../../../common/utf8.hpp"
#include "user_hide_store.hpp"

namespace sumhp::hide {
namespace input_detail {
inline bool valid_id(const std::string& value) {
    return value.size() == 32 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
inline bool valid_path(const std::string& value) {
    return !value.empty() && value.front() == '/' && value.size() < 4096 &&
           value.find('\0') == std::string::npos && ksud::text::valid_utf8(value);
}
inline bool revision_valid(const std::string& value) {
    return value.size() == 74 && value.compare(0, 10, "sha256-v1:") == 0 &&
           value.find_first_not_of("0123456789abcdef", 10) == std::string::npos;
}
inline bool text_valid(const JsonValue& value, std::size_t size) {
    return value.is_string() && value.s.size() <= size && value.s.find('\0') == std::string::npos &&
           ksud::text::valid_utf8(value.s);
}
inline bool fields_valid(const std::string& bytes, bool create, JsonValue& fields,
                         std::string& error) {
    error = "invalid definition fields: expected bounded UTF-8 JSON with supported field names and "
            "types";
    if (bytes.size() > 40 * 1024 || !ksud::text::valid_utf8(bytes))
        return false;
    std::string parse_error;
    if (!json::Parser(bytes, true).parse(fields, parse_error)) {
        error = parse_error;
        return false;
    }
    if (!fields.is_object())
        return false;
    if (create && (!fields.find("path") || !fields.o.at("path").is_string() ||
                   !valid_path(fields.o.at("path").s)))
        return false;
    if (!create && fields.o.empty())
        return false;
    for (const auto& [key, value] : fields.o) {
        if (key == "name" && text_valid(value, HIDE_NAME_MAX_BYTES))
            continue;
        if (key == "note" && text_valid(value, HIDE_NOTE_MAX_BYTES))
            continue;
        if (key == "group" && text_valid(value, HIDE_GROUP_MAX_BYTES))
            continue;
        if (create && key == "path" && value.is_string() && valid_path(value.s))
            continue;
        if (create && key == "enabled" && value.is_bool())
            continue;
        return false;
    }
    return true;
}
inline bool import_fields_valid(const std::string& bytes, JsonValue& fields, std::string& error) {
    error = "invalid import fields: expected exact path, enabled, name, note, group and expect";
    if (bytes.size() > 40 * 1024 || !ksud::text::valid_utf8(bytes) ||
        !json::Parser(bytes, true).parse(fields, error) || !fields.is_object() ||
        fields.o.size() != 6)
        return false;
    const auto* path = fields.find("path");
    const auto* enabled = fields.find("enabled");
    const auto* expected = fields.find("expect");
    const auto* name = fields.find("name");
    const auto* note = fields.find("note");
    const auto* group = fields.find("group");
    return path && path->is_string() && valid_path(path->s) && enabled && enabled->is_bool() &&
           expected && expected->is_string() &&
           (expected->s == "absent" || expected->s == "same_identity") && name &&
           text_valid(*name, HIDE_NAME_MAX_BYTES) && note &&
           text_valid(*note, HIDE_NOTE_MAX_BYTES) && group &&
           text_valid(*group, HIDE_GROUP_MAX_BYTES);
}
}  // namespace input_detail
struct Request {
    std::string action, selector, revision;
    JsonValue fields = JsonValue::object();
    std::size_t cursor = 0;
};
inline bool definition_command(const std::vector<std::string>& args) {
    static const std::set<std::string> actions = {
        "status", "definitions", "export",       "create",        "import-id", "annotate",
        "enable", "disable",     "enable-group", "disable-group", "retry",     "remove-id"};
    return args.size() >= 2 && args[0] == "hide" && actions.count(args[1]);
}
inline bool definition_query(const std::string& action) {
    return action == "status" || action == "definitions" || action == "export" ||
           action == "capabilities";
}
inline bool parse_request(const std::vector<std::string>& args, Request& request,
                          std::string& error) {
    using namespace input_detail;
    error = "invalid hide definition command; use status [revision cursor], create revision JSON, "
            "annotate id revision JSON, or enable/disable/retry/remove-id id revision, "
            "enable-group/disable-group group revision";
    if (!definition_command(args))
        return false;
    Request parsed;
    parsed.action = args[1];
    if (parsed.action == "export") {
        if (args.size() != 2)
            return false;
    } else if (parsed.action == "status" || parsed.action == "definitions") {
        if (args.size() == 4) {
            parsed.revision = args[2];
            if (!revision_valid(parsed.revision) || args[3].empty() || args[3].size() > 4 ||
                (args[3].size() > 1 && args[3][0] == '0') ||
                args[3].find_first_not_of("0123456789") != std::string::npos)
                return false;
            parsed.cursor = static_cast<std::size_t>(std::stoul(args[3]));
            if (parsed.cursor > HIDE_MAX_RULES)
                return false;
        } else if (args.size() != 2)
            return false;
    } else if (parsed.action == "create") {
        if (args.size() != 4 || !revision_valid(args[2]) ||
            !fields_valid(args[3], true, parsed.fields, error))
            return false;
        parsed.revision = args[2];
    } else {
        if (args.size() !=
                ((parsed.action == "annotate" || parsed.action == "import-id") ? 5u : 4u) ||
            !revision_valid(args[3]))
            return false;
        parsed.selector = args[2];
        parsed.revision = args[3];
        if (parsed.action == "enable-group" || parsed.action == "disable-group") {
            if (!text_valid(JsonValue(parsed.selector), HIDE_GROUP_MAX_BYTES))
                return false;
        } else if (!input_detail::valid_id(parsed.selector))
            return false;
        if (parsed.action == "annotate" && !fields_valid(args[4], false, parsed.fields, error))
            return false;
        if (parsed.action == "import-id" && !import_fields_valid(args[4], parsed.fields, error))
            return false;
    }
    request = std::move(parsed);
    error.clear();
    return true;
}
// Shared by the Manager transport before exec and by the private child route.
// The JSON representation, not a shell string, defines the fixed argv budget.
inline bool parse_manager_arguments(const std::vector<std::string>& args, Request& request,
                                    std::string& error) {
    if (args.size() > 5) {
        error = "too many user hide Manager arguments";
        return false;
    }
    auto value = JsonValue::array();
    std::size_t size = 0;
    for (const auto& arg : args) {
        if (arg.size() > 48 * 1024 - size || arg.find('\0') != std::string::npos ||
            !ksud::text::valid_utf8(arg)) {
            error = "invalid UTF-8 or NUL in user hide Manager arguments";
            return false;
        }
        size += arg.size();
        value.a.emplace_back(arg);
    }
    if (stringify_json(value).size() > 48 * 1024) {
        error = "user hide Manager input exceeds capacity";
        return false;
    }
    if (args == std::vector<std::string>{"api", "meta", "--control-only"}) {
        Request parsed;
        parsed.action = "capabilities";
        request = std::move(parsed);
        error.clear();
        return true;
    }
    Request parsed;
    if (!parse_request(args, parsed, error) || parsed.action == "export")
        return false;
    request = std::move(parsed);
    return true;
}
inline bool parse_manager_input(const std::string& bytes, std::vector<std::string>& args,
                                Request& request, std::string& error) {
    if (bytes.size() > 48 * 1024 || bytes.find('\0') != std::string::npos ||
        !ksud::text::valid_utf8(bytes)) {
        error = "invalid user hide Manager input encoding or capacity";
        return false;
    }
    JsonValue root;
    if (!json::Parser(bytes, true).parse(root, error) || !root.is_array() || root.a.size() > 5) {
        error = "user hide Manager input must be an allowlisted argument array";
        return false;
    }
    std::vector<std::string> parsed;
    for (const auto& item : root.a) {
        if (!item.is_string()) {
            error = "user hide Manager argument must be a string";
            return false;
        }
        parsed.push_back(item.s);
    }
    Request validated;
    if (!parse_manager_arguments(parsed, validated, error))
        return false;
    args = std::move(parsed);
    request = std::move(validated);
    return true;
}
}  // namespace sumhp::hide
