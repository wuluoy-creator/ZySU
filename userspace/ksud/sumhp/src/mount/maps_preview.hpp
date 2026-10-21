#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace sumhp::mount::sumh {

inline constexpr std::size_t MAPS_PREVIEW_MAX_BYTES = 512 * 1024;

// A diagnostic observation only. This never prepares runtime directories,
// restores a daemon, writes a receipt/configuration, or applies sampled IDs.
// Semantic failures are represented in the versioned JSON, including missing
// configuration (which must not be interpreted as an empty saved set).
std::string maps_preview_json();
bool is_maps_preview_command(const std::vector<std::string>& args);

}  // namespace sumhp::mount::sumh
