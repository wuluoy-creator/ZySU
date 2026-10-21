#include "core/operation_query.hpp"
#include "core/state_ledger.hpp"

namespace sumhp::state {
bool parse_operation_query_arguments(const std::vector<std::string>& args,
                                     OperationQueryRequest& request, std::string& error) {
    error = "usage: ksud sumhp operation list | show <32-lowercase-hex-id> | result <id> | wait "
            "<id> <0..10000-ms>";
    OperationQueryRequest parsed;
    if (args.size() == 2 && args[0] == "operation" && args[1] == "list") {
        request = std::move(parsed);
        error.clear();
        return true;
    }
    if (args.size() < 3 || args[0] != "operation" || !valid_operation_id(args[2]))
        return false;
    parsed.id = args[2];
    if (args.size() == 3 && args[1] == "show")
        parsed.kind = OperationQueryRequest::Kind::Show;
    else if (args.size() == 3 && args[1] == "result")
        parsed.kind = OperationQueryRequest::Kind::Result;
    else if (args.size() == 4 && args[1] == "wait") {
        parsed.kind = OperationQueryRequest::Kind::Wait;
        const auto& value = args[3];
        if (value.empty() || value.size() > 5 || (value.size() > 1 && value.front() == '0'))
            return false;
        for (const char c : value) {
            if (c < '0' || c > '9')
                return false;
            parsed.timeout_ms = parsed.timeout_ms * 10 + static_cast<unsigned>(c - '0');
        }
        if (parsed.timeout_ms > OPERATION_WAIT_MAX_MS)
            return false;
    } else
        return false;
    request = std::move(parsed);
    error.clear();
    return true;
}
}  // namespace sumhp::state
