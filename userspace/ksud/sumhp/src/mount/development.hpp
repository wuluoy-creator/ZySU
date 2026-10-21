#pragma once
#include <string>
#include <vector>
#include "core/json.hpp"
#include "mount/magic_mount.hpp"

namespace sumhp::mount {
inline constexpr std::size_t DEVELOPMENT_RESPONSE_BYTES = 512 * 1024;
enum class DevelopmentKind { Inspect, NormalizePreview, PartitionDiscovery, PartitionSave };
struct DevelopmentRequest {
    DevelopmentKind kind = DevelopmentKind::Inspect;
    std::string module_path, revision, partitions;
    bool channel = false;
};
bool is_development_prefix(const std::vector<std::string>& args);
bool parse_development_arguments(const std::vector<std::string>& args, DevelopmentRequest& request,
                                 std::string& error);
std::string development_query_json(const DevelopmentRequest& request);
std::string partition_save_json(const DevelopmentRequest& request);
JsonValue normalization_plan_json(const magic::NormalizationPlan& plan);
JsonValue normalize_module_result_json(const std::string& path);
// Observation only: caller supplies bounded init-namespace execution.
JsonValue module_inspect_observation(const std::string& path);
}  // namespace sumhp::mount
