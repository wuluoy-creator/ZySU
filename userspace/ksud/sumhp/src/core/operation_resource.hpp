#pragma once
#include "core/operation_response.hpp"
#include "core/state_ledger.hpp"

namespace sumhp::state {
inline constexpr std::size_t OPERATION_RESOURCE_MAX_BYTES = 512 * 1024;
inline constexpr std::size_t OPERATION_RESOURCE_ITEM_BYTES = 56 * 1024;
inline constexpr std::size_t OPERATION_RESOURCE_MAX_RECORDS = 16;
struct OperationResource {
    std::string id, revision, boot_id, name;
    int exit_code = 0, error_number = 0;
    JsonValue result;
};
using OperationResources = std::vector<OperationResource>;
bool make_operation_resource(const Operation& operation, const std::vector<std::string>& args,
                             const std::string& output, bool complete, int code, int number,
                             OperationResource& resource, std::string& error);
// Schema 1 contains hide receipts only. Schema 2 adds tracked temporary Maps
// receipts. The source version is returned only after complete validation.
bool parse_operation_resources(const std::string& bytes, OperationResources& resources,
                               std::string& error, int* source_schema = nullptr);
std::string encode_operation_resources(const OperationResources& resources);
bool operation_resource_matches(const OperationResource& resource, const Operation& operation);
OperationResponse query_operation_resource(const std::string& id);
}  // namespace sumhp::state
