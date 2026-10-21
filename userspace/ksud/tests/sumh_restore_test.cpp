// run_sumh_host_tests.py extracts the production restore function into this
// fixture so kernel transport, storage readiness and persisted state can fail
// independently without mounting or modifying a real device.
#include "core/restore_policy.hpp"
#include "core/user_hide_store.hpp"
#include "sumhp/sumh_client.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {
bool boot_completed = true;
bool available = true;
bool safe_mode = false;
bool external_owner = false;
bool ownership_query_ok = true;
bool managed = true;
bool rules_query_ok = true;
bool mutation_ok = true;
bool namespace_ok = true;
bool parent_ready = true;
bool persistence_ok = true;
bool definition_lock = false, lock_busy = false;
int enabled = 1;
std::vector<std::string> persisted;
std::vector<std::string> disabled;
std::vector<sumhp::sumh::UserHideRule> registered;
std::vector<std::string> mutations;

bool user_hide_restore_pending = false;

namespace logging {
enum class Level { Debug, Error };
}

void mlog(const std::string&, logging::Level = logging::Level::Debug) {}

namespace fsutil {
template <typename Callback>
bool run_in_init_mount_ns(Callback callback) {
    return namespace_ok && callback();
}
}  // namespace fsutil

namespace fs {
struct path {
    std::string value;
    explicit path(const std::string& value) : value(value) {}
    path parent_path() const { return path(value.substr(0, value.rfind('/'))); }
    const char* c_str() const { return value.c_str(); }
};
}  // namespace fs

struct stat {
    unsigned int st_mode;
};
int stat(const char*, struct stat* value) {
    value->st_mode = parent_ready ? 1 : 0;
    return parent_ready ? 0 : -1;
}
bool S_ISDIR(unsigned int mode) {
    return mode == 1;
}

void reset() {
    assert(!definition_lock);
    boot_completed = available = ownership_query_ok = managed = rules_query_ok = true;
    mutation_ok = namespace_ok = parent_ready = persistence_ok = true;
    enabled = 1;
    safe_mode = external_owner = false;
    persisted.clear();
    disabled.clear();
    lock_busy = false;
    registered.clear();
    mutations.clear();
    user_hide_restore_pending = false;
}
}  // namespace
#include "user_hide_digest_fixture.inc"

namespace sumhp::hide {
struct Transaction::Impl {
    Stored value;
    bool owns = false;
};
Transaction::Transaction() : impl_(std::make_unique<Impl>()) {}
Transaction::~Transaction() {
    if (impl_->owns)
        definition_lock = false;
}
bool Transaction::begin(std::string& error) {
    if (!persistence_ok || lock_busy || definition_lock) {
        error = "unavailable definitions or lock";
        return false;
    }
    definition_lock = true;
    impl_->owns = true;
    for (const auto& path : persisted)
        impl_->value.document.rules.push_back({std::string(32, 'a'), path, true, {}, {}, {}});
    for (const auto& path : disabled)
        impl_->value.document.rules.push_back({std::string(32, 'b'), path, false, {}, {}, {}});
    return true;
}
const Stored& Transaction::stored() const {
    assert(impl_->owns && definition_lock);
    return impl_->value;
}
}  // namespace sumhp::hide

namespace ksud {
bool is_safe_mode() {
    return safe_mode;
}
std::string getprop(const char*) {
    return boot_completed ? "1" : "0";
}
}  // namespace ksud

namespace sumhp {
std::string embedded_external_mount_owner() {
    return external_owner ? "external" : "";
}
}  // namespace sumhp

namespace sumhp::sumh {
bool is_available() {
    return available;
}
bool managed_hide_mode(bool& result) {
    assert(definition_lock);
    result = managed;
    return ownership_query_ok;
}
bool user_hide_rules(std::vector<UserHideRule>& output) {
    assert(definition_lock);
    output = registered;
    return rules_query_ok;
}
bool delete_user_hide(const std::string& path, std::uint64_t) {
    assert(definition_lock);
    mutations.push_back("delete:" + path);
    return mutation_ok;
}
bool upsert_user_hide(const std::string& path, UserHideRule*) {
    assert(definition_lock);
    mutations.push_back("upsert:" + path);
    return mutation_ok;
}
int enabled_state() {
    return enabled;
}
bool hide_path(const std::string& path) {
    assert(definition_lock);
    mutations.push_back("hide:" + path);
    return mutation_ok;
}
}  // namespace sumhp::sumh

namespace {
using sumhp::RestoreContext;
using sumhp::RestoreFeature;
using sumhp::RestoreTrigger;
using sumhp::should_restore;
#include "sumh_restore_under_test.inc"
}  // namespace

