#include "mount/development.hpp"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>
#include "core/config_document.hpp"
#include "core/partition_config.hpp"
#include "core/rule_inventory.hpp"
#include "mount/partition_discovery.hpp"
#include "userspace/ksud/src/durable_file.hpp"
constexpr int O_RDONLY = 1, O_CLOEXEC = 2, O_NOFOLLOW = 4, O_NONBLOCK = 8, O_DIRECTORY = 16,
              O_CREAT = 32, O_RDWR = 64;
constexpr int LOCK_EX = 1, LOCK_NB = 2, LOCK_UN = 4, PROT_READ = 1, PROT_WRITE = 2, MAP_SHARED = 1,
              MAP_ANONYMOUS = 2, CLONE_NEWNS = 1;
#define MAP_FAILED reinterpret_cast<void*>(-1)
namespace model {
using sumhp::JsonValue;
struct Metadata {
    unsigned st_mode = 0100600, st_nlink = 1, st_uid = 0;
    std::int64_t st_size = 0;
    std::uint64_t st_ino = 3, st_dev = 8;
};
struct File {
    std::string bytes;
    Metadata meta;
    int error = 0, read_error = 0, stat_error = 0;
    bool override_size = false;
};
struct Handle {
    File file;
    std::size_t offset = 0;
};
std::map<std::string, File> files;
std::map<int, Handle> handles;
int next_fd = 10, opens = 0, locks = 0, writes = 0, scans = 0, queries = 0, setns_calls = 0,
    normalizes = 0, inspects = 0;
int lock_error = 0, prepare_error = 0, namespace_error = 0, restore_error = 0,
    read_fail_after_write = 0;
bool query_fail = false, query_fail_after = false, hash_failure = false, scan_complete = true,
     scan_change = false, normalize_ok = true, normalize_change = false;
std::string selected_namespace = "init", boot = "boot";
std::function<void(int)> scan_hook;
std::function<void()> write_hook;
std::vector<std::string> eligible{"extra"};
ksud::DurableWriteState write_state = ksud::DurableWriteState::Complete;
ksud::DurableWriteState backup_state = ksud::DurableWriteState::Complete;
std::int64_t ticks = 0, clock_step = 0;
struct Clock {
    static auto now() {
        ticks += clock_step;
        return std::chrono::steady_clock::time_point(std::chrono::milliseconds(ticks));
    }
};
bool is_regular(unsigned mode) {
    return (mode & 0170000) == 0100000;
}
bool is_directory(unsigned mode) {
    return (mode & 0170000) == 0040000;
}
int open(const std::filesystem::path& raw, int flags, int mode = 0) {
    const auto path = std::filesystem::path(raw).generic_string();
    ++opens;
    if (flags & O_CREAT) {
        assert(path == "/private/config.json.lock" && mode == 0600);
        if (!files.count(path))
            files[path] = {};
    } else
        assert((flags & (O_RDONLY | O_CLOEXEC)) == (O_RDONLY | O_CLOEXEC));
    const auto found = files.find(path);
    if (found == files.end()) {
        errno = ENOENT;
        return -1;
    }
    if (found->second.error) {
        errno = found->second.error;
        return -1;
    }
    const int fd = next_fd++;
    handles[fd] = {found->second, 0};
    return fd;
}
int close(int fd) {
    assert(handles.erase(fd) == 1);
    return 0;
}
int fstat(int fd, Metadata* value) {
    const auto& file = handles.at(fd).file;
    if (file.stat_error) {
        errno = file.stat_error;
        return -1;
    }
    *value = file.meta;
    if (!file.override_size)
        value->st_size = static_cast<std::int64_t>(file.bytes.size());
    return 0;
}
int stat(const std::filesystem::path& raw, Metadata* value) {
    const int fd = open(raw, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    const int rc = fstat(fd, value);
    close(fd);
    return rc;
}
int read(int fd, char* buffer, std::size_t count) {
    auto& h = handles.at(fd);
    if (h.file.read_error) {
        errno = h.file.read_error;
        return -1;
    }
    const auto n = std::min(count, h.file.bytes.size() - h.offset);
    std::memcpy(buffer, h.file.bytes.data() + h.offset, n);
    h.offset += n;
    return static_cast<int>(n);
}
int flock(int, int flags) {
    if (flags == LOCK_UN)
        return 0;
    ++locks;
    assert(flags == (LOCK_EX | LOCK_NB));
    if (lock_error) {
        errno = lock_error;
        return -1;
    }
    return 0;
}
void* mmap(void*, std::size_t size, int, int, int, int) {
    return ::operator new(size);
}
int munmap(void* value, std::size_t) {
    ::operator delete(value);
    return 0;
}
int setns(int fd, int type) {
    assert(type == CLONE_NEWNS);
    ++setns_calls;
    if ((setns_calls == 1 && namespace_error) || (setns_calls > 1 && restore_error)) {
        errno = EACCES;
        return -1;
    }
    selected_namespace = handles.at(fd).file.meta.st_ino == 55 ? "init" : "original";
    return 0;
}
}  // namespace model
#include "development_checked.inc"
namespace sumhp {
std::filesystem::path runtime_data_dir() {
    return "/private";
}
std::filesystem::path runtime_config_file() {
    return "/private/config.json";
}
std::string runtime_boot_id() {
    return model::boot;
}
bool prepare_private_directory(const std::filesystem::path&, std::string& error) {
    if (model::prepare_error) {
        errno = EACCES;
        error = "prepare failed";
        return false;
    }
    return true;
}
namespace state {
std::string revision_for_bytes(const std::string& bytes) {
    if (model::hash_failure)
        return {};
    std::ostringstream out;
    out << std::hex << std::hash<std::string>{}(bytes);
    auto value = out.str();
    return "sha256-v1:" + std::string(64 - value.size(), '0') + value;
}
}  // namespace state
#include "development_config.inc"
namespace mount {
#include "development_paths.inc"
bool discover_extra_partitions(const std::vector<std::string>& saved, PartitionDiscovery& result,
                               std::string& error) {
    ++model::scans;
    if (model::scan_hook)
        model::scan_hook(model::scans);
    auto out = JsonValue::object();
    out.o["scope"] = "installed_module_candidates_not_mount_readiness";
    out.o["saved_partitions"] = JsonValue::array();
    for (const auto& name : saved)
        out.o["saved_partitions"].a.emplace_back(name);
    out.o["candidates"] = JsonValue::array();
    out.o["issues"] = JsonValue::array();
    for (const auto& name : model::eligible) {
        auto row = JsonValue::object();
        row.o["name"] = name;
        row.o["applicable"] = true;
        row.o["saved"] = std::find(saved.begin(), saved.end(), name) != saved.end();
        out.o["candidates"].a.push_back(row);
    }
    result.output = out;
    result.complete = model::scan_complete;
    result.observation_signature = model::scan_change ? std::to_string(model::scans) : "stable";
    if (!result.complete)
        error = "incomplete scan";
    return result.complete;
}
namespace fsutil {
bool run_query_in_init_mount_ns(const std::function<bool()>& fn) {
    ++model::queries;
    if (model::query_fail) {
        errno = ETIMEDOUT;
        return false;
    }
    const bool result = fn();
    return result && !model::query_fail_after;
}
}  // namespace fsutil
namespace magic {
bool prepare_normalization(const std::string& path, NormalizationPlan& plan, std::string& error) {
    ++model::normalizes;
    if (!model::normalize_ok) {
        error = "layout conflict";
        return false;
    }
    plan.requested_root = path;
    plan.complete = true;
    plan.bytes = model::normalize_change ? model::normalizes : 1;
    return true;
}
bool execute_normalization(const NormalizationPlan&, NormalizationResult& result, std::string&) {
    result.complete = true;
    result.status = "completed";
    return true;
}
}  // namespace magic
JsonValue module_inspect_observation(const std::string&) {
    ++model::inspects;
    auto out = JsonValue::object();
    out.o["kind"] = "sumh.module.inspect";
    return out;
}
}  // namespace mount
}  // namespace sumhp
namespace ksud {
DurableWriteResult replace_file_durable(const std::filesystem::path& path,
                                        const std::string& content, mode_t) {
    ++model::writes;
    const auto name = path.generic_string();
    const bool backup = name.find(".bak") != std::string::npos;
    const auto state = backup ? model::backup_state : model::write_state;
    if (state != DurableWriteState::NotCommitted)
        model::files[name].bytes = content;
    if (!backup && model::read_fail_after_write)
        model::files[name].read_error = EIO;
    if (model::write_hook)
        model::write_hook();
    return {state, state == DurableWriteState::Complete ? 0 : EIO};
}
}  // namespace ksud
#include "development_controller.inc"
using namespace sumhp;
using namespace sumhp::mount;
using namespace model;
int cases = 0;
void reset() {
    assert(handles.empty());
    files.clear();
    next_fd = 10;
    opens = locks = writes = scans = queries = setns_calls = normalizes = inspects = 0;
    lock_error = prepare_error = namespace_error = restore_error = read_fail_after_write = 0;
    query_fail = query_fail_after = hash_failure = scan_change = normalize_change = false;
    scan_complete = normalize_ok = true;
    write_state = backup_state = ksud::DurableWriteState::Complete;
    scan_hook = {};
    write_hook = {};
    eligible = {"extra"};
    ticks = clock_step = 0;
    boot = "boot";
    files["/private"].meta.st_mode = 0040700;
    files["/private/config.json"].bytes = default_config_json();
    files["/proc/1/ns/mnt"].meta.st_ino = 55;
    files["/proc/self/ns/mnt"].meta.st_ino = 55;
    errno = 0;
}
JsonValue save(const char* status, const std::string& names = "[\"extra\"]", std::string rev = {}) {
    if (rev.empty())
        rev = state::revision_for_bytes(files["/private/config.json"].bytes);
    auto result = save_partitions_conditional(rev, names);
    ++cases;
    if (result.at("status").s != status)
        std::cerr << "case " << cases << ": " << stringify_json(result) << '\n';
    assert(result.at("status").s == status && !result.at("applied").b && handles.empty());
    return result;
}
JsonValue query(const char* status, DevelopmentKind kind = DevelopmentKind::PartitionDiscovery) {
    DevelopmentRequest req;
    req.kind = kind;
    req.module_path = "/staging/m";
    auto result = json::parse(development_query_json(req));
    ++cases;
    assert(result.at("status").s == status && writes == 0 && handles.empty());
    return result;
}
int main() {
    reset();
    auto out = save("saved");
    assert(out.at("saved").b && out.at("durability_confirmed").b && writes == 1 && scans == 1);
    const auto committed = files["/private/config.json"].bytes;
    out = save("unchanged");
    assert(out.at("saved").b && !out.at("durability_confirmed").b && writes == 1 &&
           files["/private/config.json"].bytes == committed);
    reset();
    save("conflict", "[]", "sha256-v1:" + std::string(64, 'a'));
    assert(writes == 0 && scans == 0);
    for (const auto* input :
         {"null", "{}", "[1]", "[\"bad/name\"]", "[\"extra\",\"extra\"]", "[\"x\\u0000\"]"}) {
        reset();
        save("rejected", input);
        assert(writes == 0 && locks == 0);
    }
    reset();
    save("rejected", "[]", "bad");
    assert(locks == 0);
    reset();
    save("rejected", std::string(PARTITIONS_INPUT_BYTES + 1, ' '));
    assert(locks == 0);
    reset();
    auto many = JsonValue::array();
    for (int i = 0; i < 129; ++i)
        many.a.emplace_back("p" + std::to_string(i));
    save("rejected", stringify_json(many));
    for (const auto* config :
         {"{", "{\"schema_version\":2}", "{\"unknown\":1}",
          "{\"partitions\":[],\"partitions\":[\"x\"]}", "{\"debug\":\"yes\"}"}) {
        reset();
        files["/private/config.json"].bytes = config;
        save("rejected");
        assert(writes == 0);
    }
    reset();
    files.erase("/private/config.json");
    save("rejected", "[]", "sha256-v1:" + std::string(64, 'a'));
    assert(writes == 0);
    reset();
    files["/private/config.json"].meta.st_nlink = 2;
    save("rejected");
    assert(writes == 0);
    reset();
    files["/private/config.json"].meta.st_mode = 0120600;
    save("rejected");
    reset();
    files["/private/config.json"].override_size = true;
    files["/private/config.json"].meta.st_size = 5 * 1024 * 1024;
    save("rejected");
    reset();
    files["/private/config.json"].read_error = EIO;
    save("rejected");
    reset();
    clock_step = 1500;
    save("rejected");
    reset();
    lock_error = EBUSY;
    save("failed");
    assert(writes == 0);
    reset();
    files["/private/config.json.lock"].meta.st_uid = 1000;
    save("failed");
    assert(locks == 0);
    reset();
    prepare_error = 1;
    save("failed");
    assert(locks == 0);
    reset();
    scan_complete = false;
    save("rejected");
    assert(writes == 0);
    reset();
    eligible.clear();
    save("rejected");
    assert(writes == 0);
    reset();
    files["/private/config.json"].bytes = "{\"partitions\":[\"old_unknown\"]}";
    eligible.clear();
    out = save("unchanged", "[\"old_unknown\"]");
    assert(scans == 0 && writes == 0);
    out = save("saved", "[]");
    assert(scans == 0 && writes == 2 &&
           files.at("/private/config.json.pre-schema-1.bak").bytes ==
               "{\"partitions\":[\"old_unknown\"]}");
    reset();
    scan_hook = [](int) { files["/private/config.json"].bytes = "{}"; };
    save("conflict");
    assert(writes == 0);
    reset();
    files["/private/config.json"].bytes = "{}";
    backup_state = ksud::DurableWriteState::NotCommitted;
    save("failed");
    assert(files["/private/config.json"].bytes == "{}");
    reset();
    files["/private/config.json"].bytes = "{}";
    write_hook = []() { files["/private/config.json"].bytes = "{\"debug\":true}"; };
    save("conflict");
    assert(writes == 1);
    reset();
    write_state = ksud::DurableWriteState::NotCommitted;
    out = save("failed");
    assert(!out.at("saved").b && out.at("readback_confirmed").b);
    reset();
    write_state = ksud::DurableWriteState::ReplacedDurabilityUnconfirmed;
    out = save("partial");
    assert(!out.at("saved").b && out.at("readback_confirmed").b);
    reset();
    read_fail_after_write = 1;
    out = save("unknown");
    assert(!out.at("readback_confirmed").b);
    reset();
    write_hook = []() { files["/private/config.json"].bytes = "{}"; };
    save("unknown");
    reset();
    hash_failure = true;
    save("failed", "[]", "sha256-v1:" + std::string(64, 'a'));
    assert(writes == 0);
    reset();
    out = query("complete");
    assert(out.at("selection_valid").b && scans == 2 && queries == 1);
    reset();
    scan_change = true;
    out = query("stale");
    assert(!out.at("selection_valid").b);
    reset();
    scan_complete = false;
    query("partial");
    reset();
    files["/private/config.json"].bytes = "{\"future\":1}";
    query("unknown");
    reset();
    query_fail = true;
    query("unknown");
    assert(scans == 0);
    reset();
    query_fail_after = true;
    query("unknown");
    reset();
    lock_error = EBUSY;
    query("unknown");
    assert(queries == 0);
    reset();
    files["/private"].meta.st_mode = 0040755;
    query("unknown");
    assert(queries == 0);
    reset();
    files["/proc/self/ns/mnt"].meta.st_ino = 66;
    query("unknown");
    assert(scans == 0);
    reset();
    query("complete", DevelopmentKind::NormalizePreview);
    assert(normalizes == 2);
    reset();
    normalize_ok = false;
    query("invalid", DevelopmentKind::NormalizePreview);
    reset();
    normalize_change = true;
    query("stale", DevelopmentKind::NormalizePreview);
    reset();
    query("unknown", DevelopmentKind::PartitionSave);
    assert(queries == 0 && writes == 0);
    reset();
    DevelopmentRequest req;
    req.kind = DevelopmentKind::PartitionSave;
    req.partitions = "[\"extra\"]";
    req.revision = state::revision_for_bytes(files["/private/config.json"].bytes);
    files["/proc/self/ns/mnt"].meta.st_ino = 66;
    out = json::parse(partition_save_json(req));
    ++cases;
    assert(out.at("saved").b && setns_calls == 2 && selected_namespace == "original" &&
           queries == 0);
    reset();
    files["/proc/self/ns/mnt"].meta.st_ino = 66;
    namespace_error = 1;
    out = json::parse(partition_save_json(req));
    ++cases;
    assert(!out.at("saved").b && writes == 0);
    reset();
    files["/proc/self/ns/mnt"].meta.st_ino = 66;
    restore_error = 1;
    out = json::parse(partition_save_json(req));
    ++cases;
    assert(out.at("saved").b && out.at("namespace_restore_failed").b);
    assert(handles.empty());
    std::cout << cases << " production conditional-save/development scenarios passed\n";
}
