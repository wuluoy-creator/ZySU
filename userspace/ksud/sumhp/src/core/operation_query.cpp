#include "core/operation_query.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include "core/operation_resource.hpp"
#include "core/runtime.hpp"
#include "core/state_ledger.hpp"

namespace sumhp::state {
OperationResponse query_operation(const OperationQueryRequest& request) {
    if (request.kind == OperationQueryRequest::Kind::Result)
        return query_operation_resource(request.id);
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::milliseconds(request.timeout_ms);
    const auto first_boot = runtime_boot_id();
    for (;;) {
        Ledger ledger;
        std::string error;
        const auto ledger_status = read_operation_history(ledger, error);
        const auto current_boot = runtime_boot_id();
        auto root = JsonValue::object();
        root.o["schema_version"] = 2;
        root.o["resource"] = "operation_history";
        root.o["read_only"] = true;
        root.o["scope"] = "recorded_commands_in_caller_filesystem_namespace";
        root.o["runtime_verified"] = false;
        root.o["durability_confirmation"] = "not_observable_from_read";
        root.o["ledger_status"] = ledger_status;
        root.o["error"] = error;
        root.o["boot_id"] = current_boot.empty() ? JsonValue{} : JsonValue(current_boot);
        root.o["requested_id"] = request.id.empty() ? JsonValue{} : JsonValue(request.id);
        root.o["history_complete"] = false;  // Scope is bounded; not all mutations are journaled.
        root.o["history_pruned"] =
            ledger_status == "valid" ? JsonValue(ledger.history_pruned) : JsonValue{};
        root.o["retention_limit"] = static_cast<int>(LEDGER_MAX_OPERATIONS);
        root.o["records"] = JsonValue::array();
        const Operation* found = nullptr;
        if (ledger_status == "valid") {
            if (request.kind == OperationQueryRequest::Kind::List || ledger.last.id == request.id) {
                root.o["records"].a.push_back(operation_record_json(ledger.last, current_boot));
                if (ledger.last.id == request.id)
                    found = &ledger.last;
            }
            for (auto item = ledger.history.rbegin(); item != ledger.history.rend(); ++item) {
                if (request.kind == OperationQueryRequest::Kind::List || item->id == request.id)
                    root.o["records"].a.push_back(operation_record_json(*item, current_boot));
                if (item->id == request.id)
                    found = &*item;
            }
        }
        std::string status;
        int exit_code = 0;
        if (ledger_status != "valid" && ledger_status != "missing") {
            status = "unavailable";
            exit_code = 5;
        } else if (current_boot.empty() || first_boot.empty() || current_boot != first_boot) {
            status = "context_changed_or_unknown";
            exit_code = 5;
        } else if (request.kind == OperationQueryRequest::Kind::List)
            status = "observed";
        else if (!found) {
            status = "not_retained";
            exit_code = 3;
        } else if (request.kind != OperationQueryRequest::Kind::Wait || found->phase == "finished")
            status = "observed";
        else if (std::chrono::steady_clock::now() >= deadline) {
            status = "wait_timeout";
            exit_code = 4;
        } else {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining > std::chrono::steady_clock::duration::zero())
                std::this_thread::sleep_for(std::min(
                    remaining, std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                   std::chrono::milliseconds(OPERATION_POLL_MS))));
            continue;
        }
        root.o["status"] = status;
        root.o["wait_budget_ms"] = static_cast<int>(request.timeout_ms);
        root.o["elapsed_ms"] =
            static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started)
                                    .count());
        return {exit_code, std::move(root)};
    }
}
int print_operation_query(const OperationQueryRequest& request) {
    const auto result = query_operation(request);
    std::cout << stringify_json(result.value) << '\n';
    return std::cout.good() ? result.exit_code : 5;
}
}  // namespace sumhp::state
