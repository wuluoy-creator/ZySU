#pragma once

#include <cstddef>
#include <string>

namespace sumhp {
inline constexpr std::size_t CAPABILITY_SNAPSHOT_RESPONSE_BYTES = 64 * 1024;
// Fixed read-only snapshot, bounded namespace worker; never starts a daemon,
// creates persistence, restores features or infers runtime module ownership.
std::string capability_snapshot_json();
}  // namespace sumhp
