#pragma once

#include <cstddef>
#include <string>

namespace sumhp::logging {

enum class Level { Debug, Info, Warning, Error };

inline constexpr std::size_t file_limit = 1024 * 1024;
inline constexpr std::size_t tail_limit = 512 * 1024;
inline constexpr std::size_t component_limit = 256;
inline constexpr std::size_t message_limit = 8192;

void set_debug_enabled(bool value) noexcept;
bool enabled(Level level) noexcept;
void write(Level level, const std::string& component, const std::string& message) noexcept;
// Best-effort summaries on orderly shutdown; no timer or background flush.
void flush() noexcept;
bool prepare_boot_log(std::string& error, const std::string& boot_id = {});

// A wall-clock timestamp when Android has set the clock; otherwise an explicit
// early-boot marker based on CLOCK_BOOTTIME. This avoids presenting boot-time
// seconds as misleading dates in 1970.
std::string timestamp();

// Compatibility entry point for informational operation messages.
void append(const std::string& component, const std::string& message);

}  // namespace sumhp::logging
