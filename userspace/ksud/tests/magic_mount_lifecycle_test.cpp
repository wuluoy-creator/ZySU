// Production orchestration and cleanup run verbatim. Only their system,
// traversal, filesystem and kernel registration boundaries are simulated.
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

constexpr unsigned long MS_PRIVATE = 1UL << 18;
constexpr unsigned long MS_REC = 1UL << 14;
constexpr int MNT_DETACH = 2;

struct MountRecord {
    std::string path;
    std::uint64_t mount_id = 0;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
};
struct Node {};
struct ModuleEntry {};
struct Config {
    std::string work_dir = "/dev/sumhp-work";
    std::string mount_source = "sumhp";
    std::vector<std::string> partitions;
};
struct Walk {
    std::vector<std::string> committed;
    int files = 0;
    int tmpfs_dirs = 0;
    int symlinks = 0;
};

namespace fixture {
enum class Traversal { Success, Failure, Exception, UnknownException };
struct State {
    std::map<std::string, std::uint64_t> mounts;
    std::vector<MountRecord> journal;
    std::vector<std::vector<std::string>> journal_writes;
    std::map<std::string, int> registrations;
    std::vector<std::string> registration_attempts;
    std::vector<std::string> detach_attempts;
    std::vector<std::string> events;
    std::set<int> fail_journal_writes;
    std::map<std::string, int> fail_detaches;
    std::string fail_registration;
    std::string fail_unregistration;
    std::vector<std::string> targets = {
        "/system/lib", "/system/lib64/libmodule.so", "/vendor/lib/libmodule.so", "/vendor/lib64",
        "/system/etc", "/system/etc/hosts",
    };
    Traversal traversal = Traversal::Success;
    std::uint64_t next_mount_id = 1;
    int mount_calls = 0;
    int fail_mount_call = 0;
    int journal_calls = 0;
    int fail_after_targets = 2;
    bool fail_prepare = false;
    bool fail_read = false;
    bool fail_mkdir = false;
};
State state;

void add_mount(const std::string& path) {
    assert(state.mounts.count(path) == 0);
    state.mounts[path] = state.next_mount_id++;
}

using std::filesystem::path;
void create_directories(const path&, std::error_code& ec) {
    ec = state.fail_mkdir ? std::make_error_code(std::errc::permission_denied) : std::error_code{};
}
bool remove(const path&, std::error_code& ec) {
    ec.clear();
    state.journal.clear();
    return true;
}
}  // namespace fixture
namespace fs = fixture;

namespace logging {
enum class Level { Info, Error };
void write(Level, const std::string&, const std::string&) {}
}  // namespace logging
namespace sumh {
bool register_managed_mount(const std::string& path) {
    fixture::state.registration_attempts.push_back(path);
    fixture::state.events.push_back("register:" + path);
    if (fixture::state.fail_registration == path)
        return false;
    fixture::state.registrations[path] = MNT_DETACH;
    return true;
}
bool unregister_managed_mount(const std::string& path) {
    if (fixture::state.fail_unregistration == path)
        return false;
    fixture::state.registrations.erase(path);
    return true;
}
}  // namespace sumh
#include "embedded_umount_under_test.inc"

