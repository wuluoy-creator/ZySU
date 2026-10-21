#pragma once

#include "userspace/common/diagnostics_request.hpp"

namespace sumhp::diagnostics {
// Public input is read before the fixed self-exec transport. No daemon dispatch.
int doctor_command(const std::vector<std::string>& args);
// Shared validated standalone self-exec acquisition. No stdout or storage;
// preserves doctor response/status and never samples directly in the parent.
std::string query_diagnostics(const std::string& bytes);
// Dedicated, root-checked transport child only; never a query_worker callback.
std::string collect_diagnostics(const Request& request);
// Bounded file/stdin read. Never executes or expands input; failure preserves bytes.
bool read_input(const std::string& path, std::string& bytes, std::string& error);
}  // namespace sumhp::diagnostics
