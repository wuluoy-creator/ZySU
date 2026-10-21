#include "core/operation_submit.hpp"
#include <cassert>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include "core/capture_buffer.hpp"
#include "core/operation_manager_input.hpp"
#include "core/state_ledger.hpp"

namespace fixture {
const std::string id(32, 'a'), revision = "sha256-v1:" + std::string(64, 'b');
std::string digest_input, document, reply;
int reservations = 0, captures = 0, sends = 0, reads = 0, assertions = 0;
int local_captures = 0;
bool hash_fails = false, prepare_fails = false, read_fails = false, send_fails = false,
     throw_prepare = false, throw_send = false, throw_capture = false;
sumhp::CommandResult captured;
void check_at(bool value, int line) {
    ++assertions;
    if (!value) {
        std::cerr << "check " << assertions << " failed at line " << line << '\n';
        std::abort();
    }
}
}  // namespace fixture
#define check(value) check_at((value), __LINE__)
namespace sumhp {
// Configuration-field semantics have their own production config suite. This
// fixture must never substitute an accepting validator for that dependency.
bool validate_config_patch(const json::Value&, std::string&) {
    assert(false);
    return false;
}
std::string runtime_boot_id() {
    return "test-boot";
}
CommandResult run_command_capture(const std::vector<std::string>& args, const std::string& id,
                                  const std::string& revision, CaptureRoute route) {
    ++fixture::captures;
    assert(!args.empty() && id == fixture::id && revision == fixture::revision);
    if (route == CaptureRoute::LocalTemporaryMaps)
        ++fixture::local_captures;
    if (fixture::throw_capture)
        throw std::runtime_error("capture");
    return fixture::captured;
}
enum class RequestResult { Complete, Unavailable, Failed };
RequestResult send_request(const std::vector<std::string>& args, std::size_t start,
                           std::string& response, std::string& error) {
    ++fixture::sends;
    assert(start == 0 && args == std::vector<std::string>(
                                     {"operation", "execute", fixture::id, fixture::document}));
    if (fixture::throw_send)
        throw std::runtime_error("transport");
    response = fixture::reply;
    error = "lost response";
    return fixture::send_fails ? RequestResult::Unavailable : RequestResult::Complete;
}
#include "operation_exchange_under_test.inc"
namespace state {
std::string revision_for_bytes(const std::string& bytes) {
    fixture::digest_input = bytes;
    return fixture::hash_fails ? "" : fixture::revision;
}
struct Journal::Impl {};
Journal::Journal() = default;
Journal::~Journal() = default;
bool Journal::prepare(const std::string& name, const std::string& revision, std::string& error) {
    ++fixture::reservations;
    assert(!name.empty() && revision == fixture::revision);
    if (fixture::throw_prepare)
        throw std::runtime_error("prepare");
    error = "unconfirmed";
    return !fixture::prepare_fails;
}
const std::string& Journal::operation_id() const {
    return fixture::id;
}
bool read_operation_input(const std::string&, std::string& out, std::string& error) {
    ++fixture::reads;
    out = fixture::document;
    error = "read failure";
    return !fixture::read_fails;
}
}  // namespace state
}  // namespace sumhp
namespace {
using namespace sumhp;
using namespace sumhp::state;
using fixture::check_at;
std::string doc(const std::vector<std::string>& args) {
    auto value = JsonValue::object();
    value.o["schema_version"] = 1;
    value.o["args"] = JsonValue::array();
    for (const auto& a : args)
        value.o["args"].a.emplace_back(a);
    return stringify_json(value);
}
JsonValue parsed(const std::string& text) {
    JsonValue v;
    std::string e;
    assert(json::Parser(text, true).parse(v, e));
    return v;
}
void input_tests() {
    OperationInput input;
    std::string error;
    const std::string hide_revision = "sha256-v1:" + std::string(64, 'b');
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"hide", "add", "/test/汉字 "},
             {"hide", "add", "/test/line\nnext"},
             {"debug", "stealth", "enable"},
             {"module", "configure", "example", "sumh", "[]"},
             {"hide", "create", hide_revision,
              "{\"path\":\"/测试 \",\"enabled\":false,\"note\":\"a\\nb\"}"},
             {"hide", "annotate", fixture::id, hide_revision, "{\"name\":\"new\"}"},
             {"hide", "enable", fixture::id, hide_revision},
             {"hide", "disable", fixture::id, hide_revision},
             {"hide", "retry", fixture::id, hide_revision},
             {"hide", "remove-id", fixture::id, hide_revision},
             {"hide", "enable-group", "", hide_revision},
             {"hide", "disable-group", "group", hide_revision},
             {"hide", "retry-path", "/test"},
             {"sumh", "maps", "apply"},
             {"sumh", "kernel-build", "reset"},
             {"hide", "add", "--", "/test/-h"}}) {
        check(parse_operation_input(doc(args), input, error));
        check(fixture::digest_input ==
              "sumhp-operation-v1\n" + stringify_json(parsed(doc(args)).o.at("args")));
    }
    const auto valid = doc({"hide", "add", "/test"});
    check(parse_operation_input(valid, input, error));
    const auto digest = fixture::digest_input;
    check(parse_operation_input(
        " { \"args\" : [ \"hide\", \"add\", \"/test\" ], \"schema_version\" : 1 } ", input, error));
    check(fixture::digest_input == digest && input.canonical_document == valid);
    for (
        const auto& args : std::vector<std::vector<std::string>>{
            {"hide", "add"},
            {"hide", "add", "relative"},
            {"hide", "add", "/a", "extra"},
            {"hide", "list"},
            {"hide", "status"},
            {"hide", "status", hide_revision, "0"},
            {"hide", "retry-path", "relative"},
            {"hide", "enable", "bad", hide_revision},
            {"hide", "disable", fixture::id, "bad"},
            {"hide", "annotate", fixture::id, hide_revision, "{\"enabled\":false}"},
            {"hide", "create", hide_revision, "{\"path\":\"/a\",\"path\":\"/b\"}"},
            {"debug", "stealth", "invalid"},
            {"debug", "enable", "--help"},
            {"debug", "enable", "--verbose"},
            {"debug", "enable", "--color=always"},
            {"daemon", "call", "debug", "enable"},
            {"config", "apply", "--automatic"},
            {"operation", "execute", fixture::id},
            {"module", "normalize", "/test", "--dry-run"},
            {"sumh", "maps", "list-temporary"},
            {"sumh", "maps", "manager", R"({"schema_version":1,"action":"list"})", "--channel"},
            {"sumh", "maps", "manager",
             R"({"schema_version":1,"action":"remove","instance_id":"1","generation":"2","target_ino":"0","target_dev":"0"})",
             "--channel"},
            {"config", "set-partitions", "[]", "--revision", "x"},
            {"sumh", "maps", "preview"},
            {"hide", "add", std::string("/a\0b", 4)}}) {
        input.name = "sentinel";
        check(!parse_operation_input(doc(args), input, error) && input.name == "sentinel");
    }
    for (const auto& invalid :
         {"{}", "{\"schema_version\":2,\"args\":[\"debug\",\"enable\"]}",
          "{\"schema_version\":1,\"schema_version\":1,\"args\":[\"debug\",\"enable\"]}",
          "{\"schema_version\":1,\"args\":[\"debug\",true]}",
          "{\"schema_version\":1,\"args\":[\"debug\",\"enable\"],\"x\":0}"})
        check(!parse_operation_input(invalid, input, error));
    check(!parse_operation_input(valid + std::string(1, char(0xff)), input, error));
    check(parse_operation_input(valid + std::string(OPERATION_INPUT_BYTES - valid.size(), ' '),
                                input, error));
    check(!parse_operation_input(valid + std::string(OPERATION_INPUT_BYTES + 1 - valid.size(), ' '),
                                 input, error));
    check(
        !parse_operation_input(doc({"hide", "add", "/" + std::string(20000, '"')}), input, error) &&
        error == "daemon_frame_capacity");
    fixture::hash_fails = true;
    check(!parse_operation_input(valid, input, error) && error == "request_digest_unavailable");
    fixture::hash_fails = false;
    OperationSubmitRequest request;
    check(parse_operation_submit_arguments({"operation", "prepare", "--input", "-"}, request,
                                           error) &&
          !request.execute);
    check(parse_operation_submit_arguments(
              {"operation", "execute", fixture::id, "--input", "/test"}, request, error) &&
          request.execute);
    for (const auto& args :
         std::vector<std::vector<std::string>>{{"operation", "prepare", "--input", "relative"},
                                               {"operation", "execute", "BAD", "--input", "-"},
                                               {"operation", "prepare", "--input", "/a\nb"},
                                               {"operation", "prepare", "--input", "/x", "extra"}})
        check(!parse_operation_submit_arguments(args, request, error));
    check(fixture::captures == 0 && fixture::reservations == 0 && fixture::sends == 0 &&
          fixture::reads == 0);
}
void receipt_tests() {
    OperationInput input;
    input.request_revision = fixture::revision;
    CommandResult capture;
    capture.operation_id = fixture::id;
    capture.operation_status = "command_completed";
    capture.command_executed = true;
    capture.operation_recorded = true;
    auto receipt = operation_execution_json(input, capture);
    JsonValue result;
    const auto accepts = [&](const JsonValue& v) {
        return parse_operation_reply(stringify_json(v), fixture::id, fixture::revision, result);
    };
    check(accepts(receipt));
    for (const auto& key :
         {"schema_version", "kind", "operation_id", "request_revision", "boot_id", "status",
          "command_executed_this_attempt", "final_record_confirmed", "runtime_verified",
          "command_exit_code", "command_errno", "observed_record", "stdout", "stderr", "error"}) {
        auto v = receipt;
        v.o.erase(key);
        check(!accepts(v));
    }
    for (const auto& change : std::vector<std::function<void(JsonValue&)>>{
             [](auto& v) { v.o["command_executed_this_attempt"] = false; },
             [](auto& v) { v.o["final_record_confirmed"] = false; },
             [](auto& v) { v.o["runtime_verified"] = true; },
             [](auto& v) { v.o["command_exit_code"] = 1; },
             [](auto& v) { v.o["command_errno"] = 1; }, [](auto& v) { v.o["status"] = "prepared"; },
             [](auto& v) { v.o["operation_id"] = std::string(32, 'c'); },
             [](auto& v) { v.o["schema_version"] = 2; },
             [](auto& v) { v.o["stdout"].o["complete"] = "true"; },
             [](auto& v) { v.o["stdout"].o["byte_length"] = 0.5; },
             [](auto& v) { v.o["stdout"].o["byte_length"] = 1; },
             [](auto& v) { v.o["extra"] = true; }}) {
        auto v = receipt;
        change(v);
        check(!accepts(v));
    }
    capture.stdout_text = std::string(OPERATION_OUTPUT_BYTES, '\1');
    capture.stderr_text = std::string(OPERATION_OUTPUT_BYTES, char(0xff));
    capture.stdout_complete = false;
    capture.stderr_complete = false;
    receipt = operation_execution_json(input, capture);
    check(accepts(receipt) && stringify_json(receipt).size() < 1024 * 1024);
    check(receipt.o.at("stderr").o.at("encoding").s == "hex" &&
          !receipt.o.at("stdout").o.at("complete").b);
    capture.stdout_text = std::string("x\xc3", 2);
    receipt = operation_execution_json(input, capture);
    check(accepts(receipt) && receipt.o.at("stdout").o.at("value").s == "78c3");
    capture.command_exit_code = 2;
    capture.command_errno = 5;
    capture.operation_status = "failed_or_partial";
    check(accepts(operation_execution_json(input, capture)));
    capture.operation_recorded = false;
    capture.operation_status = "outcome_unknown";
    check(accepts(operation_execution_json(input, capture)));
    capture.command_executed = false;
    capture.operation_status = "already_started_or_finished";
    Operation op{fixture::id, "hide add", "finished", "test-boot", 0, 0, "", fixture::revision};
    capture.observed_record_json = stringify_json(operation_record_json(op, "test-boot"));
    receipt = operation_execution_json(input, capture);
    check(accepts(receipt));
    receipt.o["observed_record"].o["result"] = "invented";
    check(!accepts(receipt));
    for (const auto* phase : {"prepared", "started"}) {
        op.phase = phase;
        capture.observed_record_json = stringify_json(operation_record_json(op, "test-boot"));
        check(accepts(operation_execution_json(input, capture)) == (op.phase == "started"));
    }
    for (const auto* status :
         {"already_started_or_finished", "boot_mismatch", "request_conflict"}) {
        capture.operation_status = status;
        capture.observed_record_json.clear();
        check(!accepts(operation_execution_json(input, capture)));
    }
    for (const auto* status : {"not_retained", "journal_unavailable"}) {
        capture.operation_status = status;
        capture.observed_record_json.clear();
        check(accepts(operation_execution_json(input, capture)));
        capture.observed_record_json = stringify_json(operation_record_json(op, "test-boot"));
        check(!accepts(operation_execution_json(input, capture)));
    }
    capture.operation_status = "already_started_or_finished";
    op.request_revision = "sha256-v1:" + std::string(64, 'c');
    capture.observed_record_json = stringify_json(operation_record_json(op, "test-boot"));
    check(!accepts(operation_execution_json(input, capture)));
    capture.operation_status = "request_conflict";
    check(accepts(operation_execution_json(input, capture)));
    op.request_revision.clear();
    capture.observed_record_json = stringify_json(operation_record_json(op, "test-boot"));
    check(accepts(operation_execution_json(input, capture)));
    op.phase = "prepared";
    op.request_revision = fixture::revision;
    op.boot_id = "old-boot";
    capture.observed_record_json = stringify_json(operation_record_json(op, "test-boot"));
    capture.operation_status = "boot_mismatch";
    check(accepts(operation_execution_json(input, capture)));
    capture.operation_status = "already_started_or_finished";
    check(!accepts(operation_execution_json(input, capture)));
    capture.operation_status = "boot_mismatch";
    op.boot_id = "test-boot";
    capture.observed_record_json = stringify_json(operation_record_json(op, "test-boot"));
    check(!accepts(operation_execution_json(input, capture)));
    op.boot_id = "old-boot";
    capture.observed_record_json = stringify_json(operation_record_json(op, ""));
    auto missing_boot = operation_execution_json(input, capture);
    missing_boot.o["boot_id"] = "";
    check(accepts(missing_boot));
    CaptureBuffer buffer(3);
    std::ostream output(&buffer);
    output << "ab";
    check(buffer.complete());
    output << 'c';
    check(buffer.complete());
    output << "def";
    check(!buffer.complete() && buffer.str() == "abc" && output.good());
    CaptureBuffer zero(0);
    std::ostream empty(&zero);
    empty << 'x';
    check(!zero.complete() && zero.str().empty());
}
std::pair<int, JsonValue> submit(bool execute) {
    std::ostringstream text;
    auto* old = std::cout.rdbuf(text.rdbuf());
    const auto code = print_operation_submit({execute, execute ? fixture::id : "", "-"});
    std::cout.rdbuf(old);
    return {code, parsed(text.str())};
}
void workflow_tests() {
    fixture::document = doc({"debug", "enable"});
    auto prepared = submit(false);
    check(prepared.first == 0 && prepared.second.o.at("status").s == "prepared");
    check(fixture::sends == 0 && fixture::captures == 0 && fixture::reservations == 1);
    fixture::prepare_fails = true;
    check(submit(false).second.o.at("status").s == "reservation_unconfirmed");
    fixture::prepare_fails = false;
    fixture::throw_prepare = true;
    check(submit(false).first == 1);
    fixture::throw_prepare = false;
    fixture::read_fails = true;
    const auto count = fixture::reservations;
    check(submit(false).first == 2 && fixture::reservations == count);
    fixture::read_fails = false;
    fixture::captured.operation_id = fixture::id;
    fixture::captured.command_executed = true;
    fixture::captured.operation_recorded = true;
    fixture::captured.operation_status = "command_completed";
    // Run the actual daemon special dispatch block. No legacy fallback, no
    // response wrapping, even for malformed reserved requests.
    int dispatched = 0, closed = 0, legacy = 0;
    const auto send_response = [&](int fd, const std::string& text) {
        assert(fd == 7);
        ++dispatched;
        fixture::reply = text;
    };
    const auto close = [&](int fd) {
        assert(fd == 7);
        ++closed;
    };
    for (const auto& request_args : std::vector<std::vector<std::string>>{
             {"operation", "execute", fixture::id, fixture::document},
             {"operation", "execute", "bad", fixture::document}}) {
        const int client_fd = 7;
#include "operation_dispatch_under_test.inc"
        ++legacy;
    }
    check(dispatched == 2 && closed == 2 && legacy == 0 && fixture::captures == 1);
    fixture::reply =
        execute_reserved_request({"operation", "execute", fixture::id, fixture::document});
    auto done = submit(true);
    check(done.first == 0 && fixture::sends == 1);
    fixture::send_fails = true;
    auto unknown = submit(true);
    check(unknown.first == 1 && unknown.second.o.at("status").s == "transport_unknown" &&
          fixture::sends == 2);
    check(unknown.second.o.at("command_executed_this_attempt").is_null());
    fixture::send_fails = false;
    fixture::throw_send = true;
    check(submit(true).second.o.at("status").s == "transport_unknown" && fixture::sends == 3);
    fixture::throw_send = false;
    fixture::reply =
        "{\"schema_version\":1,\"kind\":\"sumh.operation.submission\",\"operation_id\":\"" +
        fixture::id + "\",\"request_revision\":\"" + fixture::revision +
        "\",\"status\":\"command_completed\"}";
    check(submit(true).first == 1 && fixture::sends == 4);
    fixture::throw_capture = true;
    fixture::reply =
        execute_reserved_request({"operation", "execute", fixture::id, fixture::document});
    check(submit(true).second.o.at("status").s == "outcome_unknown");
    fixture::throw_capture = false;
    const auto captures = fixture::captures;
    for (const auto& args : std::vector<std::vector<std::string>>{
             {},
             {"operation"},
             {"operation", "execute", fixture::id},
             {"operation", "execute", fixture::id, "{}"},
             {"operation", "execute", fixture::id, doc({"debug", "stealth", "invalid"})}})
        check(parsed(execute_reserved_request(args)).o.at("status").s == "rejected");
    check(fixture::captures == captures);
}
}  // namespace
namespace {
void manager_input_tests() {
    using namespace sumhp::state;
    OperationManagerRequest request;
    std::string error;
    auto base = JsonValue::object();
    base.o["schema_version"] = 1;
    base.o["action"] = "prepare";
    base.o["input"] = parsed(doc({"hide", "disable", fixture::id, fixture::revision}));
    check(parse_operation_manager_input(stringify_json(base), request, error) &&
          request.action == "prepare" && request.input == stringify_json(base.o.at("input")));
    auto execute = base;
    execute.o["action"] = "execute";
    execute.o["operation_id"] = fixture::id;
    check(parse_operation_manager_input(stringify_json(execute), request, error) &&
          request.id == fixture::id);
    for (const auto* action : {"show", "result", "wait"}) {
        auto query = JsonValue::object();
        query.o["schema_version"] = 1;
        query.o["action"] = action;
        query.o["operation_id"] = fixture::id;
        if (std::string(action) == "wait")
            query.o["timeout_ms"] = 5000;
        check(parse_operation_manager_input(stringify_json(query), request, error));
        if (std::string(action) == "wait")
            for (double n : {-1., 0.5, 5001.}) {
                query.o["timeout_ms"] = n;
                check(!parse_operation_manager_input(stringify_json(query), request, error));
            }
        query.o["input"] = base.o.at("input");
        check(!parse_operation_manager_input(stringify_json(query), request, error));
    }
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"hide", "status"},
             {"hide", "add", "/x"},
             {"hide", "remove", "/x"},
             {"hide", "retry-path", "/x"},
             {"config", "apply"},
             {"hide", "disable", "bad", fixture::revision},
             {"sumh", "maps", "remove", "0", "0", "--instance", "1", "--generation", "1"},
             {"hide", "annotate", fixture::id, fixture::revision,
              "{\"note\":\"a\",\"note\":\"b\"}"}}) {
        auto bad = base;
        bad.o["input"] = parsed(doc(args));
        check(!parse_operation_manager_input(stringify_json(bad), request, error));
    }
    for (const auto& change : std::vector<std::function<void(JsonValue&)>>{
             [](auto& v) { v.o["schema_version"] = 2; },
             [](auto& v) { v.o["operation_id"] = fixture::id; },
             [](auto& v) { v.o["action"] = "list"; },
             [](auto& v) { v.o["input"].o["schema_version"] = 2; },
             [](auto& v) { v.o["input"].o["extra"] = true; },
             [](auto& v) { v.o["timeout_ms"] = 0; }}) {
        auto bad = base;
        change(bad);
        check(!parse_operation_manager_input(stringify_json(bad), request, error));
    }
    auto raw = stringify_json(base);
    raw.append(48 * 1024 - raw.size(), ' ');
    check(parse_operation_manager_input(raw, request, error));
    check(!parse_operation_manager_input(raw + ' ', request, error));
    check(!parse_operation_manager_input(stringify_json(base) + " {}", request, error));
    check(!parse_operation_manager_input("{\"schema_version\":1,\"schema_version\":1}", request,
                                         error));
    check(!parse_operation_manager_input(std::string(1, char(0xff)), request, error));
    for (const auto& group :
         std::vector<std::string>{"`$();\n组 ", "--help", "--automatic", "--channel", "--"}) {
        auto literal = base;
        literal.o["input"] = parsed(doc({"hide", "disable-group", group, fixture::revision}));
        check(parse_operation_manager_input(stringify_json(literal), request, error));
        OperationInput input;
        check(parse_operation_input(request.input, input, error) && input.args[2] == group);
    }
    const int before_reads = fixture::reads;
    fixture::document = doc({"hide", "disable", fixture::id, fixture::revision});
    const auto response = submit_operation_bytes(false, {}, fixture::document);
    check(response.exit_code == 0 && fixture::reads == before_reads &&
          response.value.o.at("status").s == "prepared");
    auto fields = JsonValue::object();
    fields.o["path"] = "/" + std::string(4094, '\1');
    fields.o["enabled"] = false;
    fields.o["expect"] = "absent";
    fields.o["name"] = std::string(128, '\2');
    fields.o["note"] = std::string(1024, '\3');
    fields.o["group"] = std::string(128, '\4');
    for (const auto* expectation : {"absent", "same_identity"}) {
        fields.o["expect"] = expectation;
        const auto content =
            doc({"hide", "import-id", fixture::id, fixture::revision, stringify_json(fields)});
        OperationInput imported;
        check(parse_operation_input(content, imported, error) &&
              imported.name == "hide import-id" && imported.args[2] == fixture::id);
        auto request_value = base;
        request_value.o["input"] = parsed(content);
        check(parse_operation_manager_input(stringify_json(request_value), request, error));
        request_value.o["action"] = "execute";
        request_value.o["operation_id"] = fixture::id;
        check(parse_operation_manager_input(stringify_json(request_value), request, error));
        check(command_needs_journal(imported.args));
    }
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"hide", "definitions"},
             {"hide", "definitions", fixture::revision, "64"},
             {"hide", "export"},
             {"api", "meta", "--control-only"},
             {"hide", "import-id", fixture::id, fixture::revision, "{}"},
             {"hide", "import-id", "invalid", fixture::revision, stringify_json(fields)}}) {
        OperationInput input;
        check(!parse_operation_input(doc(args), input, error));
        auto invalid = base;
        invalid.o["input"] = parsed(doc(args));
        check(!parse_operation_manager_input(stringify_json(invalid), request, error));
    }
}
void tracked_maps_submission_tests() {
    using namespace sumhp::state;
    const std::vector<std::string> args{"sumh",
                                        "maps",
                                        "remove",
                                        "18446744073709551615",
                                        "4294967295",
                                        "--instance",
                                        "18446744073709551615",
                                        "--generation",
                                        "9007199254740993"};
    OperationInput input;
    std::string error;
    check(parse_operation_input(doc(args), input, error) && input.args == args &&
          input.name == "sumh maps remove");
    check(!command_needs_journal(
        args));  // Tracking is an explicit local route, not a new daemon mutation.
    const auto digest = fixture::digest_input;
    check(parse_operation_input(" \n" + doc(args) + "\n ", input, error) &&
          fixture::digest_input == digest);
    for (std::size_t field : {3U, 4U, 6U, 8U}) {
        for (const auto& bad :
             std::vector<std::string>{"", "01", "00", "-1", "+1", "1 ", " 1", "1\n", "1.0", "1e1",
                                      "18446744073709551616", "$(x)", "中文"}) {
            auto changed = args;
            changed[field] = bad;
            check(!parse_operation_input(doc(changed), input, error));
        }
    }
    for (std::size_t field : {6U, 8U}) {
        auto changed = args;
        changed[field] = "0";
        check(!parse_operation_input(doc(changed), input, error));
    }
    auto changed = args;
    changed[4] = "4294967296";
    check(!parse_operation_input(doc(changed), input, error));
    changed = args;
    changed[3] = changed[4] = "0";
    check(parse_operation_input(doc(changed), input, error));
    for (const auto& bad : std::vector<std::vector<std::string>>{
             {"sumh", "maps", "list-temporary"},
             {"sumh", "maps", "remove"},
             {"sumh", "maps", "remove", "0", "0", "--generation", "1", "--instance", "1"}})
        check(!parse_operation_input(doc(bad), input, error));
    changed = args;
    changed.push_back("--automatic");
    check(!parse_operation_input(doc(changed), input, error));
    changed = args;
    changed[5] = "--INSTANCE";
    check(!parse_operation_input(doc(changed), input, error));
    const auto sends = fixture::sends, captures = fixture::captures,
               locals = fixture::local_captures;
    fixture::document = doc(args);
    auto prepared = submit_operation_bytes(false, {}, fixture::document);
    check(prepared.exit_code == 0 && prepared.value.o.at("status").s == "prepared");
    check(fixture::captures == captures && fixture::sends == sends);
    fixture::prepare_fails = true;
    check(submit_operation_bytes(false, {}, fixture::document).value.o.at("status").s ==
          "reservation_unconfirmed");
    fixture::prepare_fails = false;
    check(fixture::captures == captures && fixture::sends == sends);
    fixture::captured = {};
    fixture::captured.operation_id = fixture::id;
    fixture::captured.command_executed = true;
    fixture::captured.operation_recorded = true;
    fixture::captured.operation_status = "command_completed";
    auto executed = submit_operation_bytes(true, fixture::id, fixture::document);
    check(executed.exit_code == 0 && executed.value.o.at("status").s == "command_completed");
    check(fixture::local_captures == locals + 1 && fixture::captures == captures + 1 &&
          fixture::sends == sends);
    auto raw =
        parsed(execute_reserved_request({"operation", "execute", fixture::id, fixture::document}));
    check(raw.o.at("status").s == "rejected" && fixture::captures == captures + 1 &&
          fixture::sends == sends);
    fixture::throw_capture = true;
    executed = submit_operation_bytes(true, fixture::id, fixture::document);
    fixture::throw_capture = false;
    check(executed.value.o.at("status").s == "outcome_unknown" &&
          executed.value.o.at("command_executed_this_attempt").is_null());
    check(fixture::local_captures == locals + 2 && fixture::sends == sends);
    fixture::captured = {};
    fixture::captured.operation_id = fixture::id;
    fixture::captured.operation_status = "not_started";
    executed = submit_operation_bytes(true, fixture::id, fixture::document);
    check(executed.value.o.at("status").s == "outcome_unknown" &&
          executed.value.o.at("command_executed_this_attempt").is_null());
    check(executed.value.o.at("error").s == "invalid_or_mismatched_local_result" &&
          fixture::sends == sends);
    fixture::captured.operation_status = "already_started_or_finished";
    executed = submit_operation_bytes(true, fixture::id, fixture::document);
    check(executed.value.o.at("status").s == "outcome_unknown" &&
          executed.value.o.at("observed_record").is_null());
    JsonValue checked;
    check(parse_operation_reply(stringify_json(executed.value), fixture::id, fixture::revision,
                                checked));
}
void tracked_maps_manager_tests() {
    using namespace sumhp::state;
    const std::vector<std::string> args{"sumh",
                                        "maps",
                                        "remove",
                                        "18446744073709551615",
                                        "4294967295",
                                        "--instance",
                                        "18446744073709551615",
                                        "--generation",
                                        "9007199254740993"};
    auto base = JsonValue::object();
    base.o["schema_version"] = 2;
    base.o["resource"] = "temporary_maps";
    base.o["action"] = "prepare";
    base.o["input"] = parsed(doc(args));
    OperationManagerRequest request;
    std::string error;
    check(parse_operation_manager_input(stringify_json(base), request, error) &&
          request.action == "prepare");
    OperationInput input;
    check(parse_operation_input(request.input, input, error) && input.args == args &&
          input.name == "sumh maps remove");
    auto execute = base;
    execute.o["action"] = "execute";
    execute.o["operation_id"] = fixture::id;
    check(parse_operation_manager_input(stringify_json(execute), request, error) &&
          request.id == fixture::id);
    for (const auto* action : {"show", "result", "wait"}) {
        auto query = JsonValue::object();
        query.o["schema_version"] = 2;
        query.o["resource"] = "temporary_maps";
        query.o["action"] = action;
        query.o["operation_id"] = fixture::id;
        if (std::string(action) == "wait")
            query.o["timeout_ms"] = 0;
        check(parse_operation_manager_input(stringify_json(query), request, error) &&
              request.action == action);
        if (std::string(action) == "wait") {
            query.o["timeout_ms"] = 5000;
            check(parse_operation_manager_input(stringify_json(query), request, error));
            for (double n : {-1., 0.5, 5001.}) {
                query.o["timeout_ms"] = n;
                check(!parse_operation_manager_input(stringify_json(query), request, error));
            }
            query.o["timeout_ms"] = 0;
        }
        query.o["input"] = base.o.at("input");
        check(!parse_operation_manager_input(stringify_json(query), request, error));
    }
    for (const auto& change : std::vector<std::function<void(JsonValue&)>>{
             [](auto& v) { v.o["schema_version"] = 1; }, [](auto& v) { v.o["schema_version"] = 3; },
             [](auto& v) { v.o.erase("resource"); }, [](auto& v) { v.o["resource"] = "user_hide"; },
             [](auto& v) { v.o["resource"] = JsonValue{}; },
             [](auto& v) { v.o["operation_id"] = fixture::id; },
             [](auto& v) { v.o["action"] = "list"; }, [](auto& v) { v.o["timeout_ms"] = 0; },
             [](auto& v) { v.o["input"].o["schema_version"] = 2; },
             [](auto& v) { v.o["input"].o["extra"] = true; },
             [](auto& v) { v.o["input"].o["args"].a[3] = 1; }}) {
        auto bad = base;
        change(bad);
        check(!parse_operation_manager_input(stringify_json(bad), request, error));
    }
    for (const auto& invalid : std::vector<std::vector<std::string>>{
             {"hide", "disable", fixture::id, fixture::revision},
             {"config", "apply"},
             {"sumh", "maps", "list-temporary"},
             {"sumh", "maps", "clear"},
             {"sumh", "maps", "apply"},
             {"sumh", "maps", "remove", "0", "0"},
             {"sumh", "maps", "remove", "0", "0", "--generation", "1", "--instance", "1"}}) {
        auto bad = base;
        bad.o["input"] = parsed(doc(invalid));
        check(!parse_operation_manager_input(stringify_json(bad), request, error));
    }
    for (std::size_t index : {3u, 4u, 6u, 8u})
        for (const auto& invalid :
             std::vector<std::string>{"", "00", "01", "+1", "-1", " 1", "1 ", "1\n", "1e0", "1.0",
                                      "18446744073709551616", "$(x)", "中文"}) {
            auto bad = base;
            bad.o["input"].o["args"].a[index] = invalid;
            check(!parse_operation_manager_input(stringify_json(bad), request, error));
        }
    for (std::size_t index : {6u, 8u}) {
        auto bad = base;
        bad.o["input"].o["args"].a[index] = "0";
        check(!parse_operation_manager_input(stringify_json(bad), request, error));
    }
    auto zero = base;
    zero.o["input"].o["args"].a[3] = "0";
    zero.o["input"].o["args"].a[4] = "0";
    check(parse_operation_manager_input(stringify_json(zero), request, error));
    auto too_large = base;
    too_large.o["input"].o["args"].a[4] = "4294967296";
    check(!parse_operation_manager_input(stringify_json(too_large), request, error));
    auto raw = stringify_json(base);
    raw.append(48 * 1024 - raw.size(), ' ');
    check(parse_operation_manager_input(raw, request, error));
    check(!parse_operation_manager_input(raw + ' ', request, error));
    check(!parse_operation_manager_input(stringify_json(base) + " {}", request, error));
    check(!parse_operation_manager_input(
        "{\"schema_version\":2,\"resource\":\"temporary_maps\",\"resource\":\"temporary_maps\"}",
        request, error));
    check(!parse_operation_manager_input(stringify_json(base) + std::string(1, '\0'), request,
                                         error));
    check(!parse_operation_manager_input(stringify_json(base) + std::string(1, char(0xff)), request,
                                         error));
}
}  // namespace
int main(int argc, char** argv) {
    if (argc == 2) {
        const std::string control = argv[1];
        const std::vector<std::string> args{"sumh",       "maps", "remove",       "0", "0",
                                            "--instance", "1",    "--generation", "2"};
        const auto input = doc(args);
        sumhp::state::OperationInput parsed_input;
        std::string error;
        if (control == "tracked-input") {
            const bool accepted = sumhp::state::parse_operation_input(input, parsed_input, error);
            std::cout << (accepted ? "PASS" : "SEMANTIC FAILURE") << " tracked_input_accepted\n";
            return accepted ? 0 : 253;
        }
        if (control == "local-envelope") {
            fixture::captured = {};
            fixture::captured.operation_id = fixture::id;
            fixture::captured.operation_status = "not_started";
            const auto result = sumhp::state::submit_operation_bytes(true, fixture::id, input);
            sumhp::JsonValue decoded;
            const bool valid =
                result.value.o.at("status").s == "outcome_unknown" &&
                sumhp::state::parse_operation_reply(sumhp::stringify_json(result.value),
                                                    fixture::id, fixture::revision, decoded) &&
                fixture::local_captures == 1 && fixture::sends == 0;
            std::cout << (valid ? "PASS" : "SEMANTIC FAILURE") << " local_result_normalized\n";
            return valid ? 0 : 253;
        }
        return 2;
    }
    assert(argc == 1);
    input_tests();
    receipt_tests();
    workflow_tests();
    manager_input_tests();
    tracked_maps_manager_tests();
    tracked_maps_submission_tests();
    std::cout << fixture::assertions << " operation submission assertions passed\n";
}
