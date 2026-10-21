#include <algorithm>
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../common/maps_preview_transport.hpp"
#include "../../common/profile_manager_transport.hpp"
#include "../../common/utf8.hpp"
#include "../sumhp/src/core/json.hpp"
#include "cli.hpp"
#include "cli_args.hpp"
#include "core/capture_buffer.hpp"
#include "core/diagnostic_package.hpp"
#include "core/diagnostics.hpp"
#include "core/json.hpp"
#include "core/operation_manager_input.hpp"
#include "core/operation_query.hpp"
#include "core/operation_submit.hpp"
#include "core/partition_config.hpp"
#include "core/profile.hpp"
#include "core/state_ledger.hpp"
#include "core/user_hide.hpp"
#include "log.hpp"
#include "mount/development.hpp"
#include "mount/module_preview.hpp"
#include "terminal.hpp"

namespace cli_model {
int uid = 0, previews = 0, channels = 0, daemons = 0, direct = 0, logs = 0, modes = 0, signals = 0,
    debug_logs = 0;
int checks = 0, su = 0, busybox = 0, other_commands = 0, environments = 0, executions = 0,
    cases = 0;
int module_previews = 0;
int rebuild_queries = 0;
int module_channels = 0;
int capability_queries = 0;
int development_queries = 0, partition_saves = 0;
int operation_queries = 0;
int hide_queries = 0;
int profile_commands = 0;
int maps_temporary_commands = 0;
int temporary_manager_commands = 0;
int doctor_commands = 0, diagnostic_samples = 0, diagnostic_channels = 0;
int package_commands = 0;
int package_manager_commands = 0, package_channels = 0;
std::string channel_response;
int operation_prepares = 0, operation_executes = 0, submission_result = 0;
bool save_success = true;
ksud::LogLevel level = ksud::LogLevel::INFO;
bool throw_preview = false;
std::vector<std::string> received;
struct Exit {
    int code;
};
int geteuid() {
    return uid;
}
int setenv(const char* key, const char* value, int replace) {
    assert(std::string(key) == "ASH_STANDALONE" && std::string(value) == "1" && replace == 1);
    ++environments;
    return 0;
}
int execv(const char* path, char* const argv[]) {
    assert(std::string(path) == (executions ? "/system/bin/toybox" : "/data/adb/ksu/bin/busybox"));
    ++executions;
    assert(std::string(argv[0]) == "sh");
    return -1;
}
[[noreturn]] void _exit(int status) {
    throw Exit{status};
}
void reset() {
    uid = previews = channels = daemons = direct = logs = modes = signals = debug_logs = checks =
        su = busybox = other_commands = environments = executions = 0;
    received.clear();
    module_previews = module_channels = 0;
    rebuild_queries = 0;
    capability_queries = 0;
    development_queries = partition_saves = 0;
    save_success = true;
    operation_queries = 0;
    hide_queries = 0;
    profile_commands = 0;
    maps_temporary_commands = 0;
    temporary_manager_commands = 0;
    doctor_commands = diagnostic_samples = diagnostic_channels = 0;
    package_commands = 0;
    package_manager_commands = package_channels = 0;
    channel_response.clear();
    operation_prepares = operation_executes = submission_result = 0;
    level = ksud::LogLevel::INFO;
    throw_preview = false;
}
int unrelated() {
    ++other_commands;
    return 71;
}
}  // namespace cli_model
namespace ksud {
constexpr const char* VERSION_NAME = "test";
}
namespace sumhp {
namespace diagnostic_package {
std::string manager_dispatch(const diagnostic_package_manager::Request& request) {
    assert(cli_model::level == ksud::LogLevel::OFF && cli_model::package_channels == 1);
    ++cli_model::package_manager_commands;
    cli_model::received = {request.action, request.id, request.revision, request.diagnostic,
                           request.include_identifiers ? "true" : "false"};
    if (cli_model::throw_preview)
        throw std::runtime_error("package Manager dispatch fault");
    auto result = JsonValue::object();
    result["schema_version"] = 1;
    result["resource"] = "diagnostic_package_dispatch";
    result["action"] = request.action;
    result["id"] = request.id.empty() ? JsonValue{} : JsonValue(request.id);
    result["exit_code"] = cli_model::submission_result;
    result["result"] = JsonValue::object();
    return stringify_json(result);
}
int command(const std::vector<std::string>& args) {
    assert(cli_model::level == ksud::LogLevel::OFF);
    ++cli_model::package_commands;
    cli_model::received = args;
    // Exact parsing and effects have their own production controller suite.
    return cli_model::submission_result;
}
}  // namespace diagnostic_package
namespace diagnostics {
int doctor_command(const std::vector<std::string>& args) {
    assert(cli_model::level == ksud::LogLevel::OFF);
    ++cli_model::doctor_commands;
    cli_model::received = args;
    if (args.size() != 3 || args[0] != "doctor" || args[1] != "--input" || args[2].empty() ||
        (args[2] != "-" && args[2][0] != '/'))
        return 2;
    return 0;  // Root-owned bounded input/transport body has its own I/O suite.
}
std::string collect_diagnostics(const diagnostics::Request& request) {
    assert(cli_model::level == ksud::LogLevel::OFF);
    ++cli_model::diagnostic_samples;
    cli_model::received = {diagnostics::scenario_name(request.scenario), request.definition_id,
                           request.module_id, request.operation_id};
    if (cli_model::throw_preview)
        throw std::runtime_error("diagnostic exception");
    return "{\"resource\":\"diagnostic_report\"}";
}
}  // namespace diagnostics
namespace temporary_maps {
std::string dispatch(const Request& request) {
    assert(cli_model::level == ksud::LogLevel::OFF);
    ++cli_model::temporary_manager_commands;
    cli_model::received = {request.action, request.instance, request.generation,
                           request.cursor, request.ino,      request.dev};
    if (cli_model::throw_preview)
        throw std::runtime_error("temporary Maps Manager failure");
    return "{\"resource\":\"temporary_maps_dispatch\"}";
}
}  // namespace temporary_maps
namespace maps_temporary {
// is_prefix is the real inline helper included by the Manager input parser.
int command(const std::vector<std::string>& args, std::ostream&, std::ostream&) {
    assert(cli_model::level == ksud::LogLevel::OFF);
    ++cli_model::maps_temporary_commands;
    cli_model::received = args;
    if (cli_model::throw_preview)
        throw std::runtime_error("temporary Maps exception");
    return cli_model::submission_result;
}
}  // namespace maps_temporary
namespace profiles {
#include "cli_profile_arguments_under_test.inc"
#include "cli_profile_id_under_test.inc"
std::string manager_dispatch(const std::string&) {
    // The dedicated profile transport/controller suites own this route.
    assert(false);
    return {};
}
int command(const std::vector<std::string>& args) {
    assert(cli_model::level == ksud::LogLevel::OFF);
    Request request;
    std::string error;
    if (!parse_arguments(args, request, error))
        return 2;
    ++cli_model::profile_commands;
    cli_model::received = args;
    if (cli_model::throw_preview)
        throw std::runtime_error("profile boundary failure");
    return cli_model::submission_result;
}
}  // namespace profiles
namespace profile_transport {
int serve(const std::function<std::string(const std::string&)>&) {
    // An adjacent CLI case must not enter the profile Manager transport.
    assert(false);
    return 1;
}
}  // namespace profile_transport
std::string capability_snapshot_json() {
    assert(cli_model::level == ksud::LogLevel::OFF);
    ++cli_model::capability_queries;
    if (cli_model::throw_preview)
        throw std::runtime_error("capability fixture failure");
    return "{\"schema_version\":1,\"resource\":\"capability_snapshot\"}";
}
namespace hide {
#include "cli_hide_dispatch_under_test.inc"
int print_status(const Request& request) {
    assert(cli_model::level == ksud::LogLevel::OFF);
    ++cli_model::hide_queries;
    if (cli_model::throw_preview)
        throw std::runtime_error("hide status exception");
    cli_model::received = {request.revision, std::to_string(request.cursor)};
    return cli_model::submission_result;
}
int print_definitions(const Request& request) {
    return print_status(request);
}
int print_export() {
    return print_status(Request{});
}
}  // namespace hide
namespace state {
OperationResponse submit_operation_bytes(bool execute, const std::string& id,
                                         const std::string& bytes) {
    if (execute)
        ++cli_model::operation_executes;
    else
        ++cli_model::operation_prepares;
    cli_model::received = {id, bytes};
    return {cli_model::submission_result, JsonValue::object()};
}
OperationResponse query_operation(const OperationQueryRequest& request) {
    ++cli_model::operation_queries;
    cli_model::received = {request.id, std::to_string(request.timeout_ms)};
    return {cli_model::submission_result, JsonValue::object()};
}
#include "cli_operation_submit_arguments_under_test.inc"
int print_operation_submit(const OperationSubmitRequest& request) {
    assert(cli_model::level == ksud::LogLevel::OFF);
    if (request.execute)
        ++cli_model::operation_executes;
    else
        ++cli_model::operation_prepares;
    if (cli_model::throw_preview)
        throw std::runtime_error("submit exception");
    cli_model::received = {request.id, request.input_path};
    return cli_model::submission_result;
}
int print_operation_query(const OperationQueryRequest& request) {
    assert(cli_model::level == ksud::LogLevel::OFF);
    ++cli_model::operation_queries;
    if (cli_model::throw_preview)
        throw std::runtime_error("query exception");
    cli_model::received = {request.id, std::to_string(request.timeout_ms)};
    return 0;
}
}  // namespace state
namespace operation_probe {
#include "cli_operation_daemon_rejection_under_test.inc"
#include "cli_operation_envelope_under_test.inc"
}  // namespace operation_probe
#include "cli_revision_under_test.inc"
bool validate_config_patch(const json::Value&, std::string&) {
    assert(false);
    return false;
}
namespace inventory {
#include "cli_utf8_under_test.inc"
}
namespace mount {
#include "cli_development_under_test.inc"
#include "cli_module_preview_match_under_test.inc"
std::string development_query_json(const DevelopmentRequest& request) {
    assert(request.kind != DevelopmentKind::PartitionSave &&
           cli_model::level == ksud::LogLevel::OFF);
    ++cli_model::development_queries;
    return "{\"read_only\":true}";
}
std::string partition_save_json(const DevelopmentRequest& request) {
    assert(request.kind == DevelopmentKind::PartitionSave &&
           cli_model::level == ksud::LogLevel::OFF);
    ++cli_model::partition_saves;
    cli_model::received = {request.revision, request.partitions};
    return cli_model::save_success ? "{\"saved\":true}" : "{\"saved\":false}";
}
std::string module_preview_json(const ModulePreviewRequest& request) {
    ++cli_model::module_previews;
    assert(cli_model::level == ksud::LogLevel::OFF);
    if (cli_model::throw_preview)
        throw std::runtime_error("injected module preview failure");
    cli_model::received = {request.explain_path, request.input_path};
    if (request.inline_input)
        cli_model::received.push_back(*request.inline_input);
    return "{\"read_only_module_plan\":true}";
}
}  // namespace mount
std::string embedded_external_mount_owner() {
    return "test.owner";
}
std::filesystem::path runtime_data_dir() {
    return "/runtime";
}
std::filesystem::path runtime_modules_dir() {
    return "/modules";
}
std::filesystem::path runtime_config_file() {
    return "/runtime/config.json";
}
std::filesystem::path runtime_socket_file() {
    return "/runtime/socket";
}
std::filesystem::path runtime_pid_file() {
    return "/runtime/pid";
}
std::filesystem::path runtime_log_file() {
    return "/runtime/log";
}
void print_backend_statuses_json() {
    assert(false);
}
#include "cli_module_meta_under_test.inc"
namespace mount::sumh {
int print_module_rebuild_status() {
    assert(cli_model::level == ksud::LogLevel::OFF);
    ++cli_model::rebuild_queries;
    if (cli_model::throw_preview)
        throw std::runtime_error("rebuild query exception");
    return cli_model::submission_result;
}
#include "cli_preview_match_under_test.inc"
std::string maps_preview_json() {
    ++cli_model::previews;
    assert(cli_model::level == ksud::LogLevel::OFF);  // includes failing driver/ioctl paths
    if (cli_model::throw_preview)
        throw std::runtime_error("injected ioctl/preview failure");
    return "{\"read_only\":true}";
}
}  // namespace mount::sumh
namespace preview_transport {
int serve(const std::function<std::string()>& callback, Service service) {
    ++cli_model::channels;
    if (service == Service::ModulePreview)
        ++cli_model::module_channels;
    if (service == Service::Diagnostics)
        ++cli_model::diagnostic_channels;
    if (service == Service::DiagnosticPackages)
        ++cli_model::package_channels;
    cli_model::channel_response = callback();
    assert(!cli_model::channel_response.empty());
    return 0;
}
}  // namespace preview_transport
int run_command(const std::vector<std::string>& args) {
    ++cli_model::direct;
    cli_model::received = args;
    if (args == std::vector<std::string>{"api", "meta", "--control-only"})
        return print_meta_json(false);
    return 43;
}
int run_via_daemon(const std::vector<std::string>& args, bool) {
    ++cli_model::daemons;
    cli_model::received = args;
    return 44;
}
std::string arg_or_default(const std::vector<std::string>& args, std::size_t at,
                           const std::string& fallback) {
    return at < args.size() ? args[at] : fallback;
}
#include "cli_embedded_under_test.inc"
#include "cli_module_daemon_rejection_under_test.inc"
}  // namespace sumhp
namespace ksud {
void log_init(const char*) {
    ++cli_model::logs;
}
void log_set_level(LogLevel value) {
    cli_model::level = value;
}
LogLevel log_get_level() {
    return cli_model::level;
}
void log_set_cli_mode(bool) {
    ++cli_model::modes;
}
void setup_sigsys_handler() {
    ++cli_model::signals;
}
int su_main(int, char**) {
    ++cli_model::su;
    return 72;
}
int check_input_files(const CliArguments&) {
    ++cli_model::checks;
    return 0;
}
void print_version() {
    ++cli_model::other_commands;
}
int on_post_fs_data() {
    return cli_model::unrelated();
}
void on_services() {
    (void)cli_model::unrelated();
}
void on_boot_completed() {
    (void)cli_model::unrelated();
}
int run_sulogd() {
    return cli_model::unrelated();
}
template <class... T>
int install(T&&...) {
    return cli_model::unrelated();
}
template <class... T>
int uninstall(T&&...) {
    return cli_model::unrelated();
}
#define STUB(name)                     \
    template <class... T>              \
    int name(T&&...) {                 \
        return cli_model::unrelated(); \
    }
STUB(cmd_insmod);
STUB(cmd_module);
STUB(cmd_sepolicy);
STUB(cmd_profile);
STUB(cmd_feature);
STUB(su_path_command);
STUB(cmd_initrc);
STUB(boot_patch);
STUB(boot_patch_v2);
STUB(boot_restore);
STUB(cmd_boot_info);
STUB(cmd_ramdisk_editor);
STUB(cmd_umount);
STUB(cmd_kernel);
STUB(cmd_debug);
STUB(cmd_flash_new);
#undef STUB
#undef LOGD
#define LOGD(...) (++cli_model::debug_logs)
#include "cli_run_under_test.inc"
#undef LOGD
}  // namespace ksud
extern "C" int busybox_main(int, char**) {
    ++cli_model::busybox;
    return 73;
}
#define NDK_BUSYBOX_AVAILABLE 1
#include "main_under_test.inc"

