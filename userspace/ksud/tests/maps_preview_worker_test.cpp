#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include "userspace/common/diagnostic_package_request.hpp"
#include "userspace/common/module_preview_request.hpp"
#include "userspace/common/temporary_maps_request.hpp"

namespace {
std::mutex worker_mutex;
std::atomic<int> grants{0}, requests{0}, grant_error{0};
std::atomic<bool> block_grant{false}, throw_request{false};
int ksu_grant_root() {
    ++grants;
    while (block_grant)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return grant_error;
}
int geteuid() {
    return 0;
}
}  // namespace
namespace sumhp::preview_transport {
enum class Service {
    Maps,
    PartitionDiscovery,
    UserHide,
    Operation,
    ModulePreview,
    Capabilities,
    TemporaryMaps,
    DiagnosticPackages
};
std::atomic<Service> last_service{Service::Maps};
std::string saved_revision, saved_partitions;
std::string hide_arguments;
module_preview::Input module_input;
std::string failure_envelope(int error) {
    return std::to_string(error);
}
std::string request(const std::string& executable, unsigned budget, Service service) {
    assert(executable == "/ksud" && budget > 0 && budget <= 8000);
    ++requests;
    last_service = service;
    if (throw_request)
        throw std::runtime_error("fault");
    return "success";
}
std::string request_partitions_save(const std::string& executable, const std::string& revision,
                                    const std::string& partitions, unsigned budget) {
    saved_revision = revision;
    saved_partitions = partitions;
    return request(executable, budget, Service::PartitionDiscovery);
}
std::string request_user_hide(const std::string& executable, const std::string& arguments,
                              unsigned budget) {
    hide_arguments = arguments;
    return request(executable, budget, Service::UserHide);
}
std::string request_operation(const std::string& executable, const std::string& arguments,
                              unsigned budget) {
    hide_arguments = arguments;
    return request(executable, budget, Service::Operation);
}
std::string request_temporary_maps(const std::string& executable, const std::string& arguments,
                                   unsigned budget) {
    hide_arguments = arguments;
    return request(executable, budget, Service::TemporaryMaps);
}
std::string request_module_preview(const std::string& executable,
                                   const module_preview::Input& input, unsigned budget) {
    module_input = input;
    return request(executable, budget, Service::ModulePreview);
}
std::string request_diagnostic_packages(const std::string& executable, const std::string& arguments,
                                        unsigned budget) {
    hide_arguments = arguments;
    return request(executable, budget, Service::DiagnosticPackages);
}
}  // namespace sumhp::preview_transport
namespace {
#include "maps_preview_worker_under_test.inc"
void idle() {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(preview_slot_mutex);
            if (!preview_slot)
                return;
        }
        assert(std::chrono::steady_clock::now() < until);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}  // namespace

