// Portable production-store fault model. Not Linux filesystem/locking evidence.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "core/diagnostic_package.hpp"
#include "core/runtime.hpp"
#include "core/state_ledger.hpp"
#include "userspace/common/utf8.hpp"

namespace package_io {
unsigned checks = 0, groups = 0;
void check(bool value, const char* expression, int line) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "diagnostic_package_store_test.cpp:%d: %s\n", line, expression);
        std::exit(1);
    }
}
}  // namespace package_io
#define CHECK(value) ::package_io::check((value), #value, __LINE__)
namespace package_io {
constexpr int rdonly = 0, wronly = 1, rdwr = 2, creat = 4, excl = 8, nonblock = 16, cloexec = 32,
              nofollow = 64, directory = 128;
constexpr int lock_ex = 1, lock_nb = 2;
constexpr unsigned reg = 0100000, dir = 0040000, symlink = 0120000, fifo = 0010000, mask = 0170000;
const std::string root = "/fixture/data/adb/ksu/sumhp";
const std::string bank = root + "/diagnostic_packages.json",
                  stage = root + "/diagnostic_packages.stage",
                  lock = root + "/diagnostic_packages.lock";
struct Metadata {
    unsigned st_mode = reg | 0600, st_nlink = 1, st_uid = 0;
    std::uint64_t st_dev = 1, st_ino = 0;
    std::int64_t st_size = 0;
    timespec st_mtim{};
};
struct Node {
    Metadata metadata;
    std::string bytes;
};
struct Handle {
    std::shared_ptr<Node> node;
    std::string path;
    std::size_t offset = 0;
    int flags = 0;
};
struct Fault {
    std::string operation, path;
    int error = EIO;
    unsigned skip = 0;
    bool zero = false;
};
std::map<std::string, std::shared_ptr<Node>> files;
std::map<int, Handle> descriptors;
std::vector<std::string> events, hashes;
std::optional<Fault> fault;
std::function<void(const std::string&, const std::string&)> on_operation;
std::uint64_t serial = 1;
int next_fd = 3, prepare_count = 0, sync_count = 0;
bool fail_prepare = false, fail_sync = false, fail_hash = false, zero_result = false;
std::size_t max_read = SIZE_MAX, max_write = SIZE_MAX, stage_high_water = 0;
void touch(Node& node) {
    node.metadata.st_mtim = {static_cast<time_t>(++serial), 0};
}
std::shared_ptr<Node> put(const std::string& path, const std::string& bytes = {},
                          unsigned mode = reg | 0600) {
    auto node = std::make_shared<Node>();
    node->metadata.st_ino = ++serial;
    node->metadata.st_mode = mode;
    node->bytes = bytes;
    touch(*node);
    files[path] = node;
    return node;
}
void reset() {
    CHECK(descriptors.empty());
    files.clear();
    events.clear();
    hashes.clear();
    fault.reset();
    on_operation = {};
    prepare_count = sync_count = 0;
    fail_prepare = fail_sync = fail_hash = false;
    max_read = max_write = SIZE_MAX;
    stage_high_water = 0;
    put("/fixture/data/adb", {}, dir | 0700);
    put("/fixture/data/adb/ksu", {}, dir | 0755);
    put(root, {}, dir | 0700);
}
void inject(std::string operation, std::string path = {}, int error = EIO, unsigned skip = 0,
            bool zero = false) {
    fault = Fault{std::move(operation), std::move(path), error, skip, zero};
}
bool fails(const std::string& operation, const std::string& path) {
    events.push_back(operation + ":" + path);
    zero_result = false;
    if (on_operation)
        on_operation(operation, path);
    if (!fault || fault->operation != operation || (!fault->path.empty() && fault->path != path))
        return false;
    if (fault->skip) {
        --fault->skip;
        return false;
    }
    errno = fault->error;
    zero_result = fault->zero;
    fault.reset();
    return true;
}
template <class C>
std::string key(const C* path) {
    return std::filesystem::path(path).generic_string();
}
template <class C>
int open(const C* raw, int flags, unsigned mode = 0600) {
    const auto path = key(raw);
    CHECK((flags & (nofollow | cloexec)) == (nofollow | cloexec));
    if (!(flags & directory))
        CHECK((flags & nonblock) != 0);
    if (fails("open", path))
        return -1;
    auto found = files.find(path);
    if (found == files.end()) {
        if (!(flags & creat)) {
            errno = ENOENT;
            return -1;
        }
        put(path, {}, reg | mode);
        found = files.find(path);
    } else if ((flags & creat) && (flags & excl)) {
        errno = EEXIST;
        return -1;
    }
    if ((found->second->metadata.st_mode & mask) == symlink) {
        errno = ELOOP;
        return -1;
    }
    if ((flags & directory) && (found->second->metadata.st_mode & mask) != dir) {
        errno = ENOTDIR;
        return -1;
    }
    const int fd = next_fd++;
    descriptors.emplace(fd, Handle{found->second, path, 0, flags});
    return fd;
}
int close(int fd) {
    CHECK(descriptors.count(fd) == 1);
    const auto path = descriptors.at(fd).path;
    descriptors.erase(fd);
    return fails("close", path) ? -1 : 0;
}
int fstat(int fd, Metadata* output) {
    CHECK(descriptors.count(fd) == 1);
    auto& handle = descriptors.at(fd);
    if (fails("fstat", handle.path))
        return -1;
    *output = handle.node->metadata;
    output->st_size = static_cast<std::int64_t>(handle.node->bytes.size());
    return 0;
}
template <class C>
int lstat(const C* raw, Metadata* output) {
    const auto path = key(raw);
    if (fails("lstat", path))
        return -1;
    const auto found = files.find(path);
    if (found == files.end()) {
        errno = ENOENT;
        return -1;
    }
    *output = found->second->metadata;
    output->st_size = static_cast<std::int64_t>(found->second->bytes.size());
    return 0;
}
std::ptrdiff_t read(int fd, void* buffer, std::size_t size) {
    CHECK(descriptors.count(fd) == 1);
    auto& h = descriptors.at(fd);
    if (fails("read", h.path))
        return -1;
    const auto count =
        std::min({size, max_read, h.node->bytes.size() - std::min(h.offset, h.node->bytes.size())});
    std::memcpy(buffer, h.node->bytes.data() + std::min(h.offset, h.node->bytes.size()), count);
    h.offset += count;
    return static_cast<std::ptrdiff_t>(count);
}
std::ptrdiff_t write(int fd, const void* buffer, std::size_t size) {
    CHECK(descriptors.count(fd) == 1);
    auto& h = descriptors.at(fd);
    if (fails("write", h.path))
        return zero_result ? 0 : -1;
    CHECK(h.path == stage && (h.flags & wronly));
    const auto count = std::min(size, max_write);
    h.node->bytes.append(static_cast<const char*>(buffer), count);
    touch(*h.node);
    stage_high_water = std::max(stage_high_water, h.node->bytes.size());
    CHECK(stage_high_water <= sumhp::diagnostic_package::bank_limit);
    return static_cast<std::ptrdiff_t>(count);
}
int flock(int fd, int flags) {
    CHECK(descriptors.count(fd) == 1 && descriptors.at(fd).path == lock &&
          flags == (lock_ex | lock_nb));
    return fails("flock", lock) ? -1 : 0;
}
int fsync(int fd) {
    CHECK(descriptors.count(fd) == 1);
    return fails("fsync", descriptors.at(fd).path) ? -1 : 0;
}
template <class A, class B>
int rename(const A* from_raw, const B* to_raw) {
    const auto from = key(from_raw), to = key(to_raw);
    CHECK(from == stage && to == bank);
    if (fails("rename", from + "->" + to))
        return -1;
    CHECK(files.count(from) == 1);
    const auto node = files.at(from);
    files.erase(from);
    files[to] = node;
    return 0;
}
template <class C>
int unlink(const C* raw) {
    const auto path = key(raw);
    CHECK(path == stage);
    if (fails("unlink", path))
        return -1;
    if (!files.erase(path)) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}
std::string bytes(const std::string& path = bank) {
    const auto it = files.find(path);
    return it == files.end() ? std::string{} : it->second->bytes;
}
bool query_events_only(std::size_t from) {
    for (; from < events.size(); ++from) {
        const auto& e = events[from];
        if (e.rfind("open:", 0) != 0 && e.rfind("close:", 0) != 0 && e.rfind("read:", 0) != 0 &&
            e.rfind("fstat:", 0) != 0 && e.rfind("lstat:", 0) != 0)
            return false;
    }
    return true;
}
}  // namespace package_io
namespace sumhp {
std::filesystem::path runtime_data_dir() {
    return package_io::root;
}
bool prepare_private_directory(const std::filesystem::path& path, std::string& error) {
    using namespace package_io;
    ++prepare_count;
    events.emplace_back("prepare");
    CHECK(path.generic_string() == root);
    if (fail_prepare) {
        error = "private sentinel failure";
        return false;
    }
    if (!files.count(root))
        put(root, {}, dir | 0700);
    return true;
}
bool sync_private_directory(const std::filesystem::path& path, std::string& error) {
    using namespace package_io;
    ++sync_count;
    events.emplace_back("sync-private:" + path.generic_string());
    if (fail_sync) {
        error = "private sentinel failure";
        return false;
    }
    return true;
}
namespace state {
std::string revision_for_bytes(const std::string& bytes) {
    using namespace package_io;
    hashes.push_back(bytes);
    if (fail_hash)
        return {};
    std::uint64_t h = 1469598103934665603ULL;
    for (const unsigned char c : bytes)
        h = (h ^ c) * 1099511628211ULL;
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "sha256-v1:";
    for (unsigned i = 0; i < 32; ++i) {
        const auto n = static_cast<unsigned char>((h >> ((i % 8) * 8)) ^ i);
        result += hex[n >> 4];
        result += hex[n & 15];
    }
    return result;
}
}  // namespace state
}  // namespace sumhp

