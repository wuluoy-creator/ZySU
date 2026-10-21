#include "mount/module_rebuild.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#ifndef ESTALE
#define ESTALE 116
#endif

namespace {
namespace fs = std::filesystem;
using namespace sumhp;
using namespace sumhp::mount::sumh;
int checks = 0, groups = 0;
#define CHECK(condition)                                                           \
    do {                                                                           \
        ++checks;                                                                  \
        if (!(condition)) {                                                        \
            std::cerr << "check failed line " << __LINE__ << ": " #condition "\n"; \
            std::abort();                                                          \
        }                                                                          \
    } while (false)
struct File {
    std::string bytes;
    int mode = 1, links = 1;
};
struct Handle {
    std::string path;
    std::size_t offset = 0;
};
struct FakeStat {
    int st_mode = 1, st_nlink = 1;
    std::int64_t st_size = 0;
};
std::map<std::string, File> files;
std::map<int, Handle> handles;
int next_fd = 3, locked = -1, writes = 0, failed_write = 0, preparations = 0;
int reads = 0, failed_read = 0, random_calls = 0, random_seed = 1, mismatch_write = 0;
bool expose_failed_write = false, fail_prepare = false, fail_random = false;
bool fail_open = false, fail_fstat = false, fail_lock = false, fail_unlink = false,
     fail_sync = false;
bool mismatch_readback = false, boot_changes = false;
std::string boot = "test-boot";
std::vector<std::string> actions;
int fail_action = -1, throw_action = -1;
const std::string history_name =
    (fs::path("/fixture") / "module_rebuild_history.json").generic_string();
const std::string intent_name =
    (fs::path("/fixture") / "module_rebuild_intent.json").generic_string();
const std::string lock_name =
    (fs::path("/fixture") / "module_rebuild_history.lock").generic_string();
template <typename C>
int fake_open(const C* path, int flags, int = 0) {
    CHECK((flags & 8) && (flags & 16) && (flags & 32));
    if (fail_open) {
        errno = EACCES;
        return -1;
    }
    const std::string key = fs::path(path).generic_string();
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
    if (fail_fstat) {
        errno = EIO;
        return -1;
    }
    const auto& file = files.at(handles.at(fd).path);
    out->st_mode = file.mode;
    out->st_nlink = file.links;
    out->st_size = file.bytes.size();
    return 0;
}
std::ptrdiff_t fake_read(int fd, void* target, std::size_t size) {
    if (++reads == failed_read) {
        errno = EIO;
        return -1;
    }
    auto& handle = handles.at(fd);
    const auto& bytes = files.at(handle.path).bytes;
    const auto count = std::min(size, bytes.size() - handle.offset);
    std::memcpy(target, bytes.data() + handle.offset, count);
    handle.offset += count;
    return count;
}
int fake_flock(int fd, int flags) {
    CHECK(flags == 3);
    if (locked >= 0 || fail_lock) {
        errno = EWOULDBLOCK;
        return -1;
    }
    locked = fd;
    return 0;
}
std::ptrdiff_t fake_random(void* target, std::size_t count, int) {
    ++random_calls;
    if (fail_random) {
        errno = EIO;
        return -1;
    }
    std::memset(target, random_seed++, count);
    return count;
}
template <typename C>
int fake_unlink(const C* path) {
    if (fail_unlink) {
        errno = EACCES;
        return -1;
    }
    if (!files.erase(fs::path(path).generic_string())) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}
void reset_test() {
    files.clear();
    handles.clear();
    next_fd = 3;
    locked = -1;
    writes = failed_write = preparations = reads = failed_read = random_calls = mismatch_write = 0;
    random_seed = 1;
    expose_failed_write = fail_prepare = fail_random = fail_open = fail_fstat = fail_lock = false;
    fail_unlink = fail_sync = mismatch_readback = boot_changes = false;
    boot = "test-boot";
    actions.clear();
    fail_action = throw_action = -1;
}
ModuleMountPlan sample() {
    ModuleMountPlan plan;
    plan.mappings = {{"/vendor/a", "/module/z", false},
                     {"/vendor/a", "/module/a", true},
                     {"/vendor/界\n ", std::string("/module/") + char(0xff), false}};
    plan.hide = {"/vendor/gone", "/vendor/gone2"};
    return plan;
}
bool mutation(const std::string& action) {
    CHECK(locked >= 0);
    ModuleRebuildHistory history;
    std::string error;
    CHECK(read_module_rebuild_history(history, error) == "valid" &&
          history.latest->phase == "pending");
    CHECK(files.count(intent_name));
    const int index = static_cast<int>(actions.size());
    actions.push_back(action);
    if (index == throw_action)
        throw std::runtime_error("injected callback exception");
    if (index == fail_action) {
        errno = EFAULT;
        return false;
    }
    return true;
}
bool run(const ModuleMountPlan& plan, ModuleRebuildOutcome& outcome, std::string& error,
         const std::vector<std::string>& ids = {"a.module", "z.module"}) {
    return rebuild_modules_recorded(
        plan, ids, [](std::string&) { return mutation("reset"); },
        [](const ModuleMountPlan::Mapping& m) {
            return mutation(std::string(m.merge ? "merge:" : "add:") + m.path + "<-" + m.source);
        },
        [](const std::string& path) { return mutation("hide:" + path); }, outcome, error);
}
ModuleRebuildHistory read_history() {
    ModuleRebuildHistory history;
    std::string error;
    CHECK(read_module_rebuild_history(history, error) == "valid");
    return history;
}
JsonValue status_json(int expected_code) {
    std::ostringstream output;
    auto* previous = std::cout.rdbuf(output.rdbuf());
    const int code = print_module_rebuild_status();
    std::cout.rdbuf(previous);
    CHECK(code == expected_code);
    JsonValue value;
    std::string error;
    CHECK(json::Parser(output.str(), true).parse(value, error));
    return value;
}
}  // namespace

