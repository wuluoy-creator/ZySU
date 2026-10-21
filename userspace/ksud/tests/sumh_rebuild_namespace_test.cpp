// Test actual namespace outcome bridge and reset functions, with no device calls.
#include "core/log.hpp"
#include "mount/module_rebuild.hpp"
#include "mount/namespace_result.hpp"

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#define CHECK(expr)   \
    do {              \
        ++checks;     \
        assert(expr); \
    } while (false)
namespace model {
using sumhp::mount::sumh::ModuleRebuildOutcome;
int checks = 0;
int cases = 0;
bool allowed = true, allocation_ok = true, prepare_ok = true;
bool marker_ok = true, mkdir_ok = true, clear_ok = true;
int fail_setter = -1, fail_mapping = -1, fail_hide = -1;
int allocations = 0, releases = 0, namespace_calls = 0, prepare_calls = 0;
int recorded_calls = 0, marker_calls = 0, invalidations = 0, mapping_calls = 0, hide_calls = 0;
int callback_errno = 0;
bool pending_hide = true;
std::vector<std::string> sequence;
enum class Namespace { Normal, NotStarted, MissingBefore, MissingAfter };
Namespace ns = Namespace::Normal;
enum class Recorded { Execute, Reject, Partial, AppliedReceiptFailure };
Recorded recorded = Recorded::Execute;
void reset() {
    ++cases;
    allowed = allocation_ok = prepare_ok = marker_ok = mkdir_ok = clear_ok = true;
    fail_setter = fail_mapping = fail_hide = -1;
    allocations = releases = namespace_calls = prepare_calls = recorded_calls = marker_calls = 0;
    invalidations = mapping_calls = hide_calls = callback_errno = 0;
    pending_hide = true;
    sequence.clear();
    ns = Namespace::Normal;
    recorded = Recorded::Execute;
    errno = 0;
}
bool setter(int index, const char* name, bool enabled) {
    CHECK(!enabled);
    sequence.emplace_back(name);
    if (fail_setter == index) {
        errno = EACCES;
        return false;
    }
    errno = 0;
    return true;
}
}  // namespace model

// Portable stand-ins for mmap: production allocation/cleanup control flow is
// exercised, but no real cross-process visibility claim is made by this suite.
inline constexpr int PROT_READ = 1, PROT_WRITE = 2, MAP_SHARED = 4, MAP_ANONYMOUS = 8;
#define MAP_FAILED reinterpret_cast<void*>(-1)
void* test_mmap(void*, std::size_t size, int protection, int flags, int fd, int offset) {
    using namespace model;
    ++allocations;
    CHECK(size == sizeof(ModuleRebuildOutcome));
    CHECK(protection == (PROT_READ | PROT_WRITE) && flags == (MAP_SHARED | MAP_ANONYMOUS));
    CHECK(fd == -1 && offset == 0);
    if (!allocation_ok) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return std::malloc(size);
}
int test_munmap(void* value, std::size_t size) {
    using namespace model;
    ++releases;
    CHECK(size == sizeof(ModuleRebuildOutcome));
    std::free(value);
    return 0;
}

namespace fixture_fs {
using std::filesystem::path;
bool create_directories(const path&, std::error_code& error) {
    if (!model::mkdir_ok)
        error = std::make_error_code(std::errc::permission_denied);
    return model::mkdir_ok;
}
}  // namespace fixture_fs

namespace sumhp::sumh {
bool set_debug(bool v) {
    return model::setter(0, "debug", v);
}
bool set_stealth(bool v) {
    return model::setter(1, "stealth", v);
}
bool set_mount_hide(bool v) {
    return model::setter(2, "mount_hide", v);
}
bool set_maps_spoof(bool v) {
    return model::setter(3, "maps", v);
}
bool set_statfs_spoof(bool v) {
    return model::setter(4, "statfs", v);
}
bool set_kernel_build(bool v) {
    return model::setter(5, "build", v);
}
bool clear_rules() {
    model::sequence.emplace_back("clear");
    errno = model::clear_ok ? 0 : EBUSY;
    return model::clear_ok;
}
bool add_rule(const std::string& path, const std::string& source, int type) {
    using namespace model;
    CHECK(path == "/visible/file" && source == "/source/file" && type == 0);
    const int index = mapping_calls++;
    sequence.emplace_back("add");
    errno = fail_mapping == index ? ENOSPC : 0;
    return fail_mapping != index;
}
bool add_merge_rule(const std::string& path, const std::string& source) {
    using namespace model;
    CHECK(path == "/visible/dir" && source == "/source/dir");
    const int index = mapping_calls++;
    sequence.emplace_back("merge");
    errno = fail_mapping == index ? ENOMEM : 0;
    return fail_mapping != index;
}
bool hide_path(const std::string& path) {
    using namespace model;
    CHECK(path == "/visible/hide");
    const int index = hide_calls++;
    sequence.emplace_back("hide");
    errno = fail_hide == index ? EACCES : 0;
    return fail_hide != index;
}
}  // namespace sumhp::sumh

