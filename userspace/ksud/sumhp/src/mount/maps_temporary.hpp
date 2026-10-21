#pragma once

#include <ostream>
#include "core/rule_inventory.hpp"
#include "userspace/common/temporary_maps_command.hpp"

namespace sumhp::maps_temporary {

// Only the exact root-checked local route calls this. No daemon, restoration,
// persistent configuration, shell or implicit retry. A reserved local capture
// may supply its Journal ID; direct CLI and Manager callers retain the legacy receipt.
inline int command(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                   const std::string& operation_id = {}) {
    if (is_prefix(args) && args[2] == "list-temporary") {
        std::vector<std::string> query{"api", "inventory", "maps_temporary"};
        query.insert(query.end(), args.begin() + 3, args.end());
        return inventory::command(query, "", out, false);
    }
    DeleteRequest request;
    const bool valid = parse_delete(args, request);
    bool attempted = false;
    errno = 0;
    const bool deleted =
        valid && sumh::delete_temporary_maps_rule(request.ino, request.dev, request.instance,
                                                  request.generation, attempted);
    const int error = deleted ? 0 : valid ? (errno ? errno : EIO) : EINVAL;
    const bool unknown = attempted && !deleted && !rejected_before_publication(error);
    out << "{\"schema_version\":1,\"resource\":"
        << json_quote(operation_id.empty() ? "temporary_maps" : "temporary_maps_mutation")
        << ",\"action\":\"remove\""
        << ",\"status\":"
        << json_quote(deleted   ? "deleted"
                      : unknown ? "outcome_unknown"
                                : "not_applied")
        << ",\"error_code\":" << json_quote(error_code(error, attempted)) << ",\"errno\":" << error
        << ",\"saved\":false,\"ioctl_attempted\":" << (attempted ? "true" : "false")
        << ",\"acknowledged\":" << (deleted ? "true" : "false")
        << ",\"operation_id\":" << (operation_id.empty() ? "null" : json_quote(operation_id))
        << ",\"history_supported\":" << (operation_id.empty() ? "false" : "true")
        << ",\"observer_verified\":false,\"request\":";
    if (!valid)
        out << "null";
    else
        out << "{\"target_ino\":" << json_quote(std::to_string(request.ino))
            << ",\"target_dev\":" << json_quote(std::to_string(request.dev))
            << ",\"expected_instance_id\":" << json_quote(std::to_string(request.instance))
            << ",\"expected_generation\":" << json_quote(std::to_string(request.generation)) << '}';
    out << "}\n";
    if (!deleted)
        err << "temporary Maps removal: " << error_code(error, attempted)
            << "; query list-temporary before a new explicit request\n";
    out.flush();
    errno = out.good() ? error : EIO;
    return deleted && out.good() ? 0 : 1;
}

}  // namespace sumhp::maps_temporary
