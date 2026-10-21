#pragma once

namespace sumhp::mount::fsutil {

// Describes delivery of the callback result, not whether its writes were atomic.
// CallbackFailed may only be treated as unchanged when the callback itself
// guarantees rejection before commit. Unknown must never trigger blind replay.
enum class NamespaceOperationState { NotStarted, Completed, CallbackFailed, Unknown };

}  // namespace sumhp::mount::fsutil