namespace sumhp {
fs::path runtime_data_dir() {
    return "/fixture";
}
std::string runtime_boot_id() {
    return boot_changes && writes >= 2 ? "other-boot" : boot;
}
bool prepare_private_directory(const fs::path&, std::string& error) {
    ++preparations;
    if (fail_prepare) {
        error = "injected prepare failure";
        errno = EIO;
        return false;
    }
    return true;
}
bool sync_private_directory(const fs::path&, std::string& error) {
    if (fail_sync) {
        error = "injected directory sync failure";
        errno = EIO;
        return false;
    }
    return true;
}
}  // namespace sumhp
namespace ksud {
bool write_file_durable(const fs::path& path, const std::string& bytes, std::string& error) {
    const bool failed = ++writes == failed_write;
    if (!failed || expose_failed_write)
        files[path.generic_string()].bytes = bytes;
    if (mismatch_readback || writes == mismatch_write)
        files[path.generic_string()].bytes += " ";
    if (failed) {
        error = "injected durable replace failure";
        errno = EIO;
    }
    return !failed;
}
}  // namespace ksud

#define open fake_open
#define close fake_close
#define fstat fake_fstat
#define stat FakeStat
#define read fake_read
#define flock fake_flock
#define getrandom fake_random
#define unlink fake_unlink
#define S_ISREG(mode) ((mode) == 1)
#define O_RDONLY 1
#define O_RDWR 2
#define O_CREAT 4
#define O_CLOEXEC 8
#define O_NOFOLLOW 16
#define O_NONBLOCK 32
#define LOCK_EX 1
#define LOCK_NB 2
#include "module_rebuild_under_test.inc"

