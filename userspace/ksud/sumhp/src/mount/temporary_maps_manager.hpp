#pragma once

#include <stdexcept>
#include "maps_temporary.hpp"
#include "userspace/common/temporary_maps_request.hpp"

namespace sumhp::temporary_maps {
// Called only by the root-checked private channel after its parent ACK.
// Existing local commands retain their result/exit code and never use a daemon.
inline std::string dispatch(const Request& request) {
    json::Value result;
    int exit_code = 0;
    if (request.action == "capabilities") {
        errno = 0;
        // The legacy features() helper folds query failures into zero. Use the
        // structured query so unavailable reads remain unknown, not unsupported.
        const auto capabilities = sumh::feature_capabilities();
        const int bits = capabilities.ok ? capabilities.bitmask : -1;
        const int error = bits < 0 ? (capabilities.last_errno > 0 && capabilities.last_errno <= 4095
                                          ? capabilities.last_errno
                                          : EIO)
                                   : 0;
        result = json::Value::object();
        result["schema_version"] = 1;
        result["resource"] = "temporary_maps_capabilities";
        result["inventory"] = bits < 0                             ? "unknown"
                              : bits & SUMH_FEATURE_RULE_INVENTORY ? "supported"
                                                                   : "unsupported";
        result["delete"] =
            bits < 0 ? "unknown"
            : (bits & SUMH_FEATURE_RULE_INVENTORY) && (bits & SUMH_FEATURE_TEMPORARY_MAPS_DELETE)
                ? "supported"
                : "unsupported";
        result["errno"] = error;
        exit_code = error ? 1 : 0;
    } else {
        std::vector<std::string> args{"sumh", "maps"};
        if (request.action == "list") {
            args.push_back("list-temporary");
            if (!request.instance.empty())
                args.insert(args.end(), {request.instance, request.generation, request.cursor});
        } else if (request.action == "remove") {
            args.insert(args.end(), {"remove", request.ino, request.dev, "--instance",
                                     request.instance, "--generation", request.generation});
        } else
            throw std::invalid_argument("invalid temporary Maps action");
        std::ostringstream out, err;
        exit_code = maps_temporary::command(args, out, err);
        const auto bytes = out.str();
        std::string error;
        if (!out.good() || bytes.size() > response_bytes || (exit_code != 0 && exit_code != 1) ||
            !json::Parser(bytes, true).parse(result, error) || !result.is_object())
            throw std::runtime_error("incomplete temporary Maps result");
    }
    auto value = json::Value::object();
    value["schema_version"] = 1;
    value["resource"] = "temporary_maps_dispatch";
    value["action"] = request.action;
    value["exit_code"] = exit_code;
    value["result"] = std::move(result);
    return json::dump(value);
}
}  // namespace sumhp::temporary_maps
