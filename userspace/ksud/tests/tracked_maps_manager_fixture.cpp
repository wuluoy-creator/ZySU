// Production submission/Journal/capture/resource query, with modeled filesystem
// and ioctl boundaries. Unlike the ordinary state fixture this links mbed TLS
// SHA-256 and explicitly uses synthetic Android errno values, not host errno.
#include <fstream>
#include "core/operation_manager_input.hpp"
#include "sumh_state_ledger_test.cpp"

namespace sumhp {
bool validate_config_patch(const json::Value&, std::string&) {
    assert(false && "Maps fixture must not enter config validation");
    return false;
}
bool exchange_reserved_operation(const std::string&, const std::string&, std::string&,
                                 std::string&) {
    assert(false && "tracked Manager removal must not use daemon transport");
    return false;
}
namespace state {
bool read_operation_input(const std::string&, std::string&, std::string&) {
    assert(false && "Manager bytes must not be read from an arbitrary path");
    return false;
}
}  // namespace state
}  // namespace sumhp
namespace fixture_transport {
#include "temporary_maps_envelope_under_test.inc"
}
namespace {
fs::path output_directory;
std::string manager_dispatch(const std::string& bytes) {
    state::OperationManagerRequest request;
    std::string error;
    assert(state::parse_operation_manager_input(bytes, request, error));
    // The body is extracted verbatim from embedded.cpp's Manager callback.
#include "tracked_maps_manager_dispatch_under_test.inc"
}
JsonValue request(const std::string& action, const std::vector<std::string>& args = {},
                  const std::string& id = {}) {
    auto value = JsonValue::object();
    value.o["schema_version"] = 2;
    value.o["resource"] = "temporary_maps";
    value.o["action"] = action;
    if (action != "prepare")
        value.o["operation_id"] = id;
    if (action == "prepare" || action == "execute") {
        auto input = JsonValue::object();
        input.o["schema_version"] = 1;
        input.o["args"] = JsonValue::array();
        for (const auto& argument : args)
            input.o["args"].a.emplace_back(argument);
        value.o["input"] = std::move(input);
    }
    if (action == "wait")
        value.o["timeout_ms"] = 0;
    return value;
}
void save(const std::string& name, const std::string& bytes) {
    const auto path = output_directory / name;
    assert(!fs::exists(path));
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.flush();
    assert(file.good());
}
JsonValue generate(const std::string& name, const JsonValue& input) {
    const auto bytes = stringify_json(input);
    const auto output = manager_dispatch(bytes);
    save(name + "-request.json", bytes);
    save(name + ".json", output);
    save(name + "-envelope.json", fixture_transport::envelope(output, 0));
    return decoded(output).o.at("result");
}
void scenario(const std::string& name, int number = 0, int bits = -1, int capability_error = 0,
              int write_failure = 0, bool next_boot = false) {
    reset();
    const auto args = tracked_args();
    const auto prepared = generate(name + "-prepare", request("prepare", args));
    const auto id = prepared.o.at("operation_id").s;
    assert(prepared.o.at("status").s == "prepared" && tracked_delete_calls == 0);
    const auto initial_writes = write_calls;
    fail_write = write_failure;
    tracked_response(args, number, bits, capability_error);
    const auto executed = generate(name + "-execute", request("execute", args, id));
    assert(tracked_ioctls.empty() && command_calls == 0 && kernel_reads == 0);
    assert(write_calls > initial_writes);
    const auto deletes = tracked_delete_calls, writes = write_calls,
               features = tracked_feature_calls;
    fail_write = 0;
    if (next_boot)
        boot_id = "boot-b";
    generate(name + "-show", request("show", {}, id));
    generate(name + "-wait", request("wait", {}, id));
    generate(name + "-result", request("result", {}, id));
    assert(tracked_delete_calls == deletes && tracked_feature_calls == features &&
           write_calls == writes);
    const auto duplicate = manager_dispatch(stringify_json(request("execute", args, id)));
    assert(decoded(duplicate).o.at("result").o.at("command_executed_this_attempt").is_bool());
    assert(!decoded(duplicate).o.at("result").o.at("command_executed_this_attempt").b &&
           tracked_delete_calls == deletes);
    if (number == 0 && bits == -1 && capability_error == 0 && write_failure == 0)
        assert(executed.o.at("status").s == "command_completed" && deletes == 1);
}
}  // namespace
int main(int argc, char** argv) {
    assert(argc == 2);
    output_directory = argv[1];
    assert(fs::is_directory(output_directory) && fs::is_empty(output_directory));
    static_assert(EOPNOTSUPP == 95 && ESTALE == 116 && ENOTTY == 25);
    const auto empty = state::revision_for_bytes("");
    assert(empty == "sha256-v1:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(state::revision_for_bytes("abc") ==
           "sha256-v1:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    save("sha256-vectors.json",
         "{\"empty\":\"" + empty + "\",\"abc\":\"" + state::revision_for_bytes("abc") + "\"}");
    scenario("deleted");
    scenario("stale", ESTALE);
    scenario("missing", ENOENT);
    scenario("unknown-eintr", EINTR);
    scenario("unknown-eio", EIO);
    scenario("unsupported", 0, 0);
    scenario("feature-error", 0, 0, EACCES);
    scenario("resource-unavailable", 0, -1, 0, 3);
    scenario("finish-unknown", 0, -1, 0, 4);
    scenario("historical", 0, -1, 0, 0, true);
    std::cout << "Generated production tracked Manager fixtures with real mbed TLS SHA-256 and "
                 "synthetic Android errno; no device or durability claim\n";
}
