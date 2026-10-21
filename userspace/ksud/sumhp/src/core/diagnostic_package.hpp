#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "../../../../common/diagnostic_package_request.hpp"
#include "core/json.hpp"

namespace sumhp::diagnostic_package {
inline constexpr std::size_t package_limit = 96 * 1024;
inline constexpr std::size_t bank_limit = 1024 * 1024;
inline constexpr std::size_t package_count_limit = 8;

// Pure, allowlisted conversion of the bounded doctor schema. Never saves raw
// collector bytes or an alias reverse map. Future schemas fail closed.
bool build_package(const std::string& report, bool include_identifiers, JsonValue& output,
                   std::string& reason);
// Strict canonical package parser used for every stored record read/export.
bool parse_package(const std::string& bytes, JsonValue& output, std::string& reason);
bool valid_id(const std::string& id);
bool valid_revision(const std::string& revision);

struct Reply {
    int exit_code = 5;
    JsonValue value;
};
// Queries do not mkdir, chmod, migrate, sample, lock, or repair the store.
// show returns package + package_revision; expected_revision is optional for
// preview, mandatory at the controller's export boundary.
Reply list_packages();
Reply show_package(const std::string& id, const std::string& expected_revision = {});
// IDs are immutable. A lost create reply is resolved with show(id), never an
// automatic resample or overwrite. Remove affects exactly one reviewed record.
Reply create_package(const std::string& id, const JsonValue& package);
Reply remove_package(const std::string& id, const std::string& expected_revision);

int command(const std::vector<std::string>& args);
// Private Manager dispatch. The strict channel parser supplies this request;
// business exit status is inside the compact JSON, independent of transport.
std::string manager_dispatch(const diagnostic_package_manager::Request& input);
}  // namespace sumhp::diagnostic_package