int main() {
    std::string error;
    ModuleRebuildOutcome outcome;
    const auto plan = sample();
    reset_test();
    ++groups;
    CHECK(module_rebuild_allowed(error) && files.empty() && !preparations && !random_calls &&
          !writes);
    auto value = status_json(0);
    CHECK(value.at("read_status").s == "missing" && value.at("intent_read_status").s == "missing");
    CHECK(!value.at("history_snapshot_atomic").b && !value.at("admission_is_authorization").b);
    std::cout.setstate(std::ios::badbit);
    const int failed_output = print_module_rebuild_status();
    std::cout.clear();
    CHECK(failed_output == 1);
    CHECK(value.at("runtime_verified").is_bool() && !value.at("runtime_verified").b);
    CHECK(files.empty() && !preparations && !writes && !random_calls && actions.empty());
    CHECK(run(plan, outcome, error) && outcome == ModuleRebuildOutcome::Applied &&
          actions.size() == 6);
    CHECK(writes == 3 && !files.count(intent_name) && locked == -1 && handles.empty());
    CHECK(actions[0] == "reset" && actions[1] == "add:/vendor/a<-/module/z" &&
          actions[2] == "merge:/vendor/a<-/module/a");
    auto history = read_history();
    const auto first_id = history.acknowledged->id;
    CHECK(history.latest->phase == "acknowledged" && history.latest->mappings_acknowledged == 3 &&
          history.latest->hides_acknowledged == 2);
    CHECK(history.latest->plan.mappings[2].path == plan.mappings[2].path &&
          history.latest->plan.mappings[2].source == plan.mappings[2].source);
    CHECK(module_rebuild_allowed(error));
    const auto good_bytes = files.at(history_name).bytes;

    ++groups;  // exact schema and lossless bytes, output preservation on all failures
    ModuleRebuildHistory decoded;
    CHECK(parse_module_rebuild_history(good_bytes, decoded, error));
    CHECK(encode_module_rebuild_history(decoded) == good_bytes);
    JsonValue document;
    CHECK(json::Parser(good_bytes, true).parse(document, error));
    const auto reject = [&](JsonValue candidate) {
        CHECK(!parse_module_rebuild_history(stringify_json(candidate), decoded, error));
        CHECK(decoded.latest->id == first_id);
    };
    for (const auto& bytes : {std::string("{}"), std::string("{\"schema_version\":2}"),
                              std::string(MODULE_REBUILD_MAX_BYTES + 1, 'x')})
        CHECK(!parse_module_rebuild_history(bytes, decoded, error) &&
              decoded.latest->id == first_id);
    auto bad = document;
    bad.o["future"] = true;
    reject(bad);
    bad = document;
    bad.o["schema_version"] = 2;
    reject(bad);
    bad = document;
    bad.o["latest"].o["future"] = true;
    reject(bad);
    bad = document;
    bad.o["latest"].o["plan"].o["mappings"].a[0].o["future"] = true;
    reject(bad);
    bad = document;
    bad.o["latest"].o["errno"] = 1;
    reject(bad);
    bad = document;
    bad.o["latest"].o["mappings_acknowledged"] = 2;
    reject(bad);
    bad = document;
    bad.o["acknowledged"] = JsonValue{};
    reject(bad);
    bad = document;
    bad.o["latest"].o["module_ids"].a.push_back("a.module");
    reject(bad);
    bad = document;
    bad.o["latest"].o["plan"].o["hide"].a.push_back(bad.o["latest"].o["plan"].o["hide"].a[0]);
    reject(bad);
    for (const auto& needle :
         {std::string("\"schema_version\":1"), std::string("\"phase\":\"acknowledged\""),
          std::string("\"encoding\":\"utf-8\"")}) {
        auto duplicate = good_bytes;
        const auto at = duplicate.find(needle);
        CHECK(at != std::string::npos);
        duplicate.insert(at, needle + ",");
        CHECK(!parse_module_rebuild_history(duplicate, decoded, error));
    }
    for (const auto& broken :
         {std::string("/bad\0tail", 9), std::string("relative"), std::string(4096, '/')}) {
        auto broken_plan = plan;
        broken_plan.mappings[0].path = broken;
        reset_test();
        CHECK(!run(broken_plan, outcome, error) && outcome == ModuleRebuildOutcome::Unchanged &&
              actions.empty() && !writes);
    }

    ++groups;  // every negative callback stops the sequence, can be partially published
    for (int failed = 0; failed < 6; ++failed) {
        reset_test();
        CHECK(run(plan, outcome, error));
        const auto prior = read_history().acknowledged->id;
        actions.clear();
        fail_action = failed;
        CHECK(!run(plan, outcome, error) && outcome == ModuleRebuildOutcome::PartialOrUnknown);
        CHECK(actions.size() == static_cast<std::size_t>(failed + 1) && errno == EFAULT);
        history = read_history();
        CHECK(history.latest->phase == "partial_or_unknown" && history.acknowledged->id == prior);
        CHECK(history.latest->reset_acknowledged == (failed != 0));
        CHECK(history.latest->mappings_acknowledged ==
              static_cast<std::size_t>(std::min(std::max(failed - 1, 0), 3)));
        CHECK(history.latest->hides_acknowledged ==
              static_cast<std::size_t>(std::max(failed - 4, 0)));
        CHECK(!module_rebuild_allowed(error) && errno == EBUSY && files.count(intent_name));
        const auto count_before = actions.size();
        const auto writes_before = writes;
        CHECK(!run(plan, outcome, error) && outcome == ModuleRebuildOutcome::Unchanged &&
              actions.size() == count_before && writes == writes_before);
    }

    ++groups;  // exceptions after mutation begins are also retained and never replayed
    for (int failed = 0; failed < 6; ++failed) {
        reset_test();
        throw_action = failed;
        CHECK(!run(plan, outcome, error) && outcome == ModuleRebuildOutcome::PartialOrUnknown);
        CHECK(actions.size() == static_cast<std::size_t>(failed + 1));
        history = read_history();
        CHECK(history.latest->failed_stage == "exception" &&
              history.latest->phase == "partial_or_unknown");
        CHECK(!module_rebuild_allowed(error));
    }

    ++groups;  // failed durable replace before/after rename, including exposed acknowledged final
               // bytes
    for (int step = 1; step <= 3; ++step)
        for (bool exposed : {false, true}) {
            reset_test();
            failed_write = step;
            expose_failed_write = exposed;
            CHECK(!run(plan, outcome, error));
            CHECK(actions.size() == (step == 3 ? 6u : 0u));
            CHECK(outcome ==
                  (step == 3 ? ModuleRebuildOutcome::Applied : ModuleRebuildOutcome::Unchanged));
            if (step == 3) {
                CHECK(files.count(intent_name) && !module_rebuild_allowed(error));
                history = read_history();
                CHECK(history.latest->phase == (exposed ? "acknowledged" : "pending"));
                value = status_json(0);
                CHECK(value.at("rebuild_admission").s == "blocked_unresolved_same_boot");
                const auto count_before = actions.size();
                const auto writes_before = writes;
                CHECK(!run(plan, outcome, error) && actions.size() == count_before &&
                      writes == writes_before);
            }
        }
    for (int step = 1; step <= 3; ++step) {
        reset_test();
        mismatch_write = step;
        CHECK(!run(plan, outcome, error) && actions.size() == (step == 3 ? 6u : 0u));
        CHECK(!module_rebuild_allowed(error));
        if (step == 3)
            CHECK(read_history().latest->phase == "acknowledged" && files.count(intent_name));
    }

    ++groups;  // random identity collision cannot overwrite a retained acknowledged record
    reset_test();
    CHECK(run(plan, outcome, error));
    const auto identity_bytes = files.at(history_name).bytes;
    actions.clear();
    random_seed = 1;
    const auto identity_writes = writes;
    CHECK(!run(plan, outcome, error) && errno == EEXIST &&
          outcome == ModuleRebuildOutcome::Unchanged);
    CHECK(actions.empty() && writes == identity_writes &&
          files.at(history_name).bytes == identity_bytes);

    ++groups;  // old acknowledged plan survives interrupted/new failed submissions
    reset_test();
    CHECK(run(plan, outcome, error));
    const auto prior = read_history().acknowledged->id;
    actions.clear();
    failed_write = writes + 3;
    CHECK(!run(plan, outcome, error));
    history = read_history();
    CHECK(history.latest->phase == "pending" && history.acknowledged->id == prior);
    const auto pending = files.at(history_name).bytes;
    const auto pending_intent = files.at(intent_name).bytes;
    failed_write = 0;
    boot = "next-boot";
    CHECK(module_rebuild_allowed(error));
    actions.clear();
    CHECK(run(plan, outcome, error));
    CHECK(read_history().latest->boot_id == "next-boot" && !files.count(intent_name));
    // A same-boot orphan intent remains fail closed, even when history is missing.
    reset_test();
    files[intent_name].bytes = pending_intent;
    CHECK(!module_rebuild_allowed(error));
    CHECK(!run(plan, outcome, error) && actions.empty());
    // Pending without intent can arise before the second preflight write.
    reset_test();
    files[history_name].bytes = pending;
    CHECK(!module_rebuild_allowed(error));
    CHECK(!run(plan, outcome, error) && actions.empty());

    ++groups;  // unsupported/corrupt history or latch is never overwritten
    for (const auto& path : {history_name, intent_name})
        for (int fault = 0; fault < 5; ++fault) {
            reset_test();
            files[path] = {fault == 0   ? "{\"schema_version\":2}"
                           : fault == 1 ? "invalid"
                                        : good_bytes,
                           fault == 2 ? 2 : 1, fault == 3 ? 2 : 1};
            if (fault == 4)
                files[path].bytes = std::string(MODULE_REBUILD_MAX_BYTES + 1, 'x');
            const auto original = files.at(path).bytes;
            CHECK(!module_rebuild_allowed(error));
            CHECK(!run(plan, outcome, error));
            CHECK(actions.empty() && !writes && files.at(path).bytes == original);
            value = status_json(1);
            CHECK(value.at("rebuild_admission").s == "unknown_blocked");
        }

    ++groups;  // setup/read/lock/random/durability-readback failures do not call reset
    for (int fault = 0; fault < 9; ++fault) {
        reset_test();
        if (fault == 0)
            fail_prepare = true;
        if (fault == 1)
            fail_open = true;
        if (fault == 2)
            fail_fstat = true;
        if (fault == 3)
            fail_lock = true;
        if (fault == 4)
            fail_random = true;
        if (fault == 5)
            boot.clear();
        if (fault == 6) {
            files[history_name].bytes = good_bytes;
            failed_read = 1;
        }
        if (fault == 7)
            mismatch_readback = true;
        if (fault == 8)
            boot_changes = true;
        CHECK(!run(plan, outcome, error) && outcome == ModuleRebuildOutcome::Unchanged &&
              actions.empty());
        CHECK(handles.empty() && locked == -1);
    }

    ++groups;  // complete receipt is retained; latch cleanup is metadata only
    reset_test();
    fail_unlink = true;
    CHECK(!run(plan, outcome, error) && outcome == ModuleRebuildOutcome::Applied &&
          actions.size() == 6);
    CHECK(read_history().latest->phase == "acknowledged" && !module_rebuild_allowed(error));
    reset_test();
    fail_sync = true;
    CHECK(!run(plan, outcome, error) && outcome == ModuleRebuildOutcome::Applied &&
          actions.size() == 6);
    CHECK(read_history().latest->phase == "acknowledged" && !files.count(intent_name) &&
          module_rebuild_allowed(error));

    ++groups;  // cumulative encoded capacity and all future successful receipts preflight before
               // reset
    reset_test();
    ModuleMountPlan oversized;
    oversized.mappings.assign(MODULE_REBUILD_MAX_ACTIONS + 1, {"/a", "/b", false});
    CHECK(!run(oversized, outcome, error) && actions.empty() && !writes);
    oversized.mappings.assign(128, {"/" + std::string(2048, '\x01'), "/source", false});
    CHECK(!run(oversized, outcome, error) && actions.empty() && !writes);
    oversized.mappings.assign(2200, {"/a", "/b", false});
    CHECK(!run(oversized, outcome, error) && actions.empty() && !writes);
    CHECK(!run(plan, outcome, error, {"same", "same"}) && actions.empty() && !writes);
    CHECK(!run(plan, outcome, error, {"../module"}) && actions.empty() && !writes);

    ++groups;  // largest valid old/new plans fit pending but not partial: reject before reset
    ModuleRebuildRecord boundary = *decoded.latest;
    boundary.id = std::string(32, 'a');
    boundary.module_ids.clear();
    boundary.plan.hide.clear();
    const auto build_boundary = [&](std::size_t filler) {
        auto record = boundary;
        record.plan.mappings.clear();
        while (filler) {
            const auto part = std::min(filler, std::size_t{4094});
            record.plan.mappings.push_back({"/a", "/" + std::string(part, 'x'), false});
            filler -= part;
        }
        record.phase = "acknowledged";
        record.reset_acknowledged = true;
        record.mappings_acknowledged = record.plan.mappings.size();
        record.hides_acknowledged = 0;
        record.failed_stage.clear();
        record.failed_index = -1;
        record.error_number = 0;
        return record;
    };
    std::size_t low = 0, high = MODULE_REBUILD_MAX_BYTES;
    while (low < high) {
        const auto middle = low + (high - low + 1) / 2;
        const auto record = build_boundary(middle);
        if (!encode_module_rebuild_history({record, record}).empty())
            low = middle;
        else
            high = middle - 1;
    }
    boundary = build_boundary(low);
    auto boundary_pending = boundary;
    boundary_pending.id = std::string(32, 'b');
    boundary_pending.phase = "pending";
    boundary_pending.reset_acknowledged = false;
    boundary_pending.mappings_acknowledged = 0;
    CHECK(!encode_module_rebuild_history({boundary_pending, boundary}).empty());
    auto boundary_partial = boundary;
    boundary_partial.id = std::string(32, 'b');
    boundary_partial.phase = "partial_or_unknown";
    boundary_partial.failed_stage = "exception";
    boundary_partial.error_number = 4095;
    CHECK(encode_module_rebuild_history({boundary_partial, boundary}).empty());
    reset_test();
    files[history_name].bytes = encode_module_rebuild_history({boundary, boundary});
    CHECK(!files[history_name].bytes.empty());
    const auto preserved_boundary = files[history_name].bytes;
    CHECK(!run(boundary.plan, outcome, error, {}) && outcome == ModuleRebuildOutcome::Unchanged &&
          actions.empty() && !writes);
    CHECK(files[history_name].bytes == preserved_boundary && !files.count(intent_name));

    reset_test();
    CHECK(run({}, outcome, error, {}) && actions.size() == 1 && actions[0] == "reset");
    CHECK(read_history().latest->plan.mappings.empty() && read_history().latest->plan.hide.empty());

    std::cout << "module rebuild production model/executor: " << groups << " groups, " << checks
              << " checks passed\n";
}
