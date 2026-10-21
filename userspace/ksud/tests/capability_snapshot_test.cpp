#include "core/capability_snapshot.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>
#include "core/config_document.hpp"
#include "core/rule_inventory.hpp"
#include "core/runtime_status.hpp"
#include "sumhp/config.hpp"
#include "userspace/common/utf8.hpp"

constexpr int O_RDONLY = 1, O_CLOEXEC = 2, O_NOFOLLOW = 4, O_NONBLOCK = 8;
constexpr int PROT_READ = 1, PROT_WRITE = 2, MAP_SHARED = 1, MAP_ANONYMOUS = 2;
#define MAP_FAILED reinterpret_cast<void*>(-1)
namespace capability_model {
struct Metadata {
    unsigned st_mode = 0100600, st_nlink = 1;
    std::int64_t st_size = 0;
    std::uint64_t st_ino = 3, st_dev = 8;
};
struct File {
    std::string bytes;
    int error = 0, stat_error = 0, read_error = 0;
    Metadata metadata;
    bool size_override = false;
    File(std::string value = {}, int failure = 0) : bytes(std::move(value)), error(failure) {}
};
struct Handle {
    File file;
    std::size_t offset = 0;
};
std::map<std::string, std::vector<File>> documents;
std::map<std::string, std::size_t> indexes;
std::map<int, Handle> handles;
std::vector<std::string> boots;
int next_fd = 10, boot_reads = 0, runtime_reads = 0, build_reads = 0, queries = 0, mapped = 0,
    unmapped = 0, checks = 0;
int guard_error = 0, mmap_error = 0, namespace_error = 0, eintrs = 0, boot_error = 0;
bool namespace_changed = false, query_fail = false, query_fail_after = false, hash_failure = false,
     runtime_ok = true, build_ok = true;
sumhp::sumh::VersionInfo version;
sumhp::sumh::FeatureCapabilities capabilities;
sumhp::sumh::RuntimeState runtime;
sumhp::sumh::KernelBuild current, original;
bool is_regular(unsigned mode) {
    return (mode & 0170000) == 0100000;
}
int open(const std::filesystem::path::value_type* raw, int flags) {
    const auto path = std::filesystem::path(raw).generic_string();
    assert(flags == (O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (path == "/proc/sys/kernel/random/boot_id") {
        if (boot_error) {
            errno = boot_error;
            return -1;
        }
        const int fd = next_fd++;
        handles.emplace(fd, Handle{File{boots.at(boot_reads++) + "\n"}, 0});
        return fd;
    }
    auto found = documents.find(path);
    if (found == documents.end()) {
        errno = ENOENT;
        return -1;
    }
    auto& index = indexes[path];
    const auto& values = found->second;
    const auto file = values[std::min(index++, values.size() - 1)];
    if (file.error) {
        errno = file.error;
        return -1;
    }
    const int fd = next_fd++;
    handles.emplace(fd, Handle{file, 0});
    return fd;
}
int fstat(int fd, Metadata* out) {
    const auto& file = handles.at(fd).file;
    if (file.stat_error) {
        errno = file.stat_error;
        return -1;
    }
    *out = file.metadata;
    if (!file.size_override)
        out->st_size = file.bytes.size();
    return 0;
}
int read(int fd, void* buffer, std::size_t size) {
    if (eintrs) {
        --eintrs;
        errno = EINTR;
        return -1;
    }
    auto& handle = handles.at(fd);
    if (handle.file.read_error) {
        errno = handle.file.read_error;
        return -1;
    }
    const auto count = std::min(size, handle.file.bytes.size() - handle.offset);
    std::memcpy(buffer, handle.file.bytes.data() + handle.offset, count);
    handle.offset += count;
    return static_cast<int>(count);
}
int close(int fd) {
    if (fd != 1)
        assert(handles.erase(fd));
    return 0;
}
int stat(const char* path, Metadata* out) {
    if (namespace_error) {
        errno = namespace_error;
        return -1;
    }
    *out = Metadata{};
    if (namespace_changed && std::string(path) == "/proc/1/ns/mnt")
        ++out->st_ino;
    return 0;
}
void* mmap(void*, std::size_t size, int protection, int flags, int fd, int offset) {
    assert(protection == (PROT_READ | PROT_WRITE) && flags == (MAP_SHARED | MAP_ANONYMOUS) &&
           fd == -1 && offset == 0);
    if (mmap_error) {
        errno = mmap_error;
        return MAP_FAILED;
    }
    ++mapped;
    return ::operator new(size);
}
int munmap(void* value, std::size_t) {
    ++unmapped;
    ::operator delete(value);
    return 0;
}
void reset() {
    assert(handles.empty());
    documents.clear();
    indexes.clear();
    next_fd = 10;
    boot_reads = runtime_reads = build_reads = queries = mapped = unmapped = 0;
    guard_error = mmap_error = namespace_error = eintrs = boot_error = 0;
    namespace_changed = query_fail = query_fail_after = hash_failure = false;
    runtime_ok = build_ok = true;
    boots = {"00000000-0000-0000-0000-000000000001", "00000000-0000-0000-0000-000000000001"};
    version = {};
    version.status = sumhp::sumh::Status::Available;
    version.expected_protocol = version.kernel_protocol = 1;
    capabilities = {true, 0, SUMH_FEATURE_RUNTIME_STATE | SUMH_FEATURE_KERNEL_BUILD_SPOOF};
    runtime = {};
    runtime.generation = 7;
    runtime.instance_id = 9;
    for (unsigned i = 1; i <= 6; ++i)
        runtime.entries.push_back(
            {static_cast<sumhp::sumh::RuntimeFeature>(i), true, false, false, false, 0});
    current = {false, "6.1.99-android14-example", "#1 SMP example"};
    original = current;
    documents["/private/config.json"] = {File{"{}"}};
    documents["/proc/filesystems"] = {File{"nodev\toverlay\nnodev\tproc\n"}};
}
}  // namespace capability_model
namespace sumhp {
std::filesystem::path runtime_config_file() {
    return "/private/config.json";
}
namespace state {
std::string revision_for_bytes(const std::string&) {
    return capability_model::hash_failure ? "" : "sha256-v1:" + std::string(64, 'a');
}
}  // namespace state
namespace checked {
constexpr std::size_t document_limit = 4 * 1024 * 1024;
}
#include "capability_config_under_test.inc"
namespace sumh {
VersionInfo version_info() {
    return capability_model::version;
}
FeatureCapabilities feature_capabilities() {
    return capability_model::capabilities;
}
bool runtime_state(RuntimeState& out) {
    ++capability_model::runtime_reads;
    if (!capability_model::runtime_ok) {
        errno = EIO;
        return false;
    }
    out = capability_model::runtime;
    return true;
}
bool kernel_build(KernelBuild& out) {
    ++capability_model::build_reads;
    if (!capability_model::build_ok) {
        errno = EIO;
        return false;
    }
    out = capability_model::current;
    return true;
}
bool original_kernel_build(KernelBuild& out) {
    ++capability_model::build_reads;
    if (!capability_model::build_ok) {
        errno = EIO;
        return false;
    }
    out = capability_model::original;
    return true;
}
bool kernel_build_snapshot(KernelBuildSnapshot& out, bool include_original) {
    assert(include_original);
    if (!kernel_build(out.current) || !original_kernel_build(out.original))
        return false;
    out.runtime = capability_model::runtime;
    out.has_original = true;
    return true;
}
}  // namespace sumh
namespace preview_transport {
int acquire_preview_guard() {
    if (capability_model::guard_error) {
        errno = capability_model::guard_error;
        return -1;
    }
    return 1;
}
}  // namespace preview_transport
namespace mount::fsutil {
bool run_query_in_init_mount_ns(const std::function<bool()>& callback) {
    ++capability_model::queries;
    if (capability_model::query_fail) {
        errno = ETIMEDOUT;
        return false;
    }
    const bool ok = callback();
    if (capability_model::query_fail_after) {
        errno = ECHILD;
        return false;
    }
    return ok;
}
}  // namespace mount::fsutil
}  // namespace sumhp
#include "capability_snapshot_under_test.inc"

std::string last_sample_bytes;
sumhp::JsonValue sample() {
    ++capability_model::checks;
    const auto bytes = sumhp::capability_snapshot_json();
    last_sample_bytes = bytes;
    assert(bytes.size() <= sumhp::CAPABILITY_SNAPSHOT_RESPONSE_BYTES);
    auto out = json::parse(bytes);
    assert(out.at("schema_version").n == 1 && out.at("resource").s == "capability_snapshot");
    assert(!out.at("atomic").b && out.at("observation").s == "control_plane_only");
    assert(out.at("sections").o.size() == 5 && capability_model::handles.empty() &&
           capability_model::mapped == capability_model::unmapped);
    return out;
}
int main(int argc, char** argv) {
    using namespace capability_model;
    reset();
    auto good = sample();
    const auto& sections = good.at("sections");
    assert(sections.at("saved").at("status").s == "known" &&
           sections.at("saved").at("value").at("mount_hide_mode").s == "normal");
    assert(sections.at("runtime").at("value").at("runtime").at("features").a.size() == 6);
    assert(sections.at("build").at("value").at("consistency").s == "control_generation_bracketed");
    assert(sections.at("backends").at("value").at("live_backend").s == "unknown" &&
           sections.at("owner").at("value").is_null());
    if (argc == 2) {
        std::ofstream fixture(argv[1], std::ios::binary);
        fixture << last_sample_bytes;
        assert(fixture.good());
    }
    reset();
    documents.erase("/private/config.json");
    auto out = sample();
    assert(out.at("sections").at("saved").at("status").s == "unknown");
    for (const auto& invalid :
         std::vector<std::string>{"[", "[]", R"({"schema_version":99})", R"({"enable_stealth":2})",
                                  R"({"mount_hide_mode":"wrong"})", std::string("\xff", 1),
                                  std::string(256 * 1024 + 1, ' ')}) {
        reset();
        documents["/private/config.json"] = {File{invalid}};
        out = sample();
        assert(out.at("sections").at("saved").at("status").s == "error");
        assert(out.at("sections").at("runtime").at("status").s == "known");
    }
    for (int mode = 0; mode < 5; ++mode) {
        reset();
        auto& file = documents["/private/config.json"][0];
        if (mode == 0)
            file.error = EACCES;
        if (mode == 1)
            file.stat_error = EIO;
        if (mode == 2)
            file.read_error = EIO;
        if (mode == 3)
            file.metadata.st_nlink = 2;
        if (mode == 4)
            file.metadata.st_mode = 0120600;
        out = sample();
        assert(out.at("sections").at("saved").at("status").s == "error");
    }
    reset();
    eintrs = 2;
    assert(sample().at("sections").at("saved").at("status").s == "known");
    reset();
    documents["/private/config.json"] = {File{"{}"}, File{R"({"enable_stealth":false})"}};
    out = sample();
    assert(out.at("sections").at("saved").at("reason").s == "saved_config_changed_during_sample" &&
           out.at("sections").at("saved").at("value").is_null());
    reset();
    documents["/private/config.json"] = {File{
        R"({"mount_hide_mode":"aggressive","kernel_build_version":"中文","future_field":true})"}};
    out = sample();
    assert(out.at("sections").at("saved").at("value").at("mount_hide_mode").s == "aggressive");
    reset();
    hash_failure = true;
    assert(sample().at("sections").at("saved").at("reason").s == "config_revision_unavailable");
    reset();
    capabilities.ok = false;
    capabilities.last_errno = EACCES;
    out = sample();
    assert(out.at("sections").at("runtime").at("status").s == "error" && !build_reads);
    reset();
    capabilities.bitmask = SUMH_FEATURE_KERNEL_BUILD_SPOOF;
    out = sample();
    assert(out.at("sections").at("runtime").at("status").s == "unsupported");
    assert(out.at("sections").at("runtime").at("value").at("capabilities").is_number());
    assert(out.at("sections").at("build").at("value").at("consistency").s ==
           "independently_sampled");
    reset();
    capabilities.bitmask = SUMH_FEATURE_RUNTIME_STATE;
    out = sample();
    assert(out.at("sections").at("build").at("status").s == "unsupported" && !build_reads);
    reset();
    runtime_ok = false;
    out = sample();
    assert(out.at("sections").at("runtime").at("value").at("runtime").is_null());
    reset();
    build_ok = false;
    out = sample();
    assert(out.at("sections").at("build").at("status").s == "error" &&
           out.at("sections").at("runtime").at("status").s == "known");
    reset();
    current.version = std::string("\xff", 1);
    out = sample();
    assert(out.at("sections").at("build").at("reason").s == "kernel_build_encoding_invalid");
    reset();
    current.release = "6.1 spaced release";
    out = sample();
    assert(out.at("sections").at("build").at("value").at("release").s == current.release);
    reset();
    runtime.entries[2].configured = true;
    runtime.entries[2].degraded = true;
    runtime.entries[2].last_error = -5;
    out = sample();
    const auto& entry =
        out.at("sections").at("runtime").at("value").at("runtime").at("features").a[2];
    assert(entry.at("configured").b && !entry.at("active").b && entry.at("last_set_errno").n == -5);
    for (auto status : {sumhp::sumh::Status::NotPresent, sumhp::sumh::Status::KernelTooOld,
                        sumhp::sumh::Status::ClientTooOld}) {
        reset();
        version.status = status;
        version.last_errno = ENODEV;
        out = sample();
        assert(out.at("sections").at("runtime").at("status").s == "unknown");
        assert(out.at("sections").at("backends").at("value").at("items").a[0].at("status").s ==
               (status == sumhp::sumh::Status::NotPresent ? "unknown" : "unavailable"));
    }
    reset();
    documents.erase("/proc/filesystems");
    out = sample();
    assert(out.at("sections").at("backends").at("value").at("items").a[1].at("status").s ==
           "unknown");
    reset();
    documents["/proc/filesystems"] = {File{"nodev\tproc\n"}};
    out = sample();
    assert(out.at("sections").at("backends").at("value").at("items").a[1].at("status").s ==
           "unavailable");
    for (bool missing : {false, true}) {
        reset();
        boots[1] = missing ? "" : "00000000-0000-0000-0000-000000000002";
        out = sample();
        assert(out.at("boot_id").is_null());
        for (const auto& [name, value] : out.at("sections").o)
            assert(value.at("status").s == "unknown" && value.at("value").is_null());
    }
    for (const auto& invalid : std::vector<std::string>{
             "invalid", std::string(81, 'a'), "00000000-0000-0000-0000-00000000000z",
             "00000000-0000-0000-0000-000000000001\nextra"}) {
        reset();
        boots[0] = invalid;
        out = sample();
        assert(out.at("boot_id").is_null() &&
               out.at("sections").at("saved").at("reason").s == "boot_identity_unavailable");
    }
    reset();
    boot_error = EACCES;
    out = sample();
    assert(out.at("boot_id").is_null() && out.at("sections").at("runtime").at("value").is_null());
    if (argc == 2) {
        std::ofstream fixture(std::string(argv[1]) + ".boot-missing.json", std::ios::binary);
        fixture << last_sample_bytes;
        assert(fixture.good());
    }
    for (int mode = 0; mode < 6; ++mode) {
        reset();
        if (mode == 0)
            guard_error = EBUSY;
        if (mode == 1)
            mmap_error = ENOMEM;
        if (mode == 2)
            query_fail = true;
        if (mode == 3)
            query_fail_after = true;
        if (mode == 4)
            namespace_changed = true;
        if (mode == 5)
            namespace_error = EIO;
        out = sample();
        assert(out.at("boot_id").is_null());
        for (const auto& [name, value] : out.at("sections").o)
            assert(value.at("status").s == "unknown" && value.at("value").is_null());
    }
    if (argc == 2) {
        std::ofstream fixture(std::string(argv[1]) + ".failure.json", std::ios::binary);
        fixture << last_sample_bytes;
        assert(fixture.good());
    }
    std::cout << "Production capability snapshot: " << checks
              << " boundary/serializer cases passed; no device evidence\n";
}
