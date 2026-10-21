#include <cassert>
#include <cstddef>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include "mount/kernel_build_apply.hpp"

namespace {
using namespace sumhp::sumh;
using sumhp::mount::sumh::apply_kernel_build_config;
using sumhp::mount::sumh::FeatureApplyState;
struct Call {
    unsigned long command;
    std::function<int(void*)> run;
};
std::deque<Call> calls;
const KernelBuild old{true, "6.1.old", "#1 old"};
const KernelBuild desired{true, "6.1.new", "#2 new"};
const KernelBuild original{true, "6.1.original", "#0 original"};
const FeatureCapabilities caps{
    true, 0,
    SUMH_FEATURE_KERNEL_BUILD_SPOOF | SUMH_FEATURE_RUNTIME_STATE | SUMH_FEATURE_KERNEL_BUILD_CAS};

void error(unsigned long command, int code) {
    calls.push_back({command, [code](void*) {
                         errno = code;
                         return -1;
                     }});
}
void runtime(std::uint64_t generation, bool enabled, std::uint64_t instance = 9) {
    calls.push_back({SUMH_IOC_GET_RUNTIME_STATE, [=](void* data) {
                         sumh_runtime_state_arg request{};
                         request.size = sizeof(request);
                         request.version = 1;
                         assert(!std::memcmp(data, &request, sizeof(request)));
                         request.count = 6;
                         request.flags = 3;
                         request.generation = generation;
                         request.instance_id = instance;
                         for (unsigned int i = 0; i < 6; i++) {
                             request.entries[i].id = i + 1;
                             request.entries[i].supported = 1;
                         }
                         request.entries[5].configured = enabled;
                         request.entries[5].active = enabled;
                         *static_cast<sumh_runtime_state_arg*>(data) = request;
                         return 0;
                     }});
}
void value(KernelBuild build, bool real = false) {
    calls.push_back({real ? SUMH_IOC_GET_ORIGINAL_KERNEL_BUILD : SUMH_IOC_GET_KERNEL_BUILD,
                     [build](void* data) {
                         auto* arg = static_cast<sumh_kernel_build_arg*>(data);
                         *arg = {};
                         arg->size = sizeof(*arg);
                         arg->enable = build.enabled;
                         std::memcpy(arg->release, build.release.c_str(), build.release.size() + 1);
                         std::memcpy(arg->version, build.version.c_str(), build.version.size() + 1);
                         return 0;
                     }});
}
void snapshot(std::uint64_t generation, KernelBuild build, bool real = false,
              std::uint64_t end_generation = 0, std::uint64_t instance = 9) {
    runtime(generation, build.enabled, instance);
    value(build);
    if (real) {
        auto saved = original;
        saved.enabled = build.enabled;
        value(saved, true);
    }
    runtime(end_generation ? end_generation : generation, build.enabled, instance);
}
void write(std::uint64_t generation, KernelBuild build, int code = 0) {
    calls.push_back({SUMH_IOC_SET_KERNEL_BUILD_CAS, [=](void* data) {
                         sumh_kernel_build_cas_arg expected{};
                         expected.size = sizeof(expected);
                         expected.version = 1;
                         expected.enable = build.enabled;
                         expected.expected_generation = generation;
                         expected.expected_instance_id = 9;
                         if (build.enabled) {
                             std::memcpy(expected.release, build.release.c_str(),
                                         build.release.size() + 1);
                             std::memcpy(expected.version_string, build.version.c_str(),
                                         build.version.size() + 1);
                         }
                         assert(!std::memcmp(data, &expected, sizeof(expected)));
                         errno = code;
                         return code ? -1 : 0;
                     }});
}
void legacy_write(KernelBuild build, int code = 0) {
    calls.push_back({SUMH_IOC_GET_FEATURES, [](void* data) {
                         *static_cast<int*>(data) = SUMH_FEATURE_KERNEL_BUILD_SPOOF;
                         return 0;
                     }});
    calls.push_back({SUMH_IOC_SET_KERNEL_BUILD, [=](void* data) {
                         const auto& arg = *static_cast<sumh_kernel_build_arg*>(data);
                         assert(arg.enable == build.enabled);
                         assert(std::string(arg.release) == (build.enabled ? build.release : ""));
                         assert(std::string(arg.version) == (build.enabled ? build.version : ""));
                         errno = code;
                         return code ? -1 : 0;
                     }});
}
void test_snapshot_and_wire() {
    static_assert(sizeof(sumh_kernel_build_cas_arg) == 168);
    static_assert(offsetof(sumh_kernel_build_cas_arg, release) == 32);
    static_assert(offsetof(sumh_kernel_build_cas_arg, version_string) == 97);
    static_assert(SUMH_IOC_SET_KERNEL_BUILD_CAS == 0x40a85333UL);
    KernelBuildSnapshot output;
    snapshot(7, old, true);
    assert(kernel_build_snapshot(output, true));
    assert(output.runtime.generation == 7 && output.has_original &&
           output.current.release == old.release);
    snapshot(7, old, true, 8);
    assert(!kernel_build_snapshot(output, true) && errno == ESTALE);
    assert(output.runtime.generation == 7 && output.current.release == old.release);
    runtime(7, true);
    value(old);
    runtime(7, true, 10);
    assert(!kernel_build_snapshot(output, false) && errno == ESTALE);
    runtime(7, false);
    value(old);
    runtime(7, false);
    assert(!kernel_build_snapshot(output, false) && errno == EPROTO);
    runtime(7, true);
    value(old);
    auto wrong = original;
    wrong.enabled = false;
    value(wrong, true);
    runtime(7, true);
    assert(!kernel_build_snapshot(output, true) && errno == EPROTO);
    write(7, desired);
    assert(set_kernel_build_conditional(output.runtime, desired));
    auto invalid = desired;
    invalid.version = std::string(65, 'x');
    assert(!set_kernel_build_conditional(output.runtime, invalid) && errno == ENAMETOOLONG);
    invalid = desired;
    invalid.release.clear();
    assert(!set_kernel_build_conditional(output.runtime, invalid) && errno == EINVAL);
    KernelBuild disabled{false, "ignored\ninput", std::string(80, 'x')};
    write(7, disabled);
    assert(set_kernel_build_conditional(output.runtime, disabled));
    assert(calls.empty());
}
void test_cas_workflow() {
    snapshot(7, old);
    write(7, desired);
    snapshot(8, desired);
    auto result = apply_kernel_build_config(desired, caps);
    assert(result.state == FeatureApplyState::Applied && result.previous_state_known &&
           result.previous_enabled);
    // Both implicit fields are resolved inside the bracketed snapshot.
    for (bool missing_release : {false, true}) {
        auto request = desired, resolved = desired;
        if (missing_release) {
            request.release.clear();
            resolved.release = original.release;
        } else {
            request.version.clear();
            resolved.version = original.version;
        }
        snapshot(7, old, true);
        write(7, resolved);
        snapshot(8, resolved);
        assert(apply_kernel_build_config(request, caps).state == FeatureApplyState::Applied);
    }
    KernelBuild disabled{false, original.release, original.version};
    snapshot(7, old, true);
    write(7, disabled);
    snapshot(8, disabled);
    assert(apply_kernel_build_config({false, "", ""}, caps).state == FeatureApplyState::Applied);
    // A writer inside a snapshot or immediately before CAS stops this attempt.
    snapshot(7, old, false, 8);
    assert(apply_kernel_build_config(desired, caps).state == FeatureApplyState::Failed);
    snapshot(7, old);
    write(7, desired, ESTALE);
    assert(apply_kernel_build_config(desired, caps).state == FeatureApplyState::Failed);
    error(SUMH_IOC_GET_RUNTIME_STATE, EIO);
    assert(apply_kernel_build_config(desired, caps).state == FeatureApplyState::Failed);
    // No ownership is inferred from failed transport, even if values changed.
    snapshot(7, old);
    write(7, desired, EIO);
    snapshot(8, desired);
    assert(apply_kernel_build_config(desired, caps).state == FeatureApplyState::Unknown);
    snapshot(7, old);
    write(7, desired, EIO);
    snapshot(8, old);
    assert(apply_kernel_build_config(desired, caps).state == FeatureApplyState::Failed);
    // Another writer after SET or during readback prevents rollback.
    snapshot(7, old);
    write(7, desired);
    snapshot(9, desired);
    assert(apply_kernel_build_config(desired, caps).state == FeatureApplyState::Unknown);
    snapshot(7, old);
    write(7, desired);
    snapshot(8, desired, false, 9);
    assert(apply_kernel_build_config(desired, caps).state == FeatureApplyState::Unknown);
    snapshot(7, old);
    write(7, desired);
    error(SUMH_IOC_GET_RUNTIME_STATE, EIO);
    assert(apply_kernel_build_config(desired, caps).state == FeatureApplyState::Unknown);
    // A verified mismatch at the acknowledged own token permits exact CAS rollback.
    auto wrong = desired;
    wrong.version = "#unexpected";
    snapshot(7, old);
    write(7, desired);
    snapshot(8, wrong);
    write(8, old);
    snapshot(9, old);
    assert(apply_kernel_build_config(desired, caps).state == FeatureApplyState::RolledBack);
    snapshot(7, old);
    write(7, desired);
    snapshot(8, wrong);
    write(8, old, ESTALE);
    assert(apply_kernel_build_config(desired, caps).state == FeatureApplyState::Unknown);
    snapshot(7, old);
    write(7, desired);
    snapshot(8, wrong);
    write(8, old, EIO);
    assert(apply_kernel_build_config(desired, caps).state == FeatureApplyState::RollbackFailed);
    snapshot(7, old);
    write(7, desired);
    snapshot(8, wrong);
    write(8, old);
    snapshot(10, old);
    assert(apply_kernel_build_config(desired, caps).state == FeatureApplyState::Unknown);
    snapshot(7, old);
    write(7, desired);
    snapshot(8, wrong);
    write(8, old);
    snapshot(9, wrong);
    assert(apply_kernel_build_config(desired, caps).state == FeatureApplyState::RollbackFailed);
    snapshot(std::numeric_limits<std::uint64_t>::max(), old);
    assert(apply_kernel_build_config(desired, caps).state == FeatureApplyState::Failed);
    assert(calls.empty());
}
void test_legacy_without_rollback() {
    FeatureCapabilities legacy{true, 0, SUMH_FEATURE_KERNEL_BUILD_SPOOF};
    value(old);
    legacy_write(desired);
    value(desired);
    assert(apply_kernel_build_config(desired, legacy).state == FeatureApplyState::Applied);
    value(old);
    legacy_write(desired, EIO);
    value(desired);
    assert(apply_kernel_build_config(desired, legacy).state == FeatureApplyState::Unknown);
    value(old);
    legacy_write(desired, EIO);
    value(old);
    assert(apply_kernel_build_config(desired, legacy).state == FeatureApplyState::Failed);
    value(old);
    legacy_write(desired);
    value(old);
    assert(apply_kernel_build_config(desired, legacy).state == FeatureApplyState::Unknown);
    value(old);
    legacy_write(desired);
    error(SUMH_IOC_GET_KERNEL_BUILD, EIO);
    assert(apply_kernel_build_config(desired, legacy).state == FeatureApplyState::Unknown);
    assert(apply_kernel_build_config(desired, {false, EACCES, 0}).state ==
           FeatureApplyState::Failed);
    assert(apply_kernel_build_config(desired, {true, 0, 0}).state ==
           FeatureApplyState::Unsupported);
    assert(apply_kernel_build_config({false, "", ""}, {true, 0, 0}).state ==
           FeatureApplyState::Deferred);
    assert(apply_kernel_build_config({true, "", ""}, caps).state == FeatureApplyState::Failed);
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
    test_snapshot_and_wire();
    test_cas_workflow();
    test_legacy_without_rollback();
    std::cout << "sumh_kernel_build_apply_test: coherent snapshots, conditional strings, "
                 "concurrency and legacy no-rollback passed\n";
}
