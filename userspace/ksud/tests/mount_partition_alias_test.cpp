// Compile the real planner/collector against real temporary directories. Only
// symlinks, directory handles, stat and opaque metadata use portable adapters.
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace realfs = std::filesystem;
namespace fixture {
using realfs::file_status;
using realfs::file_type;
using realfs::path;
bool is_symlink(file_status status) {
    return realfs::is_symlink(status);
}
bool is_directory(file_status status) {
    return realfs::is_directory(status);
}
std::map<path, path> links;
std::set<path> device_directories;
path key(const path& value) {
    return value.lexically_normal();
}
path read_symlink(const path& value, std::error_code& ec) {
    const auto it = links.find(key(value));
    ec = it == links.end() ? std::make_error_code(std::errc::invalid_argument) : std::error_code{};
    return it == links.end() ? path{} : it->second;
}
file_status symlink_status(const path& value, std::error_code& ec) {
    ec.clear();
    if (device_directories.count(key(value)))
        return file_status(file_type::directory);
    return links.count(key(value)) ? file_status(file_type::symlink)
                                   : realfs::symlink_status(value, ec);
}
bool is_symlink(const path& value, std::error_code& ec) {
    return fixture::is_symlink(fixture::symlink_status(value, ec));
}
path resolve(const path& value) {
    path result;
    for (const auto& component : value) {
        result /= component;
        const auto it = links.find(key(result));
        if (it != links.end())
            result = (result.parent_path() / it->second).lexically_normal();
    }
    return result;
}
bool is_directory(const path& value, std::error_code& ec) {
    if (device_directories.count(resolve(value))) {
        ec.clear();
        return true;
    }
    return realfs::is_directory(resolve(value), ec);
}
struct entry {
    realfs::path value;
    realfs::path path() const { return value; }
    file_status symlink_status(std::error_code& ec) const {
        return fixture::symlink_status(value, ec);
    }
};
struct directory_iterator {
    std::vector<entry> entries;
    std::size_t index = 0;
    directory_iterator() = default;
    directory_iterator(const path& value, std::error_code& ec) {
        for (realfs::directory_iterator it(resolve(value), ec), end; it != end && !ec;
             it.increment(ec))
            entries.push_back({value / it->path().filename()});
    }
    const entry* operator->() const { return &entries[index]; }
    const entry& operator*() const { return entries[index]; }
    void increment(std::error_code& ec) {
        ec.clear();
        ++index;
    }
    bool operator!=(const directory_iterator&) const { return index < entries.size(); }
};
void link(const path& value, const path& destination) {
    realfs::create_directories(value.parent_path());
    std::ofstream(value.string()).close();
    links[key(value)] = destination;
}
}  // namespace fixture
namespace fs = fixture;
namespace logging {
enum class Level { Error };
}
void mlog(const std::string&, logging::Level) {}
namespace fsutil {
#include "partition_alias_under_test.inc"
bool directory_is_opaque(const fs::path& path, bool& opaque) {
    opaque = realfs::exists(path / ".replace");
    return true;
}
std::string partition_mount_point(const std::string& part) {
    return "/" + part;
}
#include "partitions_under_test.inc"
}  // namespace fsutil

struct dirent {
    char d_name[512]{};
};
struct DIR {
    std::vector<std::string> names;
    std::size_t index = 0;
    dirent current;
};
DIR* opendir(const char* value) {
    std::error_code ec;
    if (!fs::is_directory(fs::path(value), ec)) {
        errno = ENOENT;
        return nullptr;
    }
    auto result = std::make_unique<DIR>();
    for (fs::directory_iterator it(fs::path(value), ec), end; it != end; it.increment(ec))
        result->names.push_back(it->path().filename().string());
    if (ec) {
        errno = EIO;
        return nullptr;
    }
    return result.release();
}
dirent* readdir(DIR* dir) {
    if (dir->index == dir->names.size())
        return nullptr;
    const auto& name = dir->names[dir->index++];
    assert(name.size() < sizeof(dir->current.d_name));
    std::strcpy(dir->current.d_name, name.c_str());
    return &dir->current;
}
int closedir(DIR* dir) {
    delete dir;
    return 0;
}
#define stat FixtureStat
struct stat {
    fs::file_type st_mode;
};
#ifdef S_ISLNK
#undef S_ISLNK
#endif
#define S_ISLNK(mode) ((mode) == fs::file_type::symlink)
#ifdef S_ISDIR
#undef S_ISDIR
#endif
#define S_ISDIR(mode) ((mode) == fs::file_type::directory)
int lstat(const char* value, struct stat* st) {
    std::error_code ec;
    st->st_mode = fs::symlink_status(fs::path(value), ec).type();
    if (ec) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}
