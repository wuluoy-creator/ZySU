// Exercise complete production path resolution without requiring a real
// Android mount namespace. Invalid batches never replace; lost acknowledgements
// after a committed replacement must propagate an unknown outcome.
#include "core/json.hpp"
#include "mount/feature_result.hpp"
#include "mount/maps_history.hpp"
#include "mount/maps_snapshot.hpp"
#include "mount/runtime_apply.hpp"
#include "sumhp/config.hpp"
#include "sumhp/sumh_client.hpp"
#include "uapi/sumh.h"
#define SUMH_NAMESPACE_MODELED_CHILD
#include "sumh_namespace_test_support.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#define stat maps_test_stat
#undef S_ISREG
#define S_ISREG maps_test_is_regular
#undef major
#define major maps_test_major
#undef minor
#define minor maps_test_minor

struct stat {
    unsigned int st_mode = 1;
    std::uint64_t st_ino = 0;
    std::uint64_t st_dev = 0;
};
namespace {
std::map<std::string, struct stat> files;
bool replace_ok = true;
bool receipt_ok = true;
bool switch_ok = true;
bool automatic_allowed = true;
sumhp::RestoreTrigger recorded_trigger = sumhp::RestoreTrigger::Automatic;
int switch_calls = 0;
int replace_calls = 0;
int stat_calls = 0;
sumhp::sumh::FeatureCapabilities caps{true, 0, SUMH_FEATURE_MANAGED_MAPS};
std::vector<sumhp::sumh::MapsRule> installed{{99, 1, 88, 2, "/keep"}};
constexpr std::uint64_t host_dev(unsigned int device_major, unsigned int device_minor) {
    return (std::uint64_t(device_major) << 32) | device_minor;
}
}  // namespace
int stat(const char* path, struct stat* result) {
    ++stat_calls;
    const auto found = files.find(path);
    if (found == files.end()) {
        errno = ENOENT;
        return -1;
    }
    *result = found->second;
    return 0;
}
bool S_ISREG(unsigned int mode) {
    return mode == 1;
}
unsigned long major(std::uint64_t value) {
    return value >> 32;
}
unsigned long minor(std::uint64_t value) {
    return value & 0xffffffff;
}

namespace sumhp {
#include "sumh_maps_validate_under_test.inc"
}  // namespace sumhp
namespace sumhp::sumh {
bool runtime_state(RuntimeState&) {
    assert(false && "legacy fixture must not use runtime getter");
    return false;
}
bool set_runtime_feature(const RuntimeState&, RuntimeFeature, bool, std::uint32_t) {
    assert(false && "legacy fixture must not use conditional setter");
    return false;
}
FeatureCapabilities feature_capabilities() {
    return caps;
}
bool replace_managed_maps_rules(const std::vector<MapsRule>& rules) {
    ++replace_calls;
    if (!replace_ok) {
        errno = EIO;
        return false;
    }
    installed = rules;
    return true;
}
bool set_maps_spoof(bool) {
    ++switch_calls;
    if (!switch_ok)
        errno = EIO;
    return switch_ok;
}
}  // namespace sumhp::sumh
namespace sumhp::mount::sumh {
namespace fsutil = namespace_fixture;
MapsSubmissionTracker::MapsSubmissionTracker() {
    shared_ = new unsigned(0);
}
MapsSubmissionTracker::~MapsSubmissionTracker() {
    delete const_cast<unsigned*>(shared_);
}
bool MapsSubmissionTracker::valid() const {
    return true;
}
void MapsSubmissionTracker::mark(ManagedMapsRestoreState value) {
    *shared_ = static_cast<unsigned>(value);
}
ManagedMapsRestoreState MapsSubmissionTracker::state() const {
    return static_cast<ManagedMapsRestoreState>(*shared_);
}
bool replace_managed_maps_recorded(const std::vector<MapsPathRule>& paths,
                                   const std::vector<::sumhp::sumh::MapsRule>& identities,
                                   MapsSubmissionTracker& tracker, std::string& error,
                                   RestoreTrigger trigger) {
    recorded_trigger = trigger;
    assert(paths.size() == identities.size());
    if (!::sumhp::sumh::replace_managed_maps_rules(identities))
        return false;
    tracker.mark(ManagedMapsRestoreState::Replaced);
    if (!receipt_ok) {
        error = "partial: injected Maps receipt failure";
        errno = EIO;
        return false;
    }
    return true;
}
bool maps_automatic_restore_allowed(std::string& error) {
    if (!automatic_allowed) {
        error = "pending submission";
        errno = EBUSY;
    }
    return automatic_allowed;
}
bool restore_managed_maps_rules(const Config&, std::string&, ManagedMapsRestoreState* = nullptr,
                                RestoreTrigger = RestoreTrigger::Automatic);
bool apply_maps_config(const Config&, std::string&, bool, FeatureApplyOutcome*,
                       RestoreTrigger = RestoreTrigger::Automatic);
#include "sumh_maps_apply_under_test.inc"
#include "sumh_maps_identity_under_test.inc"
#include "sumh_maps_restore_under_test.inc"
}  // namespace sumhp::mount::sumh

