// Execute production backend selection over real module trees. No mounts or
// root privileges are needed; directory symlink cases also run when permitted.
#include "mount/backend.hpp"
#include "mount/partition_alias.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
bool overlay_available = true;
bool sumh_available = true;
int sumh_error = 0;
int overlay_error = 0;
int log_calls = 0;
std::string metadata_fault;
fs::path metadata_fault_path;
}  // namespace

// Only filesystem boundaries are substituted. The production decision body
// remains unchanged, allowing errors unavailable on a privileged host to be
// checked without claiming real Android/filesystem fault coverage.
namespace backend_fs {
using path = std::filesystem::path;
using file_status = std::filesystem::file_status;
using file_type = std::filesystem::file_type;
using std::filesystem::is_symlink;
bool is_directory(const file_status& value) {
    return std::filesystem::is_directory(value);
}
bool fail(const std::string& phase, const path& value, std::error_code& ec) {
    if (metadata_fault == phase && value == metadata_fault_path) {
        ec = std::make_error_code(std::errc::permission_denied);
        return true;
    }
    return false;
}
bool is_directory(const path& value, std::error_code& ec) {
    if (fail("content", value, ec))
        return false;
    return std::filesystem::is_directory(value, ec);
}
file_status symlink_status(const path& value, std::error_code& ec) {
    if (fail("layout", value, ec))
        return {};
    return std::filesystem::symlink_status(value, ec);
}
path read_symlink(const path& value, std::error_code& ec) {
    return std::filesystem::read_symlink(value, ec);
}
struct directory_entry {
    std::filesystem::directory_entry inner;
    const backend_fs::path& path() const { return inner.path(); }
    file_status symlink_status(std::error_code& ec) const {
        if (fail("entry", path(), ec))
            return {};
        return inner.symlink_status(ec);
    }
};
class directory_iterator {
    std::filesystem::directory_iterator inner_;
    path directory_;
    mutable directory_entry entry_;

public:
    directory_iterator() = default;
    directory_iterator(const path& value, std::error_code& ec) : directory_(value) {
        if (!fail("open", value, ec))
            inner_ = std::filesystem::directory_iterator(value, ec);
    }
    const directory_entry& operator*() const {
        entry_.inner = *inner_;
        return entry_;
    }
    bool operator!=(const directory_iterator& other) const { return inner_ != other.inner_; }
    directory_iterator& increment(std::error_code& ec) {
        if (fail("iterate", directory_, ec))
            inner_ = std::filesystem::directory_iterator{};
        else
            inner_.increment(ec);
        return *this;
    }
};
}  // namespace backend_fs

namespace sumhp {
namespace logging {
enum class Level { Info, Warning, Error };
}
namespace sumh {
enum class Status { Available, NotPresent, KernelTooOld, ClientTooOld };
struct Version {
    Status status;
    int last_errno = 0;
};
Version version_info() {
    return {sumh_available ? Status::Available
            : sumh_error   ? Status::NotPresent
                           : Status::KernelTooOld,
            sumh_error};
}
}  // namespace sumh
}  // namespace sumhp

namespace sumhp::mount {
namespace fs = backend_fs;
namespace fsutil {
void mlog(const std::string&, logging::Level = logging::Level::Info) {
    ++log_calls;
}
#include "sumh_partitions_under_test.inc"
}  // namespace fsutil
namespace {
bool proc_filesystems_has(const std::string&, int* error = nullptr) {
    if (error)
        *error = overlay_error;
    return overlay_available;
}
}  // namespace
#include "sumh_backend_selection_under_test.inc"
}  // namespace sumhp::mount

