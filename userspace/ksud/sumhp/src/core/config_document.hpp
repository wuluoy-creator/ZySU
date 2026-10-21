#pragma once

#include "core/json.hpp"

namespace sumhp {

inline constexpr int CONFIG_SCHEMA_VERSION = 1;

// Version zero is the historical unversioned object. Parsing never writes or
// upgrades persistence; callers explicitly upgrade only after validation.
struct ConfigDocument {
    int schema_version = 0;
    JsonValue fields;
};

// Decode structure/version independently from runtime field validation. This
// also lets the narrow Kernel Build reset repair invalid known build fields.
bool parse_config_document(const std::string& input, ConfigDocument& document, std::string& error);
// Unknown fields can be inspected/read, but cannot safely be serialized by an
// older writer (including unknown numbers beyond JSON's double precision).
bool config_document_is_writable(const ConfigDocument& document, std::string& error);

}  // namespace sumhp
