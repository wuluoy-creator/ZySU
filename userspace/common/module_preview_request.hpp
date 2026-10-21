#pragma once

#include <optional>
#include "json.hpp"
#include "utf8.hpp"

namespace sumhp::module_preview {
inline constexpr std::size_t input_bytes = 64 * 1024;
inline constexpr std::size_t response_bytes = 2 * 1024 * 1024;
inline constexpr std::size_t envelope_bytes = (2 * response_bytes) + 256;

struct Input {
    std::optional<std::string> candidate;
    std::string explain_path;
};

// Literal canonical module paths. No trimming, alias resolution or IO here.
inline bool valid_path(const std::string& path) {
    if (path.empty() || path.front() != '/' || path.size() >= 4096 ||
        path.find('\0') != std::string::npos || path.find_first_of("\r\n") != std::string::npos ||
        !ksud::text::valid_utf8(path) || (path.size() > 1 && path.back() == '/'))
        return false;
    for (std::size_t begin = 1; begin < path.size();) {
        const auto end = path.find('/', begin);
        const auto part = path.substr(begin, end == std::string::npos ? end : end - begin);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return true;
}

// Shared transport shape/budget validation only. Production candidate config
// and rule semantics are validated by apply_module_preview_input on copies.
inline bool valid(const Input& input) {
    if (!input.explain_path.empty() && !valid_path(input.explain_path))
        return false;
    if (!input.candidate)
        return true;
    const auto& bytes = *input.candidate;
    if (bytes.empty() || bytes.size() > input_bytes || bytes.find('\0') != std::string::npos ||
        !ksud::text::valid_utf8(bytes))
        return false;
    json::Value value;
    std::string error;
    if (!json::Parser(bytes, true).parse(value, error) || !value.is_object())
        return false;
    const auto* version = value.find("schema_version");
    const auto* kind = value.find("kind");
    if (!version || !version->is_number() || version->n != 1 || !kind || !kind->is_string() ||
        kind->s != "sumh.module.plan.input")
        return false;
    for (const auto& [key, field] : value.o) {
        if (key == "schema_version" || key == "kind")
            continue;
        if (key == "config" && field.is_object())
            continue;
        if (key == "modules" && field.is_array() && field.a.size() <= 256)
            continue;
        return false;
    }
    return true;
}

inline std::vector<std::string> arguments(const Input& input) {
    std::vector<std::string> args{"module", "manager-preview",
                                  input.candidate ? "candidate" : "saved"};
    if (input.candidate)
        args.push_back(*input.candidate);
    args.push_back(input.explain_path);
    return args;
}

// Only the private exact route includes --channel; no public CLI wrappers.
inline bool parse_arguments(const std::vector<std::string>& args, Input& input) {
    if (args.size() < 5 || args[0] != "module" || args[1] != "manager-preview" ||
        args.back() != "--channel")
        return false;
    Input parsed;
    if (args[2] == "saved" && args.size() == 5)
        parsed.explain_path = args[3];
    else if (args[2] == "candidate" && args.size() == 6) {
        parsed.candidate = args[3];
        parsed.explain_path = args[4];
    } else
        return false;
    if (!valid(parsed))
        return false;
    input = std::move(parsed);
    return true;
}
}  // namespace sumhp::module_preview