int main() {
    using Service = sumhp::preview_transport::Service;
    assert(preview_root_call("/ksud") == "success");
    idle();
    assert(grants == 1 && requests == 1);
    {
        std::lock_guard<std::mutex> lock(worker_mutex);
        assert(preview_root_call("/ksud") == std::to_string(EBUSY));
        assert(grants == 1 && requests == 1);
    }
    idle();
    grant_error = 1;
    assert(preview_root_call("/ksud") == std::to_string(EPERM));
    idle();
    assert(requests == 1);
    grant_error = 0;
    throw_request = true;
    assert(preview_root_call("/ksud") == std::to_string(EIO));
    idle();
    throw_request = false;
    const int before_requests = requests, before_grants = grants;
    block_grant = true;
    const auto begin = std::chrono::steady_clock::now();
    assert(preview_root_call("/ksud") == std::to_string(ETIMEDOUT));
    assert(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(500));
    for (int i = 0; i < 100; ++i)
        assert(preview_root_call("/ksud", i % 2 ? Service::Maps : Service::PartitionDiscovery) ==
               std::to_string(EBUSY));
    assert(grants == before_grants + 1 && requests == before_requests);
    block_grant = false;
    idle();
    // A root grant returning after the caller deadline must not launch exec.
    assert(requests == before_requests);
    assert(preview_root_call("/ksud") == "success");
    idle();
    assert(sumhp::preview_transport::last_service == Service::Maps);
    assert(preview_root_call("/ksud", Service::PartitionDiscovery) == "success");
    idle();
    assert(sumhp::preview_transport::last_service == Service::PartitionDiscovery);
    const int final_requests = requests, final_grants = grants;
    assert(preview_root_call("/ksud", static_cast<Service>(999)) == std::to_string(EINVAL));
    assert(requests == final_requests && grants == final_grants);
    const auto payload =
        std::make_shared<const PartitionsSaveInput>(PartitionsSaveInput{"revision", "[\"oem\"]"});
    assert(preview_root_call("/ksud", Service::PartitionDiscovery, payload) == "success");
    idle();
    assert(sumhp::preview_transport::saved_revision == "revision" &&
           sumhp::preview_transport::saved_partitions == "[\"oem\"]");
    const int save_requests = requests;
    block_grant = true;
    assert(preview_root_call("/ksud", Service::PartitionDiscovery, payload) ==
           std::to_string(ETIMEDOUT));
    assert(preview_root_call("/ksud") == std::to_string(EBUSY));
    block_grant = false;
    idle();
    assert(requests == save_requests);  // Expired mutation must never be submitted later.
    const auto hide = std::make_shared<const std::string>("[\"hide\",\"status\"]");
    assert(preview_root_call("/ksud", Service::UserHide, {}, hide) == "success");
    idle();
    assert(sumhp::preview_transport::hide_arguments == *hide &&
           sumhp::preview_transport::last_service == Service::UserHide);
    const int before_hide = requests, before_hide_grants = grants;
    assert(preview_root_call("/ksud", Service::UserHide) == std::to_string(EINVAL));
    assert(preview_root_call("/ksud", Service::Maps, {}, hide) == std::to_string(EINVAL));
    assert(preview_root_call("/ksud", Service::UserHide, payload, hide) == std::to_string(EINVAL));
    assert(requests == before_hide && grants == before_hide_grants);
    block_grant = true;
    assert(preview_root_call("/ksud", Service::UserHide, {}, hide) == std::to_string(ETIMEDOUT));
    assert(preview_root_call("/ksud", Service::UserHide, {}, hide) == std::to_string(EBUSY));
    block_grant = false;
    idle();
    assert(requests == before_hide);
    assert(preview_root_call("/ksud", Service::Operation, {}, hide) == "success");
    idle();
    assert(sumhp::preview_transport::hide_arguments == *hide &&
           sumhp::preview_transport::last_service == Service::Operation);
    const int operation_requests = requests;
    assert(preview_root_call("/ksud", Service::Operation) == std::to_string(EINVAL));
    assert(preview_root_call("/ksud", Service::Operation, payload, hide) == std::to_string(EINVAL));
    block_grant = true;
    assert(preview_root_call("/ksud", Service::Operation, {}, hide) == std::to_string(ETIMEDOUT));
    assert(preview_root_call("/ksud", Service::Operation, {}, hide) == std::to_string(EBUSY));
    block_grant = false;
    idle();
    assert(requests == operation_requests);
    assert(preview_root_call("/ksud", Service::Capabilities) == "success");
    idle();
    assert(sumhp::preview_transport::last_service == Service::Capabilities);
    assert(preview_root_call("/ksud", Service::Capabilities, {}, hide) == std::to_string(EINVAL));
    const auto module =
        std::make_shared<const sumhp::module_preview::Input>(sumhp::module_preview::Input{
            R"({"schema_version":1,"kind":"sumh.module.plan.input"})", "/literal $()`; "});
    assert(preview_root_call("/ksud", Service::ModulePreview, {}, {}, module) == "success");
    idle();
    assert(sumhp::preview_transport::last_service == Service::ModulePreview &&
           sumhp::preview_transport::module_input.candidate == module->candidate &&
           sumhp::preview_transport::module_input.explain_path == module->explain_path);
    const int module_requests = requests;
    assert(preview_root_call("/ksud", Service::ModulePreview) == std::to_string(EINVAL));
    assert(preview_root_call("/ksud", Service::Maps, {}, {}, module) == std::to_string(EINVAL));
    assert(preview_root_call("/ksud", Service::ModulePreview, payload, {}, module) ==
           std::to_string(EINVAL));
    assert(preview_root_call("/ksud", Service::ModulePreview, {}, hide, module) ==
           std::to_string(EINVAL));
    auto invalid =
        std::make_shared<const sumhp::module_preview::Input>(sumhp::module_preview::Input{"", ""});
    assert(preview_root_call("/ksud", Service::ModulePreview, {}, {}, invalid) ==
           std::to_string(EINVAL));
    assert(requests == module_requests);
    block_grant = true;
    assert(preview_root_call("/ksud", Service::ModulePreview, {}, {}, module) ==
           std::to_string(ETIMEDOUT));
    assert(preview_root_call("/ksud", Service::ModulePreview, {}, {}, module) ==
           std::to_string(EBUSY));
    block_grant = false;
    idle();
    assert(requests == module_requests);
    const auto temporary = std::make_shared<const std::string>(
        R"({"schema_version":1,"action":"remove","instance_id":"1","generation":"2","target_ino":"0","target_dev":"0"})");
    assert(preview_root_call("/ksud", Service::TemporaryMaps, {}, temporary) == "success");
    idle();
    assert(sumhp::preview_transport::last_service == Service::TemporaryMaps &&
           sumhp::preview_transport::hide_arguments == *temporary);
    const int temporary_requests = requests, temporary_grants = grants;
    assert(preview_root_call("/ksud", Service::TemporaryMaps) == std::to_string(EINVAL));
    assert(preview_root_call("/ksud", Service::TemporaryMaps, payload, temporary) ==
           std::to_string(EINVAL));
    assert(preview_root_call("/ksud", Service::TemporaryMaps, {}, hide) == std::to_string(EINVAL));
    assert(preview_root_call("/ksud", Service::TemporaryMaps, {}, temporary, module) ==
           std::to_string(EINVAL));
    assert(requests == temporary_requests && grants == temporary_grants);
    block_grant = true;
    assert(preview_root_call("/ksud", Service::TemporaryMaps, {}, temporary) ==
           std::to_string(ETIMEDOUT));
    assert(preview_root_call("/ksud", Service::TemporaryMaps, {}, temporary) ==
           std::to_string(EBUSY));
    block_grant = false;
    idle();
    assert(requests == temporary_requests);
    throw_request = true;
    assert(preview_root_call("/ksud", Service::TemporaryMaps, {}, temporary) ==
           std::to_string(EIO));
    idle();
    throw_request = false;
    assert(requests == temporary_requests + 1);
    const auto package =
        std::make_shared<const std::string>(R"({"schema_version":1,"action":"list"})");
    assert(preview_root_call("/ksud", Service::DiagnosticPackages, {}, package) == "success");
    idle();
    assert(sumhp::preview_transport::last_service == Service::DiagnosticPackages &&
           sumhp::preview_transport::hide_arguments == *package);
    const int package_requests = requests, package_grants = grants;
    assert(preview_root_call("/ksud", Service::DiagnosticPackages) == std::to_string(EINVAL));
    assert(preview_root_call("/ksud", Service::DiagnosticPackages, payload, package) ==
           std::to_string(EINVAL));
    assert(preview_root_call("/ksud", Service::DiagnosticPackages, {}, hide) ==
           std::to_string(EINVAL));
    assert(preview_root_call("/ksud", Service::DiagnosticPackages, {}, package, module) ==
           std::to_string(EINVAL));
    auto duplicate = std::make_shared<const std::string>(
        R"({"schema_version":1,"action":"list","action":"list"})");
    assert(preview_root_call("/ksud", Service::DiagnosticPackages, {}, duplicate) ==
           std::to_string(EINVAL));
    assert(requests == package_requests && grants == package_grants);
    block_grant = true;
    assert(preview_root_call("/ksud", Service::DiagnosticPackages, {}, package) ==
           std::to_string(ETIMEDOUT));
    assert(preview_root_call("/ksud", Service::DiagnosticPackages, {}, package) ==
           std::to_string(EBUSY));
    block_grant = false;
    idle();
    assert(requests == package_requests);
    throw_request = true;
    assert(preview_root_call("/ksud", Service::DiagnosticPackages, {}, package) ==
           std::to_string(EIO));
    idle();
    throw_request = false;
    assert(requests == package_requests + 1);
    std::cout << "Production JNI preview worker lifetime/concurrency/service checks passed, "
                 "including diagnostic packages\n";
}
