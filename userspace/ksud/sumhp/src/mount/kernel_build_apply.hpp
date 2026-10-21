#pragma once

#include <algorithm>
#include <cerrno>
#include <limits>
#include "mount/feature_result.hpp"
#include "sumhp/sumh_client.hpp"
#include "uapi/sumh.h"

namespace sumhp::mount::sumh {

inline bool kernel_build_values_equal(const ::sumhp::sumh::KernelBuild& left,
                                      const ::sumhp::sumh::KernelBuild& right) {
    return left.enabled == right.enabled && left.release == right.release &&
           left.version == right.version;
}

// Caller decides scheduling and negotiates capabilities. No automatic replay,
// and no restoration of a previous value without proven CAS ownership.
inline FeatureApplyOutcome apply_kernel_build_config(
    const ::sumhp::sumh::KernelBuild& requested, const ::sumhp::sumh::FeatureCapabilities& caps) {
    using ::sumhp::sumh::KernelBuild;
    using ::sumhp::sumh::KernelBuildSnapshot;
    FeatureApplyOutcome result;
    result.feature = "kernel_build_spoof";
    const auto fail = [&](FeatureApplyState state, int code, const char* detail) {
        result.state = state;
        result.error_number = code;
        result.detail = detail;
        return result;
    };
    if (!caps.ok)
        return fail(FeatureApplyState::Failed, caps.last_errno ? caps.last_errno : EIO,
                    "capability query failed; no Kernel Build mutation");
    if (!(caps.bitmask & SUMH_FEATURE_KERNEL_BUILD_SPOOF))
        return fail(
            requested.enabled ? FeatureApplyState::Unsupported : FeatureApplyState::Deferred,
            requested.enabled ? EOPNOTSUPP : 0, "Kernel Build unavailable; no mutation");
    const auto valid_field = [](const std::string& text) {
        return text.size() < SUMH_KERNEL_BUILD_MAX &&
               std::none_of(
                   text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
    };
    if (requested.enabled &&
        ((!valid_field(requested.release) || !valid_field(requested.version)) ||
         (requested.release.empty() && requested.version.empty()) ||
         requested.release.find(' ') != std::string::npos))
        return fail(FeatureApplyState::Failed, EINVAL, "invalid Kernel Build draft; no mutation");
    const int required = SUMH_FEATURE_RUNTIME_STATE | SUMH_FEATURE_KERNEL_BUILD_CAS;
    const bool conditional = (caps.bitmask & required) == required;
    const bool need_original =
        !requested.enabled || requested.release.empty() || requested.version.empty();
    KernelBuildSnapshot before;
    if (conditional) {
        if (!::sumhp::sumh::kernel_build_snapshot(before, need_original))
            return fail(FeatureApplyState::Failed, errno ? errno : EIO,
                        "coherent Kernel Build preflight failed; no mutation or retry");
        if (before.runtime.generation == std::numeric_limits<std::uint64_t>::max())
            return fail(FeatureApplyState::Failed, EOVERFLOW,
                        "control sequence exhausted; no mutation");
    } else {
        if (!::sumhp::sumh::kernel_build(before.current) ||
            (need_original && !::sumhp::sumh::original_kernel_build(before.original)))
            return fail(FeatureApplyState::Failed, errno ? errno : EIO,
                        "Kernel Build preflight query failed; no mutation");
    }
    result.previous_state_known = true;
    result.previous_enabled = before.current.enabled;
    KernelBuild desired = requested;
    if (!desired.enabled || desired.release.empty())
        desired.release = before.original.release;
    if (!desired.enabled || desired.version.empty())
        desired.version = before.original.version;
    const bool applied =
        conditional
            ? ::sumhp::sumh::set_kernel_build_conditional(before.runtime, desired)
            : ::sumhp::sumh::set_kernel_build(desired.enabled, desired.release, desired.version);
    const int apply_error = applied ? 0 : (errno ? errno : EIO);
    if (!applied && conditional && apply_error == ESTALE)
        return fail(FeatureApplyState::Failed, ESTALE,
                    "Kernel Build changed before apply; no mutation or retry");
    KernelBuildSnapshot after;
    if (conditional) {
        if (!::sumhp::sumh::kernel_build_snapshot(after, false))
            return fail(FeatureApplyState::Unknown, errno ? errno : EIO,
                        "Kernel Build readback not coherent; no rollback or retry");
        if (after.runtime.instance_id != before.runtime.instance_id ||
            after.runtime.generation != before.runtime.generation + 1)
            return fail(FeatureApplyState::Unknown, apply_error ? apply_error : ESTALE,
                        "Kernel Build changed during verification; no rollback or retry");
    } else if (!::sumhp::sumh::kernel_build(after.current)) {
        return fail(FeatureApplyState::Unknown, errno ? errno : EIO,
                    "legacy Kernel Build readback failed; ownership unknown, no rollback");
    }
    if (!applied) {
        if (kernel_build_values_equal(after.current, before.current))
            return fail(FeatureApplyState::Failed, apply_error,
                        "setter failed; previous values observed; ownership unknown, no rollback");
        return fail(FeatureApplyState::Unknown, apply_error,
                    "setter failed with changed values; ownership unknown, no rollback");
    }
    if (kernel_build_values_equal(after.current, desired)) {
        result.state = FeatureApplyState::Applied;
        result.detail = conditional
                            ? "Kernel Build pair verified at own control generation"
                            : "legacy Kernel Build pair observed; concurrent ownership not proven";
        return result;
    }
    if (!conditional)
        return fail(FeatureApplyState::Unknown, EIO,
                    "legacy Kernel Build verification mismatch; no unsafe rollback");
    if (after.runtime.generation == std::numeric_limits<std::uint64_t>::max())
        return fail(FeatureApplyState::Unknown, EOVERFLOW, "sequence boundary; no rollback");
    const bool reverted =
        ::sumhp::sumh::set_kernel_build_conditional(after.runtime, before.current);
    if (!reverted) {
        const int code = errno ? errno : EIO;
        return fail(code == ESTALE ? FeatureApplyState::Unknown : FeatureApplyState::RollbackFailed,
                    code, "Kernel Build conditional rollback rejected; no retry");
    }
    KernelBuildSnapshot restored;
    if (!::sumhp::sumh::kernel_build_snapshot(restored, false))
        return fail(FeatureApplyState::Unknown, errno ? errno : EIO,
                    "Kernel Build rollback readback not coherent; no retry");
    if (restored.runtime.instance_id != after.runtime.instance_id ||
        restored.runtime.generation != after.runtime.generation + 1)
        return fail(FeatureApplyState::Unknown, ESTALE,
                    "Kernel Build changed during rollback verification; no retry");
    if (!kernel_build_values_equal(restored.current, before.current))
        return fail(FeatureApplyState::RollbackFailed, EIO,
                    "previous Kernel Build pair not verified after rollback");
    return fail(FeatureApplyState::RolledBack, EIO,
                "previous Kernel Build pair restored and verified");
}

}  // namespace sumhp::mount::sumh
