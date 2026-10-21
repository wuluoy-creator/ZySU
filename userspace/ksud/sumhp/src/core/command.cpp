#include "core/command.hpp"
#include "core/capture_buffer.hpp"
#include "core/operation_submit.hpp"
#include "core/user_hide.hpp"
#include "sumhp/embedded.hpp"

#include <chrono>
#include "core/daemon.hpp"
#include "core/diagnostic_package.hpp"
#include "core/json.hpp"
#include "core/log.hpp"
#include "core/profile.hpp"
#include "core/rule_inventory.hpp"
#include "core/runtime.hpp"
#include "core/runtime_status.hpp"
#include "core/state_ledger.hpp"
#include "defs.hpp"
#include "mount/backend.hpp"
#include "mount/development.hpp"
#include "mount/kernel_build_apply.hpp"
#include "mount/magic_mount.hpp"
#include "mount/maps_preview.hpp"
#include "mount/maps_temporary.hpp"
#include "mount/module_preview.hpp"
#include "mount/module_rebuild.hpp"
#include "mount/mount_fs.hpp"
#include "mount/overlayfs.hpp"
#include "mount/storage.hpp"
#include "mount/sumh.hpp"
#include "sumhp/config.hpp"
#include "sumhp/kernel_build.hpp"
#include "sumhp/sumh_client.hpp"
#include "uapi/sumh.h"
#include "userspace/common/temporary_maps_request.hpp"
#include "utils.hpp"

#include <sys/statvfs.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace sumhp {

namespace fs = std::filesystem;

using mount::fsutil::managed_partitions;
using mount::fsutil::partition_mount_point;

namespace {
fs::path data_dir() {
    return runtime_data_dir();
}

fs::path modules_dir() {
    return runtime_modules_dir();
}

fs::path config_file() {
    return runtime_config_file();
}

void print_usage() {
    std::cout
        << "SUMHP " << ksud::VERSION_NAME << "\n"
        << "usage:\n"
        << "  ksud sumhp version\n"
        << "  ksud sumhp config show\n"
        << "  ksud sumhp config gen\n"
        << "  ksud sumhp config merge-json JSON\n"
        << "  ksud sumhp config apply\n"
        << "  ksud sumhp daemon status|serve|call|ping|stop\n"
        << "  ksud sumhp api "
           "system|storage|sumh|runtime|state|features|hooks|backends|meta|capabilities\n"
        << "  ksud sumhp doctor --input <absolute-file|->  (read-only diagnostic JSON)\n"
        << "  ksud sumhp doctor package create <id> --input <absolute-file|-> "
           "[--include-identifiers]\n"
        << "  ksud sumhp doctor package list  |  doctor package preview <id>\n"
        << "  ksud sumhp doctor package export|remove <id> --revision <sha256-v1:...>\n"
        << "  ksud sumhp api inventory [category [instance_id generation cursor]]\n"
        << "  ksud sumhp operation list|show <id>|result <id>|wait <id> <timeout-ms>\n"
        << "  ksud sumhp operation prepare --input <absolute-file|->\n"
        << "  ksud sumhp operation execute <id> --input <absolute-file|->\n"
        << "  ksud sumhp profile list|show <id>|export <id>\n"
        << "  ksud sumhp profile preview <id> --resources <all|comma-separated-resources>\n"
        << "  ksud sumhp profile import-preview --input <absolute-file|-> --resources <scopes>\n"
        << "  ksud sumhp profile capture|import|clone|rename|remove --input <absolute-file|->\n"
        << "    Profiles stay inactive; activation/restoration are not implemented.\n"
        << "  ksud sumhp module "
           "list|add|delete|configure|set-mode|add-rule|remove-rule|hot-mount|hot-unmount|check-"
           "conflicts|mount-all|unmount|normalize\n"
        << "  ksud sumhp recovery status|reset\n"
        << "  ksud sumhp hide list|apply|add|remove\n"
        << "  ksud sumhp sumh "
           "version|list|clear|fix-mounts|hide-overlay-xattrs|mount-hide "
           "off|normal|aggressive|maps\n"
        << "  ksud sumhp sumh kernel-build show|set <release> <version>|reset\n"
        << "  ksud sumhp sumh maps list-paths|preview|apply\n"
        << "  ksud sumhp sumh maps list-temporary [instance generation cursor]\n"
        << "  ksud sumhp sumh maps remove <ino> <dev> --instance <id> --generation <generation>\n"
        << "  ksud sumhp config discover-partitions\n"
        << "  ksud sumhp config set-partitions <revision> <JSON-array>\n"
        << "  ksud sumhp module inspect <absolute-module-path>\n"
        << "  ksud sumhp module normalize <absolute-module-path> --dry-run\n"
        << "  ksud sumhp module plan [--input <absolute-file|->]\n"
        << "  ksud sumhp module rebuild-status\n"
        << "  ksud sumhp module explain <absolute-path> [--input <absolute-file|->]\n"
        << "  ksud sumhp sumh maps add-path <target-path> <replacement-path>\n"
        << "  ksud sumhp sumh maps remove-path <target-path>\n";
}

std::string arg_or_default(const std::vector<std::string>& args, std::size_t index,
                           const std::string& fallback) {
    return index < args.size() ? args[index] : fallback;
}

// Magic/Overlay mounts are boot-only; post-boot controls replay SUMH mappings only.
bool system_boot_completed() {
    return ksud::getprop("sys.boot_completed") == "1";
}

void print_string_array(const std::vector<std::string>& values) {
    std::cout << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            std::cout << ",";
        }
        std::cout << json_quote(values[i]);
    }
    std::cout << "]";
}

std::string read_first_line(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    if (std::getline(in, line)) {
        return line;
    }
    return "";
}

std::string read_file(const fs::path& path) {
    return ksud::read_file(path.string()).value_or("");
}

bool parse_unsigned_long(const std::string& value, unsigned long& out) {
    try {
        std::size_t parsed = 0;
        const unsigned long result = std::stoul(value, &parsed, 0);
        if (parsed != value.size()) {
            return false;
        }
        out = result;
        return true;
    } catch (...) {
        return false;
    }
}

std::map<std::string, std::string> read_prop_file(const fs::path& path) {
    std::map<std::string, std::string> props;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }
        props[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return props;
}

std::string kernel_release() {
    const std::string version = read_first_line("/proc/version");
    const std::string marker = "Linux version ";
    const auto start = version.find(marker);
    if (start == std::string::npos) {
        return version.empty() ? "Unknown" : version;
    }
    const auto value_start = start + marker.size();
    const auto value_end = version.find(' ', value_start);
    return version.substr(value_start, value_end - value_start);
}

std::string format_bytes(unsigned long long bytes) {
    const char* const units[] = {"B", "K", "M", "G", "T"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    if (unit == 0) {
        out << static_cast<unsigned long long>(value) << units[unit];
    } else {
        out.setf(std::ios::fixed);
        out.precision(value < 10.0 ? 1 : 0);
        out << value << units[unit];
    }
    return out.str();
}

bool path_is_read_only_mount(const std::string& mount_point) {
    std::ifstream mounts("/proc/mounts");
    std::string line;
    while (std::getline(mounts, line)) {
        std::istringstream parts(line);
        std::string dev;
        std::string mp;
        std::string fs_type;
        std::string options;
        parts >> dev >> mp >> fs_type >> options;
        if (mp == mount_point) {
            return options.find("ro") != std::string::npos;
        }
    }
    return false;
}
}  // namespace

struct MountEntry {
    std::string mount_point;
    std::string fstype;
    std::string source;
};

