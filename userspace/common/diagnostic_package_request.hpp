#pragma once

#include "diagnostics_request.hpp"

namespace sumhp::diagnostic_package_manager {
inline constexpr std::size_t input_bytes = 8192;
inline constexpr std::size_t response_bytes = 208 * 1024;
inline constexpr std::size_t envelope_bytes = 432 * 1024;
struct Request {
    std::string action, id, revision, diagnostic;
    bool include_identifiers = false;
};
inline bool valid_revision(const std::string& value) {
    if (value.size() != 74 || value.compare(0, 10, "sha256-v1:") != 0)
        return false;
    for (std::size_t i = 10; i < value.size(); ++i)
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
            return false;
    return true;
}
// Strict complete objects only. Parse failures never modify the caller's value.
inline bool parse(const std::string& bytes, Request& out) {
    if (bytes.size() > input_bytes || !ksud::text::valid_utf8(bytes) ||
        bytes.find('\0') != std::string::npos)
        return false;
    json::Value value;
    std::string reason;
    if (!json::Parser(bytes, true).parse(value, reason) || !value.is_object() ||
        !value.at("schema_version").is_number() || value.at("schema_version").n != 1 ||
        !value.at("action").is_string())
        return false;
    Request candidate;
    candidate.action = value.at("action").s;
    const bool simple = candidate.action == "capabilities" || candidate.action == "list";
    const bool create = candidate.action == "create";
    const bool reviewed = candidate.action == "export" || candidate.action == "remove";
    if (!simple && !create && !reviewed && candidate.action != "preview")
        return false;
    for (const auto& field : value.o) {
        if (field.first == "schema_version" || field.first == "action")
            continue;
        if (!simple && field.first == "id")
            continue;
        if (reviewed && field.first == "revision")
            continue;
        if (create && (field.first == "include_identifiers" || field.first == "diagnostic"))
            continue;
        return false;
    }
    if (!simple) {
        if (!value.at("id").is_string() || !diagnostics::valid_stable_id(value.at("id").s))
            return false;
        candidate.id = value.at("id").s;
    }
    if (reviewed) {
        if (!value.at("revision").is_string() || !valid_revision(value.at("revision").s))
            return false;
        candidate.revision = value.at("revision").s;
    }
    if (create) {
        if (!value.at("include_identifiers").is_bool() || !value.at("diagnostic").is_object())
            return false;
        candidate.include_identifiers = value.at("include_identifiers").b;
        // Doctor schema1 contains only scalar fields. Serialize those fields
        // compactly: the shared general dumper adds spaces after commas, which
        // would reject a valid 4096-byte Manager diagnostic after normalization.
        // Keep proper string escaping; never remove whitespace inside a value.
        candidate.diagnostic = "{";
        bool first = true;
        for (const auto& field : value.at("diagnostic").o) {
            if (!field.second.is_string() && !field.second.is_number())
                return false;
            if (!first)
                candidate.diagnostic += ',';
            first = false;
            candidate.diagnostic +=
                json::escape_string(field.first) + ':' + json::dump(field.second);
        }
        candidate.diagnostic += '}';
        diagnostics::Request diagnostic;
        if (!diagnostics::parse(candidate.diagnostic, diagnostic, reason))
            return false;
    }
    out = std::move(candidate);
    return true;
}
inline bool parse_channel_arguments(const std::vector<std::string>& args, Request& out) {
    return args.size() == 5 && args[0] == "doctor" && args[1] == "package" &&
           args[2] == "manager" && args[4] == "--channel" && parse(args[3], out);
}
}  // namespace sumhp::diagnostic_package_manager
