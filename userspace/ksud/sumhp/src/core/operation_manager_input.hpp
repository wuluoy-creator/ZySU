#pragma once
#include <cmath>
#include "user_hide_input.hpp"
#include "userspace/common/temporary_maps_command.hpp"

namespace sumhp::state {
struct OperationManagerRequest {
    std::string action, id, input;
    unsigned timeout_ms = 0;
};
inline bool parse_operation_manager_input(const std::string& bytes,
                                          OperationManagerRequest& request, std::string& error) {
    const auto reject = [&] {
        error = "invalid operation Manager request";
        return false;
    };
    if (bytes.size() > 48 * 1024 || bytes.find('\0') != std::string::npos ||
        !ksud::text::valid_utf8(bytes))
        return reject();
    JsonValue root;
    if (!json::Parser(bytes, true).parse(root, error) || !root.is_object() ||
        stringify_json(root).size() > 48 * 1024)
        return reject();
    const auto* schema = root.find("schema_version");
    const auto* action = root.find("action");
    if (!schema || !schema->is_number() || (schema->n != 1 && schema->n != 2) || !action ||
        !action->is_string())
        return reject();
    const bool temporary_maps = schema->n == 2;
    OperationManagerRequest value;
    value.action = action->s;
    const bool prepare = value.action == "prepare", execute = value.action == "execute";
    const bool wait = value.action == "wait";
    if (!prepare && !execute && !wait && value.action != "show" && value.action != "result")
        return reject();
    std::set<std::string> keys{"schema_version", "action"};
    if (temporary_maps) {
        keys.insert("resource");
        const auto* resource = root.find("resource");
        if (!resource || !resource->is_string() || resource->s != "temporary_maps")
            return reject();
    }
    if (!prepare) {
        keys.insert("operation_id");
        const auto* id = root.find("operation_id");
        if (!id || !id->is_string() || !hide::input_detail::valid_id(id->s))
            return reject();
        value.id = id->s;
    }
    if (prepare || execute) {
        keys.insert("input");
        const auto* input = root.find("input");
        if (!input || !input->is_object() || input->o.size() != 2)
            return reject();
        const auto* version = input->find("schema_version");
        const auto* args = input->find("args");
        if (!version || !version->is_number() || version->n != 1 || !args || !args->is_array())
            return reject();
        std::vector<std::string> parsed;
        if (temporary_maps) {
            for (const auto& arg : args->a) {
                if (!arg.is_string())
                    return reject();
                parsed.push_back(arg.s);
            }
            maps_temporary::DeleteRequest target;
            if (!maps_temporary::parse_tracked_delete(parsed, target))
                return reject();
        } else {
            hide::Request target;
            if (!hide::parse_manager_input(stringify_json(*args), parsed, target, error) ||
                hide::definition_query(target.action))
                return reject();
        }
        value.input = stringify_json(*input);
        auto frame = JsonValue::array();
        for (const auto& argument :
             std::vector<std::string>{"operation", "execute", std::string(32, 'a'), value.input})
            frame.a.emplace_back(argument);
        if (value.input.size() > 48 * 1024 || stringify_json(frame).size() + 1 > 64 * 1024)
            return reject();
    }
    if (wait) {
        keys.insert("timeout_ms");
        const auto* timeout = root.find("timeout_ms");
        if (!timeout || !timeout->is_number() || !std::isfinite(timeout->n) || timeout->n < 0 ||
            timeout->n > 5000 || std::floor(timeout->n) != timeout->n)
            return reject();
        value.timeout_ms = static_cast<unsigned>(timeout->n);
    }
    if (root.o.size() != keys.size())
        return reject();
    for (const auto& field : root.o)
        if (!keys.count(field.first))
            return reject();
    request = std::move(value);
    error.clear();
    return true;
}
}  // namespace sumhp::state
