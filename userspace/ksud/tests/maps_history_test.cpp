#include "mount/maps_history.hpp"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include "uapi/sumh.h"

namespace {
namespace fs = std::filesystem;
using namespace sumhp;
using namespace sumhp::mount::sumh;
struct File {
    std::string bytes;
    int mode = 1, links = 1;
};
struct Handle {
    std::string path;
    std::size_t at = 0;
};
struct FakeStat {
    int st_mode = 1, st_nlink = 1;
    std::int64_t st_size = 0;
    std::uint64_t st_ino = 7, st_dev = 1;
};
std::map<std::string, File> files;
std::map<int, Handle> handles;
int next_fd = 3, locked = -1, writes = 0, failed_write = 0, calls = 0, reads = 0, failed_read = 0,
    random_seed = 1;
bool replaced_on_failure = false, fail_prepare = false, fail_random = false, fail_map = false;
bool ioctl_ok = true, compatibility_noop = false, runtime_ok = true, namespace_ok = true;
std::string boot = "test-boot";
std::vector<sumh::MapsRule> installed;
const std::string history_name =
    (fs::path("/fixture") / "maps_submission_history.json").generic_string();
template <typename C>
int fake_open(const C* path, int flags, int = 0) {
    assert((flags & 8) && (flags & 16) && (flags & 32));
    std::string key = fs::path(path).generic_string();
    if (!files.count(key)) {
        if (flags & 4)
            files[key] = {};
        else {
            errno = ENOENT;
            return -1;
        }
    }
    handles[next_fd] = {key, 0};
    return next_fd++;
}
int fake_close(int fd) {
    if (locked == fd)
        locked = -1;
    handles.erase(fd);
    return 0;
}
int fake_fstat(int fd, FakeStat* out) {
    const auto& file = files.at(handles.at(fd).path);
    out->st_mode = file.mode;
    out->st_nlink = file.links;
    out->st_size = file.bytes.size();
    return 0;
}
int fake_stat(const char*, FakeStat* out) {
    out->st_ino = namespace_ok ? 7 : 0;
    out->st_dev = 1;
    return 0;
}
std::ptrdiff_t fake_read(int fd, void* dest, std::size_t limit) {
    if (++reads == failed_read) {
        errno = EIO;
        return -1;
    }
    auto& handle = handles.at(fd);
    const auto& bytes = files.at(handle.path).bytes;
    auto size = std::min(limit, bytes.size() - handle.at);
    std::memcpy(dest, bytes.data() + handle.at, size);
    handle.at += size;
    return size;
}
int fake_flock(int fd, int flags) {
    assert(flags == 3);
    if (locked >= 0) {
        errno = EWOULDBLOCK;
        return -1;
    }
    locked = fd;
    return 0;
}
std::ptrdiff_t fake_random(void* target, std::size_t count, int) {
    if (fail_random) {
        errno = EIO;
        return -1;
    }
    std::memset(target, random_seed++, count);
    return count;
}
void* fake_mmap(void*, std::size_t size, int, int, int, std::int64_t) {
    if (fail_map) {
        errno = ENOMEM;
        return reinterpret_cast<void*>(-1);
    }
    assert(size == sizeof(unsigned));
    return new unsigned(0);
}
int fake_munmap(void* memory, std::size_t) {
    delete static_cast<unsigned*>(memory);
    return 0;
}
void reset() {
    files.clear();
    handles.clear();
    next_fd = 3;
    locked = -1;
    writes = failed_write = calls = reads = failed_read = 0;
    replaced_on_failure = fail_prepare = fail_random = fail_map = compatibility_noop = false;
    ioctl_ok = runtime_ok = namespace_ok = true;
    boot = "test-boot";
    installed.clear();
}
}  // namespace
namespace sumhp {
fs::path runtime_data_dir() {
    return "/fixture";
}
std::string runtime_boot_id() {
    return boot;
}
bool prepare_private_directory(const fs::path&, std::string& error) {
    if (fail_prepare) {
        error = "prepare failed";
        errno = EIO;
        return false;
    }
    return true;
}
#include "maps_validation_under_test.inc"
namespace state {
std::string revision_for_bytes(const std::string&) {
    return "sha256-v1:" + std::string(64, 'a');
}
}  // namespace state
namespace sumh {
bool runtime_state(RuntimeState& out) {
    if (!runtime_ok) {
        errno = EOPNOTSUPP;
        return false;
    }
    out.instance_id = 9;
    out.generation = 7 + calls;
    return true;
}
bool replace_managed_maps_rules_checked(const std::vector<MapsRule>& values, bool& submitted) {
    ++calls;
    // Real kernel mutation cannot precede the durable, readable pending record.
    MapsHistory history;
    std::string error;
    assert(read_maps_history(history, error) == "valid" && history.latest->phase == "pending");
    submitted = ioctl_ok && !compatibility_noop;
    if (submitted)
        installed = values;
    if (!ioctl_ok)
        errno = EIO;
    return ioctl_ok;
}
}  // namespace sumh
}  // namespace sumhp
namespace ksud {
bool write_file_durable(const fs::path& path, const std::string& bytes, std::string& error) {
    const bool fail = ++writes == failed_write;
    if (!fail || replaced_on_failure)
        files[path.generic_string()].bytes = bytes;
    if (fail) {
        error = "injected durable-write failure";
        errno = EIO;
    }
    return !fail;
}
}  // namespace ksud
#define open fake_open
#define close fake_close
#define fstat fake_fstat
#define stat FakeStat
#define read fake_read
#define flock fake_flock
#define getrandom fake_random
#define mmap fake_mmap
#define munmap fake_munmap
#define S_ISREG(mode) ((mode) == 1)
#define O_RDONLY 1
#define O_RDWR 2
#define O_CREAT 4
#define O_CLOEXEC 8
#define O_NOFOLLOW 16
#define O_NONBLOCK 32
#define LOCK_EX 1
#define LOCK_NB 2
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_ANONYMOUS 2
#define MAP_FAILED reinterpret_cast<void*>(-1)
// Function and type named stat coexist, like the POSIX interface.
int FakeStat(const char* path, struct FakeStat* out) {
    return fake_stat(path, out);
}
#include "maps_history_under_test.inc"

