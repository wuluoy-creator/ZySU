// Actual preview/controller/parser code; only external observation boundaries doubled.
#include "mount/module_preview.hpp"
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
#include "core/rule_inventory.hpp"
#include "mount/development.hpp"
#include "mount/sumh.hpp"

constexpr int O_RDONLY = 1, O_CLOEXEC = 2, O_NOFOLLOW = 4, O_NONBLOCK = 8, O_DIRECTORY = 16;
constexpr int LOCK_EX = 1, LOCK_NB = 2, PROT_READ = 1, PROT_WRITE = 2, MAP_SHARED = 1,
              MAP_ANONYMOUS = 2;
#ifndef STDIN_FILENO
constexpr int STDIN_FILENO = 0;
#endif
#define MAP_FAILED reinterpret_cast<void*>(-1)
namespace preview_model {
using namespace sumhp;
using namespace sumhp::mount;
struct Metadata {
    unsigned st_mode = 0100600, st_nlink = 1, st_uid = 0;
    std::int64_t st_size = 0;
    std::uint64_t st_ino = 3, st_dev = 8;
};
struct File {
    std::string bytes;
    int error = 0, stat_error = 0, read_error = 0;
    Metadata metadata;
    bool size_override = false;
    File(std::string value = {}, int failure = 0) : bytes(std::move(value)), error(failure) {}
};
struct Handle {
    File file;
    std::size_t offset = 0;
};
std::map<std::string, std::vector<File>> documents;
std::map<std::string, std::size_t> document_index;
std::map<int, Handle> handles;
std::vector<std::string> boots, owners;
std::vector<std::vector<ModuleEntry>> discovery;
std::vector<bool> discovery_ok;
std::vector<::sumhp::sumh::RuntimeState> runtimes;
std::vector<bool> runtime_ok;
std::map<std::string, ModuleBackendDecision> decisions;
std::function<void(int, ::sumhp::mount::sumh::ModuleMountPlan&,
                   ::sumhp::mount::sumh::ModulePlanDiagnostics&)>
    plan_hook;
Metadata guard_metadata, own_ns, init_ns, observed_guard;
int next_fd = 10, closes = 0, opens = 0, boots_read = 0, owners_read = 0, discovered = 0,
    runtime_reads = 0;
int planner_calls = 0, backend_calls = 0, query_calls = 0, mapped = 0, unmapped = 0,
    explain_calls = 0;
bool explain_changes = false;
int guard_error = 0, flock_error = 0, mmap_error = 0, namespace_error = 0, explain_error = 0,
    eintrs = 0;
bool query_fail = false, query_fail_after = false, hash_failure = false, planner_failure = false;
bool inspect_safe = true, normalize_ok = true;
std::int64_t ticks = 0, clock_step = 0;
std::string stdin_bytes;
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
int open(const std::filesystem::path::value_type* raw, int flags) {
    ++opens;
    const auto path = std::filesystem::path(raw).generic_string();
    if (path == "/private") {
        assert(flags == (O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        if (guard_error) {
            errno = guard_error;
            return -1;
        }
        return 1;
    }
    assert(flags == (O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    auto found = documents.find(path);
    if (found == documents.end()) {
        errno = ENOENT;
        return -1;
    }
    auto& index = document_index[path];
    const auto& values = found->second;
    const auto file = values[std::min(index++, values.size() - 1)];
    if (file.error) {
        errno = file.error;
        return -1;
    }
    const int fd = next_fd++;
    handles.emplace(fd, Handle{file, 0});
    return fd;
}
int dup(int fd) {
    assert(fd == 0);
    const int result = next_fd++;
    handles.emplace(result, Handle{File{stdin_bytes}, 0});
    return result;
}
int fstat(int fd, Metadata* out) {
    if (fd == 1) {
        *out = guard_metadata;
        return 0;
    }
    const auto& file = handles.at(fd).file;
    if (file.stat_error) {
        errno = file.stat_error;
        return -1;
    }
    *out = file.metadata;
    if (!file.size_override)
        out->st_size = file.bytes.size();
    return 0;
}
int read(int fd, void* buffer, std::size_t size) {
    if (eintrs) {
        --eintrs;
        errno = EINTR;
        return -1;
    }
    auto& handle = handles.at(fd);
    if (handle.file.read_error) {
        errno = handle.file.read_error;
        return -1;
    }
    const auto count = std::min(size, handle.file.bytes.size() - handle.offset);
    std::memcpy(buffer, handle.file.bytes.data() + handle.offset, count);
    handle.offset += count;
    return static_cast<int>(count);
}
int close(int fd) {
    ++closes;
    if (fd != 1)
        assert(handles.erase(fd));
    return 0;
}
int flock(int fd, int flags) {
    assert(fd == 1 && flags == (LOCK_EX | LOCK_NB));
    if (flock_error) {
        errno = flock_error;
        return -1;
    }
    return 0;
}
int stat(const std::filesystem::path::value_type* raw, Metadata* out) {
    if (namespace_error) {
        errno = namespace_error;
        return -1;
    }
    const auto path = std::filesystem::path(raw).generic_string();
    if (path == "/private")
        *out = observed_guard;
    else if (path == "/proc/self/ns/mnt")
        *out = own_ns;
    else {
        assert(path == "/proc/1/ns/mnt");
        *out = init_ns;
    }
    return 0;
}
// Android namespace paths are narrow literals even on the Windows fixture.
#ifdef _WIN32
int stat(const char* path, Metadata* out) {
    return stat(std::filesystem::path(path).c_str(), out);
}
#endif
void* mmap(void*, std::size_t size, int protection, int flags, int fd, int offset) {
    assert(protection == (PROT_READ | PROT_WRITE) && flags == (MAP_SHARED | MAP_ANONYMOUS) &&
           fd == -1 && offset == 0);
    if (mmap_error) {
        errno = mmap_error;
        return MAP_FAILED;
    }
    ++mapped;
    return ::operator new(size);
}
int munmap(void* value, std::size_t) {
    ++unmapped;
    ::operator delete(value);
    return 0;
}
}  // namespace preview_model
namespace sumhp {
std::filesystem::path runtime_data_dir() {
    return "/private";
}
std::filesystem::path runtime_config_file() {
    return "/private/config.json";
}
std::string runtime_boot_id() {
    return preview_model::boots.at(preview_model::boots_read++);
}
std::filesystem::path runtime_modules_dir() {
    return "/modules";
}
std::string embedded_external_mount_owner() {
    return preview_model::owners.at(preview_model::owners_read++);
}
namespace state {
std::string revision_for_bytes(const std::string& bytes) {
    if (preview_model::hash_failure)
        return {};
    return "test:" + std::to_string(std::hash<std::string>{}(bytes));
}
}  // namespace state
namespace checked {
constexpr std::size_t document_limit = 4 * 1024 * 1024;
}
#include "module_preview_config_under_test.inc"
namespace mount {
namespace magic {
bool inspect_module_root(const std::string& path, std::string& resolved, std::string& error) {
    if (!preview_model::inspect_safe) {
        error = "unsafe fixture root";
        return false;
    }
    resolved = path;
    return true;
}
bool prepare_normalization(const std::string& path, NormalizationPlan& out, std::string& error) {
    if (!preview_model::normalize_ok) {
        error = "normalization layout conflict";
        return false;
    }
    out.complete = true;
    out.resolved_root = path;
    out.requested_root = path;
    return true;
}
}  // namespace magic
JsonValue normalization_plan_json(const magic::NormalizationPlan& plan) {
    auto out = JsonValue::object();
    out.o["complete"] = plan.complete;
    out.o["root"] = plan.resolved_root;
    return out;
}
#include "module_preview_backend_under_test.inc"
ModuleBackendDecision resolve_module_backend_decision(const ModuleEntry& module,
                                                      const Config& config,
                                                      const ModuleModeMap& modes, bool allow_sumh) {
    assert(allow_sumh);
    ++preview_model::backend_calls;
    if (preview_model::decisions.count(module.id))
        return preview_model::decisions.at(module.id);
    ModuleBackendDecision result;
    result.has_content = true;
    result.configured_mode = modes.count(module.id) ? modes.at(module.id) : "auto";
    result.requested =
        config.mount_backend == "auto" ? result.configured_mode : config.mount_backend;
    result.selected = result.requested == "auto" ? "sumh" : result.requested;
    result.global_mode = config.mount_backend;
    result.source = config.mount_backend == "auto" ? "module" : "global";
    return result;
}
bool enumerate_mountable_modules_checked(std::vector<ModuleEntry>& result, std::string& error,
                                         std::size_t limit) {
    assert(limit == 512);
    const auto i = preview_model::discovered++;
    if (!preview_model::discovery_ok.at(i)) {
        error = "fixture discovery failure";
        errno = EIO;
        return false;
    }
    result = preview_model::discovery.at(i);
    return true;
}
namespace fsutil {
bool run_query_in_init_mount_ns(const std::function<bool()>& callback) {
    ++preview_model::query_calls;
    if (preview_model::query_fail) {
        errno = ETIMEDOUT;
        return false;
    }
    const bool result = callback();
    if (preview_model::query_fail_after) {
        errno = ECHILD;
        return false;
    }
    return result;
}
}  // namespace fsutil
namespace sumh {
bool resolve_module_plan_path(const std::string& value, std::string& out, std::string& error) {
    if (preview_model::explain_error) {
        error = "fixture alias failure";
        errno = preview_model::explain_error;
        return false;
    }
    out = value.rfind("/system/vendor", 0) == 0 ? "/vendor" + value.substr(14) : value;
    if (preview_model::explain_changes && preview_model::explain_calls)
        out += "changed";
    ++preview_model::explain_calls;
    return true;
}
bool prepare_modules_diagnostic(const std::vector<ModuleEntry>& modules, const Config&,
                                const ModuleRuleMap& rules, ModuleMountPlan& out,
                                ModulePlanDiagnostics& trace, std::string& error) {
    const int call = ++preview_model::planner_calls;
    if (preview_model::planner_failure) {
        error = "fixture compiler failure";
        trace.complete = false;
        return false;
    }
    for (const auto& module : modules) {
        const auto found = rules.find(module.id);
        if (found == rules.end() || found->second.empty())
            out.mappings.push_back({"/vendor/example", module.path.string() + "/file", false});
        else
            for (const auto& rule : found->second) {
                if (rule.mode == "hide")
                    out.hide.insert(rule.path);
                else if (rule.mode != "none")
                    out.mappings.push_back({rule.path, module.path.string() + "/file", false});
            }
        trace.observations.push_back({module.id, module.path.string(), "metadata:1"});
    }
    trace.complete = true;
    if (preview_model::plan_hook)
        preview_model::plan_hook(call, out, trace);
    return true;
}
}  // namespace sumh
}  // namespace mount
namespace sumh {
bool runtime_state(RuntimeState& out) {
    const auto i = preview_model::runtime_reads++;
    if (!preview_model::runtime_ok.at(i)) {
        errno = ENOTSUP;
        return false;
    }
    out = preview_model::runtimes.at(i);
    return true;
}
}  // namespace sumh
}  // namespace sumhp
#include "module_preview_under_test.inc"
using namespace sumhp;
using namespace sumhp::mount;
using namespace preview_model;
int input_cases = 0, controller_cases = 0;
JsonValue candidate_json() {
    auto out = JsonValue::object();
    out.o["schema_version"] = 1;
    out.o["kind"] = "sumh.module.plan.input";
    return out;
}
JsonValue replacement(const std::string& id, const std::string& mode,
                      std::vector<ModuleRule> rules = {}) {
    auto out = JsonValue::object();
    out.o["id"] = id;
    out.o["mode"] = mode;
    out.o["rules"] = module_rules_json(rules);
    return out;
}
std::string snapshot(const Config& config, const ModuleModeMap& modes, const ModuleRuleMap& rules,
                     const std::vector<std::string>& ids) {
    auto out = JsonValue::object();
    out.o["backend"] = config.mount_backend;
    out.o["overlay"] = config.overlayfs_enabled;
    out.o["magic"] = config.magic_mount_enabled;
    out.o["debug"] = config.debug;
    out.o["partitions"] = JsonValue::array();
    for (const auto& p : config.partitions)
        out.o["partitions"].a.emplace_back(p);
    out.o["modes"] = JsonValue::object();
    for (const auto& [id, mode] : modes)
        out.o["modes"].o[id] = mode;
    out.o["rules"] = JsonValue::object();
    for (const auto& [id, paths] : rules)
        out.o["rules"].o[id] = module_rules_json(paths);
    out.o["ids"] = JsonValue::array();
    for (const auto& id : ids)
        out.o["ids"].a.emplace_back(id);
    return stringify_json(out);
}
void input_tests() {
    Config config;
    config.debug = true;
    config.mount_backend = "magic";
    ModuleModeMap modes{{"saved", "overlay"}};
    ModuleRuleMap rules{{"saved", {{"/keep ", "hide"}}}};
    std::vector<std::string> ids{"old-result"};
    std::string error;
    const auto reject = [&](const std::string& bytes) {
        const auto old = snapshot(config, modes, rules, ids);
        const bool accepted = apply_module_preview_input(bytes, config, modes, rules, ids, error);
        if (accepted)
            std::cerr << "unexpected accepted input case " << input_cases << ": " << bytes << '\n';
        assert(!accepted);
        assert(!error.empty() && snapshot(config, modes, rules, ids) == old);
        ++input_cases;
    };
    const auto accept = [&](const JsonValue& input) {
        assert(apply_module_preview_input(stringify_json(input), config, modes, rules, ids, error));
        assert(error.empty());
        ++input_cases;
    };
    auto input = candidate_json();
    accept(input);
    assert(config.debug && modes.at("saved") == "overlay" && ids.empty());
    input.o["config"] = JsonValue::object();
    input.o["config"].o["mount_backend"] = "sumh";
    input.o["config"].o["overlayfs_enabled"] = false;
    input.o["config"].o["magic_mount_enabled"] = false;
    input.o["config"].o["partitions"] = JsonValue::array();
    input.o["config"].o["partitions"].a.emplace_back("my_part");
    input.o["modules"] = JsonValue::array();
    input.o["modules"].a.push_back(
        replacement("saved", "sumh", {{"/vendor/中文 文件 ", "sumh"}, {"/", "none"}}));
    accept(input);
    assert(config.mount_backend == "sumh" &&
           config.partitions == std::vector<std::string>{"my_part"});
    assert(!config.overlayfs_enabled && !config.magic_mount_enabled && config.debug);
    assert(rules.at("saved")[0].path == "/vendor/中文 文件 " &&
           ids == std::vector<std::string>{"saved"});
    for (const auto* mode : {"auto", "sumh", "magic", "overlay", "none"}) {
        input = candidate_json();
        input.o["modules"] = JsonValue::array();
        input.o["modules"].a.push_back(replacement("new", mode));
        accept(input);
        assert(modes.at("new") == mode && rules.at("new").empty() && rules.at("saved").size() == 2);
    }
    for (const auto* bytes :
         {"", "{", "[]", "null", "{\"schema_version\":1,\"kind\":\"other\"}",
          "{\"schema_version\":2,\"kind\":\"sumh.module.plan.input\"}",
          "{\"schema_version\":true,\"kind\":\"sumh.module.plan.input\"}",
          "{\"schema_version\":1.5,\"kind\":\"sumh.module.plan.input\"}",
          "{\"schema_version\":1,\"schema_version\":1,\"kind\":\"sumh.module.plan.input\"}"})
        reject(bytes);
    for (
        const auto* duplicate :
        {R"({"schema_version":1,"kind":"sumh.module.plan.input","config":{"mount_backend":"none","mount_backend":"sumh"}})",
         R"({"schema_version":1,"kind":"sumh.module.plan.input","config":{"mount_backend":"none","mount\u005fbackend":"sumh"}})",
         R"({"schema_version":1,"kind":"sumh.module.plan.input","modules":[{"id":"a","mode":"sumh","mode":"none","rules":[]}]})",
         R"({"schema_version":1,"kind":"sumh.module.plan.input","modules":[{"id":"a","mode":"sumh","rules":[{"path":"/a","path":"/b","mode":"hide"}]}]})",
         R"({"schema_version":1,"kind":"sumh.module.plan.input","modules":[{"id":"a","mode":"sumh","rules":[{"path":"/a","mode":"sumh","m\u006fde":"hide"}]}]})"})
        reject(duplicate);
    JsonValue legacy;
    assert(parse_json(R"({"mode":"old","mode":"new"})", legacy, error) &&
           legacy.at("mode").s == "new");
    ++input_cases;
    reject(std::string(MODULE_PREVIEW_INPUT_BYTES + 1, ' '));
    reject(std::string("\xc0\xaf", 2));
    input = candidate_json();
    input.o["extra"] = true;
    reject(stringify_json(input));
    for (const auto* field :
         {"enable_stealth", "debug", "work_dir", "schema_version", "maps_rules"}) {
        input = candidate_json();
        input.o["config"] = JsonValue::object();
        input.o["config"].o[field] = true;
        reject(stringify_json(input));
    }
    for (const auto* patch : {"[]", "{\"mount_backend\":1}", "{\"mount_backend\":\"hide\"}",
                              "{\"overlayfs_enabled\":\"false\"}", "{\"partitions\":[\"../x\"]}"}) {
        input = candidate_json();
        input.o["config"] = json::parse(patch);
        reject(stringify_json(input));
    }
    input = candidate_json();
    input.o["modules"] = JsonValue::object();
    reject(stringify_json(input));
    for (const auto* id : {"", ".", "..", "bad/id", "空"}) {
        input = candidate_json();
        input.o["modules"] = JsonValue::array();
        input.o["modules"].a.push_back(replacement(id, "sumh"));
        reject(stringify_json(input));
    }
    input = candidate_json();
    input.o["modules"] = JsonValue::array();
    input.o["modules"].a = {replacement("a", "sumh"), replacement("a", "none")};
    reject(stringify_json(input));
    input.o["modules"].a = {replacement("a", "hide")};
    reject(stringify_json(input));
    input.o["modules"].a = {replacement("a", "sumh")};
    input.o["modules"].a[0].o.erase("rules");
    reject(stringify_json(input));
    input.o["modules"].a = {replacement("a", "sumh")};
    input.o["modules"].a[0].o["enabled"] = true;
    reject(stringify_json(input));
    for (const auto& path : std::vector<std::string>{"relative", "/a/", "/a//b", "/a/../b",
                                                     "/a/./b", std::string("/a\0b", 4), "/a\nb"}) {
        input.o["modules"].a = {replacement("a", "sumh", {{path, "sumh"}})};
        reject(stringify_json(input));
    }
    input.o["modules"].a = {replacement("a", "sumh", {{"/a", "sumh"}, {"/a", "none"}})};
    reject(stringify_json(input));
    input.o["modules"].a = {replacement("a", "sumh", {{"/a", "invalid"}})};
    reject(stringify_json(input));
    input.o["modules"].a = {replacement("a", "sumh", {{"/a", "sumh"}})};
    input.o["modules"].a[0].o["rules"].a[0].o["inode"] = 1;
    reject(stringify_json(input));
    std::vector<ModuleRule> paths;
    for (int i = 0; i < 256; ++i)
        paths.push_back({"/p" + std::to_string(i), "sumh"});
    input.o["modules"].a = {replacement("many", "sumh", paths)};
    accept(input);
    paths.push_back({"/overflow", "sumh"});
    input.o["modules"].a = {replacement("many", "sumh", paths)};
    reject(stringify_json(input));
    paths.clear();
    for (int i = 0; i < 40; ++i)
        paths.push_back({"/" + std::to_string(i) + std::string(1500, 'x'), "sumh"});
    input.o["modules"].a = {replacement("budget", "sumh", paths)};
    assert(stringify_json(input).size() < MODULE_PREVIEW_INPUT_BYTES);
    reject(stringify_json(input));
    input = candidate_json();
    input.o["modules"] = JsonValue::array();
    for (int i = 0; i < 256; ++i)
        input.o["modules"].a.push_back(replacement("m" + std::to_string(i), "sumh"));
    accept(input);
    input.o["modules"].a.push_back(replacement("overflow", "sumh"));
    reject(stringify_json(input));
    std::string exactly = stringify_json(candidate_json());
    exactly.resize(MODULE_PREVIEW_INPUT_BYTES, ' ');
    assert(apply_module_preview_input(exactly, config, modes, rules, ids, error));
    ++input_cases;
    ModulePreviewRequest request{"/saved", "/saved-input"};
    const auto reject_args = [&](const std::vector<std::string>& args) {
        const auto old = request;
        assert(!parse_module_preview_arguments(args, request, error));
        assert(request.explain_path == old.explain_path && request.input_path == old.input_path);
        ++input_cases;
    };
    for (const auto& args : std::vector<std::vector<std::string>>{
             {},
             {"module", "explain"},
             {"module", "plan", "x"},
             {"module", "plan", "--input"},
             {"module", "plan", "--input", "relative"},
             {"module", "explain", "/a", "extra"},
             {"module", "explain", "/a/.."},
             {"module", "plan", "--input", "/a", "--input", "/b"}})
        reject_args(args);
    assert(parse_module_preview_arguments({"module", "explain", "/vendor/中文 ", "--input", "-"},
                                          request, error));
    assert(request.explain_path == "/vendor/中文 " && request.input_path == "-");
    ++input_cases;
    assert(parse_module_preview_arguments({"module", "plan"}, request, error) &&
           request.explain_path.empty() && request.input_path.empty());
    ++input_cases;
}

void set_document(const std::string& path, const std::string& bytes) {
    documents[path] = {File{bytes}};
}
void reset() {
    inspect_safe = normalize_ok = true;
    documents.clear();
    document_index.clear();
    handles.clear();
    set_document("/private/config.json", default_config_json());
    set_document("/private/module_mode.json", "{}");
    set_document("/private/module_rules.json", "{}");
    boots = {"boot-one", "boot-one"};
    owners = {"", ""};
    discovery = {{{"m", "/modules/m"}}, {{"m", "/modules/m"}}};
    discovery_ok = {true, true};
    runtimes.resize(2);
    runtimes[0] = {};
    runtimes[0].instance_id = 5;
    runtimes[0].generation = 9;
    runtimes[1] = runtimes[0];
    runtime_ok = {true, true};
    decisions.clear();
    plan_hook = {};
    guard_metadata = {};
    guard_metadata.st_mode = 0040700;
    observed_guard = guard_metadata;
    own_ns = {};
    own_ns.st_ino = 55;
    init_ns = own_ns;
    next_fd = 10;
    closes = opens = boots_read = owners_read = discovered = runtime_reads = 0;
    planner_calls = backend_calls = query_calls = mapped = unmapped = explain_calls = 0;
    explain_changes = false;
    guard_error = flock_error = mmap_error = namespace_error = explain_error = eintrs = 0;
    query_fail = query_fail_after = hash_failure = planner_failure = false;
    ticks = clock_step = 0;
    stdin_bytes.clear();
    errno = 0;
}
JsonValue preview(const std::string& expected, ModulePreviewRequest request = {}) {
    ++controller_cases;
    const auto bytes = module_preview_json(request);
    assert(bytes.size() <= MODULE_PREVIEW_RESPONSE_BYTES);
    auto out = json::parse(bytes);
    if (out.at("status").s != expected)
        std::cerr << "case " << controller_cases << " expected " << expected << ": " << bytes
                  << '\n';
    assert(out.at("status").s == expected && out.at("read_only").b &&
           out.at("apply_token").is_null());
    assert(handles.empty() && mapped == unmapped && owners_read == 0);
    return out;
}
const JsonValue& baseline(const JsonValue& out) {
    return out.at("baseline");
}
std::string exact_text(const JsonValue& value) {
    assert(value.at("encoding").s == "utf-8");
    return value.at("value").s;
}
void controller_tests() {
    reset();
    auto out = preview("complete");
    assert(planner_calls == 2 && discovered == 2 && query_calls == 1);
    assert(out.at("comparison_valid").b && baseline(out).at("ordered_contributions").a.size() == 1);
    assert(!out.at("ownership").at("hot_apply_authorized").b);
    reset();
    discovery = {{}, {}};
    out = preview("complete");
    assert(planner_calls == 0 && baseline(out).at("ordered_contributions").a.empty());
    reset();
    ModuleBackendDecision other;
    other.selected = "magic";
    other.requested = "magic";
    other.has_content = true;
    decisions["m"] = other;
    out = preview("complete");
    assert(planner_calls == 0 && baseline(out).at("other_backends_unplanned").b);
    reset();
    out = preview("complete", {"/system/vendor/example", ""});
    assert(exact_text(out.at("explain").at("resolved_path")) == "/vendor/example");
    assert(baseline(out).at("ordered_contributions").a[0].at("matches_explain").b);
    auto input = candidate_json();
    input.o["modules"] = JsonValue::array();
    input.o["modules"].a.push_back(replacement("m", "none"));
    reset();
    set_document("/input", stringify_json(input));
    out = preview("complete", {"", "/input"});
    assert(out.at("candidate").at("ordered_contributions").a.empty() &&
           !out.at("diff").at("changes").a.empty());
    assert(out.at("diff").at("complete").b && out.at("comparison_valid").b);
    assert(out.at("configuration").at("before").at("modules").a.empty());
    assert(out.at("configuration").at("after").at("modules").a[0].at("mode").s == "none");
    reset();
    stdin_bytes = stringify_json(input);
    out = preview("complete", {"", "-"});
    assert(out.at("candidate").at("compiled").b);
    // Inline candidate is exact immutable bytes, never a filename/stdin read.
    reset();
    const auto inline_bytes = stringify_json(input);
    out = preview("complete", {"/vendor/中文 ", "", inline_bytes});
    assert(out.at("candidate_revision").s == state::revision_for_bytes(inline_bytes));
    assert(out.at("explain").at("requested_path").at("value").s == "/vendor/中文 ");
    assert(document_index.count(inline_bytes) == 0 && stdin_bytes.empty());
    assert(out.at("configuration").at("after").at("modules").a[0].at("mode").s == "none");
    reset();
    out = preview("complete", {"", "", stringify_json(candidate_json())});
    assert(!out.at("candidate").is_null() &&
           out.at("configuration").at("changed_module_ids").a.empty());
    reset();
    out = preview("invalid", {"", "", std::string{}});
    assert(opens == 0 && planner_calls == 0);
    reset();
    out = preview("invalid", {"", "/input", inline_bytes});
    assert(opens == 0 && planner_calls == 0);
    reset();
    auto malformed = candidate_json();
    malformed.o["modules"] = JsonValue::array();
    malformed.o["modules"].a.push_back(replacement("m", "bogus"));
    out = preview("invalid", {"", "", stringify_json(malformed)});
    assert(planner_calls == 0);
    reset();
    boots = {"boot-a", "boot-b"};
    out = preview("stale", {"", "", inline_bytes});
    assert(out.at("diff").at("changes").a.empty());
    reset();
    input.o["modules"].a[0].o["id"] = "missing";
    set_document("/input", stringify_json(input));
    out = preview("partial", {"", "/input"});
    assert(out.at("candidate_modules_not_mountable").a.size() == 1 &&
           !out.at("comparison_valid").b);
    reset();
    set_document("/input", "{");
    assert(preview("invalid", {"", "/input"}).at("reason").s == "candidate_rejected");
    reset();
    assert(preview("invalid", {"", "/missing"}).at("reason").s == "candidate_rejected");
    reset();
    set_document("/input", std::string(MODULE_PREVIEW_INPUT_BYTES + 1, ' '));
    preview("invalid", {"", "/input"});
    reset();
    documents.erase("/private/config.json");
    assert(preview("unknown").at("reason").s == "saved_config_unavailable");
    reset();
    set_document("/private/config.json", "{");
    preview("unknown");
    reset();
    set_document("/private/config.json", "{\"schema_version\":2}");
    preview("unknown");
    reset();
    documents["/private/config.json"][0].bytes = std::string("\xff", 1);
    preview("unknown");
    reset();
    documents["/private/config.json"][0].error = EACCES;
    preview("unknown");
    reset();
    documents["/private/config.json"][0].stat_error = EIO;
    preview("unknown");
    reset();
    documents["/private/config.json"][0].read_error = EIO;
    preview("unknown");
    reset();
    documents["/private/config.json"][0].metadata.st_nlink = 2;
    preview("unknown");
    reset();
    documents["/private/config.json"][0].metadata.st_mode = 0040700;
    preview("unknown");
    reset();
    documents["/private/config.json"][0].size_override = true;
    documents["/private/config.json"][0].metadata.st_size = -1;
    preview("unknown");
    reset();
    documents["/private/config.json"][0].size_override = true;
    documents["/private/config.json"][0].metadata.st_size = 5 * 1024 * 1024;
    preview("unknown");
    reset();
    eintrs = 2;
    preview("complete");
    reset();
    clock_step = 2000;
    preview("unknown");
    reset();
    documents.erase("/private/module_mode.json");
    documents.erase("/private/module_rules.json");
    preview("complete");
    reset();
    set_document("/private/module_rules.json", "{");
    assert(preview("unknown").at("reason").s == "module_configuration_unavailable");
    reset();
    documents["/private/module_rules.json"][0].error = EACCES;
    preview("unknown");
    reset();
    set_document("/private/module_mode.json", "{\"m\":\"magic\"}");
    set_document(
        "/private/module_rules.json",
        "{\"m\":{\"mode\":\"sumh\",\"rules\":[{\"path\":\"/legacy/\",\"mode\":\"none\"}]}}");
    out = preview("complete");
    assert(baseline(out).at("backend_decisions").a[0].at("configured").s == "sumh");
    reset();
    discovery_ok[0] = false;
    assert(preview("unknown").at("reason").s == "module_discovery_failed");
    reset();
    explain_error = EIO;
    assert(preview("unknown", {"/a", ""}).at("reason").s == "explain_path_unresolved");
    reset();
    boots[0] = "";
    assert(preview("unknown").at("reason").s == "boot_unknown");
    reset();
    hash_failure = true;
    out = preview("partial");
    assert(!out.at("comparison_valid").b);
    reset();
    planner_failure = true;
    out = preview("partial");
    assert(!baseline(out).at("complete").b);
    reset();
    other = {};
    other.selected = "sumh";
    other.complete = false;
    decisions["m"] = other;
    preview("partial");
    assert(planner_calls == 0);
    reset();
    other = {};
    other.requested = "sumh";
    other.selected = "magic";
    decisions["m"] = other;
    set_document("/private/module_rules.json",
                 "{\"m\":[{\"path\":\"/hidden\",\"mode\":\"hide\"}]}");
    out = preview("partial");
    assert(!baseline(out).at("compiled").b);
    reset();
    runtime_ok = {false, false};
    out = preview("complete");
    assert(!out.at("runtime_context").at("known").b);
    reset();
    runtimes[1].generation = 10;
    preview("stale");
    reset();
    runtime_ok[1] = false;
    preview("stale");
    reset();
    boots[1] = "boot-two";
    out = preview("stale");
    assert(!out.at("comparison_valid").b);
    reset();
    owners[1] = "other-owner";
    out = preview("complete");
    assert(out.at("ownership").at("reported_external_owner").is_null());
    assert(out.at("ownership").at("external_owner_status").s == "not_observed");
    reset();
    discovery_ok[1] = false;
    preview("stale");
    reset();
    discovery[1].clear();
    preview("stale");
    reset();
    documents["/private/config.json"].push_back(File{default_config_json() + " "});
    preview("stale");
    reset();
    documents["/private/module_rules.json"].push_back(File{"{\"m\":[]}"});
    preview("stale");
    reset();
    documents["/private/run/sumh_mapping_plan"] = {File{"", ENOENT}, File{"new"}};
    preview("stale");
    reset();
    plan_hook = [](int call, auto&, auto& trace) {
        if (call == 2)
            trace.observations[0].metadata = "changed";
    };
    preview("stale");
    reset();
    plan_hook = [](int call, auto& plan, auto&) {
        if (call == 2)
            plan.mappings[0].source += "changed";
    };
    preview("stale");
    reset();
    input.o["modules"].a[0].o["id"] = "m";
    set_document("/input", stringify_json(input));
    documents["/input"].push_back(File{stringify_json(input) + " "});
    out = preview("stale", {"", "/input"});
    assert(!out.at("diff").at("complete").b && out.at("diff").at("changes").a.empty());
    reset();
    guard_error = EACCES;
    assert(preview("unknown").at("reason").s == "private_preview_guard_unavailable" &&
           query_calls == 0);
    reset();
    guard_metadata.st_uid = 1000;
    preview("unknown");
    assert(query_calls == 0);
    reset();
    guard_metadata.st_mode = 0040755;
    preview("unknown");
    assert(query_calls == 0);
    reset();
    flock_error = EBUSY;
    assert(preview("unknown").at("reason").s == "module_preview_busy" && query_calls == 0);
    reset();
    mmap_error = ENOMEM;
    preview("unknown");
    assert(query_calls == 0);
    reset();
    observed_guard.st_ino = 8;
    preview("unknown");
    assert(discovered == 0);
    reset();
    init_ns.st_ino = 7;
    preview("unknown");
    assert(discovered == 0);
    reset();
    namespace_error = EACCES;
    preview("unknown");
    assert(discovered == 0);
    reset();
    query_fail = true;
    preview("unknown");
    assert(discovered == 0);
    reset();
    query_fail_after = true;
    out = preview("unknown");
    assert(out.at("reason").s == "module_preview_query_failed" && planner_calls == 2);
    reset();
    explain_changes = true;
    preview("stale", {"/system/vendor/example", ""});
    reset();
    plan_hook = [](int, auto&, auto& trace) {
        trace.events_truncated = true;
        trace.complete = false;
    };
    out = preview("partial");
    assert(baseline(out).at("events_truncated").b && !out.at("comparison_valid").b);
    reset();
    plan_hook = [](int, auto&, auto& trace) {
        trace.observations_truncated = true;
        trace.complete = false;
    };
    out = preview("partial");
    assert(baseline(out).at("observations_truncated").b);
    reset();
    plan_hook = [](int, auto& plan, auto& trace) {
        plan.mappings[0].path = "/vendor/dir";
        plan.mappings[0].merge = true;
        plan.hide.insert("/vendor");
        ::sumhp::mount::sumh::ModulePlanEvent rule;
        rule.module_id = "m";
        rule.action = "RULE";
        rule.resolved_path = "/vendor";
        trace.events.push_back(rule);
        rule.action = "ADD";
        rule.resolved_path = "/unrelated";
        trace.events.push_back(rule);
        rule.action = "POLICY";
        trace.events.push_back(rule);
    };
    out = preview("complete", {"/vendor/dir/child", ""});
    assert(baseline(out).at("ordered_contributions").a[0].at("matches_explain").b);
    assert(baseline(out).at("ordered_contributions").a[0].at("hidden_ancestors").a.size() == 1);
    assert(baseline(out).at("explain").at("events").a.size() == 1);
    assert(baseline(out).at("explain").at("hidden_ancestors").a.size() == 1);
    reset();
    plan_hook = [](int, auto& plan, auto&) {
        plan.mappings[0].source = std::string("/source/\xff", 9);
    };
    out = preview("complete");
    assert(baseline(out).at("ordered_contributions").a[0].at("source").at("encoding").s == "hex");
    reset();
    set_document("/private/run/sumh_mapping_plan", "boot-one\nm\n");
    out = preview("complete");
    assert(out.at("ownership").at("boot_plan").at("current_boot").b);
    assert(!out.at("ownership").at("boot_plan").at("authorizes_apply").b);
    reset();
    set_document("/private/run/sumh_mapping_plan", "old-boot\nm\n");
    out = preview("complete");
    assert(!out.at("ownership").at("boot_plan").at("current_boot").b);
    reset();
    set_document("/private/run/sumh_mapping_plan", "boot-one\nm\nm\n");
    out = preview("complete");
    assert(out.at("ownership").at("boot_plan").at("status").s == "invalid");
    reset();
    documents["/private/run/sumh_mapping_plan"] = {File{"", EACCES}};
    out = preview("complete");
    assert(out.at("ownership").at("boot_plan").at("status").s == "unreadable");
    reset();
    discovery = {{{"a", "/modules/a"}, {"b", "/modules/b"}},
                 {{"a", "/modules/a"}, {"b", "/modules/b"}}};
    out = preview("complete");
    assert(exact_text(baseline(out).at("sumh_emission_module_order").a[0]) == "b");
    reset();
    owners = {std::string(MODULE_PREVIEW_RESPONSE_BYTES, 'x'),
              std::string(MODULE_PREVIEW_RESPONSE_BYTES, 'x')};
    preview("complete");  // legacy unbounded owner observer must never be invoked
    reset();
    auto large_saved = JsonValue::object();
    large_saved.o["missing"] = JsonValue::array();
    for (int i = 0; i < 600; ++i) {
        auto item = JsonValue::object();
        item.o["path"] = "/" + std::to_string(i) + std::string(4000, 'x');
        item.o["mode"] = "sumh";
        large_saved.o["missing"].a.push_back(std::move(item));
    }
    const auto large_document = stringify_json(large_saved);
    assert(large_document.size() < 4U * 1024 * 1024);
    set_document("/private/module_rules.json", large_document);
    assert(preview("unknown").at("reason").s == "module_preview_response_capacity");
}
void inspect_tests() {
    const auto inspect = [](const char* status, const std::string& path = "/staging/m") {
        auto value = module_inspect_observation(path);
        assert(value.at("status").s == status && value.at("kind").s == "sumh.module.inspect");
        assert(value.at("read_only").b && !value.at("comparison_valid").b);
        assert(value.at("complete").is_bool() && value.at("stable").is_bool() &&
               value.at("sumh_compiled").is_bool());
        assert(handles.empty() && discovered == 0 && owners_read == 0);
        ++controller_cases;
        return value;
    };
    reset();
    auto out = inspect("complete");
    assert(!out.at("installed_path").b && out.at("sumh_compiled").b);
    reset();
    set_document("/private/module_rules.json", R"({"m":[{"path":"/hidden","mode":"hide"}]})");
    out = inspect("complete", "/modules/m");
    assert(out.at("installed_path").b);
    assert(out.at("plan").at("ordered_contributions").a[0].at("action").s == "HIDE");
    reset();
    set_document("/private/module_rules.json", R"({"m":[{"path":"/hidden","mode":"hide"}]})");
    out = inspect("complete");
    assert(out.at("plan").at("ordered_contributions").a[0].at("action").s == "ADD");
    reset();
    set_document("/private/config.json", R"({"mount_backend":"magic"})");
    inspect("partial");
    assert(planner_calls == 0);
    reset();
    set_document("/private/config.json", R"({"mount_backend":"none"})");
    inspect("partial");
    assert(planner_calls == 0);
    reset();
    normalize_ok = false;
    out = inspect("complete");
    assert(!out.at("normalization").at("complete").b);
    reset();
    inspect_safe = false;
    inspect("invalid");
    assert(planner_calls == 0);
    reset();
    planner_failure = true;
    inspect("partial");
    reset();
    hash_failure = true;
    inspect("partial");
    reset();
    set_document("/private/config.json", R"({"schema_version":2})");
    inspect("unknown");
    reset();
    set_document("/private/config.json", R"({"mount_backend":"sumh","mount_backend":"none"})");
    inspect("unknown");
    reset();
    set_document("/private/config.json", R"({"unknown":1})");
    inspect("unknown");
    reset();
    documents["/private/config.json"] = {File{default_config_json()}, File{"{}"}};
    inspect("stale");
    reset();
    boots = {"a", "b"};
    inspect("stale");
    reset();
    plan_hook = [](int call, auto&, auto& trace) {
        if (call == 2)
            trace.observations[0].metadata = "changed";
    };
    inspect("stale");
    reset();
    set_document("/private/module_rules.json", "invalid");
    inspect("unknown", "/modules/m");
}
int main() {
    input_tests();
    controller_tests();
    inspect_tests();
    std::cout << input_cases << " production candidate/parser scenarios passed\n";
    std::cout << controller_cases << " production module preview controller scenarios passed\n";
}