namespace sumhp::mount::fsutil {
bool run_in_init_mount_ns(const std::function<bool()>& callback, std::string& error,
                          NamespaceOperationState* state) {
    using namespace model;
    ++namespace_calls;
    if (ns == Namespace::NotStarted || ns == Namespace::MissingBefore) {
        *state = ns == Namespace::NotStarted ? NamespaceOperationState::NotStarted
                                             : NamespaceOperationState::Unknown;
        error = "injected missing child";
        errno = EIO;
        return false;
    }
    const bool ok = callback();
    callback_errno = errno;
    if (ns == Namespace::MissingAfter) {
        *state = NamespaceOperationState::Unknown;
        error = "injected lost reply";
        errno = EIO;
        return false;
    }
    *state = ok ? NamespaceOperationState::Completed : NamespaceOperationState::CallbackFailed;
    return ok;
}
}  // namespace sumhp::mount::fsutil

namespace sumhp::mount::sumh {
namespace fs = fixture_fs;
bool& user_hide_restore_pending = model::pending_hide;
void mlog(const std::string&, logging::Level) {
    errno = EDOM;
}
fs::path active_file() {
    return "/not-used/active";
}
bool write_boot_marker(const fs::path&) {
    ++model::marker_calls;
    return model::marker_ok;
}
void invalidate_active_state() {
    ++model::invalidations;
    errno = EDOM;
}
bool module_rebuild_allowed(std::string& error) {
    if (!model::allowed) {
        error = "injected guard";
        errno = EBUSY;
    }
    return model::allowed;
}
bool prepare_modules(const std::vector<ModuleEntry>& modules, const Config&, const ModuleRuleMap&,
                     ModuleMountPlan& plan, std::string& error) {
    using namespace model;
    ++prepare_calls;
    CHECK(modules.size() == 2 && modules[0].id == "first" && modules[1].id == "second");
    if (!prepare_ok) {
        error = "injected planner failure";
        errno = EIO;
        return false;
    }
    plan.mappings = {{"/visible/file", "/source/file", false},
                     {"/visible/dir", "/source/dir", true}};
    plan.hide = {"/visible/hide"};
    return true;
}
bool rebuild_modules_recorded(const ModuleMountPlan& plan, const std::vector<std::string>& ids,
                              const std::function<bool(std::string&)>& reset_fn,
                              const std::function<bool(const ModuleMountPlan::Mapping&)>& add,
                              const std::function<bool(const std::string&)>& hide,
                              ModuleRebuildOutcome& outcome, std::string& error) {
    using namespace model;
    ++recorded_calls;
    CHECK(ids == std::vector<std::string>({"first", "second"}));
    CHECK(plan.mappings.size() == 2 && plan.hide.size() == 1);
    if (recorded == Recorded::Reject) {
        outcome = ModuleRebuildOutcome::Unchanged;
        errno = EBUSY;
        return false;
    }
    outcome = ModuleRebuildOutcome::PartialOrUnknown;
    if (recorded == Recorded::Partial) {
        errno = EIO;
        return false;
    }
    if (!reset_fn(error))
        return false;
    for (const auto& rule : plan.mappings)
        if (!add(rule))
            return false;
    for (const auto& path : plan.hide)
        if (!hide(path))
            return false;
    outcome = ModuleRebuildOutcome::Applied;
    if (recorded == Recorded::AppliedReceiptFailure) {
        error = "injected receipt failure";
        errno = EIO;
        return false;
    }
    return true;
}
#define mmap test_mmap
#define munmap test_munmap
#include "sumh_rebuild_namespace_under_test.inc"
#undef mmap
#undef munmap
}  // namespace sumhp::mount::sumh

