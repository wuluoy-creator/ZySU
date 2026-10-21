#include "mount/maps_preview.hpp"
#include "core/json.hpp"
#include "core/runtime.hpp"
#include "core/state_ledger.hpp"
#include "mount/maps_history.hpp"
#include "mount/maps_snapshot.hpp"
#include "uapi/sumh.h"
#include "userspace/common/maps_preview_transport.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>

namespace sumhp::mount::sumh {
namespace {
constexpr std::size_t config_read_budget = 4UL * 1024 * 1024;
struct ConfigRead {
    std::string status = "unreadable";
    std::string bytes;
    int error = 0;
};
bool operator==(const ConfigRead& left, const ConfigRead& right) {
    return left.status == right.status && left.bytes == right.bytes && left.error == right.error;
}
class Close {
    int fd;

public:
    explicit Close(int descriptor) : fd(descriptor) {}
    Close(const Close&) = delete;
    Close& operator=(const Close&) = delete;
    Close(Close&&) = delete;
    Close& operator=(Close&&) = delete;
    ~Close() { ::close(fd); }
};

ConfigRead read_saved_config() {
    // Read the stored bytes without read_config_file's logging-state changes
    // or any persistence preparation. This is a read budget, not a new limit
    // on what other existing configuration maintenance paths may preserve.
    const int fd =
        ::open(runtime_config_file().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return {errno == ENOENT ? "missing" : "unreadable", {}, errno ? errno : EIO};
    const Close close{fd};
    struct stat metadata{};
    if (::fstat(fd, &metadata) != 0)
        return {"unreadable", {}, errno ? errno : EIO};
    if (!S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 || metadata.st_size < 0)
        return {"unreadable", {}, EINVAL};
    if (static_cast<std::uint64_t>(metadata.st_size) > config_read_budget)
        return {"unreadable", {}, EOVERFLOW};
    ConfigRead result{"valid", {}, 0};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    char buffer[4096];
    while (result.bytes.size() <= config_read_budget) {
        if (std::chrono::steady_clock::now() >= deadline)
            return {"unreadable", {}, ETIMEDOUT};
        const auto count = ::read(
            fd, buffer, std::min(sizeof(buffer), config_read_budget + 1 - result.bytes.size()));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return {"unreadable", {}, errno ? errno : EIO};
        if (!count)
            break;
        result.bytes.append(buffer, static_cast<std::size_t>(count));
    }
    if (result.bytes.size() > config_read_budget)
        return {"unreadable", {}, EOVERFLOW};
    return result;
}

JsonValue nullable_text(const std::string& value) {
    return value.empty() ? JsonValue{} : JsonValue(value);
}
JsonValue identity_json(const MapsFileIdentity& value) {
    const char* status = "unknown";
    switch (value.kind) {
    case MapsIdentityKind::Regular:
        status = "regular";
        break;
    case MapsIdentityKind::Missing:
        status = "missing";
        break;
    case MapsIdentityKind::NotRegular:
        status = "not_regular";
        break;
    case MapsIdentityKind::Unrepresentable:
        status = "unrepresentable";
        break;
    case MapsIdentityKind::Error:
        status = "error";
        break;
    case MapsIdentityKind::Unknown:
        break;
    }
    auto out = JsonValue::object();
    out.o["status"] = status;
    out.o["ino"] = value.kind == MapsIdentityKind::Regular ? JsonValue(std::to_string(value.ino))
                                                           : JsonValue{};
    out.o["dev"] = value.kind == MapsIdentityKind::Regular ? JsonValue(std::to_string(value.dev))
                                                           : JsonValue{};
    out.o["errno"] = value.error;
    return out;
}

JsonValue envelope() {
    auto out = JsonValue::object();
    out.o["schema_version"] = 1;
    out.o["kind"] = "sumh.maps.preview";
    out.o["read_only"] = true;
    out.o["status"] = "unknown";
    out.o["reason"] = "not_sampled";
    out.o["errno"] = 0;
    out.o["boot_id"] = JsonValue{};
    out.o["config_revision"] = JsonValue{};
    auto saved = JsonValue::object();
    saved.o["status"] = "unreadable";
    saved.o["enabled"] = JsonValue{};
    saved.o["count"] = 0;
    saved.o["maps_revision"] = JsonValue{};
    out.o["saved"] = std::move(saved);
    auto sample = JsonValue::object();
    for (const auto* key : {"namespace_ino", "instance_id", "generation"})
        sample.o[key] = JsonValue{};
    for (const auto* key : {"identities_sampled", "definitions_complete", "stable"})
        sample.o[key] = false;
    sample.o["comparison"] = "unknown";
    sample.o["reason"] = "not_sampled";
    sample.o["errno"] = 0;
    out.o["sample"] = std::move(sample);
    out.o["rows"] = JsonValue::array();
    out.o["registered_count"] = JsonValue{};
    auto history = JsonValue::object();
    history.o["status"] = "unavailable";
    history.o["reason"] = "submission_history_not_available";
    history.o["latest"] = JsonValue{};
    history.o["confirmed"] = JsonValue{};
    out.o["last_submission"] = std::move(history);
    auto limits = JsonValue::object();
    limits.o["max_rules"] = SUMH_MANAGED_MAPS_MAX_RULES;
    limits.o["definition_encoded_bytes"] = static_cast<int>(MAPS_RULES_MAX_ENCODED_BYTES);
    limits.o["response_bytes"] = static_cast<int>(MAPS_PREVIEW_MAX_BYTES);
    out.o["limits"] = std::move(limits);
    return out;
}

const char* registered_match(const MapsPathRule& definition, const MapsPathSample& row,
                             const MapsSnapshot& sample, bool context_stable) {
    if (!context_stable || !sample.stable || !sample.definitions_complete || !row.stable ||
        row.collision || row.target.kind != MapsIdentityKind::Regular ||
        row.replacement.kind != MapsIdentityKind::Regular)
        return "unknown";
    const auto found =
        std::find_if(sample.registered.begin(), sample.registered.end(), [&](const auto& entry) {
            return entry.target_ino == row.target.ino && entry.target_dev == row.target.dev;
        });
    if (found == sample.registered.end())
        return "missing";
    return found->spoofed_ino == row.replacement.ino && found->spoofed_dev == row.replacement.dev &&
                   found->path2 == definition.replacement_path
               ? "matches"
               : "different";
}

JsonValue observe() {
    auto out = envelope();
    const auto boot_before = runtime_boot_id();
    const auto before = read_saved_config();
    MapsHistory history_before;
    std::string history_error_before;
    const auto history_status_before = read_maps_history(history_before, history_error_before);
    Config config;
    std::string parse_error;
    const bool config_valid =
        before.status == "valid" && parse_config_json(before.bytes, config, parse_error);
    MapsSnapshot snapshot;
    if (config_valid)
        snapshot = sample_maps(config, true);
    MapsHistory history_after;
    std::string history_error_after;
    const auto history_status_after = read_maps_history(history_after, history_error_after);
    const auto after = read_saved_config();
    const auto boot_after = runtime_boot_id();
    const bool config_changed = !(before == after);
    const bool boot_changed = boot_before != boot_after;
    const bool history_changed =
        history_status_before != history_status_after ||
        history_error_before != history_error_after ||
        (history_status_before == "valid" &&
         encode_maps_history(history_before) != encode_maps_history(history_after));
    // Bracketing is deliberately conservative, but cannot detect ABA changes.
    // These are observations, never a CAS plan or a pinned filesystem view.
    const bool context_stable = config_valid && !config_changed && !boot_changed &&
                                !history_changed && !boot_before.empty();
    out.o["boot_id"] = nullable_text(boot_before);
    auto& saved = out.o["saved"];
    saved.o["status"] = before.status;
    if (config_changed)
        saved.o["status"] = "changed";
    else if (before.status == "valid" && !config_valid)
        saved.o["status"] = "invalid";
    if (before.status == "valid")
        out.o["config_revision"] = nullable_text(state::revision_for_bytes(before.bytes));
    if (config_valid) {
        saved.o["enabled"] = config.enable_maps_spoof;
        saved.o["count"] = static_cast<int>(config.maps_rules.size());
        auto desired = JsonValue::object();
        desired.o["enabled"] = config.enable_maps_spoof;
        desired.o["rules"] = maps_path_rules_json(config.maps_rules);
        saved.o["maps_revision"] =
            nullable_text(state::revision_for_bytes(stringify_json(desired)));
    }
    auto& history = out.o["last_submission"];
    history.o["status"] = history_status_before;
    history.o["reason"] = "maps_submission_history_invalid";
    if (history_changed)
        history.o["reason"] = "maps_submission_history_changed";
    else if (history_status_before == "valid")
        history.o["reason"] = "maps_submission_history_valid";
    else if (history_status_before == "missing")
        history.o["reason"] = "maps_submission_history_missing";
    else if (history_status_before == "unreadable")
        history.o["reason"] = "maps_submission_history_unreadable";
    history.o["stable"] = !history_changed;
    if (history_status_before == "valid") {
        if (history_before.latest)
            history.o["latest"] = maps_submission_json(*history_before.latest);
        if (history_before.confirmed)
            history.o["confirmed"] = maps_submission_json(*history_before.confirmed);
    }
    auto& sample = out.o["sample"];
    if (snapshot.namespace_ino)
        sample.o["namespace_ino"] = std::to_string(snapshot.namespace_ino);
    if (snapshot.instance_id)
        sample.o["instance_id"] = std::to_string(snapshot.instance_id);
    if (snapshot.generation)
        sample.o["generation"] = std::to_string(snapshot.generation);
    sample.o["identities_sampled"] = snapshot.identities_sampled;
    sample.o["definitions_complete"] = snapshot.definitions_complete;
    sample.o["stable"] = context_stable && snapshot.stable;
    const auto comparison = compare_maps(config, snapshot);
    sample.o["comparison"] = context_stable ? comparison.status : "unknown";
    sample.o["reason"] = comparison.reason;
    sample.o["errno"] = snapshot.error;
    if (context_stable && snapshot.stable && snapshot.definitions_complete)
        out.o["registered_count"] = static_cast<int>(snapshot.registered.size());
    if (config_valid)
        for (std::size_t i = 0; i < config.maps_rules.size(); ++i) {
            const auto& definition = config.maps_rules[i];
            const MapsPathSample row =
                i < snapshot.paths.size() ? snapshot.paths[i] : MapsPathSample{};
            auto item = JsonValue::object();
            item.o["index"] = static_cast<int>(i);
            item.o["target_path"] = definition.target_path;
            item.o["replacement_path"] = definition.replacement_path;
            item.o["target"] = identity_json(row.target);
            item.o["replacement"] = identity_json(row.replacement);
            item.o["same_target_collision"] = row.collision;
            item.o["stable"] = context_stable && row.stable;
            item.o["registered_match"] =
                registered_match(definition, row, snapshot, context_stable);
            out.o["rows"].a.push_back(std::move(item));
        }
    const auto result = [&](const char* status, const std::string& reason, int error) {
        out.o["status"] = status;
        out.o["reason"] = reason;
        out.o["errno"] = error;
    };
    if (config_changed || boot_changed || history_changed) {
        const char* reason = "maps_submission_history_changed";
        if (config_changed)
            reason = "saved_config_changed";
        else if (boot_changed)
            reason = "boot_id_changed";
        result("stale", reason, ESTALE);
        sample.o["reason"] = reason;
    } else if (!config_valid) {
        result("unknown",
               before.status == "valid" ? "saved_config_invalid" : "saved_config_" + before.status,
               before.error ? before.error : EINVAL);
    } else if (boot_before.empty()) {
        result("unknown", "boot_id_unavailable", EIO);
        sample.o["reason"] = "boot_id_unavailable";
    } else if (out.o["config_revision"].is_null() || saved.o["maps_revision"].is_null()) {
        result("unknown", "revision_unavailable", EIO);
    } else if (!snapshot.stable && snapshot.identities_sampled) {
        result("stale", snapshot.reason, snapshot.error ? snapshot.error : ESTALE);
    } else if (snapshot.stable && snapshot.definitions_complete && snapshot.identities_sampled &&
               snapshot.paths.size() == config.maps_rules.size()) {
        if (history_status_before == "valid" || history_status_before == "missing")
            result("complete", "maps_preview_complete", 0);
        else
            result("partial", history.o["reason"].s, EIO);
    } else {
        result(snapshot.identities_sampled || snapshot.definitions_complete ? "partial" : "unknown",
               snapshot.reason, snapshot.error);
    }
    return out;
}
}  // namespace

bool is_maps_preview_command(const std::vector<std::string>& args) {
    return args.size() == 3 && args[0] == "sumh" && args[1] == "maps" && args[2] == "preview";
}

std::string maps_preview_json() {
    // The inherited socket stays open in the namespace sampler even if this
    // short-lived controller exits. A stuck sampler therefore prevents an
    // unbounded sequence of fresh CLI processes from creating more workers.
    const int guard = preview_transport::acquire_preview_guard();
    if (guard < 0) {
        const int code = errno ? errno : EBUSY;
        auto error = envelope();
        error.o["reason"] = "maps_preview_guard_unavailable";
        error.o["errno"] = code;
        return stringify_json(error);
    }
    const Close close{guard};
    auto bytes = stringify_json(observe());
    if (bytes.size() <= MAPS_PREVIEW_MAX_BYTES)
        return bytes;
    auto error = envelope();
    error.o["reason"] = "maps_preview_response_capacity_exceeded";
    error.o["errno"] = EOVERFLOW;
    return stringify_json(error);
}
}  // namespace sumhp::mount::sumh
