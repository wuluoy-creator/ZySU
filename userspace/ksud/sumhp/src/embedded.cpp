#include "sumhp/embedded.hpp"
#include <sys/mount.h>
#include <unistd.h>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include "core/capability_snapshot.hpp"
#include "core/command.hpp"
#include "core/daemon.hpp"
#include "core/diagnostic_package.hpp"
#include "core/diagnostics.hpp"
#include "core/ksucalls.hpp"
#include "core/log.hpp"
#include "core/operation_manager_input.hpp"
#include "core/operation_query.hpp"
#include "core/operation_submit.hpp"
#include "core/profile.hpp"
#include "core/restore_policy.hpp"
#include "core/runtime.hpp"
#include "core/user_hide.hpp"
#include "module/metamodule.hpp"
#include "mount/backend.hpp"
#include "mount/development.hpp"
#include "mount/maps_preview.hpp"
#include "mount/maps_temporary.hpp"
#include "mount/module_preview.hpp"
#include "mount/module_rebuild.hpp"
#include "mount/temporary_maps_manager.hpp"
#include "sumhp/config.hpp"
#include "sumhp/sumh_client.hpp"
#include "terminal.hpp"
#include "userspace/common/maps_preview_transport.hpp"
#include "userspace/common/profile_manager_transport.hpp"
#include "utils.hpp"

