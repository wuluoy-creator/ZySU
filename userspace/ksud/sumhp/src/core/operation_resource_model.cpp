#include <algorithm>
#include <cmath>
#include <set>
#include "../../../../common/temporary_maps_command.hpp"
#include "core/operation_resource.hpp"
#include "core/user_hide_input.hpp"

namespace sumhp::state {
namespace {
bool keys(const JsonValue& value, const std::set<std::string>& expected) {
    if (!value.is_object() || value.o.size() != expected.size())
        return false;
    return std::all_of(value.o.begin(), value.o.end(),
                       [&](const auto& entry) { return expected.count(entry.first) != 0; });
}
bool text(const JsonValue* value, std::size_t maximum) {
    return value && value->is_string() && value->s.size() <= maximum &&
           value->s.find('\0') == std::string::npos && ksud::text::valid_utf8(value->s);
}
bool integer(const JsonValue* value, int maximum) {
    return value && value->is_number() && std::isfinite(value->n) && value->n >= 0 &&
           value->n <= maximum && std::floor(value->n) == value->n;
}
bool revision(const JsonValue* value, bool nullable) {
    return value && ((nullable && value->is_null()) ||
                     (value->is_string() && hide::input_detail::revision_valid(value->s)));
}
bool nullable_bool(const JsonValue* value) {
    return value && (value->is_null() || value->is_bool());
}
bool decimal(const JsonValue* value, bool allow_zero = false) {
    return text(value, 20) && !value->s.empty() &&
           (value->s.front() != '0' || (allow_zero && value->s == "0")) &&
           value->s.find_first_not_of("0123456789") == std::string::npos &&
           (value->s.size() < 20 || value->s <= "18446744073709551615");
}
bool runtime(const JsonValue& value) {
    const auto* number = value.find("errno");
    const bool signed_error = number && number->is_number() && std::isfinite(number->n) &&
                              number->n >= -4095 && number->n <= 4095 &&
                              std::floor(number->n) == number->n;
    return keys(value, {"id", "generation", "management", "binding", "errno"}) &&
           decimal(value.find("id")) && decimal(value.find("generation"), true) &&
           integer(value.find("management"), 1) && integer(value.find("binding"), 3) &&
           signed_error;
}
bool hide_receipt(const JsonValue& result, const OperationResource& record) {
    if (!keys(result, {"schema_version", "resource", "action", "status", "reason", "error", "saved",
                       "revision", "previous_revision", "boot_id", "runtime_verified",
                       "verification_scope", "affected_count", "confirmed_count", "failed_count",
                       "unknown_count", "items_complete", "items"}))
        return false;
    const auto& v = result.o;
    if (!integer(result.find("schema_version"), 1) || v.at("schema_version").n != 1 ||
        !text(result.find("resource"), 32) || v.at("resource").s != "user_hide_mutation" ||
        !text(result.find("action"), 32) || record.name != "hide " + v.at("action").s ||
        !text(result.find("status"), 32) || !text(result.find("reason"), 1024) ||
        !text(result.find("error"), 4096) || !text(result.find("boot_id"), 80) ||
        v.at("boot_id").s != record.boot_id || !nullable_bool(result.find("saved")) ||
        !revision(result.find("revision"), true) ||
        !revision(result.find("previous_revision"), true) || !v.at("runtime_verified").is_bool() ||
        v.at("runtime_verified").b || !text(result.find("verification_scope"), 80) ||
        v.at("verification_scope").s != "registered_membership_not_observer_visibility" ||
        !v.at("items_complete").is_bool() || !v.at("items").is_array() ||
        v.at("items").a.size() > 64)
        return false;
    const std::set<std::string> actions{"create",        "import-id", "annotate",
                                        "enable",        "disable",   "enable-group",
                                        "disable-group", "retry",     "remove-id"};
    const auto& action = v.at("action").s;
    const auto& status = v.at("status").s;
    if (!actions.count(action))
        return false;
    for (const auto* key : {"affected_count", "confirmed_count", "failed_count", "unknown_count"})
        if (!integer(result.find(key), 4096))
            return false;
    const auto affected = static_cast<int>(v.at("affected_count").n);
    const auto confirmed = static_cast<int>(v.at("confirmed_count").n);
    const auto failed = static_cast<int>(v.at("failed_count").n);
    const auto unknown = static_cast<int>(v.at("unknown_count").n);
    const bool success = status == "applied" || status == "saved" || status == "unchanged";
    if ((record.exit_code == 0) != success || (record.exit_code == 0 && record.error_number != 0))
        return false;
    if (success || status == "partial") {
        if (!v.at("saved").is_bool() || !v.at("saved").b ||
            !revision(result.find("revision"), false) ||
            !revision(result.find("previous_revision"), false))
            return false;
        if (action != "enable-group" && action != "disable-group" && affected != 1)
            return false;
    } else if (status == "rejected" || status == "not_saved") {
        if (!v.at("saved").is_bool() || v.at("saved").b || !v.at("revision").is_null())
            return false;
    } else if (status == "outcome_unknown") {
        if (!v.at("saved").is_null() || !v.at("revision").is_null())
            return false;
    } else
        return false;
    if (status == "saved" || status == "unchanged")
        if (action != "annotate")
            return false;
    const bool applied = status == "applied" || status == "partial";
    if (applied) {
        if (action == "annotate" || !affected || confirmed + failed + unknown != affected ||
            v.at("items").a.size() != static_cast<std::size_t>(std::min(affected, 64)) ||
            v.at("items_complete").b != (affected <= 64) ||
            (status == "applied") != (!failed && !unknown))
            return false;
    } else if (!v.at("items").a.empty() || confirmed || failed || unknown ||
               !v.at("items_complete").b)
        return false;
    std::set<std::string> ids;
    int found_confirmed = 0;
    int found_failed = 0;
    int found_unknown = 0;
    for (const auto& item : v.at("items").a) {
        if (!keys(item, {"id", "outcome", "errno", "membership_matches", "runtime"}) ||
            !text(item.find("id"), 32) || !valid_operation_id(item.o.at("id").s) ||
            !ids.insert(item.o.at("id").s).second || !text(item.find("outcome"), 80) ||
            !integer(item.find("errno"), 4095) || !nullable_bool(item.find("membership_matches")))
            return false;
        const auto& outcome = item.o.at("outcome").s;
        const auto& member = item.o.at("membership_matches");
        const auto& live = item.o.at("runtime");
        const auto number = item.o.at("errno").n;
        if (!live.is_null() && !runtime(live))
            return false;
        if (outcome == "absent" || outcome == "registered_bound" ||
            outcome == "registered_pending" || outcome == "registered_binding_error") {
            if (number || !member.is_bool() || !member.b)
                return false;
            ++found_confirmed;
            if (outcome == "absent") {
                if (!live.is_null())
                    return false;
            } else {
                if (live.is_null())
                    return false;
                const auto binding = live.o.at("binding").n;
                if ((outcome == "registered_bound" && binding != 2) ||
                    (outcome == "registered_binding_error" && binding != 3) ||
                    (outcome == "registered_pending" && binding > 1))
                    return false;
            }
        } else if (outcome == "outcome_unknown") {
            if (number || !member.is_null() || !live.is_null())
                return false;
            ++found_unknown;
        } else if (outcome == "runtime_differs") {
            if (number || !member.is_bool() || member.b)
                return false;
            ++found_failed;
        } else if (outcome == "mutation_failed_or_partial") {
            if (!number)
                return false;
            ++found_failed;
        } else
            return false;
        if (status == "applied" &&
            (action == "disable" || action == "disable-group" || action == "remove-id") &&
            outcome != "absent")
            return false;
        if (status == "applied" &&
            (action == "enable" || action == "enable-group" || action == "retry") &&
            outcome.compare(0, 11, "registered_") != 0)
            return false;
    }
    return !applied || !v.at("items_complete").b ||
           (found_confirmed == confirmed && found_failed == failed && found_unknown == unknown);
}
bool temporary_maps_receipt(const JsonValue& result, const OperationResource& record) {
    if (!keys(result, {"schema_version", "resource", "action", "status", "error_code", "errno",
                       "saved", "ioctl_attempted", "acknowledged", "operation_id",
                       "history_supported", "observer_verified", "request"}))
        return false;
    const auto& v = result.o;
    if (!integer(result.find("schema_version"), 1) || v.at("schema_version").n != 1 ||
        !text(result.find("resource"), 32) || v.at("resource").s != "temporary_maps_mutation" ||
        !text(result.find("action"), 16) || v.at("action").s != "remove" ||
        record.name != "sumh maps remove" || !text(result.find("operation_id"), 32) ||
        v.at("operation_id").s != record.id || !text(result.find("status"), 32) ||
        !text(result.find("error_code"), 80) || !integer(result.find("errno"), 4095) ||
        !v.at("saved").is_bool() || v.at("saved").b || !v.at("observer_verified").is_bool() ||
        v.at("observer_verified").b || !v.at("history_supported").is_bool() ||
        !v.at("history_supported").b || !v.at("ioctl_attempted").is_bool() ||
        !v.at("acknowledged").is_bool())
        return false;
    const auto& request = v.at("request");
    if (!keys(request,
              {"target_ino", "target_dev", "expected_instance_id", "expected_generation"}) ||
        !decimal(request.find("target_ino"), true) || !decimal(request.find("target_dev"), true) ||
        !decimal(request.find("expected_instance_id")) ||
        !decimal(request.find("expected_generation")))
        return false;
    maps_temporary::DeleteRequest parsed;
    if (!maps_temporary::parse_tracked_delete(
            {"sumh", "maps", "remove", request.o.at("target_ino").s, request.o.at("target_dev").s,
             "--instance", request.o.at("expected_instance_id").s, "--generation",
             request.o.at("expected_generation").s},
            parsed))
        return false;
    // Tracked requests use canonical numeric argv, so the persisted selector
    // independently reconstructs the exact originally admitted request digest.
    auto arguments = JsonValue::array();
    for (const auto& argument : maps_temporary::delete_arguments(parsed))
        arguments.a.emplace_back(argument);
    if (record.revision != revision_for_bytes("sumhp-operation-v1\n" + stringify_json(arguments)))
        return false;
    const int number = static_cast<int>(v.at("errno").n);
    const bool attempted = v.at("ioctl_attempted").b;
    if (record.error_number != number ||
        v.at("error_code").s != maps_temporary::error_code(number, attempted))
        return false;
    if (!number)
        return record.exit_code == 0 && attempted && v.at("acknowledged").b &&
               v.at("status").s == "deleted";
    const bool unknown = attempted && !maps_temporary::rejected_before_publication(number);
    return record.exit_code == 1 && !v.at("acknowledged").b &&
           v.at("status").s == (unknown ? "outcome_unknown" : "not_applied");
}
JsonValue resource_json(const OperationResource& value) {
    auto root = JsonValue::object();
    root.o["operation_id"] = value.id;
    root.o["request_revision"] = value.revision;
    root.o["boot_id"] = value.boot_id;
    root.o["name"] = value.name;
    root.o["command_exit_code"] = value.exit_code;
    root.o["command_errno"] = value.error_number;
    root.o["result"] = value.result;
    return root;
}
bool parse_resource(const JsonValue& root, OperationResource& value, int source_schema) {
    if (!keys(root, {"operation_id", "request_revision", "boot_id", "name", "command_exit_code",
                     "command_errno", "result"}) ||
        !text(root.find("operation_id"), 32) || !valid_operation_id(root.o.at("operation_id").s) ||
        !revision(root.find("request_revision"), false) || !text(root.find("boot_id"), 80) ||
        root.o.at("boot_id").s.empty() || !text(root.find("name"), 80) ||
        !integer(root.find("command_exit_code"), 255) ||
        !integer(root.find("command_errno"), 4095) ||
        stringify_json(root).size() > OPERATION_RESOURCE_ITEM_BYTES)
        return false;
    value = {root.o.at("operation_id").s,
             root.o.at("request_revision").s,
             root.o.at("boot_id").s,
             root.o.at("name").s,
             static_cast<int>(root.o.at("command_exit_code").n),
             static_cast<int>(root.o.at("command_errno").n),
             root.o.at("result")};
    return hide_receipt(value.result, value) ||
           (source_schema == 2 && temporary_maps_receipt(value.result, value));
}
}  // namespace

bool make_operation_resource(const Operation& operation, const std::vector<std::string>& args,
                             const std::string& output, bool complete, int code, int number,
                             OperationResource& resource, std::string& error) {
    hide::Request request;
    maps_temporary::DeleteRequest maps_request;
    JsonValue result;
    const bool temporary_maps = maps_temporary::parse_tracked_delete(args, maps_request);
    if (!complete || output.size() > 64UL * 1024 ||
        (!temporary_maps && (!hide::parse_manager_arguments(args, request, error) ||
                             hide::definition_query(request.action))) ||
        !ksud::text::valid_utf8(output) || !json::Parser(output, true).parse(result, error)) {
        error = "resource_output_incomplete_or_invalid";
        return false;
    }
    OperationResource const value{
        operation.id, operation.request_revision, operation.boot_id, operation.name, code,
        number,       std::move(result)};
    OperationResource checked;
    if (!parse_resource(resource_json(value), checked, 2)) {
        error = "resource_receipt_invalid";
        return false;
    }
    if (temporary_maps) {
        if (checked.result.o.at("resource").s != "temporary_maps_mutation") {
            error = "resource_receipt_type_mismatch";
            return false;
        }
        const auto& echoed = checked.result.o.at("request").o;
        if (echoed.at("target_ino").s != std::to_string(maps_request.ino) ||
            echoed.at("target_dev").s != std::to_string(maps_request.dev) ||
            echoed.at("expected_instance_id").s != std::to_string(maps_request.instance) ||
            echoed.at("expected_generation").s != std::to_string(maps_request.generation)) {
            error = "resource_selector_mismatch";
            return false;
        }
    } else {
        if (checked.result.o.at("resource").s != "user_hide_mutation") {
            error = "resource_receipt_type_mismatch";
            return false;
        }
        if (checked.result.o.at("saved").is_bool() && checked.result.o.at("saved").b &&
            checked.result.o.at("previous_revision").s != request.revision) {
            error = "resource_revision_mismatch";
            return false;
        }
        if (request.action != "create" && request.action != "enable-group" &&
            request.action != "disable-group")
            for (const auto& item : checked.result.o.at("items").a)
                if (item.o.at("id").s != request.selector) {
                    error = "resource_selector_mismatch";
                    return false;
                }
    }
    resource = std::move(checked);
    error.clear();
    return true;
}
std::string encode_operation_resources(const OperationResources& resources) {
    auto root = JsonValue::object();
    root.o["schema_version"] = 2;
    root.o["receipts"] = JsonValue::array();
    for (const auto& resource : resources)
        root.o["receipts"].a.push_back(resource_json(resource));
    return stringify_json(root) + '\n';
}
bool parse_operation_resources(const std::string& bytes, OperationResources& resources,
                               std::string& error, int* source_schema) {
    JsonValue root;
    const auto fail = [&] {
        error = "invalid or unsupported operation resource store; preserving stored bytes";
        return false;
    };
    if (bytes.size() > OPERATION_RESOURCE_MAX_BYTES || !ksud::text::valid_utf8(bytes) ||
        !json::Parser(bytes, true).parse(root, error) ||
        !keys(root, {"schema_version", "receipts"}) || !integer(root.find("schema_version"), 2) ||
        root.o.at("schema_version").n < 1 || !root.o.at("receipts").is_array() ||
        root.o.at("receipts").a.size() > OPERATION_RESOURCE_MAX_RECORDS)
        return fail();
    const int schema = static_cast<int>(root.o.at("schema_version").n);
    OperationResources parsed;
    std::set<std::string> ids;
    for (const auto& raw : root.o.at("receipts").a) {
        OperationResource resource;
        if (!parse_resource(raw, resource, schema) || !ids.insert(resource.id).second)
            return fail();
        parsed.push_back(std::move(resource));
    }
    resources = std::move(parsed);
    if (source_schema)
        *source_schema = schema;
    error.clear();
    return true;
}
bool operation_resource_matches(const OperationResource& resource, const Operation& operation) {
    return resource.id == operation.id && resource.revision == operation.request_revision &&
           resource.boot_id == operation.boot_id && resource.name == operation.name &&
           resource.exit_code == operation.exit_code &&
           resource.error_number == operation.error_number && operation.phase == "finished";
}
}  // namespace sumhp::state
