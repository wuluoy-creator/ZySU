// Run production registration and orchestration; simulate mount syscalls and
// module planning so failures can be tested without root or an Android device.
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

constexpr int MNT_DETACH = 2;

struct Config {
    bool enable_overlay_xattr_hide = true;
    std::string mount_source = "KSU";
    std::vector<std::string> partitions;
};
struct ModuleEntry {
    std::string id;
};
namespace logging {
enum class Level { Info, Error };
}
namespace fsutil {
struct MountRecord {
    std::string path;
    std::uint64_t mount_id = 0;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
};
}  // namespace fsutil

namespace fixture {
const std::vector<std::string> overlays = {"/system/etc", "/system/etc/child", "/vendor/lib"};
const std::vector<std::string> mounted = {"/system/etc", "/system/etc/child", "/system/etc/stock",
                                          "/vendor/lib"};
struct State {
    bool available = true;
    bool fail_journal = false;
    bool fail_snapshot = false;
    bool fail_first_journal = false;
    std::string fail_registration;
    std::string fail_xattrs;
    std::string fail_capture;
    std::string replace_on_failure;
    std::map<std::string, int> fail_detaches;
    std::map<std::string, std::uint64_t> identities;
    std::map<std::string, int> identity_reads;
    std::map<std::string, int> identity_matches;
    std::string replace_after_match;
    int replace_after_match_count = 1;
    std::uint64_t next_identity = 1;
    std::set<std::string> live;
    std::set<std::string> registered;
    std::vector<std::string> xattr_attempts;
    std::vector<std::string> journal;
    std::vector<fsutil::MountRecord> journal_records;
    std::vector<std::string> logs;
    std::vector<std::string> detached;
    int journal_writes = 0;
} state;
}  // namespace fixture

std::string runtime_boot_id() {
    return "fixture-boot";
}
namespace ksud {
bool write_file_durable(const std::string&, const std::string& data, std::string&) {
    auto& state = fixture::state;
    ++state.journal_writes;
    if (state.fail_journal || (state.fail_first_journal && state.journal_writes == 1))
        return false;
    state.journal.clear();
    state.journal_records.clear();
    std::istringstream input(data);
    std::string header;
    std::getline(input, header);
    assert(header == "SUMHP_MOUNTS_V2 fixture-boot");
    fsutil::MountRecord record;
    while (input >> record.mount_id >> record.device >> record.inode >> std::quoted(record.path)) {
        state.journal.push_back(record.path);
        state.journal_records.push_back(record);
    }
    return true;
}
}  // namespace ksud

namespace sumhp::sumh {
bool is_available() {
    return fixture::state.available;
}
bool hide_overlay_xattrs(const std::string& path) {
    auto& state = fixture::state;
    assert(state.live.count(path));
    assert(state.registered.count(path));
    state.xattr_attempts.push_back(path);
    if (path == state.fail_xattrs && !state.replace_on_failure.empty())
        state.identities[state.replace_on_failure] = state.next_identity++;
    return path != state.fail_xattrs;
}
}  // namespace sumhp::sumh

namespace fsutil {
#include "mount_decode_under_test.inc"

void mlog(const std::string& message, logging::Level = logging::Level::Info) {
    fixture::state.logs.push_back(message);
}
const std::vector<std::string>& managed_partitions(const std::vector<std::string>&) {
    static const std::vector<std::string> partitions = {"system", "vendor"};
    return partitions;
}
bool register_umount(const std::string& path) {
    auto& state = fixture::state;
    assert(state.live.count(path));
    if (path == state.fail_registration)
        return false;
    state.registered.insert(path);
    return true;
}
bool unregister_umount(const std::string& path) {
    fixture::state.registered.erase(path);
    return true;
}
bool capture_mount_identity(const std::string& path, MountRecord& record) {
    auto& state = fixture::state;
    ++state.identity_reads[path];
    if (path == state.fail_capture || !state.live.count(path))
        return false;
    auto& identity = state.identities[path];
    if (!identity)
        identity = state.next_identity++;
    record = {path, identity, 1, 1};
    return true;
}
bool mount_matches(const MountRecord& record) {
    auto& state = fixture::state;
    const bool matches =
        state.live.count(record.path) && state.identities[record.path] == record.mount_id;
    if (++state.identity_matches[record.path] == state.replace_after_match_count &&
        record.path == state.replace_after_match)
        state.identities[record.path] = state.next_identity++;
    return matches;
}
bool read_mount_journal(const std::string&, std::vector<MountRecord>& records, bool& legacy) {
    records = fixture::state.journal_records;
    legacy = false;
    return true;
}
#include "mount_journal_under_test.inc"
}  // namespace fsutil
using fsutil::mlog;

