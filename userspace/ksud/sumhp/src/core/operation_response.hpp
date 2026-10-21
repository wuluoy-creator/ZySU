#pragma once
#include "core/json.hpp"
namespace sumhp::state {
struct OperationResponse {
    int exit_code = 1;
    JsonValue value;
};
}  // namespace sumhp::state
