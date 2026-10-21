// This executes production log.cpp with deterministic filesystem calls. It is
// a portable fault model, not proof of Linux flock, power-loss, or SELinux behavior.
#include "core/log.hpp"
#include "core/log_repeat.hpp"
#include "core/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace logfs {
unsigned checks = 0;
void check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "log_storage_test.cpp:%d: %s failed\n", line, expression);
        std::exit(EXIT_FAILURE);
    }
}
}  // namespace logfs
#define CHECK(expression) ::logfs::check((expression), #expression, __LINE__)

// The runner maps only production syscall identifiers to these model values.
namespace logfs {
constexpr int read_only = 0, write_only = 1, read_write = 2;
constexpr int create = 4, exclusive = 8, truncate = 16, append = 32;
constexpr int cloexec = 64, nofollow = 128, nonblock = 256, directory = 512;
constexpr int lock_exclusive = 1, lock_unlock = 2;
constexpr unsigned regular_type = 0100000, directory_type = 0040000;
constexpr unsigned symlink_type = 0120000, fifo_type = 0010000, type_mask = 0170000;
constexpr std::size_t limit = 1024 * 1024, tail_limit = 512 * 1024;
constexpr const char* base = "/private/sumhp.log";
using ModelOffset = std::int64_t;
using ModelPid = int;
struct ModelStat {
    std::uint64_t st_dev = 1, st_ino = 0, st_nlink = 1;
    std::int64_t st_size = 0;
    unsigned st_mode = 0600 | regular_type;
    timespec st_mtim{}, st_ctim{};
};
struct Node {
    std::string data;
    ModelStat metadata;
};
struct Handle {
    std::shared_ptr<Node> node;
    std::string path;
    std::size_t offset = 0;
    int flags = 0;
};
struct Fault {
    std::string operation, path;
    unsigned skip = 0;
    int error = EIO;
    bool zero = false;
};
std::map<std::string, std::shared_ptr<Node>> nodes;
std::map<int, Handle> handles;
std::vector<std::string> operations;
std::optional<Fault> fault;
std::uint64_t next_inode = 1, tick = 1, boot_serial = 0;
int next_fd = 10;
std::size_t max_read = std::numeric_limits<std::size_t>::max();
std::size_t max_write = std::numeric_limits<std::size_t>::max();
std::size_t largest_read_request = 0, maximum_stage_size = 0;
std::string boot;
bool fail_directory = false;
bool fault_zero = false;
unsigned fallback_messages = 0;

template <class Character>
std::string path_of(const Character* path) {
    return std::filesystem::path(path).generic_string();
}
void touch(Node& node) {
    node.metadata.st_mtim = {static_cast<time_t>(++tick), 0};
    node.metadata.st_ctim = node.metadata.st_mtim;
}
std::shared_ptr<Node> put(const std::string& path, const std::string& bytes = {},
                          unsigned type = regular_type, unsigned links = 1) {
    auto node = std::make_shared<Node>();
    node->data = bytes;
    node->metadata.st_mode = type | 0600;
    node->metadata.st_nlink = links;
    node->metadata.st_ino = next_inode++;
    touch(*node);
    nodes[path] = node;
    return node;
}
void reset() {
    CHECK(handles.empty());
    nodes.clear();
    operations.clear();
    fault.reset();
    max_read = max_write = std::numeric_limits<std::size_t>::max();
    largest_read_request = maximum_stage_size = 0;
    fail_directory = fault_zero = false;
    fallback_messages = 0;
    boot = "test-boot-" + std::to_string(++boot_serial);
    put("/private", {}, directory_type);
    sumhp::logging::set_debug_enabled(false);
}
bool fails(const std::string& operation, const std::string& path) {
    operations.push_back(operation + ":" + path);
    fault_zero = false;
    if (!fault || fault->operation != operation || (!fault->path.empty() && fault->path != path))
        return false;
    if (fault->skip) {
        --fault->skip;
        return false;
    }
    errno = fault->error;
    fault_zero = fault->zero;
    fault.reset();
    return true;
}
void inject(std::string operation, std::string path = {}, int error = EIO, unsigned skip = 0,
            bool zero = false) {
    fault = Fault{std::move(operation), std::move(path), skip, error, zero};
}
template <class Character>
int open(const Character* raw, int flags, unsigned mode = 0600) {
    const auto path = path_of(raw);
    CHECK((flags & (nofollow | cloexec)) == (nofollow | cloexec));
    if (path != "/private")
        CHECK((flags & nonblock) != 0);
    if (fails("open", path))
        return -1;
    auto found = nodes.find(path);
    if (found == nodes.end()) {
        if (!(flags & create)) {
            errno = ENOENT;
            return -1;
        }
        found = nodes.emplace(path, put(path)).first;
        found->second->metadata.st_mode = regular_type | mode;
    } else if ((flags & create) && (flags & exclusive)) {
        errno = EEXIST;
        return -1;
    }
    auto node = found->second;
    if ((node->metadata.st_mode & type_mask) == symlink_type && (flags & nofollow)) {
        errno = ELOOP;
        return -1;
    }
    if ((flags & directory) && (node->metadata.st_mode & type_mask) != directory_type) {
        errno = ENOTDIR;
        return -1;
    }
    if (flags & truncate) {
        node->data.clear();
        touch(*node);
    }
    const auto fd = next_fd++;
    handles.emplace(fd, Handle{node, path, 0, flags});
    return fd;
}
int close(int fd) {
    const auto found = handles.find(fd);
    CHECK(found != handles.end());
    const auto path = found->second.path;
    handles.erase(found);  // A failing close still consumes the descriptor.
    return fails("close", path) ? -1 : 0;
}
int fstat(int fd, ModelStat* output) {
    CHECK(handles.count(fd) == 1);
    auto& handle = handles.at(fd);
    if (fails("fstat", handle.path))
        return -1;
    *output = handle.node->metadata;
    output->st_size = static_cast<std::int64_t>(handle.node->data.size());
    return 0;
}
template <class Character>
int lstat(const Character* raw, ModelStat* output) {
    const auto path = path_of(raw);
    if (fails("lstat", path))
        return -1;
    const auto found = nodes.find(path);
    if (found == nodes.end()) {
        errno = ENOENT;
        return -1;
    }
    *output = found->second->metadata;
    output->st_size = static_cast<std::int64_t>(found->second->data.size());
    return 0;
}
int fchmod(int fd, unsigned mode) {
    CHECK(handles.count(fd) == 1);
    auto& handle = handles.at(fd);
    if (fails("fchmod", handle.path))
        return -1;
    handle.node->metadata.st_mode = (handle.node->metadata.st_mode & type_mask) | mode;
    return 0;
}
int flock(int fd, int operation) {
    CHECK(handles.count(fd) == 1);
    CHECK(operation == lock_exclusive || operation == lock_unlock);
    return fails("flock", handles.at(fd).path) ? -1 : 0;
}
std::ptrdiff_t pread(int fd, void* bytes, std::size_t length, std::int64_t offset) {
    CHECK(handles.count(fd) == 1 && offset >= 0);
    auto& handle = handles.at(fd);
    largest_read_request = std::max(largest_read_request, length);
    if (fails("read", handle.path))
        return -1;
    const auto position = static_cast<std::size_t>(offset);
    if (position >= handle.node->data.size())
        return 0;
    const auto count = std::min({length, max_read, handle.node->data.size() - position});
    std::memcpy(bytes, handle.node->data.data() + position, count);
    return static_cast<std::ptrdiff_t>(count);
}
std::ptrdiff_t read(int fd, void* bytes, std::size_t length) {
    CHECK(handles.count(fd) == 1);
    auto& handle = handles.at(fd);
    const auto count = pread(fd, bytes, length, static_cast<std::int64_t>(handle.offset));
    if (count > 0)
        handle.offset += static_cast<std::size_t>(count);
    return count;
}
std::int64_t lseek(int fd, std::int64_t offset, int whence) {
    CHECK(handles.count(fd) == 1);
    auto& handle = handles.at(fd);
    if (fails("lseek", handle.path))
        return -1;
    std::int64_t position = offset;
    if (whence == SEEK_CUR)
        position += static_cast<std::int64_t>(handle.offset);
    if (whence == SEEK_END)
        position += static_cast<std::int64_t>(handle.node->data.size());
    if (position < 0) {
        errno = EINVAL;
        return -1;
    }
    handle.offset = static_cast<std::size_t>(position);
    return position;
}
std::ptrdiff_t write(int fd, const void* bytes, std::size_t length) {
    CHECK(handles.count(fd) == 1);
    auto& handle = handles.at(fd);
    if (fails("write", handle.path))
        return fault_zero ? 0 : -1;
    CHECK((handle.flags & (write_only | read_write)) != 0);
    const auto count = std::min(length, max_write);
    if (handle.flags & append)
        handle.offset = handle.node->data.size();
    if (handle.node->data.size() < handle.offset + count)
        handle.node->data.resize(handle.offset + count);
    std::memcpy(handle.node->data.data() + handle.offset, bytes, count);
    handle.offset += count;
    touch(*handle.node);
    if (handle.path == std::string(base) + ".stage") {
        maximum_stage_size = std::max(maximum_stage_size, handle.node->data.size());
        CHECK(handle.node->data.size() <= limit);
    }
    return static_cast<std::ptrdiff_t>(count);
}
int ftruncate(int fd, std::int64_t size) {
    CHECK(handles.count(fd) == 1 && size >= 0);
    auto& handle = handles.at(fd);
    if (fails("ftruncate", handle.path))
        return -1;
    handle.node->data.resize(static_cast<std::size_t>(size));
    touch(*handle.node);
    return 0;
}
int fsync(int fd) {
    CHECK(handles.count(fd) == 1);
    return fails("fsync", handles.at(fd).path) ? -1 : 0;
}
template <class From, class To>
int rename(const From* from_raw, const To* to_raw) {
    const auto from = path_of(from_raw), to = path_of(to_raw);
    if (fails("rename", from + "->" + to))
        return -1;
    const auto found = nodes.find(from);
    if (found == nodes.end()) {
        errno = ENOENT;
        return -1;
    }
    const auto node = found->second;
    nodes.erase(found);
    nodes[to] = node;
    return 0;
}
template <class Character>
int unlink(const Character* raw) {
    const auto path = path_of(raw);
    if (fails("unlink", path))
        return -1;
    if (nodes.erase(path) == 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}
tm* localtime_r(const time_t* value, tm* result) {
#if defined(_WIN32)
    return localtime_s(result, value) == 0 ? result : nullptr;
#else
    return ::localtime_r(value, result);
#endif
}
int clock_gettime(int, timespec* value) {
    value->tv_sec = static_cast<time_t>(tick);
    value->tv_nsec = 0;
    return 0;
}
ModelPid getpid() {
    return 1234;
}
int fallback_fprintf(std::FILE*, const char*, ...) {
    ++fallback_messages;
    return 0;
}
int fallback_fputs(const char*, std::FILE*) {
    ++fallback_messages;
    return 0;
}
std::string contents(const std::string& path = base) {
    const auto found = nodes.find(path);
    return found == nodes.end() ? std::string{} : found->second->data;
}
void set_contents(const std::string& path, const std::string& value) {
    CHECK(nodes.count(path) == 1);
    nodes.at(path)->data = value;
    touch(*nodes.at(path));
}
void no_leaks() {
    CHECK(handles.empty());
}
std::string records(std::size_t length, char byte = 'x') {
    std::string data(length, byte);
    for (std::size_t index = 127; index < data.size(); index += 128)
        data[index] = '\n';
    if (!data.empty())
        data.back() = '\n';
    return data;
}
void ready() {
    reset();
    std::string error = "old failure";
    CHECK(sumhp::logging::prepare_boot_log(error));
    CHECK(error.empty());
    no_leaks();
}
}  // namespace logfs

#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
namespace sumhp {
std::filesystem::path runtime_log_file() {
    return logfs::base;
}
std::string runtime_boot_id() {
    return logfs::boot;
}
bool prepare_private_directory(const std::filesystem::path&, std::string& error) {
    if (logfs::fail_directory) {
        error = "injected directory failure";
        errno = EACCES;
        return false;
    }
    return true;
}
}  // namespace sumhp

// The generated include is the full production namespace, with syscall/type
// identifiers substituted. No storage decision is copied into this test.
#include "log_storage_under_test.inc"

namespace {
using sumhp::logging::Level;
void basic_and_bounds() {
    using namespace logfs;
    reset();
    const auto before = operations.size();
    errno = E2BIG;
    sumhp::logging::write(Level::Debug, "unit", "disabled");
    CHECK(operations.size() == before && nodes.count(base) == 0 && errno == E2BIG);
    ready();
    sumhp::logging::write(Level::Info, "unit", "ordinary");
    const auto record_length = contents().size();
    CHECK(record_length > 0 && contents().back() == '\n');
    CHECK((nodes.at(base)->metadata.st_mode & 0777) == 0600);
    for (const auto suffix : {".lock", ".boot"})
        CHECK((nodes.at(std::string(base) + suffix)->metadata.st_mode & 0777) == 0600);
    for (const int delta : {-1, 0, 1}) {
        set_contents(base, records(limit - record_length + delta));
        const auto original_inode = nodes.at(base)->metadata.st_ino;
        errno = E2BIG;
        sumhp::logging::write(Level::Info, "unit", "ordinary");
        CHECK(errno == E2BIG);
        CHECK(contents().size() <= limit);
        CHECK(contents().find("ordinary") != std::string::npos);
        CHECK((nodes.at(base)->metadata.st_ino != original_inode) == (delta == 1));
        no_leaks();
    }
    set_contents(base, {});
    sumhp::logging::set_debug_enabled(true);
    sumhp::logging::write(Level::Debug, "unit", "enabled");
    CHECK(contents().find("[DEBUG]") != std::string::npos);
    set_contents(base, {});
    sumhp::logging::write(Level::Info, std::string(20000, 'c'), std::string(100000, 'm'));
    const auto bounded = contents();
    CHECK(bounded.size() < 9000);
    CHECK(bounded.find("truncated") != std::string::npos);
    CHECK(std::count(bounded.begin(), bounded.end(), '\n') == 1);
    set_contents(base, {});
    sumhp::logging::write(Level::Info, "comp\r\n", "Unicode: 中文 😀\r\nnext\x01");
    const auto escaped = contents();
    CHECK(escaped.find("中文 😀") != std::string::npos);
    CHECK(escaped.find("\\r\\nnext?") != std::string::npos);
    CHECK(std::count(escaped.begin(), escaped.end(), '\n') == 1);
    no_leaks();
}

void retention_and_rotation() {
    using namespace logfs;
    ready();
    put(std::string(base) + ".old", "previous-boot\n");
    set_contents(base, records(3 * limit) + "newest-record\nunfinished");
    sumhp::logging::write(Level::Info, "unit", "after-trim");
    CHECK(contents().size() <= limit);
    CHECK(contents().find("newest-record\n") != std::string::npos);
    CHECK(contents().find("unfinished") == std::string::npos);
    CHECK(contents(std::string(base) + ".old") == "previous-boot\n");
    CHECK(largest_read_request <= tail_limit + 4096);
    CHECK(maximum_stage_size <= limit);
    CHECK(contents().find("[WARN] [logging]") != std::string::npos);
    CHECK(contents().find("storage limit") != std::string::npos);
    no_leaks();

    set_contents(base, std::string(2 * limit, 'x'));
    sumhp::logging::write(Level::Info, "unit", "after-long-line");
    CHECK(contents().size() < 1000);
    CHECK(contents().find("after-long-line") != std::string::npos);
    set_contents(std::string(base) + ".old", records(4 * limit) + "history-tail\n");
    sumhp::logging::write(Level::Info, "unit", "normal-write");
    CHECK(contents(std::string(base) + ".old").size() <= limit);
    CHECK(contents(std::string(base) + ".old").find("history-tail\n") != std::string::npos);

    set_contents(base, "boot-A-final\n");
    boot += "-next";
    std::string error;
    CHECK(sumhp::logging::prepare_boot_log(error));
    CHECK(contents().empty());
    CHECK(contents(std::string(base) + ".old") == "boot-A-final\n");
    set_contents(base, "boot-B-start\n");
    CHECK(sumhp::logging::prepare_boot_log(error));
    CHECK(contents() == "boot-B-start\n");
    CHECK(contents(std::string(base) + ".old") == "boot-A-final\n");
    no_leaks();

    // Restart at each persisted P phase: before rename, after rename, and
    // after creating the replacement current. P identifies the old inode.
    for (int phase = 0; phase < 3; ++phase) {
        ready();
        set_contents(base, "old-boot-preserve\n");
        const auto inode = nodes.at(base)->metadata.st_ino;
        boot += "-new";
        set_contents(std::string(base) + ".boot", boot + " P 1 " + std::to_string(inode) + "\n");
        if (phase >= 1) {
            nodes[std::string(base) + ".old"] = nodes.at(base);
            nodes.erase(base);
        }
        if (phase == 2)
            put(base, "new-boot-already-created\n");
        CHECK(sumhp::logging::prepare_boot_log(error));
        CHECK(contents(std::string(base) + ".old") == "old-boot-preserve\n");
        CHECK(contents() == (phase == 2 ? "new-boot-already-created\n" : ""));
        CHECK(contents(std::string(base) + ".boot").find(" R ") != std::string::npos);
        no_leaks();
    }
}

void unsafe_paths_and_failures() {
    using namespace logfs;
    for (const auto suffix : {"", ".old", ".lock", ".stage", ".boot"}) {
        for (const unsigned type : {symlink_type, directory_type, fifo_type, regular_type}) {
            ready();
            set_contents(base, "must-stay\n");
            const auto path = std::string(base) + suffix;
            put(path, "foreign-object", type, type == regular_type ? 2 : 1);
            const auto original = contents();
            sumhp::logging::write(Level::Info, "unit", "must-not-append");
            CHECK(contents() == original);
            CHECK(nodes.at(path)->data == "foreign-object");
            no_leaks();
        }
    }
    ready();
    put(std::string(base) + ".stage", "stale-private-stage");
    sumhp::logging::write(Level::Info, "unit", "after-stage-cleanup");
    CHECK(contents().find("after-stage-cleanup") != std::string::npos);
    CHECK(nodes.count(std::string(base) + ".stage") == 0);

    const auto stage = std::string(base) + ".stage";
    for (const auto& failure :
         std::vector<std::pair<std::string, std::string>>{{"open", stage},
                                                          {"fchmod", stage},
                                                          {"fstat", stage},
                                                          {"write", stage},
                                                          {"fsync", stage},
                                                          {"close", stage},
                                                          {"rename", stage + "->" + base}}) {
        ready();
        set_contents(base, records(limit));
        const auto original = contents();
        inject(failure.first, failure.second);
        sumhp::logging::write(Level::Info, "unit", "must-not-append");
        CHECK(!fault.has_value());  // The intended production seam was reached.
        CHECK(contents() == original);
        CHECK(nodes.count(stage) == 0 || contents(stage).size() <= limit);
        no_leaks();
    }
    for (const auto operation : {"fsync", "close"}) {
        ready();
        set_contents(base, records(limit));
        const auto original_inode = nodes.at(base)->metadata.st_ino;
        inject(operation, "/private");
        sumhp::logging::write(Level::Info, "unit", "uncertain-append-must-stop");
        CHECK(!fault.has_value());
        CHECK(nodes.at(base)->metadata.st_ino != original_inode);
        CHECK(contents().size() <= limit);
        CHECK(contents().find("uncertain-append-must-stop") == std::string::npos);
        no_leaks();
        sumhp::logging::write(Level::Info, "unit", "rechecked-next-call");
        CHECK(contents().find("rechecked-next-call") != std::string::npos);
        no_leaks();
    }

    for (const auto& failure :
         std::vector<std::pair<std::string, std::string>>{{"open", base},
                                                          {"fstat", base},
                                                          {"fchmod", base},
                                                          {"flock", std::string(base) + ".lock"},
                                                          {"fchmod", std::string(base) + ".lock"},
                                                          {"write", base}}) {
        ready();
        set_contents(base, "preserved\n");
        inject(failure.first, failure.second, ENOSPC);
        errno = ERANGE;
        sumhp::logging::write(Level::Info, "unit", "rejected");
        CHECK(!fault.has_value());
        CHECK(errno == ERANGE);
        CHECK(contents() == "preserved\n");
        no_leaks();
    }
    ready();
    fail_directory = true;
    sumhp::logging::write(Level::Info, "unit", "directory-rejected");
    CHECK(contents().empty());
    no_leaks();
}

void short_and_interrupted_io() {
    using namespace logfs;
    for (const auto operation : {"read", "write"}) {
        ready();
        set_contents(base, records(limit));
        max_read = 127;
        max_write = 113;
        inject(operation, operation == std::string("read") ? base : std::string(base) + ".stage",
               EINTR);
        sumhp::logging::write(Level::Info, "unit", "interrupted-then-complete");
        CHECK(!fault.has_value());
        CHECK(contents().size() <= limit);
        CHECK(contents().find("interrupted-then-complete") != std::string::npos);
        no_leaks();
    }
    ready();
    inject("write", base, EIO, 0, true);
    sumhp::logging::write(Level::Info, "unit", "zero-write");
    CHECK(!fault.has_value());
    CHECK(contents().empty());
    no_leaks();
    ready();
    max_write = 7;
    inject("write", base, ENOSPC, 1);
    sumhp::logging::write(Level::Info, "unit", "partial-write");
    CHECK(!fault.has_value() && contents().size() == 7);
    CHECK(contents().size() <= limit);
    no_leaks();
    const auto partial = contents();
    max_write = std::numeric_limits<std::size_t>::max();
    sumhp::logging::write(Level::Info, "unit", "after-partial-write");
    const auto recovered = contents();
    CHECK(recovered.substr(0, partial.size()) == partial);
    CHECK(recovered[partial.size()] == '\n');
    CHECK(recovered.back() == '\n' && recovered.size() <= limit);
    const auto complete_record = recovered.substr(partial.size() + 1);
    CHECK(complete_record.find(" [INFO] [unit] after-partial-write\n") != std::string::npos);
    CHECK(std::count(complete_record.begin(), complete_record.end(), '\n') == 1);
    no_leaks();
}

void encoded_utf8_boundaries() {
    using namespace logfs;
    ready();
    // Both fields exactly fill their encoded-byte budget, with the final
    // complete scalar occupying the last four bytes. No suffix is justified.
    const auto exact_component = std::string(252, 'c') + "😀";
    const auto exact_message = std::string(8188, 'm') + "😀";
    sumhp::logging::write(Level::Info, exact_component, exact_message);
    const auto exact = contents();
    CHECK(exact.find("[" + exact_component + "] " + exact_message + "\n") != std::string::npos);
    CHECK(exact.find("truncated") == std::string::npos);
    CHECK(std::count(exact.begin(), exact.end(), '\n') == 1);
    no_leaks();

    // Adding a truncation suffix would cut inside a four-byte scalar at
    // byte 242/8178. It must retreat to the leading byte, preserving UTF-8.
    set_contents(base, {});
    const auto long_component = std::string(241, 'c') + "😀" + std::string(20, 'z');
    const auto long_message = std::string(8177, 'm') + "😀" + std::string(20, 'z');
    sumhp::logging::write(Level::Info, long_component, long_message);
    const auto truncated = contents();
    const auto wanted_component = std::string(241, 'c') + "...[truncated]";
    const auto wanted_message = std::string(8177, 'm') + "...[truncated]";
    CHECK(wanted_component.size() == 255 && wanted_message.size() == 8191);
    CHECK(truncated.find("[" + wanted_component + "] " + wanted_message + "\n") !=
          std::string::npos);
    CHECK(truncated.find("😀") == std::string::npos);
    CHECK(std::count(truncated.begin(), truncated.end(), '\n') == 1);
    no_leaks();

    // Escaping also counts encoded bytes: 4096 LF bytes become exactly 8192
    // printable bytes while the complete record still has one LF delimiter.
    set_contents(base, {});
    sumhp::logging::write(Level::Info, "unit", std::string(4096, '\n'));
    const auto escaped = contents();
    const auto start = escaped.find(" [INFO] [unit] ");
    CHECK(start != std::string::npos);
    const auto body = escaped.substr(start + std::string(" [INFO] [unit] ").size());
    CHECK(body.size() == 8193 && body.back() == '\n');
    CHECK(body.find("truncated") == std::string::npos);
    for (std::size_t index = 0; index < 8192; index += 2)
        CHECK(body[index] == '\\' && body[index + 1] == 'n');
    no_leaks();
}

void marker_failures_and_repeat_reset() {
    using namespace logfs;
    // Reject ambiguous/future/corrupt boot markers without rotating their log.
    for (const auto marker : {"", "boot X 1 2\n", "boot R 1 2 unexpected\n"}) {
        ready();
        set_contents(base, "current-preserved\n");
        set_contents(std::string(base) + ".boot", marker);
        std::string error;
        errno = EDOM;
        CHECK(!sumhp::logging::prepare_boot_log(error));
        CHECK(!error.empty() && errno == EDOM);
        CHECK(contents() == "current-preserved\n");
        no_leaks();
    }
    for (const auto invalid_boot : {"", "contains space", "contains\nnewline"}) {
        ready();
        boot = invalid_boot;
        std::string error;
        CHECK(!sumhp::logging::prepare_boot_log(error));
        CHECK(!error.empty());
        no_leaks();
    }
    // A failed durable P write must leave current/history intact. A failed
    // creation after rename leaves P, and retry must preserve the old history.
    ready();
    set_contents(base, "old-before-rotation\n");
    put(std::string(base) + ".old", "older-history\n");
    boot += "-next";
    inject("rename", std::string(base) + ".stage->" + base + ".boot");
    std::string error;
    CHECK(!sumhp::logging::prepare_boot_log(error));
    CHECK(!fault.has_value());
    CHECK(contents() == "old-before-rotation\n");
    CHECK(contents(std::string(base) + ".old") == "older-history\n");
    no_leaks();
    // Existing current is opened once by validation; creation is the second.
    inject("open", base, EACCES, 1);
    CHECK(!sumhp::logging::prepare_boot_log(error));
    CHECK(!fault.has_value());
    CHECK(nodes.count(base) == 0);
    CHECK(contents(std::string(base) + ".old") == "old-before-rotation\n");
    CHECK(contents(std::string(base) + ".boot").find(" P ") != std::string::npos);
    no_leaks();
    CHECK(sumhp::logging::prepare_boot_log(error));
    CHECK(contents().empty());
    CHECK(contents(std::string(base) + ".old") == "old-before-rotation\n");
    no_leaks();

    // P commits successfully, rename/create complete, then R replacement
    // fails. Retry must identify the already-created current by its new inode.
    ready();
    set_contents(base, "history-before-failed-R\n");
    const auto previous_inode = nodes.at(base)->metadata.st_ino;
    boot += "-next";
    inject("rename", std::string(base) + ".stage->" + base + ".boot", EIO, 1);
    CHECK(!sumhp::logging::prepare_boot_log(error));
    CHECK(!fault.has_value());
    CHECK(nodes.count(base) == 1 && contents().empty());
    CHECK(nodes.at(base)->metadata.st_ino != previous_inode);
    CHECK(contents(std::string(base) + ".old") == "history-before-failed-R\n");
    CHECK(contents(std::string(base) + ".boot").find(" P ") != std::string::npos);
    no_leaks();
    set_contents(base, "new-boot-after-failed-R\n");
    CHECK(sumhp::logging::prepare_boot_log(error));
    CHECK(contents() == "new-boot-after-failed-R\n");
    CHECK(contents(std::string(base) + ".old") == "history-before-failed-R\n");
    CHECK(contents(std::string(base) + ".boot").find(" R ") != std::string::npos);
    no_leaks();

    // The real logger connects repeat-state invalidation to observed file
    // identity/size/time. A user clear must reveal the next repeated error.
    ready();
    sumhp::logging::write(Level::Warning, "unit", "repeat-me");
    const auto first = contents();
    sumhp::logging::write(Level::Warning, "unit", "repeat-me");
    CHECK(contents() == first);
    set_contents(base, {});
    sumhp::logging::write(Level::Warning, "unit", "repeat-me");
    CHECK(contents().find("repeat-me") != std::string::npos);
    const auto after_clear = contents();
    sumhp::logging::write(Level::Warning, "unit", "repeat-me");
    CHECK(contents() == after_clear);
    sumhp::logging::flush();
    CHECK(contents().size() > after_clear.size());
    no_leaks();
}
}  // namespace

int main() {
    basic_and_bounds();
    retention_and_rotation();
    unsafe_paths_and_failures();
    short_and_interrupted_io();
    encoded_utf8_boundaries();
    marker_failures_and_repeat_reset();
    std::cout << "Production logger portable filesystem model: " << logfs::checks
              << " checks passed (not Linux locking/crash execution).\n";
}
