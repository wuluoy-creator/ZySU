#include <cassert>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include "mount/maps_temporary.hpp"

using namespace sumhp;
namespace {
struct Call {
    unsigned long command;
    std::function<int(void*)> invoke;
};
std::deque<Call> calls;
int checks = 0;
constexpr auto max64 = UINT64_MAX;
constexpr int supported = SUMH_FEATURE_TEMPORARY_MAPS_DELETE | SUMH_FEATURE_RULE_INVENTORY;
void capabilities(int bits = supported, int error = 0) {
    calls.push_back({SUMH_IOC_GET_FEATURES, [=](void* p) {
                         *static_cast<int*>(p) = bits;
                         errno = error;
                         return error ? -1 : 0;
                     }});
}
void deletion(std::uint64_t ino, std::uint64_t dev, std::uint64_t instance,
              std::uint64_t generation, int error = 0, int result = 0) {
    calls.push_back({SUMH_IOC_DELETE_TEMPORARY_MAPS, [=](void* p) {
                         sumh_maps_delete_arg expected{};
                         expected.size = 40;
                         expected.version = 1;
                         expected.expected_instance_id = instance;
                         expected.expected_generation = generation;
                         expected.target_ino = ino;
                         expected.target_dev = dev;
                         assert(!std::memcmp(&expected, p, sizeof(expected)));
                         errno = error;
                         return error ? -1 : result;
                     }});
}
std::vector<std::string> remove_args() {
    return {"sumh",
            "maps",
            "remove",
            "0",
            "0",
            "--instance",
            std::to_string(max64),
            "--generation",
            "9007199254740993"};
}
void client_contract() {
    bool attempted = true;
    assert(!sumh::delete_temporary_maps_rule(0, 0, 0, 1, attempted) && !attempted &&
           errno == EINVAL);
    assert(!sumh::delete_temporary_maps_rule(0, 0, 1, 0, attempted) && !attempted &&
           errno == EINVAL);
    assert(!sumh::delete_temporary_maps_rule(0, 0x100000000ULL, 1, 1, attempted) && !attempted &&
           errno == EINVAL);
    checks += 3;
    for (int bits : {0, SUMH_FEATURE_RULE_INVENTORY, SUMH_FEATURE_TEMPORARY_MAPS_DELETE}) {
        capabilities(bits);
        assert(!sumh::delete_temporary_maps_rule(0, 0, 1, 1, attempted) && !attempted &&
               errno == EOPNOTSUPP);
        ++checks;
    }
    for (int error : {EIO, EACCES, EINTR, ENOTTY}) {
        capabilities(0, error);
        assert(!sumh::delete_temporary_maps_rule(0, 0, 1, 1, attempted) && !attempted &&
               errno == error);
        ++checks;
    }
    for (auto ino : {std::uint64_t(0), max64}) {
        capabilities();
        deletion(ino, UINT32_MAX, max64, max64);
        assert(sumh::delete_temporary_maps_rule(ino, UINT32_MAX, max64, max64, attempted) &&
               attempted);
        ++checks;
    }
    for (int error : {ESTALE, ENOENT, ENOMEM, ESHUTDOWN, EOVERFLOW, EFAULT, EINVAL, EINTR, EIO}) {
        capabilities();
        deletion(12, 7, 9, 17, error);
        assert(!sumh::delete_temporary_maps_rule(12, 7, 9, 17, attempted) && attempted &&
               errno == error);
        ++checks;
    }
    capabilities();
    deletion(0, 0, 1, 1, 0, -1);
    assert(!sumh::delete_temporary_maps_rule(0, 0, 1, 1, attempted) && attempted && errno == EIO);
    assert(sumh::feature_names(SUMH_FEATURE_TEMPORARY_MAPS_DELETE) ==
           std::vector<std::string>{"temporary_maps_delete"});
    assert(calls.empty());
    checks += 2;
}
void parser_and_results() {
    for (std::size_t index : {3U, 4U, 6U, 8U}) {
        for (const auto& value : std::vector<std::string>{
                 "", "-1", "+1", " 1", "1 ", "1\n", "0x1", "1e2", "18446744073709551616",
                 "000000000000000000001", "中", std::string("1\0", 2)}) {
            auto args = remove_args();
            args[index] = value;
            std::ostringstream out, err;
            assert(maps_temporary::command(args, out, err) == 1 && calls.empty());
            const auto value_json = json::parse(out.str());
            assert(value_json.find("error_code")->s == "invalid_request" &&
                   !value_json.find("ioctl_attempted")->b);
            ++checks;
        }
    }
    for (auto index : {6U, 8U}) {
        auto args = remove_args();
        args[index] = "0";
        std::ostringstream out, err;
        assert(maps_temporary::command(args, out, err) == 1 && calls.empty());
        ++checks;
    }
    for (int mode = 0; mode < 6; ++mode) {
        auto args = remove_args();
        if (mode == 0)
            args[4] = "4294967296";
        if (mode == 1)
            args[5] = "--generation";
        if (mode == 2)
            args[7] = "--instance";
        if (mode == 3)
            args.push_back("--help");
        if (mode == 4)
            args.pop_back();
        if (mode == 5)
            args[2] = "clear";
        std::ostringstream out, err;
        assert(maps_temporary::command(args, out, err) == 1 && calls.empty());
        ++checks;
    }
    for (int error : {0, ESTALE, ENOENT, ENOMEM, ESHUTDOWN, EOVERFLOW, EFAULT, EINVAL, EACCES,
                      EPERM, ENOTTY, EOPNOTSUPP, EINTR, EIO}) {
        capabilities();
        deletion(0, 0, max64, 9007199254740993ULL, error);
        std::ostringstream out, err;
        assert(maps_temporary::command(remove_args(), out, err) == (error ? 1 : 0));
        const auto value = json::parse(out.str());
        const bool unknown = error == EINTR || error == EIO;
        assert(value.find("status")->s ==
               (error ? (unknown ? "outcome_unknown" : "not_applied") : "deleted"));
        assert(value.find("request")->find("expected_instance_id")->s == std::to_string(max64));
        assert(value.find("request")->find("expected_generation")->s == "9007199254740993");
        assert(value.find("operation_id")->is_null() && !value.find("saved")->b &&
               !value.find("observer_verified")->b);
        assert(value.find("acknowledged")->b == !error && value.find("errno")->n == error &&
               calls.empty());
        assert(out.str().size() < 1024 && err.str().empty() == !error);
        ++checks;
    }
    capabilities(0);
    std::ostringstream out, err;
    assert(maps_temporary::command(remove_args(), out, err) == 1 && calls.empty());
    assert(json::parse(out.str()).find("status")->s == "not_applied");
    ++checks;
}
void list_pages() {
    for (auto tail : std::vector<std::vector<std::string>>{{"1"},
                                                           {"1", "2"},
                                                           {"0", "1", "0"},
                                                           {"1", "0", "0"},
                                                           {"1", "2", "-1"},
                                                           {"--channel"}}) {
        std::vector<std::string> args{"sumh", "maps", "list-temporary"};
        args.insert(args.end(), tail.begin(), tail.end());
        std::ostringstream out, err;
        assert(maps_temporary::command(args, out, err) == 1 && calls.empty());
        ++checks;
    }
    for (int mode = 0; mode < 3; ++mode) {
        calls.push_back({SUMH_IOC_GET_VERSION, [](void* p) {
                             *static_cast<int*>(p) = 1;
                             return 0;
                         }});
        capabilities(mode == 2 ? 0 : supported);
        if (mode != 2)
            calls.push_back({SUMH_IOC_GET_RULE_INVENTORY, [=](void* p) {
                                 auto& page = *static_cast<sumh_rule_inventory_arg*>(p);
                                 assert(page.category == SUMH_INVENTORY_MAPS_TEMPORARY &&
                                        page.cursor == 0);
                                 assert(page.expected_instance_id == (mode ? max64 : 0) &&
                                        page.expected_generation == (mode ? 9 : 0));
                                 if (mode) {
                                     errno = ESTALE;
                                     return -1;
                                 }
                                 page.instance_id = max64;
                                 page.generation = 9;
                                 page.complete = 1;
                                 page.flags = 1;
                                 page.supported_categories = SUMH_INVENTORY_SUPPORTED_CATEGORIES;
                                 return 0;
                             }});
        auto args = std::vector<std::string>{"sumh", "maps", "list-temporary"};
        if (mode == 1)
            args.insert(args.end(), {std::to_string(max64), "9", "0"});
        std::ostringstream out, err;
        assert(maps_temporary::command(args, out, err) == (mode == 1 ? 1 : 0));
        const auto value = json::parse(out.str());
        assert(value.find("status")->s == (mode == 0   ? "known"
                                           : mode == 1 ? "stale"
                                                       : "unsupported"));
        assert(value.find("legacy")->is_null() && value.find("boot_id")->is_null());
        assert(calls.empty());
        ++checks;
    }
}
struct FlushFailure : std::stringbuf {
    int sync() override { return -1; }
};
void output_failure() {
    for (bool unsupported : {false, true}) {
        calls.push_back({SUMH_IOC_GET_VERSION, [](void* p) {
                             *static_cast<int*>(p) = 1;
                             return 0;
                         }});
        capabilities(unsupported ? 0 : supported);
        if (!unsupported)
            calls.push_back({SUMH_IOC_GET_RULE_INVENTORY, [](void* p) {
                                 auto& page = *static_cast<sumh_rule_inventory_arg*>(p);
                                 page.instance_id = 3;
                                 page.generation = 5;
                                 page.complete = 1;
                                 page.flags = 1;
                                 page.supported_categories = SUMH_INVENTORY_SUPPORTED_CATEGORIES;
                                 return 0;
                             }});
        FlushFailure buffer;
        std::ostream out(&buffer);
        std::ostringstream err;
        assert(out.good());
        assert(maps_temporary::command({"sumh", "maps", "list-temporary"}, out, err) == 1);
        assert(errno == EIO && !out.good() && calls.empty());
        ++checks;
    }
    capabilities();
    deletion(0, 0, max64, 9007199254740993ULL);
    FlushFailure buffer;
    std::ostream out(&buffer);
    std::ostringstream err;
    assert(maps_temporary::command(remove_args(), out, err) == 1);
    assert(errno == EIO && !out.good() && calls.empty());
    assert(
        json::parse(buffer.str()).find("acknowledged")->b);  // One successful mutation, no replay.
    ++checks;
}
}  // namespace
extern "C" int ksu_sumh_ioctl(unsigned long command, void* argument) {
    assert(!calls.empty());
    auto call = std::move(calls.front());
    calls.pop_front();
    assert(command == call.command);
    return call.invoke(argument);
}
int main() {
    client_contract();
    parser_and_results();
    list_pages();
    output_failure();
    std::cout << "Temporary Maps client/parser/results: " << checks << " cases passed\n";
}