// Parse /proc/self/mountinfo into (mount_point, fstype, source) tuples.
namespace {
std::vector<MountEntry> read_mountinfo() {
    std::vector<MountEntry> out;
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    while (std::getline(in, line)) {
        const auto sep = line.find(" - ");
        if (sep == std::string::npos) {
            continue;
        }
        std::istringstream pre(line.substr(0, sep));
        std::vector<std::string> f;
        std::string tok;
        while (pre >> tok) {
            f.push_back(tok);
        }
        if (f.size() < 5) {
            continue;
        }
        std::istringstream post(line.substr(sep + 3));
        MountEntry e;
        e.mount_point = mount::fsutil::decode_mount_path(f[4]);
        post >> e.fstype >> e.source;
        e.source = mount::fsutil::decode_mount_path(e.source);
        out.push_back(e);
    }
    return out;
}

Config current_config() {
    Config config;
    std::string error;
    read_config_file(config, error);  // struct defaults on miss
    return config;
}

// A fresh installation can use defaults; damaged saved mount settings cannot.
bool read_mount_config(Config& config, std::string& error) {
    errno = 0;
    if (read_config_file(config, error))
        return true;
    if (errno != ENOENT)
        return false;
    config = Config{};
    error.clear();
    return true;
}

bool write_hot_mount_marker(const fs::path& marker, bool unmounted, std::string& error) {
    if (unmounted) {
        if (ksud::write_file_durable(marker, "\n", error))
            return true;
        error = "write hot-mount state: " + error;
        return false;
    }
    std::error_code ec;
    fs::remove(marker, ec);
    if (ec) {
        error = "remove hot-mount state: " + ec.message();
        return false;
    }
    return true;
}

bool change_module_mount_state(const std::string& id, bool unmounting, const Config& config,
                               std::string& error) {
    if (!mount::sumh::module_rebuild_allowed(error))
        return false;
    const fs::path marker = data_dir() / "run" / "hot_unmounted" / id;
    std::error_code ec;
    const auto status = fs::symlink_status(marker, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        error = "read hot-mount state: " + ec.message();
        return false;
    }
    const bool existed = fs::exists(status);
    if (existed && !fs::is_regular_file(status)) {
        error = "hot-mount state is not a regular file: " + marker.string();
        return false;
    }
    if (existed != unmounting) {
        if (unmounting) {
            ec.clear();
            fs::create_directories(marker.parent_path(), ec);
            if (ec) {
                error = "prepare hot-mount state: " + ec.message();
                return false;
            }
        }
        if (!write_hot_mount_marker(marker, unmounting, error))
            return false;
    }
    bool mappings_applied = false;
    mount::sumh::ModuleRebuildOutcome outcome = mount::sumh::ModuleRebuildOutcome::Unchanged;
    if (mount::refresh_sumh_modules(config, &mappings_applied, &outcome))
        return true;
    if (outcome != mount::sumh::ModuleRebuildOutcome::Unchanged) {
        error = std::string(outcome == mount::sumh::ModuleRebuildOutcome::Applied
                                ? "module rule calls acknowledged, but follow-up failed; "
                                : "module rebuild partially applied or outcome unknown; ") +
                "requested hot-mount state retained; query 'module rebuild-status' "
                "before further action (see controller log)";
        return false;
    }
    error = "failed to refresh SUMH mappings";
    if (existed != unmounting) {
        std::string rollback_error;
        if (!write_hot_mount_marker(marker, existed, rollback_error))
            error += "; previous hot-mount state could not be restored: " + rollback_error;
    }
    return false;
}

int print_storage_json() {
    const Config config = current_config();
    const std::string mirror_base = mount::storage::current_mirror_dir(config);

    // Report the active backend's storage base (tmpfs/ext4/erofs): the shared
    // mirror root if mounted, else a committed Magic Mount tmpfs skeleton,
    // else fall back to the host /data filesystem. Magic releases its staging
    // mount after commit. Loop-backed ext4/EROFS uses a loop-device source, so
    // the mirror is identified by mountpoint rather than source.
    std::string mode = "host";
    fs::path target = data_dir();
    const auto magic_mounts = mount::magic::active_mounts(config);
    for (const auto& m : read_mountinfo()) {
        if (m.mount_point == mirror_base ||
            (m.fstype == "tmpfs" && std::find(magic_mounts.begin(), magic_mounts.end(),
                                              m.mount_point) != magic_mounts.end())) {
            mode = m.fstype;
            target = m.mount_point;
            break;
        }
    }

    struct statvfs st = {};
    if (statvfs(target.c_str(), &st) != 0) {
        std::cout << "{\"error\":\"not mounted\"}\n";
        return 0;
    }

    const auto total = static_cast<unsigned long long>(st.f_blocks) * st.f_frsize;
    const auto avail = static_cast<unsigned long long>(st.f_bavail) * st.f_frsize;
    const auto used = total > avail ? total - avail : 0;
    const int percent = total > 0 ? static_cast<int>((used * 100ULL) / total) : 0;

    std::cout << "{"
              << "\"size\":" << json_quote(format_bytes(total)) << ","
              << "\"used\":" << json_quote(format_bytes(used)) << ","
              << "\"avail\":" << json_quote(format_bytes(avail)) << ","
              << "\"percent\":" << percent << ","
              << "\"mode\":" << json_quote(mode) << "}\n";
    return 0;
}

int print_partitions_json() {
    std::cout << "[";
    for (std::size_t i = 0; i < managed_partitions().size(); ++i) {
        const auto& name = managed_partitions()[i];
        const auto mount_point = partition_mount_point(name);
        if (i > 0) {
            std::cout << ",";
        }
        std::cout << "{"
                  << "\"name\":" << json_quote(name) << ","
                  << "\"mount_point\":" << json_quote(mount_point) << ","
                  << "\"fs_type\":\"\","
                  << "\"is_read_only\":"
                  << (path_is_read_only_mount(mount_point) ? "true" : "false") << ","
                  << "\"exists_as_symlink\":" << (fs::is_symlink(mount_point) ? "true" : "false")
                  << "}";
    }
    std::cout << "]";
    return 0;
}

int print_features_json(int bitmask) {
    const auto names = sumh::feature_names(bitmask);
    std::cout << "{\"bitmask\":" << bitmask << ",\"names\":";
    print_string_array(names);
    std::cout << "}";
    return 0;
}

int print_sumh_snapshot_json() {
    const auto version = sumh::version_info();
    const bool available = version.status == sumh::Status::Available;
    const int bitmask = available ? sumh::features() : 0;
    std::cout << "{\"transport\":\"ksu-fd\",\"available\":" << (available ? "true" : "false")
              << ",\"status\":" << static_cast<int>(version.status)
              << ",\"expected_protocol\":" << version.expected_protocol
              << ",\"kernel_protocol\":" << version.kernel_protocol
              << ",\"last_errno\":" << version.last_errno
              << ",\"enabled\":" << (available && sumh::enabled_state() > 0 ? "true" : "false")
              << ",\"features\":";
    print_features_json(bitmask);
    std::cout << ",\"hooks\":" << json_quote(available ? sumh::hooks() : "")
              << ",\"runtime_status\":"
              << runtime_status_json(query_runtime_status(),
                                     read_first_line("/proc/sys/kernel/random/boot_id"))
              << "}\n";
    return 0;
}

int apply_config_file() {
    Config config;
    std::string error;
    if (!read_config_file(config, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    if (!sumh::is_available()) {
        std::cerr << "SUMH is unavailable\n";
        return 1;
    }

    // Rebuild module mappings only when this boot already established that
    // SUMH owns them. A first post-boot load may follow an Overlay/Magic
    // fallback and must not migrate or stack backends underneath live mounts.
    if (mount::sumh::has_replayable_mappings() && !mount::sumh::is_active()) {
        if (!mount::refresh_sumh_modules(config)) {
            std::cerr << "failed to replay persisted SUMH configuration\n";
            return 1;
        }
        return print_sumh_snapshot_json();
    }

    // These features have separate state and failure boundaries. Report every
    // attempted group and retain successful work when a later group fails.
    std::string failures;
    const auto failed = [&](const std::string& detail) {
        if (!failures.empty())
            failures += "; ";
        failures += detail;
    };
    if (!mount::sumh::apply_feature_config(config, error, true))
        failed(error);
    if (!mount::fsutil::run_in_init_mount_ns(
            [&]() { return mount::overlay::restore_xattr_hiding(config); })) {
        failed("overlay_xattr_hide=partial_or_unknown: restoration failed; "
               "unrelated features retained");
    }
    if (!mount::sumh::restore_persisted_hide_rules(error, RestoreTrigger::UserRequested)) {
        failed("user_hide=partial_or_unknown: restoration failed: " + error);
    }
    if (!failures.empty()) {
        std::cerr << "configuration application incomplete: " << failures << "\n";
        return 1;
    }
    return print_sumh_snapshot_json();
}

void print_backend_statuses_json() {
    const auto statuses = mount::backend_statuses();
    std::cout << "[";
    for (std::size_t i = 0; i < statuses.size(); ++i) {
        const auto& status = statuses[i];
        if (i > 0) {
            std::cout << ",";
        }
        std::cout << "{"
                  << "\"kind\":" << json_quote(mount::backend_kind_name(status.kind)) << ","
                  << "\"name\":" << json_quote(status.name) << ","
                  << "\"available\":" << (status.available ? "true" : "false") << ","
                  << "\"preferred\":" << (status.preferred ? "true" : "false") << ","
                  << "\"detail\":" << json_quote(status.detail) << "}";
    }
    std::cout << "]";
}

int print_system_json() {
    const auto version = sumh::version_info();
    const int bitmask = version.status == sumh::Status::Available ? sumh::features() : 0;
    const std::string hook_text = version.status == sumh::Status::Available ? sumh::hooks() : "";

    // Count our live mounts (our mount source) for the stats panel.
    const Config config = current_config();
    auto owned_paths = mount::magic::active_mounts(config);
    const auto overlay_paths = mount::overlay::active_mounts(config);
    owned_paths.insert(owned_paths.end(), overlay_paths.begin(), overlay_paths.end());
    const std::set<std::string> owned(owned_paths.begin(), owned_paths.end());
    int total_mounts = 0;
    int overlay_mounts = 0;
    for (const auto& m : read_mountinfo()) {
        if (m.source != config.mount_source && !owned.count(m.mount_point)) {
            continue;
        }
        ++total_mounts;
        if (m.fstype == "overlay") {
            ++overlay_mounts;
        }
    }

    std::cout << "{"
              << "\"kernel\":" << json_quote(kernel_release()) << ","
              << "\"mount_base\":" << json_quote(mount::storage::current_mirror_dir(config)) << ","
              << "\"sumh_available\":"
              << (version.status == sumh::Status::Available ? "true" : "false") << ","
              << "\"sumh_status\":" << static_cast<int>(version.status) << ","
              << "\"hooks\":" << json_quote(hook_text) << ","
              << "\"features\":";
    print_features_json(bitmask);
    std::cout << ",\"mountStats\":{\"total_mounts\":" << total_mounts
              << ",\"successful_mounts\":" << total_mounts
              << ",\"failed_mounts\":0,\"tmpfs_created\":0,\"files_mounted\":0,\"dirs_mounted\":0,"
              << "\"symlinks_created\":0,\"overlayfs_mounts\":" << overlay_mounts
              << ",\"success_rate\":" << (total_mounts > 0 ? 100 : 0) << "},"
              << "\"detectedPartitions\":";
    print_partitions_json();
    std::cout << ",\"backends\":";
    print_backend_statuses_json();
    std::cout << "}\n";
    return 0;
}

int print_meta_json(bool include_backends = true) {
    std::cout
        << "{"
        << "\"embedded\":true,\"native_control\":true,\"module_configure\":true,\"external_"
           "mount_owner\":"
        << json_quote(embedded_external_mount_owner()) << ","
        << "\"runtime_status_schema\":1,\"state_ledger_schema\":1,\"state_ledger_storage_schema\":"
           "3,"
        << "\"capability_snapshot_schema\":1,\"capability_snapshot_read_only\":true,"
        << "\"capability_snapshot_transport_schema\":1,\"capability_snapshot_response_bytes\":"
           "65536,"
        << "\"diagnostics_schema\":1,\"diagnostics_read_only\":true,\"diagnostics_transport_"
           "schema\":1,"
        << "\"diagnostics_input_bytes\":4096,\"diagnostics_response_bytes\":65536,"
        << "\"diagnostics_scenarios\":[\"rule-not-effective\",\"module-fallback\",\"maps-failure\","
           "\"boot-protection\"],"
        << "\"diagnostics_manager\":false,\"diagnostics_repair\":false,"
        << "\"diagnostic_package_schema\":1,\"diagnostic_package_create\":true,\"diagnostic_"
           "package_preview\":true,"
        << "\"diagnostic_package_export\":true,\"diagnostic_package_remove\":true,\"diagnostic_"
           "package_manager\":true,"
        << "\"diagnostic_package_manager_schema\":1,"
        << "\"diagnostic_package_sampling_read_only\":true,\"diagnostic_package_storage_"
           "mutations\":true,"
        << "\"diagnostic_package_default_privacy\":\"redacted\",\"diagnostic_package_identifiers_"
           "opt_in\":true,"
        << "\"diagnostic_package_bytes\":" << diagnostic_package::package_limit << ","
        << "\"diagnostic_package_bank_bytes\":" << diagnostic_package::bank_limit << ","
        << "\"diagnostic_package_count_limit\":" << diagnostic_package::package_count_limit << ","
        << "\"operation_query_schema\":2,\"operation_list\":true,\"operation_show\":true,"
           "\"operation_wait\":true,"
        << "\"operation_submission_schema\":1,\"operation_prepare\":true,\"operation_execute\":"
           "true,\"operation_reserved_id\":true,"
        << "\"operation_manager_transport_schema\":1,\"operation_resource_result_schema\":1,"
           "\"operation_result\":true,"
        << "\"operation_resource_result_resources\":[\"user_hide_mutation\",\"temporary_maps_"
           "mutation\"],"
        << "\"operation_local_temporary_maps_remove\":true,\"operation_resource_store_schema\":2,"
        << "\"operation_wait_max_ms\":10000,\"operation_history_limit\":16,\"operation_caller_id\":"
           "false,"
        << "\"operation_input_bytes\":49152,\"operation_output_stream_bytes\":65536,"
        << "\"hide_definitions_schema\":1,\"hide_status\":true,\"hide_definition_mutations\":true,"
        << "\"hide_manager_transport_schema\":1,"
        << "\"hide_definition_import_schema\":1,\"hide_definition_exchange_schema\":1,\"hide_"
           "definition_export_page_schema\":1,"
        << "\"hide_status_page_size\":16,\"hide_result_item_limit\":64,\"hide_mutation_requires_"
           "revision\":true,"
        << "\"rule_inventory_schema\":1,"
        << "\"maps_temporary_list\":true,\"maps_temporary_remove_schema\":1,"
        << "\"maps_temporary_manager_schema\":1,\"maps_temporary_manager_channel\":true,"
        << "\"maps_temporary_remove_requires_kernel_capability\":true,\"maps_temporary_operation_"
           "journal\":false,"
        << "\"maps_temporary_tracked_cli\":true,\"maps_temporary_tracked_manager\":true,"
        << "\"operation_manager_temporary_maps_schema\":2,"
        << "\"profile_schema\":1,\"profile_bank_schema\":1,\"profile_capture\":true,\"profile_"
           "import\":true,"
        << "\"profile_export\":true,\"profile_clone\":true,\"profile_rename\":true,\"profile_"
           "remove\":true,"
        << "\"profile_list\":true,\"profile_show\":true,\"profile_import_preview\":true,\"profile_"
           "diff_preview\":true,"
        << "\"profile_activate\":false,\"profile_restore\":false,\"profile_manager_transport\":"
           "true,"
        << "\"profile_manager_schema\":1,\"profile_manager_actions\":{\"list\":true,\"show\":true,"
           "\"export\":true,"
        << "\"preview\":true,\"import-preview\":true,\"capture\":true,\"import\":true,\"clone\":"
           "true,\"rename\":true,\"remove\":true},"
        << "\"profile_operation_journal\":false,\"profile_input_bytes\":"
        << profiles::PROFILE_INPUT_MAX_BYTES << ","
        << "\"profile_document_bytes\":" << profiles::PROFILE_MAX_BYTES << ","
        << "\"profile_bank_bytes\":" << profiles::BANK_MAX_BYTES << ","
        << "\"profile_count_limit\":" << profiles::BANK_MAX_PROFILES << ","
        << "\"module_plan\":true,\"module_explain\":true,\"module_plan_schema\":1,"
        << "\"module_manager_preview_transport_schema\":1,\"module_manager_inline_candidate\":true,"
        << "\"module_manager_saved_preview\":true,\"module_manager_explain\":true,"
        << "\"module_inspect\":true,\"module_apply\":false,\"module_normalize_preview\":true,"
        << "\"module_rebuild_status\":true,\"module_rebuild_status_schema\":1,"
        << "\"module_rebuild_owner_transaction\":false,\"module_rebuild_history_bytes\":524288,"
        << "\"partition_discovery\":true,\"partition_save_only\":true,\"partition_workflow_"
           "schema\":1,"
        << "\"module_plan_input_bytes\":" << mount::MODULE_PREVIEW_INPUT_BYTES << ","
        << "\"module_plan_response_bytes\":" << mount::MODULE_PREVIEW_RESPONSE_BYTES << ","
        << "\"version\":" << json_quote(ksud::VERSION_NAME) << ","
        << "\"data_dir\":" << json_quote(runtime_data_dir().string()) << ","
        << "\"modules_dir\":" << json_quote(runtime_modules_dir().string()) << ","
        << "\"config_file\":" << json_quote(runtime_config_file().string()) << ","
        << "\"socket\":" << json_quote(runtime_socket_file().string()) << ","
        << "\"pid_file\":" << json_quote(runtime_pid_file().string()) << ","
        << "\"log_file\":" << json_quote(runtime_log_file().string()) << ","
        << "\"backends\":";
    if (include_backends)
        print_backend_statuses_json();
    else
        std::cout << "[]";
    std::cout << "}\n";
    return 0;
}

int print_mount_status_json() {
    const Config config = current_config();
    auto owned_paths = mount::magic::active_mounts(config);
    const auto overlay_paths = mount::overlay::active_mounts(config);
    owned_paths.insert(owned_paths.end(), overlay_paths.begin(), overlay_paths.end());
    const std::set<std::string> owned(owned_paths.begin(), owned_paths.end());
    int total = 0;
    int overlays = 0;
    std::vector<std::string> active;
    for (const auto& mount : read_mountinfo()) {
        if (mount.source != config.mount_source && !owned.count(mount.mount_point))
            continue;
        ++total;
        active.push_back(mount.mount_point);
        if (mount.fstype == "overlay")
            ++overlays;
    }
    std::cout << "{\"mount_base\":" << json_quote(mount::storage::current_mirror_dir(config))
              << ",\"mountStats\":{\"total_mounts\":" << total
              << ",\"overlayfs_mounts\":" << overlays << "},\"active_mounts\":";
    print_string_array(active);
    std::cout << ",\"detectedPartitions\":";
    print_partitions_json();
    std::cout << "}\n";
    return 0;
}

int print_sumh_version_json() {
    const auto version = sumh::version_info();
    const std::string rules = version.status == sumh::Status::Available ? sumh::active_rules() : "";
    const auto modules = sumh::active_modules_from_rules(rules);
    const bool mismatch = version.status == sumh::Status::KernelTooOld ||
                          version.status == sumh::Status::ClientTooOld;

    const Config config = current_config();
    std::cout << "{"
              << "\"backend\":\"sumh\","
              << "\"protocol_version\":" << version.expected_protocol << ","
              << "\"kernel_version\":" << version.kernel_protocol << ","
              << "\"sumh_available\":"
              << (version.status == sumh::Status::Available ? "true" : "false") << ","
              << "\"protocol_mismatch\":" << (mismatch ? "true" : "false") << ","
              << "\"mismatch_message\":" << json_quote(mismatch ? "SUMH protocol mismatch" : "")
              << ","
              << "\"active_modules\":";
    print_string_array(modules);
    std::cout << ",\"mount_base\":" << json_quote(mount::storage::current_mirror_dir(config))
              << "}\n";
    return 0;
}

int print_sumh_rules_json() {
    const std::string rules = sumh::active_rules();
    std::istringstream lines(rules);
    std::string line;
    bool first = true;

    std::cout << "[";
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }

        std::istringstream parts(line);
        std::string type;
        parts >> type;
        std::transform(type.begin(), type.end(), type.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        if (!first) {
            std::cout << ",";
        }
        first = false;

        std::cout << "{\"type\":" << json_quote(type);
        if (type == "ADD" || type == "MERGE") {
            std::string target;
            std::string source;
            parts >> target >> source;
            std::cout << ",\"target\":" << json_quote(target)
                      << ",\"source\":" << json_quote(source);
        } else if (type == "HIDE") {
            std::string path;
            parts >> path;
            std::cout << ",\"path\":" << json_quote(path);
        } else {
            std::string rest;
            std::getline(parts, rest);
            if (!rest.empty() && rest[0] == ' ') {
                rest.erase(0, 1);
            }
            std::cout << ",\"args\":" << json_quote(rest);
        }
        std::cout << "}";
    }
    std::cout << "]\n";
    return 0;
}

int handle_config(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (mount::is_development_prefix(args)) {
        std::cerr << "partition discovery/save requires the exact local command; no daemon route\n";
        return 1;
    }
    if (sub == "show") {
        std::string config = read_file(config_file());
        if (config.empty()) {
            config = default_config_json();
        }
        JsonValue stored;
        std::string parse_error;
        if (!parse_json(config, stored, parse_error) || !stored.is_object()) {
            std::cerr << "invalid stored config: " << parse_error << "\n";
            return 1;
        }
        // Return exact stored bytes: unknown fields and large numeric values
        // must remain available for inspection without a lossy reserialization.
        std::cout << config;
        return 0;
    }
    if (sub == "gen") {
        std::string error;
        if (args.size() != 2) {
            std::cerr << "usage: ksud sumhp config gen\n";
            return 1;
        }
        if (!write_default_config(error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "merge-json") {
        if (args.size() != 3) {
            std::cerr << "usage: ksud sumhp config merge-json JSON\n";
            return 1;
        }
        std::string error;
        if (!merge_config_json(args[2], error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "apply") {
        if (args.size() != 2) {
            std::cerr << "usage: ksud sumhp config apply\n";
            return 1;
        }
        return apply_config_file();
    }
    print_usage();
    return 1;
}

int handle_api(const std::vector<std::string>& args) {
    if (args.size() >= 2 && args[1] == "inventory")
        return inventory::command(args, read_first_line("/proc/sys/kernel/random/boot_id"),
                                  std::cout);
    if (args.size() == 2 && args[1] == "state")
        return state::print_state_json();
    if (args.size() == 2 && args[1] == "mounts")
        return print_mount_status_json();
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "system") {
        return print_system_json();
    }
    if (sub == "storage") {
        return print_storage_json();
    }
    if (sub == "sumh") {
        return print_sumh_snapshot_json();
    }
    if (sub == "runtime" && args.size() == 2) {
        const auto status = query_runtime_status();
        std::cout << runtime_status_json(status, read_first_line("/proc/sys/kernel/random/boot_id"))
                  << '\n';
        const bool failed =
            std::string(status.status) == "error" || std::string(status.status) == "unavailable";
        if (failed)
            errno = status.error_number ? status.error_number : EIO;
        return failed ? 1 : 0;
    }
    if (sub == "features") {
        const auto version = sumh::version_info();
        return print_features_json(version.status == sumh::Status::Available ? sumh::features()
                                                                             : 0);
    }
    if (sub == "hooks") {
        if (!sumh::is_available()) {
            std::cerr << "SUMH not available.\n";
            return 1;
        }
        std::cout << sumh::hooks() << "\n";
        return 0;
    }
    if (sub == "backends") {
        print_backend_statuses_json();
        std::cout << "\n";
        return 0;
    }
    if (sub == "meta") {
        return print_meta_json(args.size() != 3 || args[2] != "--control-only");
    }
    print_usage();
    return 1;
}

int handle_module(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "plan" || sub == "explain" || sub == "manager-preview" || sub == "rebuild-status" ||
        mount::is_development_prefix(args)) {
        std::cerr << "module preview requires the exact local command: ksud sumhp module " << sub
                  << "; daemon routing is not a read-only preview entry\n";
        return 1;
    }
    if (sub == "list") {
        mount::ModuleModeMap modes;
        mount::ModuleRuleMap rule_map;
        std::string error;
        if (!mount::load_module_configuration(modes, rule_map, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        const bool include_disabled = args.size() == 3 && args[2] == "--all";
        const auto replayable = mount::sumh::replayable_module_ids();
        const std::set<std::string> sumh_boot_plan(replayable.begin(), replayable.end());
        Config cfg;
        if (!read_mount_config(cfg, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        const fs::path module_root = modules_dir();
        std::cout << "{\"modules\":[";
        bool first = true;
        if (fs::is_directory(module_root)) {
            for (const auto& entry : fs::directory_iterator(module_root)) {
                if (!entry.is_directory()) {
                    continue;
                }
                // Skip modules that opt out of metamodule mounting.
                std::error_code mec;
                if ((!include_disabled && fs::exists(entry.path() / "disable", mec)) ||
                    fs::exists(entry.path() / "remove", mec) ||
                    fs::exists(entry.path() / "skip_mount", mec) ||
                    (!include_disabled &&
                     fs::exists(data_dir() / "run" / "hot_unmounted" / entry.path().filename(),
                                mec)) ||
                    !fs::exists(entry.path() / "module.prop", mec)) {
                    continue;
                }
                // Only list modules that contribute mounts (have a managed
                // partition tree); skip plain modules (zygisk, etc.).
                bool has_mount_content = false;
                const auto parts = mount::fsutil::managed_partitions(cfg.partitions);
                for (const auto& part : parts) {
                    std::error_code ec;
                    if (fs::is_directory(entry.path() / part, ec)) {
                        has_mount_content = true;
                        break;
                    }
                }
                if (!has_mount_content) {
                    continue;
                }
                const std::string id = entry.path().filename().string();
                const auto props = read_prop_file(entry.path() / "module.prop");
                if (!first) {
                    std::cout << ",";
                }
                first = false;
                const auto mode_it = modes.find(id);
                const std::string mode = mode_it == modes.end() ? "auto" : mode_it->second;
                const auto rules_it = rule_map.find(id);
                // The same-boot plan is authoritative for SUMH ownership.
                // Without it, an explicit SUMH choice may already be running
                // through the boot-time Overlay/Magic fallback.
                std::string strategy;
                if (sumh_boot_plan.count(id) != 0) {
                    strategy = "sumh";
                } else {
                    strategy = mount::resolve_module_backend(mount::ModuleEntry{id, entry.path()},
                                                             cfg, modes, false);
                }
                std::cout << "{"
                          << "\"id\":" << json_quote(id) << ","
                          << "\"name\":" << json_quote(props.count("name") ? props.at("name") : id)
                          << ","
                          << "\"version\":"
                          << json_quote(props.count("version") ? props.at("version") : "") << ","
                          << "\"author\":"
                          << json_quote(props.count("author") ? props.at("author") : "") << ","
                          << "\"description\":"
                          << json_quote(props.count("description") ? props.at("description") : "")
                          << ","
                          << "\"mode\":" << json_quote(mode) << ","
                          << "\"strategy\":" << json_quote(strategy) << ","
                          << "\"path\":" << json_quote(entry.path().string()) << ","
                          << "\"rules\":[";
                if (rules_it != rule_map.end()) {
                    for (std::size_t i = 0; i < rules_it->second.size(); ++i) {
                        if (i > 0) {
                            std::cout << ",";
                        }
                        std::cout << "{\"path\":" << json_quote(rules_it->second[i].path)
                                  << ",\"mode\":" << json_quote(rules_it->second[i].mode) << "}";
                    }
                }
                std::cout << "]}";
            }
        }
        std::cout << "]}\n";
        return 0;
    }
    if (sub == "configure") {
        if (args.size() != 5) {
            std::cerr << "usage: ksud sumhp module configure <id> <mode> <rules-json-array>\n";
            return 1;
        }
        std::vector<mount::ModuleRule> rules;
        std::string error;
        if (!mount::parse_module_rules(args[4], rules, error) ||
            !mount::configure_module(args[2], args[3], rules, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "set-mode") {
        if (args.size() != 4) {
            std::cerr << "usage: ksud sumhp module set-mode <id> auto|sumh|overlay|magic|none\n";
            return 1;
        }
        std::string error;
        if (!mount::set_module_mode(args[2], args[3], error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "add-rule") {
        if (args.size() != 5) {
            std::cerr << "usage: ksud sumhp module add-rule <id> <absolute-path> "
                         "auto|sumh|overlay|magic|none|hide\n";
            return 1;
        }
        std::string error;
        if (!mount::upsert_module_rule(args[2], {args[3], args[4]}, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "remove-rule") {
        if (args.size() != 4) {
            std::cerr << "usage: ksud sumhp module remove-rule <id> <absolute-path>\n";
            return 1;
        }
        std::string error;
        if (!mount::remove_module_rule(args[2], args[3], error)) {
            std::cerr << error << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "hot-mount" || sub == "hot-unmount" || sub == "add" || sub == "delete") {
        const std::string id = arg_or_default(args, 2, "");
        if (!mount::valid_module_id(id)) {
            std::cerr << "usage: ksud sumhp module " << sub << " <id>\n";
            return 1;
        }
        Config cfg;
        std::string error;
        if (!read_mount_config(cfg, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        const fs::path module_root = modules_dir();
        const fs::path module_path = module_root / id;
        if (!fs::is_directory(module_path) || !fs::exists(module_path / "module.prop")) {
            std::cerr << "module not found: " << id << "\n";
            return 1;
        }
        const auto replayable = mount::sumh::replayable_module_ids();
        if (std::find(replayable.begin(), replayable.end(), id) == replayable.end()) {
            std::cerr
                << "module was not assigned to SUMH this boot; reboot to change its backend\n";
            return 1;
        }
        const bool unmounting = sub == "hot-unmount" || sub == "delete";
        if (!change_module_mount_state(id, unmounting, cfg, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cout << "{\"ok\":true,\"module\":" << json_quote(id)
                  << ",\"action\":" << json_quote(sub) << "}\n";
        return 0;
    }
    if (sub == "check-conflicts") {
        Config cfg;
        std::string error;
        if (!read_mount_config(cfg, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        std::vector<mount::ModuleEntry> modules;
        if (!mount::enumerate_mountable_modules_checked(modules, error)) {
            std::cerr << error << '\n';
            return 1;
        }
        const auto parts = mount::fsutil::managed_partitions(cfg.partitions);
        std::map<std::string, std::vector<std::string>> owners;
        for (const auto& module : modules) {
            for (const auto& part : parts) {
                const fs::path root = module.path / part;
                std::error_code ec;
                auto it = fs::recursive_directory_iterator(root, ec);
                const auto end = fs::recursive_directory_iterator();
                for (; it != end && !ec; it.increment(ec)) {
                    if (it->is_regular_file(ec) || it->is_symlink(ec)) {
                        owners[(fs::path("/") / part / it->path().lexically_relative(root))
                                   .string()]
                            .push_back(module.id);
                    }
                }
            }
        }
        std::cout << "[";
        bool first = true;
        for (const auto& [path, ids] : owners) {
            if (ids.size() < 2) {
                continue;
            }
            if (!first) {
                std::cout << ",";
            }
            first = false;
            std::cout << "{\"file\":" << json_quote(path) << ",\"modules\":";
            print_string_array(ids);
            std::cout << "}";
        }
        std::cout << "]\n";
        return 0;
    }
    if (sub == "mount-all") {
        // Boot-only: mounting over live service namespaces is unsafe.
        if (system_boot_completed()) {
            std::cerr << "refusing module mount-all: magic mount only runs at boot via "
                         "ksud; post-boot mounting breaks namespaces\n";
            return 1;
        }
        Config config;
        std::string cfg_err;
        if (!read_mount_config(config, cfg_err)) {
            std::cerr << cfg_err << "\n";
            return 1;
        }
        const auto report = mount::mount_all_enabled(config);
        std::cout << "{"
                  << "\"ok\":" << (report.ok ? "true" : "false") << ","
                  << "\"backend\":" << json_quote(report.backend) << ","
                  << "\"modules\":" << report.modules << ","
                  << "\"mounts\":" << report.mounts << ","
                  << "\"detail\":" << json_quote(report.detail) << "}\n";
        return report.ok ? 0 : 1;
    }
    if (sub == "normalize") {
        if (args.size() != 3 || !mount::valid_module_rule_path(args[2])) {
            std::cerr << "usage: ksud sumhp module normalize <absolute-module-path> [--dry-run]\n";
            return 1;
        }
        const auto result = mount::normalize_module_result_json(args[2]);
        std::cout << stringify_json(result) << '\n';
        const auto* ok = result.find("ok");
        return ok && ok->is_bool() && ok->b ? 0 : 1;
    }
    if (sub == "unmount") {
        // Tear down SUMHP's own mounts (source-gated; never touches real partitions).
        Config config;
        std::string cfg_err;
        if (!read_mount_config(config, cfg_err)) {
            std::cerr << cfg_err << "\n";
            return 1;
        }
        const bool ok = mount::unmount_all(config);
        std::cout << "{\"ok\":" << (ok ? "true" : "false") << "}\n";
        return ok ? 0 : 1;
    }
    print_usage();
    return 1;
}

int handle_sumh(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "version") {
        return print_sumh_version_json();
    }
    if (sub == "list") {
        return print_sumh_rules_json();
    }
    if (sub == "features") {
        const auto version = sumh::version_info();
        return print_features_json(version.status == sumh::Status::Available ? sumh::features()
                                                                             : 0);
    }
    if (sub == "kernel-build") {
        const auto operation = arg_or_default(args, 2, "show");
        if (operation == "show" && args.size() <= 3) {
            sumh::KernelBuild state;
            sumh::KernelBuild original;
            if (!sumh::kernel_build(state) || !sumh::original_kernel_build(original)) {
                std::cerr << "failed to query SUMH kernel build: " << std::strerror(errno) << "\n";
                return 1;
            }
            const auto suggestion = suggest_kernel_build_version(
                original.version, ksud::getprop("ro.build.date").value_or(""));
            std::cout << "{\"enabled\":" << (state.enabled ? "true" : "false")
                      << ",\"release\":" << json_quote(state.release)
                      << ",\"version\":" << json_quote(state.version)
                      << ",\"original_release\":" << json_quote(original.release)
                      << ",\"original_version\":" << json_quote(original.version)
                      << ",\"suggested_version\":" << json_quote(suggestion)
                      << ",\"suggestion_source\":"
                      << json_quote(suggestion.empty() ? "" : "ro.build.date") << "}\n";
            return 0;
        }
        const bool enable = operation == "set";
        if ((enable && args.size() != 5) ||
            (!enable && (operation != "reset" || args.size() != 3))) {
            std::cerr << "usage: ksud sumhp sumh kernel-build show|set <release> <version>|reset\n";
            return 1;
        }
        if (!enable) {
            // Disabling must remain possible when hand-edited build settings
            // cannot be parsed. Save the disabled state even if runtime reset
            // fails, so the next boot cannot reapply a broken configuration.
            std::string error;
            const auto outcome = mount::sumh::apply_kernel_build_config(
                {false, {}, {}}, sumh::feature_capabilities());
            const bool runtime_ok = outcome.state == mount::sumh::FeatureApplyState::Applied ||
                                    outcome.state == mount::sumh::FeatureApplyState::Deferred;
            const bool saved_ok = reset_kernel_build_config(error);
            if (!saved_ok)
                std::cerr << error << "\n";
            if (!runtime_ok)
                std::cerr << "SUMH kernel build reset: " << outcome.detail << "\n";
            std::cout << "{\"ok\":" << (saved_ok && runtime_ok ? "true" : "false")
                      << ",\"enabled\":false,\"saved\":" << (saved_ok ? "true" : "false")
                      << ",\"applied\":"
                      << (outcome.state == mount::sumh::FeatureApplyState::Applied ? "true"
                                                                                   : "false")
                      << ",\"runtime_result\":"
                      << json_quote(mount::sumh::feature_apply_state_name(outcome.state))
                      << ",\"detail\":" << json_quote(outcome.detail) << "}\n";
            if (!saved_ok || !runtime_ok)
                errno = outcome.error_number ? outcome.error_number : EIO;
            return saved_ok && runtime_ok ? 0 : 1;
        }
        const auto caps = sumh::feature_capabilities();
        if (!caps.ok || !(caps.bitmask & SUMH_FEATURE_KERNEL_BUILD_SPOOF)) {
            errno = caps.ok ? EOPNOTSUPP : caps.last_errno;
            std::cerr << "SUMH kernel build spoof is unavailable: " << std::strerror(errno) << "\n";
            return 1;
        }
        const auto release = args[3] == "default" ? "" : args[3];
        const auto version = args[4] == "default" ? "" : args[4];
        std::string error;
        JsonValue patch;
        patch.type = JsonValue::Type::Object;
        patch.o["enable_kernel_build_spoof"] = JsonValue(enable);
        if (enable) {
            patch.o["kernel_build_release"] = JsonValue(release);
            patch.o["kernel_build_version"] = JsonValue(version);
        }
        error.clear();
        if (!merge_config_json(stringify_json(patch), error)) {
            std::cerr << error << "\n";
            return 1;
        }
        const auto outcome = mount::sumh::apply_kernel_build_config({true, release, version}, caps);
        const bool applied = outcome.state == mount::sumh::FeatureApplyState::Applied;
        // The saved intent is independent from runtime success. Never rewrite
        // old configuration after a failed/unknown ioctl: another writer may
        // already have changed it, and an acknowledgement may have been lost.
        if (!applied)
            std::cerr << "Kernel Build settings saved; runtime "
                      << mount::sumh::feature_apply_state_name(outcome.state) << ": "
                      << outcome.detail << "; query state before retry\n";
        std::cout << "{\"ok\":" << (applied ? "true" : "false")
                  << ",\"enabled\":true,\"saved\":true,\"applied\":" << (applied ? "true" : "false")
                  << ",\"runtime_result\":"
                  << json_quote(mount::sumh::feature_apply_state_name(outcome.state))
                  << ",\"detail\":" << json_quote(outcome.detail) << "}\n";
        if (!applied)
            errno = outcome.error_number ? outcome.error_number : EIO;
        return applied ? 0 : 1;
    }
    if (sub == "maps") {
        const std::string op = arg_or_default(args, 2, "");
        if (op == "preview") {
            if (!mount::sumh::is_maps_preview_command(args)) {
                std::cerr << "usage: ksud sumhp sumh maps preview\n";
                return 1;
            }
            std::cout << mount::sumh::maps_preview_json() << '\n';
            return std::cout.good() ? 0 : 1;
        }
        if (op == "list-paths" || op == "add-path" || op == "remove-path" || op == "apply") {
            const bool automatic = op == "apply" && args.size() == 4 && args[3] == "--automatic";
            std::size_t expected = 3;
            if (op == "add-path")
                expected = 5;
            else if (op == "remove-path")
                expected = 4;
            if (args.size() != expected && !automatic) {
                std::cerr
                    << "usage: ksud sumhp sumh maps list-paths|apply|"
                       "add-path <target-path> <replacement-path>|remove-path <target-path>\n";
                return 1;
            }
            // Lifecycle requests are rechecked in the daemon; a cold start or
            // delayed request must not turn an automatic restore into a user action.
            if (automatic &&
                !should_restore(
                    {ksud::is_safe_mode(), ksud::getprop("sys.boot_completed").value_or("0") == "1",
                     sumh::is_available(), !embedded_external_mount_owner().empty()},
                    RestoreFeature::Maps)) {
                std::cout << "{\"saved\":false,\"applied\":false,\"skipped\":true}\n";
                return 0;
            }
            Config config;
            std::string error;
            if (!read_config_file(config, error)) {
                std::cerr << error << "\n";
                return 1;
            }
            if (op == "add-path") {
                const auto found = std::find_if(
                    config.maps_rules.begin(), config.maps_rules.end(),
                    [&](const MapsPathRule& rule) { return rule.target_path == args[3]; });
                if (found == config.maps_rules.end())
                    config.maps_rules.push_back({args[3], args[4]});
                else
                    found->replacement_path = args[4];
            } else if (op == "remove-path") {
                if (args[3].empty() || args[3].front() != '/' ||
                    args[3].find('\0') != std::string::npos ||
                    args[3].find_first_of("\r\n") != std::string::npos) {
                    std::cerr << "maps target must be an absolute, NUL/CR/LF-free path\n";
                    return 1;
                }
                config.maps_rules.erase(
                    std::remove_if(
                        config.maps_rules.begin(), config.maps_rules.end(),
                        [&](const MapsPathRule& rule) { return rule.target_path == args[3]; }),
                    config.maps_rules.end());
            }
            JsonValue rules = maps_path_rules_json(config.maps_rules);
            if (op == "list-paths") {
                std::cout << stringify_json(rules) << "\n";
                return 0;
            }
            const bool saving = op != "apply";
            if (saving) {
                JsonValue patch;
                patch.type = JsonValue::Type::Object;
                patch.o["maps_rules"] = std::move(rules);
                if (!merge_config_json(stringify_json(patch), error)) {
                    std::cerr << error << "\n";
                    return 1;
                }
            }
            // Automatic boot restoration uses apply too: disabled definitions
            // must not resolve missing files or reinstall stale identities.
            mount::sumh::FeatureApplyOutcome outcome;
            mount::sumh::ManagedMapsRestoreState managed_state =
                mount::sumh::ManagedMapsRestoreState::Unchanged;
            const bool applied =
                op == "apply"
                    ? mount::sumh::apply_maps_config(
                          config, error, true, &outcome,
                          automatic ? RestoreTrigger::Automatic : RestoreTrigger::UserRequested)
                    : mount::sumh::restore_managed_maps_rules(config, error, &managed_state,
                                                              RestoreTrigger::UserRequested);
            const int apply_errno = errno;
            if (op != "apply") {
                if (applied)
                    outcome.state = mount::sumh::FeatureApplyState::Applied;
                else if (managed_state == mount::sumh::ManagedMapsRestoreState::Unknown)
                    outcome.state = mount::sumh::FeatureApplyState::Unknown;
                else if (managed_state == mount::sumh::ManagedMapsRestoreState::Replaced)
                    outcome.state = mount::sumh::FeatureApplyState::Partial;
                else
                    outcome.state = mount::sumh::FeatureApplyState::Failed;
            }
            if (!applied && error.empty())
                error = std::strerror(apply_errno);
            std::cout << "{\"saved\":" << (saving ? "true" : "false")
                      << ",\"applied\":" << (applied ? "true" : "false")
                      << ",\"count\":" << config.maps_rules.size() << ",\"runtime_result\":"
                      << json_quote(mount::sumh::feature_apply_state_name(outcome.state))
                      << ",\"detail\":" << json_quote(error) << "}\n";
            if (!applied) {
                std::cerr
                    << (saving ? "maps paths saved; apply failed: " : "maps apply failed: ")
                    << error
                    << "; query 'ksud sumhp sumh maps preview' before an explicit new apply\n";
                errno = apply_errno;
                return 1;
            }
            return 0;
        }
        if (op == "clear") {
            if (args.size() != 3) {
                std::cerr << "usage: ksud sumhp sumh maps clear\n";
                return 1;
            }
            if (!sumh::clear_maps_rules()) {
                std::cerr << "failed to clear SUMH maps rules\n";
                return 1;
            }
            return 0;
        }
        if (op == "add") {
            if (args.size() != 8) {
                std::cerr << "usage: ksud sumhp sumh maps add <target-ino> <target-dev> "
                             "<spoof-ino> <spoof-dev> <spoof-path>\n";
                return 1;
            }
            unsigned long target_ino = 0;
            unsigned long target_dev = 0;
            unsigned long spoof_ino = 0;
            unsigned long spoof_dev = 0;
            if (!parse_unsigned_long(args[3], target_ino) ||
                !parse_unsigned_long(args[4], target_dev) ||
                !parse_unsigned_long(args[5], spoof_ino) ||
                !parse_unsigned_long(args[6], spoof_dev)) {
                std::cerr << "SUMH maps values must be unsigned integers\n";
                return 1;
            }
            if (!sumh::add_maps_rule(target_ino, target_dev, spoof_ino, spoof_dev, args[7])) {
                std::cerr << "failed to add SUMH maps rule\n";
                return 1;
            }
            return 0;
        }
        std::cerr << "usage: ksud sumhp sumh maps "
                     "clear|add|list-paths|preview|add-path|remove-path|apply ...\n";
        return 1;
    }
    if (sub == "clear") {
        if (!sumh::clear_rules()) {
            std::cerr << "failed to clear SUMH rules\n";
            return 1;
        }
        mount::sumh::invalidate_active_state();
        return 0;
    }
    if (sub == "hide-path" || sub == "delete-rule") {
        const std::string path = arg_or_default(args, 2, "");
        if (path.empty() || path.front() != '/') {
            std::cerr << "SUMH path must be absolute\n";
            return 1;
        }
        const bool ok = sub == "hide-path" ? sumh::hide_path(path) : sumh::delete_rule(path);
        if (!ok) {
            std::cerr << "failed to update SUMH path rule\n";
            return 1;
        }
        return 0;
    }
    if (sub == "fix-mounts") {
        if (!sumh::fix_mounts()) {
            std::cerr << "failed to reorder SUMH mount ids\n";
            return 1;
        }
        return 0;
    }
    if (sub == "hide-overlay-xattrs") {
        const std::string path = arg_or_default(args, 2, "");
        if (path.empty() || path.front() != '/') {
            std::cerr << "sumh hide-overlay-xattrs requires an absolute path\n";
            return 1;
        }
        if (!sumh::hide_overlay_xattrs(path)) {
            std::cerr << "failed to hide overlay xattrs for " << path << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "mount-hide" || sub == "maps-spoof" || sub == "statfs-spoof") {
        const std::string value = arg_or_default(args, 2, "off");
        const bool on = value == "on" || value == "normal" || value == "aggressive";
        bool ok = false;
        if (sub == "mount-hide") {
            if (value != "off" && value != "on" && value != "normal" && value != "aggressive") {
                std::cerr << "usage: ksud sumhp sumh mount-hide off|normal|aggressive\n";
                return 1;
            }
            ok = sumh::set_mount_hide(on, value == "aggressive" ? sumh::MountHideMode::Aggressive
                                                                : sumh::MountHideMode::Normal);
        } else if (sub == "maps-spoof") {
            ok = sumh::set_maps_spoof(on);
        } else if (sub == "statfs-spoof") {
            ok = sumh::set_statfs_spoof(on);
        }
        if (!ok) {
            std::cerr << "failed to set SUMH " << sub << "\n";
            return 1;
        }
        return 0;
    }
    print_usage();
    return 1;
}

int handle_hide(const std::vector<std::string>& args) {
    if (hide::definition_command(args))
        return hide::run_definition_command(args);
    if (args.size() >= 2 && (args[1] == "add" || args[1] == "remove" || args[1] == "retry-path"))
        return hide::run_path_command(args);
    const auto sub = arg_or_default(args, 1, "");
    if (sub == "apply") {
        const bool automatic = args.size() == 3 && args[2] == "--automatic";
        if (args.size() != 2 && !automatic) {
            std::cerr << "usage: ksud sumhp hide apply\n";
            return 1;
        }
        Config config;
        std::string error;
        if (!read_config_file(config, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        if (!mount::sumh::restore_persisted_hide_rules(
                error, automatic ? RestoreTrigger::Automatic : RestoreTrigger::UserRequested)) {
            std::cerr << error << "\n";
            return 1;
        }
        return 0;
    }
    if (sub == "list") {
        std::vector<std::string> rules;
        std::string error;
        if (!load_user_hide_rules(rules, error)) {
            std::cerr << error << "\n";
            return 1;
        }
        print_string_array(rules);
        std::cout << "\n";
        return 0;
    }
    print_usage();
    return 1;
}

int handle_recovery(const std::vector<std::string>& args) {
    const auto sub = arg_or_default(args, 1, "status");
    if (sub == "reset") {
        std::string error;
        const bool ok = mount::recovery_reset(error);
        std::cout << "{\"ok\":" << (ok ? "true" : "false")
                  << ",\"action\":\"reset\",\"error\":" << json_quote(error) << "}\n";
        if (!ok)
            std::cerr << error << "\n";
        return ok ? 0 : 1;
    }
    if (sub == "status") {
        bool ok = false;
        std::cout << mount::recovery_status_json(&ok) << "\n";
        return ok ? 0 : 1;
    }
    std::cerr << "usage: ksud sumhp recovery status|reset\n";
    return 1;
}
}  // namespace

int run_command(const std::vector<std::string>& args) {
    if (!args.empty() && args[0] == "doctor") {
        std::cerr << "SUMH diagnostics require the exact local CLI route; daemon dispatch is not "
                     "supported\n";
        return 2;
    }
    if (maps_temporary::is_prefix(args) || temporary_maps::is_manager_prefix(args)) {
        std::cerr << "temporary Maps commands require the exact local CLI route; daemon dispatch "
                     "is not supported\n";
        return 2;
    }
    if (!args.empty() && args[0] == "profile") {
        std::cerr << "SUMH profiles require the exact local CLI route; daemon dispatch is not "
                     "supported\n";
        return 2;
    }
    if (args.size() >= 2 && args[0] == "api" && args[1] == "capabilities") {
        std::cerr << "capability queries require the exact local CLI route; daemon dispatch is not "
                     "supported\n";
        return 2;
    }
    if (args.size() >= 2 && args[0] == "hide" && hide::definition_query(args[1])) {
        std::cerr << "hide definition queries require the exact local CLI route; daemon dispatch "
                     "is not supported\n";
        return 2;
    }
    if (!args.empty() && args[0] == "operation") {
        std::cerr << "operation queries require the exact local CLI route; daemon dispatch is not "
                     "supported\n";
        return 2;
    }
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        print_usage();
        return args.empty() ? 1 : 0;
    }
    if (args[0] == "version" || args[0] == "--version") {
        std::cout << ksud::VERSION_NAME << "\n";
        return 0;
    }
    if (args[0] == "daemon") {
        return run_daemon_command(args);
    }
    if (args[0] == "config") {
        return handle_config(args);
    }
    if (args[0] == "api") {
        return handle_api(args);
    }
    if (args[0] == "module") {
        return handle_module(args);
    }
    if (args[0] == "sumh") {
        return handle_sumh(args);
    }
    if (args[0] == "debug") {
        if (args.size() >= 2 && (args[1] == "enable" || args[1] == "disable")) {
            return sumh::set_debug(args[1] == "enable") ? 0 : 1;
        }
        if (args.size() >= 3 && args[1] == "stealth") {
            return sumh::set_stealth(args[2] == "enable") ? 0 : 1;
        }
        return 0;
    }
    if (args[0] == "hide") {
        return handle_hide(args);
    }
    if (args[0] == "recovery") {
        return handle_recovery(args);
    }

    print_usage();
    return 1;
}

namespace {
bool is_query(const std::vector<std::string>& args) {
    if (args.empty())
        return true;
    const auto sub = arg_or_default(args, 1, "");
    return mount::is_development_prefix(args) || args[0] == "profile" || args[0] == "operation" ||
           args[0] == "api" || args[0] == "version" || args[0] == "help" ||
           (args[0] == "config" && sub == "show") ||
           (args[0] == "module" &&
            (sub == "list" || sub == "check-conflicts" || sub == "plan" || sub == "explain" ||
             sub == "manager-preview" || sub == "rebuild-status")) ||
           (args[0] == "hide" && (sub == "list" || hide::definition_query(sub))) ||
           (args[0] == "sumh" &&
            (sub == "version" || sub == "list" || sub == "features" || sub == "hooks" ||
             (sub == "maps" && arg_or_default(args, 2, "") == "list-paths") ||
             mount::sumh::is_maps_preview_command(args) ||
             (sub == "kernel-build" && arg_or_default(args, 2, "show") == "show"))) ||
           (args[0] == "recovery" && sub == "status");
}

std::string operation_description(const std::vector<std::string>& args) {
    if (args.empty())
        return "help";
    std::string result = args[0] + (args.size() > 1 ? " " + args[1] : "");
    if (args[0] == "config" && args.size() == 3 && args[1] == "merge-json") {
        JsonValue patch;
        std::string error;
        if (parse_json(args[2], patch, error) && patch.is_object()) {
            result += " keys=";
            const std::set<std::string> public_values = {"debug",
                                                         "verbose",
                                                         "enable_kernel_debug",
                                                         "enable_stealth",
                                                         "enable_mount_hide",
                                                         "mount_hide_mode",
                                                         "enable_maps_spoof",
                                                         "enable_statfs_spoof",
                                                         "enable_kernel_build_spoof",
                                                         "enable_overlay_xattr_hide",
                                                         "overlayfs_enabled",
                                                         "magic_mount_enabled",
                                                         "fs_type",
                                                         "mount_backend",
                                                         "mountsource"};
            for (const auto& item : patch.o) {
                result += item.first;
                if (public_values.count(item.first))
                    result += "=" + stringify_json(item.second);
                result += ",";
            }
        }
    } else if ((args[0] == "module" || args[0] == "hide") && args.size() > 2) {
        result += " target=" + json_quote(args[2]);
        if (args.size() > 3)
            result += " value=" + json_quote(args[3]);
        if (args.size() > 4 && args[1] != "configure")
            result += " mode=" + json_quote(args[4]);
    }
    return result;
}
}  // namespace

CommandResult run_command_capture(const std::vector<std::string>& args,
                                  const std::string& prepared_id,
                                  const std::string& request_revision, CaptureRoute route) {
    const bool local_maps = route == CaptureRoute::LocalTemporaryMaps;
    maps_temporary::DeleteRequest local_request;
    if (local_maps && (!state::valid_operation_id(prepared_id) || request_revision.empty() ||
                       !maps_temporary::parse_tracked_delete(args, local_request))) {
        CommandResult rejected;
        rejected.exit_code = 2;
        rejected.error_number = EINVAL;
        rejected.operation_id = prepared_id;
        rejected.operation_status = "rejected";
        rejected.stderr_text =
            "local temporary Maps execution requires a matching reservation and canonical delete\n";
        return rejected;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto operation = operation_description(args);
    const auto level = is_query(args) ? logging::Level::Debug : logging::Level::Info;
    logging::write(level, "command", "begin " + operation);
    const auto capture_limit = prepared_id.empty() ? std::numeric_limits<std::size_t>::max()
                                                   : state::OPERATION_OUTPUT_BYTES;
    CaptureBuffer stdout_buffer(capture_limit);
    CaptureBuffer stderr_buffer(capture_limit);
    auto* old_stdout = std::cout.rdbuf(&stdout_buffer);
    auto* old_stderr = std::cerr.rdbuf(&stderr_buffer);
    class RestoreStreams {
    public:
        RestoreStreams(std::streambuf* output, std::streambuf* errors) : out(output), err(errors) {}
        RestoreStreams(const RestoreStreams&) = delete;
        RestoreStreams& operator=(const RestoreStreams&) = delete;
        RestoreStreams(RestoreStreams&&) = delete;
        RestoreStreams& operator=(RestoreStreams&&) = delete;
        ~RestoreStreams() {
            std::cout.rdbuf(out);
            std::cerr.rdbuf(err);
        }

    private:
        std::streambuf* out;
        std::streambuf* err;
    };
    const RestoreStreams restore_streams{old_stdout, old_stderr};
    errno = 0;
    int exit_code;
    std::unique_ptr<state::Journal> journal;
    bool operation_recorded = false;
    CommandResult result;
    result.operation_id = prepared_id;
    result.operation_status = "not_started";
    try {
        bool may_run = prepared_id.empty();
        if (local_maps || state::command_needs_journal(args)) {
            auto candidate = std::make_unique<state::Journal>();
            std::string journal_error;
            const std::string name = args[0] + (args.size() > 1 ? " " + args[1] : "") +
                                     (args[0] == "sumh" && args.size() > 2 &&
                                              (args[1] == "maps" || args[1] == "kernel-build")
                                          ? " " + args[2]
                                          : "");
            if (prepared_id.empty())
                may_run = candidate->begin(name, journal_error);
            else {
                const auto status =
                    candidate->begin_prepared(prepared_id, name, request_revision, journal_error);
                may_run = status == state::ReservedStart::Started;
                switch (status) {
                case state::ReservedStart::Started:
                    result.operation_status = "started";
                    break;
                case state::ReservedStart::Existing:
                    result.operation_status = "already_started_or_finished";
                    break;
                case state::ReservedStart::NotRetained:
                    result.operation_status = "not_retained";
                    break;
                case state::ReservedStart::Conflict:
                    result.operation_status = "request_conflict";
                    break;
                case state::ReservedStart::StaleBoot:
                    result.operation_status = "boot_mismatch";
                    break;
                case state::ReservedStart::Failed:
                    result.operation_status = "journal_unavailable";
                    break;
                }
                if (const auto* observed = candidate->observed_operation())
                    result.observed_record_json =
                        stringify_json(state::operation_record_json(*observed, runtime_boot_id()));
            }
            if (may_run)
                journal = std::move(candidate);
            if (!may_run) {
                std::cerr << "operation not started: " << journal_error << '\n';
            }
        }
        // Ledger IO can leave ENOENT/EINTR behind after a successful begin;
        // only the command's own failure may populate its result errno.
        if (may_run)
            errno = 0;
        result.command_executed = may_run;
        if (!may_run)
            exit_code = 1;
        else if (local_maps)
            exit_code =
                maps_temporary::command(args, std::cout, std::cerr, journal->operation_id());
        else
            exit_code = run_command(args);
    } catch (const std::exception& error) {
        std::cerr << "SUMHP: " << error.what() << '\n';
        errno = EIO;
        exit_code = 1;
    }
    int error_number = exit_code == 0 ? 0 : (errno != 0 ? errno : EIO);
    result.command_exit_code = exit_code;
    result.command_errno = error_number;
    if (journal) {
        std::string journal_error;
        bool recorded = false;
        try {
            if (!journal->retain_resource(args, stdout_buffer.str(), stdout_buffer.complete(),
                                          exit_code, error_number, journal_error))
                std::cerr << "operation resource retention unconfirmed: " << journal_error << '\n';
        } catch (const std::exception&) {
            std::cerr << "operation resource retention unconfirmed\n";
        }
        try {
            const auto* detail = exit_code == 0
                                     ? "command_completed; inspect each resource state"
                                     : "command_failed_or_partial; inspect runtime before retry";
            recorded = local_maps ? journal->finish_record_only(exit_code, error_number, detail,
                                                                journal_error)
                                  : journal->finish(exit_code, error_number, detail, journal_error);
        } catch (const std::exception& failure) {
            journal_error = failure.what();
        }
        if (!recorded) {
            std::cerr << "operation result persistence unconfirmed: " << journal_error
                      << "; runtime/configuration may have changed; query api state before retry\n";
            exit_code = 1;
            error_number = EIO;
        }
        operation_recorded = recorded;
        if (!recorded)
            result.operation_status = "outcome_unknown";
        else
            result.operation_status =
                result.command_exit_code == 0 ? "command_completed" : "failed_or_partial";
    }
    std::cout.rdbuf(old_stdout);
    std::cerr.rdbuf(old_stderr);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    logging::write(exit_code == 0 ? level : logging::Level::Error, "command",
                   operation + " result=" + (exit_code == 0 ? "ok" : "failed") + " exit=" +
                       std::to_string(exit_code) + " errno=" + std::to_string(error_number) +
                       " elapsed_ms=" + std::to_string(elapsed));
    if (!stderr_buffer.str().empty())
        logging::write(exit_code == 0 ? logging::Level::Warning : logging::Level::Error, "command",
                       operation + ": " + stderr_buffer.str());
    result.exit_code = exit_code;
    result.error_number = error_number;
    result.stdout_text = stdout_buffer.str();
    result.stderr_text = stderr_buffer.str();
    if (journal)
        result.operation_id = journal->operation_id();
    result.operation_recorded = operation_recorded;
    result.stdout_complete = stdout_buffer.complete();
    result.stderr_complete = stderr_buffer.complete();
    return result;
}

}  // namespace sumhp
