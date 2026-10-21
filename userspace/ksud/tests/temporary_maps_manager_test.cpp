#include "mount/temporary_maps_manager.hpp"
#include <cassert>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>

using namespace sumhp;
namespace fixture_transport {
#include "temporary_maps_envelope_under_test.inc"
}
namespace {
struct Call {
    unsigned long command;
    std::function<int(void*)> invoke;
};
std::deque<Call> calls;
int checks = 0;
std::filesystem::path fixtures;
constexpr int supported = SUMH_FEATURE_RULE_INVENTORY | SUMH_FEATURE_TEMPORARY_MAPS_DELETE;
const std::string capabilities_json = R"({"schema_version":1,"action":"capabilities"})";
const std::string list_json = R"({"schema_version":1,"action":"list"})";
const std::string remove_json =
    R"({"schema_version":1,"action":"remove","instance_id":"18446744073709551615","generation":"9007199254740993","target_ino":"0","target_dev":"0"})";
void features(int bits = supported, int error = 0) {
    calls.push_back({SUMH_IOC_GET_FEATURES, [=](void* p) {
                         *static_cast<int*>(p) = bits;
                         errno = error;
                         return error ? -1 : 0;
                     }});
}
json::Value dispatch(const std::string& bytes, const std::string& fixture = {}) {
    temporary_maps::Request request;
    assert(temporary_maps::parse(bytes, request));
    const auto output = temporary_maps::dispatch(request);
    assert(calls.empty() && output.size() <= temporary_maps::response_bytes);
    const auto value = json::parse(output);
    assert(value.at("schema_version").n == 1 &&
           value.at("resource").s == "temporary_maps_dispatch");
    assert(value.at("action").s == request.action && value.at("result").is_object());
    const auto envelope = fixture_transport::envelope(output, 0);
    assert(envelope.size() <= temporary_maps::envelope_bytes);
    assert(json::parse(envelope).at("stdout").s == output && json::parse(envelope).at("ok").b);
    if (!fixtures.empty() && !fixture.empty()) {
        std::filesystem::create_directories(fixtures);
        std::ofstream file(fixtures / (fixture + ".json"), std::ios::binary);
        file << output;
        file.flush();
        assert(file.good());
        std::ofstream wrapped(fixtures / (fixture + "-envelope.json"), std::ios::binary);
        wrapped << envelope;
        wrapped.flush();
        assert(wrapped.good());
    }
    ++checks;
    return value;
}
void grammar() {
    temporary_maps::Request request;
    for (const auto& input : {capabilities_json, list_json, remove_json}) {
        assert(temporary_maps::parse(input, request));
        assert(temporary_maps::parse_arguments({"sumh", "maps", "manager", input, "--channel"},
                                               request));
        ++checks;
    }
    for (const auto& input : std::vector<std::string>{
             "", "{}", "[]", list_json + " {}",
             "{\"schema_version\":1,\"action\":\"list\",\"action\":\"remove\"}",
             "{\"schema_version\":2,\"action\":\"list\"}",
             "{\"schema_version\":true,\"action\":\"list\"}",
             "{\"schema_version\":1,\"action\":\"clear\"}",
             "{\"schema_version\":1,\"action\":\"capabilities\",\"extra\":null}",
             "{\"schema_version\":1,\"action\":\"list\",\"cursor\":\"0\"}",
             list_json + std::string(1, '\0'), list_json + std::string(1, char(0xff)),
             std::string(1025, ' ')}) {
        assert(!temporary_maps::parse(input, request) && calls.empty());
        ++checks;
    }
    for (const auto& field : {"instance_id", "generation", "target_ino", "target_dev"}) {
        for (const auto& bad :
             std::vector<std::string>{"", "00", "01", "+1", "-1", " 1", "1 ", "1\n", "1.0", "1e0",
                                      "18446744073709551616", std::string(21, '1'),
                                      std::string("1\0", 2), "中文", "$(x)"}) {
            auto value = json::parse(remove_json);
            value[field] = bad;
            assert(!temporary_maps::parse(json::dump(value), request));
            ++checks;
        }
        for (auto bad : {json::Value(1), json::Value(), json::Value(true)}) {
            auto value = json::parse(remove_json);
            value[field] = bad;
            assert(!temporary_maps::parse(json::dump(value), request));
            ++checks;
        }
    }
    for (const auto& field : {"instance_id", "generation"}) {
        auto value = json::parse(remove_json);
        value[field] = "0";
        assert(!temporary_maps::parse(json::dump(value), request));
        ++checks;
    }
    auto value = json::parse(remove_json);
    value["target_dev"] = "4294967296";
    assert(!temporary_maps::parse(json::dump(value), request));
    ++checks;
    value["target_dev"] = "4294967295";
    value["target_ino"] = "18446744073709551615";
    assert(temporary_maps::parse(json::dump(value), request));
    ++checks;
    auto list = json::parse(list_json);
    list["instance_id"] = "1";
    list["generation"] = "2";
    list["cursor"] = "18446744073709551615";
    assert(temporary_maps::parse(json::dump(list), request));
    ++checks;
    list["cursor"] = "01";
    assert(!temporary_maps::parse(json::dump(list), request));
    ++checks;
    auto padded = list_json;
    padded.resize(temporary_maps::input_bytes, ' ');
    assert(temporary_maps::parse(padded, request));
    assert(!temporary_maps::parse(padded + " ", request));
    checks += 2;
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"sumh", "maps", "manager"},
             {"sumh", "maps", "manager", list_json},
             {"sumh", "maps", "manager", list_json, "extra", "--channel"},
             {"sumh", "maps", "manager", list_json, "--channel", "extra"},
             {"sumh", "maps", "remove", remove_json, "--channel"}}) {
        assert(!temporary_maps::parse_arguments(args, request));
        ++checks;
    }
}
void capabilities() {
    for (int bits :
         {0, SUMH_FEATURE_RULE_INVENTORY, SUMH_FEATURE_TEMPORARY_MAPS_DELETE, supported}) {
        features(bits);
        const auto value = dispatch(capabilities_json, bits == supported ? "capabilities-supported"
                                                       : bits == 0 ? "capabilities-unsupported"
                                                       : bits == SUMH_FEATURE_RULE_INVENTORY
                                                           ? "capabilities-inventory-only"
                                                           : "capabilities-delete-only");
        assert(value.at("exit_code").n == 0 && value.at("result").at("errno").n == 0);
        assert(value.at("result").at("inventory").s ==
               (bits & SUMH_FEATURE_RULE_INVENTORY ? "supported" : "unsupported"));
        assert(value.at("result").at("delete").s ==
               (bits == supported ? "supported" : "unsupported"));
    }
    for (int error : {EIO, EINTR, EACCES, ENOTTY}) {
        features(0, error);
        const auto value =
            dispatch(capabilities_json, "capabilities-error-" + std::to_string(error));
        assert(value.at("exit_code").n == 1 && value.at("result").at("errno").n == error);
        assert(value.at("result").at("inventory").s == "unknown" &&
               value.at("result").at("delete").s == "unknown");
    }
    features(-1);
    const auto negative = dispatch(capabilities_json, "capabilities-malformed-negative");
    assert(negative.at("exit_code").n == 1 && negative.at("result").at("errno").n == EIO);
    assert(negative.at("result").at("inventory").s == "unknown" &&
           negative.at("result").at("delete").s == "unknown");
}
void pages() {
    for (int mode = 0; mode < 5; ++mode) {
        calls.push_back({SUMH_IOC_GET_VERSION, [](void* p) {
                             *static_cast<int*>(p) = 1;
                             return 0;
                         }});
        features(mode == 1 ? 0 : supported);
        if (mode != 1)
            calls.push_back({SUMH_IOC_GET_RULE_INVENTORY, [=](void* p) {
                                 auto& page = *static_cast<sumh_rule_inventory_arg*>(p);
                                 assert(page.category == SUMH_INVENTORY_MAPS_TEMPORARY &&
                                        !page.expected_instance_id && !page.cursor);
                                 if (mode == 2) {
                                     errno = ESTALE;
                                     return -1;
                                 }
                                 page.instance_id = UINT64_MAX;
                                 page.generation = 9007199254740993ULL;
                                 page.count = 1;
                                 page.next_cursor = 1;
                                 page.complete = mode == 4 ? 0 : 1;
                                 page.flags = 1;
                                 page.supported_categories = SUMH_INVENTORY_SUPPORTED_CATEGORIES;
                                 auto& entry = page.entry;
                                 entry.category = page.category;
                                 entry.id_kind = SUMH_INVENTORY_ID_GENERATION_ORDINAL;
                                 entry.id = 1;
                                 entry.target_ino = UINT64_MAX;
                                 entry.target_dev = UINT32_MAX;
                                 entry.spoofed_ino = 9007199254740993ULL;
                                 entry.spoofed_dev = 0;
                                 const auto path = mode == 3 ? std::string("/a\xff", 3)
                                                             : std::string("/synthetic/$() ` ");
                                 entry.path2_len = static_cast<__u32>(path.size());
                                 std::memcpy(entry.path2, path.data(), path.size());
                                 return 0;
                             }});
        const auto value = dispatch(
            list_json, std::vector<std::string>{"page-high-u64", "page-unsupported", "page-stale",
                                                "page-invalid-utf8", "page-partial"}[mode]);
        const auto& result = value.at("result");
        assert(value.at("exit_code").n == (mode == 2 ? 1 : 0));
        assert(result.at("boot_id").is_null() && result.at("legacy").is_null());
        assert(result.at("status").s == (mode == 1   ? "unsupported"
                                         : mode == 2 ? "stale"
                                                     : "known"));
    }
}
void remove() {
    for (int error : {0, ESTALE, ENOENT, EIO, EINTR}) {
        features();
        calls.push_back({SUMH_IOC_DELETE_TEMPORARY_MAPS, [=](void* p) {
                             const auto& request = *static_cast<sumh_maps_delete_arg*>(p);
                             assert(request.size == 40 && request.version == 1 &&
                                    !request.target_ino && !request.target_dev);
                             assert(request.expected_instance_id == UINT64_MAX &&
                                    request.expected_generation == 9007199254740993ULL);
                             errno = error;
                             return error ? -1 : 0;
                         }});
        const auto value =
            dispatch(remove_json, error == 0        ? "delete-acknowledged"
                                  : error == ESTALE ? "delete-stale"
                                  : error == ENOENT ? "delete-missing"
                                                    : "delete-unknown-" + std::to_string(error));
        assert(value.at("exit_code").n == (error ? 1 : 0));
        const auto& result = value.at("result");
        assert(result.at("ioctl_attempted").b && result.at("acknowledged").b == !error);
        assert(result.at("request").at("target_ino").s == "0");
        assert(result.at("status").s == (!error                           ? "deleted"
                                         : error == EIO || error == EINTR ? "outcome_unknown"
                                                                          : "not_applied"));
    }
    features(0);
    const auto value = dispatch(remove_json, "delete-unsupported");
    assert(value.at("exit_code").n == 1 && !value.at("result").at("ioctl_attempted").b);
}
}  // namespace
extern "C" int ksu_sumh_ioctl(unsigned long command, void* argument) {
    assert(!calls.empty());
    auto call = std::move(calls.front());
    calls.pop_front();
    assert(command == call.command);
    return call.invoke(argument);
}
int main(int argc, char** argv) {
    assert(argc == 1 || argc == 2);
    if (argc == 2)
        fixtures = argv[1];
#ifdef SUMH_TEMPORARY_MAPS_ANDROID_ERRNO
    static_assert(EOPNOTSUPP == 95 && ENOTSUP == 95 && EOVERFLOW == 75 && EPROTO == 71);
    std::cout << "Synthetic Android-protocol fixture target: Linux errno ABI normalized; no "
                 "Android execution\n";
#else
    assert(argc == 1);  // Native Windows errno must never enter Android fixtures.
#endif
    grammar();
    capabilities();
    pages();
    remove();
    std::cout << "Temporary Maps Manager grammar/actual callback/client: " << checks
              << " cases passed\n";
}
