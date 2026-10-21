#include "core/state_ledger.hpp"
#include "../../../../common/temporary_maps_command.hpp"
#include "core/operation_resource.hpp"
#include "core/runtime.hpp"
#include "core/runtime_status.hpp"
#include "core/user_hide_input.hpp"
#include "core/user_hide_store.hpp"
#include "mount/maps_snapshot.hpp"
#include "sumhp/config.hpp"
#include "sumhp/sumh_client.hpp"
#include "uapi/sumh.h"
#include "utils.hpp"

#include <fcntl.h>
#include <mbedtls/sha256.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <set>

namespace sumhp::state {
namespace {
struct FileRead {
    std::string status;
    std::string bytes;
    std::string error;
};
FileRead read_bounded(const std::filesystem::path& path, std::size_t max) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return {errno == ENOENT ? "missing" : "unreadable", {}, std::strerror(errno)};
    struct stat metadata{};
    if (::fstat(fd, &metadata) != 0) {
        const int code = errno;
        ::close(fd);
        return {"unreadable", {}, std::strerror(code)};
    }
    if (!S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 || metadata.st_size < 0 ||
        static_cast<std::uint64_t>(metadata.st_size) > max) {
        ::close(fd);
        return {"unreadable", {}, "invalid file type/link count or snapshot capacity exceeded"};
    }
    FileRead result{"valid", {}, {}};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    char buffer[4096];
    while (result.bytes.size() <= max) {
        if (std::chrono::steady_clock::now() >= deadline) {
            result.status = "unreadable";
            result.error = "snapshot read budget exceeded";
            break;
        }
        const auto count =
            ::read(fd, buffer, std::min(sizeof(buffer), max + 1 - result.bytes.size()));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            result.status = "unreadable";
            result.error = std::strerror(errno);
            break;
        }
        if (!count)
            break;
        result.bytes.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(fd);
    if (result.bytes.size() > max) {
        result.status = "unreadable";
        result.error = "snapshot capacity exceeded";
    }
    if (result.status != "valid")
        result.bytes.clear();
    return result;
}
using Sources = std::map<std::string, FileRead>;
Sources read_sources() {
    Sources result;
    // This is a snapshot budget, not a new persistence limit: older larger
    // definitions remain on disk and other existing repair paths remain usable.
    for (const char* name :
         {"config.json", "module_rules.json", "module_mode.json", "user_hide_rules.json"})
        result.emplace(name, read_bounded(runtime_data_dir() / name, 4UL * 1024 * 1024));
    return result;
}
std::filesystem::path ledger_path() {
    return runtime_data_dir() / "state_ledger.json";
}
std::string read_ledger(Ledger& value, std::string& error, std::string* original_bytes = nullptr) {
    auto source = read_bounded(ledger_path(), LEDGER_MAX_BYTES);
    if (source.status == "missing")
        return "missing";
    if (source.status != "valid") {
        error = source.error;
        return source.status;
    }
    if (!parse_ledger(source.bytes, value, error))
        return "corrupt_or_unsupported";
    if (original_bytes)
        *original_bytes = source.bytes;
    return "valid";
}
bool write_ledger(const Ledger& ledger, std::string& error) {
    const auto bytes = encode_ledger(ledger);
    Ledger checked;
    if (!parse_ledger(bytes, checked, error)) {
        errno = EINVAL;
        return false;
    }
    if (!ksud::write_file_durable(ledger_path(), bytes, error))
        return false;
    const auto readback = read_bounded(ledger_path(), LEDGER_MAX_BYTES);
    if (readback.status != "valid" || readback.bytes != bytes) {
        error = "state ledger committed but readback not confirmed";
        errno = EIO;
        return false;
    }
    return true;
}
bool backup_legacy_ledger(int schema, const std::string& expected, std::string& error) {
    const auto original = read_bounded(ledger_path(), LEDGER_MAX_BYTES);
    Ledger checked;
    if ((schema != 1 && schema != 2) || original.status != "valid" || original.bytes != expected ||
        !parse_ledger(original.bytes, checked, error) || checked.source_schema != schema) {
        error = "legacy ledger changed before migration";
        errno = EIO;
        return false;
    }
    const auto backup_path =
        runtime_data_dir() / ("state_ledger.schema" + std::to_string(schema) + ".bak");
    const auto existing = read_bounded(backup_path, LEDGER_MAX_BYTES);
    if (existing.status != "missing" &&
        (existing.status != "valid" || existing.bytes != original.bytes)) {
        error = "legacy ledger backup differs or is unreadable; preserving both files";
        errno = EIO;
        return false;
    }
    // Re-sync even an identical backup: a previous replacement may have been
    // visible without a confirmed directory fsync. Never overwrite other bytes.
    if (!ksud::write_file_durable(backup_path, original.bytes, error))
        return false;
    const auto readback = read_bounded(backup_path, LEDGER_MAX_BYTES);
    if (readback.status != "valid" || readback.bytes != original.bytes) {
        error = "legacy ledger backup readback unconfirmed";
        errno = EIO;
        return false;
    }
    return true;
}
bool backup_legacy_operation_resources(const std::string& expected, std::string& error) {
    const auto path = runtime_data_dir() / "operation_resources.json";
    const auto original = read_bounded(path, OPERATION_RESOURCE_MAX_BYTES);
    OperationResources checked;
    int schema = 0;
    if (original.status != "valid" || original.bytes != expected ||
        !parse_operation_resources(original.bytes, checked, error, &schema) || schema != 1) {
        error = "legacy operation resources changed before migration";
        errno = EIO;
        return false;
    }
    const auto backup_path = runtime_data_dir() / "operation_resources.schema1.bak";
    const auto existing = read_bounded(backup_path, OPERATION_RESOURCE_MAX_BYTES);
    if (existing.status != "missing" &&
        (existing.status != "valid" || existing.bytes != expected)) {
        error = "legacy operation resource backup differs or is unreadable; preserving both files";
        errno = EIO;
        return false;
    }
    // Reconfirm durability even when an identical backup is already visible.
    // A prior replacement may have preceded a directory-sync failure.
    if (!ksud::write_file_durable(backup_path, expected, error))
        return false;
    const auto readback = read_bounded(backup_path, OPERATION_RESOURCE_MAX_BYTES);
    if (readback.status != "valid" || readback.bytes != expected) {
        error = "legacy operation resource backup readback unconfirmed";
        errno = EIO;
        return false;
    }
    const auto current = read_bounded(path, OPERATION_RESOURCE_MAX_BYTES);
    if (current.status != "valid" || current.bytes != expected) {
        error = "legacy operation resources changed during backup";
        errno = EIO;
        return false;
    }
    return true;
}
std::string hex(const unsigned char* bytes, std::size_t count) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string value;
    value.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        value += digits[bytes[i] >> 4];
        value += digits[bytes[i] & 15];
    }
    return value;
}
bool new_operation_id(std::string& id, std::string& error) {
    unsigned char bytes[16];
    std::size_t used = 0;
    while (used != sizeof(bytes)) {
        const auto count = ::getrandom(bytes + used, sizeof(bytes) - used, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            error = "cannot allocate operation identity";
            errno = EIO;
            return false;
        }
        used += static_cast<std::size_t>(count);
    }
    id = hex(bytes, sizeof(bytes));
    return true;
}
JsonValue intent(bool enabled) {
    auto value = JsonValue::object();
    value.o["enabled"] = enabled;
    return value;
}
}  // namespace

