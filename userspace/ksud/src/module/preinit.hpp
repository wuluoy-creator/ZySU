#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ksud::preinit {

struct Status {
    std::string state;
    std::string message;
};

using Modules = std::map<std::string, std::filesystem::path>;
using States = std::map<std::string, Status>;

// Metadata writes are bounded independently of available disk space.
constexpr unsigned long long LIMIT_TOTAL = 4ULL << 20;
constexpr unsigned long long RESERVE = 8ULL << 20;
constexpr unsigned BOOTCOUNT_LIMIT = 3;

int sync(const std::filesystem::path& directory,
         const std::vector<std::filesystem::path>& stale_directories, const Modules& modules,
         const char* context, bool supported, const std::filesystem::path& status_path);
States module_states(const std::filesystem::path& directory,
                     const std::filesystem::path& status_path);
int reset_bootcount(const std::filesystem::path& directory);
void on_boot_completed(const std::filesystem::path& directory);
void print_status(const std::filesystem::path& directory, const std::filesystem::path& status_path);

}  // namespace ksud::preinit