namespace sumhp {
int embedded_command(const std::vector<std::string>& args) try {
    if (!args.empty() && (args[0] == "version" || args[0] == "--version"))
        return run_command(args);
    if (geteuid() != 0) {
        return ksud::terminal::error("SUMHP requires root",
                                     "Run this command from an authorized root shell.");
    }
    if (!args.empty() && args[0] == "doctor") {
        if (args.size() >= 3 && args[1] == "package" && args[2] == "manager") {
            diagnostic_package_manager::Request request;
            if (!diagnostic_package_manager::parse_channel_arguments(args, request))
                return 2;
            return preview_transport::serve(
                [request] { return diagnostic_package::manager_dispatch(request); },
                preview_transport::Service::DiagnosticPackages);
        }
        if (args.size() >= 2 && args[1] == "package")
            return diagnostic_package::command(args);
        diagnostics::Request request;
        std::string reason;
        if (diagnostics::parse_channel_arguments(args, request, reason))
            return preview_transport::serve(
                [request] { return diagnostics::collect_diagnostics(request); },
                preview_transport::Service::Diagnostics);
        return diagnostics::doctor_command(args);
    }
    if (!args.empty() && args[0] == "profile") {
        if (args.size() >= 2 && args[1] == "manager") {
            if (args != std::vector<std::string>{"profile", "manager", "--channel"})
                return 2;
            return profile_transport::serve(profiles::manager_dispatch);
        }
        return profiles::command(args);
    }
    if (temporary_maps::is_manager_prefix(args)) {
        temporary_maps::Request request;
        if (!temporary_maps::parse_arguments(args, request)) {
            std::cerr << "invalid temporary Maps Manager channel request\n";
            return 2;
        }
        return preview_transport::serve([&] { return temporary_maps::dispatch(request); },
                                        preview_transport::Service::TemporaryMaps);
    }
    if (maps_temporary::is_prefix(args))
        return maps_temporary::command(args, std::cout, std::cerr);
    if (args.size() >= 2 && args[0] == "api" && args[1] == "capabilities") {
        if (args == std::vector<std::string>{"api", "capabilities", "--channel"})
            return preview_transport::serve(capability_snapshot_json,
                                            preview_transport::Service::Capabilities);
        if (args != std::vector<std::string>{"api", "capabilities"}) {
            std::cerr << "usage: ksud sumhp api capabilities\n";
            return 2;
        }
        std::cout << capability_snapshot_json() << '\n';
        return std::cout.good() ? 0 : 1;
    }
    if (!args.empty() && args[0] == "operation") {
        if (args.size() >= 2 && args[1] == "manager") {
            state::OperationManagerRequest request;
            std::string error;
            if (args.size() != 4 || args[3] != "--channel" ||
                !state::parse_operation_manager_input(args[2], request, error)) {
                std::cerr << "invalid operation Manager channel request\n";
                return 2;
            }
            return preview_transport::serve([&] {
                state::OperationResponse response;
                if (request.action == "prepare" || request.action == "execute")
                    response = state::submit_operation_bytes(request.action == "execute",
                                                             request.id, request.input);
                else {
                    state::OperationQueryRequest query;
                    query.id = request.id;
                    query.timeout_ms = request.timeout_ms;
                    if (request.action == "wait")
                        query.kind = state::OperationQueryRequest::Kind::Wait;
                    else
                        query.kind = request.action == "result"
                                         ? state::OperationQueryRequest::Kind::Result
                                         : state::OperationQueryRequest::Kind::Show;
                    response = state::query_operation(query);
                }
                auto value = JsonValue::object();
                value.o["schema_version"] = 1;
                value.o["resource"] = "operation_dispatch";
                value.o["exit_code"] = response.exit_code;
                value.o["result"] = std::move(response.value);
                return stringify_json(value);
            });
        }
        if (args.size() >= 2 && (args[1] == "prepare" || args[1] == "execute")) {
            state::OperationSubmitRequest request;
            std::string error;
            if (!state::parse_operation_submit_arguments(args, request, error)) {
                std::cerr << error << '\n';
                return 2;
            }
            return state::print_operation_submit(request);
        }
        state::OperationQueryRequest request;
        std::string error;
        if (!state::parse_operation_query_arguments(args, request, error)) {
            std::cerr << error << '\n';
            return 2;
        }
        return state::print_operation_query(request);
    }
    if (args.size() >= 2 && (args[0] == "hide" || (args[0] == "api" && args[1] == "meta")) &&
        args.back() == "--channel") {
        const std::vector<std::string> target(args.begin(), args.end() - 1);
        hide::Request request;
        std::string error;
        if (!hide::parse_manager_arguments(target, request, error)) {
            std::cerr << error << '\n';
            return 2;
        }
        return preview_transport::serve([&] {
            return hide::manager_dispatch_json([&] {
                if (request.action == "status")
                    return hide::print_status(request);
                if (request.action == "definitions")
                    return hide::print_definitions(request);
                if (request.action == "capabilities")
                    return run_command(target);
                return run_via_daemon(target, true);
            });
        });
    }
    if (args.size() >= 2 && args[0] == "hide" && hide::definition_query(args[1])) {
        hide::Request request;
        std::string error;
        if (!hide::parse_request(args, request, error)) {
            std::cerr << error << '\n';
            return 2;
        }
        if (request.action == "status")
            return hide::print_status(request);
        if (request.action == "definitions")
            return hide::print_definitions(request);
        return hide::print_export();
    }
    // This query must never cold-start a daemon (which can restore settings).
    // Reject malformed preview requests here as well, before any recovery.
    if (args.size() >= 3 && args[0] == "sumh" && args[1] == "maps" && args[2] == "preview") {
        if (args.size() == 4 && args[3] == "--channel")
            return preview_transport::serve(mount::sumh::maps_preview_json);
        if (!mount::sumh::is_maps_preview_command(args)) {
            std::cerr << "usage: ksud sumhp sumh maps preview\n";
            return 1;
        }
        std::cout << mount::sumh::maps_preview_json() << '\n';
        return std::cout.good() ? 0 : 1;
    }
    if (mount::is_development_prefix(args)) {
        mount::DevelopmentRequest request;
        std::string error;
        if (!mount::parse_development_arguments(args, request, error)) {
            std::cerr << error << '\n';
            return 1;
        }
        const auto callback = [&] {
            return request.kind == mount::DevelopmentKind::PartitionSave
                       ? mount::partition_save_json(request)
                       : mount::development_query_json(request);
        };
        if (request.channel)
            return preview_transport::serve(callback);
        const auto bytes = callback();
        std::cout << bytes << '\n';
        if (!std::cout.good())
            return 1;
        if (request.kind == mount::DevelopmentKind::PartitionSave) {
            const auto value = json::parse(bytes);
            const auto* saved = value.find("saved");
            return saved && saved->is_bool() && saved->b ? 0 : 1;
        }
        return 0;
    }
    if (args.size() >= 2 && args[0] == "module" && args[1] == "manager-preview") {
        module_preview::Input input;
        if (!module_preview::parse_arguments(args, input)) {
            std::cerr << "invalid module Planner Manager channel request\n";
            return 2;
        }
        mount::ModulePreviewRequest request;
        request.explain_path = std::move(input.explain_path);
        request.inline_input = std::move(input.candidate);
        return preview_transport::serve([&] { return mount::module_preview_json(request); },
                                        preview_transport::Service::ModulePreview);
    }
    if (args.size() >= 2 && args[0] == "module" && args[1] == "rebuild-status") {
        if (args.size() != 2) {
            std::cerr << "usage: ksud sumhp module rebuild-status\n";
            return 2;
        }
        return mount::sumh::print_module_rebuild_status();
    }
    if (mount::is_module_preview_prefix(args)) {
        mount::ModulePreviewRequest request;
        std::string error;
        if (!mount::parse_module_preview_arguments(args, request, error)) {
            std::cerr << error << '\n';
            return 1;
        }
        std::cout << mount::module_preview_json(request) << '\n';
        return std::cout.good() ? 0 : 1;
    }
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h" ||
        args[0] == "version" || args[0] == "--version" || args[0] == "daemon")
        return run_command(args);
    return run_via_daemon(args, true);
} catch (const std::exception& error) {
    return ksud::terminal::error(std::string("SUMHP: ") + error.what());
}