std::string revision_for_bytes(const std::string& bytes) {
    unsigned char digest[32];
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest,
                       0) != 0)
        return {};
    return "sha256-v1:" + hex(digest, sizeof(digest));
}

Snapshot collect_snapshot() {
    Snapshot result;
    result.boot_id = runtime_boot_id();
    const auto before = read_sources();
    Config config;
    std::string config_error;
    const auto& config_source = before.at("config.json");
    const bool config_ok = config_source.status == "valid" &&
                           parse_config_json(config_source.bytes, config, config_error);
    if (!config_ok && config_error.empty())
        config_error = "configuration " + config_source.status;
    const auto runtime = query_runtime_status();
    JsonValue runtime_json;
    std::string ignored;
    (void)parse_json(runtime_status_json(runtime, result.boot_id), runtime_json, ignored);
    result.runtime = std::move(runtime_json);
    const bool runtime_known = std::string(runtime.status) == "known";
    if (runtime_known) {
        result.instance_id = std::to_string(runtime.runtime.instance_id);
        result.generation = std::to_string(runtime.runtime.generation);
    }
    const auto append = [&](const char* name, const JsonValue& desired) -> FeatureObservation& {
        FeatureObservation item;
        item.id = name;
        if (config_ok)
            item.desired_revision = revision_for_bytes(stringify_json(desired));
        item.reason = config_ok ? "runtime_state_not_available" : config_error;
        result.features.push_back(std::move(item));
        return result.features.back();
    };
    const auto flag = [&](sumh::RuntimeFeature id, bool enabled) {
        auto desired = intent(enabled);
        if (id == sumh::RuntimeFeature::MountHide)
            desired.o["mode"] = config.mount_hide_mode;
        auto& feature = append(sumh::runtime_feature_name(id), desired);
        const auto* actual = runtime.runtime.find(id);
        if (!config_ok || !runtime_known || !actual)
            return;
        feature.detail.o["configured"] = actual->configured;
        feature.detail.o["active"] = actual->active;
        feature.detail.o["last_set_errno"] = actual->last_error;
        if (enabled && !actual->supported) {
            feature.status = "unsupported";
            feature.reason = "required_runtime_machinery_unavailable";
        } else {
            const bool matches = actual->configured == enabled &&
                                 (id != sumh::RuntimeFeature::MountHide ||
                                  runtime.runtime.mount_hide_mode ==
                                      (config.mount_hide_mode == "aggressive" ? 1u : 0u));
            feature.status = matches ? "matches" : "drift";
            if (matches)
                feature.reason =
                    enabled && !actual->active ? "configured_but_inactive" : "setting_verified";
            else
                feature.reason = "saved_setting_differs_from_runtime";
        }
    };
    flag(sumh::RuntimeFeature::Debug, config.enable_kernel_debug);
    flag(sumh::RuntimeFeature::Stealth, config.enable_stealth);
    flag(sumh::RuntimeFeature::MountHide, config.enable_mount_hide);
    flag(sumh::RuntimeFeature::MapsSpoof, config.enable_maps_spoof);
    flag(sumh::RuntimeFeature::StatfsSpoof, config.enable_statfs_spoof);

    auto build_intent = intent(config.enable_kernel_build_spoof);
    build_intent.o["release"] = config.kernel_build_release;
    build_intent.o["version"] = config.kernel_build_version;
    build_intent.o["stage"] = config.kernel_build_apply_stage;
    auto& build = append("kernel_build_spoof", build_intent);
    if (config_ok && runtime_known) {
        sumh::KernelBuild live;
        sumh::KernelBuild original;
        const auto* feature = runtime.runtime.find(sumh::RuntimeFeature::KernelBuild);
        if (!feature || !feature->supported) {
            build.status = "unsupported";
            build.reason = "kernel_build_unavailable";
        } else if (!sumh::kernel_build(live) ||
                   ((!config.enable_kernel_build_spoof || config.kernel_build_release.empty() ||
                     config.kernel_build_version.empty()) &&
                    !sumh::original_kernel_build(original))) {
            build.reason = "kernel_build_query_failed";
        } else if (live.enabled != feature->configured) {
            build.reason = "kernel_build_control_state_inconsistent";
        } else {
            const auto& release =
                !config.enable_kernel_build_spoof || config.kernel_build_release.empty()
                    ? original.release
                    : config.kernel_build_release;
            const auto& version =
                !config.enable_kernel_build_spoof || config.kernel_build_version.empty()
                    ? original.version
                    : config.kernel_build_version;
            const bool matches = live.enabled == config.enable_kernel_build_spoof &&
                                 live.release == release && live.version == version;
            build.status = matches ? "matches" : "drift";
            build.reason = matches ? "both_strings_verified" : "saved_build_differs_from_runtime";
            build.detail.o["configured"] = live.enabled;
            if (!matches && config.enable_kernel_build_spoof &&
                config.kernel_build_apply_stage == "boot-completed" &&
                ksud::getprop("sys.boot_completed") != "1") {
                build.status = "deferred";
                build.reason = "waiting_for_boot_stage";
            }
        }
    }
    auto maps_intent = intent(config.enable_maps_spoof);
    maps_intent.o["rules"] = maps_path_rules_json(config.maps_rules);
    auto& maps = append("maps_rules", maps_intent);
    maps.reason = config_ok ? "managed_identity_query_not_available" : config_error;
    if (config_ok && runtime_known &&
        (runtime.capabilities.bitmask & SUMH_FEATURE_RULE_INVENTORY)) {
        const auto snapshot = mount::sumh::sample_maps(config, false, &runtime.runtime);
        const auto comparison = mount::sumh::compare_maps(config, snapshot);
        maps.status = comparison.status;
        maps.reason = comparison.reason;
        maps.detail.o["verification_scope"] = "registered_managed_maps_vs_current_path_identities";
        maps.detail.o["identity_consistency"] = "twice_sampled_not_pinned";
        maps.detail.o["definitions_complete"] = snapshot.definitions_complete;
        maps.detail.o["identities_sampled"] = snapshot.identities_sampled;
        maps.detail.o["namespace_ino"] = snapshot.namespace_ino
                                             ? JsonValue(std::to_string(snapshot.namespace_ino))
                                             : JsonValue{};
        maps.detail.o["registered_count"] = static_cast<int>(snapshot.registered.size());
        maps.detail.o["errno"] = snapshot.error;
    }
    auto& xattrs = append("overlay_xattr_hide", intent(config.enable_overlay_xattr_hide));
    xattrs.reason = config_ok ? "runtime_inventory_not_available" : config_error;

    FeatureObservation hidden;
    hidden.id = "user_hide";
    // Binding work changes asynchronously without the control generation.
    // A matching revision verifies membership, never simultaneous visibility.
    hidden.detail.o["verification_scope"] = "registered_definitions";
    hidden.detail.o["binding_consistency"] = "per_rule_sampled";
    const auto& hide_file = before.at("user_hide_rules.json");
    hide::Document hide_definitions;
    std::string hide_error;
    auto hide_values = JsonValue::array();
    std::set<std::string> desired_paths;
    bool hide_ok = hide_file.status == "missing";
    if (hide_file.status == "valid")
        hide_ok = hide::parse_document(hide_file.bytes, hide_definitions, hide_error);
    if (hide_ok) {
        for (const auto& definition : hide_definitions.rules)
            if (definition.enabled)
                desired_paths.insert(definition.path);
        for (const auto& path : desired_paths)
            hide_values.a.emplace_back(path);
        hidden.detail.o["enabled_count"] = static_cast<int>(desired_paths.size());
        hidden.detail.o["disabled_count"] =
            static_cast<int>(hide_definitions.rules.size() - desired_paths.size());
    }
    hidden.detail.o["revision_scope"] = "enabled_path_set_only";
    if (hide_ok)
        hidden.desired_revision = revision_for_bytes(stringify_json(hide_values));
    hidden.reason = hide_ok ? "managed_hide_query_not_available"
                            : "saved_hide_definitions_unreadable_or_invalid";
    if (hide_ok && runtime_known && (runtime.capabilities.bitmask & SUMH_FEATURE_MANAGED_HIDE)) {
        std::vector<sumh::UserHideRule> registered;
        if (!sumh::user_hide_rules(registered))
            hidden.reason = "managed_hide_query_failed";
        else {
            std::set<std::string> actual_paths;
            int pending = 0;
            int failed = 0;
            int suspended = 0;
            for (const auto& rule : registered) {
                actual_paths.insert(rule.path);
                if (rule.binding == SUMH_USER_HIDE_ERROR)
                    ++failed;
                else if (rule.binding != SUMH_USER_HIDE_BOUND)
                    ++pending;
                if (rule.management == SUMH_USER_HIDE_SUSPENDED)
                    ++suspended;
            }
            hidden.detail.o["pending_count"] = pending;
            hidden.detail.o["error_count"] = failed;
            hidden.detail.o["suspended_count"] = suspended;
            if (actual_paths != desired_paths || failed)
                hidden.status = "drift";
            else
                hidden.status = pending ? "unknown" : "matches";
            if (actual_paths != desired_paths)
                hidden.reason = "registered_definitions_differ";
            else if (failed)
                hidden.reason = "binding_errors";
            else if (pending)
                hidden.reason = "definitions_registered_binding_pending";
            else
                hidden.reason = suspended ? "registered_definitions_verified_globally_suspended"
                                          : "registered_definitions_verified";
        }
    }
    result.features.push_back(std::move(hidden));
    FeatureObservation modules;
    modules.id = "modules";
    auto definitions = JsonValue::object();
    bool modules_ok = true;
    for (const auto* name : {"module_rules.json", "module_mode.json"}) {
        const auto& file = before.at(name);
        if (file.status != "valid" && file.status != "missing")
            modules_ok = false;
        JsonValue value;
        std::string error;
        if (file.status == "valid" && (!parse_json(file.bytes, value, error) || !value.is_object()))
            modules_ok = false;
        definitions.o[name] = file.status + ":" + file.bytes;
    }
    if (modules_ok)
        modules.desired_revision = revision_for_bytes(stringify_json(definitions));
    modules.reason = modules_ok ? "complete_runtime_mapping_and_source_inventory_required"
                                : "module_definitions_unreadable_or_invalid";
    modules.detail.o["revision_scope"] = "saved_definition_bytes_only";
    modules.detail.o["semantic_validation"] = "requires_production_planner";
    result.features.push_back(std::move(modules));

    bool stable = true;
    if (runtime_known) {
        sumh::RuntimeState end;
        stable = sumh::runtime_state(end) && end.instance_id == runtime.runtime.instance_id &&
                 end.generation == runtime.runtime.generation;
    }
    const auto after = read_sources();
    bool sources_stable = true;
    for (const auto& [name, source] : before) {
        const auto& later = after.at(name);
        if (source.status != later.status || source.bytes != later.bytes)
            sources_stable = false;
    }
    if (result.boot_id != runtime_boot_id())
        stable = false;
    for (auto& item : result.features) {
        if (!sources_stable || !stable) {
            item.status = "unknown";
            item.reason = !sources_stable ? "definitions_changed_during_sample"
                                          : "runtime_changed_during_sample";
        }
        if (item.desired_revision.empty() && item.status == "matches") {
            item.status = "unknown";
            item.reason = "desired_revision_unavailable";
        }
    }
    return result;
}

