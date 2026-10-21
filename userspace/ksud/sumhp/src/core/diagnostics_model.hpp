#pragma once

#include <optional>
#include "core/json.hpp"
#include "userspace/common/diagnostics_request.hpp"

namespace sumhp::diagnostics {
enum class SourceKind {
    HideSelection,
    ModulePlan,
    ModuleRebuild,
    MapsPreview,
    RecoveryLegacy,
    Operations,
    OperationSelected
};
struct Sample {
    SourceKind kind = SourceKind::RecoveryLegacy;
    std::string started_ms, finished_ms;
    int query_exit_code = 0;
    std::string bytes;
    std::string failure;
    int error_number = 0;
};
struct Context {
    std::string boot_before, boot_after;
    std::string namespace_dev, namespace_ino;
    bool namespace_verified = false;
    bool namespace_consistent = true;
    std::string started_ms, finished_ms;
};
struct Sources {
    Context context;
    Sample primary;
    std::optional<Sample> rebuild;
    Sample operations;
    std::optional<Sample> selected_operation;
};
struct Report {
    int exit_code = 5;
    JsonValue value;
};
Report diagnose(const Request&, const Sources&);
Report unavailable(const Request*, const char* reason, int error_number);
std::string encode_report(const Report&);
}  // namespace sumhp::diagnostics