namespace storage {
enum class Mode { Tmpfs };
struct Handle {
    bool ok = true;
    Mode mode = Mode::Tmpfs;
    std::string content_dir = "/mnt/module-mirror";
};
const char* mode_name(Mode) {
    return "tmpfs";
}
Handle setup(const Config&) {
    return {};
}
}  // namespace storage

#include "overlay_index_under_test.inc"
#include "overlay_record_under_test.inc"

std::optional<MountPointIndex> read_mount_points() {
    if (fixture::state.fail_snapshot)
        return std::nullopt;
    std::istringstream input;
    return MountPointIndex(input);
}
namespace fs {
bool remove(const std::string&, std::error_code& error) {
    error.clear();
    fixture::state.journal.clear();
    fixture::state.journal_records.clear();
    return true;
}
}  // namespace fs

bool sync_content(const std::vector<ModuleEntry>&, const storage::Handle&,
                  const std::vector<std::string>&) {
    return true;
}
std::map<std::string, std::vector<std::string>> plan_overlays(const std::string&,
                                                              const std::vector<std::string>&,
                                                              const std::vector<std::string>&) {
    return {{"/system/etc", {"system-layer"}}, {"/vendor/lib", {"vendor-layer"}}};
}
bool mount_overlay(const std::string& root, const std::vector<std::string>&, const std::string&,
                   const std::string&, const std::string&, std::vector<AttachedMount>& attached,
                   const MountPointIndex&) {
    auto& state = fixture::state;
    state.live.insert(root);
    if (!record_mount(root, attached))
        return false;
    if (root == "/system/etc") {
        state.live.insert(root + "/child");
        if (!record_mount(root + "/child", attached))
            return false;
        state.live.insert(root + "/stock");
        if (!record_mount(root + "/stock", attached, false))
            return false;
    }
    return true;
}
std::string overlay_journal() {
    return "/data/adb/sumhp/run/overlay_mounts.list";
}
int umount2(const char* path, int flags) {
    assert(flags == MNT_DETACH);
    fixture::state.detached.emplace_back(path);
    if (fixture::state.fail_detaches[path] > 0) {
        --fixture::state.fail_detaches[path];
        errno = EBUSY;
        return -1;
    }
    if (!fixture::state.live.erase(path))
        return -1;
    const std::string prefix = std::string(path) + "/";
    for (auto it = fixture::state.live.begin(); it != fixture::state.live.end();) {
        if (it->rfind(prefix, 0) == 0)
            it = fixture::state.live.erase(it);
        else
            ++it;
    }
    return 0;
}

#include "overlay_mount_under_test.inc"