std::string read_operation_history(Ledger& ledger, std::string& error) {
    error.clear();
    return read_ledger(ledger, error);
}

struct Journal::Impl {
private:
    friend class Journal;
    int lock_fd = -1;
    Ledger ledger;
    bool attempted = false;
    bool active = false;
    std::string id;
    Operation observed;
    bool has_observed = false;
    std::string original_bytes;

public:
    bool load(std::string& status, std::string& error) {
        if (attempted) {
            error = "journal begin already attempted";
            errno = EALREADY;
            return false;
        }
        attempted = true;
        if (!prepare_private_directory(runtime_data_dir(), error))
            return false;
        lock_fd = ::open((runtime_data_dir() / "state_ledger.lock").c_str(),
                         O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        struct stat metadata{};
        if (lock_fd < 0 || ::fstat(lock_fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
            metadata.st_nlink != 1) {
            error = "cannot open private state ledger lock";
            errno = EIO;
            return false;
        }
        // Serialize reservation transitions with every existing journaled
        // mutation; reads never take this lock and no waiter blocks forever.
        if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
            error = "state operation already running or lock unavailable";
            return false;
        }
        status = read_ledger(ledger, error, &original_bytes);
        if (status != "valid" && status != "missing") {
            errno = EIO;
            return false;
        }
        return true;
    }

    bool insert(const std::string& status, const std::string& name, const std::string& revision,
                bool prepared, std::string& error) {
        Operation operation;
        operation.name = name;
        operation.phase = prepared ? "prepared" : "started";
        operation.boot_id = runtime_boot_id();
        operation.request_revision = revision;
        if (operation.boot_id.empty() || !new_operation_id(operation.id, error)) {
            if (error.empty())
                error = "boot identity unavailable; operation not started";
            errno = EIO;
            return false;
        }
        if (operation.id == ledger.last.id ||
            std::any_of(ledger.history.begin(), ledger.history.end(),
                        [&](const Operation& earlier) { return earlier.id == operation.id; })) {
            error = "operation identity collision";
            errno = EEXIST;
            return false;
        }
        Ledger candidate = ledger;
        if (status == "valid")
            candidate.history.push_back(candidate.last);
        candidate.last = std::move(operation);
        candidate.source_schema = 3;
        trim_ledger_history(candidate);
        Ledger validated;
        if (!parse_ledger(encode_ledger(candidate), validated, error)) {
            errno = EINVAL;
            return false;
        }
        if (status == "valid" && ledger.source_schema < 3 &&
            !backup_legacy_ledger(ledger.source_schema, original_bytes, error))
            return false;
        ledger = std::move(candidate);
        if (!write_ledger(ledger, error))
            return false;
        active = !prepared;
        id = ledger.last.id;
        observed = ledger.last;
        has_observed = true;
        error.clear();
        return true;
    }
};
Journal::Journal() : impl_(std::make_unique<Impl>()) {}
Journal::~Journal() {
    if (impl_->lock_fd >= 0)
        ::close(impl_->lock_fd);
}
bool Journal::begin(const std::string& name, std::string& error) {
    std::string status;
    return impl_->load(status, error) && impl_->insert(status, name, {}, false, error);
}
bool Journal::prepare(const std::string& name, const std::string& request_revision,
                      std::string& error) {
    std::string status;
    return impl_->load(status, error) && impl_->insert(status, name, request_revision, true, error);
}
ReservedStart Journal::begin_prepared(const std::string& id, const std::string& name,
                                      const std::string& request_revision, std::string& error) {
    std::string status;
    if (!impl_->load(status, error))
        return ReservedStart::Failed;
    const Operation* selected =
        status == "valid" && impl_->ledger.last.id == id ? &impl_->ledger.last : nullptr;
    std::size_t historical_index = impl_->ledger.history.size();
    for (std::size_t index = 0; !selected && index < impl_->ledger.history.size(); ++index) {
        if (impl_->ledger.history[index].id == id) {
            historical_index = index;
            selected = &impl_->ledger.history[index];
        }
    }
    if (!selected) {
        error = "operation reservation is not retained; it cannot be recreated";
        errno = ENOENT;
        return ReservedStart::NotRetained;
    }
    impl_->observed = *selected;
    impl_->has_observed = true;
    if (selected->request_revision.empty() || selected->request_revision != request_revision ||
        selected->name != name) {
        error = "operation reservation input or command does not match";
        errno = EINVAL;
        return ReservedStart::Conflict;
    }
    const auto boot = runtime_boot_id();
    if (boot.empty() || selected->boot_id != boot) {
        error = "operation reservation belongs to a different or unknown boot";
        errno = EXDEV;
        return ReservedStart::StaleBoot;
    }
    if (selected->phase != "prepared") {
        error = "operation was already started; execution is never repeated";
        errno = EALREADY;
        return ReservedStart::Existing;
    }
    // Promote precisely the selected reservation. Its identity is never
    // recreated and every other retained operation keeps its relative order.
    Operation operation = *selected;
    if (historical_index < impl_->ledger.history.size()) {
        impl_->ledger.history.erase(impl_->ledger.history.begin() + historical_index);
        impl_->ledger.history.push_back(impl_->ledger.last);
    }
    operation.phase = "started";
    impl_->ledger.last = std::move(operation);
    trim_ledger_history(impl_->ledger);
    if (!write_ledger(impl_->ledger, error)) {
        // The replacement may already be visible. Do not expose the earlier
        // prepared observation as a current retry authorization.
        impl_->has_observed = false;
        return ReservedStart::Failed;
    }
    impl_->active = true;
    impl_->id = id;
    impl_->observed = impl_->ledger.last;
    impl_->has_observed = true;
    error.clear();
    return ReservedStart::Started;
}
bool Journal::finish(int exit_code, int error_number, const std::string& detail,
                     std::string& error) {
    if (!impl_->active) {
        error = "journal has no active operation";
        errno = EINVAL;
        return false;
    }
    impl_->active = false;  // A failed finish must not trigger another write/replay.
    finish_ledger(impl_->ledger, collect_snapshot(), exit_code, error_number, detail);
    const bool saved = write_ledger(impl_->ledger, error);
    impl_->has_observed = saved;
    if (saved)
        impl_->observed = impl_->ledger.last;
    return saved;
}
bool Journal::finish_record_only(int exit_code, int error_number, const std::string& detail,
                                 std::string& error) {
    if (!impl_->active) {
        error = "journal has no active operation";
        errno = EINVAL;
        return false;
    }
    impl_->active = false;  // Persistence uncertainty never permits replay.
    finish_ledger(impl_->ledger, Snapshot{}, exit_code, error_number, detail);
    const bool saved = write_ledger(impl_->ledger, error);
    impl_->has_observed = saved;
    if (saved)
        impl_->observed = impl_->ledger.last;
    return saved;
}
bool Journal::retain_resource(const std::vector<std::string>& args, const std::string& output,
                              bool complete, int exit_code, int error_number, std::string& error) {
    if (!impl_->active || impl_->ledger.last.request_revision.empty())
        return true;
    maps_temporary::DeleteRequest maps_request;
    const bool temporary_maps = maps_temporary::parse_tracked_delete(args, maps_request);
    if (!temporary_maps && (!hide::definition_command(args) || hide::definition_query(args[1])))
        return true;
    OperationResource resource;
    if (!make_operation_resource(impl_->ledger.last, args, output, complete, exit_code,
                                 error_number, resource, error))
        return false;
    const auto path = runtime_data_dir() / "operation_resources.json";
    const auto previous = read_bounded(path, OPERATION_RESOURCE_MAX_BYTES);
    OperationResources resources;
    int source_schema = 0;
    if (previous.status != "missing" &&
        (previous.status != "valid" ||
         !parse_operation_resources(previous.bytes, resources, error, &source_schema))) {
        if (error.empty())
            error = previous.error;
        return false;
    }
    std::set<std::string> retained{impl_->ledger.last.id};
    for (const auto& item : impl_->ledger.history)
        retained.insert(item.id);
    resources.erase(std::remove_if(resources.begin(), resources.end(),
                                   [&](const auto& item) { return !retained.count(item.id); }),
                    resources.end());
    if (std::any_of(resources.begin(), resources.end(),
                    [&](const auto& item) { return item.id == resource.id; })) {
        error = "operation resource identity already retained";
        return false;
    }
    resources.push_back(std::move(resource));
    while (resources.size() > OPERATION_RESOURCE_MAX_RECORDS ||
           encode_operation_resources(resources).size() > OPERATION_RESOURCE_MAX_BYTES)
        resources.erase(resources.begin());
    const auto bytes = encode_operation_resources(resources);
    OperationResources checked;
    if (!parse_operation_resources(bytes, checked, error))
        return false;
    if (source_schema == 1 && !backup_legacy_operation_resources(previous.bytes, error))
        return false;
    if (!ksud::write_file_durable(path, bytes, error))
        return false;
    const auto after = read_bounded(path, OPERATION_RESOURCE_MAX_BYTES);
    if (after.status != "valid" || after.bytes != bytes) {
        error = "operation resource committed but readback not confirmed";
        return false;
    }
    error.clear();
    return true;
}

OperationResponse query_operation_resource(const std::string& id) {
    const auto first_boot = runtime_boot_id();
    Ledger ledger;
    std::string error;
    const auto ledger_status = read_ledger(ledger, error);
    auto root = JsonValue::object();
    root.o["schema_version"] = 1;
    root.o["resource"] = "operation_resource_result";
    root.o["read_only"] = true;
    root.o["runtime_verified"] = false;
    root.o["durability_confirmation"] = "not_observable_from_read";
    root.o["status"] = "unavailable";
    root.o["error"] = error;
    root.o["ledger_status"] = ledger_status;
    root.o["receipt_status"] = "not_read";
    root.o["requested_id"] = id;
    root.o["boot_id"] = first_boot.empty() ? JsonValue{} : JsonValue(first_boot);
    root.o["origin_boot_id"] = JsonValue{};
    root.o["same_boot"] = JsonValue{};
    root.o["request_revision"] = JsonValue{};
    root.o["record"] = JsonValue{};
    root.o["result"] = JsonValue{};
    root.o["retention_limit"] = static_cast<int>(OPERATION_RESOURCE_MAX_RECORDS);
    const auto finish = [&](const char* status, int code) -> OperationResponse {
        root.o["status"] = status;
        return {code, std::move(root)};
    };
    if (!valid_operation_id(id)) {
        root.o["error"] = "invalid_operation_id";
        return finish("unavailable", 5);
    }
    if (ledger_status != "valid" && ledger_status != "missing")
        return finish("unavailable", 5);
    const auto sampled_boot = runtime_boot_id();
    if (first_boot.empty() || sampled_boot != first_boot) {
        root.o["boot_id"] = sampled_boot.empty() ? JsonValue{} : JsonValue(sampled_boot);
        return finish("context_changed_or_unknown", 5);
    }
    const auto find = [&](const Ledger& value) -> const Operation* {
        if (value.last.id == id)
            return &value.last;
        for (const auto& item : value.history)
            if (item.id == id)
                return &item;
        return nullptr;
    };
    const Operation* operation = ledger_status == "valid" ? find(ledger) : nullptr;
    if (!operation)
        return finish("not_retained", 3);
    root.o["origin_boot_id"] = operation->boot_id;
    root.o["same_boot"] = operation->boot_id == first_boot;
    root.o["request_revision"] =
        operation->request_revision.empty() ? JsonValue{} : JsonValue(operation->request_revision);
    root.o["record"] = operation_record_json(*operation, first_boot);
    if (operation->phase != "finished")
        return finish("not_finished", 4);
    const auto source =
        read_bounded(runtime_data_dir() / "operation_resources.json", OPERATION_RESOURCE_MAX_BYTES);
    root.o["receipt_status"] = source.status;
    OperationResources resources;
    std::string resource_error;
    if (source.status == "valid" &&
        !parse_operation_resources(source.bytes, resources, resource_error))
        root.o["receipt_status"] = "corrupt_or_unsupported";
    Ledger after;
    std::string after_error;
    const auto after_status = read_ledger(after, after_error);
    const auto current_boot = runtime_boot_id();
    const auto* current = after_status == "valid" ? find(after) : nullptr;
    if (current_boot.empty() || current_boot != first_boot || !current ||
        stringify_json(operation_record_json(*operation, first_boot)) !=
            stringify_json(operation_record_json(*current, current_boot))) {
        root.o["boot_id"] = current_boot.empty() ? JsonValue{} : JsonValue(current_boot);
        root.o["result"] = JsonValue{};
        root.o["record"] = JsonValue{};
        root.o["origin_boot_id"] = JsonValue{};
        root.o["same_boot"] = JsonValue{};
        root.o["request_revision"] = JsonValue{};
        root.o["error"] = "operation context changed while reading resource";
        return finish("context_changed_or_unknown", 5);
    }
    if (source.status == "missing")
        return finish("resource_unavailable", 6);
    if (source.status != "valid" || !resource_error.empty()) {
        root.o["error"] = resource_error.empty() ? source.error : resource_error;
        return finish("unavailable", 5);
    }
    const auto found = std::find_if(resources.begin(), resources.end(),
                                    [&](const auto& item) { return item.id == id; });
    if (found == resources.end()) {
        root.o["receipt_status"] = "not_retained";
        return finish("resource_unavailable", 6);
    }
    if (!operation_resource_matches(*found, *operation)) {
        root.o["receipt_status"] = "mismatch";
        root.o["error"] = "operation resource identity mismatch";
        return finish("unavailable", 5);
    }
    root.o["result"] = found->result;
    return finish("observed", 0);
}
const std::string& Journal::operation_id() const {
    return impl_->id;
}
const Operation* Journal::observed_operation() const {
    return impl_->has_observed ? &impl_->observed : nullptr;
}
int print_state_json() {
    const auto snapshot = collect_snapshot();
    Ledger ledger;
    std::string error;
    const auto status = read_ledger(ledger, error);
    std::cout << stringify_json(
                     state_json(snapshot, status == "valid" ? &ledger : nullptr, status, error))
              << '\n';
    if (status != "valid" && status != "missing") {
        errno = EIO;
        return 1;
    }
    return 0;
}
}  // namespace sumhp::state
