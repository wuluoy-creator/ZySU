#pragma once

#include <charconv>
#include <cstdint>
#include <vector>
#include "json.hpp"
#include "utf8.hpp"

namespace sumhp::temporary_maps {
inline constexpr std::size_t input_bytes = 1024;
inline constexpr std::size_t response_bytes = 56 * 1024;
inline constexpr std::size_t envelope_bytes = 4 * response_bytes;

struct Request {
    std::string action, instance, generation, cursor, ino, dev;
};

inline bool decimal(const json::Value* field, bool nonzero = false,
                    std::uint64_t maximum = UINT64_MAX) {
    if (!field || !field->is_string())
        return false;
    const auto& text = field->s;
    if (text.empty() || text.size() > 20 || (text.size() > 1 && text.front() == '0') ||
        text.find_first_not_of("0123456789") != std::string::npos)
        return false;
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
           (!nonzero || value) && value <= maximum;
}

// Shared private grammar. No filesystem IO, numeric precision loss or shell.
inline bool parse(const std::string& bytes, Request& request) {
    if (bytes.empty() || bytes.size() > input_bytes || bytes.find('\0') != std::string::npos ||
        !ksud::text::valid_utf8(bytes))
        return false;
    json::Value value;
    std::string error;
    if (!json::Parser(bytes, true).parse(value, error) || !value.is_object())
        return false;
    const auto* version = value.find("schema_version");
    const auto* action = value.find("action");
    if (!version || !version->is_number() || version->n != 1 || !action || !action->is_string())
        return false;
    Request parsed;
    parsed.action = action->s;
    if (parsed.action == "capabilities" || (parsed.action == "list" && value.o.size() == 2)) {
        if (value.o.size() != 2)
            return false;
    } else if (parsed.action == "list" || parsed.action == "remove") {
        const auto* instance = value.find("instance_id");
        const auto* generation = value.find("generation");
        if (!decimal(instance, true) || !decimal(generation, true))
            return false;
        parsed.instance = instance->s;
        parsed.generation = generation->s;
        if (parsed.action == "list") {
            const auto* cursor = value.find("cursor");
            if (value.o.size() != 5 || !decimal(cursor))
                return false;
            parsed.cursor = cursor->s;
        } else {
            const auto* ino = value.find("target_ino");
            const auto* dev = value.find("target_dev");
            if (value.o.size() != 6 || !decimal(ino) || !decimal(dev, false, UINT32_MAX))
                return false;
            parsed.ino = ino->s;
            parsed.dev = dev->s;
        }
    } else
        return false;
    request = std::move(parsed);
    return true;
}

inline bool is_manager_prefix(const std::vector<std::string>& args) {
    return args.size() >= 3 && args[0] == "sumh" && args[1] == "maps" && args[2] == "manager";
}

inline bool parse_arguments(const std::vector<std::string>& args, Request& request) {
    return is_manager_prefix(args) && args.size() == 5 && args[4] == "--channel" &&
           parse(args[3], request);
}
}  // namespace sumhp::temporary_maps