int embedded_mount() try {
    std::string error;
    if (!prepare_runtime(error)) {
        logging::write(logging::Level::Error, "boot", error);
        return 1;
    }
    if (!std::filesystem::exists(runtime_config_file()) && !write_default_config(error)) {
        std::cerr << "SUMHP: " << error << '\n';
        return 1;
    }
    Config config;
    if (!read_config_file(config, error)) {
        std::cerr << "SUMHP: " << error << '\n';
        return 1;
    }
    logging::write(logging::Level::Info, "boot",
                   "starting built-in mount plan; no external LKM lookup");
    return run_via_daemon({"module", "mount-all"});
} catch (const std::exception& error) {
    std::cerr << "SUMHP mount: " << error.what() << '\n';
    return 1;
}

void embedded_restore_kernel_build(bool boot_completed) {
    const RestoreContext context{ksud::is_safe_mode(), boot_completed, sumh::is_available(),
                                 !embedded_external_mount_owner().empty()};
    if (!should_restore(context, RestoreFeature::KernelBuild)) {
        return;
    }
    // Kernel identity is independent of which backend owns module mounts.
    if (sumh::is_available()) {
        Config config;
        std::string error;
        if (!prepare_runtime(error)) {
            logging::write(logging::Level::Error, "boot",
                           "prepare kernel build settings: " + error);
            return;
        }
        errno = 0;
        if (read_config_file(config, error)) {
            if (!should_restore(context, RestoreFeature::KernelBuild, RestoreTrigger::Automatic,
                                config.enable_kernel_build_spoof &&
                                    config.kernel_build_apply_stage == "boot-completed"))
                return;
            if (!sumh::set_kernel_build(config.enable_kernel_build_spoof,
                                        config.kernel_build_release, config.kernel_build_version)) {
                logging::write(logging::Level::Error, "boot",
                               "failed to restore kernel build spoof");
            }
        } else if (errno != ENOENT) {
            logging::write(logging::Level::Error, "boot", "read kernel build settings: " + error);
        }
    }
}

void embedded_boot_completed() {
    logging::write(logging::Level::Info, "boot", "boot completed");
    std::string recovery_error;
    if (!mount::recovery_boot_completed(recovery_error)) {
        logging::write(logging::Level::Error, "boot",
                       "failed to confirm recovery state: " + recovery_error);
    }
    // Retry an early restoration that could not reach the kernel yet.
    embedded_restore_kernel_build(true);
    const RestoreContext context{ksud::is_safe_mode(), true, sumh::is_available(),
                                 !embedded_external_mount_owner().empty()};
    // Path identities are resolved only after module mounts have settled.
    // Maps filtering is independent of which controller owns those mounts.
    if (should_restore(context, RestoreFeature::Maps) &&
        run_via_daemon({"sumh", "maps", "apply", "--automatic"}) != 0) {
        logging::write(logging::Level::Error, "boot", "failed to restore maps path rules");
    }
    if (should_restore(context, RestoreFeature::UserHide) &&
        run_via_daemon({"hide", "apply", "--automatic"}) != 0) {
        logging::write(logging::Level::Error, "boot", "failed to restore user hide rules");
    }
}

void embedded_post_fs_data() {
    std::string error;
    if (!prepare_runtime(error) || !logging::prepare_boot_log(error)) {
        logging::write(logging::Level::Error, "boot", "prepare built-in controller: " + error);
        return;
    }
    logging::write(logging::Level::Info, "boot",
                   "post-fs-data; state=" + runtime_data_dir().string());
}

void embedded_mount_skipped(const std::string& reason) {
    logging::write(logging::Level::Info, "boot", "built-in mount skipped: " + reason);
}

bool embedded_register_umount(const std::string& path) {
    // Skeletons contain child binds and files may already be open in zygote.
    // Match the mountinfo scan's detach semantics when the kernel falls back
    // to this list; ordinary umount can leave the whole tree behind with EBUSY.
    const bool ok = ksud::umount_list_add(path, MNT_DETACH) == 0;
    if (!ok)
        logging::write(logging::Level::Error, "mount", "failed to register unmount path " + path);
    return ok;
}

bool embedded_unregister_umount(const std::string& path) {
    return ksud::umount_list_del(path) == 0;
}

std::string embedded_external_mount_owner() {
    return ksud::metamodule_mount_owner();
}
}  // namespace sumhp

extern "C" int ksu_sumh_ioctl(unsigned long cmd, void* arg) {
    return ksud::ksuctl(static_cast<int>(cmd), arg);
}
