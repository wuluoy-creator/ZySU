#include "core/capability_snapshot.hpp"
#include "core/runtime.hpp"
#include "core/runtime_status.hpp"
#include "core/state_ledger.hpp"
#include "mount/query_worker.hpp"
#include "sumhp/config.hpp"
#include "userspace/common/maps_preview_transport.hpp"
#include "userspace/common/utf8.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>

namespace sumhp {
namespace {
constexpr std::size_t config_limit = 256UL * 1024UL;
constexpr std::size_t filesystems_limit = 64UL * 1024UL;
class Fd {
    int value;

public:
    explicit Fd(int descriptor) : value(descriptor) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&&) = delete;
    Fd& operator=(Fd&&) = delete;
    [[nodiscard]] int get() const { return value; }
    ~Fd() {
        if (value >= 0)
            ::close(value);
    }
};
struct Document {
    std::string status, bytes;
    int error = 0;
};
bool operator==(const Document& left, const Document& right) {
    return left.status == right.status && left.bytes == right.bytes && left.error == right.error;
}
std::string tick() {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
}
Document read_document(const std::filesystem::path& path, std::size_t limit, bool proc = false) {
    const Fd fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (fd.get() < 0)
        return {errno == ENOENT ? "missing" : "unreadable", {}, errno ? errno : EIO};
    struct stat metadata{};
    if (::fstat(fd.get(), &metadata) != 0)
        return {"unreadable", {}, errno ? errno : EIO};
    if (!S_ISREG(metadata.st_mode) || (!proc && metadata.st_nlink != 1) || metadata.st_size < 0)
        return {"unreadable", {}, EINVAL};
    if (static_cast<std::uint64_t>(metadata.st_size) > limit)
        return {"unreadable", {}, EOVERFLOW};
    Document out{"valid", {}, 0};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    char buffer[4096];
    while (out.bytes.size() <= limit) {
        if (std::chrono::steady_clock::now() >= deadline)
            return {"unreadable", {}, ETIMEDOUT};
        const auto count =
            ::read(fd.get(), buffer, std::min(sizeof(buffer), limit + 1 - out.bytes.size()));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return {"unreadable", {}, errno ? errno : EIO};
        if (!count)
            break;
        out.bytes.append(buffer, static_cast<std::size_t>(count));
    }
    return out.bytes.size() <= limit ? out : Document{"unreadable", {}, EOVERFLOW};
}
std::string boot_identity() {
    auto value = read_document("/proc/sys/kernel/random/boot_id", 80, true);
    if (value.status != "valid")
        return {};
    if (!value.bytes.empty() && value.bytes.back() == '\n')
        value.bytes.pop_back();
    if (value.bytes.size() != 36)
        return {};
    for (std::size_t i = 0; i < value.bytes.size(); ++i) {
        const unsigned char c = value.bytes[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return {};
        } else if ((c < '0' || c > '9') && (c < 'a' || c > 'f'))
            return {};
    }
    return value.bytes;
}
JsonValue nullable(const std::string& value) {
    return value.empty() ? JsonValue{} : JsonValue(value);
}
JsonValue section(const char* source) {
    auto out = JsonValue::object();
    out.o["status"] = "unknown";
    out.o["reason"] = "not_sampled";
    out.o["errno"] = 0;
    out.o["source"] = source;
    out.o["sample_started_monotonic_ms"] = tick();
    out.o["sample_finished_monotonic_ms"] = out.o.at("sample_started_monotonic_ms");
    out.o["value"] = JsonValue{};
    return out;
}
void finish(JsonValue& value, const char* status, const std::string& reason, int error = 0) {
    value.o["status"] = status;
    value.o["reason"] = reason;
    value.o["errno"] = error;
    value.o["sample_finished_monotonic_ms"] = tick();
}
JsonValue base() {
    auto out = JsonValue::object();
    out.o["schema_version"] = 1;
    out.o["resource"] = "capability_snapshot";
    out.o["observation"] = "control_plane_only";
    out.o["atomic"] = false;
    out.o["boot_id"] = JsonValue{};
    out.o["sample_started_monotonic_ms"] = tick();
    out.o["sample_finished_monotonic_ms"] = out.o.at("sample_started_monotonic_ms");
    auto sections = JsonValue::object();
    sections.o["saved"] = section("saved_config");
    sections.o["runtime"] = section("kernel_ioctl");
    sections.o["build"] = section("initial_uts_ioctl");
    sections.o["backends"] = section("capability_probes");
    sections.o["owner"] = section("not_collected");
    out.o["sections"] = std::move(sections);
    return out;
}
JsonValue failure(const std::string& reason, int error) {
    auto out = base();
    for (auto& [name, value] : out.o.at("sections").o)
        finish(value, "unknown", reason, error);
    out.o["sample_finished_monotonic_ms"] = tick();
    return out;
}
JsonValue saved_controls(const Document& document) {
    auto out = section("saved_config");
    Config config;
    std::string error;
    if (document.status != "valid") {
        finish(out, document.status == "missing" ? "unknown" : "error",
               "saved_config_" + document.status, document.error);
        return out;
    }
    if (!ksud::text::valid_utf8(document.bytes) ||
        !parse_config_json(document.bytes, config, error)) {
        finish(out, "error", "saved_config_invalid_or_unsupported", EINVAL);
        return out;
    }
    const auto revision = state::revision_for_bytes(document.bytes);
    if (revision.empty()) {
        finish(out, "error", "config_revision_unavailable", EIO);
        return out;
    }
    auto value = JsonValue::object();
    value.o["revision"] = revision;
    value.o["revision_scope"] = "saved_config_bytes_only";
    value.o["enable_kernel_debug"] = config.enable_kernel_debug;
    value.o["enable_stealth"] = config.enable_stealth;
    value.o["enable_mount_hide"] = config.enable_mount_hide;
    value.o["mount_hide_mode"] = config.mount_hide_mode;
    value.o["enable_maps_spoof"] = config.enable_maps_spoof;
    value.o["enable_statfs_spoof"] = config.enable_statfs_spoof;
    value.o["enable_kernel_build_spoof"] = config.enable_kernel_build_spoof;
    value.o["enable_overlay_xattr_hide"] = config.enable_overlay_xattr_hide;
    value.o["kernel_build_release"] = config.kernel_build_release;
    value.o["kernel_build_version"] = config.kernel_build_version;
    value.o["kernel_build_apply_stage"] = config.kernel_build_apply_stage;
    value.o["mount_backend"] = config.mount_backend;
    value.o["overlayfs_enabled"] = config.overlayfs_enabled;
    value.o["magic_mount_enabled"] = config.magic_mount_enabled;
    out.o["value"] = std::move(value);
    finish(out, "known", "saved_config_sample");
    return out;
}
JsonValue build_sample(const RuntimeStatus& runtime) {
    auto out = section("initial_uts_ioctl");
    if (!runtime.capabilities.ok) {
        finish(out, "unknown", "capabilities_unavailable", runtime.error_number);
        return out;
    }
    if (!(runtime.capabilities.bitmask & SUMH_FEATURE_KERNEL_BUILD_SPOOF)) {
        finish(out, "unsupported", "kernel_build_not_advertised");
        return out;
    }
    const bool bracketed = runtime.capabilities.bitmask & SUMH_FEATURE_RUNTIME_STATE;
    sumh::KernelBuildSnapshot sample;
    errno = 0;
    const bool ok = bracketed ? sumh::kernel_build_snapshot(sample, true)
                              : (sumh::kernel_build(sample.current) &&
                                 sumh::original_kernel_build(sample.original));
    if (!ok) {
        finish(out, "error", "kernel_build_query_failed", errno ? errno : EIO);
        return out;
    }
    for (const auto* text : {&sample.current.release, &sample.current.version,
                             &sample.original.release, &sample.original.version}) {
        if (!ksud::text::valid_utf8(*text)) {
            finish(out, "error", "kernel_build_encoding_invalid", EILSEQ);
            return out;
        }
    }
    auto value = JsonValue::object();
    value.o["enabled"] = sample.current.enabled;
    value.o["release"] = sample.current.release;
    value.o["version"] = sample.current.version;
    value.o["original_release"] = sample.original.release;
    value.o["original_version"] = sample.original.version;
    value.o["consistency"] = bracketed ? "control_generation_bracketed" : "independently_sampled";
    value.o["instance_id"] =
        bracketed ? JsonValue(std::to_string(sample.runtime.instance_id)) : JsonValue{};
    value.o["generation"] =
        bracketed ? JsonValue(std::to_string(sample.runtime.generation)) : JsonValue{};
    out.o["value"] = std::move(value);
    finish(out, "known", "initial_uts_sample_not_abi_probe");
    return out;
}
JsonValue backend_sample(const RuntimeStatus& runtime) {
    auto out = section("capability_probes");
    auto value = JsonValue::object();
    value.o["scope"] = "capability_only";
    value.o["live_backend"] = "unknown";
    value.o["live_backend_reason"] = "runtime_backend_ownership_not_collected";
    value.o["items"] = JsonValue::array();
    const auto append = [&](const char* id, const char* status, const char* source,
                            const char* reason) {
        auto item = JsonValue::object();
        item.o["id"] = id;
        item.o["status"] = status;
        item.o["source"] = source;
        item.o["reason"] = reason;
        value.o["items"].a.push_back(std::move(item));
    };
    if (runtime.version.status == sumh::Status::Available)
        append("sumh", "available", "kernel_protocol", "protocol_compatible_not_mapping_proof");
    else if (runtime.version.status == sumh::Status::NotPresent)
        append("sumh", "unknown", "kernel_protocol", "transport_unavailable");
    else
        append("sumh", "unavailable", "kernel_protocol", "protocol_mismatch");
    const auto filesystems = read_document("/proc/filesystems", filesystems_limit, true);
    if (filesystems.status != "valid")
        append("overlay", "unknown", "proc_filesystems", "filesystem_query_failed");
    else {
        bool registered = false;
        std::istringstream lines(filesystems.bytes);
        std::string line;
        while (std::getline(lines, line)) {
            std::istringstream words(line);
            std::string first;
            std::string second;
            words >> first >> second;
            if (first == "overlay" || second == "overlay")
                registered = true;
        }
        append("overlay", registered ? "available" : "unavailable", "proc_filesystems",
               registered ? "filesystem_registered_not_mount_test" : "filesystem_not_registered");
    }
    append("magic", "available", "controller_strategy", "compiled_strategy_not_mount_test");
    out.o["value"] = std::move(value);
    finish(out, "known", "capability_report_not_runtime_ownership");
    return out;
}
JsonValue observe() {
    auto out = base();
    const auto boot_before = boot_identity();
    auto& sections = out.o.at("sections");
    const auto saved_started = tick();
    const auto before = read_document(runtime_config_file(), config_limit);
    sections.o["saved"] = saved_controls(before);
    sections.o["saved"].o["sample_started_monotonic_ms"] = saved_started;
    const auto runtime = query_runtime_status();
    auto& state = sections.o["runtime"];
    state.o["sample_started_monotonic_ms"] = runtime.started_ms;
    std::string error;
    if (!parse_json(runtime_status_json(runtime, boot_before), state.o["value"], error)) {
        state.o["value"] = JsonValue{};
        finish(state, "error", "runtime_encoding_failed", EIO);
    } else {
        finish(state, std::string(runtime.status) == "unavailable" ? "unknown" : runtime.status,
               runtime.reason, runtime.error_number);
    }
    sections.o["build"] = build_sample(runtime);
    sections.o["backends"] = backend_sample(runtime);
    finish(sections.o["owner"], "unknown", "configured_owner_is_not_runtime_ownership_proof");
    const auto after = read_document(runtime_config_file(), config_limit);
    if (!(before == after)) {
        sections.o["saved"].o["value"] = JsonValue{};
        finish(sections.o["saved"], "unknown", "saved_config_changed_during_sample", ESTALE);
    } else
        sections.o["saved"].o["sample_finished_monotonic_ms"] = tick();
    const auto boot_after = boot_identity();
    if (boot_before.empty() || boot_after.empty() || boot_before != boot_after) {
        for (auto& [name, value] : sections.o) {
            value.o["value"] = JsonValue{};
            finish(value, "unknown",
                   boot_before.empty() || boot_after.empty() ? "boot_identity_unavailable"
                                                             : "boot_changed_during_sample",
                   ESTALE);
        }
    } else
        out.o["boot_id"] = nullable(boot_before);
    out.o["sample_finished_monotonic_ms"] = tick();
    return out;
}
struct SharedReply {
    std::uint32_t done, size;
    char bytes[CAPABILITY_SNAPSHOT_RESPONSE_BYTES];
};
}  // namespace

