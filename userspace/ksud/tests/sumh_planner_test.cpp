// Exercise actual planner functions; only filesystem/transport boundaries are doubles.
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace realfs = std::filesystem;
namespace fixture {
std::map<std::string, std::string> links;
std::set<std::string> directories;
std::set<std::string> whiteouts, opaque, metadata_errors;
std::map<std::string, std::error_code> resolution_errors;
std::set<std::string> source_status_errors, stat_errors, readlink_errors, iterator_errors;
std::map<std::string, int> directory_calls, directory_error_after;
std::map<std::string, long long> metadata_revisions;
std::map<std::string, realfs::file_type> source_types, stat_types;
std::string increment_error;
std::vector<std::string> operations;
realfs::path root;
bool available = true;
int logs = 0;
std::string key(const realfs::path& path) {
    return path.generic_string();
}
bool virtual_path(const realfs::path& path) {
    const auto value = key(path);
    return value == "/" || value.rfind("/system", 0) == 0 || value.rfind("/vendor", 0) == 0 ||
           value.rfind("/product", 0) == 0 || value.rfind("/extra", 0) == 0;
}
std::string resolved(const realfs::path& path) {
    auto value = key(path);
    while (value.size() > 1 && value.back() == '/')
        value.pop_back();
    for (const auto& [from, to] : links) {
        if (!virtual_path(from))
            continue;
        if (value == from ||
            (value.size() > from.size() && value.compare(0, from.size(), from) == 0 &&
             value[from.size()] == '/'))
            return to + value.substr(from.size());
    }
    return value;
}
void file(const std::string& name) {
    const auto path = root / name;
    realfs::create_directories(path.parent_path());
    std::ofstream(path) << "module";
}
void reset() {
    realfs::remove_all(root);
    realfs::create_directories(root);
    links = {{"/system/vendor", "/vendor"}};
    directories = {"/",           "/system",         "/system/etc", "/vendor",
                   "/vendor/etc", "/vendor/etc/sub", "/extra",      "/extra/etc"};
    whiteouts.clear();
    opaque.clear();
    metadata_errors.clear();
    operations.clear();
    resolution_errors.clear();
    source_status_errors.clear();
    stat_errors.clear();
    readlink_errors.clear();
    iterator_errors.clear();
    directory_calls.clear();
    directory_error_after.clear();
    metadata_revisions.clear();
    source_types.clear();
    stat_types.clear();
    increment_error.clear();
    available = true;
    logs = 0;
}
}  // namespace fixture
namespace fs {
using realfs::file_status;
using realfs::file_type;
using realfs::path;
bool is_directory(file_status status) {
    return realfs::is_directory(status);
}
bool exists(file_status status) {
    return realfs::exists(status);
}
using realfs::is_symlink;
bool is_directory(const path& path, std::error_code& ec) {
    ec.clear();
    const auto name = fixture::key(path);
    const auto count = ++fixture::directory_calls[name];
    if (fixture::directory_error_after.count(name) &&
        count >= fixture::directory_error_after.at(name)) {
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }
    if (fixture::resolution_errors.count(fixture::key(path))) {
        ec = fixture::resolution_errors.at(fixture::key(path));
        return false;
    }
    if (fixture::virtual_path(path))
        return fixture::directories.count(fixture::resolved(path));
    return realfs::is_directory(path, ec);
}
bool exists(const path& path, std::error_code& ec) {
    ec.clear();
    if (fixture::resolution_errors.count(fixture::key(path))) {
        ec = fixture::resolution_errors.at(fixture::key(path));
        return false;
    }
    if (fixture::virtual_path(path))
        return fixture::directories.count(fixture::resolved(path));
    return realfs::exists(path, ec);
}
path canonical(const path& path, std::error_code& ec) {
    ec.clear();
    if (fixture::resolution_errors.count("canonical:" + fixture::key(path))) {
        ec = fixture::resolution_errors.at("canonical:" + fixture::key(path));
        return {};
    }
    if (fixture::virtual_path(path))
        return fixture::resolved(path);
    return realfs::canonical(path, ec);
}
file_status symlink_status(const path& path, std::error_code& ec) {
    ec.clear();
    if (fixture::source_status_errors.count(fixture::key(path))) {
        ec = std::make_error_code(std::errc::permission_denied);
        return file_status(file_type::none);
    }
    if (fixture::source_types.count(fixture::key(path)))
        return file_status(fixture::source_types.at(fixture::key(path)));
    if (fixture::links.count(fixture::key(path)))
        return file_status(file_type::symlink);
    if (fixture::virtual_path(path))
        return file_status(fixture::directories.count(fixture::key(path)) ? file_type::directory
                                                                          : file_type::not_found);
    return realfs::symlink_status(path, ec);
}
path read_symlink(const path& path, std::error_code& ec) {
    ec.clear();
    if (fixture::readlink_errors.count(fixture::key(path))) {
        ec = std::make_error_code(std::errc::io_error);
        return {};
    }
    const auto it = fixture::links.find(fixture::key(path));
    if (it != fixture::links.end())
        return it->second;
    ec = std::make_error_code(std::errc::invalid_argument);
    return {};
}
using realfs::create_directories;
using realfs::remove;
struct directory_entry {
    realfs::path value;
    realfs::path path() const { return value; }
    file_status symlink_status(std::error_code& ec) const { return fs::symlink_status(value, ec); }
    bool is_directory(std::error_code& ec) const { return fs::is_directory(value, ec); }
};
struct recursive_directory_iterator {
    realfs::recursive_directory_iterator value;
    mutable directory_entry entry;
    recursive_directory_iterator() = default;
    recursive_directory_iterator(const path& root, std::error_code& ec) : value(root, ec) {
        if (fixture::iterator_errors.count(fixture::key(root)))
            ec = std::make_error_code(std::errc::permission_denied);
    }
    const directory_entry* operator->() const {
        entry.value = value->path();
        return &entry;
    }
    bool operator!=(const recursive_directory_iterator& other) const {
        return value != other.value;
    }
    void increment(std::error_code& ec) {
        const bool inject = fixture::key(value->path()) == fixture::increment_error;
        value.increment(ec);
        if (inject)
            ec = std::make_error_code(std::errc::io_error);
    }
    void disable_recursion_pending() { value.disable_recursion_pending(); }
    int depth() const { return value.depth(); }
};
}  // namespace fs
namespace sumhp::sumh {
bool is_available() {
    return fixture::available;
}
bool add_rule(const std::string& path, const std::string&, int) {
    fixture::operations.push_back("add " + path);
    return true;
}
bool add_merge_rule(const std::string& path, const std::string&) {
    fixture::operations.push_back("merge " + path);
    return true;
}
bool hide_path(const std::string& path) {
    fixture::operations.push_back("hide " + path);
    return true;
}
}  // namespace sumhp::sumh
namespace planner {
namespace logging {
enum class Level { Debug, Error };
}
void mlog(const std::string&, logging::Level = logging::Level::Debug) {
    ++fixture::logs;
}
struct ModuleRule {
    std::string path, mode;
};
struct ModuleEntry {
    std::string id;
    fs::path path;
};
struct Config {
    std::vector<std::string> partitions;
};
using ModuleRuleMap = std::map<std::string, std::vector<ModuleRule>>;
namespace fsutil {
std::vector<std::string> managed_partitions(const std::vector<std::string>& extras) {
    std::vector<std::string> result{"system", "vendor", "product"};
    for (const auto& part : extras)
        if (std::find(result.begin(), result.end(), part) == result.end())
            result.push_back(part);
    return result;
}
bool directory_is_opaque(const std::string& path, bool& opaque) {
    opaque = fixture::opaque.count(fixture::key(path)) ||
             realfs::exists(realfs::path(path) / ".replace");
    return !fixture::metadata_errors.count(fixture::key(path));
}
bool path_is_whiteout(const std::string& path, bool& whiteout) {
    whiteout = fixture::whiteouts.count(fixture::key(path));
    return !fixture::metadata_errors.count(fixture::key(path));
}
std::optional<fs::path> module_partition_alias(const fs::path& path) {
    const auto it = fixture::links.find(fixture::key(path));
    if (it != fixture::links.end() && path.parent_path().filename() == "system" &&
        it->second == "../" + path.filename().generic_string())
        return path.parent_path().parent_path() / path.filename();
    return std::nullopt;
}
}  // namespace fsutil
struct stat {
    unsigned st_mode = 0;
    unsigned long long st_dev = 1, st_ino = 0;
    long long st_size = 0;
    struct timestamp {
        long long tv_sec = 0, tv_nsec = 0;
    } st_mtim, st_ctim;
};
int lstat(const fs::path::value_type* name, struct stat* st) {
    const fs::path path(name);
    const auto key = fixture::key(path);
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec || !fs::exists(status) || fixture::stat_errors.count(key)) {
        errno = EIO;
        return -1;
    }
    st->st_mode = static_cast<unsigned>(fixture::stat_types.count(key) ? fixture::stat_types.at(key)
                                                                       : status.type());
    st->st_ino = std::hash<std::string>{}(key);
    if (status.type() == fs::file_type::regular)
        st->st_size = static_cast<long long>(realfs::file_size(path, ec));
    st->st_mtim.tv_sec = fixture::metadata_revisions[key];
    st->st_ctim.tv_nsec = fixture::metadata_revisions[key];
    return 0;
}
#define S_ISDIR(mode) ((mode) == static_cast<unsigned>(fs::file_type::directory))
#define S_ISREG(mode) ((mode) == static_cast<unsigned>(fs::file_type::regular))
#define S_ISLNK(mode) ((mode) == static_cast<unsigned>(fs::file_type::symlink))
fs::path active_file() {
    return fixture::root / "run" / "active";
}
bool write_boot_marker(const fs::path& path) {
    std::ofstream(path) << "active";
    return true;
}
void invalidate_active_state() {
    std::error_code ec;
    realfs::remove(active_file(), ec);
}
#include "sumh_planner_under_test.inc"
}  // namespace planner
namespace whiteout_test {
namespace fsutil = ::whiteout_test;
using ssize_t = long long;
struct stat {
    unsigned st_mode = 0;
    unsigned st_rdev = 0;
    long long st_size = 0;
};
constexpr unsigned regular = static_cast<unsigned>(fs::file_type::regular);
constexpr unsigned character = static_cast<unsigned>(fs::file_type::character);
#define S_ISCHR(mode) ((mode) == static_cast<unsigned>(fs::file_type::character))
#ifndef ENODATA
#define ENODATA 61
#endif
unsigned makedev(unsigned major, unsigned minor) {
    return major * 256 + minor;
}
struct stat current;
int stat_error = 0, attribute_error = ENODATA, attribute_calls = 0;
int lstat(const char*, struct stat* result) {
    if (stat_error) {
        errno = stat_error;
        return -1;
    }
    *result = current;
    return 0;
}
ssize_t lgetxattr(const char*, const char*, void*, std::size_t) {
    ++attribute_calls;
    if (attribute_error) {
        errno = attribute_error;
        return -1;
    }
    return 0;
}
namespace logging {
enum class Level { Error };
}
void mlog(const std::string&, logging::Level) {}
[[noreturn]] void scan_error(const std::string&) {
    throw std::runtime_error("metadata failure");
}
enum class NType { Regular, Directory, Symlink, Whiteout };
#include "sumh_whiteout_under_test.inc"
void run() {
    bool whiteout = false;
    current = {character, 0, 0};
    assert(path_is_whiteout("device", whiteout) && whiteout && attribute_calls == 0);
    assert(type_from_lstat("device", current) == NType::Whiteout);
    current = {character, 1, 0};
    assert(path_is_whiteout("other-device", whiteout) && !whiteout);
    current = {regular, 0, 0};
    attribute_error = 0;
    assert(path_is_whiteout("attribute", whiteout) && whiteout);
    assert(type_from_lstat("attribute", current) == NType::Whiteout);
    attribute_error = ENODATA;
    assert(path_is_whiteout("empty", whiteout) && !whiteout);
    assert(type_from_lstat("empty", current) == NType::Regular);
    attribute_error = EOPNOTSUPP;
    assert(path_is_whiteout("unsupported", whiteout) && !whiteout);
    attribute_error = EPERM;
    assert(!path_is_whiteout("denied", whiteout));
    bool threw = false;
    try {
        (void)type_from_lstat("denied", current);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    current.st_size = 1;
    attribute_calls = 0;
    assert(path_is_whiteout("nonempty", whiteout) && !whiteout && attribute_calls == 0);
    assert(type_from_lstat("nonempty", current) == NType::Regular);
    stat_error = EACCES;
    assert(!path_is_whiteout("missing", whiteout));
}
}  // namespace whiteout_test

namespace diagnostic_tests {
using namespace planner;
int cases = 0;

bool same_plan(const ModuleMountPlan& a, const ModuleMountPlan& b) {
    if (a.hide != b.hide || a.mappings.size() != b.mappings.size())
        return false;
    for (std::size_t i = 0; i < a.mappings.size(); ++i) {
        if (a.mappings[i].path != b.mappings[i].path ||
            a.mappings[i].source != b.mappings[i].source ||
            a.mappings[i].merge != b.mappings[i].merge)
            return false;
    }
    return true;
}

const ModulePlanEvent* event(const ModulePlanDiagnostics& trace, const std::string& action,
                             const std::string& reason = "", const std::string& path = "") {
    const auto found =
        std::find_if(trace.events.begin(), trace.events.end(), [&](const auto& item) {
            return item.action == action && (reason.empty() || item.reason == reason) &&
                   (path.empty() || item.resolved_path == path);
        });
    return found == trace.events.end() ? nullptr : &*found;
}

std::vector<std::string> observations(const ModulePlanDiagnostics& trace) {
    std::vector<std::string> result;
    for (const auto& item : trace.observations)
        result.push_back(item.module_id + "\n" + item.path + "\n" + item.metadata);
    std::sort(result.begin(), result.end());
    return result;
}

void run() {
    Config config;
    std::string error;
    std::string explained;
    assert(resolve_module_plan_path("/system/vendor/keep ", explained, error));
    assert(explained == "/vendor/keep ");
    ++cases;
    ModuleMountPlan plan, plain;
    ModulePlanDiagnostics trace, previous;
    const auto m = fixture::root / "m";
    const std::vector<ModuleEntry> modules{{"m", m}};
    const auto pass = [&](const ModuleRuleMap& rules) {
        const auto previous_logs = fixture::logs;
        assert(prepare_modules_diagnostic(modules, config, rules, plan, trace, error));
        assert(trace.complete && !trace.events_truncated && !trace.observations_truncated);
        assert(fixture::logs == previous_logs);
        assert(prepare_modules(modules, config, rules, plain, error));
        assert(same_plan(plan, plain));
        assert(fixture::operations.empty());
        assert(!realfs::exists(fixture::root / "run" / "active"));
        ++cases;
    };
    const auto failure = [&](const ModuleRuleMap& rules, const std::string& text) {
        const auto old = plan;
        assert(!prepare_modules_diagnostic(modules, config, rules, plan, trace, error));
        assert(!trace.complete && event(trace, "ERROR"));
        assert(error.find(text) != std::string::npos && same_plan(old, plan));
        const auto old_plain = plain;
        assert(!prepare_modules(modules, config, rules, plain, error));
        assert(same_plan(old_plain, plain));
        assert(fixture::operations.empty());
        ++cases;
    };

    fixture::reset();
    fixture::file("m/vendor/etc/a");
    fixture::file("m/vendor/etc/sub/keep");
    pass({});
    assert(plan.mappings.size() == 1 && event(trace, "MERGE", "eligible_directory", "/vendor/etc"));
    assert(event(trace, "SKIP", "missing_partition", "/system"));
    assert(std::any_of(trace.observations.begin(), trace.observations.end(), [&](const auto& item) {
        return fs::path(item.path).generic_string() == (m / "vendor/etc/sub/keep").generic_string();
    }));
    previous = trace;
    pass({});
    assert(observations(previous) == observations(trace));
    ++fixture::metadata_revisions[fixture::key(m / "vendor/etc/sub/keep")];
    pass({});
    assert(observations(previous) != observations(trace));
    previous = trace;
    std::ofstream(m / "vendor/etc/sub/keep", std::ios::app) << "new contents";
    pass({});
    assert(observations(previous) != observations(trace));
    previous = trace;
    fixture::file("m/vendor/etc/sub/new");
    pass({});
    assert(observations(previous) != observations(trace));
    previous = trace;
    realfs::remove(m / "vendor/etc/sub/new");
    pass({});
    assert(observations(previous) != observations(trace));

    pass({{"m", {{"/system/vendor/etc", "none"}, {"/system/vendor/etc/sub/keep", "sumh"}}}});
    const auto* chosen = event(trace, "ADD", "", "/vendor/etc/sub/keep");
    assert(chosen && chosen->matched_rule_path == "/vendor/etc/sub/keep");
    assert(event(trace, "RULE", "resolved", "/vendor/etc")->alias);
    assert(event(trace, "SKIP", "excluded_with_nested_rules", "/vendor/etc"));
    pass({{"m", {{"/vendor", "none"}}}});
    assert(plan.mappings.empty() && event(trace, "SKIP", "excluded_partition", "/vendor"));
    pass({{"m", {{"/vendor/etc", "hide"}, {"/vendor/etc/sub/keep", "sumh"}}}});
    assert(plan.mappings.empty() && plan.hide.count("/vendor/etc"));
    assert(event(trace, "HIDE", "selected_hide", "/vendor/etc")->hidden_parent == "/vendor/etc");
    pass({{"m", {{"/vendor/etc/sub/keep", "magic"}}}});
    chosen = event(trace, "ADD", "", "/vendor/etc/sub/keep");
    assert(chosen && chosen->requested_mode == "magic" && chosen->effective_mode == "sumh");
    assert(event(trace, "POLICY", "path_backend_coerced_to_sumh", "/vendor/etc/sub/keep"));
    pass({{"m", {{"/vendor/etc/sub/keep", "overlay"}}}});
    assert(event(trace, "ADD", "", "/vendor/etc/sub/keep")->effective_mode == "sumh");
    pass({{"m", {{"/vendor/etc", "none"}, {"/vendor/etc", "sumh"}}}});
    assert(event(trace, "RULE", "literal_overridden")->requested_mode == "none");
    pass({{"m", {{"/vendor/etc", "sumh"}, {"/system/vendor/etc", "sumh"}}}});
    assert(event(trace, "RULE", "alias_duplicate", "/vendor/etc"));
    failure({{"m", {{"/vendor/etc", "sumh"}, {"/system/vendor/etc", "hide"}}}}, "conflicting");
    pass({{"m", {{"/vendor/etc/", "none"}}}});
    assert(plan.mappings.empty());

    fixture::whiteouts.insert(fixture::key(m / "vendor/etc/a"));
    pass({});
    assert(event(trace, "WALK", "merge_whiteout") &&
           event(trace, "HIDE", "whiteout", "/vendor/etc/a"));
    fixture::whiteouts.clear();
    fixture::metadata_errors.insert(fixture::key(m / "vendor/etc/a"));
    failure({}, "metadata");
    fixture::metadata_errors.clear();
    fixture::opaque.insert(fixture::key(m / "vendor/etc/sub"));
    failure({}, "replacement");
    fixture::opaque.clear();
    fixture::file("m/vendor/etc/sub/.replace");
    failure({}, "replacement");
    realfs::remove(m / "vendor/etc/sub/.replace");

    fixture::directory_calls.clear();
    fixture::directory_error_after["/vendor/etc"] = 2;
    failure({}, "cannot inspect virtual directory");
    fixture::directory_error_after.clear();
    fixture::iterator_errors.insert(fixture::key(m / "vendor"));
    failure({}, "scan failed");
    fixture::iterator_errors.clear();
    fixture::increment_error = fixture::key(m / "vendor/etc/sub/keep");
    failure({}, "scan failed");
    fixture::increment_error.clear();
    fixture::source_status_errors.insert(fixture::key(m / "vendor/etc/a"));
    failure({}, "scan failed");
    fixture::source_status_errors.clear();
    fixture::stat_errors.insert(fixture::key(m / "vendor/etc/a"));
    failure({}, "cannot inspect module entry");
    fixture::stat_errors.clear();
    fixture::stat_types[fixture::key(m / "vendor/etc")] = fs::file_type::regular;
    failure({}, "type changed");
    fixture::stat_types.clear();

    fixture::reset();
    fixture::file("m/vendor/etc/link");
    fixture::links[fixture::key(m / "vendor/etc/link")] = "target";
    fixture::links["/vendor/etc/link"] = "target";
    pass({});
    assert(event(trace, "WALK", "merge_symlink"));
    assert(event(trace, "SKIP", "matching_system_symlink", "/vendor/etc/link"));
    assert(fixture::logs > 0);  // normal production preparation retains its log
    fixture::readlink_errors.insert("/vendor/etc/link");
    failure({}, "cannot read virtual symlink");
    fixture::readlink_errors.clear();
    fixture::readlink_errors.insert(fixture::key(m / "vendor/etc/link"));
    failure({}, "cannot read module symlink");
    fixture::readlink_errors.clear();
    previous = trace;
    fixture::links[fixture::key(m / "vendor/etc/link")] = "other";
    pass({});
    assert(event(trace, "ADD", "", "/vendor/etc/link"));
    previous = trace;
    fixture::links[fixture::key(m / "vendor/etc/link")] = "third";
    pass({});
    assert(observations(previous) != observations(trace));

    fixture::reset();
    fixture::file("m/system/vendor/etc/a");
    pass({{"m", {{"/vendor/etc/a", "sumh"}}}});
    chosen = event(trace, "ADD", "", "/vendor/etc/a");
    assert(chosen && chosen->alias && chosen->original_path == "/system/vendor/etc/a");

    fixture::reset();
    fixture::file("m/vendor/etc/a");
    fixture::file("m/system/vendor");
    fixture::links[fixture::key(m / "system/vendor")] = "../vendor";
    pass({});
    assert(event(trace, "SKIP", "installer_partition_alias", "/system/vendor"));
    fixture::links["/vendor/etc/alias"] = "/extra";
    fixture::file("m/vendor/etc/alias/item");
    pass({});
    assert(event(trace, "WALK", "merge_partition_alias"));

    fixture::reset();
    fixture::file("m/vendor/etc/a/b/c/d/e/f/g/h/i/leaf");
    pass({});
    assert(event(trace, "WALK", "merge_depth_limit"));

    fixture::reset();
    fixture::file("a/vendor/etc/same");
    fixture::file("b/vendor/etc/same");
    const std::vector<ModuleEntry> overlapping{{"a", fixture::root / "a"},
                                               {"b", fixture::root / "b"}};
    const ModuleRuleMap direct{{"a", {{"/vendor/etc/same", "sumh"}}},
                               {"b", {{"/vendor/etc/same", "sumh"}}}};
    assert(prepare_modules_diagnostic(overlapping, config, direct, plan, trace, error));
    assert(prepare_modules(overlapping, config, direct, plain, error) && same_plan(plan, plain));
    assert(plan.mappings[0].source.find("b") != std::string::npos);
    const auto overwrite = std::find_if(
        trace.events.begin(), trace.events.end(),
        [](const auto& item) { return item.module_id == "a" && item.action == "ADD"; });
    assert(overwrite != trace.events.end() && overwrite->has_previous_direct &&
           overwrite->previous_module_id == "b" && overwrite->mapping_index == 1);
    ++cases;
    assert(prepare_modules_diagnostic(overlapping, config, {}, plan, trace, error));
    assert(plan.mappings.size() == 2 && plan.mappings[0].merge && plan.mappings[1].merge);
    assert(std::none_of(trace.events.begin(), trace.events.end(),
                        [](const auto& item) { return item.has_previous_direct; }));
    ++cases;
    assert(prepare_modules_diagnostic(overlapping, config, {{"a", {{"/vendor/etc", "hide"}}}}, plan,
                                      trace, error));
    assert(event(trace, "MERGE", "", "/vendor/etc")->hidden_parent == "/vendor/etc");
    ++cases;
    fixture::whiteouts.insert(fixture::key(fixture::root / "a/vendor/etc/same"));
    assert(prepare_modules_diagnostic(overlapping, config, direct, plan, trace, error));
    assert(event(trace, "ADD", "", "/vendor/etc/same")->hidden_parent == "/vendor/etc/same");
    ++cases;

    fixture::reset();
    fixture::file("m/vendor/etc/new directory/item value");
    pass({{"m", {{"/vendor/etc/new directory/item value", "sumh"}}}});
    assert(event(trace, "WALK", "target_not_directory", "/vendor/etc/new directory"));
    assert(event(trace, "ADD", "", "/vendor/etc/new directory/item value"));

    // Diagnostic saturation stops the read-only walk and preserves the caller's
    // last complete plan. Default production behavior retains its original limit.
    fixture::reset();
    for (int i = 0; i < 2300; ++i)
        fixture::file("m/vendor/flat/" + std::to_string(i));
    const auto old_plan = plan;
    assert(!prepare_modules_diagnostic(modules, config, {}, plan, trace, error));
    assert(error.find("capacity exceeded") != std::string::npos && same_plan(old_plan, plan));
    assert(prepare_modules(modules, config, {}, plain, error));
    assert(plain.mappings.size() == 2300 && !trace.complete);
    assert(trace.events_truncated || trace.observations_truncated);
    assert(trace.events.size() <= ModulePlanDiagnostics::max_events &&
           trace.observations.size() <= ModulePlanDiagnostics::max_observations &&
           trace.bytes <= ModulePlanDiagnostics::max_bytes);
    ++cases;
    ModulePlanDiagnostics bounded;
    PlannerTrace cursor;
    cursor.output = &bounded;
    cursor.module_id = "m";
    ModulePlanEvent large;
    large.source.assign(ModulePlanDiagnostics::max_bytes + 1, 'x');
    trace_event(&cursor, large);
    assert(bounded.events.empty() && bounded.events_truncated && bounded.bytes == 0);
    trace_observation(&cursor, m, large.source);
    assert(bounded.observations.empty() && bounded.observations_truncated && bounded.bytes == 0);
    ++cases;
    bounded = {};
    ModuleMountPlan full;
    full.mappings.resize(ModulePlanDiagnostics::max_actions);
    assert(!trace_action_available(&cursor, full, error) && bounded.events_truncated);
    assert(trace_action_available(nullptr, full, error));
    ++cases;
    bounded = {};
    for (std::size_t i = 0; i <= ModulePlanDiagnostics::max_observations; ++i)
        trace_observation(&cursor, {}, "");
    assert(bounded.observations_truncated &&
           bounded.observations.size() <= ModulePlanDiagnostics::max_observations &&
           bounded.bytes <= ModulePlanDiagnostics::max_bytes);
    ++cases;
    fixture::available = false;
    failure({}, "unavailable");
    assert(trace.observations.empty() && !trace.events_truncated && !trace.observations_truncated);
    std::cout << cases << " production planner diagnostic/error scenarios passed\n";
}
}  // namespace diagnostic_tests

int main() {
    whiteout_test::run();
    using namespace planner;
    fixture::root = realfs::temp_directory_path() /
                    ("sumh-planner-fixture-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fixture::reset();
    std::string error;
    std::vector<ModuleRule> rules;
    assert(resolve_module_rules({{"/vendor/etc", "none"}, {"/vendor/etc", "sumh"}}, rules, error));
    assert(rules.size() == 1 && effective_mode("/vendor/etc/a", rules) == "sumh");
    assert(resolve_module_rules({{"/system/vendor/etc", "none"}, {"/vendor/etc", "none"}}, rules,
                                error));
    assert(rules.size() == 1 && rules[0].path == "/vendor/etc");
    assert(!resolve_module_rules({{"/system/vendor/etc", "none"}, {"/vendor/etc", "hide"}}, rules,
                                 error));
    assert(error.find("conflicting") != std::string::npos);
    assert(resolve_module_rules({{"/system/vendor/", "none"}}, rules, error));
    assert(effective_mode("/vendor", rules) == "sumh");
    assert(effective_mode("/vendor/etc/a", rules) == "none");
    assert(resolve_module_rules({{"/system/vendor/ Missing ", "none"}}, rules, error));
    assert(rules[0].path == "/vendor/ Missing ");

    fixture::file("m/vendor/etc/a");
    fixture::file("m/vendor/etc/sub/keep");
    ModuleMountPlan plan;
    std::vector<ModuleEntry> modules{{"m", fixture::root / "m"}};
    Config config;
    ModuleRuleMap saved{
        {"m", {{"/system/vendor/etc", "none"}, {"/system/vendor/etc/sub/keep", "sumh"}}}};
    assert(prepare_modules(modules, config, saved, plan, error));
    assert(plan.mappings.size() == 1 && plan.mappings[0].path == "/vendor/etc/sub/keep" &&
           !plan.mappings[0].merge);
    assert(fixture::operations.empty());
    saved["m"] = {{"/system/vendor", "none"}};
    assert(prepare_modules(modules, config, saved, plan, error));
    assert(plan.mappings.empty());
    saved.clear();
    assert(prepare_modules(modules, config, saved, plan, error));
    assert(plan.mappings.size() == 1 && plan.mappings[0].merge);

    // Both device and attribute whiteouts enter the shared path classifier;
    // either prevents bulk MERGE and is emitted through existing HIDE.
    fixture::whiteouts.insert(fixture::key(fixture::root / "m/vendor/etc/a"));
    assert(prepare_modules(modules, config, saved, plan, error));
    assert(plan.hide.count("/vendor/etc/a") == 1);
    assert(std::none_of(plan.mappings.begin(), plan.mappings.end(),
                        [](const auto& m) { return m.path == "/vendor/etc"; }));
    assert(apply_modules(plan));
    assert(std::find(fixture::operations.begin(), fixture::operations.end(),
                     "hide /vendor/etc/a") != fixture::operations.end());

    // A failed preflight does not overwrite a previously prepared plan or emit ioctls.
    fixture::operations.clear();
    const auto old_count = plan.mappings.size();
    fixture::opaque.insert(fixture::key(fixture::root / "m/vendor/etc/sub"));
    assert(!prepare_modules(modules, config, saved, plan, error));
    assert(plan.mappings.size() == old_count && plan.hide.count("/vendor/etc/a"));
    assert(fixture::operations.empty());
    saved["m"] = {{"/vendor/etc/sub", "none"}};
    assert(prepare_modules(modules, config, saved, plan, error));
    fixture::opaque.clear();
    fixture::metadata_errors.insert(fixture::key(fixture::root / "m/vendor/etc/a"));
    assert(!prepare_modules(modules, config, {}, plan, error));
    fixture::metadata_errors.clear();
    const auto previous_hide = plan.hide;
    fixture::resolution_errors["/system/vendor/etc"] =
        std::make_error_code(std::errc::permission_denied);
    assert(
        !prepare_modules(modules, config, {{"m", {{"/system/vendor/etc", "none"}}}}, plan, error));
    assert(error.find("cannot resolve") != std::string::npos && plan.hide == previous_hide);
    fixture::resolution_errors.clear();
    fixture::resolution_errors["canonical:/vendor/etc"] = std::make_error_code(std::errc::io_error);
    assert(!prepare_modules(modules, config, {}, plan, error));
    assert(plan.hide == previous_hide && fixture::operations.empty());
    fixture::resolution_errors.clear();
    fixture::resolution_errors["/vendor/etc"] =
        std::make_error_code(std::errc::too_many_symbolic_link_levels);
    assert(!prepare_modules(modules, config, {}, plan, error));
    fixture::resolution_errors.clear();
    fixture::links[fixture::key(fixture::root / "m/vendor")] = "../elsewhere";
    assert(!prepare_modules(modules, config, {}, plan, error));
    assert(error.find("real directory") != std::string::npos && fixture::operations.empty());

    // Installer-created relative aliases are compiled only through /vendor.
    fixture::reset();
    fixture::file("m/vendor/etc/a");
    fixture::file("m/system/vendor");
    fixture::links[fixture::key(fixture::root / "m/system/vendor")] = "../vendor";
    assert(prepare_modules(modules, config, {}, plan, error));
    assert(plan.mappings.size() == 1 && plan.mappings[0].path == "/vendor/etc");
    // An arbitrary replacement symlink remains an error rather than being followed.
    fixture::links[fixture::key(fixture::root / "m/system/vendor")] = "../other";
    assert(!prepare_modules(modules, config, {}, plan, error));

    fixture::reset();
    fixture::file("m/vendor/etc/a");
    fixture::file("m/extra/etc/b");
    config.partitions = {"extra"};
    assert(prepare_modules(modules, config, {}, plan, error));
    assert(plan.mappings.size() == 2);
    fixture::available = false;
    assert(!prepare_modules(modules, config, {}, plan, error));
    diagnostic_tests::run();
    realfs::remove_all(fixture::root);
    std::cout << "SUMH native planner regressions passed\n";
}
