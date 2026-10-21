#pragma once

namespace sumhp {

enum class RestoreFeature { UserHide, Maps, KernelBuild };
enum class RestoreTrigger { Automatic, UserRequested };

struct RestoreContext {
    bool safe_mode;
    bool boot_completed;
    bool kernel_available;
    bool external_mount_owner;
};

// A policy decision never probes or mutates the device. Callers supply fresh
// state at the restore boundary; explicit requests do not inherit the automatic
// safe-mode/owner exemptions, but still require a ready kernel and boot stage.
constexpr bool should_restore(const RestoreContext& context, RestoreFeature feature,
                              RestoreTrigger trigger = RestoreTrigger::Automatic,
                              bool kernel_build_late = false) {
    if (!context.kernel_available || (trigger == RestoreTrigger::Automatic && context.safe_mode))
        return false;
    if (feature == RestoreFeature::KernelBuild)
        return trigger == RestoreTrigger::UserRequested || !kernel_build_late ||
               context.boot_completed;
    if (!context.boot_completed)
        return false;
    return feature != RestoreFeature::UserHide || trigger == RestoreTrigger::UserRequested ||
           !context.external_mount_owner;
}

}  // namespace sumhp