int main() {
    using namespace sumhp::mount;
    namespace fs = std::filesystem;
    assert(sumh_usable());
    const fs::path root =
        fs::temp_directory_path() /
        ("sumh-selection-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const ModuleEntry module{"sample", root};
    sumhp::Config config;
    config.overlayfs_enabled = config.magic_mount_enabled = true;
    config.mount_backend = "auto";
    ModuleModeMap modes;

    assert(resolve_module_backend(module, config, modes) == "none");
    const auto missing = resolve_module_backend_decision(module, config, modes);
    assert(!missing.complete && missing.reason == "content_observation_failed");
    fs::create_directories(root);
    const auto empty = resolve_module_backend_decision(module, config, modes);
    assert(empty.complete && !empty.has_content && empty.reason == "no_managed_partition_tree");
    assert(!empty.sumh.checked && !empty.overlay.checked);
    fs::create_directories(root / "system" / "etc");
    std::ofstream(root / "system" / "etc" / "sample.conf").put('x');
    assert(resolve_module_backend(module, config, modes) == "overlay");

    // Root files cannot be represented by leaf overlays, including an explicit
    // request for OverlayFS. Fallback must preserve the complete module tree.
    std::ofstream(root / "system" / "build.prop").put('x');
    assert(resolve_module_backend(module, config, modes) == "magic");
    modes[module.id] = "overlay";
    assert(resolve_module_backend(module, config, modes) == "magic");
    config.magic_mount_enabled = false;
    assert(resolve_module_backend(module, config, modes) == "none");
    config.magic_mount_enabled = true;
    fs::remove(root / "system" / "build.prop");

    // Configured partitions extend defaults consistently and are deduplicated.
    config.partitions = {"custom", "vendor", "custom", ""};
    const auto partitions = fsutil::managed_partitions(config.partitions);
    assert(partitions.size() == fsutil::managed_partitions().size() + 1);
    assert(std::count(partitions.begin(), partitions.end(), "custom") == 1);
    assert(std::count(partitions.begin(), partitions.end(), "system") == 1);
    fs::create_directories(root / "custom");
    std::ofstream(root / "custom" / "default.prop").put('x');
    assert(resolve_module_backend(module, config, modes) == "magic");
    fs::remove(root / "custom" / "default.prop");

    // A normalized installer alias is not a replacement entry. Its root files
    // still count, while arbitrary symlinks must use Magic's replacement path.
    fs::create_directories(root / "vendor" / "etc");
    std::error_code link_error;
    fs::create_directory_symlink(fs::path("..") / "vendor", root / "system" / "vendor", link_error);
    if (!link_error) {
        assert(resolve_module_backend(module, config, modes) == "overlay");
        std::ofstream(root / "vendor" / "build.prop").put('x');
        assert(resolve_module_backend(module, config, modes) == "magic");
        fs::remove(root / "vendor" / "build.prop");
        fs::create_directory_symlink(fs::path("..") / "vendor" / "etc",
                                     root / "system" / "custom-link");
        assert(resolve_module_backend(module, config, modes) == "magic");
        fs::remove(root / "system" / "custom-link");
    } else {
        std::cout << "symlink fixtures unavailable: " << link_error.message() << '\n';
    }

    modes[module.id] = "sumh";
    assert(resolve_module_backend(module, config, modes) == "sumh");
    assert(resolve_module_backend(module, config, modes, false) == "overlay");
    sumh_available = false;
    assert(resolve_module_backend(module, config, modes) == "overlay");
    overlay_available = false;
    assert(resolve_module_backend(module, config, modes) == "magic");
    config.mount_backend = "none";
    assert(resolve_module_backend(module, config, modes) == "none");

    auto decision = resolve_module_backend_decision(module, config, modes);
    assert(decision.configured_mode == "sumh" && decision.global_mode == "none");
    assert(decision.source == "global" && decision.requested == "none");
    assert(decision.reason == "explicit_none" && !decision.used_fallback);
    assert(!decision.sumh.checked && !decision.overlay.checked);

    config.mount_backend = "auto";
    overlay_available = true;
    decision = resolve_module_backend_decision(module, config, modes);
    assert(decision.sumh.checked && decision.sumh.known && !decision.sumh.available);
    assert(decision.reason == "sumh_protocol_mismatch" && decision.complete);
    assert(decision.source == "module" && decision.selected == "overlay");
    sumh_error = EACCES;
    decision = resolve_module_backend_decision(module, config, modes);
    assert(!decision.complete && !decision.sumh.known && decision.sumh.error_number == EACCES);
    assert(decision.selected == "overlay" && decision.reason == "sumh_readiness_unknown");
    decision = resolve_module_backend_decision(module, config, modes, false);
    assert(decision.complete && !decision.allow_sumh && !decision.sumh.checked);
    assert(decision.reason == "sumh_not_allowed_for_boot_owner");

    modes[module.id] = "overlay";
    overlay_available = false;
    overlay_error = EIO;
    log_calls = 0;
    decision = resolve_module_backend_decision(module, config, modes);
    assert(!decision.complete && decision.used_fallback && decision.selected == "magic");
    assert(decision.overlay.checked && !decision.overlay.known);
    assert(decision.reason == "overlay_readiness_unknown" && log_calls == 0);
    assert(resolve_module_backend(module, config, modes) == "magic" && log_calls == 1);
    overlay_error = 0;
    decision = resolve_module_backend_decision(module, config, modes);
    assert(decision.complete && decision.reason == "overlay_not_registered");
    config.overlayfs_enabled = false;
    decision = resolve_module_backend_decision(module, config, modes);
    assert(decision.reason == "overlay_disabled" && !decision.overlay.checked);
    config.overlayfs_enabled = true;
    overlay_available = true;
    std::ofstream(root / "system" / "root-file").put('x');
    decision = resolve_module_backend_decision(module, config, modes);
    assert(decision.requires_magic && decision.reason == "partition_root_requires_magic");
    assert(decision.complete && decision.selected == "magic");
    fs::remove(root / "system" / "root-file");

    // The diagnostic selected value agrees with the production wrapper for
    // each existing global/module request, while preserving their provenance.
    sumh_error = 0;
    sumh_available = true;
    for (const auto* requested : {"auto", "sumh", "overlay", "magic", "none"}) {
        modes[module.id] = requested;
        for (const auto* global : {"auto", "sumh", "overlay", "magic", "none"}) {
            config.mount_backend = global;
            const auto actual = resolve_module_backend_decision(module, config, modes);
            assert(actual.selected == resolve_module_backend(module, config, modes));
            assert(actual.source == (std::string(global) == "auto" ? "module" : "global"));
        }
    }

    std::string rule_error;
    for (const auto* excluded : {"hide", "none"}) {
        const std::vector<ModuleRule> exclusion{{"/hidden", excluded}};
        for (const auto* fallback : {"overlay", "magic", "none"}) {
            assert(!validate_module_backend_rules("sumh", fallback, exclusion, rule_error));
            assert(rule_error.find("refusing fallback backend") != std::string::npos);
        }
        assert(validate_module_backend_rules("sumh", "sumh", exclusion, rule_error));
        assert(validate_module_backend_rules("overlay", "overlay", exclusion, rule_error));
    }
    assert(validate_module_backend_rules("sumh", "magic", {}, rule_error));
    assert(validate_module_backend_rules("sumh", "magic", {{"/file", "sumh"}}, rule_error));

    config.mount_backend = "overlay";
    metadata_fault_path = root / "system";
    for (const auto* phase : {"layout", "open", "iterate"}) {
        metadata_fault = phase;
        const auto uncertain = resolve_module_backend_decision(module, config, modes);
        assert(!uncertain.complete && !uncertain.observation_error.empty());
        assert(uncertain.selected == "magic" && uncertain.reason == "overlay_layout_unknown");
    }
    metadata_fault = "entry";
    metadata_fault_path = root / "system" / "etc";
    const auto entry_error = resolve_module_backend_decision(module, config, modes);
    assert(!entry_error.complete && entry_error.selected == "magic");
    metadata_fault = "content";
    metadata_fault_path = root;
    const auto root_error = resolve_module_backend_decision(module, config, modes);
    assert(!root_error.complete && root_error.selected == "none" && !root_error.has_content);
    metadata_fault.clear();

    fs::remove_all(root);
    std::cout << "sumh_backend_selection_test: all checks passed\n";
}
