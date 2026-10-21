#pragma once
#include <functional>
#include "operation_response.hpp"
#include "user_hide_input.hpp"

namespace sumhp::hide {
inline constexpr std::size_t HIDE_STATUS_PAGE = 16;
inline constexpr std::size_t HIDE_EXPORT_PAGE = 64;
inline constexpr std::size_t HIDE_RESULT_ITEMS = 64;
// status is an exact local query; mutations run inside the existing journal.
int print_status(const Request& request);
state::OperationResponse query_status(const Request& request);
int print_definitions(const Request& request);
int print_export();
int run_definition_command(const std::vector<std::string>& args);
int run_path_command(const std::vector<std::string>& args);
// Bounded stdout/stderr receipt for a validated private-channel callback.
// A successful dispatch envelope alone never proves the inner mutation applied.
std::string manager_dispatch_json(const std::function<int()>& dispatch);
}  // namespace sumhp::hide
