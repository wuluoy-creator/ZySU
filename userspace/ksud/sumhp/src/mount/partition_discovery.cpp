#include "mount/partition_discovery.hpp"
#include "core/rule_inventory.hpp"
#include "core/runtime.hpp"
#include "mount/mount_fs.hpp"
#include "sumhp/config.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace sumhp::mount {
namespace {
constexpr std::size_t max_modules = 256, max_root_entries = 512, max_entries = 4096;
constexpr std::size_t max_candidates = 128, max_sources = 512, max_observations = 4096;
constexpr std::size_t max_observation_bytes = 128UL * 1024, max_mount_bytes = 256UL * 1024;
constexpr std::size_t max_json_bytes = 320UL * 1024, max_issues = 64;
class Fd {
    int value = -1;

public:
    Fd() = default;
    explicit Fd(int fd) : value(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value(other.value) { other.value = -1; }
    Fd& operator=(Fd&&) = delete;
    [[nodiscard]] int get() const { return value; }
    void swap(Fd& other) noexcept { std::swap(value, other.value); }
    ~Fd() {
        if (value >= 0)
            ::close(value);
    }
};
JsonValue exact(const std::string& bytes) {
    return json::parse(inventory::bytes_json(bytes));
}
std::string kind(const struct stat& st) {
    if (S_ISDIR(st.st_mode))
        return "directory";
    if (S_ISREG(st.st_mode))
        return "regular";
    if (S_ISLNK(st.st_mode))
        return "symlink";
    return "other";
}
std::string metadata(const struct stat& st) {
    return std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino) + ":" +
           std::to_string(st.st_mode) + ":" + std::to_string(st.st_nlink) + ":" +
           std::to_string(st.st_size) + ":" + std::to_string(st.st_mtim.tv_sec) + ":" +
           std::to_string(st.st_mtim.tv_nsec) + ":" + std::to_string(st.st_ctim.tv_sec) + ":" +
           std::to_string(st.st_ctim.tv_nsec);
}
bool same(const struct stat& a, const struct stat& b) {
    return metadata(a) == metadata(b);
}
struct MountFact {
    std::string root, filesystem, source;
};
struct Candidate {
    JsonValue sources = JsonValue::array();
    bool content = false, unsafe = false, unknown = false;
};
class Scan {
    friend void read_mounts(Scan& scan);
    friend void source(Scan& scan, int module_fd, int system_fd, const std::string& module_path,
                       const std::string& module_id, const std::string& name, bool system_layout,
                       bool disabled, bool remove, bool skip);
    friend void installed(Scan& scan, int modules_fd, const std::string& root_path,
                          const std::string& id);
    friend JsonValue finish(Scan& scan, const std::vector<std::string>& saved);
    friend bool ::sumhp::mount::discover_extra_partitions(const std::vector<std::string>& saved,
                                                          PartitionDiscovery& result,
                                                          std::string& error);
    bool complete = true, root_known = false, stopped = false;
    std::size_t entries = 0, modules = 0, sources = 0, observations = 0;
    std::size_t issue_bytes = 0;
    std::string signature, first_error;
    JsonValue issues = JsonValue::array();
    std::map<std::string, Candidate> candidates;
    std::map<std::string, std::vector<MountFact>> mounts;
    bool mounts_known = false;

public:
    void issue(const std::string& code, const std::string& path, int number,
               const std::string& detail, bool incomplete = true) {
        if (incomplete) {
            complete = false;
            if (first_error.empty())
                first_error = code + ": " + detail;
        }
        if (issues.a.size() >= max_issues)
            return;
        auto item = JsonValue::object();
        item.o["code"] = code;
        item.o["path"] = exact(path);
        item.o["errno"] = number;
        item.o["detail"] = exact(detail);
        const auto bytes = stringify_json(item).size();
        if (bytes > 32UL * 1024 - issue_bytes)
            return;
        issue_bytes += bytes;
        issues.a.push_back(std::move(item));
    }
    bool observe(const std::string& path, const std::string& fact) {
        if (stopped)
            return false;
        const auto value =
            std::to_string(path.size()) + ":" + path + std::to_string(fact.size()) + ":" + fact;
        if (observations >= max_observations ||
            value.size() > max_observation_bytes - signature.size()) {
            issue("observation_capacity", path, E2BIG, "metadata observation budget exhausted");
            stopped = true;
            return false;
        }
        signature += value;
        ++observations;
        return true;
    }
    bool stat_at(int parent, const std::string& entry_name, const std::string& observation_path,
                 struct stat& st, bool& missing) {
        missing = false;
        if (stopped)
            return false;
        if (::fstatat(parent, entry_name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            const int code = errno ? errno : EIO;
            observe(observation_path, "errno:" + std::to_string(code));
            if (code == ENOENT) {
                missing = true;
                return true;
            }
            issue("stat_failed", observation_path, code, std::strerror(code));
            return false;
        }
        return observe(observation_path, metadata(st));
    }
    Fd child(int parent, const std::string& entry_name, const std::string& observation_path,
             const struct stat& expected) {
        Fd result(::openat(parent, entry_name.c_str(),
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        if (result.get() < 0) {
            issue("directory_open_failed", observation_path, errno, std::strerror(errno));
            return {};
        }
        struct stat actual{};
        if (::fstat(result.get(), &actual) != 0) {
            issue("directory_stat_failed", observation_path, errno, std::strerror(errno));
            return {};
        }
        if (!same(expected, actual)) {
            issue("directory_changed", observation_path, ESTALE, "directory changed while opening");
            return {};
        }
        return result;
    }
    bool unchanged(int fd, const std::string& path, const struct stat& before) {
        struct stat after{};
        if (::fstat(fd, &after) != 0) {
            issue("directory_stat_failed", path, errno, std::strerror(errno));
            return false;
        }
        if (!same(before, after)) {
            issue("directory_changed", path, ESTALE, "directory changed while scanning");
            return false;
        }
        return true;
    }
    bool list(int fd, const std::string& path, std::vector<std::string>& names, std::size_t limit,
              bool only_first = false) {
        const int copied =
            ::openat(fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (copied < 0) {
            issue("directory_open_failed", path, errno, std::strerror(errno));
            return false;
        }
        DIR* directory = ::fdopendir(copied);
        if (!directory) {
            const int code = errno;
            ::close(copied);
            issue("directory_open_failed", path, code, std::strerror(code));
            return false;
        }
        bool ok = true;
        while (!stopped) {
            errno = 0;
            const auto* item = ::readdir(directory);
            if (!item) {
                if (errno) {
                    issue("directory_read_failed", path, errno, std::strerror(errno));
                    ok = false;
                }
                break;
            }
            const std::string name(item->d_name);
            if (name == "." || name == "..")
                continue;
            if (++entries > max_entries || names.size() >= limit) {
                issue("entry_capacity", path, E2BIG, "directory entry budget exhausted");
                stopped = true;
                ok = false;
                break;
            }
            names.push_back(name);
            if (only_first)
                break;
        }
        if (::closedir(directory) != 0) {
            issue("directory_close_failed", path, errno, std::strerror(errno));
            ok = false;
        }
        std::sort(names.begin(), names.end());
        if (!only_first)
            for (const auto& name : names)
                if (!observe(path, "entry:" + name))
                    return false;
        return ok && !stopped;
    }
    std::string link(int parent, const std::string& name, const std::string& path,
                     const struct stat& before, bool& ok) {
        char buffer[4096];
        const auto count = ::readlinkat(parent, name.c_str(), buffer, sizeof(buffer));
        ok = false;
        if (count < 0 || static_cast<std::size_t>(count) >= sizeof(buffer)) {
            const int code = count < 0 ? errno : ENAMETOOLONG;
            issue("link_read_failed", path, code, std::strerror(code));
            return {};
        }
        std::string value(buffer, static_cast<std::size_t>(count));
        struct stat after{};
        bool missing = false;
        if (!stat_at(parent, name, path, after, missing) || missing || !same(before, after)) {
            issue("link_changed", path, ESTALE, "link changed while reading");
            return {};
        }
        ok = observe(path, "link:" + value);
        return value;
    }
    Fd absolute(const std::string& path) {
        if (path.empty() || path.front() != '/' || path.size() >= 4096) {
            issue("invalid_root", path, EINVAL, "root must be a bounded absolute path");
            return {};
        }
        Fd current(::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        if (current.get() < 0) {
            issue("root_open_failed", "/", errno, std::strerror(errno));
            return {};
        }
        std::string traversed;
        std::istringstream parts(path);
        std::string part;
        while (std::getline(parts, part, '/')) {
            if (part.empty())
                continue;
            if (part == "." || part == "..") {
                issue("invalid_root", path, EINVAL, "relative root component");
                return {};
            }
            traversed += "/" + part;
            struct stat st{};
            bool missing = false;
            if (!stat_at(current.get(), part, traversed, st, missing))
                return {};
            if (missing || !S_ISDIR(st.st_mode)) {
                issue("unsafe_root", traversed, missing ? ENOENT : ENOTDIR,
                      "root component is not a real directory");
                return {};
            }
            auto next = child(current.get(), part, traversed, st);
            if (next.get() < 0)
                return {};
            current.swap(next);
        }
        return current;
    }
};
bool candidate_name(const std::string& name) {
    auto patch = JsonValue::object();
    patch.o["partitions"] = JsonValue::array();
    patch.o["partitions"].a.emplace_back(name);
    std::string error;
    return validate_config_patch(patch, error);
}
bool reserved(const std::string& name) {
    const auto& builtins = fsutil::managed_partitions();
    static const std::set<std::string> excluded = {
        "META-INF", "common",   "data",      "dev",     "proc",  "sys",  "mnt",  "storage",
        "sdcard",   "cache",    "metadata",  "persist", "tmp",   "root", "sbin", "debug_ramdisk",
        "acct",     "config",   "d",         "apex",    "bin",   "etc",  "lib",  "lib64",
        "app",      "priv-app", "framework", "fonts",   "media", "usr",  "xbin", "overlay"};
    return std::find(builtins.begin(), builtins.end(), name) != builtins.end() ||
           excluded.count(name);
}
bool unescape_mount(const std::string& text, std::string& result) {
    result.clear();
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\') {
            result += text[i];
            continue;
        }
        if (i + 3 >= text.size())
            return false;
        const auto escape = text.substr(i, 4);
        if (escape == "\\040")
            result += ' ';
        else if (escape == "\\011")
            result += '\t';
        else if (escape == "\\012")
            result += '\n';
        else if (escape == "\\134")
            result += '\\';
        else
            return false;
        i += 3;
    }
    return true;
}
void read_mounts(Scan& scan) {
    Fd const input(::open("/proc/self/mountinfo", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (input.get() < 0) {
        scan.issue("mountinfo_unreadable", "/proc/self/mountinfo", errno, std::strerror(errno));
        return;
    }
    std::string bytes;
    char buffer[4096];
    while (bytes.size() <= max_mount_bytes) {
        const auto count = ::read(input.get(), buffer,
                                  std::min(sizeof(buffer), max_mount_bytes + 1 - bytes.size()));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            scan.issue("mountinfo_unreadable", "/proc/self/mountinfo", errno, std::strerror(errno));
            return;
        }
        if (!count)
            break;
        bytes.append(buffer, static_cast<std::size_t>(count));
    }
    if (bytes.size() > max_mount_bytes) {
        scan.issue("mountinfo_capacity", "/proc/self/mountinfo", E2BIG,
                   "mountinfo byte budget exhausted");
        return;
    }
    std::istringstream lines(bytes);
    std::string line;
    std::size_t records = 0;
    while (std::getline(lines, line)) {
        if (++records > max_entries) {
            scan.issue("mountinfo_capacity", "/proc/self/mountinfo", E2BIG,
                       "mountinfo record budget exhausted");
            return;
        }
        std::istringstream fields(line);
        std::vector<std::string> values;
        std::string value;
        while (fields >> value)
            values.push_back(value);
        const auto separator = std::find(values.begin(), values.end(), "-");
        if (values.size() < 10 || separator == values.end() || separator < values.begin() + 6 ||
            std::distance(separator, values.end()) < 4) {
            scan.issue("mountinfo_invalid", "/proc/self/mountinfo", EINVAL,
                       "malformed mount record");
            return;
        }
        std::string path;
        MountFact fact;
        if (!unescape_mount(values[3], fact.root) || !unescape_mount(values[4], path) ||
            !unescape_mount(*(separator + 2), fact.source)) {
            scan.issue("mountinfo_invalid", "/proc/self/mountinfo", EINVAL,
                       "malformed mount path escape");
            return;
        }
        fact.filesystem = *(separator + 1);
        if (path.size() > 1 && path.front() == '/' && path.find('/', 1) == std::string::npos)
            scan.mounts[path].push_back(std::move(fact));
    }
    scan.mounts_known = true;
}
void source(Scan& scan, int module_fd, int system_fd, const std::string& module_path,
            const std::string& module_id, const std::string& name, bool system_layout,
            bool disabled, bool remove, bool skip) {
    if (scan.stopped || reserved(name))
        return;
    const auto path = module_path + (system_layout ? "/system/" : "/") + name;
    const int parent = system_layout ? system_fd : module_fd;
    struct stat st{};
    bool missing = false;
    if (!scan.stat_at(parent, name, path, st, missing) || missing) {
        if (missing)
            scan.issue("source_disappeared", path, ESTALE, "source disappeared during discovery");
        return;
    }
    if (!S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
        return;
    if (!candidate_name(name)) {
        scan.issue("invalid_name", path, EINVAL, "name is not allowed by partition configuration",
                   false);
        return;
    }
    if (!scan.candidates.count(name) && scan.candidates.size() >= max_candidates) {
        scan.issue("candidate_capacity", path, E2BIG, "candidate budget exhausted");
        scan.stopped = true;
        return;
    }
    if (scan.sources >= max_sources) {
        scan.issue("source_capacity", path, E2BIG, "source budget exhausted");
        scan.stopped = true;
        return;
    }
    auto& candidate = scan.candidates[name];
    ++scan.sources;
    auto row = JsonValue::object();
    row.o["module_id"] = exact(module_id);
    row.o["path"] = exact(path);
    row.o["layout"] = system_layout ? "system" : "top";
    row.o["disabled"] = disabled;
    row.o["remove_pending"] = remove;
    row.o["skip_mount"] = skip;
    std::string status = "empty";
    std::string reason = "no_direct_entries";
    int source_parent = parent;
    std::string source_path = path;
    struct stat actual = st;
    bool safe = true;
    if (S_ISLNK(st.st_mode)) {
        bool ok = false;
        const auto target = scan.link(parent, name, path, st, ok);
        if (!ok) {
            candidate.unknown = true;
            safe = false;
            status = "unknown";
            reason = "link_observation_failed";
        } else if ((!system_layout &&
                    (target == "./system/" + name || target == "system/" + name) &&
                    system_fd >= 0) ||
                   (system_layout && target == "../" + name)) {
            source_parent = system_layout ? module_fd : system_fd;
            source_path = module_path + (system_layout ? "/" : "/system/") + name;
            bool absent = false;
            if (!scan.stat_at(source_parent, name, source_path, actual, absent)) {
                candidate.unknown = true;
                safe = false;
                status = "unknown";
                reason = "alias_target_unreadable";
            } else if (absent || !S_ISDIR(actual.st_mode)) {
                candidate.unsafe = true;
                safe = false;
                status = "unsafe";
                reason = "alias_target_not_real_directory";
            }
        } else {
            candidate.unsafe = true;
            safe = false;
            status = "unsafe";
            reason = "source_link_not_known_in_module_alias";
        }
    }
    if (safe) {
        auto directory = scan.child(source_parent, name, source_path, actual);
        std::vector<std::string> entries;
        if (directory.get() < 0 || !scan.list(directory.get(), source_path, entries, 1, true) ||
            !scan.unchanged(directory.get(), source_path, actual)) {
            candidate.unknown = true;
            status = "unknown";
            reason = "source_read_failed";
        } else {
            scan.observe(source_path, entries.empty() ? "content:empty" : "content:nonempty");
            if (!entries.empty()) {
                candidate.content = true;
                status = S_ISLNK(st.st_mode) ? "alias" : "content";
                reason = "observed_direct_entries";
            }
        }
    }
    row.o["status"] = status;
    row.o["reason"] = reason;
    candidate.sources.a.push_back(std::move(row));
}
void installed(Scan& scan, int modules_fd, const std::string& root_path, const std::string& id) {
    const auto path = root_path + "/" + id;
    struct stat st{};
    bool missing = false;
    if (!scan.stat_at(modules_fd, id, path, st, missing))
        return;
    if (missing) {
        scan.issue("module_disappeared", path, ESTALE, "module disappeared during discovery");
        return;
    }
    if (S_ISLNK(st.st_mode)) {
        scan.issue("module_symlink_excluded", path, ELOOP, "module links are not traversed");
        return;
    }
    if (!S_ISDIR(st.st_mode))
        return;
    auto module = scan.child(modules_fd, id, path, st);
    if (module.get() < 0)
        return;
    struct stat prop{};
    bool absent = false;
    if (!scan.stat_at(module.get(), "module.prop", path + "/module.prop", prop, absent) || absent)
        return;
    if (!S_ISREG(prop.st_mode)) {
        scan.issue("module_prop_not_regular", path + "/module.prop", EINVAL,
                   "module.prop is not a real regular file");
        return;
    }
    if (++scan.modules > max_modules) {
        scan.issue("module_capacity", path, E2BIG, "installed module budget exhausted");
        scan.stopped = true;
        return;
    }
    bool disabled = false;
    bool remove = false;
    bool skip = false;
    const auto marker = [&](const char* name, bool& present) {
        struct stat value{};
        bool not_found = false;
        if (!scan.stat_at(module.get(), name, path + "/" + name, value, not_found))
            return false;
        present = !not_found;
        return true;
    };
    if (!marker("disable", disabled) || !marker("remove", remove) || !marker("skip_mount", skip))
        return;
    struct stat system_st{};
    bool no_system = false;
    Fd system;
    if (!scan.stat_at(module.get(), "system", path + "/system", system_st, no_system))
        return;
    if (!no_system && S_ISDIR(system_st.st_mode)) {
        auto opened = scan.child(module.get(), "system", path + "/system", system_st);
        system.swap(opened);
    } else if (!no_system)
        scan.issue("module_system_unsafe", path + "/system", ENOTDIR,
                   "module system is not a real directory");
    std::vector<std::string> names;
    if (!scan.list(module.get(), path, names, max_entries))
        return;
    for (const auto& name : names) {
        source(scan, module.get(), system.get(), path, id, name, false, disabled, remove, skip);
        if (scan.stopped)
            break;
    }
    if (system.get() >= 0 && !scan.stopped) {
        names.clear();
        if (scan.list(system.get(), path + "/system", names, max_entries))
            for (const auto& name : names) {
                source(scan, module.get(), system.get(), path, id, name, true, disabled, remove,
                       skip);
                if (scan.stopped)
                    break;
            }
        scan.unchanged(system.get(), path + "/system", system_st);
    }
    scan.unchanged(module.get(), path, st);
}
JsonValue target_fact(Scan& scan, int root, const std::string& name, const std::string& path,
                      struct stat& st, bool& known, bool& directory) {
    auto fact = JsonValue::object();
    fact.o["path"] = exact(path);
    fact.o["errno"] = 0;
    bool missing = false;
    known = root >= 0 && scan.stat_at(root, name, path, st, missing);
    directory = known && !missing && S_ISDIR(st.st_mode);
    fact.o["status"] = missing ? "missing" : kind(st);
    if (!known)
        fact.o["status"] = "unknown";
    if (!known)
        fact.o["errno"] = errno ? errno : EIO;
    return fact;
}
JsonValue finish(Scan& scan, const std::vector<std::string>& saved) {
    auto output = JsonValue::object();
    output.o["schema_version"] = 1;
    output.o["kind"] = "sumh.partitions.discovery";
    output.o["read_only"] = true;
    output.o["scope"] = "installed_module_candidates_not_mount_readiness";
    output.o["requires_reboot"] = true;
    output.o["saved_partitions"] = JsonValue::array();
    output.o["saved_partitions_complete"] = true;
    std::size_t saved_bytes = 0;
    for (const auto& name : saved) {
        if (output.o["saved_partitions"].a.size() >= max_candidates || name.size() > 255 ||
            name.size() > 40UL * 1024 - saved_bytes) {
            scan.issue("saved_capacity", "", E2BIG,
                       "saved partition export budget exhausted; original config unchanged");
            output.o["saved_partitions_complete"] = false;
            break;
        }
        if (!candidate_name(name)) {
            scan.issue("saved_name_invalid", "", EINVAL,
                       "saved partition name is not valid; original config unchanged");
            output.o["saved_partitions_complete"] = false;
            continue;
        }
        saved_bytes += name.size();
        output.o["saved_partitions"].a.emplace_back(name);
    }
    auto values = JsonValue::array();
    auto root = scan.absolute("/");
    auto system = scan.absolute("/system");
    for (auto& [name, candidate] : scan.candidates) {
        auto row = JsonValue::object();
        row.o["name"] = name;
        row.o["saved"] = std::find(saved.begin(), saved.end(), name) != saved.end();
        struct stat target{};
        struct stat alias{};
        bool target_known = false;
        bool target_directory = false;
        bool alias_known = false;
        bool alias_directory = false;
        row.o["target"] =
            target_fact(scan, root.get(), name, "/" + name, target, target_known, target_directory);
        auto alias_json = target_fact(scan, system.get(), name, "/system/" + name, alias,
                                      alias_known, alias_directory);
        alias_json.o["link_target"] = JsonValue{};
        bool alias_ok = alias_known && alias_json.o["status"].s == "missing";
        if (alias_known && S_ISLNK(alias.st_mode)) {
            bool ok = false;
            const auto text = scan.link(system.get(), name, "/system/" + name, alias, ok);
            alias_json.o["link_target"] = exact(text);
            alias_ok = ok && (text == "/" + name || text == "../" + name || text == "./../" + name);
            alias_json.o["status"] = alias_ok ? "target_alias" : "conflicting_link";
            if (!ok)
                alias_json.o["status"] = "unknown";
            if (!ok)
                alias_known = false;
        } else if (alias_directory) {
            alias_ok =
                target_directory && target.st_dev == alias.st_dev && target.st_ino == alias.st_ino;
            alias_json.o["status"] = alias_ok ? "same_directory" : "separate_directory";
        }
        row.o["alias"] = std::move(alias_json);
        auto mount = JsonValue::object();
        mount.o["status"] = "unknown";
        mount.o["filesystem"] = JsonValue{};
        mount.o["source"] = JsonValue{};
        bool mounted = false;
        const auto facts = scan.mounts.find("/" + name);
        if (scan.mounts_known) {
            if (facts == scan.mounts.end())
                mount.o["status"] = "not_mountpoint";
            else if (facts->second.size() != 1)
                mount.o["status"] = "stacked_unknown";
            else {
                const auto& fact = facts->second.front();
                static const std::set<std::string> filesystems = {"ext4", "erofs", "f2fs",
                                                                  "squashfs"};
                mounted = fact.root == "/" && filesystems.count(fact.filesystem) &&
                          fact.source.rfind("/dev/", 0) == 0;
                mount.o["status"] = mounted ? "block_filesystem" : "unverified_backing";
                mount.o["filesystem"] = fact.filesystem;
                mount.o["source"] = exact(fact.source);
                scan.observe("/" + name,
                             "mount:" + fact.root + ":" + fact.filesystem + ":" + fact.source);
            }
        }
        scan.observe("/" + name, "mount_status:" + mount.o["status"].s);
        row.o["mount"] = std::move(mount);
        const bool known = target_known && alias_known && scan.mounts_known && !candidate.unknown;
        const bool applicable = known && target_directory && alias_ok && mounted &&
                                candidate.content && !candidate.unsafe;
        row.o["applicable"] = applicable;
        row.o["status"] = "unavailable";
        if (applicable)
            row.o["status"] = "applicable";
        else if (!known || (target_directory && !mounted && facts != scan.mounts.end()))
            row.o["status"] = "unknown";
        row.o["reason"] = "partition_backing_unverified";
        if (applicable)
            row.o["reason"] = "observed_partition_and_module_content";
        else if (candidate.unknown)
            row.o["reason"] = "source_unknown";
        else if (candidate.unsafe)
            row.o["reason"] = "unsafe_source";
        else if (!candidate.content)
            row.o["reason"] = "source_empty";
        else if (!target_known)
            row.o["reason"] = "target_unknown";
        else if (!target_directory)
            row.o["reason"] = "target_not_real_directory";
        else if (!alias_known)
            row.o["reason"] = "alias_unknown";
        else if (!alias_ok)
            row.o["reason"] = "alias_conflict";
        row.o["sources"] = std::move(candidate.sources);
        values.a.push_back(std::move(row));
    }
    output.o["candidates"] = std::move(values);
    auto limits = JsonValue::object();
    limits.o["root_entries"] = static_cast<int>(max_root_entries);
    limits.o["modules"] = static_cast<int>(max_modules);
    limits.o["entries"] = static_cast<int>(max_entries);
    limits.o["candidates"] = static_cast<int>(max_candidates);
    limits.o["sources"] = static_cast<int>(max_sources);
    limits.o["observations"] = static_cast<int>(max_observations);
    limits.o["observation_bytes"] = static_cast<int>(max_observation_bytes);
    limits.o["mountinfo_bytes"] = static_cast<int>(max_mount_bytes);
    limits.o["response_bytes"] = static_cast<int>(PARTITION_DISCOVERY_RESPONSE_BYTES);
    output.o["limits"] = std::move(limits);
    auto counts = JsonValue::object();
    counts.o["entries"] = static_cast<int>(scan.entries);
    counts.o["modules"] = static_cast<int>(scan.modules);
    counts.o["sources"] = static_cast<int>(scan.sources);
    counts.o["observations"] = static_cast<int>(scan.observations);
    output.o["counts"] = std::move(counts);
    output.o["issues"] = scan.issues;
    output.o["complete"] = scan.complete;
    output.o["status"] = scan.root_known ? "partial" : "unknown";
    if (scan.complete)
        output.o["status"] = "complete";
    while (stringify_json(output).size() > max_json_bytes && !output.o["candidates"].a.empty()) {
        output.o["candidates"].a.pop_back();
        scan.complete = false;
        output.o["complete"] = false;
        output.o["status"] = "partial";
        output.o["reason"] = "response_capacity";
        if (scan.first_error.empty())
            scan.first_error = "response_capacity";
    }
    return output;
}
}  // namespace

bool discover_extra_partitions(const std::vector<std::string>& saved, PartitionDiscovery& result,
                               std::string& error) {
    Scan scan;
    const auto root_path = runtime_modules_dir().string();
    auto modules = scan.absolute(root_path);
    if (modules.get() >= 0) {
        scan.root_known = true;
        struct stat before{};
        if (::fstat(modules.get(), &before) != 0)
            scan.issue("root_stat_failed", root_path, errno, std::strerror(errno));
        else {
            std::vector<std::string> names;
            if (scan.list(modules.get(), root_path, names, max_root_entries))
                for (const auto& name : names) {
                    installed(scan, modules.get(), root_path, name);
                    if (scan.stopped)
                        break;
                }
            scan.unchanged(modules.get(), root_path, before);
        }
    }
    if (!scan.stopped)
        read_mounts(scan);
    result.output = finish(scan, saved);
    result.complete = scan.complete;
    result.observation_signature = std::move(scan.signature);
    error = std::move(scan.first_error);
    return result.complete;
}

}  // namespace sumhp::mount