fs::path runtime_data_dir() {
    return fs::path("/data/adb/sumhp");
}
int mount(const char*, const char* target, const char*, unsigned long flags, const void*) {
    auto& state = fixture::state;
    if (++state.mount_calls == state.fail_mount_call) {
        errno = EIO;
        return -1;
    }
    if (flags == 0)
        fixture::add_mount(target);
    else
        assert(flags == (MS_PRIVATE | MS_REC));
    return 0;
}
int umount2(const char* target, int flags) {
    auto& state = fixture::state;
    assert(flags == MNT_DETACH);
    state.detach_attempts.emplace_back(target);
    state.events.push_back(std::string("detach:") + target);
    if (state.fail_detaches[target] > 0) {
        --state.fail_detaches[target];
        errno = EBUSY;
        return -1;
    }
    if (!state.mounts.erase(target)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

namespace fsutil {
void mlog(const std::string&, logging::Level = logging::Level::Info) {}
bool prepare_empty_mountpoint(const std::string&) {
    return !fixture::state.fail_prepare;
}
bool register_umount(const std::string& path) {
    return embedded_register_umount(path);
}
bool unregister_umount(const std::string& path) {
    return embedded_unregister_umount(path);
}
bool read_mount_journal(const std::string&, std::vector<MountRecord>& records, bool& legacy) {
    records = fixture::state.journal;
    legacy = false;
    return !fixture::state.fail_read;
}
bool write_mount_journal(const std::string&, const std::vector<std::string>& mounts) {
    auto& state = fixture::state;
    state.events.push_back("journal");
    state.journal_writes.push_back(mounts);
    if (state.fail_journal_writes.count(++state.journal_calls))
        return false;
    std::vector<MountRecord> records;
    for (const auto& path : mounts) {
        const auto live = state.mounts.find(path);
        if (live == state.mounts.end())
            return false;
        records.push_back({path, live->second, 1, 1});
    }
    state.journal = std::move(records);
    return true;
}
bool mount_matches(const MountRecord& record) {
    const auto live = fixture::state.mounts.find(record.path);
    return live != fixture::state.mounts.end() && live->second == record.mount_id;
}
}  // namespace fsutil
using fsutil::mlog;

std::set<std::string> mounts_with_source(const std::string&) {
    throw std::logic_error("these fixtures use the current journal format");
}
std::optional<Node> collect_module_files(const std::vector<ModuleEntry>& modules,
                                         const std::vector<std::string>&) {
    return modules.empty() ? std::nullopt : std::optional<Node>{Node{}};
}
bool do_mount(Node&, const std::string&, const std::string&, bool, Walk& walk) {
    auto& state = fixture::state;
    for (const auto& path : state.targets) {
        fixture::add_mount(path);
        walk.committed.push_back(path);
        ++walk.files;
        if (walk.files == state.fail_after_targets) {
            if (state.traversal == fixture::Traversal::Failure)
                return false;
            if (state.traversal == fixture::Traversal::Exception)
                throw std::runtime_error("injected traversal exception");
            if (state.traversal == fixture::Traversal::UnknownException)
                throw 1;
        }
    }
    return true;
}
#include "magic_mount_under_test.inc"

void reset() {
    fixture::state = {};
}
bool mount_one() {
    return mount_modules({ModuleEntry{}}, Config{});
}
void expect_clean() {
    assert(fixture::state.mounts.empty());
    assert(fixture::state.registrations.empty());
    assert(fixture::state.journal.empty());
}

int main() {
    const Config config;
    auto& state = fixture::state;
    reset();
    assert(mount_modules({}, config));
    expect_clean();
    assert(state.mount_calls == 0);

    // Libraries, their descendants and nested mount points obey the same
    // unmount policy. Only published module targets survive a successful mount.
    reset();
    assert(mount_one());
    assert(state.mounts.size() == state.targets.size());
    assert(state.journal.size() == state.targets.size());
    assert(state.registrations.size() == state.targets.size());
    assert(state.mounts.count(config.work_dir) == 0);
    for (const auto& path : state.targets) {
        assert(state.registrations.at(path) == MNT_DETACH);
        assert(std::any_of(state.journal.begin(), state.journal.end(),
                           [&](const auto& entry) { return entry.path == path; }));
    }
    const auto detached =
        std::find(state.events.begin(), state.events.end(), "detach:" + config.work_dir);
    const auto registered =
        std::find_if(state.events.begin(), state.events.end(),
                     [](const auto& event) { return event.rfind("register:", 0) == 0; });
    assert(detached < registered);
    assert(unmount_all(config));
    expect_clean();
    const auto child =
        std::find(state.detach_attempts.begin(), state.detach_attempts.end(), "/system/etc/hosts");
    const auto parent =
        std::find(state.detach_attempts.begin(), state.detach_attempts.end(), "/system/etc");
    assert(child < parent);

    // Setup errors must never reach traversal or kernel registration.
    for (int failure = 0; failure < 6; ++failure) {
        reset();
        switch (failure) {
        case 0:
            state.fail_read = true;
            break;
        case 1:
            state.fail_mkdir = true;
            break;
        case 2:
            state.fail_prepare = true;
            break;
        case 3:
            state.fail_mount_call = 1;
            break;
        case 4:
            state.fail_mount_call = 2;
            break;
        case 5:
            state.fail_journal_writes.insert(1);
            break;
        }
        assert(!mount_one());
        expect_clean();
        assert(state.registration_attempts.empty());
    }

    // Setup rollback can itself fail before the initial journal exists.
    // Preserve the scratch mount for a subsequent cleanup attempt.
    for (const bool journal_failure : {false, true}) {
        reset();
        if (journal_failure)
            state.fail_journal_writes.insert(1);
        else
            state.fail_mount_call = 2;
        state.fail_detaches[config.work_dir] = 1;
        assert(!mount_one());
        assert(state.mounts.size() == 1 && state.mounts.count(config.work_dir) == 1);
        assert(state.journal.size() == 1 && state.journal.front().path == config.work_dir);
        assert(state.registration_attempts.empty());
        assert(unmount_all(config));
        expect_clean();
    }

    // Traversal can fail after publishing some targets, including by exception.
    for (const auto failure : {fixture::Traversal::Failure, fixture::Traversal::Exception,
                               fixture::Traversal::UnknownException}) {
        reset();
        state.traversal = failure;
        assert(!mount_one());
        expect_clean();
        assert(state.registration_attempts.empty());
    }

    reset();
    state.fail_detaches[config.work_dir] = 1;
    assert(!mount_one());
    expect_clean();
    assert(state.registration_attempts.empty());

    reset();
    state.fail_journal_writes.insert(2);
    assert(!mount_one());
    expect_clean();
    assert(state.registration_attempts.empty());

    // A partial registration failure must remove earlier successful entries.
    reset();
    state.fail_registration = state.targets.at(2);
    assert(!mount_one());
    expect_clean();

    // A rollback detach can fail too. Preserve that mount's identity for a
    // later cleanup even when the final journal update itself was what failed.
    reset();
    const auto survivor = state.targets.front();
    state.fail_journal_writes.insert(2);
    state.fail_detaches[survivor] = 1;
    assert(!mount_one());
    assert(state.mounts.size() == 1 && state.mounts.count(survivor) == 1);
    assert(state.journal.size() == 1 && state.journal.front().path == survivor);
    assert(unmount_all(config));
    expect_clean();

    // Replaced mounts belong to somebody else; stale journal entries must
    // unregister only our old policy entry without detaching the new mount.
    reset();
    assert(mount_one());
    const auto replaced = state.targets.front();
    state.mounts.at(replaced) = state.next_mount_id++;
    assert(unmount_all(config));
    assert(state.mounts.size() == 1 && state.mounts.count(replaced) == 1);
    assert(state.registrations.empty() && state.journal.empty());
    assert(std::count(state.detach_attempts.begin(), state.detach_attempts.end(), replaced) == 0);

    std::cout << "Magic Mount lifecycle regressions passed\n";
}
