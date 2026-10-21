#pragma once
#include "core/operation_response.hpp"

#include <string>
#include <vector>

namespace sumhp::state {
inline constexpr unsigned OPERATION_WAIT_MAX_MS = 10000;
inline constexpr unsigned OPERATION_POLL_MS = 100;
struct OperationQueryRequest {
    enum class Kind { List, Show, Wait, Result };
    Kind kind = Kind::List;
    std::string id;
    unsigned timeout_ms = 0;
};
bool parse_operation_query_arguments(const std::vector<std::string>& args,
                                     OperationQueryRequest& request, std::string& error);
// Local read-only route. Exit: 0 observed, 3 not retained, 4 timeout,
// 5 unavailable/context changed. A failed command can still be observed (0).
int print_operation_query(const OperationQueryRequest& request);
OperationResponse query_operation(const OperationQueryRequest& request);
}  // namespace sumhp::state