enum class NType { Regular, Directory, Symlink, Whiteout };
struct Node {
    std::string name;
    NType type = NType::Directory;
    std::map<std::string, Node> children;
    std::string module_path;
    bool replace = false;
};
struct ModuleEntry {
    std::string id;
    realfs::path path;
};
struct Config {
    std::vector<std::string> partitions;
};
#include "backend_alias_under_test.inc"
NType type_from_lstat(const std::string&, const struct stat& st) {
    return st.st_mode == fs::file_type::symlink     ? NType::Symlink
           : st.st_mode == fs::file_type::directory ? NType::Directory
                                                    : NType::Regular;
}
std::string join(const std::string& dir, const std::string& name) {
    return (fs::path(dir) / name).string();
}
[[noreturn]] void scan_error(const std::string& message) {
    throw std::runtime_error(message);
}
bool dir_is_replace(const std::string& path) {
    return realfs::exists(fs::path(path) / ".replace");
}
#include "magic_alias_under_test.inc"
void add_leaf(std::map<std::string, std::vector<std::string>>& ops, const std::string& target,
              const std::string& layer) {
    ops[target].push_back(layer);
}
#include "overlay_alias_under_test.inc"

int main() {
    const auto root = realfs::temp_directory_path() /
                      ("sumhp-alias-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto module = root / "module";
    const std::vector<std::string> parts{"system", "vendor", "product", "system_ext", "odm"};
    realfs::create_directories(module / "system/app");
    for (const auto& part : parts) {
        if (part == "system")
            continue;
        realfs::create_directories(module / part / "etc");
        std::ofstream(module / part / "etc/config") << "installed";
        fixture::link(module / "system" / part, fs::path("..") / part);
        fixture::device_directories.insert(fs::path("/") / part);
        fixture::links[fs::path("/system") / part] = fs::path("/") / part;
        assert(fsutil::module_partition_alias(module / "system" / part) == module / part);
    }
    assert(validate_partition_tree(module / "system", parts, true));
    std::string scan_error_message;
    assert(!module_needs_magic({"module", module}, {}, scan_error_message));
    assert(scan_error_message.empty());
    const auto plan = plan_overlays(root.string(), {"module"}, parts);
    for (const auto& part : parts) {
        if (part == "system")
            continue;
        assert(plan.at("/" + part + "/etc").size() == 1);
        assert(fs::path(plan.at("/" + part + "/etc").front()) == module / part / "etc");
    }
    Node system;
    assert(collect_into(system, (module / "system").string(), &parts));
    for (const auto& part : parts) {
        if (part == "system")
            continue;
        const auto& node = system.children.at(part);
        assert(node.type == NType::Directory);
        assert(node.children.at("etc").children.at("config").type == NType::Regular);
        assert(fs::path(node.module_path) == module / part);
    }
    // Collection must still obey module priority when aliased trees overlap.
    const auto lower = root / "lower";
    realfs::create_directories(lower / "system/vendor/etc");
    std::ofstream(lower / "system/vendor/etc/config") << "lower priority";
    std::ofstream(lower / "system/vendor/etc/additional") << "extra";
    assert(collect_into(system, (lower / "system").string(), &parts));
    const auto& etc = system.children.at("vendor").children.at("etc");
    assert(fs::path(etc.children.at("config").module_path) == module / "vendor/etc/config");
    assert(etc.children.count("additional") == 1);
    const auto merged = collect_module_files({{"module", module}, {"lower", lower}}, {});
    assert(merged);
    for (const auto& part : parts) {
        if (part == "system")
            continue;
        assert(merged->children.count(part) == 1);
        const auto& partition = merged->children.at(part);
        assert(partition.type == NType::Directory && partition.module_path.empty());
        assert(partition.children.at("etc").children.count("config") == 1);
        assert(merged->children.at("system").children.count(part) == 0);
    }

    // Alias acceptance must not bypass replacement and partition-root checks.
    std::ofstream(module / "vendor/.replace").close();
    assert(!validate_partition_tree(module / "system", parts, true));
    realfs::remove(module / "vendor/.replace");
    std::ofstream(module / "vendor/direct-file").close();
    assert(!validate_partition_tree(module / "system", parts, true));
    assert(module_needs_magic({"module", module}, {}, scan_error_message));
    assert(scan_error_message.empty());
    realfs::remove(module / "vendor/direct-file");
    for (const auto& destination :
         {fs::path("../../lower/system/vendor"), fs::path("../missing"), fs::path("../product"),
          module / "vendor", fs::path("../system/../vendor")}) {
        fixture::links[fixture::key(module / "system/vendor")] = destination;
        assert(!fsutil::module_partition_alias(module / "system/vendor"));
        assert(module_needs_magic({"module", module}, {}, scan_error_message));
        assert(scan_error_message.empty());
        assert(!validate_partition_tree(module / "system", parts, true));
        bool rejected = false;
        try {
            (void)collect_module_files({{"module", module}}, {});
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected);
    }
    fixture::links[fixture::key(module / "system/vendor")] = "../vendor";
    realfs::rename(module / "vendor", module / "missing-vendor");
    assert(!fsutil::module_partition_alias(module / "system/vendor"));
    assert(!validate_partition_tree(module / "system", parts, true));
    realfs::rename(module / "missing-vendor", module / "vendor");
    fixture::links[fixture::key(module / "vendor")] = lower / "system/vendor";
    assert(!fsutil::module_partition_alias(module / "system/vendor"));
    assert(!validate_partition_tree(module / "system", parts, true));
    fixture::links.clear();
    realfs::remove_all(root);
    std::cout << "SUMHP partition alias regressions passed\n";
}
