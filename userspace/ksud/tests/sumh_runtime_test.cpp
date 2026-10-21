#include <cassert>
#include <cstddef>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include "core/runtime_status.hpp"
#include "mount/runtime_apply.hpp"

namespace {
using namespace sumhp;
using namespace sumhp::sumh;
using mount::sumh::FeatureApplyState;
struct Call {
    unsigned long command;
    std::function<int(void*)> run;
};
std::deque<Call> calls;
void error(unsigned long cmd, int code) {
    calls.push_back({cmd, [code](void*) {
                         errno = code;
                         return -1;
                     }});
}
sumh_runtime_state_arg sample(std::uint64_t generation = 7, bool enabled = false) {
    sumh_runtime_state_arg state{};
    state.size = sizeof(state);
    state.version = 1;
    state.count = 6;
    state.flags = 3;
    state.generation = generation;
    state.instance_id = 0xf123456789abcdefULL;
    for (std::uint32_t i = 0; i < 6; ++i) {
        state.entries[i].id = i + 1;
        state.entries[i].supported = 1;
        state.entries[i].configured = enabled;
        state.entries[i].active = enabled;
    }
    return state;
}
void get(sumh_runtime_state_arg state) {
    calls.push_back({SUMH_IOC_GET_RUNTIME_STATE, [state](void* value) {
                         sumh_runtime_state_arg expected{};
                         expected.size = sizeof(expected);
                         expected.version = 1;
                         assert(std::memcmp(&expected, value, sizeof(expected)) == 0);
                         *static_cast<sumh_runtime_state_arg*>(value) = state;
                         return 0;
                     }});
}
void set(std::uint64_t generation, bool enabled, int code = 0) {
    calls.push_back({SUMH_IOC_SET_RUNTIME_FEATURE, [=](void* value) {
                         sumh_runtime_set_arg expected{};
                         expected.size = sizeof(expected);
                         expected.version = 1;
                         expected.id = SUMH_RUNTIME_DEBUG;
                         expected.enable = enabled;
                         expected.expected_generation = generation;
                         expected.expected_instance_id = 0xf123456789abcdefULL;
                         assert(std::memcmp(&expected, value, sizeof(expected)) == 0);
                         errno = code;
                         return code ? -1 : 0;
                     }});
}
void protocol(int version = 1) {
    calls.push_back({SUMH_IOC_GET_VERSION, [version](void* value) {
                         *static_cast<int*>(value) = version;
                         return 0;
                     }});
}
void capabilities(int bits) {
    calls.push_back({SUMH_IOC_GET_FEATURES, [bits](void* value) {
                         *static_cast<int*>(value) = bits;
                         return 0;
                     }});
}
void test_client() {
    static_assert(sizeof(sumh_runtime_state_arg) == 240);
    static_assert(sizeof(sumh_runtime_set_arg) == 48);
    static_assert(offsetof(sumh_runtime_state_arg, entries) == 48);
    static_assert(offsetof(sumh_runtime_set_arg, expected_generation) == 16);
    static_assert(SUMH_IOC_GET_RUNTIME_STATE == 0xc0f05331UL);
    static_assert(SUMH_IOC_SET_RUNTIME_FEATURE == 0x40305332UL);
    RuntimeState out;
    get(sample());
    assert(runtime_state(out));
    assert(out.entries.size() == 6 && out.generation == 7);
    assert(out.instance_id == 0xf123456789abcdefULL);
    for (int failure = 0; failure < 20; ++failure) {
        auto bad = sample();
        switch (failure) {
        case 0:
            bad.size--;
            break;
        case 1:
            bad.version++;
            break;
        case 2:
            bad.count--;
            break;
        case 3:
            bad.generation = 0;
            break;
        case 4:
            bad.instance_id = 0;
            break;
        case 5:
            bad.flags = 0;
            break;
        case 6:
            bad.mount_hide_mode = 2;
            break;
        case 7:
            bad.reserved[2] = 1;
            break;
        case 8:
            bad.entries[0].id = 0;
            break;
        case 9:
            bad.entries[1].id = 1;
            break;
        case 10:
            bad.entries[0].supported = 2;
            break;
        case 11:
            bad.entries[0].configured = 2;
            break;
        case 12:
            bad.entries[0].active = 2;
            break;
        case 13:
            bad.entries[0].degraded = 2;
            break;
        case 14:
            bad.entries[0].flags = 1;
            break;
        case 15:
            bad.entries[0].reserved = 1;
            break;
        case 16:
            bad.entries[0].last_error = 1;
            break;
        case 17:
            bad.entries[0].last_error = -4096;
            break;
        case 18:
            bad.entries[0].active = 1;
            break;
        case 19:
            bad.entries[0].configured = 1;
            break;
        }
        get(bad);
        assert(!runtime_state(out) && errno == EPROTO);
        assert(out.generation == 7 && !out.entries[0].configured);
    }
    error(SUMH_IOC_GET_RUNTIME_STATE, ENOTTY);
    assert(!runtime_state(out) && errno == ENOTTY && out.generation == 7);
    set(7, true);
    assert(set_runtime_feature(out, RuntimeFeature::Debug, true));
    set(7, true, ESTALE);
    assert(!set_runtime_feature(out, RuntimeFeature::Debug, true));
    assert(errno == ESTALE);
    assert(!set_runtime_feature(out, RuntimeFeature::KernelBuild, true) && errno == EINVAL);
    assert(!set_runtime_feature(out, RuntimeFeature::Debug, true, 1) && errno == EINVAL);
    assert(!set_runtime_feature(out, RuntimeFeature::MountHide, true, 1) && errno == EINVAL);
    out.generation = 0;
    assert(!set_runtime_feature(out, RuntimeFeature::Debug, true) && errno == EINVAL);
    assert(calls.empty());
}
void test_apply() {
    const auto apply = [] {
        return mount::sumh::apply_runtime_feature(RuntimeFeature::Debug, true);
    };
    get(sample());
    set(7, true);
    get(sample(8, true));
    auto outcome = apply();
    assert(outcome.state == FeatureApplyState::Applied && outcome.previous_state_known &&
           !outcome.previous_enabled);
    // A synthetic inactive sample is distinct from successful configuration.
    // Real global-suspension semantics per feature are covered by kernel tests.
    auto suspended = sample(8, true);
    suspended.entries[0].active = 0;
    suspended.entries[0].degraded = 1;
    get(sample());
    set(7, true);
    get(suspended);
    assert(apply().state == FeatureApplyState::Applied);
    error(SUMH_IOC_GET_RUNTIME_STATE, EIO);
    assert(apply().state == FeatureApplyState::Failed);
    auto unavailable = sample();
    unavailable.entries[0].supported = 0;
    get(unavailable);
    assert(apply().state == FeatureApplyState::Unsupported);
    get(sample());
    set(7, true, ESTALE);
    assert(apply().state == FeatureApplyState::Failed);
    get(sample());
    set(7, true);
    error(SUMH_IOC_GET_RUNTIME_STATE, EIO);
    assert(apply().state == FeatureApplyState::Unknown);
    get(sample());
    set(7, true);
    get(sample(9, true));
    assert(apply().state == FeatureApplyState::Unknown);
    auto reinitialized = sample(8, true);
    reinitialized.instance_id++;
    get(sample());
    set(7, true);
    get(reinitialized);
    assert(apply().state == FeatureApplyState::Unknown);
    // A failed transport cannot authorize rollback even at the expected token.
    get(sample());
    set(7, true, EIO);
    get(sample(8, true));
    assert(apply().state == FeatureApplyState::Unknown);
    get(sample());
    set(7, true, EOPNOTSUPP);
    get(sample(8));
    assert(apply().state == FeatureApplyState::Failed);
    // Acknowledged own mutation + exact token permits conditional rollback.
    get(sample());
    set(7, true);
    get(sample(8));
    set(8, false);
    get(sample(9));
    assert(apply().state == FeatureApplyState::RolledBack);
    get(sample());
    set(7, true);
    get(sample(8));
    set(8, false, ESTALE);
    assert(apply().state == FeatureApplyState::Unknown);
    get(sample());
    set(7, true);
    get(sample(8));
    set(8, false, EIO);
    assert(apply().state == FeatureApplyState::RollbackFailed);
    get(sample());
    set(7, true);
    get(sample(8));
    set(8, false);
    error(SUMH_IOC_GET_RUNTIME_STATE, EIO);
    assert(apply().state == FeatureApplyState::Unknown);
    get(sample());
    set(7, true);
    get(sample(8));
    set(8, false);
    get(sample(10));
    assert(apply().state == FeatureApplyState::Unknown);
    get(sample());
    set(7, true);
    get(sample(8));
    set(8, false);
    get(sample(9, true));
    assert(apply().state == FeatureApplyState::RollbackFailed);
    get(sample(std::numeric_limits<std::uint64_t>::max()));
    assert(apply().state == FeatureApplyState::Failed);
    assert(calls.empty());
}
void test_status() {
    protocol();
    capabilities(0);
    auto status = query_runtime_status();
    assert(std::string(status.status) == "unsupported");
    assert(runtime_status_json(status, "").find("\"runtime\":null") != std::string::npos);
    error(SUMH_IOC_GET_VERSION, ENODEV);
    status = query_runtime_status();
    assert(std::string(status.status) == "unavailable" && status.error_number == ENODEV);
    protocol(2);
    status = query_runtime_status();
    assert(std::string(status.reason) == "protocol_mismatch");
    protocol();
    error(SUMH_IOC_GET_FEATURES, EACCES);
    status = query_runtime_status();
    assert(std::string(status.status) == "error" && status.error_number == EACCES);
    protocol();
    capabilities(SUMH_FEATURE_RUNTIME_STATE);
    error(SUMH_IOC_GET_RUNTIME_STATE, ESHUTDOWN);
    status = query_runtime_status();
    assert(std::string(status.status) == "error" && status.error_number == ESHUTDOWN);
    protocol();
    capabilities(SUMH_FEATURE_RUNTIME_STATE | SUMH_FEATURE_RUNTIME_SET);
    get(sample());
    status = query_runtime_status();
    assert(std::string(status.status) == "known");
    const auto encoded = runtime_status_json(status, "boot-\"quoted");
    JsonValue json;
    std::string parse_error;
    assert(parse_json(encoded, json, parse_error));
    assert(json.o.at("runtime").o.at("instance_id").s == "17375808098319191535");
    assert(json.o.at("runtime").o.at("generation").s == "7");
    assert(json.o.at("runtime").o.at("features").a.size() == 6);
    assert(json.o.at("boot_id").s == "boot-\"quoted");
    assert(json.o.at("conditional_set_supported").b);
    assert(calls.empty());
}
}  // namespace

extern "C" int ksu_sumh_ioctl(unsigned long command, void* arg) {
    assert(!calls.empty());
    auto call = std::move(calls.front());
    calls.pop_front();
    assert(command == call.command);
    return call.run(arg);
}
int main() {
    test_client();
    test_apply();
    test_status();
    std::cout
        << "sumh_runtime_test: ABI, malformed replies, conditional apply and JSON checks passed\n";
}
