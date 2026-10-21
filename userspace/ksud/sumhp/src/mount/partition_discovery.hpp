#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include "core/json.hpp"

namespace sumhp::mount {

inline constexpr std::size_t PARTITION_DISCOVERY_RESPONSE_BYTES = 512 * 1024;

struct PartitionDiscovery {
    JsonValue output = JsonValue::object();
    // Deterministic bounded observations, not an atomic snapshot or content hash.
    std::string observation_signature;
    bool complete = false;
};

// Read-only installed-module discovery. No config loading, logging or writes.
// Failure still returns bounded partial evidence; callers must check complete
// and independently bracket config/boot and these observations for stability.
bool discover_extra_partitions(const std::vector<std::string>& saved, PartitionDiscovery& result,
                               std::string& error);

}  // namespace sumhp::mount
