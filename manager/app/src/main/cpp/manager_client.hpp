#pragma once
#include "userspace/common/manager_protocol.hpp"

namespace zysu::manager {
std::string call(Operation operation,
                 const std::vector<std::string> &fields = {});
} // namespace zysu::manager
