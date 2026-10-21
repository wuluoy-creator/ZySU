#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include "json.hpp"
#include "utf8.hpp"

namespace sumhp::diagnostics {
inline constexpr std::size_t input_bytes = 4096;
inline constexpr std::size_t response_bytes = 65536;
inline constexpr std::size_t envelope_bytes = 2 * response_bytes + 256;

enum class Scenario { UserHide, ModuleFallback, MapsFailure, BootProtection };
struct Request {
    Scenario scenario = Scenario::BootProtection;
    std::string definition_id, module_id, operation_id;
};

inline bool valid_stable_id(const std::string& value) {
    if (value.size() != 32)
        return false;
    for (const char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}
inline bool valid_module_selector(const std::string& value) {
    if (value.empty() || value == "." || value == "..")
        return false;
    for (const char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == '-'))
            return false;
    return true;
}
inline const char* scenario_name(Scenario scenario) {
    switch (scenario) {
    case Scenario::UserHide:
        return "rule-not-effective";
    case Scenario::ModuleFallback:
        return "module-fallback";
    case Scenario::MapsFailure:
        return "maps-failure";
    case Scenario::BootProtection:
        return "boot-protection";
    }
    return "unknown";
}

// Failed parsing never changes the caller's request. This grammar is shared by
// the public input reader and private transport admission; neither executes text.
inline bool parse(const std::string& bytes, Request& out, std::string& reason) {
    const auto reject = [&](const char* code) {
        reason = code;
        return false;
    };
    if (bytes.size() > input_bytes)
        return reject("input_capacity");
    if (!ksud::text::valid_utf8(bytes) || bytes.find('\0') != std::string::npos)
        return reject("input_encoding");
    json::Value value;
    std::string error;
    if (!json::Parser(bytes, true).parse(value, error) || !value.is_object())
        return reject("input_json");
    if (!value.at("schema_version").is_number() || value.at("schema_version").n != 1)
        return reject("input_schema");
    if (!value.at("scenario").is_string())
        return reject("input_scenario");
    Request candidate;
    const auto& scenario = value.at("scenario").s;
    if (scenario == "rule-not-effective")
        candidate.scenario = Scenario::UserHide;
    else if (scenario == "module-fallback")
        candidate.scenario = Scenario::ModuleFallback;
    else if (scenario == "maps-failure")
        candidate.scenario = Scenario::MapsFailure;
    else if (scenario == "boot-protection")
        candidate.scenario = Scenario::BootProtection;
    else
        return reject("input_scenario");
    for (const auto& field : value.o) {
        if (!ksud::text::valid_utf8(field.first) || field.first.find('\0') != std::string::npos)
            return reject("input_encoding");
        if (field.first == "schema_version" || field.first == "scenario")
            continue;
        if (field.first != "operation_id" &&
            !(field.first == "definition_id" && candidate.scenario == Scenario::UserHide) &&
            !(field.first == "module_id" && candidate.scenario == Scenario::ModuleFallback))
            return reject("input_field");
        if (!field.second.is_string())
            return reject("input_type");
        if (!ksud::text::valid_utf8(field.second.s) ||
            field.second.s.find('\0') != std::string::npos)
            return reject("input_encoding");
    }
    if (candidate.scenario == Scenario::UserHide) {
        if (!value.at("definition_id").is_string() || !valid_stable_id(value.at("definition_id").s))
            return reject("input_definition_id");
        candidate.definition_id = value.at("definition_id").s;
    }
    if (candidate.scenario == Scenario::ModuleFallback) {
        if (!value.at("module_id").is_string() || !valid_module_selector(value.at("module_id").s))
            return reject("input_module_id");
        candidate.module_id = value.at("module_id").s;
    }
    if (value.contains("operation_id")) {
        if (!valid_stable_id(value.at("operation_id").s))
            return reject("input_operation_id");
        candidate.operation_id = value.at("operation_id").s;
    }
    out = std::move(candidate);
    reason.clear();
    return true;
}
inline bool parse_channel_arguments(const std::vector<std::string>& args, Request& out,
                                    std::string& reason) {
    if (args.size() != 4 || args[0] != "doctor" || args[1] != "manager" || args[3] != "--channel") {
        reason = "channel_arguments";
        return false;
    }
    return parse(args[2], out, reason);
}
}  // namespace sumhp::diagnostics