std::string capability_snapshot_json() {
    const Fd guard{preview_transport::acquire_preview_guard()};
    if (guard.get() < 0)
        return stringify_json(failure("capability_query_guard_unavailable", errno ? errno : EBUSY));
    auto* shared = static_cast<SharedReply*>(::mmap(
        nullptr, sizeof(SharedReply), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (shared == MAP_FAILED)
        return stringify_json(failure("reply_memory_unavailable", errno ? errno : ENOMEM));
    shared->done = 0;
    shared->size = 0;
    const bool ok = mount::fsutil::run_query_in_init_mount_ns([&] {
        struct stat own_ns{};
        struct stat init_ns{};
        if (::stat("/proc/self/ns/mnt", &own_ns) != 0 || ::stat("/proc/1/ns/mnt", &init_ns) != 0 ||
            own_ns.st_ino != init_ns.st_ino || own_ns.st_dev != init_ns.st_dev) {
            errno = ESTALE;
            return false;
        }
        auto bytes = stringify_json(observe());
        if (bytes.size() > CAPABILITY_SNAPSHOT_RESPONSE_BYTES)
            bytes = stringify_json(failure("capability_snapshot_response_capacity", EOVERFLOW));
        std::memcpy(shared->bytes, bytes.data(), bytes.size());
        shared->size = static_cast<std::uint32_t>(bytes.size());
        shared->done = 1;
        return true;
    });
    const int error = errno;
    const auto result =
        ok && shared->done == 1 && shared->size <= CAPABILITY_SNAPSHOT_RESPONSE_BYTES
            ? std::string(shared->bytes, shared->size)
            : stringify_json(failure("capability_query_failed", error ? error : EIO));
    ::munmap(shared, sizeof(SharedReply));
    return result;
}
}  // namespace sumhp
