#pragma once

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <string>
#include <vector>

namespace sumhp::maps_temporary {

inline bool is_prefix(const std::vector<std::string>& args) {
    return args.size() >= 3 && args[0] == "sumh" && args[1] == "maps" &&
           (args[2] == "remove" || args[2] == "list-temporary");
}

struct DeleteRequest {
    std::uint64_t ino = 0, dev = 0, instance = 0, generation = 0;
};

inline bool parse_delete(const std::vector<std::string>& args, DeleteRequest& request) {
    if (args.size() != 9 || !is_prefix(args) || args[2] != "remove" || args[5] != "--instance" ||
        args[7] != "--generation")
        return false;
    const auto number = [](const std::string& text, std::uint64_t& value) {
        if (text.empty() || text.size() > 20 ||
            text.find_first_not_of("0123456789") != std::string::npos)
            return false;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    };
    DeleteRequest parsed;
    if (!number(args[3], parsed.ino) || !number(args[4], parsed.dev) ||
        !number(args[6], parsed.instance) || !number(args[8], parsed.generation) ||
        parsed.dev > UINT32_MAX || !parsed.instance || !parsed.generation)
        return false;
    request = parsed;
    return true;
}

// These are pre-publication kernel rejections. Other failures after submission
// can be transport failures; report unknown and require an explicit new query.
inline bool rejected_before_publication(int error) {
    return error == ESTALE || error == ENOENT || error == EINVAL || error == EOVERFLOW ||
           error == ENOMEM || error == ESHUTDOWN || error == EFAULT || error == EOPNOTSUPP ||
           error == ENOTTY || error == EACCES || error == EPERM;
}

inline const char* error_code(int error, bool attempted) {
    if (!error)
        return "deleted";
    if (error == ESTALE)
        return "snapshot_changed";
    if (error == ENOENT && attempted)
        return "temporary_key_not_found";
    if (error == EOPNOTSUPP || error == ENOTTY)
        return "unsupported";
    if (error == EINVAL)
        return "invalid_request";
    if (error == EOVERFLOW)
        return "key_not_representable";
    if (error == ENOMEM)
        return "insufficient_memory";
    if (error == EACCES || error == EPERM)
        return "permission_denied";
    if (error == ESHUTDOWN)
        return "runtime_stopped";
    if (error == EFAULT)
        return "invalid_user_memory";
    return attempted ? "outcome_unknown" : "capability_query_failed";
}

// The opt-in tracked workflow has one reconstructable spelling. Legacy CLI
// parsing above deliberately retains its existing leading-zero compatibility.
inline std::vector<std::string> delete_arguments(const DeleteRequest& request) {
    return {"sumh",
            "maps",
            "remove",
            std::to_string(request.ino),
            std::to_string(request.dev),
            "--instance",
            std::to_string(request.instance),
            "--generation",
            std::to_string(request.generation)};
}

inline bool parse_tracked_delete(const std::vector<std::string>& args, DeleteRequest& request) {
    DeleteRequest parsed;
    if (!parse_delete(args, parsed) || args != delete_arguments(parsed))
        return false;
    request = parsed;
    return true;
}

}  // namespace sumhp::maps_temporary
