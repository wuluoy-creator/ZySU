#pragma once

#include <string>
#include <vector>
#include "core/command.hpp"
#include "core/json.hpp"
#include "core/operation_response.hpp"

namespace sumhp::state {
inline constexpr std::size_t OPERATION_INPUT_BYTES = 48 * 1024;
inline constexpr std::size_t OPERATION_OUTPUT_BYTES = 64 * 1024;
struct OperationSubmitRequest {
    bool execute = false;
    std::string id;
    std::string input_path;
};
struct OperationInput {
    std::vector<std::string> args;  // production parser's effective argv
    std::string canonical_document;
    std::string request_revision;  // domain-separated digest of the supplied argv
    std::string name;
};
bool parse_operation_submit_arguments(const std::vector<std::string>& args,
                                      OperationSubmitRequest& request, std::string& error);
bool parse_operation_input(const std::string& bytes, OperationInput& input, std::string& error);
bool read_operation_input(const std::string& path, std::string& bytes, std::string& error);
JsonValue operation_submission_failure(const std::string& id, const std::string& revision,
                                       const std::string& status, const std::string& error);
JsonValue operation_execution_json(const OperationInput& input, const CommandResult& result);
bool parse_operation_reply(const std::string& response, const std::string& id,
                           const std::string& revision, JsonValue& result);
int print_operation_submit(const OperationSubmitRequest& request);
OperationResponse submit_operation_bytes(bool execute, const std::string& id,
                                         const std::string& bytes);
// Receives one untrusted raw socket request; validates before any journal or mutation.
std::string execute_reserved_request(const std::vector<std::string>& request);
}  // namespace sumhp::state
