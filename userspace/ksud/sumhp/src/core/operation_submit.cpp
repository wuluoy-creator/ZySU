#include "core/operation_submit.hpp"
#include <cmath>
#include <iostream>
#include <set>
#include "../../../../common/temporary_maps_command.hpp"
#include "../../../../common/utf8.hpp"
#include "core/daemon.hpp"
#include "core/runtime.hpp"
#include "core/state_ledger.hpp"

namespace sumhp::state {
namespace {
JsonValue bytes_value(const std::string& bytes, bool complete) {
    auto value = JsonValue::object();
    const bool utf8 = ksud::text::valid_utf8(bytes);
    value.o["encoding"] = utf8 ? "utf-8" : "hex";
    if (utf8)
        value.o["value"] = bytes;
    else {
        static constexpr char hex[] = "0123456789abcdef";
        std::string encoded;
        encoded.reserve(bytes.size() * 2);
        for (const unsigned char c : bytes) {
            encoded += hex[c >> 4];
            encoded += hex[c & 15];
        }
        value.o["value"] = encoded;
    }
    value.o["byte_length"] = static_cast<int>(bytes.size());
    value.o["complete"] = complete;
    return value;
}
JsonValue base(const std::string& id, const std::string& revision, const std::string& status) {
    auto value = JsonValue::object();
    value.o["schema_version"] = 1;
    value.o["kind"] = "sumh.operation.submission";
    value.o["operation_id"] = id.empty() ? JsonValue{} : JsonValue(id);
    value.o["request_revision"] = revision.empty() ? JsonValue{} : JsonValue(revision);
    value.o["boot_id"] = runtime_boot_id();
    value.o["status"] = status;
    value.o["command_executed_this_attempt"] = false;
    value.o["final_record_confirmed"] = false;
    value.o["runtime_verified"] = false;
    value.o["command_exit_code"] = JsonValue{};
    value.o["command_errno"] = JsonValue{};
    value.o["observed_record"] = JsonValue{};
    value.o["stdout"] = bytes_value({}, true);
    value.o["stderr"] = bytes_value({}, true);
    value.o["error"] = "";
    return value;
}
bool integer(const JsonValue* value, int maximum) {
    return value && value->is_number() && value->n >= 0 && value->n <= maximum &&
           std::floor(value->n) == value->n;
}
bool output_valid(const JsonValue* value) {
    if (!value || !value->is_object() || value->o.size() != 4)
        return false;
    const auto* encoding = value->find("encoding");
    const auto* bytes = value->find("value");
    const auto* length = value->find("byte_length");
    const auto* complete = value->find("complete");
    if (!encoding || !encoding->is_string() || !bytes || !bytes->is_string() ||
        !integer(length, OPERATION_OUTPUT_BYTES) || !complete || !complete->is_bool())
        return false;
    const auto size = static_cast<std::size_t>(length->n);
    if (encoding->s == "utf-8")
        return bytes->s.size() == size && ksud::text::valid_utf8(bytes->s);
    return encoding->s == "hex" && bytes->s.size() == size * 2 &&
           bytes->s.find_first_not_of("0123456789abcdef") == std::string::npos;
}
}  // namespace

JsonValue operation_submission_failure(const std::string& id, const std::string& revision,
                                       const std::string& status, const std::string& error) {
    auto value = base(id, revision, status);
    value.o["error"] = error;
    return value;
}

JsonValue operation_execution_json(const OperationInput& input, const CommandResult& result) {
    auto value = base(result.operation_id, input.request_revision, result.operation_status);
    value.o["command_executed_this_attempt"] = result.command_executed;
    value.o["final_record_confirmed"] = result.operation_recorded;
    if (result.command_executed) {
        value.o["command_exit_code"] = result.command_exit_code;
        value.o["command_errno"] = result.command_errno;
    } else if (!result.observed_record_json.empty()) {
        JsonValue observed;
        std::string error;
        if (json::Parser(result.observed_record_json, true).parse(observed, error))
            value.o["observed_record"] = std::move(observed);
    }
    value.o["stdout"] = bytes_value(result.stdout_text, result.stdout_complete);
    value.o["stderr"] = bytes_value(result.stderr_text, result.stderr_complete);
    return value;
}

bool parse_operation_reply(const std::string& response, const std::string& expected_id,
                           const std::string& expected_revision, JsonValue& result) {
    JsonValue value;
    std::string error;
    if (response.size() > 1024UL * 1024 || !ksud::text::valid_utf8(response) ||
        !json::Parser(response, true).parse(value, error) || !value.is_object() ||
        value.o.size() != 15)
        return false;
    const auto* schema = value.find("schema_version");
    const auto* kind = value.find("kind");
    const auto* id = value.find("operation_id");
    const auto* revision = value.find("request_revision");
    const auto* status = value.find("status");
    const auto* boot = value.find("boot_id");
    const auto* ran = value.find("command_executed_this_attempt");
    const auto* recorded = value.find("final_record_confirmed");
    const auto* verified = value.find("runtime_verified");
    const auto* code = value.find("command_exit_code");
    const auto* number = value.find("command_errno");
    const auto* observed = value.find("observed_record");
    const auto* failure = value.find("error");
    if (!schema || !schema->is_number() || schema->n != 1 || !kind || !kind->is_string() ||
        kind->s != "sumh.operation.submission" || !id || !id->is_string() || id->s != expected_id ||
        !revision || !revision->is_string() || revision->s != expected_revision || !status ||
        !status->is_string() || !boot || !boot->is_string() || !ran ||
        (!ran->is_bool() && !ran->is_null()) || !recorded || !recorded->is_bool() || !verified ||
        !verified->is_bool() || verified->b || !code || !number || !observed || !failure ||
        !failure->is_string() || !output_valid(value.find("stdout")) ||
        !output_valid(value.find("stderr")))
        return false;
    static const std::set<std::string> rejected = {"already_started_or_finished", "not_retained",
                                                   "request_conflict", "boot_mismatch",
                                                   "journal_unavailable"};
    if (status->s == "command_completed" || status->s == "failed_or_partial") {
        if (!ran->is_bool() || !ran->b || !recorded->b || !integer(code, 255) ||
            !integer(number, 4095) ||
            (status->s == "command_completed" ? (code->n != 0 || number->n != 0) : code->n == 0))
            return false;
    } else if (status->s == "outcome_unknown") {
        if (recorded->b || (ran->is_bool() && !ran->b) ||
            (ran->is_null() ? (!code->is_null() || !number->is_null())
                            : (!integer(code, 255) || !integer(number, 4095))))
            return false;
    } else if (!rejected.count(status->s) || !ran->is_bool() || ran->b || recorded->b ||
               !code->is_null() || !number->is_null())
        return false;
    // An observation is not a cached response. Validate its persisted record
    // through the production ledger codec before exposing it to scripts.
    if (!observed->is_null()) {
        if (!observed->is_object() || observed->o.size() != 10 || !observed->find("result") ||
            !observed->find("same_boot"))
            return false;
        auto record = *observed;
        const auto* phase = record.find("phase");
        const auto* observed_id = record.find("id");
        if (!phase || !phase->is_string() || !observed_id || !observed_id->is_string() ||
            observed_id->s != expected_id)
            return false;
        if (phase->s != "finished") {
            record.o["exit_code"] = 0;
            record.o["errno"] = 0;
        }
        if (record.find("request_revision") && record.o["request_revision"].is_null())
            record.o["request_revision"] = "";
        record.o.erase("result");
        record.o.erase("same_boot");
        auto ledger = JsonValue::object();
        ledger.o["schema_version"] = 3;
        ledger.o["last_operation"] = record;
        ledger.o["history"] = JsonValue::array();
        ledger.o["history_pruned"] = false;
        ledger.o["verified"] = JsonValue::object();
        Ledger parsed;
        if (!parse_ledger(stringify_json(ledger), parsed, error) ||
            stringify_json(operation_record_json(parsed.last, boot->s)) !=
                stringify_json(*observed))
            return false;
    }
    if ((ran->is_null() || ran->b) && !observed->is_null())
        return false;
    if (status->s == "not_retained" || status->s == "journal_unavailable") {
        if (!observed->is_null())
            return false;
    } else if (status->s == "already_started_or_finished" || status->s == "boot_mismatch" ||
               status->s == "request_conflict") {
        if (observed->is_null())
            return false;
        if (status->s != "request_conflict") {
            const auto& binding = observed->o.at("request_revision");
            const auto& same_boot = observed->o.at("same_boot");
            if (!binding.is_string() || binding.s != expected_revision)
                return false;
            if (status->s == "already_started_or_finished") {
                if (observed->o.at("phase").s == "prepared" || !same_boot.is_bool() || !same_boot.b)
                    return false;
            } else if (same_boot.is_bool() && same_boot.b)
                return false;
        }
    }
    result = std::move(value);
    return true;
}

std::string execute_reserved_request(const std::vector<std::string>& request) {
    const auto id =
        request.size() > 2 && valid_operation_id(request[2]) ? request[2] : std::string{};
    OperationInput input;
    std::string error;
    if (request.size() != 4 || request[0] != "operation" || request[1] != "execute" || id.empty() ||
        !parse_operation_input(request[3], input, error))
        return stringify_json(
                   operation_submission_failure(id, {}, "rejected", "invalid_reserved_request")) +
               "\n";
    // A raw daemon request cannot consume a local-only reservation. In
    // particular, do not begin the journal and then rely on run_command's refusal.
    maps_temporary::DeleteRequest local;
    if (maps_temporary::parse_tracked_delete(input.args, local))
        return stringify_json(
                   operation_submission_failure(id, input.request_revision, "rejected",
                                                "temporary_maps_requires_local_execution")) +
               "\n";
    try {
        const auto result = run_command_capture(input.args, id, input.request_revision);
        return stringify_json(operation_execution_json(input, result)) + "\n";
    } catch (const std::exception&) {
        auto value = operation_submission_failure(id, input.request_revision, "outcome_unknown",
                                                  "execution_exception");
        value.o["command_executed_this_attempt"] = JsonValue{};
        return stringify_json(value) + "\n";
    }
}

OperationResponse submit_operation_bytes(bool execute, const std::string& id,
                                         const std::string& bytes) {
    std::string error;
    OperationInput input;
    JsonValue result;
    int exit_code = 1;
    bool dispatch_attempted = false;
    bool local_execution = false;
    try {
        if (!parse_operation_input(bytes, input, error) || (execute && !valid_operation_id(id))) {
            result = operation_submission_failure(id, {}, "rejected", error);
            exit_code = 2;
        } else if (!execute) {
            Journal journal;
            if (!journal.prepare(input.name, input.request_revision, error))
                result = operation_submission_failure({}, input.request_revision,
                                                      "reservation_unconfirmed", error);
            else {
                result = base(journal.operation_id(), input.request_revision, "prepared");
                result.o["reservation_confirmed"] = true;
                exit_code = 0;
            }
        } else {
            maps_temporary::DeleteRequest local;
            local_execution = maps_temporary::parse_tracked_delete(input.args, local);
            std::string response;
            dispatch_attempted = true;
            if (local_execution) {
                const auto captured = run_command_capture(input.args, id, input.request_revision,
                                                          CaptureRoute::LocalTemporaryMaps);
                // Capture may catch an exception during the durable start or
                // observed-record construction and return an incomplete status.
                // Apply the public reply contract here as on the daemon route;
                // uncertainty never permits redispatch of the reservation.
                const auto local_reply = stringify_json(operation_execution_json(input, captured));
                if (!parse_operation_reply(local_reply, id, input.request_revision, result)) {
                    result =
                        operation_submission_failure(id, input.request_revision, "outcome_unknown",
                                                     "invalid_or_mismatched_local_result");
                    result.o["command_executed_this_attempt"] = JsonValue{};
                } else if (result.o.at("status").s == "command_completed")
                    exit_code = 0;
            } else if (!exchange_reserved_operation(id, input.canonical_document, response,
                                                    error)) {
                result = operation_submission_failure(id, input.request_revision,
                                                      "transport_unknown", error);
                result.o["command_executed_this_attempt"] = JsonValue{};
            } else {
                if (!parse_operation_reply(response, id, input.request_revision, result)) {
                    result = operation_submission_failure(id, input.request_revision,
                                                          "transport_unknown",
                                                          "invalid_or_mismatched_reply");
                    result.o["command_executed_this_attempt"] = JsonValue{};
                } else if (result.o.at("status").s == "command_completed")
                    exit_code = 0;
            }
        }
    } catch (const std::exception&) {
        const char* status;
        if (dispatch_attempted)
            status = local_execution ? "outcome_unknown" : "transport_unknown";
        else
            status = execute ? "rejected" : "reservation_unconfirmed";
        result = operation_submission_failure(id, input.request_revision, status,
                                              "submission_exception");
        if (dispatch_attempted)
            result.o["command_executed_this_attempt"] = JsonValue{};
    }
    return {exit_code, std::move(result)};
}

int print_operation_submit(const OperationSubmitRequest& request) {
    std::string bytes;
    std::string error;
    OperationResponse result;
    try {
        result = read_operation_input(request.input_path, bytes, error)
                     ? submit_operation_bytes(request.execute, request.id, bytes)
                     : OperationResponse{
                           2, operation_submission_failure(request.id, {}, "rejected", error)};
    } catch (const std::exception&) {
        result = {1, operation_submission_failure(
                         request.id, {}, request.execute ? "rejected" : "reservation_unconfirmed",
                         "submission_exception")};
    }
    std::cout << stringify_json(result.value) << '\n';
    return std::cout.good() ? result.exit_code : 1;
}
}  // namespace sumhp::state
