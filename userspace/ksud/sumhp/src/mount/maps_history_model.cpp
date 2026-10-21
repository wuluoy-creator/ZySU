#include <algorithm>
#include <cmath>
#include <set>
#include "mount/maps_history.hpp"

namespace sumhp::mount::sumh {
namespace {
bool keys(const JsonValue& value, const std::set<std::string>& expected) {
    if (!value.is_object() || value.o.size() != expected.size())
        return false;
    return std::all_of(value.o.begin(), value.o.end(),
                       [&](const auto& entry) { return expected.count(entry.first); });
}
bool text(const JsonValue& value, const char* key, std::string& output, std::size_t limit) {
    const auto* item = value.find(key);
    if (!item || !item->is_string() || item->s.size() > limit ||
        item->s.find('\0') != std::string::npos)
        return false;
    output = item->s;
    return true;
}
bool decimal(const std::string& value) {
    return !value.empty() && value.size() <= 20 && value.front() != '0' &&
           std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }) &&
           (value.size() < 20 || value <= "18446744073709551615");
}
bool token(const JsonValue& value, const char* key, std::string& output, bool optional = false) {
    return text(value, key, output, 20) && ((optional && output.empty()) || decimal(output));
}
bool hex(const std::string& value, std::size_t length) {
    return value.size() == length && std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
bool parse_submission(const JsonValue& value, MapsSubmission& out) {
    if (!keys(value, {"id", "boot_id", "definition_revision", "namespace_ino", "instance_id",
                      "generation_before", "generation_after", "phase", "errno", "rows",
                      "supersedes_pending_id"}) ||
        !text(value, "id", out.id, 32) || !hex(out.id, 32) ||
        !text(value, "supersedes_pending_id", out.supersedes_pending_id, 32) ||
        (!out.supersedes_pending_id.empty() &&
         (!hex(out.supersedes_pending_id, 32) || out.supersedes_pending_id == out.id)) ||
        !text(value, "boot_id", out.boot_id, 80) || out.boot_id.empty() ||
        !text(value, "definition_revision", out.definition_revision, 74) ||
        out.definition_revision.size() != 74 ||
        out.definition_revision.substr(0, 10) != "sha256-v1:" ||
        !hex(out.definition_revision.substr(10), 64) ||
        !token(value, "namespace_ino", out.namespace_ino) ||
        !token(value, "instance_id", out.instance_id, true) ||
        !token(value, "generation_before", out.generation_before, true) ||
        !token(value, "generation_after", out.generation_after, true) ||
        out.instance_id.empty() != out.generation_before.empty() ||
        (!out.generation_after.empty() && out.instance_id.empty()) ||
        !text(value, "phase", out.phase, 16) ||
        (out.phase != "pending" && out.phase != "acknowledged" && out.phase != "rejected"))
        return false;
    const auto* code = value.find("errno");
    const auto* rows = value.find("rows");
    if (!code || !code->is_number() || !std::isfinite(code->n) || std::floor(code->n) != code->n ||
        code->n < 0 || code->n > 4095 || !rows || !rows->is_array() || rows->a.size() > 256)
        return false;
    out.error = static_cast<int>(code->n);
    if ((out.phase == "rejected") != (out.error != 0) ||
        (out.phase != "acknowledged" && !out.generation_after.empty()))
        return false;
    std::set<std::pair<std::uint64_t, std::uint64_t>> targets;
    for (const auto& row : rows->a) {
        MapsPathRule paths;
        std::string ino;
        std::string dev;
        std::string spoof_ino;
        std::string spoof_dev;
        if (!keys(row, {"target_path", "replacement_path", "target_ino", "target_dev",
                        "spoofed_ino", "spoofed_dev"}) ||
            !text(row, "target_path", paths.target_path, 4095) ||
            !text(row, "replacement_path", paths.replacement_path, 255) ||
            !token(row, "target_ino", ino) || !token(row, "target_dev", dev) ||
            !token(row, "spoofed_ino", spoof_ino) || !token(row, "spoofed_dev", spoof_dev))
            return false;
        MapsSubmissionIdentity identity;
        try {
            identity.target_ino = std::stoull(ino);
            identity.target_dev = std::stoull(dev);
            identity.spoofed_ino = std::stoull(spoof_ino);
            identity.spoofed_dev = std::stoull(spoof_dev);
        } catch (...) {
            return false;
        }
        if (identity.target_dev > 0xffffffffUL || identity.spoofed_dev > 0xffffffffUL ||
            !targets.emplace(identity.target_ino, identity.target_dev).second)
            return false;
        out.definitions.push_back(std::move(paths));
        out.identities.push_back(identity);
    }
    std::string error;
    return validate_maps_path_rules(out.definitions, error);
}
}  // namespace

