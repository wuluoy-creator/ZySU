#include "../../../../common/utf8.hpp"
#include "core/state_ledger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace sumhp::state {
namespace {
constexpr std::array<const char*, 10> feature_ids = {
    "kernel_debug",       "stealth",    "mount_hide", "maps_spoof", "statfs_spoof",
    "kernel_build_spoof", "maps_rules", "user_hide",  "modules",    "overlay_xattr_hide"};
bool exact_keys(const JsonValue& value, const std::set<std::string>& keys) {
    if (!value.is_object() || value.o.size() != keys.size())
        return false;
    return std::all_of(value.o.begin(), value.o.end(),
                       [&](const auto& item) { return keys.count(item.first) != 0; });
}
bool text_field(const JsonValue& value, const char* key, std::string& out, std::size_t max) {
    const auto* item = value.find(key);
    if (!item || !item->is_string() || item->s.size() > max ||
        item->s.find('\0') != std::string::npos)
        return false;
    out = item->s;
    return true;
}
bool integer_field(const JsonValue& value, const char* key, int& out, int max) {
    const auto* item = value.find(key);
    if (!item || !item->is_number() || !std::isfinite(item->n) || item->n < 0 || item->n > max ||
        std::floor(item->n) != item->n)
        return false;
    out = static_cast<int>(item->n);
    return true;
}
bool decimal(const std::string& value) {
    return !value.empty() && value.size() <= 20 && value.front() != '0' &&
           std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }) &&
           (value.size() < 20 || value <= "18446744073709551615");
}
bool revision(const std::string& value) {
    return value.size() == 74 && value.compare(0, 10, "sha256-v1:") == 0 &&
           std::all_of(value.begin() + 10, value.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
JsonValue operation_json(const Operation& op, bool storage = true) {
    auto value = JsonValue::object();
    value.o["id"] = op.id;
    value.o["name"] = op.name;
    value.o["phase"] = op.phase;
    value.o["boot_id"] = op.boot_id;
    value.o["exit_code"] = op.exit_code;
    value.o["errno"] = op.error_number;
    value.o["detail"] = op.detail;
    if (storage)
        value.o["request_revision"] = op.request_revision;
    return value;
}
bool parse_operation(const JsonValue& value, Operation& op, int schema) {
    return (schema == 3 ? exact_keys(value, {"id", "name", "phase", "boot_id", "exit_code", "errno",
                                             "detail", "request_revision"})
                        : exact_keys(value, {"id", "name", "phase", "boot_id", "exit_code", "errno",
                                             "detail"})) &&
           text_field(value, "id", op.id, 32) && valid_operation_id(op.id) &&
           text_field(value, "name", op.name, 80) && !op.name.empty() &&
           text_field(value, "phase", op.phase, 8) &&
           (op.phase == "started" || op.phase == "finished" ||
            (schema == 3 && op.phase == "prepared")) &&
           text_field(value, "boot_id", op.boot_id, 80) && !op.boot_id.empty() &&
           integer_field(value, "exit_code", op.exit_code, 255) &&
           integer_field(value, "errno", op.error_number, 4095) &&
           text_field(value, "detail", op.detail, 1024) &&
           (schema != 3 || (text_field(value, "request_revision", op.request_revision, 74) &&
                            (op.request_revision.empty() || revision(op.request_revision)))) &&
           (op.phase != "prepared" || (!op.request_revision.empty() && op.exit_code == 0 &&
                                       op.error_number == 0 && op.detail.empty()));
}
JsonValue verification_json(const Verification& record) {
    auto value = JsonValue::object();
    value.o["revision"] = record.revision;
    value.o["boot_id"] = record.boot_id;
    value.o["instance_id"] = record.instance_id;
    value.o["generation"] = record.generation;
    return value;
}
}  // namespace

bool valid_operation_id(const std::string& id) {
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool parse_ledger(const std::string& bytes, Ledger& ledger, std::string& error) {
    JsonValue root;
    const auto reject = [&] {
        error = "invalid or unsupported state ledger; preserving stored bytes";
        return false;
    };
    if (bytes.size() > LEDGER_MAX_BYTES || !ksud::text::valid_utf8(bytes) ||
        !json::Parser(bytes, true).parse(root, error))
        return reject();
    const auto* version = root.find("schema_version");
    if (!version || !version->is_number() ||
        (version->n != 1 && version->n != 2 && version->n != 3))
        return reject();
    if (!(version->n == 1 ? exact_keys(root, {"schema_version", "last_operation", "verified"})
                          : exact_keys(root, {"schema_version", "last_operation", "verified",
                                              "history", "history_pruned"})))
        return reject();
    Ledger parsed;
    parsed.source_schema = static_cast<int>(version->n);
    parsed.history_pruned = parsed.source_schema == 1;
    if (!parse_operation(root.o.at("last_operation"), parsed.last, parsed.source_schema))
        return reject();
    if (parsed.source_schema >= 2) {
        const auto& history = root.o.at("history");
        const auto& pruned = root.o.at("history_pruned");
        if (!history.is_array() || history.a.size() >= LEDGER_MAX_OPERATIONS || !pruned.is_bool())
            return reject();
        parsed.history_pruned = pruned.b;
        std::set<std::string> ids{parsed.last.id};
        for (const auto& value : history.a) {
            Operation item;
            if (!parse_operation(value, item, parsed.source_schema) || !ids.insert(item.id).second)
                return reject();
            parsed.history.push_back(std::move(item));
        }
    }
    const auto& entries = root.o.at("verified");
    if (!entries.is_object() || entries.o.size() > feature_ids.size())
        return reject();
    for (const auto& [id, fields] : entries.o) {
        Verification record;
        if (std::find(feature_ids.begin(), feature_ids.end(), id) == feature_ids.end() ||
            !exact_keys(fields, {"revision", "boot_id", "instance_id", "generation"}) ||
            !text_field(fields, "revision", record.revision, 74) || !revision(record.revision) ||
            !text_field(fields, "boot_id", record.boot_id, 80) || record.boot_id.empty() ||
            !text_field(fields, "instance_id", record.instance_id, 20) ||
            !decimal(record.instance_id) ||
            !text_field(fields, "generation", record.generation, 20) || !decimal(record.generation))
            return reject();
        parsed.verified.emplace(id, std::move(record));
    }
    ledger = std::move(parsed);
    error.clear();
    return true;
}

std::string encode_ledger(const Ledger& ledger) {
    auto root = JsonValue::object();
    root.o["schema_version"] = 3;
    root.o["last_operation"] = operation_json(ledger.last);
    root.o["history"] = JsonValue::array();
    for (const auto& operation : ledger.history)
        root.o["history"].a.push_back(operation_json(operation));
    root.o["history_pruned"] = ledger.history_pruned;
    root.o["verified"] = JsonValue::object();
    for (const auto& [id, record] : ledger.verified)
        root.o["verified"].o[id] = verification_json(record);
    return stringify_json(root) + "\n";
}

void trim_ledger_history(Ledger& ledger) {
    while (!ledger.history.empty() && (ledger.history.size() >= LEDGER_MAX_OPERATIONS ||
                                       encode_ledger(ledger).size() > LEDGER_MAX_BYTES)) {
        ledger.history.erase(ledger.history.begin());
        ledger.history_pruned = true;
    }
}

JsonValue operation_record_json(const Operation& operation, const std::string& current_boot) {
    auto value = operation_json(operation);
    value.o["request_revision"] =
        operation.request_revision.empty() ? JsonValue{} : JsonValue(operation.request_revision);
    if (operation.phase == "prepared")
        value.o["result"] = "not_started";
    else if (operation.phase == "started")
        value.o["result"] = "outcome_unknown";
    else
        value.o["result"] = operation.exit_code == 0 ? "command_completed" : "failed_or_partial";
    value.o["same_boot"] =
        current_boot.empty() ? JsonValue{} : JsonValue(operation.boot_id == current_boot);
    // Persisted intent has no command result. Zero defaults are not success.
    if (operation.phase != "finished") {
        value.o["exit_code"] = JsonValue{};
        value.o["errno"] = JsonValue{};
    }
    return value;
}

void finish_ledger(Ledger& ledger, const Snapshot& current, int exit_code, int error_number,
                   const std::string& detail) {
    ledger.last.phase = "finished";
    ledger.last.exit_code = std::clamp(exit_code, 0, 255);
    ledger.last.error_number = std::clamp(error_number, 0, 4095);
    // Detail is a stable non-sensitive result description, never raw stdout,
    // arbitrary command arguments, file contents or credentials.
    ledger.last.detail = detail;
    for (const auto& entry : current.features) {
        if (entry.status == "matches" && revision(entry.desired_revision) &&
            !current.boot_id.empty() && decimal(current.instance_id) && decimal(current.generation))
            ledger.verified[entry.id] = {entry.desired_revision, current.boot_id,
                                         current.instance_id, current.generation};
    }
    trim_ledger_history(ledger);
}

JsonValue state_json(const Snapshot& current, const Ledger* ledger,
                     const std::string& ledger_status, const std::string& error) {
    auto root = JsonValue::object();
    root.o["schema_version"] = 1;
    root.o["consistency"] = "sampled_definitions_and_kernel_control";
    root.o["boot_id"] = current.boot_id.empty() ? JsonValue{} : JsonValue(current.boot_id);
    root.o["runtime"] = current.runtime;
    auto history = JsonValue::object();
    history.o["status"] = ledger_status;
    history.o["error"] = error;
    const Operation* last_executed =
        ledger && ledger->last.phase != "prepared" ? &ledger->last : nullptr;
    if (ledger && !last_executed) {
        for (auto item = ledger->history.rbegin(); item != ledger->history.rend(); ++item) {
            if (item->phase != "prepared") {
                last_executed = &*item;
                break;
            }
        }
    }
    history.o["last_operation"] =
        last_executed ? operation_json(*last_executed, false) : JsonValue{};
    if (last_executed) {
        if (last_executed->phase == "started")
            history.o["last_operation"].o["result"] = "outcome_unknown";
        else
            history.o["last_operation"].o["result"] =
                last_executed->exit_code == 0 ? "completed" : "failed_or_partial";
    }
    root.o["history"] = std::move(history);
    auto features = JsonValue::object();
    for (const auto& entry : current.features) {
        auto value = JsonValue::object();
        value.o["desired_revision"] =
            entry.desired_revision.empty() ? JsonValue{} : JsonValue(entry.desired_revision);
        value.o["status"] = entry.status;
        value.o["reason"] = entry.reason;
        value.o["detail"] = entry.detail;
        value.o["applied_revision"] = JsonValue{};
        value.o["last_verified"] = JsonValue{};
        if (ledger) {
            const auto found = ledger->verified.find(entry.id);
            if (found != ledger->verified.end()) {
                const auto& verified = found->second;
                value.o["last_verified"] = verification_json(verified);
                // History alone never establishes current runtime state.
                if (entry.status == "matches" && verified.revision == entry.desired_revision &&
                    verified.boot_id == current.boot_id &&
                    verified.instance_id == current.instance_id)
                    value.o["applied_revision"] = verified.revision;
            }
        }
        features.o[entry.id] = std::move(value);
    }
    root.o["features"] = std::move(features);
    return root;
}

bool command_needs_journal(const std::vector<std::string>& args) {
    if (args.empty())
        return false;
    const std::string sub = args.size() > 1 ? args[1] : "";
    if (args[0] == "config")
        return sub == "gen" || sub == "merge-json" || sub == "apply";
    if (args[0] == "hide")
        return sub == "add" || sub == "remove" || sub == "apply" || sub == "create" ||
               sub == "import-id" || sub == "annotate" || sub == "enable" || sub == "disable" ||
               sub == "enable-group" || sub == "disable-group" || sub == "retry" ||
               sub == "retry-path" || sub == "remove-id";
    if (args[0] == "recovery")
        return sub == "reset";
    if (args[0] == "debug")
        return sub == "enable" || sub == "disable" || sub == "stealth";
    if (args[0] == "module") {
        static const std::set<std::string> mutations = {
            "configure", "set-mode", "add-rule",  "remove-rule", "hot-mount", "hot-unmount",
            "add",       "delete",   "mount-all", "normalize",   "unmount"};
        return mutations.count(sub) != 0;
    }
    if (args[0] != "sumh" || sub.empty())
        return false;
    if (sub == "kernel-build")
        return args.size() > 2 && (args[2] == "set" || args[2] == "reset");
    if (sub == "maps")
        return args.size() > 2 && (args[2] == "add-path" || args[2] == "remove-path" ||
                                   args[2] == "apply" || args[2] == "add" || args[2] == "clear");
    static const std::set<std::string> mutations = {
        "clear",      "hide-path",  "delete-rule", "fix-mounts", "hide-overlay-xattrs",
        "mount-hide", "maps-spoof", "statfs-spoof"};
    return mutations.count(sub) != 0;
}
}  // namespace sumhp::state
