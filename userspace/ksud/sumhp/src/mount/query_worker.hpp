#pragma once

#include <functional>

namespace sumhp::mount::fsutil {

// Read-only callbacks only. True confirms a successful child exit and reply;
// false/errno means the caller must discard all shared sample contents.
// Parent budgets five monotonic seconds including setup, subject to syscall
// scheduling. A stuck/unreaped child blocks further samples with EBUSY, not a
// queue or additional children. No mutation may use this cancellable helper.
// Requires kernel pidfd + P_PIDFD waitid support; unsupported/denied setup
// returns unknown. Without a delivered pidfd, the single pending slot stays
// quarantined (EBUSY) until daemon exit; numeric PID reaping is never attempted.
bool run_query_in_init_mount_ns(const std::function<bool()>& fn);

}  // namespace sumhp::mount::fsutil
