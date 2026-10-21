#include "core/diagnostics.hpp"
#include "core/diagnostics_model.hpp"
#include "core/operation_query.hpp"
#include "core/runtime.hpp"
#include "core/user_hide.hpp"
#include "mount/backend.hpp"
#include "mount/maps_preview.hpp"
#include "mount/module_preview.hpp"
#include "mount/module_rebuild.hpp"
#include "userspace/common/maps_preview_transport.hpp"

#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <stdexcept>

namespace sumhp::diagnostics {
namespace {
class OwnedFd {
    int fd;

public:
    explicit OwnedFd(int descriptor) : fd(descriptor) {}
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    OwnedFd(OwnedFd&&) = delete;
    OwnedFd& operator=(OwnedFd&&) = delete;
    [[nodiscard]] int get() const { return fd; }
    ~OwnedFd() {
        if (fd >= 0)
            ::close(fd);
    }
};

std::string monotonic_ms() {
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec < 0 || now.tv_nsec < 0 ||
        now.tv_nsec >= 1'000'000'000 ||
        static_cast<std::uint64_t>(now.tv_sec) > (UINT64_MAX - 999) / 1000)
        return {};
    return std::to_string((static_cast<std::uint64_t>(now.tv_sec) * 1000) +
                          (static_cast<std::uint64_t>(now.tv_nsec) / 1'000'000));
}

bool same_namespace(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
bool enter_init_namespace(int target, struct stat& identity) {
    struct stat current{};
    if (::fstat(target, &identity) || ::stat("/proc/self/ns/mnt", &current))
        return false;
    if (!same_namespace(current, identity) && ::setns(target, CLONE_NEWNS))
        return false;
    if (::stat("/proc/self/ns/mnt", &current))
        return false;
    if (!same_namespace(current, identity)) {
        errno = ESTALE;
        return false;
    }
    return true;
}
bool namespace_unchanged(const struct stat& identity) {
    struct stat current{};
    struct stat init{};
    return ::stat("/proc/self/ns/mnt", &current) == 0 && ::stat("/proc/1/ns/mnt", &init) == 0 &&
           same_namespace(current, identity) && same_namespace(init, identity);
}

// This selector reads the complete persistent definition list, then asks the
// existing status producer for the exact cursor/revision. It never paginates a
// changing list or mistakes an absent first page for an absent definition.
state::OperationResponse hide_selection(const Request& request) {
    auto value = JsonValue::object();
    value.o["schema_version"] = 1;
    value.o["resource"] = "diagnostic_hide_selection";
    value.o["definition_id"] = request.definition_id;
    value.o["selection_status"] = "unavailable";
    value.o["revision"] = JsonValue{};
    value.o["saved_definition"] = JsonValue{};
    value.o["status_page"] = JsonValue{};
    value.o["definition_store_stable"] = false;
    hide::Stored before;
    hide::Stored after;
    std::string error;
    if (!hide::read_store(before, error))
        return {1, std::move(value)};
    value.o["revision"] = before.revision;
    const auto selected = std::find_if(
        before.document.rules.begin(), before.document.rules.end(),
        [&](const auto& definition) { return definition.id == request.definition_id; });
    if (selected == before.document.rules.end()) {
        if (!hide::read_store(after, error))
            return {1, std::move(value)};
        const bool stable = before.revision == after.revision;
        value.o["selection_status"] = stable ? "absent" : "stale";
        value.o["definition_store_stable"] = stable;
        return {stable ? 0 : 1, std::move(value)};
    }
    hide::Request query;
    query.action = "status";
    query.revision = before.revision;
    query.cursor = static_cast<std::size_t>(selected - before.document.rules.begin());
    const auto status = hide::query_status(query);
    const auto* observed = status.value.find("status");
    const auto* revision = status.value.find("revision");
    const bool stable = observed && observed->is_string() && observed->s == "observed" &&
                        revision && revision->is_string() && revision->s == before.revision;
    auto definition = JsonValue::object();
    definition.o["id"] = selected->id;
    definition.o["enabled"] = selected->enabled;
    value.o["saved_definition"] = std::move(definition);
    value.o["status_page"] = status.value;
    value.o["definition_store_stable"] = stable;
    if (stable)
        value.o["selection_status"] = "found";
    else if (observed && observed->is_string() &&
             (observed->s == "stale_or_invalid_cursor" || observed->s == "changed_or_unavailable"))
        value.o["selection_status"] = "stale";
    else
        value.o["selection_status"] = "unavailable";
    return {stable ? status.exit_code : 1, std::move(value)};
}

template <typename Callback>
Sample sample(SourceKind kind, std::size_t limit, const Callback& callback) {
    Sample result;
    result.kind = kind;
    result.started_ms = monotonic_ms();
    if (result.started_ms.empty()) {
        result.failure = "sample_clock_unavailable";
        return result;
    }
    try {
        auto response = callback();
        result.query_exit_code = response.exit_code;
        result.bytes = stringify_json(response.value);
        if (result.bytes.size() > limit) {
            result.bytes.clear();
            result.failure = "source_response_capacity";
            result.error_number = EOVERFLOW;
        }
    } catch (...) {
        result.bytes.clear();
        result.failure = "source_query_failed";
        result.error_number = EIO;
    }
    result.finished_ms = monotonic_ms();
    if (result.finished_ms.empty()) {
        result.bytes.clear();
        result.failure = "sample_clock_unavailable";
        result.error_number = EIO;
    }
    return result;
}

// Parse producer bytes strictly before passing their object through the shared
// typed source adapter. Legacy recovery is intentionally a distinct SourceKind.
state::OperationResponse produced(const std::string& bytes, std::size_t limit, int exit_code = 0) {
    JsonValue value;
    std::string error;
    if (bytes.size() > limit || bytes.find('\0') != std::string::npos ||
        !ksud::text::valid_utf8(bytes) || !json::Parser(bytes, true).parse(value, error) ||
        !value.is_object())
        throw std::runtime_error("source_contract_invalid");
    return {exit_code, std::move(value)};
}

Report invalid(const char* reason) {
    auto report = unavailable(nullptr, reason, EINVAL);
    report.exit_code = 2;
    report.value.o["status"] = "invalid";
    report.value.o["exit_code"] = 2;
    return report;
}
int emit_report(const Report& report) {
    const auto bytes = encode_report(report);
    std::cout << bytes << '\n';
    if (!std::cout.good())
        return 5;
    // Encoding may replace an oversized report with a small unavailable result.
    JsonValue value;
    std::string error;
    if (!json::Parser(bytes, true).parse(value, error))
        return 5;
    const auto* code = value.find("exit_code");
    return code && code->is_number() ? static_cast<int>(code->n) : 5;
}

bool report_contract(const JsonValue& value, const Request& request, int& exit_code) {
    const auto text = [&](const char* key, const char* expected) {
        const auto* field = value.find(key);
        return field && field->is_string() && field->s == expected;
    };
    const auto boolean = [&](const char* key, bool expected) {
        const auto* field = value.find(key);
        return field && field->is_bool() && field->b == expected;
    };
    const auto* version = value.find("schema_version");
    const auto* code = value.find("exit_code");
    const auto* status = value.find("status");
    const char* scenario = scenario_name(request.scenario);
    if (!value.is_object() || !version || !version->is_number() || version->n != 1 ||
        !text("resource", "sumh_diagnostics") || !text("scenario", scenario) ||
        !boolean("read_only", true) || !boolean("mutations_performed", false) ||
        !boolean("observer_verified", false) || !boolean("atomic", false) || !code ||
        !code->is_number() || !status || !status->is_string())
        return false;
    if ((status->s == "observed" && code->n == 0) || (status->s == "invalid" && code->n == 2) ||
        (status->s == "partial" && code->n == 3) || (status->s == "stale" && code->n == 4) ||
        (status->s == "unavailable" && code->n == 5)) {
        exit_code = static_cast<int>(code->n);
        return true;
    }
    return false;
}
}  // namespace

std::string collect_diagnostics(const Request& request) {
    Sources sources;
    sources.context.started_ms = monotonic_ms();
    const OwnedFd init_namespace{::open("/proc/1/ns/mnt", O_RDONLY | O_CLOEXEC)};
    struct stat identity{};
    if (sources.context.started_ms.empty() || init_namespace.get() < 0 ||
        !enter_init_namespace(init_namespace.get(), identity))
        return encode_report(
            unavailable(&request, "init_namespace_unavailable", errno ? errno : EIO));
    sources.context.namespace_verified = true;
    sources.context.namespace_dev = std::to_string(static_cast<std::uint64_t>(identity.st_dev));
    sources.context.namespace_ino = std::to_string(static_cast<std::uint64_t>(identity.st_ino));
    sources.context.boot_before = runtime_boot_id();
    switch (request.scenario) {
    case Scenario::UserHide:
        sources.primary = sample(SourceKind::HideSelection, response_bytes,
                                 [&] { return hide_selection(request); });
        break;
    case Scenario::ModuleFallback:
        sources.primary = sample(SourceKind::ModulePlan, mount::MODULE_PREVIEW_RESPONSE_BYTES, [&] {
            return produced(mount::module_preview_json({}), mount::MODULE_PREVIEW_RESPONSE_BYTES);
        });
        sources.rebuild =
            sample(SourceKind::ModuleRebuild, (2 * mount::sumh::MODULE_REBUILD_MAX_BYTES) + 4096,
                   [] { return mount::sumh::query_module_rebuild_status(); });
        break;
    case Scenario::MapsFailure:
        sources.primary = sample(SourceKind::MapsPreview, 512UL * 1024UL, [] {
            return produced(mount::sumh::maps_preview_json(), 512UL * 1024UL);
        });
        break;
    case Scenario::BootProtection:
        sources.primary = sample(SourceKind::RecoveryLegacy, 64UL * 1024UL, [] {
            bool ok = false;
            const auto bytes = mount::recovery_status_json(&ok);
            return produced(bytes, 64UL * 1024UL, ok ? 0 : 1);
        });
        break;
    }
    sources.operations = sample(SourceKind::Operations, 64UL * 1024UL, [] {
        state::OperationQueryRequest query;
        query.kind = state::OperationQueryRequest::Kind::List;
        return state::query_operation(query);
    });
    if (!request.operation_id.empty()) {
        sources.selected_operation = sample(SourceKind::OperationSelected, 64UL * 1024UL, [&] {
            state::OperationQueryRequest query;
            query.kind = state::OperationQueryRequest::Kind::Show;
            query.id = request.operation_id;
            return state::query_operation(query);
        });
    }
    sources.context.boot_after = runtime_boot_id();
    sources.context.namespace_consistent = namespace_unchanged(identity);
    sources.context.finished_ms = monotonic_ms();
    return encode_report(diagnose(request, sources));
}

std::string query_diagnostics(const std::string& bytes) {
    std::string reason;
    Request request;
    if (!parse(bytes, request, reason))
        return encode_report(invalid(reason.c_str()));
    const auto envelope = preview_transport::request_diagnostics(bytes);
    JsonValue transport;
    std::string error;
    if (envelope.size() > envelope_bytes || !ksud::text::valid_utf8(envelope) ||
        !json::Parser(envelope, true).parse(transport, error) || !transport.is_object())
        return encode_report(unavailable(&request, "transport_contract_invalid", EPROTO));
    const auto* ok = transport.find("ok");
    const auto* output = transport.find("stdout");
    const auto* code = transport.find("errno");
    if (!ok || !ok->is_bool() || !output || !output->is_string() || !code || !code->is_number() ||
        code->n < 0 || code->n > 4095 || code->n != static_cast<int>(code->n))
        return encode_report(unavailable(&request, "transport_contract_invalid", EPROTO));
    if (!ok->b)
        return encode_report(
            unavailable(&request, "transport_unavailable", static_cast<int>(code->n)));
    JsonValue report;
    int exit_code = 5;
    if (code->n != 0 || output->s.size() > response_bytes || !ksud::text::valid_utf8(output->s) ||
        std::any_of(output->s.begin(), output->s.end(), [](unsigned char c) { return c < 0x20; }) ||
        !json::Parser(output->s, true).parse(report, error) ||
        !report_contract(report, request, exit_code))
        return encode_report(unavailable(&request, "response_contract_invalid", EPROTO));
    return output->s;
}

int doctor_command(const std::vector<std::string>& args) {
    if (args.size() != 3 || args[0] != "doctor" || args[1] != "--input")
        return emit_report(invalid("invalid_arguments"));
    std::string bytes;
    std::string reason;
    if (!read_input(args[2], bytes, reason))
        return emit_report(invalid(reason.c_str()));
    const auto output = query_diagnostics(bytes);
    JsonValue report;
    if (!json::Parser(output, true).parse(report, reason) || !report.at("exit_code").is_number())
        return emit_report(unavailable(nullptr, "response_contract_invalid", EPROTO));
    const auto exit_code = static_cast<int>(report.at("exit_code").n);
    std::cout << output << '\n';
    return std::cout.good() ? exit_code : 5;
}
}  // namespace sumhp::diagnostics
