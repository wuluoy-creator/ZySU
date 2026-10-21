#pragma once

#include <cerrno>
#include <cstdint>
#include <limits>
#include "mount/feature_result.hpp"
#include "sumhp/sumh_client.hpp"

namespace sumhp::mount::sumh {

// GET is evidence, not permission to overwrite another writer. Every mutation,
// including rollback, must compare the lifecycle identity and generation.
template <typename Read, typename Write>
FeatureApplyOutcome apply_runtime_feature(::sumhp::sumh::RuntimeFeature id, bool enable,
                                          std::uint32_t mode, Read read, Write write) {
    using ::sumhp::sumh::RuntimeFeature;
    using ::sumhp::sumh::RuntimeState;
    FeatureApplyOutcome result;
    result.feature = ::sumhp::sumh::runtime_feature_name(id);
    const auto fail = [&](FeatureApplyState state, int code, const char* detail) {
        result.state = state;
        result.error_number = code;
        result.detail = detail;
        return result;
    };
    RuntimeState before;
    if (!read(before))
        return fail(FeatureApplyState::Failed, errno ? errno : EIO,
                    "runtime preflight query failed; no mutation");
    const auto* previous = before.find(id);
    if (!previous)
        return fail(FeatureApplyState::Failed, EPROTO, "missing feature; no mutation");
    result.previous_state_known = true;
    result.previous_enabled = previous->configured;
    if (enable && !previous->supported)
        return fail(FeatureApplyState::Unsupported, EOPNOTSUPP,
                    "runtime readiness rejected enable; no mutation");
    // Avoid sequence rollover ambiguity. A fresh snapshot may be used by a
    // later explicit operation, never by retrying this mutation automatically.
    if (before.generation == std::numeric_limits<std::uint64_t>::max())
        return fail(FeatureApplyState::Failed, EOVERFLOW,
                    "runtime sequence exhausted; no mutation");
    const bool applied = write(before, id, enable, mode);
    const int apply_error = applied ? 0 : (errno ? errno : EIO);
    if (!applied && apply_error == ESTALE)
        return fail(FeatureApplyState::Failed, ESTALE,
                    "runtime state changed before apply; no mutation and no retry");
    RuntimeState after;
    if (!read(after))
        return fail(FeatureApplyState::Unknown, errno ? errno : EIO,
                    "runtime readback failed; no blind rollback or retry");
    const auto* current = after.find(id);
    if (!current || after.instance_id != before.instance_id ||
        after.generation != before.generation + 1)
        return fail(FeatureApplyState::Unknown, apply_error ? apply_error : ESTALE,
                    "runtime changed during verification; no rollback or retry");
    const auto matches = [&](bool expected, std::uint32_t expected_mode) {
        return current->configured == expected &&
               (id != RuntimeFeature::MountHide || after.mount_hide_mode == expected_mode);
    };
    if (!applied) {
        // An unsuccessful transport does not prove that this client produced
        // generation+1: another writer could have done so. Never roll it back.
        if (matches(previous->configured, before.mount_hide_mode))
            return fail(FeatureApplyState::Failed, apply_error,
                        "setter failed; previous setting observed; no rollback attempted");
        return fail(FeatureApplyState::Unknown, apply_error,
                    "setter failed with changed setting; ownership uncertain; no rollback");
    }
    if (matches(enable, mode)) {
        result.state = FeatureApplyState::Applied;
        result.detail = enable && !current->active
                            ? "setting verified, currently inactive; check global switch and "
                              "readiness/dependencies"
                            : "runtime setting verified; observer visibility not measured";
        return result;
    }
    if (after.generation == std::numeric_limits<std::uint64_t>::max())
        return fail(FeatureApplyState::Unknown, EOVERFLOW,
                    "verification failed at sequence boundary; no rollback");
    const std::uint32_t previous_mode =
        id == RuntimeFeature::MountHide ? before.mount_hide_mode : 0;
    const bool reverted = write(after, id, previous->configured, previous_mode);
    const int rollback_error = reverted ? 0 : (errno ? errno : EIO);
    if (!reverted)
        return fail(rollback_error == ESTALE ? FeatureApplyState::Unknown
                                             : FeatureApplyState::RollbackFailed,
                    rollback_error, "verification failed; conditional rollback rejected; no retry");
    RuntimeState restored;
    if (!read(restored))
        return fail(FeatureApplyState::Unknown, errno ? errno : EIO,
                    "rollback acknowledged but readback failed; no retry");
    const auto* restored_feature = restored.find(id);
    if (restored.instance_id != after.instance_id || restored.generation != after.generation + 1)
        return fail(FeatureApplyState::Unknown, ESTALE,
                    "runtime changed during rollback verification; no retry");
    if (!restored_feature || restored_feature->configured != previous->configured ||
        (id == RuntimeFeature::MountHide && restored.mount_hide_mode != before.mount_hide_mode))
        return fail(FeatureApplyState::RollbackFailed, EIO,
                    "previous setting could not be verified after rollback");
    return fail(FeatureApplyState::RolledBack, EIO,
                "requested setting mismatch; previous setting restored and verified");
}

inline FeatureApplyOutcome apply_runtime_feature(::sumhp::sumh::RuntimeFeature id, bool enable,
                                                 std::uint32_t mode = 0) {
    return apply_runtime_feature(id, enable, mode, ::sumhp::sumh::runtime_state,
                                 ::sumhp::sumh::set_runtime_feature);
}

}  // namespace sumhp::mount::sumh
