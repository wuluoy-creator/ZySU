#pragma once

#include <cerrno>
#include <chrono>
#include <sstream>
#include "core/json.hpp"
#include "sumhp/sumh_client.hpp"
#include "uapi/sumh.h"

namespace sumhp {

struct RuntimeStatus {
    const char* status = "unknown";
    const char* reason = "not_queried";
    int error_number = 0;
    sumh::VersionInfo version;
    sumh::FeatureCapabilities capabilities;
    sumh::RuntimeState runtime;
    std::string started_ms;
    std::string finished_ms;
};

inline RuntimeStatus query_runtime_status() {
    RuntimeStatus result;
    const auto monotonic_ms = [] {
        return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count());
    };
    result.started_ms = monotonic_ms();
    result.version = sumh::version_info();
    if (result.version.status != sumh::Status::Available) {
        result.status = "unavailable";
        result.reason = result.version.status == sumh::Status::NotPresent ? "transport_unavailable"
                                                                          : "protocol_mismatch";
        result.error_number = result.version.last_errno;
    } else {
        result.capabilities = sumh::feature_capabilities();
        if (!result.capabilities.ok) {
            result.status = "error";
            result.reason = "capability_query_failed";
            result.error_number =
                result.capabilities.last_errno ? result.capabilities.last_errno : EIO;
        } else if (!(result.capabilities.bitmask & SUMH_FEATURE_RUNTIME_STATE)) {
            result.status = "unsupported";
            result.reason = "runtime_state_not_advertised";
        } else if (!sumh::runtime_state(result.runtime)) {
            result.status = "error";
            result.reason = "runtime_query_failed";
            result.error_number = errno ? errno : EIO;
        } else {
            result.status = "known";
            result.reason = "kernel_runtime_sample";
        }
    }
    result.finished_ms = monotonic_ms();
    return result;
}

inline std::string runtime_status_json(const RuntimeStatus& value, const std::string& boot_id) {
    const bool known = std::string(value.status) == "known";
    std::ostringstream out;
    out << "{\"schema_version\":1,\"status\":" << json_quote(value.status)
        << ",\"reason\":" << json_quote(value.reason) << ",\"errno\":" << value.error_number
        << ",\"source\":\"kernel_ioctl\",\"observation\":\"control_plane_only\""
        << ",\"sample_started_monotonic_ms\":" << json_quote(value.started_ms)
        << ",\"sample_finished_monotonic_ms\":" << json_quote(value.finished_ms)
        << ",\"boot_id\":" << (boot_id.empty() ? "null" : json_quote(boot_id))
        << ",\"protocol_status\":" << static_cast<int>(value.version.status)
        << ",\"kernel_protocol\":" << value.version.kernel_protocol << ",\"capabilities\":";
    if (value.capabilities.ok)
        out << value.capabilities.bitmask;
    else
        out << "null";
    out << ",\"conditional_set_supported\":";
    if (value.capabilities.ok)
        out << ((value.capabilities.bitmask & SUMH_FEATURE_RUNTIME_SET) ? "true" : "false");
    else
        out << "null";
    out << ",\"runtime\":";
    if (!known) {
        out << "null}";
        return out.str();
    }
    out << "{\"generation\":" << json_quote(std::to_string(value.runtime.generation))
        << ",\"instance_id\":" << json_quote(std::to_string(value.runtime.instance_id))
        << ",\"consistency\":\"control_serialized_readiness_sampled\""
        << ",\"generation_scope\":\"control_mutations_only\""
        << ",\"mount_hide_mode\":" << value.runtime.mount_hide_mode << ",\"features\":[";
    bool first = true;
    for (const auto& entry : value.runtime.entries) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"id\":" << static_cast<std::uint32_t>(entry.id)
            << ",\"name\":" << json_quote(sumh::runtime_feature_name(entry.id))
            << ",\"supported\":" << (entry.supported ? "true" : "false")
            << ",\"configured\":" << (entry.configured ? "true" : "false")
            << ",\"active\":" << (entry.active ? "true" : "false")
            << ",\"degraded\":" << (entry.degraded ? "true" : "false")
            << ",\"last_set_errno\":" << entry.last_error << '}';
    }
    out << "]}}";
    return out.str();
}

}  // namespace sumhp
