#include "core/diagnostic_package.hpp"
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <iostream>
#include "core/diagnostics.hpp"

namespace sumhp::diagnostic_package {
namespace {
struct Request {
    std::string action, id, revision, input_path;
    bool include_identifiers = false;
};
bool arguments(const std::vector<std::string>& args, Request& out) {
    if (args.size() < 3 || args[0] != "doctor" || args[1] != "package")
        return false;
    Request candidate;
    candidate.action = args[2];
    if (candidate.action == "list") {
        if (args.size() != 3)
            return false;
    } else {
        if (args.size() < 4 || !valid_id(args[3]))
            return false;
        candidate.id = args[3];
        if (candidate.action == "create") {
            if ((args.size() != 6 && args.size() != 7) || args[4] != "--input" ||
                (args.size() == 7 && args[6] != "--include-identifiers"))
                return false;
            candidate.input_path = args[5];
            candidate.include_identifiers = args.size() == 7;
        } else if (candidate.action == "preview") {
            if (args.size() != 4)
                return false;
        } else if (candidate.action == "export" || candidate.action == "remove") {
            if (args.size() != 6 || args[4] != "--revision" || !valid_revision(args[5]))
                return false;
            candidate.revision = args[5];
        } else
            return false;
    }
    out = std::move(candidate);
    return true;
}
Reply failure(const Request& request, const char* reason, int code = 5,
              const char* status = "not_saved") {
    auto value = JsonValue::object();
    value["schema_version"] = 1;
    value["kind"] = "sumh.diagnostic.package.result";
    value["action"] = request.action;
    value["id"] = request.id.empty() ? JsonValue{} : JsonValue(request.id);
    value["status"] = status;
    value["reason"] = reason;
    value["saved"] = false;
    value["deleted"] = false;
    value["durability_confirmed"] = false;
    value["readback_confirmed"] = false;
    value["active_configuration_changed"] = false;
    value["runtime_changed"] = false;
    value["package_revision"] = JsonValue{};
    return {code, std::move(value)};
}
Reply execute_local(const Request& request, const std::string& input) {
    if (request.action == "list")
        return list_packages();
    if (request.action == "preview" || request.action == "export")
        return show_package(request.id, request.revision);
    if (request.action == "remove")
        return remove_package(request.id, request.revision);
    // Resolve a previously lost create reply BEFORE even resampling. IDs remain
    // immutable and a concurrent creator is checked again by the store lock.
    const auto prior = show_package(request.id);
    if (prior.exit_code == 0)
        return failure(request, "package_id_conflict", 4, "conflict");
    if (prior.exit_code != 3 || prior.value.at("status").s != "not_found")
        return failure(request, "store_preflight_failed");
    const auto report = diagnostics::query_diagnostics(input);
    JsonValue package;
    std::string reason;
    if (!build_package(report, request.include_identifiers, package, reason))
        return failure(request, "package_projection_failed");
    return create_package(request.id, package);
}
class Fd {
    int value;

public:
    explicit Fd(int descriptor) : value(descriptor) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&&) = delete;
    Fd& operator=(Fd&&) = delete;
    [[nodiscard]] int get() const { return value; }
    ~Fd() {
        if (value >= 0)
            ::close(value);
    }
};
Reply execute(const Request& request, const std::string& input) {
    const Fd original{::open("/proc/self/ns/mnt", O_RDONLY | O_CLOEXEC)};
    const Fd target{::open("/proc/1/ns/mnt", O_RDONLY | O_CLOEXEC)};
    struct stat own{};
    struct stat init{};
    if (original.get() < 0 || target.get() < 0 || ::fstat(original.get(), &own) != 0 ||
        ::fstat(target.get(), &init) != 0)
        return failure(request, "namespace_unavailable");
    const bool changed = own.st_dev != init.st_dev || own.st_ino != init.st_ino;
    if (changed && ::setns(target.get(), CLONE_NEWNS) != 0)
        return failure(request, "enter_init_namespace_failed");
    class Restore {
        int fd;
        bool needed;

    public:
        Restore(int descriptor, bool should_restore) : fd(descriptor), needed(should_restore) {}
        Restore(const Restore&) = delete;
        Restore& operator=(const Restore&) = delete;
        Restore(Restore&&) = delete;
        Restore& operator=(Restore&&) = delete;
        void dismiss() { needed = false; }
        ~Restore() {
            if (needed)
                (void)::setns(fd, CLONE_NEWNS);
        }
    } restore{original.get(), changed};
    const auto consistent = [&] {
        struct stat current{};
        struct stat expected{};
        return ::stat("/proc/self/ns/mnt", &current) == 0 &&
               ::stat("/proc/1/ns/mnt", &expected) == 0 && current.st_dev == init.st_dev &&
               current.st_ino == init.st_ino && expected.st_dev == init.st_dev &&
               expected.st_ino == init.st_ino;
    };
    Reply out;
    if (!consistent())
        out = failure(request, "namespace_identity_changed");
    else {
        out = execute_local(request, input);
        if (!consistent()) {
            out = failure(request, "namespace_changed_query_known_id", 5, "unknown");
        }
    }
    if (changed && ::setns(original.get(), CLONE_NEWNS) != 0)
        return failure(request, "namespace_restore_failed_query_known_id", 5, "unknown");
    restore.dismiss();
    return out;
}
int emit(const Reply& reply, bool export_payload) {
    std::string bytes;
    if (export_payload && reply.exit_code == 0) {
        const auto& value = reply.value.at("export_bytes");
        if (!value.is_string() || value.s.empty() || value.s.size() > package_limit + 1)
            return 5;
        bytes = value.s;
    } else
        bytes = stringify_json(reply.value) + "\n";
#ifdef SIGPIPE
    // A failed export must leave the retained record available for inspection.
    // Restore the process disposition for a returning embedding caller.
    const auto previous = std::signal(SIGPIPE, SIG_IGN);
    if (previous == SIG_ERR)
        return 5;
#endif
    std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    std::cout.flush();
    const bool ok = std::cout.good();
#ifdef SIGPIPE
    (void)std::signal(SIGPIPE, previous);
#endif
    return ok ? reply.exit_code : 5;
}
}  // namespace

