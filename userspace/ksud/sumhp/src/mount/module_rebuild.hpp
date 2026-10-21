#pragma once

#include <functional>
#include <optional>
#include "core/json.hpp"
#include "core/operation_response.hpp"
#include "mount/sumh.hpp"

namespace sumhp::mount::sumh {

enum class ModuleRebuildOutcome { Unchanged, Applied, PartialOrUnknown };
inline constexpr std::size_t MODULE_REBUILD_MAX_BYTES = 512 * 1024;
inline constexpr std::size_t MODULE_REBUILD_MAX_ACTIONS = 4096;

// Diagnostic inputs and acknowledged call prefixes, never ownership authority,
// pinned source objects, a runtime snapshot, or automatic replay configuration.
struct ModuleRebuildRecord {
    std::string id, boot_id;
    std::vector<std::string> module_ids;
    ModuleMountPlan plan;
    std::string phase = "pending";  // pending, acknowledged, partial_or_unknown
    bool reset_acknowledged = false;
    std::size_t mappings_acknowledged = 0, hides_acknowledged = 0;
    std::string failed_stage;  // empty, reset, mapping, hide, exception
    int failed_index = -1;
    int error_number = 0;
};
struct ModuleRebuildHistory {
    std::optional<ModuleRebuildRecord> latest;
    std::optional<ModuleRebuildRecord> acknowledged;
};
// Strict, bounded, duplicate-key-rejecting format. Failure preserves output.
bool parse_module_rebuild_history(const std::string& bytes, ModuleRebuildHistory& out,
                                  std::string& error);
// Empty output means invalid data or encoded capacity exceeded.
std::string encode_module_rebuild_history(const ModuleRebuildHistory& history);
JsonValue module_rebuild_record_json(const ModuleRebuildRecord& record);
// Read only: valid / missing / unreadable / corrupt_or_unsupported.
std::string read_module_rebuild_history(ModuleRebuildHistory& out, std::string& error);
bool module_rebuild_allowed(std::string& error);
int print_module_rebuild_status();
state::OperationResponse query_module_rebuild_status();

// Durable pending precedes reset. Stops at the first failed mutator and never
// cleans up, rolls back, retries or resumes. Negative legacy calls may mutate.
// The caller must treat an unconfirmed namespace callback as PartialOrUnknown,
// regardless of an intermediate shared outcome value.
bool rebuild_modules_recorded(const ModuleMountPlan& plan,
                              const std::vector<std::string>& module_ids,
                              const std::function<bool(std::string&)>& reset,
                              const std::function<bool(const ModuleMountPlan::Mapping&)>& add,
                              const std::function<bool(const std::string&)>& hide,
                              ModuleRebuildOutcome& outcome, std::string& error);

}  // namespace sumhp::mount::sumh
