#pragma once

// Magic Mount backend: Magisk-style systemless mounting (tmpfs skeleton +
// bind-mount + MS_MOVE) over module partition trees. File binds and directory
// skeletons are tracked by a boot-scoped mount identity journal.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mount/backend.hpp"
#include "sumhp/config.hpp"

namespace sumhp::mount::magic {

// Mount every module's partition trees over the live filesystem. Must run inside
// the init mount namespace; records committed mount points to magic_mounts.list.
// The staging tmpfs is detached after commit; only final targets remain mounted.
bool mount_modules(const std::vector<ModuleEntry>& modules, const Config& config);

// Detach SUMHP's recorded mounts and clean skeletons. Must run in the init ns.
bool unmount_all(const Config& config);

std::vector<std::string> active_mounts(const Config& config);
bool is_active(const Config& config);

struct NormalizationObservation {
    std::string path, kind;
    std::uint64_t device = 0, inode = 0, mode = 0, size = 0;
    std::int64_t mtime_seconds = 0, mtime_nanoseconds = 0;
    std::int64_t ctime_seconds = 0, ctime_nanoseconds = 0;
    std::string link_target;
};
struct NormalizationAction {
    std::string kind, source, target, link_target, reason;
};
struct NormalizationPlan {
    static constexpr std::size_t max_actions = 32, max_observations = 64;
    static constexpr std::size_t max_bytes = std::size_t{64} * 1024;
    std::string requested_root, resolved_root;
    std::vector<NormalizationObservation> observations;
    std::vector<NormalizationAction> actions;
    std::size_t bytes = 0;
    bool complete = false;
};
struct NormalizationActionResult {
    std::size_t index = 0;
    std::string status;
    int error_number = 0;
    std::string detail;
    bool effect_observed = false;
    bool durability_confirmed = false;
};
struct NormalizationResult {
    bool complete = false, changed = false;
    std::string status;
    std::vector<NormalizationActionResult> actions;
    std::string recovery_hint;
};

// Root validation only; intentionally independent of normalization layout or
// platform aliases so inspect can diagnose those conflicts separately.
bool inspect_module_root(const std::string& module_path, std::string& resolved, std::string& error);
// Read-only checked plan. Errors preserve the caller's previous plan. Equal
// metadata does not establish a pinned content snapshot or exclude ABA races.
bool prepare_normalization(const std::string& module_path, NormalizationPlan& plan,
                           std::string& error);
// Revalidates the same plan, uses anchored no-follow parents and no-replace
// moves. Failure can leave a partial layout; never automatically roll it back.
bool execute_normalization(const NormalizationPlan& plan, NormalizationResult& result,
                           std::string& error);

// Install-time layout normalization (metainstall.sh): fold <module>/<p> into
// <module>/system/<p> for partitions exposed as /system/<p> symlinks.
bool normalize_module(const std::string& module_path);

}  // namespace sumhp::mount::magic