int main() {
    using namespace model;
    using namespace sumhp::mount;
    using namespace sumhp::mount::sumh;
    std::string error;
    for (int setter = 0; setter != 6; ++setter) {
        reset();
        fail_setter = setter;
        CHECK(!reset_runtime_state(error));
        CHECK(sequence.size() == static_cast<std::size_t>(setter + 1));
        CHECK(sequence.back() != "clear" && invalidations == 1 && !pending_hide && errno == EACCES);
    }
    reset();
    clear_ok = false;
    CHECK(!reset_runtime_state(error));
    CHECK(sequence.size() == 7 && sequence.back() == "clear");
    CHECK(errno == EBUSY && invalidations == 1);
    reset();
    CHECK(reset_runtime_state(error));
    CHECK(sequence == std::vector<std::string>(
                          {"debug", "stealth", "mount_hide", "maps", "statfs", "build", "clear"}));
    CHECK(invalidations == 1 && !pending_hide);

    const std::vector<ModuleEntry> modules{{"first", "/modules/first"},
                                           {"second", "/modules/second"}};
    const sumhp::Config config;
    const ModuleRuleMap rules;
    ModuleRebuildOutcome outcome = ModuleRebuildOutcome::Applied;
    const auto run = [&] {
        error.clear();
        return rebuild_modules_in_init_ns(modules, config, rules, outcome, error);
    };
    reset();
    allowed = false;
    CHECK(!run());
    CHECK(outcome == ModuleRebuildOutcome::Unchanged && !allocations && !namespace_calls &&
          !recorded_calls);
    reset();
    allocation_ok = false;
    CHECK(!run());
    CHECK(outcome == ModuleRebuildOutcome::Unchanged && allocations == 1 && !releases &&
          !namespace_calls);
    reset();
    ns = Namespace::NotStarted;
    CHECK(!run());
    CHECK(outcome == ModuleRebuildOutcome::Unchanged && !prepare_calls && !recorded_calls &&
          releases == 1);
    reset();
    ns = Namespace::MissingBefore;
    CHECK(!run());
    CHECK(outcome == ModuleRebuildOutcome::PartialOrUnknown && !prepare_calls && !recorded_calls &&
          releases == 1);
    reset();
    prepare_ok = false;
    CHECK(!run());
    CHECK(outcome == ModuleRebuildOutcome::Unchanged && prepare_calls == 1 && !recorded_calls &&
          sequence.empty());
    reset();
    recorded = Recorded::Reject;
    CHECK(!run());
    CHECK(outcome == ModuleRebuildOutcome::Unchanged && recorded_calls == 1 && sequence.empty() &&
          !marker_calls);
    reset();
    recorded = Recorded::Partial;
    CHECK(!run());
    CHECK(outcome == ModuleRebuildOutcome::PartialOrUnknown && !marker_calls);
    reset();
    recorded = Recorded::AppliedReceiptFailure;
    CHECK(!run());
    CHECK(outcome == ModuleRebuildOutcome::Applied && !marker_calls && mapping_calls == 2 &&
          hide_calls == 1);
    reset();
    CHECK(run());
    CHECK(outcome == ModuleRebuildOutcome::Applied && marker_calls == 1 && releases == 1);
    CHECK(sequence == std::vector<std::string>({"debug", "stealth", "mount_hide", "maps", "statfs",
                                                "build", "clear", "add", "merge", "hide"}));
    reset();
    marker_ok = false;
    CHECK(!run());
    CHECK(outcome == ModuleRebuildOutcome::Applied && marker_calls == 1 && callback_errno == EIO);
    reset();
    mkdir_ok = false;
    CHECK(!run());
    CHECK(outcome == ModuleRebuildOutcome::Applied && !marker_calls && callback_errno == EIO);
    reset();
    fail_mapping = 0;
    CHECK(!run());
    CHECK(outcome == ModuleRebuildOutcome::PartialOrUnknown && mapping_calls == 1 && !hide_calls &&
          !marker_calls && callback_errno == ENOSPC);
    reset();
    fail_mapping = 1;
    CHECK(!run());
    CHECK(outcome == ModuleRebuildOutcome::PartialOrUnknown && mapping_calls == 2 && !hide_calls &&
          !marker_calls && callback_errno == ENOMEM);
    reset();
    fail_hide = 0;
    CHECK(!run());
    CHECK(outcome == ModuleRebuildOutcome::PartialOrUnknown && mapping_calls == 2 &&
          hide_calls == 1 && !marker_calls && callback_errno == EACCES);
    reset();
    ns = Namespace::MissingAfter;
    recorded = Recorded::Reject;
    CHECK(!run());
    CHECK(outcome == ModuleRebuildOutcome::PartialOrUnknown && sequence.empty() && !marker_calls);
    reset();
    ns = Namespace::MissingAfter;
    CHECK(!run());
    CHECK(outcome == ModuleRebuildOutcome::PartialOrUnknown && marker_calls == 1 &&
          mapping_calls == 2);
    reset();
    ns = Namespace::MissingAfter;
    recorded = Recorded::Partial;
    CHECK(!run());
    CHECK(outcome == ModuleRebuildOutcome::PartialOrUnknown && !marker_calls);
    std::cout << "sumh_rebuild_namespace_test: " << cases << " scenarios, " << checks
              << " assertions passed (production extraction; host boundary doubles)\n";
}
