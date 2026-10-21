#include <algorithm>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef ESTALE
#define ESTALE 116
#endif
constexpr unsigned directory_mode = 0040000, regular_mode = 0100000, symlink_mode = 0120000;
#define S_ISDIR(mode) (((mode) & 0170000) == directory_mode)
#define S_ISREG(mode) (((mode) & 0170000) == regular_mode)
#define S_ISLNK(mode) (((mode) & 0170000) == symlink_mode)
constexpr int O_RDONLY = 1, O_DIRECTORY = 2, O_CLOEXEC = 4, O_NOFOLLOW = 8, SYS_renameat2 = 276;
namespace normalizer {
struct stat {
    std::uint64_t st_dev = 8, st_ino = 0, st_mode = 0, st_size = 0;
    struct Time {
        std::int64_t tv_sec = 0, tv_nsec = 0;
    } st_mtim, st_ctim;
};
}  // namespace normalizer
namespace model {
struct Node {
    normalizer::stat metadata;
    std::string link;
};
std::map<std::string, Node> nodes;
std::map<std::string, int> probe_errors, canonical_errors, operation_errors;
std::map<int, std::uint64_t> descriptors;
std::vector<std::string> mutations;
std::function<void(const std::string&)> before_mutation;
std::function<void(const std::string&)> before_probe;
std::uint64_t next_inode = 1;
int next_fd = 10, logs = 0;
std::string parent(const std::string& p) {
    const auto at = p.rfind('/');
    return at == 0 ? "/" : p.substr(0, at);
}
std::string normalize(const std::string& input) {
    std::vector<std::string> segments;
    std::istringstream stream(input);
    std::string part;
    while (std::getline(stream, part, '/')) {
        if (part.empty() || part == ".")
            continue;
        if (part == "..") {
            if (!segments.empty())
                segments.pop_back();
        } else
            segments.push_back(part);
    }
    std::string out = input.empty() || input[0] != '/' ? "" : "/";
    for (const auto& item : segments) {
        if (!out.empty() && out.back() != '/')
            out += '/';
        out += item;
    }
    return out;
}
std::string resolve(const std::string& input, int depth = 0) {
    if (depth > 16) {
        errno = ELOOP;
        return {};
    }
    const auto path = normalize(input);
    for (std::size_t end = 1; end <= path.size(); ++end) {
        if (end != path.size() && path[end] != '/')
            continue;
        const auto prefix = path.substr(0, end);
        const auto it = nodes.find(prefix);
        if (it == nodes.end())
            continue;
        if (S_ISLNK(it->second.metadata.st_mode)) {
            const auto& link = it->second.link;
            return resolve((!link.empty() && link[0] == '/' ? link : parent(prefix) + "/" + link) +
                               path.substr(end),
                           depth + 1);
        }
    }
    return path;
}
void touch(const std::string& path) {
    auto it = nodes.find(path);
    if (it != nodes.end()) {
        ++it->second.metadata.st_mtim.tv_nsec;
        ++it->second.metadata.st_ctim.tv_nsec;
    }
}
void add(const std::string& path, unsigned mode, const std::string& link = {}) {
    Node node;
    node.metadata.st_ino = next_inode++;
    node.metadata.st_mode = mode | 0755;
    node.link = link;
    node.metadata.st_size = link.size();
    nodes[path] = node;
    touch(parent(path));
}
void tree(const std::string& path) {
    if (path != "/" && !nodes.count(parent(path)))
        tree(parent(path));
    if (!nodes.count(path))
        add(path, directory_mode);
}
std::string descriptor_path(int fd) {
    for (const auto& [path, node] : nodes)
        if (node.metadata.st_ino == descriptors.at(fd))
            return path;
    return {};
}
int error(const std::string& name) {
    const auto it = operation_errors.find(name);
    if (it == operation_errors.end())
        return 0;
    errno = it->second;
    return -1;
}
int lstat(const char* name, normalizer::stat* out) {
    const std::string path(name);
    if (before_probe)
        before_probe(path);
    if (probe_errors.count(path)) {
        errno = probe_errors.at(path);
        return -1;
    }
    const auto it = nodes.find(path);
    if (it == nodes.end()) {
        errno = ENOENT;
        return -1;
    }
    *out = it->second.metadata;
    return 0;
}
long long readlink(const char* name, char* out, std::size_t capacity) {
    if (error("readlink:" + std::string(name)))
        return -1;
    const auto it = nodes.find(name);
    if (it == nodes.end() || !S_ISLNK(it->second.metadata.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    const auto count = std::min(capacity, it->second.link.size());
    std::memcpy(out, it->second.link.data(), count);
    return count;
}
int open(const char* name, int flags) {
    assert(flags == (O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (error("open:" + std::string(name)))
        return -1;
    const auto it = nodes.find(name);
    if (it == nodes.end() || !S_ISDIR(it->second.metadata.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    const int fd = next_fd++;
    descriptors[fd] = it->second.metadata.st_ino;
    return fd;
}
int openat(int fd, const char* name, int flags) {
    return open((descriptor_path(fd) + "/" + name).c_str(), flags);
}
int close(int fd) {
    assert(descriptors.erase(fd));
    return 0;
}
int fstat(int fd, normalizer::stat* out) {
    const auto path = descriptor_path(fd);
    if (path.empty()) {
        errno = ESTALE;
        return -1;
    }
    *out = nodes.at(path).metadata;
    return 0;
}
int mkdirat(int fd, const char* name, unsigned mode) {
    assert(mode == 0755);
    const auto path = descriptor_path(fd) + "/" + name;
    if (before_mutation)
        before_mutation("mkdir");
    if (error("mkdir"))
        return -1;
    if (nodes.count(path)) {
        errno = EEXIST;
        return -1;
    }
    add(path, directory_mode);
    mutations.push_back("mkdir " + path);
    return 0;
}
long long syscall(int number, int source_fd, const char* source_name, int target_fd,
                  const char* target_name, unsigned flags) {
    assert(number == SYS_renameat2 && flags == 1);
    const auto source = descriptor_path(source_fd) + "/" + source_name;
    const auto target = descriptor_path(target_fd) + "/" + target_name;
    if (before_mutation)
        before_mutation("move");
    if (error("move"))
        return -1;
    if (nodes.count(target)) {
        errno = EEXIST;
        return -1;
    }
    if (!nodes.count(source)) {
        errno = ENOENT;
        return -1;
    }
    std::vector<std::pair<std::string, Node>> moving;
    for (auto it = nodes.begin(); it != nodes.end();) {
        if (it->first == source || it->first.rfind(source + "/", 0) == 0) {
            moving.push_back({target + it->first.substr(source.size()), it->second});
            it = nodes.erase(it);
        } else
            ++it;
    }
    for (auto& [path, node] : moving)
        nodes[path] = node;
    ++nodes[target].metadata.st_ctim.tv_nsec;
    touch(parent(source));
    touch(parent(target));
    mutations.push_back("move " + source + " " + target);
    return 0;
}
int symlinkat(const char* link, int fd, const char* name) {
    const auto path = descriptor_path(fd) + "/" + name;
    if (before_mutation)
        before_mutation("symlink");
    if (error("symlink"))
        return -1;
    if (nodes.count(path)) {
        errno = EEXIST;
        return -1;
    }
    add(path, symlink_mode, link);
    mutations.push_back("symlink " + path);
    return 0;
}
int fsync(int) {
    if (error("fsync"))
        return -1;
    return 0;
}
void reset() {
    nodes.clear();
    probe_errors.clear();
    canonical_errors.clear();
    operation_errors.clear();
    descriptors.clear();
    mutations.clear();
    before_mutation = {};
    before_probe = {};
    next_inode = 1;
    next_fd = 10;
    logs = 0;
    tree("/data/adb/modules/m");
    add("/data/adb/modules/m/module.prop", regular_mode);
    tree("/system");
    tree("/vendor");
    add("/system/vendor", symlink_mode, "/vendor");
    errno = 0;
}
}  // namespace model
namespace normalizer {
namespace fs {
class path {
    std::string value;

public:
    path(const std::string& input) : value(input) {}
    bool is_absolute() const { return !value.empty() && value[0] == '/'; }
    path operator/(const path& other) const { return path(value + "/" + other.value); }
    path lexically_normal() const { return path(model::normalize(value)); }
    std::string generic_string() const { return value; }
    std::string string() const { return value; }
    path filename() const { return path(value.substr(value.rfind('/') + 1)); }
};
bool is_directory(const std::string& path, std::error_code& ec) {
    ec.clear();
    if (model::probe_errors.count(path)) {
        ec = {model::probe_errors.at(path), std::generic_category()};
        return false;
    }
    const auto found = model::nodes.find(model::resolve(path));
    if (found == model::nodes.end()) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return false;
    }
    return S_ISDIR(found->second.metadata.st_mode);
}
path canonical(const std::string& name, std::error_code& ec) {
    ec.clear();
    if (model::canonical_errors.count(name)) {
        ec = {model::canonical_errors.at(name), std::generic_category()};
        return path("");
    }
    const auto result = model::resolve(name);
    if (result.empty() || !model::nodes.count(result)) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return path("");
    }
    return path(result);
}
}  // namespace fs
int lstat(const char* name, struct stat* out) {
    return model::lstat(name, out);
}
#include "module_normalize_path_under_test.inc"

namespace logging {
enum class Level { Error };
}
void mlog(const std::string&, logging::Level) {
    ++model::logs;
}
#include "module_normalize_under_test.inc"
}  // namespace normalizer
using namespace normalizer;
const std::string root = "/data/adb/modules/m";
int cases = 0;
NormalizationPlan planned() {
    NormalizationPlan plan;
    std::string error;
    const auto old = model::mutations;
    assert(prepare_normalization(root, plan, error));
    assert(plan.complete && error.empty());
    assert(model::mutations == old && model::logs == 0);
    ++cases;
    return plan;
}
void execute_success(const NormalizationPlan& plan, bool changed) {
    NormalizationResult result;
    std::string error;
    if (!execute_normalization(plan, result, error))
        std::cerr << error << '\n';
    assert(result.complete && result.status == "completed" && result.changed == changed &&
           error.empty());
    for (const auto& step : result.actions)
        if (step.status == "completed")
            assert(step.effect_observed && step.durability_confirmed);
    assert(model::descriptors.empty());
    ++cases;
}
void prepare_failure(const std::string& path = root) {
    NormalizationPlan plan;
    plan.resolved_root = "preserved";
    std::string error;
    assert(!prepare_normalization(path, plan, error));
    assert(!error.empty() && plan.resolved_root == "preserved");
    assert(model::mutations.empty() && model::logs == 0);
    ++cases;
}
NormalizationResult execute_failure(const NormalizationPlan& plan, bool changed) {
    NormalizationResult result;
    std::string error;
    assert(!execute_normalization(plan, result, error));
    assert(!result.complete && !error.empty() && result.changed == changed);
    assert(model::descriptors.empty());
    if (changed)
        assert(result.status == "partial");
    ++cases;
    return result;
}
int main() {
    model::reset();
    auto plan = planned();
    assert(plan.actions.size() == 5);
    execute_success(plan, false);
    model::reset();
    model::tree(root + "/vendor");
    model::add(root + "/vendor/file", regular_mode);
    plan = planned();
    assert(plan.actions[0].kind == "mkdir" && plan.actions[1].kind == "move" &&
           plan.actions[2].kind == "symlink");
    const auto source_inode = model::nodes.at(root + "/vendor").metadata.st_ino;
    execute_success(plan, true);
    assert(model::nodes.at(root + "/system/vendor").metadata.st_ino == source_inode);
    assert(model::nodes.at(root + "/vendor").link == "./system/vendor" &&
           model::nodes.count(root + "/system/vendor/file"));
    model::mutations.clear();
    plan = planned();
    assert(plan.actions[0].kind == "unchanged");
    execute_success(plan, false);
    model::reset();
    model::tree(root + "/system/vendor");
    plan = planned();
    assert(plan.actions[0].kind == "symlink");
    execute_success(plan, true);
    model::reset();
    model::tree(root + "/system/vendor");
    model::add(root + "/vendor", symlink_mode, root + "/system/vendor");
    plan = planned();
    execute_success(plan, false);
    for (const auto* path : {"/", "/data", "/data/adb", "/data/adb/modules", "/system", "/vendor",
                             "relative", "/data/../data/adb/modules/m", "/data/adb/modules/m/"}) {
        model::reset();
        prepare_failure(path);
    }
    model::reset();
    model::nodes.erase(root + "/module.prop");
    prepare_failure();
    model::reset();
    model::nodes.at(root + "/module.prop").metadata.st_mode = symlink_mode;
    model::nodes.at(root + "/module.prop").link = "/etc/foo";
    prepare_failure();
    model::reset();
    model::add(root + "/system", symlink_mode, "/system");
    prepare_failure();
    model::reset();
    model::tree(root + "/vendor");
    model::tree(root + "/system/vendor");
    prepare_failure();
    model::reset();
    model::add(root + "/vendor", regular_mode);
    prepare_failure();
    model::reset();
    model::tree(root + "/system/vendor");
    model::add(root + "/vendor", symlink_mode, "/vendor");
    prepare_failure();
    model::reset();
    model::add(root + "/vendor", symlink_mode, "./system/vendor");
    prepare_failure();
    model::reset();
    model::tree(root + "/vendor");
    model::tree(root + "/system");
    model::add(root + "/system/vendor", symlink_mode, "../vendor");
    prepare_failure();
    model::reset();
    model::nodes.at("/system/vendor").link = "/system";
    prepare_failure();
    model::reset();
    model::probe_errors["/system/vendor"] = EACCES;
    prepare_failure();
    model::reset();
    model::probe_errors["/vendor"] = EIO;
    prepare_failure();
    model::reset();
    model::canonical_errors["/system/vendor"] = EIO;
    prepare_failure();
    model::reset();
    model::operation_errors["readlink:/system/vendor"] = EACCES;
    prepare_failure();
    model::reset();
    model::nodes.at("/system/vendor").link = std::string(PATH_MAX, 'x');
    prepare_failure();
    model::reset();
    model::tree(root + "/vendor");
    plan = planned();
    model::touch(root + "/vendor");
    execute_failure(plan, false);
    assert(model::mutations.empty());
    model::reset();
    model::tree(root + "/vendor");
    plan = planned();
    model::add(root + "/system", symlink_mode, "/system");
    execute_failure(plan, false);
    model::reset();
    model::tree(root + "/vendor");
    plan = planned();
    plan.actions[1].target = "/outside";
    execute_failure(plan, false);
    assert(model::mutations.empty());
    model::reset();
    model::tree(root + "/vendor");
    plan = planned();
    model::operation_errors["mkdir"] = EACCES;
    auto result = execute_failure(plan, false);
    assert(result.actions[0].status == "failed");
    model::reset();
    model::tree(root + "/vendor");
    plan = planned();
    model::operation_errors["move"] = ENOSYS;
    result = execute_failure(plan, true);
    assert(result.actions[0].status == "completed" && result.actions[1].status == "failed" &&
           result.actions[2].status == "not_run");
    assert(model::nodes.count(root + "/vendor"));
    model::reset();
    model::tree(root + "/vendor");
    plan = planned();
    model::operation_errors["symlink"] = EACCES;
    result = execute_failure(plan, true);
    assert(model::nodes.count(root + "/system/vendor") && !model::nodes.count(root + "/vendor"));
    assert(result.actions[2].status == "failed");
    model::operation_errors.clear();
    model::mutations.clear();
    plan = planned();
    execute_success(plan, true);
    assert(model::mutations.size() == 1);
    model::reset();
    model::tree(root + "/vendor");
    plan = planned();
    model::operation_errors["fsync"] = EIO;
    result = execute_failure(plan, true);
    assert(result.actions[0].status == "failed" && model::nodes.count(root + "/system"));
    assert(result.actions[0].effect_observed && !result.actions[0].durability_confirmed);
    model::reset();
    model::tree(root + "/vendor");
    plan = planned();
    model::before_mutation = [](const std::string& action) {
        if (action == "move")
            model::add(root + "/system/vendor", directory_mode);
    };
    execute_failure(plan, true);
    assert(model::nodes.count(root + "/vendor"));
    model::reset();
    model::tree(root + "/vendor");
    plan = planned();
    model::before_mutation = [](const std::string& action) {
        if (action == "symlink")
            model::add(root + "/vendor", regular_mode);
    };
    execute_failure(plan, true);
    assert(S_ISREG(model::nodes.at(root + "/vendor").metadata.st_mode));
    model::reset();
    model::tree(root + "/vendor");
    plan = planned();
    model::operation_errors["open:" + root] = EACCES;
    execute_failure(plan, false);
    model::reset();
    model::tree(root + "/vendor");
    plan = planned();
    model::operation_errors["open:" + root + "/system"] = EACCES;
    execute_failure(plan, true);
    model::reset();
    model::tree(root + "/vendor");
    model::tree("/product");
    model::add("/system/product", symlink_mode, "../product");
    model::tree(root + "/product");
    plan = planned();
    execute_success(plan, true);
    assert(model::nodes.count(root + "/system/vendor") &&
           model::nodes.count(root + "/system/product"));
    model::reset();
    model::add("/data/adb/modules/alias", symlink_mode, root);
    prepare_failure("/data/adb/modules/alias");
    model::reset();
    model::add("/data/alias", symlink_mode, "/data/adb");
    prepare_failure("/data/alias/modules/m");
    model::reset();
    model::tree(root + "/vendor");
    plan = planned();
    model::nodes.at(root).metadata.st_ino = 999;
    execute_failure(plan, false);
    assert(model::mutations.empty());
    model::reset();
    model::tree(root + "/vendor");
    plan = planned();
    model::nodes.at("/system/vendor").metadata.st_ino = 999;
    execute_failure(plan, false);
    assert(model::mutations.empty());
    model::reset();
    model::tree(root + "/vendor");
    plan = planned();
    plan.bytes = 0;
    execute_failure(plan, false);
    model::reset();
    model::tree(root + "/vendor");
    plan = planned();
    model::before_mutation = [](const std::string& action) {
        if (action == "move")
            model::add(root + "/vendor", directory_mode);
    };
    execute_failure(plan, true);  // observed moved inode differs; no overwrite/rollback
    model::reset();
    model::tree(root + "/vendor");
    plan = planned();
    int probes = 0;
    model::before_probe = [&](const std::string& path) {
        if (path == root && ++probes == 2)
            model::nodes.at(root).metadata.st_ino = 999;
    };
    execute_failure(plan, false);
    assert(model::mutations.empty());
    model::reset();
    model::tree(root + "/vendor");
    plan = planned();
    model::before_mutation = [](const std::string& action) {
        if (action == "symlink")
            model::operation_errors["readlink:" + root + "/vendor"] = EIO;
    };
    result = execute_failure(plan, true);
    assert(result.actions[2].effect_observed);
    model::reset();
    model::nodes.at(root + "/module.prop").metadata.st_size = 1024ULL * 1024 * 1024;
    plan = planned();
    execute_success(plan, false);  // module.prop identity only, never contents
    model::reset();
    const std::string long_root = "/data/" + std::string(4000, 'x');
    model::tree(long_root);
    model::add(long_root + "/module.prop", regular_mode);
    model::tree(long_root + "/vendor");
    prepare_failure(long_root);  // bounded plan bytes; caller plan remains unchanged
    model::reset();
    std::string inspected = "previous", inspect_error;
    assert(inspect_module_root(root, inspected, inspect_error) && inspected == root &&
           inspect_error.empty());
    ++cases;
    model::tree(root + "/vendor");
    model::tree(root + "/system/vendor");
    assert(inspect_module_root(root, inspected, inspect_error));
    ++cases;  // conflict belongs to normalize/planner
    model::nodes[root + "/system"].metadata.st_mode = symlink_mode;
    model::nodes[root + "/system"].link = "/outside";
    assert(inspect_module_root(root, inspected, inspect_error));
    ++cases;  // no layout traversal here
    model::probe_errors["/vendor"] = EIO;
    assert(inspect_module_root(root, inspected, inspect_error));
    ++cases;
    inspected = "previous";
    model::nodes.erase(root + "/module.prop");
    assert(!inspect_module_root(root, inspected, inspect_error) && inspected == "previous");
    ++cases;
    assert(model::mutations.empty() && model::logs == 0);
    model::reset();
    NormalizationPlan capacity;
    capacity.resolved_root = root;
    capacity.bytes = NormalizationPlan::max_bytes;
    std::string error;
    assert(!normalization_action(capacity, {"skip", "", "", "", ""}, error));
    ++cases;
    model::reset();
    assert(!normalize_module("/"));
    assert(model::logs == 1);
    ++cases;
    std::cout << cases << " production normalization plan/execution scenarios passed\n";
}