int command(const std::vector<std::string>& args) {
    Request request;
    try {
        if (!arguments(args, request))
            return emit(failure({}, "invalid_arguments", 2, "invalid"), false);
        std::string input;
        std::string reason;
        if (request.action == "create") {
            if (!diagnostics::read_input(request.input_path, input, reason))
                return emit(failure(request, "input_unavailable", 2, "invalid"), false);
            diagnostics::Request source;
            if (!diagnostics::parse(input, source, reason))
                return emit(failure(request, "invalid_diagnostic_request", 2, "invalid"), false);
        }
        return emit(execute(request, input), request.action == "export");
    } catch (...) {
        // An allocation/IO exception can follow a committed package write.
        // Do not leak parser/input bytes or promise that no mutation happened.
        return emit(failure(request, "request_failed_query_known_id", 5, "unknown"), false);
    }
}

std::string manager_dispatch(const diagnostic_package_manager::Request& input) {
    const Request request{input.action, input.id, input.revision, {}, input.include_identifiers};
    Reply reply;
    try {
        if (input.action == "capabilities") {
            auto value = JsonValue::object();
            value["schema_version"] = 1;
            value["kind"] = "sumh.diagnostic.package.capabilities";
            value["package_schema"] = 1;
            value["manager_schema"] = 1;
            for (const auto* name : {"create", "list", "preview", "export", "remove"})
                value[name] = true;
            value["default_privacy"] = "redacted";
            value["identifiers_opt_in"] = true;
            value["package_bytes"] = static_cast<int>(package_limit);
            value["package_count_limit"] = static_cast<int>(package_count_limit);
            value["bank_bytes"] = static_cast<int>(bank_limit);
            value["sampling_read_only"] = true;
            value["storage_mutations"] = true;
            reply = {0, std::move(value)};
        } else
            reply = execute(request, input.diagnostic);
        if (reply.exit_code == 0 && (input.action == "preview" || input.action == "export"))
            reply.value.o.erase(
                "package");  // export_bytes remains the store's exact verified byte string.
    } catch (...) {
        reply = failure(request, "request_failed_query_known_id", 5, "unknown");
    }
    auto dispatch = JsonValue::object();
    dispatch["schema_version"] = 1;
    dispatch["resource"] = "diagnostic_package_dispatch";
    dispatch["action"] = input.action;
    dispatch["id"] = input.id.empty() ? JsonValue{} : JsonValue(input.id);
    dispatch["exit_code"] = reply.exit_code;
    dispatch["result"] = std::move(reply.value);
    return stringify_json(dispatch);
}
}  // namespace sumhp::diagnostic_package
