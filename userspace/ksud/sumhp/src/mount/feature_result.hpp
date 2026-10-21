#pragma once

#include <string>
#include <vector>

namespace sumhp::mount::sumh {

enum class ManagedMapsRestoreState { Unchanged, Replaced, Unknown };

enum class FeatureApplyState {
    // The setter/managed replacement acknowledged completion. This is not a
    // per-observer visibility claim. New runtime ABI also verifies flag state.
    Applied,
    Deferred,
    Unsupported,
    Failed,
    RolledBack,
    RollbackFailed,
    Partial,
    Unknown,
};

inline const char* feature_apply_state_name(FeatureApplyState state) {
    switch (state) {
    case FeatureApplyState::Applied:
        return "applied";
    case FeatureApplyState::Deferred:
        return "deferred";
    case FeatureApplyState::Unsupported:
        return "unsupported";
    case FeatureApplyState::Failed:
        return "failed";
    case FeatureApplyState::RolledBack:
        return "rolled_back";
    case FeatureApplyState::RollbackFailed:
        return "rollback_failed";
    case FeatureApplyState::Partial:
        return "partial";
    case FeatureApplyState::Unknown:
        return "outcome_unknown";
    }
    return "outcome_unknown";
}

struct FeatureApplyOutcome {
    std::string feature;
    FeatureApplyState state = FeatureApplyState::Deferred;
    int error_number = 0;
    std::string detail;
    // Legacy kernels have a structured getter only for Kernel Build. Do not
    // infer disabled states from absent/truncated LIST_RULES text.
    bool previous_state_known = false;
    bool previous_enabled = false;
};

struct FeatureApplyReport {
    std::vector<FeatureApplyOutcome> outcomes;

    bool ok() const {
        for (const auto& outcome : outcomes) {
            if (outcome.state != FeatureApplyState::Applied &&
                outcome.state != FeatureApplyState::Deferred)
                return false;
        }
        return true;
    }

    std::string summary() const {
        std::string result;
        for (const auto& outcome : outcomes) {
            if (!result.empty())
                result += "; ";
            result += outcome.feature + "=" + feature_apply_state_name(outcome.state);
            if (!outcome.detail.empty())
                result += " (" + outcome.detail + ")";
        }
        return result;
    }
};

}  // namespace sumhp::mount::sumh