int invoke(std::vector<std::string> arguments, bool outer = true) {
    ++cli_model::cases;
    std::vector<char*> argv;
    for (auto& item : arguments)
        argv.push_back(item.data());
    argv.push_back(nullptr);
    return outer ? production_main(static_cast<int>(arguments.size()), argv.data())
                 : ksud::cli_run(static_cast<int>(arguments.size()), argv.data());
}
void no_normal_setup() {
    using namespace cli_model;
    assert(logs == 0 && modes == 0 && signals == 0 && debug_logs == 0 && checks == 0 &&
           daemons == 0 && direct == 0 && other_commands == 0);
    assert(level == ksud::LogLevel::INFO);
}
int main() {
    using namespace cli_model;
    std::ostringstream output, errors;
    auto* old_out = std::cout.rdbuf(output.rdbuf());
    auto* old_err = std::cerr.rdbuf(errors.rdbuf());
    const std::string diagnostic = R"({"schema_version":1,"scenario":"boot-protection"})";
    for (const auto& tail : std::vector<std::vector<std::string>>{
             {"list"},
             {"preview", "0123456789abcdef0123456789abcdef"},
             {"create", "0123456789abcdef0123456789abcdef", "--input", "-"},
             {"export", "0123456789abcdef0123456789abcdef", "--revision",
              "sha256-v1:" + std::string(64, 'a')},
             {"remove", "0123456789abcdef0123456789abcdef", "--revision",
              "sha256-v1:" + std::string(64, 'a')},
             {},
             {"unknown", "--channel"}}) {
        auto args = std::vector<std::string>{"ksud", "sumhp", "doctor", "package"};
        args.insert(args.end(), tail.begin(), tail.end());
        reset();
        submission_result = 7;
        assert(invoke(args) == 7 && package_commands == 1 && !doctor_commands &&
               !diagnostic_samples && !channels);
        assert(received == std::vector<std::string>(args.begin() + 2, args.end()));
        no_normal_setup();
        reset();
        uid = 1000;
        assert(invoke(args) != 0 && !package_commands && !doctor_commands && !diagnostic_samples &&
               !channels);
        no_normal_setup();
    }
    for (
        const auto& request : std::vector<std::string>{
            diagnostic, R"({"schema_version":1,"scenario":"maps-failure"})",
            R"({"schema_version":1,"scenario":"module-fallback","module_id":"example.module"})",
            R"({"schema_version":1,"scenario":"rule-not-effective","definition_id":"0123456789abcdef0123456789abcdef"})"}) {
        reset();
        assert(invoke({"ksud", "sumhp", "doctor", "manager", request, "--channel"}) == 0);
        assert(diagnostic_samples == 1 && diagnostic_channels == 1 && doctor_commands == 0);
        no_normal_setup();
    }
    const std::string package_id(32, 'a'), package_revision = "sha256-v1:" + std::string(64, 'b');
    for (const auto* action : {"capabilities", "list", "preview", "export", "remove", "create"}) {
        auto request = sumhp::JsonValue::object();
        request["schema_version"] = 1;
        request["action"] = action;
        const bool has_id = std::string(action) != "capabilities" && std::string(action) != "list";
        if (has_id)
            request["id"] = package_id;
        const bool reviewed = std::string(action) == "export" || std::string(action) == "remove";
        if (reviewed)
            request["revision"] = package_revision;
        const bool create = std::string(action) == "create";
        if (create) {
            request["include_identifiers"] = true;
            request["diagnostic"] = json::parse(diagnostic);
        }
        const auto wire = sumhp::stringify_json(request);
        for (const auto* executable :
             {"ksud", "/data/adb/ksud", "/data/app/example/lib/arm64/libksud.so"}) {
            const std::vector<std::string> args{executable, "sumhp", "doctor",   "package",
                                                "manager",  wire,    "--channel"};
            reset();
            submission_result = 5;
            assert(invoke(args) == 0 && package_manager_commands == 1 && package_channels == 1 &&
                   channels == 1);
            assert(!package_commands && !doctor_commands && !diagnostic_samples &&
                   !diagnostic_channels);
            no_normal_setup();
            assert(received[0] == action && received[1] == (has_id ? package_id : "") &&
                   received[2] == (reviewed ? package_revision : "") &&
                   received[4] == (create ? "true" : "false"));
            if (create)
                assert(json::parse(received[3]).at("scenario").s == "boot-protection");
            else
                assert(received[3].empty());
            const auto dispatch = json::parse(channel_response);
            assert(dispatch.at("action").s == action && dispatch.at("exit_code").n == 5 &&
                   dispatch.at("resource").s == "diagnostic_package_dispatch");
            assert(has_id ? dispatch.at("id").s == package_id : dispatch.at("id").is_null());
            reset();
            uid = 1000;
            assert(invoke(args) != 0 && !package_manager_commands && !package_commands &&
                   !channels);
            no_normal_setup();
        }
        for (const auto& prefix : std::vector<std::vector<std::string>>{
                 {"ksud", "--verbose", "sumhp", "doctor"},
                 {"ksud", "--log-file", "/tmp/must-not-write", "sumhp", "doctor"},
                 {"ksud", "sumhp", "daemon", "call", "doctor"},
                 {"ksud", "sumhp", "daemon", "call", "daemon", "call", "doctor"}}) {
            auto args = prefix;
            args.insert(args.end(), {"package", "manager", wire, "--channel"});
            reset();
            assert(invoke(args) == 2 && !package_manager_commands && !package_commands &&
                   !channels && !daemons && !direct && !debug_logs);
        }
        reset();
        assert(sumhp::operation_probe::run_command(
                   {"doctor", "package", "manager", wire, "--channel"}) == 2 &&
               !package_manager_commands && !package_commands && !channels && !daemons);
        no_normal_setup();
    }
    const std::string package_list = R"({"schema_version":1,"action":"list"})";
    for (const auto& tail : std::vector<std::vector<std::string>>{
             {},
             {package_list},
             {package_list, "--channel", "extra"},
             {package_list, "--channel=3"},
             {"{}", "--channel"},
             {R"({"schema_version":1,"action":"list","action":"list"})", "--channel"},
             {R"({"schema_version":1,"action":"list","path":"/x"})", "--channel"},
             {R"({"schema_version":2,"action":"list"})", "--channel"},
             {R"({"schema_version":1,"action":"create"})", "--channel"},
             {std::string(8193, ' '), "--channel"}}) {
        reset();
        auto args = std::vector<std::string>{"ksud", "sumhp", "doctor", "package", "manager"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(invoke(args) == 2 && !package_manager_commands && !package_commands &&
               !doctor_commands && !diagnostic_samples && !channels);
        no_normal_setup();
    }
    reset();
    throw_preview = true;
    assert(invoke({"ksud", "sumhp", "doctor", "package", "manager", package_list, "--channel"}) !=
               0 &&
           package_manager_commands == 1 && package_channels == 1 && !package_commands &&
           !doctor_commands);
    no_normal_setup();
    // The actual production main has BusyBox enabled. The transport fixture
    // separately asserts executable=/proc/self/exe and argv[0]=ksud.
    reset();
    assert(invoke({"/proc/self/exe", "sumhp", "doctor", "manager", diagnostic, "--channel"}) == 73);
    assert(busybox == 1 && diagnostic_samples == 0 && doctor_commands == 0);
    no_normal_setup();
    for (const auto& input : std::vector<std::string>{"/absolute/request.json", "-"}) {
        reset();
        assert(invoke({"ksud", "sumhp", "doctor", "--input", input}) == 0);
        assert(doctor_commands == 1 && diagnostic_samples == 0 && channels == 0);
        no_normal_setup();
    }
    for (const auto& tail : std::vector<std::vector<std::string>>{
             {},
             {"--help"},
             {"--input"},
             {"--input", "relative"},
             {"manager", diagnostic},
             {"manager", diagnostic, "--channel", "extra"},
             {"manager", "{}", "--channel"},
             {"manager", R"({"schema_version":1,"scenario":"maps-failure","path":"/x"})",
              "--channel"},
             {"manager",
              R"({"schema_version":1,"scenario":"boot-protection","scenario":"maps-failure"})",
              "--channel"},
             {"manager", diagnostic, "--channel=3"},
             {"--channel"},
             {"--input", "-", "--channel"}}) {
        reset();
        std::vector<std::string> args{"ksud", "sumhp", "doctor"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(invoke(args) == 2 && doctor_commands == 1 && diagnostic_samples == 0 &&
               channels == 0);
        no_normal_setup();
    }
    for (bool private_channel : {false, true}) {
        reset();
        uid = 1000;
        assert(invoke(private_channel ? std::vector<std::string>{"ksud", "sumhp", "doctor",
                                                                 "manager", diagnostic, "--channel"}
                                      : std::vector<std::string>{"ksud", "sumhp", "doctor",
                                                                 "--input", "-"}) != 0);
        assert(doctor_commands == 0 && diagnostic_samples == 0 && channels == 0);
        no_normal_setup();
    }
    reset();
    throw_preview = true;
    assert(invoke({"ksud", "sumhp", "doctor", "manager", diagnostic, "--channel"}) != 0 &&
           diagnostic_samples == 1 && channels == 1);
    no_normal_setup();
    for (const auto& tail : std::vector<std::vector<std::string>>{
             {"--input", "-"},
             {"--input", "/absolute/request.json"},
             {"package", "list"},
             {"package", "create", "0123456789abcdef0123456789abcdef", "--input", "-"},
             {"package", "preview", "0123456789abcdef0123456789abcdef"},
             {"package", "remove", "0123456789abcdef0123456789abcdef", "--revision",
              "sha256-v1:" + std::string(64, 'a')},
             {"manager", diagnostic, "--channel"},
             {"manager", "{}", "--channel"},
             {}}) {
        for (const auto& prefix : std::vector<std::vector<std::string>>{
                 {"ksud", "--verbose", "sumhp", "doctor"},
                 {"ksud", "--log-file", "/tmp/doctor-must-not-write", "sumhp", "doctor"},
                 {"ksud", "sumhp", "--verbose", "doctor"},
                 {"ksud", "sumhp", "daemon", "call", "doctor"},
                 {"ksud", "sumhp", "daemon", "call", "daemon", "call", "doctor"}}) {
            reset();
            auto args = prefix;
            args.insert(args.end(), tail.begin(), tail.end());
            assert(invoke(args) == 2 && !package_commands && !doctor_commands &&
                   !diagnostic_samples && !channels && !daemons && !direct);
            // Generic rejection retains ordinary in-memory logger/signal setup,
            // but stops before verbose dispatch, input reads or daemon startup.
            assert(logs == 1 && modes == 1 && signals == 1 && !debug_logs && !checks &&
                   !other_commands);
        }
        reset();
        std::vector<std::string> args{"doctor"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(sumhp::operation_probe::run_command(args) == 2 && !doctor_commands &&
               !diagnostic_samples && !channels && !daemons);
        no_normal_setup();
        ksud::CliArguments doctor_parsed;
        args.insert(args.begin(), "sumhp");
        assert(ksud::parse_cli(args, doctor_parsed) == 2);
    }
    const int diagnostic_cases = cases;
    for (const auto* executable :
         {"ksud", "/data/adb/ksud", "/data/app/example/lib/arm64/libksud.so"}) {
        reset();
        assert(invoke({executable, "sumhp", "sumh", "maps", "preview"}) == 0);
        assert(previews == 1 && channels == 0);
        no_normal_setup();
        reset();
        assert(invoke({executable, "sumhp", "sumh", "maps", "preview", "--channel"}) == 0);
        assert(previews == 1 && channels == 1);
        no_normal_setup();
    }
    for (const auto& suffix : std::vector<std::vector<std::string>>{{"extra"},
                                                                    {"--help"},
                                                                    {"--channel", "extra"},
                                                                    {"--channel", "--channel"},
                                                                    {"--channel=1"},
                                                                    {"--"}}) {
        reset();
        std::vector<std::string> args = {"ksud", "sumhp", "sumh", "maps", "preview"};
        args.insert(args.end(), suffix.begin(), suffix.end());
        assert(invoke(args) == 1 && previews == 0 && channels == 0);
        no_normal_setup();
    }
    for (bool channel : {false, true}) {
        reset();
        uid = 1000;
        std::vector<std::string> args = {"ksud", "sumhp", "sumh", "maps", "preview"};
        if (channel)
            args.push_back("--channel");
        assert(invoke(args) != 0 && previews == 0 && channels == 0);
        no_normal_setup();
    }
    reset();
    throw_preview = true;
    assert(invoke({"ksud", "sumhp", "sumh", "maps", "preview"}) != 0 && previews == 1);
    no_normal_setup();
    for (const auto* executable : {"su", "/system/bin/su"}) {
        reset();
        assert(invoke({executable, "sumhp", "sumh", "maps", "preview", "--channel"}) == 72);
        assert(su == 1 && previews == 0 && channels == 0 && logs == 1 && signals == 1);
    }
    reset();
    assert(invoke({"/system/bin/sh", "sumhp", "sumh", "maps", "preview", "--channel"}) == 73);
    assert(busybox == 1 && previews == 0 && logs == 0);  // production main applet behavior
    reset();
    try {
        (void)invoke({"/system/bin/sh", "sumhp", "sumh", "maps", "preview"}, false);
        assert(false);
    } catch (const Exit& error) {
        assert(error.code == 127);
    }
    assert(executions == 2 && environments == 1 && previews == 0 && channels == 0 && logs == 1 &&
           signals == 1);
    for (const auto& args :
         std::vector<std::vector<std::string>>{{"ksud", "sumhp", "sumh", "maps", "apply"},
                                               {"ksud", "sumhp", "sumh", "maps", "list-paths"},
                                               {"ksud", "sumhp", "config", "show"}}) {
        reset();
        assert(invoke(args) == 44 && daemons == 1 && previews == 0 && logs == 1 && signals == 1 &&
               checks == 1 && debug_logs == 1);
    }
    reset();
    assert(invoke({"ksud", "sumhp", "daemon", "status"}) == 43 && direct == 1 && daemons == 0 &&
           previews == 0);
    reset();
    assert(invoke({"ksud", "version"}) == 0 && other_commands == 1 && previews == 0);
    reset();
    assert(invoke({"ksud", "busybox", "sumhp", "sumh", "maps", "preview"}) == 73 && busybox == 1 &&
           previews == 0);
    reset();
    assert(invoke({"ksud", "sumhp", "sumh", "maps", "preview-typo"}) == 2 && daemons == 0 &&
           previews == 0);
    // Alternative generic routes must not accidentally start a restoring
    // daemon or enable verbose logging for this newly introduced query.
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"ksud", "--verbose", "sumhp", "sumh", "maps", "preview"},
             {"ksud", "sumhp", "--verbose", "sumh", "maps", "preview"},
             {"ksud", "sumhp", "daemon", "call", "sumh", "maps", "preview"}}) {
        reset();
        assert(invoke(args) == 2 && daemons == 0 && direct == 0 && previews == 0 && channels == 0 &&
               debug_logs == 0);
    }
    ksud::CliArguments parsed;
    assert(ksud::parse_cli({"sumhp", "sumh", "maps", "preview"}, parsed) == -1);
    assert(parsed.args == std::vector<std::string>({"sumh", "maps", "preview"}));
    assert(ksud::parse_cli({"sumhp", "sumh", "maps", "preview", "--channel"}, parsed) == 2);
    const int maps_cases = cases;
    for (const auto* executable :
         {"ksud", "/data/adb/ksud", "/data/app/example/lib/arm64/libksud.so"}) {
        for (const auto& tail : std::vector<std::vector<std::string>>{
                 {"plan"},
                 {"plan", "--input", "-"},
                 {"plan", "--input", "/data/local/tmp/plan.json"},
                 {"explain", "/vendor/etc/example ", "--input", "/data/local/tmp/input path.json"},
                 {"explain", "/"},
                 {"explain", u8"/system/中文 "}}) {
            reset();
            std::vector<std::string> args = {executable, "sumhp", "module"};
            args.insert(args.end(), tail.begin(), tail.end());
            assert(invoke(args) == 0 && module_previews == 1 && previews == 0);
            no_normal_setup();
            if (tail[0] == "explain")
                assert(received[0] == tail[1]);
        }
    }
    for (const auto& tail :
         std::vector<std::vector<std::string>>{{"plan", "extra"},
                                               {"plan", "--help"},
                                               {"plan", "--input"},
                                               {"plan", "--input", "relative"},
                                               {"plan", "--input", "-", "extra"},
                                               {"plan", "--channel"},
                                               {"plan", "--input=/file"},
                                               {"explain"},
                                               {"explain", "relative"},
                                               {"explain", "/a//b"},
                                               {"explain", "/a/../b"},
                                               {"explain", "/x", "--input"},
                                               {"explain", "/x", "--input", "/a\nb"},
                                               {"explain", std::string("/bad\xff", 5)}}) {
        reset();
        std::vector<std::string> args = {"ksud", "sumhp", "module"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(invoke(args) == 1 && module_previews == 0 && previews == 0);
        no_normal_setup();
    }
    for (const auto& tail : std::vector<std::vector<std::string>>{{"plan"}, {"explain", "/file"}}) {
        reset();
        uid = 1000;
        std::vector<std::string> args = {"ksud", "sumhp", "module"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(invoke(args) != 0 && module_previews == 0);
        no_normal_setup();
        assert(sumhp::handle_module({"module", tail[0]}) ==
               1);  // daemon body rejects without preview
    }
    reset();
    throw_preview = true;
    assert(invoke({"ksud", "sumhp", "module", "plan"}) != 0 && module_previews == 1);
    no_normal_setup();
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"ksud", "--verbose", "sumhp", "module", "plan"},
             {"ksud", "sumhp", "--verbose", "module", "plan"},
             {"ksud", "sumhp", "daemon", "call", "module", "plan"},
             {"ksud", "sumhp", "daemon", "call", "module", "explain", "/x"}}) {
        reset();
        assert(invoke(args) == 2 && daemons == 0 && direct == 0 && module_previews == 0 &&
               debug_logs == 0);
    }
    reset();
    assert(invoke({"su", "sumhp", "module", "plan"}) == 72 && su == 1 && module_previews == 0);
    reset();
    assert(invoke({"sh", "sumhp", "module", "plan"}) == 73 && busybox == 1 && module_previews == 0);
    reset();
    assert(invoke({"ksud", "sumhp", "module", "list"}) == 44 && daemons == 1 &&
           module_previews == 0);
    reset();
    assert(invoke({"ksud", "sumhp", "module", "plan-typo"}) == 2 && daemons == 0 &&
           module_previews == 0);
    assert(ksud::parse_cli({"sumhp", "module", "plan"}, parsed) == 2);
    assert(ksud::parse_cli({"sumhp", "module", "explain", "/x"}, parsed) == 2);
    assert(ksud::parse_cli({"sumhp", "module", "--help"}, parsed) == 0);
    output.str("");
    output.clear();
    assert(sumhp::print_meta_json(false) == 0);
    sumhp::JsonValue meta;
    std::string meta_error;
    assert(sumhp::parse_json(output.str(), meta, meta_error));
    assert(meta.at("diagnostic_package_schema").n == 1 && meta.at("diagnostic_package_manager").b &&
           meta.at("diagnostic_package_manager_schema").n == 1);
    for (const auto* feature : {"diagnostic_package_create", "diagnostic_package_preview",
                                "diagnostic_package_export", "diagnostic_package_remove"})
        assert(meta.at(feature).b);
    assert(meta.at("module_plan").b && meta.at("module_explain").b);
    assert(meta.at("module_plan_schema").n == 1 && meta.at("module_inspect").b &&
           !meta.at("module_apply").b);
    assert(meta.at("module_normalize_preview").b && meta.at("partition_discovery").b &&
           meta.at("partition_save_only").b);
    assert(meta.at("module_plan_input_bytes").n == 64 * 1024);
    assert(meta.at("module_plan_response_bytes").n == 2 * 1024 * 1024 &&
           meta.at("backends").a.empty());
    const int previous_cases = cases;
    const std::string revision = "sha256-v1:" + std::string(64, 'a');
    for (const auto& tail : std::vector<std::vector<std::string>>{
             {"module", "inspect", "/staging/module"},
             {"module", "normalize", "/staging/module", "--dry-run"},
             {"config", "discover-partitions"},
             {"config", "discover-partitions", "--channel"},
             {"config", "sync-partitions"}}) {
        reset();
        std::vector<std::string> args = {"ksud", "sumhp"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(invoke(args) == 0 && development_queries == 1 && partition_saves == 0);
        no_normal_setup();
        reset();
        uid = 1000;
        assert(invoke(args) != 0 && development_queries == 0);
        no_normal_setup();
        args.insert(args.begin() + 1, "--verbose");
        reset();
        assert(invoke(args) == 2 && daemons == 0 && development_queries == 0);
    }
    for (bool channel : {false, true}) {
        reset();
        std::vector<std::string> args = {"ksud",           "sumhp",  "config",
                                         "set-partitions", revision, "[\"my_part\"]"};
        if (channel)
            args.push_back("--channel");
        assert(invoke(args) == 0 && partition_saves == 1 && channels == (channel ? 1 : 0));
        no_normal_setup();
        assert(received == std::vector<std::string>({revision, "[\"my_part\"]"}));
        reset();
        save_success = false;
        assert(invoke(args) == (channel ? 0 : 1) && partition_saves == 1);
        no_normal_setup();
        reset();
        uid = 1000;
        assert(invoke(args) != 0 && partition_saves == 0);
        no_normal_setup();
    }
    for (const auto& tail : std::vector<std::vector<std::string>>{
             {"module", "inspect"},
             {"module", "inspect", "relative"},
             {"module", "inspect", "/x", "--channel"},
             {"module", "normalize", "/x", "--dry-run", "extra"},
             {"module", "normalize", "--dry-run"},
             {"config", "discover-partitions", "extra"},
             {"config", "sync-partitions", "--channel"},
             {"config", "set-partitions"},
             {"config", "set-partitions", "bad", "[]"},
             {"config", "set-partitions", revision, "[]", "extra"}}) {
        reset();
        std::vector<std::string> args = {"ksud", "sumhp"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(invoke(args) == 1 && development_queries == 0 && partition_saves == 0);
        no_normal_setup();
    }
    for (const auto& tail :
         std::vector<std::vector<std::string>>{{"module", "inspect", "/x"},
                                               {"module", "normalize", "/x", "--dry-run"},
                                               {"config", "discover-partitions"},
                                               {"config", "set-partitions", revision, "[]"}}) {
        reset();
        std::vector<std::string> args = {"ksud", "sumhp", "daemon", "call"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(invoke(args) == 2 && daemons == 0 && development_queries == 0 &&
               partition_saves == 0);
    }
    const int operation_start = cases;
    const std::string operation_id(32, 'a');
    for (const auto* executable :
         {"ksud", "/data/adb/ksud", "/data/app/example/lib/arm64/libksud.so"}) {
        for (const auto& tail :
             std::vector<std::vector<std::string>>{{"list"},
                                                   {"show", operation_id},
                                                   {"wait", operation_id, "0"},
                                                   {"wait", operation_id, "10000"}}) {
            reset();
            std::vector<std::string> args = {executable, "sumhp", "operation"};
            args.insert(args.end(), tail.begin(), tail.end());
            assert(invoke(args) == 0 && operation_queries == 1);
            no_normal_setup();
            if (tail[0] != "list")
                assert(received[0] == operation_id);
            reset();
            uid = 1000;
            assert(invoke(args) != 0 && operation_queries == 0);
            no_normal_setup();
        }
    }
    for (const auto& tail :
         std::vector<std::vector<std::string>>{{},
                                               {"typo"},
                                               {"list", "extra"},
                                               {"list", "--help"},
                                               {"list", "--channel"},
                                               {"show"},
                                               {"show", ""},
                                               {"show", std::string(32, 'A')},
                                               {"show", std::string(33, 'a')},
                                               {"show", operation_id, "extra"},
                                               {"wait", operation_id},
                                               {"wait", operation_id, "10001"},
                                               {"wait", operation_id, "01"},
                                               {"wait", operation_id, "-1"},
                                               {"wait", operation_id, "+1"},
                                               {"wait", operation_id, " 1"},
                                               {"wait", operation_id, "1 "},
                                               {"wait", operation_id, "1.0"},
                                               {"wait", operation_id, "99999999999999999999"},
                                               {"wait", operation_id, "1", "extra"}}) {
        reset();
        std::vector<std::string> args = {"ksud", "sumhp", "operation"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(invoke(args) == 2 && operation_queries == 0);
        no_normal_setup();
    }
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"ksud", "--verbose", "sumhp", "operation", "list"},
             {"ksud", "sumhp", "--verbose", "operation", "show", operation_id},
             {"ksud", "sumhp", "daemon", "call", "operation", "wait", operation_id, "1"},
             {"ksud", "sumhp", "daemon", "call", "daemon", "call", "operation", "list"}}) {
        reset();
        assert(invoke(args) == 2 && operation_queries == 0 && daemons == 0 && debug_logs == 0);
    }
    reset();
    throw_preview = true;
    assert(invoke({"ksud", "sumhp", "operation", "list"}) != 0 && operation_queries == 1);
    no_normal_setup();
    reset();
    assert(invoke({"su", "sumhp", "operation", "list"}) == 72 && su == 1 && operation_queries == 0);
    reset();
    assert(invoke({"sh", "sumhp", "operation", "list"}) == 73 && busybox == 1 &&
           operation_queries == 0);
    assert(sumhp::operation_probe::run_command({"operation", "list"}) == 2);
    auto envelope = json::parse(sumhp::operation_probe::response_json(
        true, 0, 0, "old stdout", "old stderr", operation_id, true));
    assert(envelope.at("operation_id").s == operation_id && envelope.at("operation_recorded").b);
    assert(envelope.at("stdout").s == "old stdout" && envelope.at("stderr").s == "old stderr");
    envelope = json::parse(
        sumhp::operation_probe::response_json(false, 1, 5, "", "", operation_id, false));
    assert(envelope.at("operation_id").s == operation_id && !envelope.at("operation_recorded").b);
    envelope = json::parse(sumhp::operation_probe::response_json(true, 0, 0, "", ""));
    assert(!envelope.at("operation_id").is_string() && !envelope.at("operation_recorded").b);
    assert(meta.at("operation_query_schema").n == 2 && meta.at("operation_list").b &&
           meta.at("operation_show").b);
    assert(meta.at("operation_wait").b && meta.at("operation_wait_max_ms").n == 10000);
    assert(meta.at("state_ledger_schema").n == 1 && meta.at("state_ledger_storage_schema").n == 3);
    assert(meta.at("operation_history_limit").n == 16 && !meta.at("operation_caller_id").b);
    assert(meta.at("operation_submission_schema").n == 1 && meta.at("operation_prepare").b &&
           meta.at("operation_execute").b);
    assert(meta.at("operation_reserved_id").b && meta.at("operation_input_bytes").n == 48 * 1024 &&
           meta.at("operation_output_stream_bytes").n == 64 * 1024);
    const int submission_start = cases;
    for (const auto* executable :
         {"ksud", "/data/adb/ksud", "/data/app/example/lib/arm64/libksud.so"}) {
        for (bool execute : {false, true}) {
            for (const auto& input : std::vector<std::string>{
                     "-", "/data/local/tmp/规则 input.json ", "/" + std::string(4094, 'a')}) {
                std::vector<std::string> args = {executable, "sumhp", "operation",
                                                 execute ? "execute" : "prepare"};
                if (execute)
                    args.push_back(operation_id);
                args.insert(args.end(), {"--input", input});
                reset();
                assert(invoke(args) == 0 && operation_queries == 0);
                assert(operation_prepares == !execute && operation_executes == execute);
                assert(received == std::vector<std::string>({execute ? operation_id : "", input}));
                no_normal_setup();
                reset();
                uid = 1000;
                assert(invoke(args) != 0 && operation_prepares == 0 && operation_executes == 0);
                no_normal_setup();
                reset();
                submission_result = 17;
                assert(invoke(args) == 17);
                no_normal_setup();
            }
        }
    }
    for (const auto& tail : std::vector<std::vector<std::string>>{
             {"prepare"},
             {"prepare", "--input"},
             {"prepare", "--input", ""},
             {"prepare", "--input", "relative.json"},
             {"prepare", "--input", "/input\n.json"},
             {"prepare", "--input", "/input\r.json"},
             {"prepare", "--input", std::string("/bad") + char(0xff)},
             {"prepare", "--input", "/" + std::string(4095, 'a')},
             {"prepare", "-i", "-"},
             {"prepare", "--input=-"},
             {"prepare", "--input", "-", "extra"},
             {"prepare", "--input", "-", "--input", "/other"},
             {"prepare", "--", "--input", "-"},
             {"execute"},
             {"execute", operation_id},
             {"execute", operation_id, "--input"},
             {"execute", "", "--input", "-"},
             {"execute", std::string(31, 'a'), "--input", "-"},
             {"execute", std::string(33, 'a'), "--input", "-"},
             {"execute", std::string(32, 'A'), "--input", "-"},
             {"execute", std::string(32, 'g'), "--input", "-"},
             {"execute", operation_id, "--input", "relative.json"},
             {"execute", operation_id, "--input", "-", "extra"},
             {"execute", operation_id, "--input=-"},
             {"execute", "--input", "-", operation_id},
             {"execute", operation_id, "--help"}}) {
        reset();
        std::vector<std::string> args = {"ksud", "sumhp", "operation"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(invoke(args) == 2 && operation_prepares == 0 && operation_executes == 0 &&
               operation_queries == 0);
        no_normal_setup();
    }
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"ksud", "--verbose", "sumhp", "operation", "prepare", "--input", "-"},
             {"ksud", "sumhp", "--verbose", "operation", "execute", operation_id, "--input", "-"},
             {"ksud", "sumhp", "daemon", "call", "operation", "prepare", "--input", "-"},
             {"ksud", "sumhp", "daemon", "call", "daemon", "call", "operation", "execute",
              operation_id, "--input", "-"}}) {
        reset();
        assert(invoke(args) == 2 && operation_prepares == 0 && operation_executes == 0 &&
               daemons == 0 && direct == 0 && debug_logs == 0);
    }
    for (bool execute : {false, true}) {
        std::vector<std::string> args = {"ksud", "sumhp", "operation",
                                         execute ? "execute" : "prepare"};
        if (execute)
            args.push_back(operation_id);
        args.insert(args.end(), {"--input", "-"});
        reset();
        throw_preview = true;
        assert(invoke(args) != 0);
        assert(operation_prepares == !execute && operation_executes == execute);
        no_normal_setup();
        reset();
        args[0] = "su";
        assert(invoke(args) == 72 && su == 1 && operation_prepares == 0 && operation_executes == 0);
        reset();
        args[0] = "sh";
        assert(invoke(args) == 73 && busybox == 1 && operation_prepares == 0 &&
               operation_executes == 0);
        assert(sumhp::operation_probe::run_command(
                   {"operation", execute ? "execute" : "prepare"}) == 2);
    }
    // argv cannot represent NUL. Exercise the same production parser directly
    // and ensure failed validation leaves the caller's request untouched.
    sumhp::state::OperationSubmitRequest unchanged{true, operation_id, "/original"};
    std::string submit_error;
    assert(!sumhp::state::parse_operation_submit_arguments(
        {"operation", "prepare", "--input", std::string("/x\0y", 4)}, unchanged, submit_error));
    assert(unchanged.execute && unchanged.id == operation_id &&
           unchanged.input_path == "/original" && !submit_error.empty());
    const int hide_start = cases;
    for (const auto* executable :
         {"ksud", "/data/adb/ksud", "/data/app/example/lib/arm64/libksud.so"}) {
        for (const auto& tail :
             std::vector<std::vector<std::string>>{{"status"}, {"status", revision, "16"}}) {
            std::vector<std::string> args = {executable, "sumhp", "hide"};
            args.insert(args.end(), tail.begin(), tail.end());
            reset();
            assert(invoke(args) == 0 && hide_queries == 1);
            no_normal_setup();
            assert(received == std::vector<std::string>({tail.size() == 1 ? "" : revision,
                                                         tail.size() == 1 ? "0" : "16"}));
            reset();
            uid = 1000;
            assert(invoke(args) != 0 && hide_queries == 0);
            no_normal_setup();
        }
    }
    for (const auto& tail :
         std::vector<std::vector<std::string>>{{"status", "extra"},
                                               {"status", "bad", "0"},
                                               {"status", revision, "01"},
                                               {"status", revision, "4097"},
                                               {"status", revision, "0", "extra"},
                                               {"status", "--help"}}) {
        reset();
        std::vector<std::string> args = {"ksud", "sumhp", "hide"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(invoke(args) == 2 && hide_queries == 0);
        no_normal_setup();
    }
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"ksud", "--verbose", "sumhp", "hide", "status"},
             {"ksud", "sumhp", "--verbose", "hide", "status"},
             {"ksud", "sumhp", "daemon", "call", "hide", "status"}}) {
        reset();
        assert(invoke(args) == 2 && hide_queries == 0 && daemons == 0 && direct == 0 &&
               debug_logs == 0);
    }
    reset();
    throw_preview = true;
    assert(invoke({"ksud", "sumhp", "hide", "status"}) != 0 && hide_queries == 1);
    no_normal_setup();
    reset();
    submission_result = 1;
    assert(invoke({"ksud", "sumhp", "hide", "status"}) == 1 && hide_queries == 1);
    no_normal_setup();
    for (const auto& tail : std::vector<std::vector<std::string>>{
             {"create", revision, "{\"path\":\"/test\",\"enabled\":false}"},
             {"annotate", operation_id, revision, "{\"name\":\"test\"}"},
             {"enable", operation_id, revision},
             {"disable", operation_id, revision},
             {"enable-group", "g", revision},
             {"disable-group", "", revision},
             {"retry", operation_id, revision},
             {"remove-id", operation_id, revision},
             {"retry-path", "/test"}}) {
        reset();
        std::vector<std::string> args = {"ksud", "sumhp", "hide"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(invoke(args) == 44 && hide_queries == 0 && daemons == 1);
    }
    for (const auto& tail : std::vector<std::vector<std::string>>{
             {"create", revision, "{\"path\":\"/test\",\"enabled\":0}"},
             {"annotate", operation_id, revision, "{\"enabled\":false}"},
             {"enable", operation_id, "bad"},
             {"retry", "bad", revision},
             {"retry-path", "relative"}}) {
        reset();
        std::vector<std::string> args = {"ksud", "sumhp", "hide"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(invoke(args) == 2 && hide_queries == 0 && daemons == 0);
    }
    assert(meta.at("hide_definitions_schema").n == 1 && meta.at("hide_status").b &&
           meta.at("hide_definition_mutations").b);
    assert(meta.at("hide_mutation_requires_revision").b &&
           meta.at("hide_status_page_size").n == 16 && meta.at("hide_result_item_limit").n == 64);
    const int hide_channel_start = cases;
    for (const auto& tail : std::vector<std::vector<std::string>>{
             {"status"},
             {"status", revision, "16"},
             {"create", revision, "{\"path\":\"/路径 \",\"enabled\":false}"},
             {"annotate", operation_id, revision, "{\"note\":\"literal `$()\\n\"}"},
             {"enable", operation_id, revision},
             {"disable", operation_id, revision},
             {"enable-group", "--channel", revision},
             {"disable-group", "组\n ", revision},
             {"retry", operation_id, revision},
             {"remove-id", operation_id, revision}}) {
        const bool query = tail[0] == "status";
        std::vector<std::string> args = {"ksud", "sumhp", "hide"};
        args.insert(args.end(), tail.begin(), tail.end());
        args.push_back("--channel");
        reset();
        assert(invoke(args) == 0 && channels == 1 && hide_queries == query && daemons == !query);
        const auto dispatch = json::parse(channel_response);
        assert(dispatch.at("resource").s == "user_hide_dispatch" &&
               dispatch.at("schema_version").n == 1);
        assert(dispatch.at("exit_code").n == (query ? 0 : 44) && dispatch.at("stdout_complete").b &&
               dispatch.at("stderr_complete").b);
        if (!query) {
            auto target = args;
            target.erase(target.begin(), target.begin() + 2);
            target.pop_back();
            assert(received == target);
        }
        assert(logs == 0 && signals == 0 && level == ksud::LogLevel::INFO);
        reset();
        uid = 1000;
        assert(invoke(args) != 0 && channels == 0 && daemons == 0 && hide_queries == 0);
        no_normal_setup();
    }
    for (const auto& tail : std::vector<std::vector<std::string>>{
             {"status", "extra"},
             {"status", revision, "01"},
             {"create", revision, "{\"path\":\"/a\",\"path\":\"/b\"}"},
             {"enable", operation_id, "bad"},
             {"disable", std::string(32, 'A'), revision},
             {"enable-group", std::string(1, char(0xff)), revision},
             {"add", "/test"},
             {"remove", "/test"},
             {"retry-path", "/test"},
             {"status", "--channel"}}) {
        reset();
        std::vector<std::string> args = {"ksud", "sumhp", "hide"};
        args.insert(args.end(), tail.begin(), tail.end());
        args.push_back("--channel");
        assert(invoke(args) == 2 && channels == 0 && hide_queries == 0 && daemons == 0);
        no_normal_setup();
    }
    for (const auto& args : std::vector<std::string>{"--verbose", "--help"}) {
        reset();
        const auto code =
            invoke({"ksud", args, "sumhp", "hide", "disable", operation_id, revision, "--channel"});
        assert(code == (args == "--help" ? 0 : 2));  // Global help remains help, never dispatch.
        assert(channels == 0 && daemons == 0 && hide_queries == 0);
    }
    for (const auto& tail :
         std::vector<std::vector<std::string>>{{"status"}, {"disable", operation_id, revision}}) {
        reset();
        std::vector<std::string> args = {"ksud", "sumhp", "daemon", "call", "hide"};
        args.insert(args.end(), tail.begin(), tail.end());
        args.push_back("--channel");
        assert(invoke(args) == 2 && channels == 0 && daemons == 0 && hide_queries == 0);
    }
    for (const auto& tail : std::vector<std::vector<std::string>>{
             {"disable", operation_id, revision, "--channel", "extra"},
             {"disable", operation_id, revision, "--channel=1"},
             {"status", "--channel", "extra"}}) {
        reset();
        std::vector<std::string> args = {"ksud", "sumhp", "hide"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(invoke(args) == 2 && channels == 0 && daemons == 0 && hide_queries == 0);
    }
    reset();
    throw_preview = true;
    assert(invoke({"ksud", "sumhp", "hide", "status", "--channel"}) == 0 && channels == 1 &&
           hide_queries == 1);
    assert(json::parse(channel_response).at("exit_code").n == 1 &&
           !json::parse(channel_response).at("stderr").s.empty());
    no_normal_setup();
    assert(meta.at("hide_manager_transport_schema").n == 1);
    const int operation_channel_start = cases;
    using sumhp::JsonValue;
    using sumhp::stringify_json;
    auto operation_request = JsonValue::object();
    operation_request.o["schema_version"] = 1;
    operation_request.o["action"] = "prepare";
    operation_request.o["input"] = JsonValue::object();
    operation_request.o["input"].o["schema_version"] = 1;
    operation_request.o["input"].o["args"] = JsonValue::array();
    for (const auto& arg :
         std::vector<std::string>{"hide", "disable-group", "`$();\n组 ", revision})
        operation_request.o["input"].o["args"].a.emplace_back(arg);
    for (const auto* action : {"prepare", "execute", "show", "wait", "result"}) {
        auto request = operation_request;
        request.o["action"] = action;
        if (std::string(action) != "prepare")
            request.o["operation_id"] = operation_id;
        if (std::string(action) != "prepare" && std::string(action) != "execute")
            request.o.erase("input");
        if (std::string(action) == "wait")
            request.o["timeout_ms"] = 5000;
        const auto bytes = stringify_json(request);
        reset();
        submission_result = 6;
        assert(invoke({"ksud", "sumhp", "operation", "manager", bytes, "--channel"}) == 0 &&
               channels == 1 && daemons == 0);
        const auto value = json::parse(channel_response);
        assert(value.o.size() == 4 && value.at("resource").s == "operation_dispatch" &&
               value.at("exit_code").n == 6 && value.at("result").is_object());
        assert(operation_prepares + operation_executes + operation_queries == 1);
        no_normal_setup();
        reset();
        uid = 1000;
        assert(invoke({"ksud", "sumhp", "operation", "manager", bytes, "--channel"}) != 0 &&
               !channels && !operation_queries && !operation_prepares && !operation_executes);
        reset();
        assert(invoke({"ksud", "sumhp", "operation", "manager", bytes}) == 2 && !channels &&
               !daemons);
        reset();
        assert(invoke({"ksud", "sumhp", "daemon", "call", "operation", "manager", bytes,
                       "--channel"}) == 2 &&
               !channels && !daemons);
    }
    for (const auto& invalid :
         std::vector<std::string>{"{}", "[]", stringify_json(operation_request) + " {}",
                                  std::string(48 * 1024 + 1, ' ')}) {
        reset();
        assert(invoke({"ksud", "sumhp", "operation", "manager", invalid, "--channel"}) == 2 &&
               !channels && !daemons && !operation_prepares);
    }
    reset();
    assert(invoke({"ksud", "sumhp", "operation", "result", operation_id}) == 0 &&
           operation_queries == 1 && !daemons);
    no_normal_setup();
    reset();
    assert(invoke({"ksud", "sumhp", "operation", "result", "bad"}) == 2 && !operation_queries &&
           !daemons);
    no_normal_setup();
    assert(meta.at("operation_manager_transport_schema").n == 1 &&
           meta.at("operation_resource_result_schema").n == 1 && meta.at("operation_result").b);
    assert(meta.at("operation_manager_temporary_maps_schema").n == 2 &&
           meta.at("maps_temporary_tracked_manager").b);
    auto tracked_request = operation_request;
    tracked_request.o["schema_version"] = 2;
    tracked_request.o["resource"] = "temporary_maps";
    tracked_request.o["input"].o["args"] = JsonValue::array();
    for (const auto& arg : std::vector<std::string>{"sumh", "maps", "remove", "0", "0",
                                                    "--instance", "1", "--generation", "1"})
        tracked_request.o["input"].o["args"].a.emplace_back(arg);
    for (const auto* action : {"prepare", "execute", "show", "wait", "result"}) {
        auto request = tracked_request;
        request.o["action"] = action;
        if (std::string(action) != "prepare")
            request.o["operation_id"] = operation_id;
        if (std::string(action) != "prepare" && std::string(action) != "execute")
            request.o.erase("input");
        if (std::string(action) == "wait")
            request.o["timeout_ms"] = 0;
        const auto bytes = stringify_json(request);
        reset();
        assert(invoke({"ksud", "sumhp", "operation", "manager", bytes, "--channel"}) == 0 &&
               channels == 1 && !daemons);
        assert(operation_prepares + operation_executes + operation_queries == 1 &&
               !maps_temporary_commands && !temporary_manager_commands);
        assert(json::parse(channel_response).at("resource").s == "operation_dispatch");
        no_normal_setup();
        reset();
        assert(invoke({"ksud", "sumhp", "daemon", "call", "operation", "manager", bytes,
                       "--channel"}) == 2 &&
               !channels && !daemons);
        reset();
        uid = 1000;
        assert(invoke({"ksud", "sumhp", "operation", "manager", bytes, "--channel"}) != 0 &&
               !channels && !operation_executes);
    }
    for (const auto& change : std::vector<std::function<void(JsonValue&)>>{
             [](auto& v) { v.o["schema_version"] = 1; }, [](auto& v) { v.o.erase("resource"); },
             [](auto& v) { v.o["input"].o["args"].a[3] = "01"; }}) {
        auto bad = tracked_request;
        change(bad);
        reset();
        assert(invoke({"ksud", "sumhp", "operation", "manager", stringify_json(bad),
                       "--channel"}) == 2 &&
               !channels && !daemons && !operation_prepares);
    }
    const int exchange_start = cases;
    const std::string import_fields = "{\"path\":\"/导入\\n "
                                      "\",\"enabled\":false,\"name\":\"\",\"note\":\"\",\"group\":"
                                      "\"--help\",\"expect\":\"absent\"}";
    for (const auto& tail : std::vector<std::vector<std::string>>{
             {"definitions"}, {"definitions", revision, "64"}, {"export"}}) {
        std::vector<std::string> args = {"ksud", "sumhp", "hide"};
        args.insert(args.end(), tail.begin(), tail.end());
        reset();
        assert(invoke(args) == 0 && hide_queries == 1 && !daemons);
        no_normal_setup();
        reset();
        uid = 1000;
        assert(invoke(args) != 0 && !hide_queries && !daemons);
        no_normal_setup();
        reset();
        auto wrapped = args;
        wrapped.insert(wrapped.begin() + 1, "--verbose");
        assert(invoke(wrapped) == 2 && !hide_queries && !daemons);
        reset();
        wrapped = args;
        wrapped.insert(wrapped.begin() + 2, {"daemon", "call"});
        assert(invoke(wrapped) == 2 && !hide_queries && !daemons);
        reset();
        assert(sumhp::operation_probe::run_command(
                   std::vector<std::string>(args.begin() + 2, args.end())) == 2 &&
               !hide_queries && !daemons);
    }
    for (const auto& tail : std::vector<std::vector<std::string>>{{"definitions", "bad", "0"},
                                                                  {"definitions", revision, "4097"},
                                                                  {"definitions", revision, "01"},
                                                                  {"export", "extra"},
                                                                  {"export", "--channel"}}) {
        reset();
        std::vector<std::string> args = {"ksud", "sumhp", "hide"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(invoke(args) == 2 && !hide_queries && !daemons && !channels);
        no_normal_setup();
    }
    for (const auto& tail : std::vector<std::vector<std::string>>{
             {"definitions"},
             {"definitions", revision, "64"},
             {"import-id", operation_id, revision, import_fields}}) {
        reset();
        std::vector<std::string> args = {"ksud", "sumhp", "hide"};
        args.insert(args.end(), tail.begin(), tail.end());
        args.push_back("--channel");
        const bool query = tail[0] == "definitions";
        assert(invoke(args) == 0 && channels == 1 && hide_queries == query && daemons == !query);
        assert(logs == 0 && signals == 0 && level == ksud::LogLevel::INFO);
        if (query)
            no_normal_setup();
    }
    reset();
    assert(invoke({"ksud", "sumhp", "hide", "import-id", operation_id, revision, import_fields}) ==
               44 &&
           daemons == 1 && !hide_queries);
    reset();
    assert(invoke({"ksud", "sumhp", "hide", "import-id", operation_id, revision, "{}"}) == 2 &&
           !daemons);
    assert(meta.at("hide_definition_import_schema").n == 1 &&
           meta.at("hide_definition_exchange_schema").n == 1 &&
           meta.at("hide_definition_export_page_schema").n == 1);
    reset();
    assert(invoke({"ksud", "sumhp", "api", "meta", "--control-only", "--channel"}) == 0 &&
           channels == 1 && direct == 1 && !daemons && !hide_queries && !logs && !signals);
    const auto capabilities = json::parse(json::parse(channel_response).at("stdout").s);
    assert(capabilities.at("hide_definition_import_schema").n == 1 &&
           capabilities.at("backends").a.empty());
    reset();
    uid = 1000;
    assert(invoke({"ksud", "sumhp", "api", "meta", "--control-only", "--channel"}) != 0 &&
           !channels && !direct && !daemons);
    no_normal_setup();
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"ksud", "sumhp", "api", "meta", "--channel"},
             {"ksud", "sumhp", "api", "meta", "extra", "--channel"},
             {"ksud", "sumhp", "api", "meta", "--control-only", "extra", "--channel"},
             {"ksud", "--verbose", "sumhp", "api", "meta", "--control-only", "--channel"},
             {"ksud", "sumhp", "daemon", "call", "api", "meta", "--control-only", "--channel"}}) {
        reset();
        assert(invoke(args) == 2 && !channels && !direct && !daemons && !hide_queries);
    }
    const int module_channel_start = cases;
    const std::string module_input =
        R"({"schema_version":1,"kind":"sumh.module.plan.input","modules":[]})";
    for (const auto& path : std::vector<std::string>{"", "/vendor/中文 $()`; \t "}) {
        for (bool saved : {false, true}) {
            std::vector<std::string> args = {"ksud", "sumhp", "module", "manager-preview",
                                             saved ? "saved" : "candidate"};
            if (!saved)
                args.push_back(module_input);
            args.push_back(path);
            args.push_back("--channel");
            reset();
            assert(invoke(args) == 0 && channels == 1 && module_channels == 1 &&
                   module_previews == 1);
            no_normal_setup();
            assert(received[0] == path && received[1].empty());
            assert(saved ? received.size() == 2
                         : received.size() == 3 && received[2] == module_input);
            reset();
            uid = 1000;
            assert(invoke(args) != 0 && !module_previews && !channels);
            no_normal_setup();
            reset();
            auto wrapped = args;
            wrapped.insert(wrapped.begin() + 1, "--verbose");
            assert(invoke(wrapped) == 2 && !channels && !module_previews && !daemons);
            reset();
            wrapped = args;
            wrapped.insert(wrapped.begin() + 2, {"daemon", "call"});
            assert(invoke(wrapped) == 2 && !channels && !module_previews && !daemons);
            reset();
            assert(sumhp::handle_module(std::vector<std::string>(args.begin() + 2, args.end())) ==
                       1 &&
                   !module_previews && !daemons);
        }
    }
    for (const auto& tail : std::vector<std::vector<std::string>>{
             {},
             {"saved"},
             {"saved", ""},
             {"saved", "", "extra", "--channel"},
             {"candidate", "", "", "--channel"},
             {"candidate", "{}", "", "--channel"},
             {"candidate", module_input, "/a/../b", "--channel"},
             {"saved", "relative", "--channel"},
             {"candidate", module_input, "", "--input", "-", "--channel"}}) {
        reset();
        std::vector<std::string> args = {"ksud", "sumhp", "module", "manager-preview"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(invoke(args) == 2 && !channels && !module_previews && !daemons);
        no_normal_setup();
    }
    reset();
    assert(invoke({"ksud", "sumhp", "module", "plan", "--channel"}) != 0 && !channels &&
           !module_previews && !daemons);
    no_normal_setup();
    assert(meta.at("module_manager_preview_transport_schema").n == 1 &&
           meta.at("module_manager_inline_candidate").b &&
           meta.at("module_manager_saved_preview").b && meta.at("module_manager_explain").b &&
           !meta.at("module_apply").b);
    const int capability_start = cases;
    for (bool channel : {false, true}) {
        auto args = std::vector<std::string>{"ksud", "sumhp", "api", "capabilities"};
        if (channel)
            args.push_back("--channel");
        reset();
        assert(invoke(args) == 0 && capability_queries == 1 &&
               channels == static_cast<int>(channel));
        no_normal_setup();
        reset();
        uid = 1000;
        assert(invoke(args) != 0 && !capability_queries && !channels);
        no_normal_setup();
        reset();
        throw_preview = true;
        assert(invoke(args) != 0 && capability_queries == 1);
        no_normal_setup();
        for (const auto& prefix : std::vector<std::vector<std::string>>{
                 {"--verbose"}, {"sumhp", "--verbose"}, {"sumhp", "daemon", "call"}}) {
            reset();
            auto wrapped = std::vector<std::string>{"ksud"};
            wrapped.insert(wrapped.end(), prefix.begin(), prefix.end());
            wrapped.insert(wrapped.end(), args.begin() + (prefix.size() == 1 ? 1 : 2), args.end());
            assert(invoke(wrapped) == 2 && !capability_queries && !channels && !daemons);
        }
    }
    for (const auto& tail : std::vector<std::vector<std::string>>{{"extra"},
                                                                  {"--help"},
                                                                  {"--channel", "extra"},
                                                                  {"--channel", "--channel"},
                                                                  {"--channel=1"},
                                                                  {"--"}}) {
        reset();
        auto args = std::vector<std::string>{"ksud", "sumhp", "api", "capabilities"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(invoke(args) == 2 && !capability_queries && !channels);
        no_normal_setup();
    }
    assert(meta.at("capability_snapshot_schema").n == 1 &&
           meta.at("capability_snapshot_transport_schema").n == 1 &&
           meta.at("capability_snapshot_read_only").b &&
           meta.at("capability_snapshot_response_bytes").n == 65536);
    reset();
    assert(sumhp::operation_probe::run_command({"api", "capabilities"}) == 2 &&
           !capability_queries && !daemons);
    const int rebuild_start = cases;
    reset();
    assert(invoke({"ksud", "sumhp", "module", "rebuild-status"}) == 0 && rebuild_queries == 1);
    no_normal_setup();
    reset();
    uid = 1000;
    assert(invoke({"ksud", "sumhp", "module", "rebuild-status"}) != 0 && !rebuild_queries);
    no_normal_setup();
    reset();
    throw_preview = true;
    assert(invoke({"ksud", "sumhp", "module", "rebuild-status"}) != 0 && rebuild_queries == 1);
    no_normal_setup();
    reset();
    submission_result = 1;
    assert(invoke({"ksud", "sumhp", "module", "rebuild-status"}) == 1 && rebuild_queries == 1);
    no_normal_setup();
    for (const auto& tail : {"extra", "--channel", "--help", "--", "--input"}) {
        reset();
        assert(invoke({"ksud", "sumhp", "module", "rebuild-status", tail}) == 2 &&
               !rebuild_queries && !daemons);
        no_normal_setup();
    }
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"ksud", "--verbose", "sumhp", "module", "rebuild-status"},
             {"ksud", "sumhp", "--verbose", "module", "rebuild-status"},
             {"ksud", "sumhp", "daemon", "call", "module", "rebuild-status"}}) {
        reset();
        assert(invoke(args) == 2 && !rebuild_queries && !daemons);
    }
    reset();
    assert(sumhp::handle_module({"module", "rebuild-status"}) == 1 && !rebuild_queries && !daemons);
    assert(meta.at("module_rebuild_status").b && meta.at("module_rebuild_status_schema").n == 1 &&
           !meta.at("module_rebuild_owner_transaction").b && !meta.at("module_apply").b);
    const int profile_start = cases;
    const std::vector<std::vector<std::string>> profile_tails = {
        {"list"},
        {"show", "named-profile"},
        {"export", "named-profile"},
        {"preview", "named-profile", "--resources", "all"},
        {"import-preview", "--input", "/data/local/tmp/input \n.json", "--resources",
         "maps,user_hide"},
        {"capture", "--input", "-"},
        {"import", "--input", "/data/local/tmp/profile.json"},
        {"clone", "--input", "-"},
        {"rename", "--input", "-"},
        {"remove", "--input", "-"}};
    for (const auto* executable :
         {"ksud", "/data/adb/ksud", "/data/app/example/lib/arm64/libksud.so"}) {
        for (const auto& tail : profile_tails) {
            std::vector<std::string> args{executable, "sumhp", "profile"};
            args.insert(args.end(), tail.begin(), tail.end());
            reset();
            assert(invoke(args) == 0 && profile_commands == 1 &&
                   received == std::vector<std::string>(args.begin() + 2, args.end()));
            no_normal_setup();
            reset();
            uid = 1000;
            assert(invoke(args) != 0 && !profile_commands);
            no_normal_setup();
        }
    }
    for (const auto& tail : std::vector<std::vector<std::string>>{
             {},
             {"activate"},
             {"list", "extra"},
             {"list", "--channel"},
             {"list", "--help"},
             {"show"},
             {"show", "../bad"},
             {"export", "UPPER"},
             {"preview", "p", "--resources", "maps,maps"},
             {"preview", "p", "--resources", "all,maps"},
             {"preview", "p", "--resources", ""},
             {"preview", "p", "--resources", "settings,"},
             {"capture", "--input", "relative"},
             {"capture", "--input=-"},
             {"capture", "--input", "-", "extra"},
             {"import-preview", "--input", "-", "--resources", "bad"},
             {"rename", "--input"}}) {
        reset();
        std::vector<std::string> args{"ksud", "sumhp", "profile"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(invoke(args) == 2 && !profile_commands && !channels);
        no_normal_setup();
    }
    for (const auto& tail : profile_tails) {
        for (const auto& prefix : std::vector<std::vector<std::string>>{
                 {"ksud", "--verbose", "sumhp", "profile"},
                 {"ksud", "sumhp", "--verbose", "profile"},
                 {"ksud", "sumhp", "daemon", "call", "profile"},
                 {"ksud", "sumhp", "daemon", "call", "daemon", "call", "profile"}}) {
            reset();
            auto args = prefix;
            args.insert(args.end(), tail.begin(), tail.end());
            assert(invoke(args) == 2 && !profile_commands && !daemons && !direct && !debug_logs);
        }
        reset();
        auto direct_request = std::vector<std::string>{"profile"};
        direct_request.insert(direct_request.end(), tail.begin(), tail.end());
        assert(sumhp::operation_probe::run_command(direct_request) == 2 && !profile_commands &&
               !daemons);
    }
    reset();
    throw_preview = true;
    assert(invoke({"ksud", "sumhp", "profile", "list"}) != 0 && profile_commands == 1);
    no_normal_setup();
    reset();
    submission_result = 1;
    assert(invoke({"ksud", "sumhp", "profile", "capture", "--input", "-"}) == 1 &&
           profile_commands == 1);
    no_normal_setup();
    reset();
    assert(invoke({"su", "sumhp", "profile", "list"}) == 72 && su == 1 && !profile_commands);
    reset();
    assert(invoke({"sh", "sumhp", "profile", "list"}) == 73 && busybox == 1 && !profile_commands);
    assert(ksud::parse_cli({"sumhp", "profile", "list"}, parsed) == 2);
    assert(ksud::parse_cli({"sumhp", "profile", "capture", "--input", "-"}, parsed) == 2);
    assert(meta.at("profile_schema").n == 1 && meta.at("profile_bank_schema").n == 1);
    for (const auto* feature : {"profile_capture", "profile_import", "profile_export",
                                "profile_clone", "profile_rename", "profile_remove", "profile_list",
                                "profile_show", "profile_import_preview", "profile_diff_preview"})
        assert(meta.at(feature).b);
    assert(meta.at("profile_manager_transport").b && meta.at("profile_manager_schema").n == 1);
    for (const auto* feature : {"profile_activate", "profile_restore", "profile_operation_journal"})
        assert(!meta.at(feature).b);
    assert(meta.at("profile_input_bytes").n == sumhp::profiles::PROFILE_INPUT_MAX_BYTES &&
           meta.at("profile_document_bytes").n == sumhp::profiles::PROFILE_MAX_BYTES);
    assert(meta.at("profile_bank_bytes").n == sumhp::profiles::BANK_MAX_BYTES &&
           meta.at("profile_count_limit").n == sumhp::profiles::BANK_MAX_PROFILES);
    const int temporary_start = cases;
    for (const auto& tail : std::vector<std::vector<std::string>>{
             {"list-temporary"},
             {"list-temporary", "18446744073709551615", "9", "0"},
             {"remove", "0", "0", "--instance", "1", "--generation", "9"},
             {"remove"},
             {"remove", "--channel"},
             {"list-temporary", "extra"}}) {
        for (const auto* executable : {"ksud", "/data/adb/ksud", "/data/app/test/libksud.so"}) {
            auto args = std::vector<std::string>{executable, "sumhp", "sumh", "maps"};
            args.insert(args.end(), tail.begin(), tail.end());
            reset();
            assert(invoke(args) == 0 && maps_temporary_commands == 1 &&
                   received == std::vector<std::string>(args.begin() + 2, args.end()));
            no_normal_setup();
            reset();
            uid = 1000;
            assert(invoke(args) != 0 && !maps_temporary_commands);
            no_normal_setup();
        }
        for (const auto& prefix : std::vector<std::vector<std::string>>{
                 {"ksud", "--verbose", "sumhp", "sumh", "maps"},
                 {"ksud", "sumhp", "--verbose", "sumh", "maps"},
                 {"ksud", "sumhp", "daemon", "call", "sumh", "maps"},
                 {"ksud", "sumhp", "daemon", "call", "daemon", "call", "sumh", "maps"}}) {
            reset();
            auto args = prefix;
            args.insert(args.end(), tail.begin(), tail.end());
            assert(invoke(args) == 2 && !maps_temporary_commands && !daemons && !direct &&
                   !debug_logs);
        }
        auto args = std::vector<std::string>{"sumh", "maps"};
        args.insert(args.end(), tail.begin(), tail.end());
        reset();
        assert(sumhp::operation_probe::run_command(args) == 2 && !maps_temporary_commands &&
               !daemons);
    }
    reset();
    throw_preview = true;
    assert(invoke({"ksud", "sumhp", "sumh", "maps", "remove"}) != 0 &&
           maps_temporary_commands == 1);
    no_normal_setup();
    reset();
    submission_result = 1;
    assert(invoke({"ksud", "sumhp", "sumh", "maps", "remove"}) == 1 &&
           maps_temporary_commands == 1);
    no_normal_setup();
    reset();
    assert(invoke({"su", "sumhp", "sumh", "maps", "remove"}) == 72 && su == 1 &&
           !maps_temporary_commands);
    reset();
    assert(invoke({"sh", "sumhp", "sumh", "maps", "remove"}) == 73 && busybox == 1 &&
           !maps_temporary_commands);
    assert(meta.at("maps_temporary_list").b && meta.at("maps_temporary_remove_schema").n == 1);
    assert(meta.at("maps_temporary_remove_requires_kernel_capability").b &&
           !meta.at("maps_temporary_operation_journal").b);
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    std::cout << "Diagnostics: " << diagnostic_cases
              << " actual main/CLI/embedded root/silent/exact-route checks (BusyBox enabled, old "
                 "argv0 counterexample) passed\n";
    std::cout << "Full Maps/module preview main/CLI/embedded chain: "
              << (maps_cases - diagnostic_cases) << " Maps + " << (operation_start - maps_cases)
              << " module/development dispatch cases (" << (operation_start - previous_cases)
              << " Batch10) + parser/daemon-rejection contracts passed\n";
    std::cout << "Operation history: " << (submission_start - operation_start)
              << " exact-route/root/parser cases + serializer/rejection/meta passed\n";
    std::cout << "Operation submission: " << (hide_start - submission_start)
              << " exact-route/root/parser cases + exception/preservation/meta passed\n";
    const int temporary_manager_start = cases;
    const std::string manager_capabilities = R"({"schema_version":1,"action":"capabilities"})";
    for (
        const auto& request : std::vector<std::string>{
            manager_capabilities, R"({"schema_version":1,"action":"list"})",
            R"({"schema_version":1,"action":"list","instance_id":"1","generation":"2","cursor":"0"})",
            R"({"schema_version":1,"action":"remove","instance_id":"18446744073709551615","generation":"9","target_ino":"0","target_dev":"0"})"}) {
        const std::vector<std::string> args{"ksud",    "sumhp", "sumh",     "maps",
                                            "manager", request, "--channel"};
        reset();
        assert(invoke(args) == 0 && temporary_manager_commands == 1 && channels == 1 &&
               !maps_temporary_commands);
        no_normal_setup();
        reset();
        uid = 1000;
        assert(invoke(args) != 0 && !temporary_manager_commands && !channels);
        no_normal_setup();
        reset();
        throw_preview = true;
        assert(invoke(args) != 0 && temporary_manager_commands == 1 && channels == 1);
        no_normal_setup();
        for (const auto& prefix : std::vector<std::vector<std::string>>{
                 {"--verbose"}, {"--log-file", "/tmp/no-write"}, {"sumhp", "call"}}) {
            reset();
            auto wrapped = std::vector<std::string>{"ksud"};
            wrapped.insert(wrapped.end(), prefix.begin(), prefix.end());
            wrapped.insert(wrapped.end(), args.begin() + 1, args.end());
            assert(invoke(wrapped) == 2 && !temporary_manager_commands && !channels && !daemons);
        }
        reset();
        assert(sumhp::operation_probe::run_command(
                   std::vector<std::string>(args.begin() + 2, args.end())) == 2 &&
               !temporary_manager_commands && !daemons);
    }
    for (const auto& tail : std::vector<std::vector<std::string>>{
             {},
             {"{}"},
             {"{}", "--channel"},
             {manager_capabilities},
             {manager_capabilities, "--channel", "extra"},
             {manager_capabilities, "extra", "--channel"},
             {"{\"schema_version\":1,\"action\":\"list\",\"action\":\"remove\"}", "--channel"},
             {std::string(1025, ' '), "--channel"}}) {
        reset();
        std::vector<std::string> args{"ksud", "sumhp", "sumh", "maps", "manager"};
        args.insert(args.end(), tail.begin(), tail.end());
        assert(invoke(args) == 2 && !temporary_manager_commands && !channels &&
               !maps_temporary_commands);
        no_normal_setup();
        reset();
        uid = 1000;
        assert(invoke(args) != 0 && !temporary_manager_commands && !channels);
        no_normal_setup();
    }
    reset();
    assert(invoke({"su", "sumhp", "sumh", "maps", "manager"}) == 72 && su == 1 &&
           !temporary_manager_commands);
    reset();
    assert(invoke({"sh", "sumhp", "sumh", "maps", "manager"}) == 73 && busybox == 1 &&
           !temporary_manager_commands);
    assert(meta.at("maps_temporary_manager_schema").n == 1 &&
           meta.at("maps_temporary_manager_channel").b);
    std::cout << (cases - temporary_manager_start)
              << " temporary Maps Manager exact-route/root/parser/dispatch cases passed\n";
    std::cout << "User hide definitions: " << (hide_channel_start - hide_start)
              << " main/CLI/embedded route/root/parser cases + capability metadata passed\n";
    std::cout << "User hide Manager channel: " << (operation_channel_start - hide_channel_start)
              << " exact-route/root/parser/dispatch cases passed\n";
    std::cout << "Operation Manager channel: " << (exchange_start - operation_channel_start)
              << " exact-route/root/parser/dispatch cases passed\n";
    std::cout << "User hide exchange: " << (module_channel_start - exchange_start)
              << " exact-route/root/parser/dispatch cases passed\n";
    std::cout << "Module Planner Manager channel: " << (capability_start - module_channel_start)
              << " exact-route/root/parser/dispatch cases passed\n";
    std::cout << "Capability snapshot: " << (rebuild_start - capability_start)
              << " exact-route/root/parser/dispatch cases passed\n";
    std::cout << "Module rebuild status: " << (profile_start - rebuild_start)
              << " exact-route/root/parser/dispatch cases + daemon rejection/meta passed\n";
    std::cout << "Inactive SUMH profiles: " << (temporary_start - profile_start)
              << " exact-route/root/parser/dispatch cases + daemon rejection/meta passed\n";
    std::cout << "Temporary Maps: " << (temporary_manager_start - temporary_start)
              << " exact-route/root/parser/dispatch cases + daemon rejection/meta passed\n";
}