JsonValue maps_submission_json(const MapsSubmission& entry) {
    auto value = JsonValue::object();
    value.o["id"] = entry.id;
    value.o["boot_id"] = entry.boot_id;
    value.o["supersedes_pending_id"] = entry.supersedes_pending_id;
    value.o["definition_revision"] = entry.definition_revision;
    value.o["namespace_ino"] = entry.namespace_ino;
    value.o["instance_id"] = entry.instance_id;
    value.o["generation_before"] = entry.generation_before;
    value.o["generation_after"] = entry.generation_after;
    value.o["phase"] = entry.phase;
    value.o["errno"] = entry.error;
    auto rows = JsonValue::array();
    for (std::size_t i = 0; i < entry.definitions.size(); ++i) {
        auto row = JsonValue::object();
        const auto& paths = entry.definitions[i];
        const auto& identity = entry.identities.at(i);
        row.o["target_path"] = paths.target_path;
        row.o["replacement_path"] = paths.replacement_path;
        row.o["target_ino"] = std::to_string(identity.target_ino);
        row.o["target_dev"] = std::to_string(identity.target_dev);
        row.o["spoofed_ino"] = std::to_string(identity.spoofed_ino);
        row.o["spoofed_dev"] = std::to_string(identity.spoofed_dev);
        rows.a.push_back(std::move(row));
    }
    value.o["rows"] = std::move(rows);
    return value;
}
bool parse_maps_history(const std::string& bytes, MapsHistory& out, std::string& error) {
    JsonValue value;
    const auto reject = [&] {
        error = "invalid or unsupported Maps submission history; stored bytes preserved";
        return false;
    };
    if (bytes.size() > MAPS_HISTORY_MAX_BYTES || !parse_json(bytes, value, error) ||
        !keys(value, {"schema_version", "latest", "confirmed"}))
        return reject();
    const auto* version = value.find("schema_version");
    if (!version || !version->is_number() || version->n != 1)
        return reject();
    MapsHistory parsed;
    for (const auto* name : {"latest", "confirmed"}) {
        const auto& field = value.o.at(name);
        if (field.is_null())
            continue;
        MapsSubmission entry;
        if (!parse_submission(field, entry) ||
            (std::string(name) == "confirmed" && entry.phase != "acknowledged"))
            return reject();
        (std::string(name) == "latest" ? parsed.latest : parsed.confirmed) = std::move(entry);
    }
    if (!parsed.latest ||
        (parsed.latest->phase == "acknowledged" &&
         (!parsed.confirmed || stringify_json(maps_submission_json(*parsed.latest)) !=
                                   stringify_json(maps_submission_json(*parsed.confirmed)))))
        return reject();
    out = std::move(parsed);
    error.clear();
    return true;
}
std::string encode_maps_history(const MapsHistory& history) {
    auto value = JsonValue::object();
    value.o["schema_version"] = 1;
    value.o["latest"] = history.latest ? maps_submission_json(*history.latest) : JsonValue{};
    value.o["confirmed"] =
        history.confirmed ? maps_submission_json(*history.confirmed) : JsonValue{};
    return stringify_json(value) + "\n";
}
}  // namespace sumhp::mount::sumh