#include "diagnostic_package_store_under_test.inc"

namespace {
using namespace sumhp;
using namespace sumhp::diagnostic_package;
using namespace package_io;
std::vector<JsonValue> samples;
std::string id(unsigned value) {
    std::ostringstream out;
    out << std::hex << value;
    const auto digits = out.str();
    return std::string(32 - digits.size(), '0') + digits;
}
std::string rev(const JsonValue& package) {
    return state::revision_for_bytes(stringify_json(package) + "\n");
}
void outcome(const Reply& reply, int code, const char* status, const char* reason) {
    CHECK(reply.exit_code == code);
    CHECK(reply.value.at("status").s == status && reply.value.at("reason").s == reason);
    CHECK(reply.value.at("kind").s == "sumh.diagnostic.package.result");
    CHECK(!reply.value.at("active_configuration_changed").b &&
          !reply.value.at("runtime_changed").b);
    CHECK(descriptors.empty());
}
JsonValue bank_with(const JsonValue& entries) {
    auto v = JsonValue::object();
    v.o["schema_version"] = 1;
    v.o["kind"] = "sumh.diagnostic.package.bank";
    v.o["packages"] = entries;
    return v;
}
void seed(unsigned count = 1) {
    auto entries = JsonValue::object();
    for (unsigned i = 1; i <= count; ++i)
        entries.o[id(i)] = samples[(i - 1) % samples.size()];
    put(bank, stringify_json(bank_with(entries)) + "\n");
}
void query_and_lifecycle() {
    ++groups;
    reset();
    auto out = list_packages();
    outcome(out, 0, "observed", "store_observed");
    CHECK(out.value.at("items").a.empty());
    outcome(show_package(id(1)), 3, "not_found", "package_missing");
    CHECK(prepare_count == 0 && sync_count == 0 && !files.count(lock) && query_events_only(0));
    put(root + "/config.json", "configuration-sentinel");
    put(root + "/sumhp.log", "log-sentinel");
    for (unsigned i = 0; i < samples.size(); ++i) {
        const auto package_id = id(i + 1);
        out = create_package(package_id, samples[i]);
        outcome(out, 0, "saved", "package_saved");
        CHECK(out.value.at("saved").b && out.value.at("durability_confirmed").b &&
              out.value.at("readback_confirmed").b);
        CHECK(out.value.at("privacy_policy").s == samples[i].at("privacy").at("policy").s);
        const auto expected = rev(samples[i]);
        CHECK(out.value.at("package_revision").s == expected);
        const auto event_count = events.size();
        const auto prepare_before = prepare_count, sync_before = sync_count;
        const auto previous = bytes();
        out = show_package(package_id, expected);
        outcome(out, 0, "observed", "package_observed");
        CHECK(out.value.at("export_bytes").s == stringify_json(samples[i]) + "\n");
        CHECK(out.value.at("export_byte_count").n == out.value.at("export_bytes").s.size());
        CHECK(out.value.at("package_revision").s ==
              state::revision_for_bytes(out.value.at("export_bytes").s));
        CHECK(stringify_json(out.value.at("package")) == stringify_json(samples[i]));
        CHECK(bytes() == previous && prepare_count == prepare_before && sync_count == sync_before &&
              query_events_only(event_count));
    }
    out = list_packages();
    outcome(out, 0, "observed", "store_observed");
    CHECK(out.value.at("items").a.size() == samples.size());
    for (const auto& item : out.value.at("items").a) {
        CHECK(item.o.size() == 6);
        CHECK(!item.contains("package") && !item.contains("target") && !item.contains("boot_id"));
    }
    const auto before = bytes();
    outcome(create_package(id(1), samples.back()), 4, "conflict", "id_exists");
    CHECK(bytes() == before);
    const auto wrong = "sha256-v1:" + std::string(64, 'f');
    out = show_package(id(1), wrong);
    outcome(out, 4, "conflict", "revision_conflict");
    CHECK(!out.value.contains("package") && !out.value.contains("export_bytes"));
    outcome(remove_package(id(1), wrong), 4, "conflict", "revision_conflict");
    CHECK(bytes() == before);
    out = remove_package(id(1), rev(samples[0]));
    outcome(out, 0, "deleted", "package_deleted");
    CHECK(out.value.at("deleted").b && !out.value.at("saved").b);
    outcome(show_package(id(1)), 3, "not_found", "package_missing");
    outcome(show_package(id(2), rev(samples[1])), 0, "observed", "package_observed");
    outcome(remove_package(id(1), rev(samples[0])), 3, "not_found", "package_missing");
    CHECK(bytes(root + "/config.json") == "configuration-sentinel" &&
          bytes(root + "/sumhp.log") == "log-sentinel");
}
void input_and_capacity() {
    ++groups;
    reset();
    for (const auto bad : {"", "../path", "UPPER", "123"}) {
        outcome(create_package(bad, samples[0]), 2, "invalid", "invalid_request");
        outcome(show_package(bad), 2, "invalid", "invalid_request");
    }
    outcome(remove_package(id(1), "bad"), 2, "invalid", "invalid_request");
    outcome(show_package(id(1), "bad"), 2, "invalid", "invalid_request");
    auto invalid = samples[0];
    invalid.o["private-secret"] = "must-never-return";
    outcome(create_package(id(1), invalid), 2, "invalid", "invalid_request");
    CHECK(prepare_count == 0 && events.empty());
    seed(7);
    outcome(create_package(id(8), samples[0]), 0, "saved", "package_saved");
    CHECK(list_packages().value.at("items").a.size() == 8);
    const auto full = bytes();
    outcome(create_package(id(9), samples[0]), 5, "not_saved", "store_capacity");
    CHECK(bytes() == full);
    seed(9);
    outcome(list_packages(), 5, "unavailable", "store_unreadable");
    seed();
    auto exact = bytes();
    exact.resize(bank_limit, ' ');
    put(bank, exact);
    outcome(show_package(id(1)), 0, "observed", "package_observed");
    put(bank, exact + " ");
    outcome(list_packages(), 5, "unavailable", "store_unreadable");
    const auto too_large = bytes();
    outcome(create_package(id(2), samples[1]), 5, "not_saved", "store_unreadable");
    CHECK(bytes() == too_large);
}
void corrupt_and_unsafe() {
    ++groups;
    const auto good = stringify_json(bank_with(JsonValue::object()));
    for (const auto& corrupt : std::vector<std::string>{
             "", "broken-private-secret", "{}",
             "{\"kind\":\"sumh.diagnostic.package.bank\",\"schema_version\":2,\"packages\":{}}",
             "{\"kind\":\"sumh.diagnostic.package.bank\",\"schema_version\":1,\"schema_version\":1,"
             "\"packages\":{}}",
             good.substr(0, good.size() - 1) + ",\"future\":1}"}) {
        reset();
        put(bank, corrupt);
        put(stage, "unrelated-stale-sentinel");
        const auto shown = list_packages();
        outcome(shown, 5, "unavailable", "store_unreadable");
        CHECK(stringify_json(shown.value).find("private-secret") == std::string::npos);
        outcome(create_package(id(1), samples[0]), 5, "not_saved", "store_unreadable");
        CHECK(bytes() == corrupt && bytes(stage) == "unrelated-stale-sentinel");
    }
    for (const auto& target : std::vector<std::string>{bank, lock, stage})
        for (int bad = 0; bad < 6; ++bad) {
            reset();
            seed();
            auto node = target == bank ? files.at(bank) : put(target, "private-sentinel");
            if (bad == 0)
                node->metadata.st_mode = symlink | 0600;
            if (bad == 1)
                node->metadata.st_mode = fifo | 0600;
            if (bad == 2)
                node->metadata.st_mode = dir | 0600;
            if (bad == 3)
                node->metadata.st_nlink = 2;
            if (bad == 4)
                node->metadata.st_uid = 1000;
            if (bad == 5)
                node->metadata.st_mode = reg | 0644;
            const auto before = bytes();
            const auto other = bytes(target);
            if (target == bank)
                outcome(show_package(id(1)), 5, "unavailable", "store_unreadable");
            const auto out = create_package(id(2), samples[1]);
            CHECK(out.exit_code == 5 && out.value.at("status").s == "not_saved");
            CHECK(bytes() == before && bytes(target) == other);
            CHECK(descriptors.empty());
        }
    for (const auto& target :
         std::vector<std::string>{root, "/fixture/data/adb/ksu", "/fixture/data/adb"})
        for (int bad = 0; bad < 3; ++bad) {
            reset();
            seed();
            auto& mode = files.at(target)->metadata;
            if (bad == 0)
                mode.st_uid = 1000;
            if (bad == 1)
                mode.st_mode = symlink | 0700;
            if (bad == 2)
                mode.st_mode = dir | 0777;
            outcome(list_packages(), 5, "unavailable", "store_unreadable");
            outcome(create_package(id(2), samples[0]), 5, "not_saved",
                    "private_directory_unavailable");
            CHECK(prepare_count == 0);
        }
    reset();
    seed();
    put(stage, "bounded-stale");
    outcome(create_package(id(2), samples[1]), 0, "saved", "package_saved");
    CHECK(!files.count(stage));
    reset();
    seed();
    put(stage, std::string(bank_limit + 1, 'x'));
    const auto before = bytes();
    outcome(create_package(id(2), samples[1]), 5, "not_saved", "store_not_replaced");
    CHECK(bytes() == before && bytes(stage).size() == bank_limit + 1);
}
void durability_faults() {
    ++groups;
    for (const auto& seam :
         std::vector<std::pair<std::string, std::string>>{{"open", stage},
                                                          {"fstat", stage},
                                                          {"write", stage},
                                                          {"fsync", stage},
                                                          {"close", stage},
                                                          {"rename", stage + "->" + bank}}) {
        reset();
        seed();
        const auto before = bytes();
        inject(seam.first, seam.second);
        const auto out = create_package(id(2), samples[1]);
        outcome(out, 5, "not_saved", "store_not_replaced");
        CHECK(!fault && bytes() == before && !out.value.at("saved").b);
        CHECK(!files.count(stage));
    }
    for (const auto* seam : {"fsync", "close"}) {
        reset();
        seed();
        inject(seam, root);
        const auto out = create_package(id(2), samples[1]);
        outcome(out, 5, "unknown", "replaced_durability_unconfirmed");
        CHECK(!fault && !out.value.at("saved").b && !out.value.at("durability_confirmed").b &&
              out.value.at("readback_confirmed").b);
        outcome(show_package(id(2), rev(samples[1])), 0, "observed", "package_observed");
    }
    reset();
    seed();
    inject("flock", lock, EWOULDBLOCK);
    outcome(create_package(id(2), samples[1]), 5, "not_saved", "store_lock_unavailable");
    CHECK(!fault);
    reset();
    seed();
    fail_prepare = true;
    outcome(create_package(id(2), samples[1]), 5, "not_saved", "private_directory_unavailable");
    reset();
    seed();
    fail_sync = true;
    outcome(create_package(id(2), samples[1]), 5, "not_saved", "directory_durability_unconfirmed");
    reset();
    seed();
    fail_hash = true;
    outcome(show_package(id(1)), 5, "unavailable", "revision_unavailable");
    outcome(create_package(id(2), samples[1]), 5, "not_saved", "revision_unavailable");
    // Same durability distinction applies to deletion: no rollback/replay.
    reset();
    seed(2);
    inject("fsync", root);
    const auto out = remove_package(id(1), rev(samples[0]));
    outcome(out, 5, "unknown", "replaced_durability_unconfirmed");
    CHECK(!out.value.at("deleted").b && !fault);
    outcome(show_package(id(1)), 3, "not_found", "package_missing");
    outcome(show_package(id(2)), 0, "observed", "package_observed");
}
void io_and_race_faults() {
    ++groups;
    for (const auto* operation : {"read", "write", "fsync"}) {
        reset();
        seed();
        max_read = 17;
        max_write = 13;
        inject(operation, std::string(operation) == "read" ? bank : stage, EINTR);
        outcome(create_package(id(2), samples[1]), 0, "saved", "package_saved");
        CHECK(!fault);
    }
    for (const bool zero : {false, true}) {
        reset();
        seed();
        const auto before = bytes();
        max_write = 7;
        inject("write", stage, ENOSPC, zero ? 0 : 1, zero);
        outcome(create_package(id(2), samples[1]), 5, "not_saved", "store_not_replaced");
        CHECK(!fault && bytes() == before);
    }
    // Change exact bytes before the second read; whitespace-only changes still
    // conflict even though parsed content and its package revisions match.
    reset();
    seed();
    unsigned opens = 0;
    on_operation = [&](const std::string& operation, const std::string& path) {
        if (operation == "open" && path == bank && ++opens == 2) {
            files.at(bank)->bytes += " ";
            touch(*files.at(bank));
        }
    };
    outcome(create_package(id(2), samples[1]), 4, "conflict", "store_changed");
    CHECK(!files.count(stage));
    on_operation = {};
    // Readback failure after durable replacement must report unknown and leave
    // the package available for a subsequent explicit query.
    reset();
    seed();
    inject("open", bank, EACCES, 2);
    const auto out = create_package(id(2), samples[1]);
    outcome(out, 5, "unknown", "replaced_readback_unconfirmed");
    CHECK(!fault && out.value.at("durability_confirmed").b &&
          !out.value.at("readback_confirmed").b && out.value.at("package_revision").is_null());
    outcome(show_package(id(2)), 0, "observed", "package_observed");
    // Mutating the opened source during read is rejected, including stable-size
    // replacement, without exporting any content from the incoherent snapshot.
    reset();
    seed();
    bool changed = false;
    on_operation = [&](const std::string& operation, const std::string& path) {
        if (!changed && operation == "read" && path == bank) {
            changed = true;
            touch(*files.at(bank));
        }
    };
    const auto invalid = show_package(id(1));
    outcome(invalid, 5, "unavailable", "store_unreadable");
    CHECK(!invalid.value.contains("export_bytes"));
    on_operation = {};
}
}  // namespace
int main(int argc, char** argv) {
    CHECK(argc == 2);
    for (const auto* scenario :
         {"rule-not-effective", "module-fallback", "maps-failure", "boot-protection"}) {
        const auto path = std::filesystem::path(argv[1]) / (std::string(scenario) + "-report.json");
        std::ifstream input(path, std::ios::binary);
        CHECK(input.good());
        std::ostringstream bytes;
        bytes << input.rdbuf();
        JsonValue package;
        std::string reason;
        CHECK(build_package(bytes.str(), false, package, reason));
        samples.push_back(package);
    }
    query_and_lifecycle();
    input_and_capacity();
    corrupt_and_unsafe();
    durability_faults();
    io_and_race_faults();
    std::cout << "Diagnostic package production store portable model: " << groups << " groups / "
              << checks << " checks passed; no Linux runtime claim.\n";
}
