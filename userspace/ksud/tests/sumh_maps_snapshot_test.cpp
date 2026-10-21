// Production resolver, bounded sampler and comparison with deterministic IO.
// POSIX also crosses a real fork/shared mmap. Windows models the worker; the
// separate namespace-result regression covers pipe/acknowledgement failures.
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <tuple>
#include <type_traits>
#include "core/json.hpp"
#include "mount/maps_snapshot.hpp"
#include "uapi/sumh.h"
#ifndef _WIN32
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_ANONYMOUS 2
#define MAP_FAILED reinterpret_cast<void*>(-1)
#endif

namespace {
struct TestStat {
    unsigned st_mode = 1;
    std::uint64_t st_ino = 0, st_dev = 0;
};
std::map<std::string, TestStat> files;
std::map<std::string, int> reads;
std::vector<sumhp::sumh::InventoryEntry> entries;
bool allocation_fails = false, worker_fails = false, lost_reply = false, real_fork = false;
bool stale_final = false, mask_changes = false, replace_between = false, namespace_changes = false;
int page_calls = 0, fail_page = -1, allocations = 0, worker_errno = EIO;
int fixture_stat(const char* path, TestStat* value) {
    const std::string key(path);
    const int call = ++reads[key];
    auto it = files.find(key);
    if (it == files.end()) {
        errno = ENOENT;
        return -1;
    }
    *value = it->second;
    if ((replace_between && key == "/target" && call == 2) ||
        (namespace_changes && key == "/proc/self/ns/mnt" && call == 2))
        ++value->st_ino;
    return 0;
}
void* fixture_mmap(void* address, std::size_t size, int prot, int flags, int fd,
                   std::ptrdiff_t offset) {
    if (allocation_fails) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    ++allocations;
#ifndef _WIN32
    return ::mmap(address, size, prot, flags, fd, offset);
#else
    (void)address;
    (void)prot;
    (void)flags;
    (void)fd;
    (void)offset;
    return ::operator new(size);
#endif
}
int fixture_munmap(void* address, std::size_t size) {
    --allocations;
#ifndef _WIN32
    return ::munmap(address, size);
#else
    (void)size;
    ::operator delete(address);
    return 0;
#endif
}
sumhp::sumh::InventoryEntry record(std::uint64_t inode = 11) {
    sumhp::sumh::InventoryEntry value;
    value.id = inode;
    value.target_ino = inode;
    value.target_dev = (8u << 20) | 1;
    value.spoofed_ino = 22;
    value.spoofed_dev = (8u << 20) | 1;
    value.path2 = "/replacement";
    return value;
}
void reset() {
    assert(allocations == 0);
    files.clear();
    reads.clear();
    entries = {record()};
    files["/target"] = {1, 11, (8ull << 32) | 1};
    files["/replacement"] = {1, 22, (8ull << 32) | 1};
    files["/proc/self/ns/mnt"] = {2, 33, 4};
    files["/proc/1/ns/mnt"] = {2, 33, 4};
    allocation_fails = worker_fails = lost_reply = real_fork = stale_final = mask_changes = false;
    replace_between = namespace_changes = false;
    page_calls = 0;
    fail_page = -1;
    worker_errno = EIO;
}
}  // namespace
namespace sumhp::mount::fsutil {
bool run_query_in_init_mount_ns(const std::function<bool()>& fn) {
    if (worker_fails) {
        errno = worker_errno;
        return false;
    }
#ifndef _WIN32
    if (real_fork) {
        const auto pid = ::fork();
        assert(pid >= 0);
        if (!pid)
            ::_exit(fn() ? 0 : 1);
        int status = 0;
        assert(::waitpid(pid, &status, 0) == pid);
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
#endif
    const bool success = fn();
    if (lost_reply) {
        errno = EPIPE;
        return false;
    }
    return success;
}
}  // namespace sumhp::mount::fsutil
namespace sumhp::sumh {
// Wire validation is covered by sumh_inventory_test against production client.
bool rule_inventory(const InventoryRequest& request, InventoryPage& page) {
    ++page_calls;
    assert(!request.instance_id || (request.instance_id == 9 && request.generation == 7));
    if (page_calls == fail_page || (!request.category && stale_final)) {
        errno = ESTALE;
        return false;
    }
    page = {};
    page.category = request.category;
    page.instance_id = 9;
    page.generation = 7;
    page.supported_categories = 0x7f;
    page.cursor = request.cursor;
    if (!request.category) {
        if (mask_changes)
            page.supported_categories = 0x1f;
        return true;
    }
    assert(request.category == SUMH_INVENTORY_MAPS_MANAGED);
    if (request.cursor < entries.size())
        page.entries = {entries[request.cursor]};
    page.next_cursor = request.cursor + page.entries.size();
    page.at_end = page.next_cursor >= entries.size();
    return true;
}
}  // namespace sumhp::sumh
namespace sumhp {
#include "sumh_maps_validate_under_test.inc"
}
#define stat TestStat
#define S_ISREG(value) ((value) == 1)
#define major(value) ((value) >> 32)
#define minor(value) ((value) & 0xffffffff)
#define mmap fixture_mmap
#define munmap fixture_munmap
// The production declarations use `struct stat`; alias the injected tag.
int TestStat(const char* path, struct TestStat* value) {
    return fixture_stat(path, value);
}
#include "sumh_maps_snapshot_under_test.inc"
#undef stat
#undef mmap
#undef munmap

int main() {
    using namespace sumhp::mount::sumh;
    sumhp::Config config;
    config.enable_maps_spoof = true;
    config.maps_rules = {{"/target", "/replacement"}};
    reset();
    auto snapshot = sample_maps(config);
    assert(snapshot.definitions_complete && snapshot.stable && snapshot.registered.size() == 1);
    assert(compare_maps(config, snapshot).status == "matches" && allocations == 0);
    // Unchanged numeric identities match; contents are not sampled. Replacing
    // an inode changes the comparison independently of path spelling.
    assert(compare_maps(config, sample_maps(config)).status == "matches");
    files["/target"].st_ino = 12;
    assert(compare_maps(config, sample_maps(config)).status == "drift");
    reset();
    replace_between = true;
    snapshot = sample_maps(config);
    assert(!snapshot.stable && snapshot.registered.empty());
    assert(compare_maps(config, snapshot).status == "unknown");
    reset();
    files.erase("/target");
    snapshot = sample_maps(config);
    assert(snapshot.paths[0].target.kind == MapsIdentityKind::Missing);
    assert(compare_maps(config, snapshot).status == "unknown");
    reset();
    files["/target"].st_mode = 2;
    assert(read_maps_identity("/target").kind == MapsIdentityKind::NotRegular);
    files["/target"] = {1, 11, 0x1000ull << 32};
    assert(read_maps_identity("/target").kind == MapsIdentityKind::Unrepresentable);
    files["/target"] = {1, 0, 8ull << 32};
    assert(read_maps_identity("/target").kind == MapsIdentityKind::Unrepresentable);
    reset();
    config.maps_rules.push_back({"/alias", "/replacement"});
    files["/alias"] = files["/target"];
    snapshot = sample_maps(config);
    assert(snapshot.paths[0].collision && snapshot.paths[1].collision);
    assert(compare_maps(config, snapshot).reason == "maps_target_identity_collision");
    config.maps_rules.pop_back();
    reset();
    config.enable_maps_spoof = false;
    snapshot = sample_maps(config);
    assert(!snapshot.identities_sampled && !reads.count("/target"));
    assert(compare_maps(config, snapshot).status == "drift");
    entries.clear();
    assert(compare_maps(config, sample_maps(config)).status == "matches");
    assert(sample_maps(config, true).identities_sampled);
    config.enable_maps_spoof = true;
    reset();
    entries.clear();
    for (int i = 0; i < 256; ++i)
        entries.push_back(record(i + 1));
    snapshot = sample_maps(config);
    assert(snapshot.definitions_complete && snapshot.registered.size() == 256 && page_calls == 257);
    entries.push_back(record(257));
    snapshot = sample_maps(config);
    assert(!snapshot.definitions_complete && snapshot.registered.empty() &&
           snapshot.error == EOVERFLOW);
    reset();
    entries.push_back(record());
    snapshot = sample_maps(config);
    assert(!snapshot.definitions_complete && snapshot.error == EPROTO &&
           snapshot.registered.empty());
    reset();
    entries.push_back(record(12));
    fail_page = 2;
    snapshot = sample_maps(config);
    assert(!snapshot.definitions_complete && snapshot.registered.empty());
    for (bool* fault : {&stale_final, &mask_changes, &namespace_changes, &worker_fails, &lost_reply,
                        &allocation_fails}) {
        reset();
        *fault = true;
        snapshot = sample_maps(config);
        assert(snapshot.registered.empty() && compare_maps(config, snapshot).status == "unknown" &&
               allocations == 0);
    }
    for (const int failure : {ETIMEDOUT, EBUSY, ENOSYS}) {
        reset();
        worker_fails = true;
        worker_errno = failure;
        snapshot = sample_maps(config);
        assert(snapshot.error == failure && snapshot.registered.empty() &&
               !snapshot.definitions_complete);
        assert(compare_maps(config, snapshot).status == "unknown" && page_calls == 0 &&
               allocations == 0);
    }
    reset();
    sumhp::sumh::RuntimeState expected;
    expected.instance_id = 9;
    expected.generation = 7;
    assert(compare_maps(config, sample_maps(config, false, &expected)).status == "matches");
    expected.generation = 0;
    assert(sample_maps(config, false, &expected).reason == "runtime_token_invalid");
#ifndef _WIN32
    reset();
    real_fork = true;
    snapshot = sample_maps(config);
    assert(snapshot.definitions_complete && snapshot.registered.size() == 1 &&
           compare_maps(config, snapshot).status == "matches");
#else
    std::cout << "Windows: real fork/shared mmap smoke not available; worker transport modeled\n";
#endif
    assert(allocations == 0);
    std::cout << "SUMH Maps snapshot: production resolver, bounded inventory, races and comparison "
                 "passed\n";
}
