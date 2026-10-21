#pragma once

#include <optional>
#include "core/json.hpp"
#include "core/restore_policy.hpp"
#include "mount/feature_result.hpp"
#include "sumhp/config.hpp"
#include "sumhp/sumh_client.hpp"

namespace sumhp::mount::sumh {

// Diagnostic evidence only: these identities must never be replayed as config.
struct MapsSubmissionIdentity {
    std::uint64_t target_ino = 0, target_dev = 0, spoofed_ino = 0, spoofed_dev = 0;
};
struct MapsSubmission {
    std::string id, boot_id, definition_revision, namespace_ino, supersedes_pending_id;
    std::string instance_id, generation_before, generation_after;
    std::string phase;  // pending, acknowledged, rejected
    int error = 0;
    std::vector<MapsPathRule> definitions;
    std::vector<MapsSubmissionIdentity> identities;
};
struct MapsHistory {
    std::optional<MapsSubmission> latest;
    std::optional<MapsSubmission> confirmed;
};
inline constexpr std::size_t MAPS_HISTORY_MAX_BYTES = 256 * 1024;
bool parse_maps_history(const std::string& bytes, MapsHistory& out, std::string& error);
std::string encode_maps_history(const MapsHistory& history);
JsonValue maps_submission_json(const MapsSubmission& entry);
// No directory creation, locks, normalization or recovery on read.
// valid / missing / unreadable / corrupt_or_unsupported; failure preserves out.
std::string read_maps_history(MapsHistory& out, std::string& error);
bool maps_automatic_restore_allowed(std::string& error);

// Tiny shared status, consumed only after the namespace helper confirms that
// its callback completed. Unknown child completion always overrides this slot.
class MapsSubmissionTracker {
public:
    MapsSubmissionTracker();
    ~MapsSubmissionTracker();
    MapsSubmissionTracker(const MapsSubmissionTracker&) = delete;
    MapsSubmissionTracker& operator=(const MapsSubmissionTracker&) = delete;
    bool valid() const;
    void mark(ManagedMapsRestoreState value);
    ManagedMapsRestoreState state() const;

private:
    volatile unsigned* shared_ = nullptr;
};

// Called inside the real mutation namespace callback AFTER resolving all
// paths. A durable pending record precedes the ioctl; the acknowledgement is
// recorded only after the kernel accepts these exact values. A post-commit
// recording failure returns false with tracker=Replaced, never Unchanged.
bool replace_managed_maps_recorded(const std::vector<MapsPathRule>& definitions,
                                   const std::vector<::sumhp::sumh::MapsRule>& identities,
                                   MapsSubmissionTracker& tracker, std::string& error,
                                   RestoreTrigger trigger = RestoreTrigger::Automatic);
}  // namespace sumhp::mount::sumh