int main() {
    // Existing path callers still capture current identities, while invalid
    // captured records cannot replace the last valid journal.
    fixture::state = {};
    fixture::state.live.insert("/fixture");
    assert(fsutil::write_mount_journal("journal", std::vector<std::string>{"/fixture"}));
    const auto saved_identity = fixture::state.journal_records.front().mount_id;
    for (const auto& invalid :
         std::vector<fsutil::MountRecord>{{"relative", 1, 1, 1},
                                          {"/bad\npath", 1, 1, 1},
                                          {std::string("/bad\0path", 9), 1, 1, 1},
                                          {"/missing-identity", 0, 1, 1}}) {
        assert(!fsutil::write_mount_journal("journal", std::vector<fsutil::MountRecord>{invalid}));
        assert(fixture::state.journal_records.front().mount_id == saved_identity);
    }
    assert(fixture::state.journal_writes == 1);

    // A single snapshot preserves escaped paths and nested stock submounts.
    // Duplicates and neighboring path prefixes must not become child mounts.
    std::istringstream mountinfo(
        "12 1 0:1 / /system/etc/deep/child rw - tmpfs tmpfs rw\n"
        "13 1 0:1 / /system/etc rw - tmpfs tmpfs rw\n"
        "14 1 0:1 / /system/etc/space\\040name rw - tmpfs tmpfs rw\n"
        "15 1 0:1 / /system/etc/deep rw - tmpfs tmpfs rw\n"
        "16 1 0:1 / /system/etc/deep rw - tmpfs tmpfs rw\n"
        "17 1 0:1 / /system/etc-neighbor/child rw - tmpfs tmpfs rw\n"
        "18 1 0:1 / /vendor/lib/child rw - tmpfs tmpfs rw\n"
        "malformed\n"
        "19 1 0:1 /\n"
        "\t20\t1\t0:1\t/\t/system/etc/tab\\011name\trw - tmpfs tmpfs rw\n");
    const MountPointIndex index(mountinfo);
    assert(index.valid());
    assert((index.children("/system/etc") ==
            std::vector<std::string>{"/system/etc/deep", "/system/etc/deep/child",
                                     "/system/etc/space name", "/system/etc/tab\tname"}));
    assert((index.children("/vendor/lib") == std::vector<std::string>{"/vendor/lib/child"}));
    assert(index.children("/system/etc/deep/child").empty());
    assert(index.children("/system/et").empty());
    assert(index.children("/missing").empty());
    // Queries continue to use the captured stock view after the input changes.
    mountinfo.clear();
    mountinfo.str("21 1 0:1 / /system/etc/new-overlay rw - overlay KSU rw\n");
    assert(index.children("/system/etc").size() == 4);
    assert(index.children("/system/etc/deep").size() == 1);
    for (const auto failure : {std::ios::badbit, std::ios::failbit}) {
        std::istringstream unreadable;
        unreadable.setstate(failure);
        assert(!MountPointIndex(unreadable).valid());
    }
    std::istringstream interrupted;
    interrupted.setstate(std::ios::badbit | std::ios::eofbit);
    assert(!MountPointIndex(interrupted).valid());

    const std::vector<ModuleEntry> modules = {{"module"}};
    Config config;

    // Every independent overlay superblock is registered on initial mount,
    // including a child reconstructed under a planned leaf. Stock binds are not.
    fixture::state = {};
    assert(mount_modules(modules, config));
    assert(fixture::state.xattr_attempts == fixture::overlays);
    assert(fixture::state.journal == fixture::overlays);
    assert(fixture::state.live.size() == fixture::mounted.size());
    assert(fixture::state.detached.empty());
    for (const auto& [path, count] : fixture::state.identity_reads) {
        (void)path;
        assert(count == 1);
    }

    // An operator-disabled feature and an unavailable SUMH kernel preserve
    // ordinary module mounting, without trying to register attributes.
    for (int disabled = 0; disabled < 2; ++disabled) {
        fixture::state = {};
        config.enable_overlay_xattr_hide = disabled != 0;
        fixture::state.available = disabled == 0;
        assert(mount_modules(modules, config));
        assert(fixture::state.xattr_attempts.empty());
        assert(fixture::state.journal == fixture::overlays);
    }
    config.enable_overlay_xattr_hide = true;

    // A child registration error must fail the plan and roll back all mounts,
    // including the stock bind, without committing a success journal.
    fixture::state = {};
    fixture::state.fail_xattrs = "/system/etc/child";
    assert(!mount_modules(modules, config));
    assert((fixture::state.xattr_attempts ==
            std::vector<std::string>{"/system/etc", "/system/etc/child"}));
    assert(fixture::state.journal_writes == 0);
    assert(fixture::state.live.empty() && fixture::state.registered.empty());
    assert(fixture::state.detached ==
           std::vector<std::string>(fixture::mounted.rbegin(), fixture::mounted.rend()));

    fixture::state = {};
    fixture::state.fail_registration = "/system/etc/child";
    assert(!mount_modules(modules, config));
    assert(fixture::state.xattr_attempts.empty());
    assert(fixture::state.journal_writes == 0);
    assert(fixture::state.live.empty() && fixture::state.registered.empty());

    fixture::state = {};
    fixture::state.fail_journal = true;
    assert(!mount_modules(modules, config));
    assert(fixture::state.xattr_attempts == fixture::overlays);
    assert(fixture::state.live.empty() && fixture::state.registered.empty());
    // A failed stock snapshot must stop before publishing even one overlay.
    fixture::state = {};
    fixture::state.fail_snapshot = true;
    assert(!mount_modules(modules, config));
    assert(fixture::state.live.empty() && fixture::state.detached.empty());

    // A failed final journal followed by a failed detach retains the original
    // survivor for a subsequent cleanup, including the registration.
    fixture::state = {};
    fixture::state.fail_first_journal = true;
    fixture::state.fail_detaches["/vendor/lib"] = 1;
    assert(!mount_modules(modules, config));
    assert((fixture::state.journal == std::vector<std::string>{"/vendor/lib"}));
    assert(fixture::state.live.size() == 1 && fixture::state.registered.size() == 1);
    assert(unmount_all(config));
    assert(fixture::state.live.empty() && fixture::state.registered.empty());
    assert(fixture::state.journal.empty());

    // A stock child and its parent may both resist rollback. Only the parent
    // overlay is journaled, and the next parent detach also removes the stock.
    fixture::state = {};
    fixture::state.fail_xattrs = "/vendor/lib";
    fixture::state.fail_detaches["/system/etc/stock"] = 1;
    fixture::state.fail_detaches["/system/etc"] = 1;
    assert(!mount_modules(modules, config));
    assert((fixture::state.journal == std::vector<std::string>{"/system/etc"}));
    assert(fixture::state.live.size() == 2);
    assert(unmount_all(config));
    assert(fixture::state.live.empty() && fixture::state.registered.empty());

    // Never detach somebody else's replacement, including during rollback.
    fixture::state = {};
    fixture::state.fail_xattrs = "/vendor/lib";
    fixture::state.replace_on_failure = "/vendor/lib";
    assert(!mount_modules(modules, config));
    assert(fixture::state.live == std::set<std::string>{"/vendor/lib"});
    assert(fixture::state.registered.empty());
    assert(std::count(fixture::state.detached.begin(), fixture::state.detached.end(),
                      "/vendor/lib") == 0);
    fixture::state = {};
    assert(mount_modules(modules, config));
    fixture::state.identities["/vendor/lib"] = fixture::state.next_identity++;
    assert(unmount_all(config));
    assert(fixture::state.live == std::set<std::string>{"/vendor/lib"});
    assert(fixture::state.registered.empty() && fixture::state.journal.empty());

    // A replacement between the final check and journal serialization must
    // never be adopted as ours, for both commit and a rollback survivor.
    for (const bool rollback_survivor : {false, true}) {
        fixture::state = {};
        auto& state = fixture::state;
        state.replace_after_match = "/vendor/lib";
        if (rollback_survivor) {
            state.fail_xattrs = "/system/etc";
            state.fail_detaches["/vendor/lib"] = 1;
            state.replace_after_match_count = 2;
        }
        assert(mount_modules(modules, config) == !rollback_survivor);
        const auto saved =
            std::find_if(state.journal_records.begin(), state.journal_records.end(),
                         [](const auto& record) { return record.path == "/vendor/lib"; });
        assert(saved != state.journal_records.end());
        assert(saved->mount_id != state.identities.at("/vendor/lib"));
        assert(state.identity_reads.at("/vendor/lib") == 1);
        state.detached.clear();
        assert(unmount_all(config));
        assert(state.live == std::set<std::string>{"/vendor/lib"});
        assert(state.registered.empty() && state.journal.empty());
        assert(std::count(state.detached.begin(), state.detached.end(), "/vendor/lib") == 0);
    }

    // Identity failure is explicit: an unverifiable path is not detached or
    // registered and cleanup is reported as incomplete.
    fixture::state = {};
    fixture::state.fail_capture = "/vendor/lib";
    assert(!mount_modules(modules, config));
    assert(fixture::state.live == std::set<std::string>{"/vendor/lib"});
    assert(fixture::state.registered.empty());
    assert(std::any_of(
        fixture::state.logs.begin(), fixture::state.logs.end(),
        [](const auto& line) { return line.find("rollback incomplete") != std::string::npos; }));

    // Persistent journal failure must be reported if a mount also survives.
    fixture::state = {};
    fixture::state.fail_journal = true;
    fixture::state.fail_detaches["/vendor/lib"] = 1;
    assert(!mount_modules(modules, config));
    assert(fixture::state.live == std::set<std::string>{"/vendor/lib"});
    assert(fixture::state.journal.empty());
    assert(std::any_of(
        fixture::state.logs.begin(), fixture::state.logs.end(),
        [](const auto& line) { return line.find("cannot journal") != std::string::npos; }));
    std::cout << "PASS OverlayFS stock mount index, parent/child registration and rollback\n";
}
