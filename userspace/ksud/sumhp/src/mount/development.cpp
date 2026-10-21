#include "mount/development.hpp"
#include <fcntl.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include "core/checked_document.hpp"
#include "core/partition_config.hpp"
#include "core/rule_inventory.hpp"
#include "core/runtime.hpp"
#include "core/state_ledger.hpp"
#include "mount/partition_discovery.hpp"
#include "mount/query_worker.hpp"

namespace sumhp::mount {
namespace {
class Fd {
    int value;

public:
    explicit Fd(int fd) : value(fd) {}
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
JsonValue exact(const std::string& bytes) {
    return json::parse(inventory::bytes_json(bytes));
}
const char* kind_name(DevelopmentKind kind) {
    switch (kind) {
    case DevelopmentKind::Inspect:
        return "sumh.module.inspect";
    case DevelopmentKind::NormalizePreview:
        return "sumh.module.normalize.plan";
    case DevelopmentKind::PartitionDiscovery:
        return "sumh.partitions.discovery";
    case DevelopmentKind::PartitionSave:
        return "sumh.partitions.save";
    }
    return "sumh.development.unknown";
}
JsonValue failure(DevelopmentKind kind, const char* reason, int code) {
    auto out = JsonValue::object();
    out.o["schema_version"] = 1;
    out.o["kind"] = kind_name(kind);
    out.o["read_only"] = kind != DevelopmentKind::PartitionSave;
    out.o["status"] = "unknown";
    out.o["complete"] = false;
    out.o["stable"] = false;
    out.o["selection_valid"] = false;
    out.o["saved"] = false;
    out.o["applied"] = false;
    out.o["config_revision"] = JsonValue{};
    out.o["config_status"] = "unreadable";
    out.o["saved_partitions"] = JsonValue::array();
    out.o["candidates"] = JsonValue::array();
    out.o["saved_partitions_complete"] = false;
    out.o["requires_reboot"] = true;
    out.o["reason"] = reason;
    out.o["errno"] = code;
    out.o["readback_confirmed"] = false;
    out.o["durability_confirmed"] = false;
    out.o["previous_revision"] = JsonValue{};
    out.o["detail"] = exact("");
    out.o["scope"] = "installed_module_candidates_not_mount_readiness";
    out.o["boot_id"] = JsonValue{};
    out.o["namespace_ino"] = JsonValue{};
    out.o["issues"] = JsonValue::array();
    return out;
}
JsonValue discover() {
    auto out = failure(DevelopmentKind::PartitionDiscovery, "not_sampled", 0);
    const auto before = checked::read_document(runtime_config_file());
    Config config;
    std::string error;
    const bool config_ok =
        before.status == "valid" && parse_partition_config(before.bytes, config, error);
    const auto boot = runtime_boot_id();
    PartitionDiscovery first;
    PartitionDiscovery second;
    std::string first_error;
    std::string second_error;
    const bool first_ok = discover_extra_partitions(config.partitions, first, first_error);
    const bool second_ok = discover_extra_partitions(config.partitions, second, second_error);
    if (first.output.is_object())
        out = first.output;
    const auto revision = config_ok ? state::revision_for_bytes(before.bytes) : std::string{};
    const bool stable =
        before == checked::read_document(runtime_config_file()) && !boot.empty() &&
        boot == runtime_boot_id() && first.observation_signature == second.observation_signature &&
        stringify_json(first.output) == stringify_json(second.output) && first_ok == second_ok;
    out.o["schema_version"] = 1;
    out.o["kind"] = kind_name(DevelopmentKind::PartitionDiscovery);
    out.o["read_only"] = true;
    out.o["requires_reboot"] = true;
    out.o["stable"] = stable;
    out.o["boot_id"] = boot.empty() ? JsonValue{} : JsonValue(boot);
    out.o["config_status"] = before.status;
    if (config_ok)
        out.o["config_status"] = "valid";
    else if (before.status == "valid")
        out.o["config_status"] = "invalid";
    out.o["config_revision"] = revision.empty() ? JsonValue{} : JsonValue(revision);
    // Keep the scanner's bounded export. An oversized legacy config must not
    // reintroduce an unbounded array after the scanner already marked it partial.
    if (!out.find("saved_partitions"))
        out.o["saved_partitions"] = JsonValue::array();
    out.o["config_error"] = exact(error);
    const bool valid = stable && config_ok && !revision.empty() && first_ok && second_ok &&
                       first.complete && second.complete;
    out.o["selection_valid"] = valid;
    out.o["complete"] = first_ok && second_ok && first.complete && second.complete;
    out.o["status"] = "partial";
    if (!stable)
        out.o["status"] = "stale";
    else if (!config_ok || revision.empty())
        out.o["status"] = "unknown";
    else if (valid)
        out.o["status"] = "complete";
    out.o["reason"] = "discovery_incomplete";
    if (!stable)
        out.o["reason"] = "inputs_changed";
    else if (!config_ok)
        out.o["reason"] = "config_unavailable";
    else if (valid)
        out.o["reason"] = "candidates_only_not_saved";
    out.o["consistency"] = "twice_observed_metadata_not_pinned_or_mount_readiness";
    return out;
}
JsonValue normalize_preview(const std::string& path) {
    magic::NormalizationPlan first;
    magic::NormalizationPlan second;
    std::string error;
    std::string second_error;
    const bool ok = magic::prepare_normalization(path, first, error);
    const bool checked = magic::prepare_normalization(path, second, second_error);
    auto out = failure(DevelopmentKind::NormalizePreview, "normalization_preflight", 0);
    out.o["module"] = exact(path);
    out.o["plan"] = normalization_plan_json(first);
    const bool stable = ok == checked && error == second_error &&
                        stringify_json(normalization_plan_json(first)) ==
                            stringify_json(normalization_plan_json(second));
    out.o["stable"] = stable;
    out.o["complete"] = ok && checked && stable;
    out.o["status"] = ok ? "complete" : "invalid";
    if (!stable)
        out.o["status"] = "stale";
    out.o["detail"] = exact(error);
    out.o["execution_token"] = JsonValue{};
    out.o["execution_note"] =
        "explicit_normalize_regenerates_and_checks_plan_not_a_persistent_transaction";
    return out;
}
struct SharedReply {
    std::uint32_t done = 0, size = 0;
    char bytes[DEVELOPMENT_RESPONSE_BYTES]{};
};
}  // namespace

JsonValue normalization_plan_json(const magic::NormalizationPlan& plan) {
    auto out = JsonValue::object();
    out.o["complete"] = plan.complete;
    out.o["requested_root"] = exact(plan.requested_root);
    out.o["resolved_root"] = exact(plan.resolved_root);
    auto actions = JsonValue::array();
    for (const auto& item : plan.actions) {
        auto row = JsonValue::object();
        row.o["kind"] = item.kind;
        row.o["source"] = exact(item.source);
        row.o["target"] = exact(item.target);
        row.o["link_target"] = exact(item.link_target);
        row.o["reason"] = exact(item.reason);
        actions.a.push_back(std::move(row));
    }
    out.o["actions"] = std::move(actions);
    auto observations = JsonValue::array();
    for (const auto& item : plan.observations) {
        auto row = JsonValue::object();
        row.o["path"] = exact(item.path);
        row.o["kind"] = item.kind;
        row.o["device"] = std::to_string(item.device);
        row.o["inode"] = std::to_string(item.inode);
        row.o["mode"] = std::to_string(item.mode);
        row.o["size"] = std::to_string(item.size);
        row.o["mtime_seconds"] = std::to_string(item.mtime_seconds);
        row.o["mtime_nanoseconds"] = std::to_string(item.mtime_nanoseconds);
        row.o["ctime_seconds"] = std::to_string(item.ctime_seconds);
        row.o["ctime_nanoseconds"] = std::to_string(item.ctime_nanoseconds);
        row.o["link_target"] = exact(item.link_target);
        observations.a.push_back(std::move(row));
    }
    out.o["observations"] = std::move(observations);
    out.o["metadata_bytes"] = static_cast<int>(plan.bytes);
    return out;
}

JsonValue normalize_module_result_json(const std::string& path) {
    magic::NormalizationPlan plan;
    magic::NormalizationResult result;
    std::string error;
    const bool prepared = magic::prepare_normalization(path, plan, error);
    const bool ok = prepared && magic::execute_normalization(plan, result, error);
    auto out = JsonValue::object();
    out.o["schema_version"] = 1;
    out.o["kind"] = "sumh.module.normalize.result";
    out.o["read_only"] = false;
    out.o["ok"] = ok;
    out.o["module"] = path;
    out.o["status"] = prepared ? result.status : "rejected";
    out.o["changed"] = result.changed;
    out.o["complete"] = result.complete;
    out.o["detail"] = exact(error);
    out.o["recovery_hint"] = exact(result.recovery_hint);
    out.o["plan"] = normalization_plan_json(plan);
    auto actions = JsonValue::array();
    for (const auto& item : result.actions) {
        auto row = JsonValue::object();
        row.o["index"] = static_cast<int>(item.index);
        row.o["status"] = item.status;
        row.o["errno"] = item.error_number;
        row.o["detail"] = exact(item.detail);
        row.o["effect_observed"] = item.effect_observed;
        row.o["durability_confirmed"] = item.durability_confirmed;
        actions.a.push_back(std::move(row));
    }
    out.o["results"] = std::move(actions);
    return out;
}

std::string development_query_json(const DevelopmentRequest& request) {
    const auto fail = [&](const char* reason, int code) {
        return stringify_json(failure(request.kind, reason, code));
    };
    if (request.kind == DevelopmentKind::PartitionSave)
        return fail("mutation_forbidden_in_query_worker", EINVAL);
    const Fd guard{::open(runtime_data_dir().c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    struct stat directory{};
    if (guard.get() < 0 || ::fstat(guard.get(), &directory) != 0 || !S_ISDIR(directory.st_mode) ||
        directory.st_uid != 0 || (directory.st_mode & 0077) != 0)
        return fail("private_query_guard_unavailable", errno ? errno : EACCES);
    if (::flock(guard.get(), LOCK_EX | LOCK_NB) != 0)
        return fail("development_query_busy", errno ? errno : EBUSY);
    auto* shared = static_cast<SharedReply*>(::mmap(
        nullptr, sizeof(SharedReply), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (shared == MAP_FAILED)
        return fail("reply_memory_unavailable", errno ? errno : ENOMEM);
    shared->done = 0;
    shared->size = 0;
    const bool ok = fsutil::run_query_in_init_mount_ns([&] {
        struct stat init_directory{};
        struct stat own_ns{};
        struct stat init_ns{};
        if (::stat(runtime_data_dir().c_str(), &init_directory) != 0 ||
            init_directory.st_ino != directory.st_ino ||
            init_directory.st_dev != directory.st_dev ||
            ::stat("/proc/self/ns/mnt", &own_ns) != 0 || ::stat("/proc/1/ns/mnt", &init_ns) != 0 ||
            own_ns.st_ino != init_ns.st_ino || own_ns.st_dev != init_ns.st_dev) {
            errno = ESTALE;
            return false;
        }
        JsonValue out;
        if (request.kind == DevelopmentKind::PartitionDiscovery)
            out = discover();
        else if (request.kind == DevelopmentKind::NormalizePreview)
            out = normalize_preview(request.module_path);
        else
            out = module_inspect_observation(request.module_path);
        out.o["namespace_ino"] = std::to_string(own_ns.st_ino);
        auto bytes = stringify_json(out);
        if (bytes.size() > DEVELOPMENT_RESPONSE_BYTES)
            bytes = fail("development_response_capacity", EOVERFLOW);
        std::memcpy(shared->bytes, bytes.data(), bytes.size());
        shared->size = static_cast<std::uint32_t>(bytes.size());
        shared->done = 1;
        return true;
    });
    const int code = errno;
    auto result = ok && shared->done == 1 && shared->size <= DEVELOPMENT_RESPONSE_BYTES
                      ? std::string(shared->bytes, shared->size)
                      : fail("development_query_failed", code ? code : EIO);
    ::munmap(shared, sizeof(SharedReply));
    return result;
}

std::string partition_save_json(const DevelopmentRequest& request) {
    const auto fail = [&](const char* reason, int code) {
        return stringify_json(failure(DevelopmentKind::PartitionSave, reason, code));
    };
    if (request.kind != DevelopmentKind::PartitionSave)
        return fail("invalid_save_request", EINVAL);
    // Standalone exact route only. No fork/cancellable read-only worker and no daemon startup.
    const Fd original{::open("/proc/self/ns/mnt", O_RDONLY | O_CLOEXEC)};
    const Fd target{::open("/proc/1/ns/mnt", O_RDONLY | O_CLOEXEC)};
    if (original.get() < 0 || target.get() < 0)
        return fail("namespace_unavailable", errno ? errno : EIO);
    struct stat own{};
    struct stat init{};
    if (::fstat(original.get(), &own) != 0 || ::fstat(target.get(), &init) != 0)
        return fail("namespace_identity_unknown", errno ? errno : EIO);
    const bool changed = own.st_dev != init.st_dev || own.st_ino != init.st_ino;
    if (changed && ::setns(target.get(), CLONE_NEWNS) != 0)
        return fail("enter_init_namespace_failed", errno ? errno : EIO);
    class Restore {
        int fd;
        bool needed;

    public:
        Restore(int descriptor, bool changed) : fd(descriptor), needed(changed) {}
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
    auto out = save_partitions_conditional(request.revision, request.partitions);
    if (changed && ::setns(original.get(), CLONE_NEWNS) != 0) {
        out.o["namespace_restore_failed"] = true;
        out.o["namespace_restore_errno"] = errno ? errno : EIO;
    } else
        restore.dismiss();
    return stringify_json(out);
}
}  // namespace sumhp::mount