int main() {
    using sumhp::mount::sumh::restore_managed_maps_rules;
    sumhp::Config config;
    std::string error;
    caps = {true, 0, 0};
    assert(restore_managed_maps_rules(config, error));
    assert(replace_calls == 0 && stat_calls == 0 && installed[0].target_ino == 99);
    config.maps_rules = {{"/target", "/replacement"}};
    assert(!restore_managed_maps_rules(config, error));
    assert(errno == EOPNOTSUPP && replace_calls == 0 && stat_calls == 0);
    config.maps_rules.clear();
    caps = {false, EACCES, 0};
    assert(!restore_managed_maps_rules(config, error));
    assert(errno == EACCES && replace_calls == 0);
    caps = {true, 0, SUMH_FEATURE_MANAGED_MAPS};
    config.maps_rules = {{"/target", "/replacement"}, {"/late", "/replacement"}};
    files["/target"] = {1, 123, host_dev(259, 4097)};
    files["/replacement"] = {1, 456, host_dev(8, 17)};
    assert(!restore_managed_maps_rules(config, error));
    assert(errno == ENOENT && replace_calls == 0 && installed[0].target_ino == 99);
    files["/late"] = {2, 789, host_dev(8, 1)};
    assert(!restore_managed_maps_rules(config, error));
    assert(errno == EINVAL && replace_calls == 0);
    files["/late"] = files["/target"];
    assert(!restore_managed_maps_rules(config, error));
    assert(errno == EEXIST && replace_calls == 0);
    files["/late"] = {1, 789, host_dev(8, 1)};
    namespace_fixture::switch_ok = false;
    error.clear();
    assert(!restore_managed_maps_rules(config, error));
    assert(!error.empty() && replace_calls == 0);
    namespace_fixture::switch_ok = true;
    replace_ok = false;
    assert(!restore_managed_maps_rules(config, error));
    assert(errno == EIO && replace_calls == 1 && installed[0].target_ino == 99);
    replace_ok = true;
    assert(restore_managed_maps_rules(config, error));
    assert(replace_calls == 2 && installed.size() == 2);
    assert(installed[0].target_ino == 123 && installed[0].target_dev == ((259UL << 20) | 4097));
    assert(installed[0].spoofed_ino == 456 && installed[0].spoofed_dev == ((8UL << 20) | 17));
    assert(installed[0].spoofed_path == "/replacement");
    files["/target"].st_ino = 321;
    assert(restore_managed_maps_rules(config, error));
    assert(installed[0].target_ino == 321 && replace_calls == 3);
    files["/target"].st_dev = host_dev(0x1000, 0);
    assert(!restore_managed_maps_rules(config, error));
    assert(errno == EOVERFLOW && replace_calls == 3);
    config.maps_rules.clear();
    assert(restore_managed_maps_rules(config, error));
    assert(replace_calls == 4 && installed.empty());

    // A completed callback that fails AFTER replacement is partial, not unchanged.
    receipt_ok = false;
    sumhp::mount::sumh::ManagedMapsRestoreState submitted;
    assert(!restore_managed_maps_rules(config, error, &submitted));
    assert(submitted == sumhp::mount::sumh::ManagedMapsRestoreState::Replaced);
    receipt_ok = true;

    // Cross the real production restore + namespace reply + Maps orchestrator.
    // The replace callback succeeds before reply/write/read/completion is lost.
    using namespace sumhp::mount::sumh;
    caps = {true, 0, SUMH_FEATURE_MANAGED_MAPS | SUMH_FEATURE_MAPS_SPOOF};
    config.enable_maps_spoof = true;
    config.maps_rules = {{"/target", "/replacement"}};
    files["/target"] = {1, 123, host_dev(8, 1)};
    FeatureApplyOutcome outcome;
    // A same-boot pending receipt blocks automatic switch and clear, too.
    automatic_allowed = false;
    for (bool enabled : {true, false}) {
        config.enable_maps_spoof = enabled;
        const int replacements_before = replace_calls, switches_before = switch_calls;
        assert(!apply_maps_config(config, error, true, &outcome));
        assert(outcome.state == FeatureApplyState::Failed && errno == EBUSY);
        assert(replace_calls == replacements_before && switch_calls == switches_before);
    }
    config.enable_maps_spoof = true;
    assert(apply_maps_config(config, error, true, &outcome, sumhp::RestoreTrigger::UserRequested));
    assert(recorded_trigger == sumhp::RestoreTrigger::UserRequested);
    automatic_allowed = true;
    for (bool* fault :
         {&namespace_fixture::drop_reply, &namespace_fixture::truncate_reply,
          &namespace_fixture::failed_read, &namespace_fixture::lost_child_completion}) {
        installed = {{99, 1, 88, 2, "/keep"}};
        const int replacements = replace_calls;
        const int switches = switch_calls;
        *fault = true;
        assert(!apply_maps_config(config, error, true, &outcome));
        assert(installed.size() == 1 && installed[0].target_ino == 123);
        assert(replace_calls == replacements + 1 && switch_calls == switches);
        assert(outcome.state == FeatureApplyState::Unknown);
        assert(error.find("may have committed") != std::string::npos);
        *fault = false;
    }
    // A complete negative reply remains a confirmed pre-commit rejection.
    const int replacements = replace_calls;
    const int switches = switch_calls;
    files.erase("/target");
    assert(!apply_maps_config(config, error, true, &outcome));
    assert(outcome.state == FeatureApplyState::Failed && errno == ENOENT);
    assert(replace_calls == replacements && switch_calls == switches);
    files["/target"] = {1, 123, host_dev(8, 1)};
    replace_ok = false;
    assert(!apply_maps_config(config, error, true, &outcome));
    assert(outcome.state == FeatureApplyState::Failed && errno == EIO);
    assert(replace_calls == replacements + 1 && switch_calls == switches);
    replace_ok = true;
    namespace_fixture::pipe_ok = false;
    assert(!apply_maps_config(config, error, true, &outcome));
    assert(outcome.state == FeatureApplyState::Failed && errno == EMFILE);
    assert(replace_calls == replacements + 1 && switch_calls == switches);
    namespace_fixture::pipe_ok = true;

    // On legacy kernels an empty saved list is accepted without replacement.
    // A switch error must not claim that any managed collection was committed.
    caps.bitmask = SUMH_FEATURE_MAPS_SPOOF;
    config.maps_rules.clear();
    switch_ok = false;
    assert(!apply_maps_config(config, error, true, &outcome));
    assert(outcome.state == FeatureApplyState::Unknown);
    assert(error.find("managed rules replaced") == std::string::npos);
    assert(replace_calls == replacements + 1 && switch_calls == switches + 1);
    std::cout << "sumh_maps_restore_test: all checks passed\n";
}
