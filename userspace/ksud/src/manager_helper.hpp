#pragma once
#include <cstdint>
#include <string>
namespace zysu::manager {
enum class Operation : uint32_t;
}
namespace ksud {
int manager_helper_main();
std::string manager_integrity(zysu::manager::Operation operation, const std::string& source);
}  // namespace ksud