int main() {
    std::string error;
    // Automatic startup and event retries may not alter rules in safe mode.
    for (bool retry : {false, true}) {
        reset();
        safe_mode = true;
        persisted = {"/new"};
        registered = {{1, 0, 0, 0, 0, "/keep"}};
        user_hide_restore_pending = true;
        assert(restore_user_hide_rules(error, retry));
        assert(!user_hide_restore_pending && mutations.empty());

        reset();
        external_owner = true;
        persisted = {"/new"};
        registered = {{1, 0, 0, 0, 0, "/keep"}};
        assert(restore_user_hide_rules(error, retry));
        assert(mutations.empty());
    }

    // A deliberate request may synchronize user rules without taking ownership
    // of external module mounts, including while automatic recovery is blocked.
    reset();
    safe_mode = external_owner = true;
    persisted = {"/requested"};
    assert(restore_user_hide_rules(error, false, RestoreTrigger::UserRequested));
    assert((mutations == std::vector<std::string>{"upsert:/requested"}));
    mutations.clear();
    available = false;
    assert(!restore_user_hide_rules(error, false, RestoreTrigger::UserRequested));
    assert(!error.empty() && mutations.empty());
    available = true;
    boot_completed = false;
    assert(!restore_user_hide_rules(error, false, RestoreTrigger::UserRequested));
    assert(!error.empty() && mutations.empty());

    // Maps and kernel identity are independent of external module ownership.
    const RestoreContext external{false, true, true, true};
    assert(should_restore(external, RestoreFeature::Maps));
    assert(should_restore(external, RestoreFeature::KernelBuild));
    assert(!should_restore(external, RestoreFeature::UserHide));
    const RestoreContext early{false, false, true, false};
    assert(!should_restore(early, RestoreFeature::Maps));
    assert(should_restore(early, RestoreFeature::KernelBuild));
    assert(!should_restore(early, RestoreFeature::KernelBuild, RestoreTrigger::Automatic, true));
    for (auto feature :
         {RestoreFeature::UserHide, RestoreFeature::Maps, RestoreFeature::KernelBuild}) {
        assert(!should_restore({true, true, true, false}, feature));
        assert(!should_restore({false, true, false, false}, feature));
    }

    reset();
    boot_completed = false;
    user_hide_restore_pending = true;
    assert(restore_user_hide_rules(error, false));
    assert(!user_hide_restore_pending && mutations.empty());

    reset();
    persistence_ok = false;
    registered = {{1, 0, 0, 0, 0, "/keep"}};
    assert(!restore_user_hide_rules(error, false));
    assert(user_hide_restore_pending && mutations.empty());

    reset();
    ownership_query_ok = false;
    assert(!restore_user_hide_rules(error, false));
    assert(user_hide_restore_pending && mutations.empty());

    reset();
    rules_query_ok = false;
    assert(!restore_user_hide_rules(error, false));
    assert(user_hide_restore_pending && mutations.empty());

    reset();
    registered = {{1, 0, 0, 0, 0, "/stale"}, {2, 0, 0, 0, 0, "/keep"}};
    persisted = {"/keep", "/new", "/new"};
    assert(restore_user_hide_rules(error, false));
    assert(!user_hide_restore_pending);
    assert((mutations == std::vector<std::string>{"delete:/stale", "upsert:/new"}));

    mutations.clear();
    mutation_ok = false;
    assert(!restore_user_hide_rules(error, true));
    assert(user_hide_restore_pending);
    assert((mutations == std::vector<std::string>{"delete:/stale", "upsert:/new"}));

    mutations.clear();
    mutation_ok = true;
    assert(restore_user_hide_rules(error, true));
    assert(!user_hide_restore_pending);

    reset();
    managed = false;
    enabled = -1;
    assert(!restore_user_hide_rules(error, false));
    assert(user_hide_restore_pending && mutations.empty());

    enabled = 1;
    persisted = {"/parent/file"};
    parent_ready = false;
    assert(!restore_user_hide_rules(error, true));
    assert(user_hide_restore_pending && mutations.empty());

    parent_ready = true;
    assert(restore_user_hide_rules(error, true));
    assert(!user_hide_restore_pending);
    assert((mutations == std::vector<std::string>{"hide:/parent/file"}));
    for (bool retry : {false, true}) {
        reset();
        persisted = {"/enabled"};
        disabled = {"/disabled"};
        registered = {{1, 0, 0, 0, 0, "/disabled"}};
        assert(restore_user_hide_rules(error, retry));
        assert((mutations == std::vector<std::string>{"delete:/disabled", "upsert:/enabled"}) &&
               !definition_lock);
        mutations.clear();
        registered = {{2, 0, 0, 0, 0, "/enabled"}};
        assert(restore_user_hide_rules(error, true) && mutations.empty());
    }
    reset();
    persisted = {"/enabled"};
    lock_busy = true;
    assert(!restore_user_hide_rules(error, true) && mutations.empty() && !definition_lock &&
           user_hide_restore_pending);
    reset();
    managed = false;
    persisted = {"/enabled"};
    disabled = {"/disabled"};
    assert(restore_user_hide_rules(error, true) &&
           mutations == std::vector<std::string>{"hide:/enabled"} && !definition_lock);
    std::cout << "sumh_restore_test: all checks passed\n";
}