int main() {
    std::string error;
    const std::vector<MapsPathRule> definitions = {{"/target ", "/replacement"}};
    const std::vector<sumhp::sumh::MapsRule> identities = {{123, 17, 456, 33, "/replacement"}};
    reset();
    MapsHistory history;
    assert(read_maps_history(history, error) == "missing" && files.empty());
    {
        MapsSubmissionTracker tracker;
        assert(tracker.valid());
        assert(replace_managed_maps_recorded(definitions, identities, tracker, error));
        assert(tracker.state() == ManagedMapsRestoreState::Replaced && calls == 1 && writes == 2);
    }
    assert(read_maps_history(history, error) == "valid" && history.confirmed);
    const auto first = history.confirmed->id;
    assert(history.confirmed->definitions[0].target_path == "/target ");
    assert(history.confirmed->identities[0].target_ino == 123 &&
           history.confirmed->generation_after == "8");
    // A genuine full-u64 diagnostic value survives a 32-bit-long host too.
    auto large = history;
    large.latest->identities[0].target_ino = UINT64_MAX;
    large.confirmed = large.latest;
    MapsHistory decoded;
    assert(parse_maps_history(encode_maps_history(large), decoded, error));
    assert(decoded.confirmed->identities[0].target_ino == UINT64_MAX);
    for (const auto& future : {std::string("{}"), std::string("{\"schema_version\":2}"),
                               std::string(MAPS_HISTORY_MAX_BYTES + 1, 'x')}) {
        assert(!parse_maps_history(future, decoded, error) &&
               decoded.confirmed->identities[0].target_ino == UINT64_MAX);
    }
    // Failed mutation retains the prior acknowledged submission.
    ioctl_ok = false;
    {
        MapsSubmissionTracker t;
        assert(!replace_managed_maps_recorded(definitions, identities, t, error));
        assert(t.state() == ManagedMapsRestoreState::Unchanged);
    }
    assert(read_maps_history(history, error) == "valid" && history.latest->phase == "rejected" &&
           history.confirmed->id == first);
    ioctl_ok = true;
    // Each durable step, both before rename and rename-with-unconfirmed-durability.
    for (int step : {1, 2})
        for (bool replaced : {false, true}) {
            reset();
            failed_write = step;
            replaced_on_failure = replaced;
            MapsSubmissionTracker t;
            assert(!replace_managed_maps_recorded(definitions, identities, t, error));
            assert(calls == (step == 2 ? 1 : 0));
            assert(t.state() == (step == 2 ? ManagedMapsRestoreState::Replaced
                                           : ManagedMapsRestoreState::Unchanged));
            if (step == 2)
                assert(error.find("partial:") == 0 && installed[0].target_ino == 123);
        }
    // Successful replacement but failed final write preserves prior confirmed.
    reset();
    {
        MapsSubmissionTracker t;
        assert(replace_managed_maps_recorded(definitions, identities, t, error));
    }
    read_maps_history(history, error);
    const auto previous = history.confirmed->id;
    failed_write = writes + 2;
    {
        MapsSubmissionTracker t;
        assert(!replace_managed_maps_recorded(definitions, identities, t, error));
    }
    assert(read_maps_history(history, error) == "valid" && history.latest->phase == "pending" &&
           history.confirmed->id == previous);
    const auto pending_id = history.latest->id;
    const auto pending_bytes = files[history_name].bytes;
    const int pending_calls = calls, pending_writes = writes;
    assert(!maps_automatic_restore_allowed(error) && errno == EBUSY);
    {
        MapsSubmissionTracker t;
        assert(!replace_managed_maps_recorded(definitions, identities, t, error));
        assert(errno == EBUSY && t.state() == ManagedMapsRestoreState::Unchanged);
    }
    assert(calls == pending_calls && writes == pending_writes &&
           files[history_name].bytes == pending_bytes);
    // Explicit application is a new attempt, with a link to the unknown one.
    {
        MapsSubmissionTracker t;
        assert(replace_managed_maps_recorded(definitions, identities, t, error,
                                             RestoreTrigger::UserRequested));
    }
    assert(read_maps_history(history, error) == "valid" && history.latest->phase == "acknowledged");
    assert(history.latest->supersedes_pending_id == pending_id &&
           history.confirmed->supersedes_pending_id == pending_id);
    assert(maps_automatic_restore_allowed(error));
    auto invalid = history;
    invalid.latest->supersedes_pending_id = invalid.latest->id;
    invalid.confirmed = invalid.latest;
    assert(!parse_maps_history(encode_maps_history(invalid), decoded, error));
    invalid = history;
    invalid.latest->supersedes_pending_id = "bad";
    invalid.confirmed = invalid.latest;
    assert(!parse_maps_history(encode_maps_history(invalid), decoded, error));
    // A new boot can restore definitions; old pending remains historical only.
    files[history_name].bytes = pending_bytes;
    boot = "next-boot";
    assert(maps_automatic_restore_allowed(error));
    {
        MapsSubmissionTracker t;
        assert(replace_managed_maps_recorded(definitions, identities, t, error));
    }
    assert(read_maps_history(history, error) == "valid" && history.latest->boot_id == "next-boot" &&
           history.latest->supersedes_pending_id.empty());
    reset();
    assert(maps_automatic_restore_allowed(error) && files.empty());
    boot.clear();
    assert(!maps_automatic_restore_allowed(error) && files.empty() && calls == 0);
    for (int fault = 0; fault < 3; ++fault) {
        reset();
        files[history_name] = {fault == 0 ? "{\"schema_version\":2}" : "invalid",
                               fault == 1 ? 2 : 1, 1};
        if (fault == 2)
            failed_read = 1;
        const auto before = files[history_name].bytes;
        assert(!maps_automatic_restore_allowed(error) && calls == 0 && writes == 0 &&
               files[history_name].bytes == before);
    }
    // No-op compatibility is never persisted as an acknowledged submission.
    reset();
    compatibility_noop = true;
    {
        MapsSubmissionTracker t;
        assert(!replace_managed_maps_recorded({}, {}, t, error));
        assert(t.state() == ManagedMapsRestoreState::Unchanged);
    }
    assert(read_maps_history(history, error) == "valid" && !history.confirmed &&
           history.latest->phase == "rejected");
    // Genuine empty replacement records the actual cleared set.
    reset();
    {
        MapsSubmissionTracker t;
        assert(replace_managed_maps_recorded({}, {}, t, error));
    }
    assert(read_maps_history(history, error) == "valid" && history.confirmed->definitions.empty());
    // Unavailable optional kernel tokens do not fabricate values or lose receipt.
    reset();
    runtime_ok = false;
    {
        MapsSubmissionTracker t;
        assert(replace_managed_maps_recorded(definitions, identities, t, error));
    }
    assert(read_maps_history(history, error) == "valid" && history.confirmed->instance_id.empty());
    for (int fault = 0; fault < 9; ++fault) {
        reset();
        if (fault == 0)
            boot.clear();
        if (fault == 1)
            fail_prepare = true;
        if (fault == 2)
            fail_random = true;
        if (fault == 3)
            namespace_ok = false;
        if (fault == 4)
            locked = 999;
        if (fault == 5)
            files[history_name] = {"{\"schema_version\":2}", 1, 1};
        if (fault == 6)
            files[history_name] = {"ignored", 2, 1};
        if (fault == 7)
            files[history_name] = {"ignored", 1, 2};
        if (fault == 8) {
            files[history_name] = {"ignored", 1, 1};
            failed_read = 1;
        }
        MapsSubmissionTracker t;
        assert(!replace_managed_maps_recorded(definitions, identities, t, error));
        assert(calls == 0);
    }
    reset();
    fail_map = true;
    {
        MapsSubmissionTracker t;
        assert(!t.valid());
        assert(!replace_managed_maps_recorded(definitions, identities, t, error));
        assert(calls == 0);
    }
    reset();
    {
        MapsSubmissionTracker t;
        assert(!replace_managed_maps_recorded(definitions, {}, t, error));
        assert(calls == 0);
    }
    // Pending readback failure blocks ioctl even if replacement reached disk.
    reset();
    failed_read = 1;
    {
        MapsSubmissionTracker t;
        assert(!replace_managed_maps_recorded(definitions, identities, t, error));
        assert(calls == 0);
    }
    // Final receipt bytes may commit but readback can still fail: report partial.
    reset();
    failed_read = 5;
    {
        MapsSubmissionTracker t;
        assert(!replace_managed_maps_recorded(definitions, identities, t, error));
        assert(calls == 1 && t.state() == ManagedMapsRestoreState::Replaced &&
               error.find("partial:") == 0);
    }
    assert(read_maps_history(history, error) == "valid" && history.latest->phase == "acknowledged");
    assert(handles.empty());
    std::cout << "Maps history production model/storage fault cases passed\n";
}
