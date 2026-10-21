// Actual production scanner, config validation and builtins; only POSIX boundary doubles.
#include "mount/partition_discovery.hpp"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>
#include "core/config_document.hpp"
#include "core/rule_inventory.hpp"
#include "sumhp/config.hpp"

constexpr int O_RDONLY = 1, O_DIRECTORY = 2, O_NOFOLLOW = 4, O_NONBLOCK = 8, O_CLOEXEC = 16,
              AT_SYMLINK_NOFOLLOW = 32;
namespace partition_model {
struct Time {
    std::int64_t tv_sec = 1, tv_nsec = 0;
};
struct Metadata {
    unsigned st_mode = 0040755;
    std::uint64_t st_dev = 8, st_ino = 1, st_nlink = 1;
    std::int64_t st_size = 0;
    Time st_mtim, st_ctim;
};
struct Node {
    Metadata metadata;
    std::string bytes, link;
};
struct Handle {
    std::string path;
    std::size_t offset = 0;
};
struct Entry {
    char d_name[256]{};
};
struct Directory {
    int fd;
    std::vector<std::string> names;
    std::size_t index = 0;
    Entry entry;
};
std::map<std::string, Node> nodes;
std::map<int, Handle> handles;
std::map<std::string, int> stat_fault, open_fault, read_fault, dir_fault;
std::string opened_change, after_change, link_change, close_fault;
int next_fd = 5, reads = 0, opens = 0, scenarios = 0;
bool is_dir(unsigned mode) {
    return (mode & 0170000) == 0040000;
}
bool is_reg(unsigned mode) {
    return (mode & 0170000) == 0100000;
}
bool is_lnk(unsigned mode) {
    return (mode & 0170000) == 0120000;
}
std::string join(const std::string& parent, const std::string& name) {
    assert(name == "." || (name.find('/') == std::string::npos && name != ".."));
    return name == "." ? parent : parent == "/" ? parent + name : parent + "/" + name;
}
Node& add(const std::string& path, unsigned mode = 0040755) {
    auto& node = nodes[path];
    node.metadata.st_mode = mode;
    node.metadata.st_ino = nodes.size() + 10;
    return node;
}
void file(const std::string& path, const std::string& value = {}) {
    add(path, 0100644).bytes = value;
}
void link(const std::string& path, const std::string& target) {
    add(path, 0120777).link = target;
}
int open(const char* raw, int flags) {
    const std::string path(raw);
    ++opens;
    assert(path == "/" || path == "/proc/self/mountinfo");
    assert(flags ==
           (O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | (path == "/" ? O_DIRECTORY : 0)));
    if (open_fault.count(path)) {
        errno = open_fault[path];
        return -1;
    }
    if (!nodes.count(path)) {
        errno = ENOENT;
        return -1;
    }
    const int fd = next_fd++;
    handles[fd] = {path, 0};
    return fd;
}
int openat(int parent, const char* raw, int flags) {
    ++opens;
    assert(flags == (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    const auto path = join(handles.at(parent).path, raw);
    if (open_fault.count(path)) {
        errno = open_fault[path];
        return -1;
    }
    if (!nodes.count(path)) {
        errno = ENOENT;
        return -1;
    }
    if (!is_dir(nodes.at(path).metadata.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    if (path == opened_change) {
        ++nodes[path].metadata.st_ino;
        opened_change.clear();
    }
    const int fd = next_fd++;
    handles[fd] = {path, 0};
    return fd;
}
int fstatat(int parent, const char* name, Metadata* out, int flags) {
    assert(flags == AT_SYMLINK_NOFOLLOW);
    const auto path = join(handles.at(parent).path, name);
    if (stat_fault.count(path)) {
        errno = stat_fault[path];
        return -1;
    }
    if (!nodes.count(path)) {
        errno = ENOENT;
        return -1;
    }
    *out = nodes.at(path).metadata;
    return 0;
}
int fstat(int fd, Metadata* out) {
    const auto path = handles.at(fd).path;
    if (stat_fault.count(path + ":fd")) {
        errno = stat_fault[path + ":fd"];
        return -1;
    }
    *out = nodes.at(path).metadata;
    return 0;
}
int close(int fd) {
    assert(handles.erase(fd));
    return 0;
}
Directory* fdopendir(int fd) {
    auto* directory = new Directory{fd, {}, 0, {}};
    const auto path = handles.at(fd).path;
    for (const auto& [name, node] : nodes) {
        (void)node;
        const auto prefix = path == "/" ? path : path + "/";
        if (name.rfind(prefix, 0) == 0 && name.size() > prefix.size() &&
            name.find('/', prefix.size()) == std::string::npos)
            directory->names.push_back(name.substr(prefix.size()));
    }
    return directory;
}
Entry* readdir(Directory* directory) {
    const auto path = handles.at(directory->fd).path;
    if (directory->index == directory->names.size()) {
        if (dir_fault.count(path))
            errno = dir_fault[path];
        return nullptr;
    }
    const auto& name = directory->names.at(directory->index++);
    assert(name.size() < 256);
    std::memcpy(directory->entry.d_name, name.c_str(), name.size() + 1);
    return &directory->entry;
}
int closedir(Directory* directory) {
    const auto path = handles.at(directory->fd).path;
    close(directory->fd);
    delete directory;
    if (path == after_change) {
        ++nodes[path].metadata.st_ctim.tv_nsec;
        after_change.clear();
    }
    if (path == close_fault) {
        errno = EIO;
        return -1;
    }
    return 0;
}
int readlinkat(int parent, const char* name, char* target, std::size_t size) {
    const auto path = join(handles.at(parent).path, name);
    if (read_fault.count(path)) {
        errno = read_fault[path];
        return -1;
    }
    const auto& bytes = nodes.at(path).link;
    const auto count = std::min(bytes.size(), size);
    std::memcpy(target, bytes.data(), count);
    if (path == link_change) {
        ++nodes[path].metadata.st_ino;
        link_change.clear();
    }
    return static_cast<int>(count);
}
int read(int fd, char* target, std::size_t size) {
    ++reads;
    auto& handle = handles.at(fd);
    assert(handle.path == "/proc/self/mountinfo");
    if (read_fault.count(handle.path)) {
        errno = read_fault[handle.path];
        return -1;
    }
    const auto& bytes = nodes.at(handle.path).bytes;
    const auto count = std::min(bytes.size() - handle.offset, size);
    std::memcpy(target, bytes.data() + handle.offset, count);
    handle.offset += count;
    return static_cast<int>(count);
}
void reset() {
    assert(handles.empty());
    nodes.clear();
    stat_fault.clear();
    open_fault.clear();
    read_fault.clear();
    dir_fault.clear();
    opened_change.clear();
    after_change.clear();
    link_change.clear();
    close_fault.clear();
    reads = opens = 0;
    add("/");
    add("/system");
    add("/modules");
    file("/proc/self/mountinfo", "1 0 8:1 / / ro - ext4 /dev/root ro\n");
}
void module(const std::string& id = "example") {
    add("/modules/" + id);
    file("/modules/" + id + "/module.prop", "id=" + id);
}
void partition(const std::string& name = "custom") {
    add("/" + name);
    nodes.at("/proc/self/mountinfo").bytes +=
        "2 1 8:2 / /" + name + " ro - erofs /dev/block/dm-9 ro\n";
}
void source(const std::string& name = "custom", const std::string& id = "example",
            bool system = false) {
    if (system)
        add("/modules/" + id + "/system");
    const auto root = "/modules/" + id + (system ? "/system/" : "/") + name;
    add(root);
    file(root + "/payload");
}
void setup() {
    reset();
    module();
    partition();
    source();
}
}  // namespace partition_model
namespace sumhp {
std::filesystem::path runtime_modules_dir() {
    return "/modules";
}
#include "partition_config_under_test.inc"
namespace mount::fsutil {
#include "partition_builtins_under_test.inc"
}
}  // namespace sumhp
#include "partition_discovery_under_test.inc"

int main() {
    using namespace partition_model;
    using namespace sumhp;
    using namespace sumhp::mount;
    PartitionDiscovery result;
    std::string error;
    const auto run = [&](bool expected,
                         const std::vector<std::string>& saved = std::vector<std::string>{}) {
        assert(discover_extra_partitions(saved, result, error) == expected);
        assert(result.complete == expected);
        assert(result.output.o.at("complete").b == expected);
        assert(handles.empty());
        assert(result.observation_signature.size() <= 128 * 1024);
        assert(stringify_json(result.output).size() <= 320 * 1024);
        if (!expected)
            assert(!error.empty());
        ++scenarios;
    };
    const auto candidate = [&](const std::string& name = "custom") -> const JsonValue& {
        for (const auto& value : result.output.o.at("candidates").a)
            if (value.o.at("name").s == name)
                return value;
        assert(false);
        return result.output;
    };
    const auto applicable = [&](bool value) { assert(candidate().o.at("applicable").b == value); };
    reset();
    run(true);
    assert(result.output.o.at("candidates").a.empty());
    reset();
    nodes.erase("/modules");
    run(false);
    assert(result.output.o.at("status").s == "unknown");
    reset();
    link("/modules", "/outside");
    run(false);
    reset();
    open_fault["/modules"] = EACCES;
    run(false);
    setup();
    run(true, {"custom", "gone"});
    applicable(true);
    assert(candidate().o.at("saved").b);
    assert(result.output.o.at("saved_partitions").a.size() == 2);
    const auto original = result.observation_signature;
    run(true);
    assert(result.observation_signature == original);
    ++nodes["/modules/example/custom"].metadata.st_ctim.tv_nsec;
    run(true);
    assert(result.observation_signature != original);
    setup();
    file("/modules/example/disable");
    file("/modules/example/remove");
    file("/modules/example/skip_mount");
    run(true);
    applicable(true);
    assert(candidate().o.at("sources").a.at(0).o.at("disabled").b &&
           candidate().o.at("sources").a.at(0).o.at("remove_pending").b &&
           candidate().o.at("sources").a.at(0).o.at("skip_mount").b);
    module("aaa");
    source("custom", "aaa");
    run(true);
    assert(candidate().o.at("sources").a.size() == 2);
    assert(candidate().o.at("sources").a.at(0).o.at("module_id").o.at("value").s == "aaa");
    for (const auto& alias : std::vector<std::string>{"/custom", "../custom", "./../custom"}) {
        setup();
        link("/system/custom", alias);
        run(true);
        applicable(true);
    }
    for (const auto& alias :
         std::vector<std::string>{"/outside", "/outside/../custom", "other/../custom"}) {
        setup();
        link("/system/custom", alias);
        run(true);
        applicable(false);
    }
    setup();
    add("/system/custom");
    run(true);
    applicable(false);
    setup();
    add("/system/custom").metadata = nodes["/custom"].metadata;
    run(true);
    applicable(true);
    setup();
    read_fault["/system/custom"] = EIO;
    link("/system/custom", "/custom");
    run(false);
    applicable(false);
    setup();
    link("/system/custom", "/custom");
    link_change = "/system/custom";
    run(false);
    applicable(false);
    setup();
    nodes.erase("/custom");
    run(true);
    applicable(false);
    setup();
    link("/custom", "/outside");
    run(true);
    applicable(false);
    setup();
    file("/custom");
    run(true);
    applicable(false);
    setup();
    stat_fault["/custom"] = EACCES;
    run(false);
    applicable(false);
    setup();
    nodes["/proc/self/mountinfo"].bytes = "1 0 8:1 / / ro - ext4 /dev/root ro\n";
    run(true);
    applicable(false);
    setup();
    nodes["/proc/self/mountinfo"].bytes += "3 1 8:3 / /custom rw - tmpfs tmpfs rw\n";
    run(true);
    applicable(false);
    assert(candidate().o.at("status").s == "unknown");
    setup();
    nodes["/proc/self/mountinfo"].bytes = "1 0 8:1 / /custom ro - overlay overlay ro\n";
    run(true);
    applicable(false);
    setup();
    nodes["/proc/self/mountinfo"].bytes = "1 0 8:1 /sub /custom ro - ext4 /dev/root ro\n";
    run(true);
    applicable(false);
    setup();
    nodes["/proc/self/mountinfo"].bytes = "bad record\n";
    run(false);
    applicable(false);
    setup();
    nodes["/proc/self/mountinfo"].bytes = "1 2 3 4 5 6 7 8 9 10 11\n";
    run(false);
    applicable(false);
    setup();
    nodes["/proc/self/mountinfo"].bytes = "1 2 3 4 5 6 7 8 9 -\n";
    run(false);
    applicable(false);
    setup();
    nodes["/proc/self/mountinfo"].bytes.assign(256 * 1024 + 1, 'x');
    run(false);
    applicable(false);
    setup();
    open_fault["/proc/self/mountinfo"] = EACCES;
    run(false);
    applicable(false);
    setup();
    read_fault["/proc/self/mountinfo"] = EIO;
    run(false);
    applicable(false);
    setup();
    source("分区");
    source("bad name");
    source("with\tcontrol");
    run(true);
    assert(result.output.o.at("candidates").a.size() == 1);
    for (const auto& name : std::vector<std::string>{"system_ext", "vendor", "META-INF", "common",
                                                     "data", "dev", "proc", "etc", "apex"}) {
        setup();
        source(name);
        partition(name);
        run(true);
        assert(result.output.o.at("candidates").a.size() == 1);
    }
    setup();
    nodes["/modules/example/module.prop"].metadata.st_size = std::int64_t{1} << 60;
    run(true);
    applicable(true);
    setup();
    link("/modules/example/module.prop", "/outside");
    run(false);
    assert(result.output.o.at("candidates").a.empty());
    setup();
    add("/modules/example/module.prop");
    run(false);
    setup();
    add("/modules/example/module.prop", 0010600);
    run(false);
    setup();
    link("/modules/external", "/outside");
    run(false);
    applicable(true);
    setup();
    link("/modules/example/system", "/outside");
    run(false);
    setup();
    nodes.erase("/modules/example/custom/payload");
    run(true);
    applicable(false);
    setup();
    nodes.erase("/modules/example/custom/payload");
    link("/modules/example/custom", "/outside");
    run(true);
    applicable(false);
    assert(candidate().o.at("reason").s == "unsafe_source");
    setup();
    nodes.erase("/modules/example/custom/payload");
    source("custom", "example", true);
    link("/modules/example/custom", "./system/custom");
    run(true);
    applicable(true);
    setup();
    add("/modules/example/system");
    link("/modules/example/system/custom", "../custom");
    run(true);
    applicable(true);
    setup();
    add("/modules/example/system");
    link("/modules/example/custom", "./system/custom");
    link("/modules/example/system/custom", "../custom");
    run(true);
    applicable(false);
    setup();
    open_fault["/modules/example/custom"] = EACCES;
    run(false);
    applicable(false);
    setup();
    dir_fault["/modules/example"] = EIO;
    run(false);
    setup();
    close_fault = "/modules/example/custom";
    run(false);
    applicable(false);
    setup();
    opened_change = "/modules/example/custom";
    run(false);
    applicable(false);
    setup();
    after_change = "/modules/example/custom";
    run(false);
    applicable(false);
    setup();
    stat_fault["/modules/example/disable"] = EIO;
    run(false);
    setup();
    stat_fault["/modules/example/module.prop"] = EIO;
    run(false);
    setup();
    stat_fault["/modules:fd"] = EIO;
    run(false);
    setup();
    for (int i = 0; i < 513; ++i)
        file("/modules/ignored" + std::to_string(i));
    run(false);
    reset();
    for (int i = 0; i < 257; ++i)
        module("m" + std::to_string(i));
    run(false);
    setup();
    for (int i = 0; i < 129; ++i)
        source("extra" + std::to_string(i));
    run(false);
    assert(!result.output.o.at("candidates").a.empty());
    setup();
    run(false, std::vector<std::string>(129, "custom"));
    assert(!result.output.o.at("saved_partitions_complete").b);
    setup();
    run(false, {std::string(100000, 'x')});
    setup();
    const std::string raw("invalid\xff", 8);
    module(raw);
    source("custom", raw);
    run(true);
    assert(candidate().o.at("sources").a.at(1).o.at("module_id").o.at("encoding").s == "hex");
    std::cout << scenarios << " production partition discovery scenarios passed\n";
}
