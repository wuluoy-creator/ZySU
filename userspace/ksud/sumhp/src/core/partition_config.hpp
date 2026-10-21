#pragma once
#include "core/json.hpp"
#include "sumhp/config.hpp"

namespace sumhp {
inline constexpr std::size_t PARTITIONS_INPUT_BYTES = 40 * 1024;
inline constexpr std::size_t PARTITIONS_MAX_NAMES = 128;
bool valid_config_revision(const std::string& revision);
// Strict safe rewrite preflight; reads do not migrate or change logger state.
bool parse_partition_config(const std::string& bytes, Config& config, std::string& error);
// Caller must enter init namespace. Lock, CAS, scan additions, durable save/readback.
JsonValue save_partitions_conditional(const std::string& revision, const std::string& array_json);
}  // namespace sumhp
