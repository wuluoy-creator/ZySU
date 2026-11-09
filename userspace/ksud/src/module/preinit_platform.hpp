#pragma once

#include <cstdint>
#include <filesystem>

namespace ksud::preinit::platform {
// Kept separate from planning/copying so host regressions can run without root.
void secure(const std::filesystem::path& path, unsigned mode, const char* context);
void sync_path(const std::filesystem::path& path);
void replace_file(const std::filesystem::path& from, const std::filesystem::path& to);
uint64_t available(const std::filesystem::path& path);
uint64_t allocated(const std::filesystem::path& path);
unsigned mode(const std::filesystem::path& path);
}  // namespace ksud::preinit::platform
