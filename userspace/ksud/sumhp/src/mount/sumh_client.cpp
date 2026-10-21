#include "sumhp/sumh_client.hpp"

#include "uapi/sumh.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#if defined(__linux__)
#include <sys/types.h>
#include <unistd.h>
#endif

namespace sumhp::sumh {

static_assert(sizeof(sumh_user_hide_arg) == 4160);
static_assert(offsetof(sumh_user_hide_arg, path) == 64);
static_assert(sizeof(sumh_kernel_build_arg) == 148);
static_assert(offsetof(sumh_kernel_build_arg, release) == 16);
static_assert(sizeof(sumh_managed_maps_arg) == 24);
static_assert(offsetof(sumh_managed_maps_arg, rules) == 8);
static_assert(sizeof(sumh_maps_delete_arg) == 40);
static_assert(offsetof(sumh_maps_delete_arg, target_ino) == 24);
static_assert(SUMH_IOC_DELETE_TEMPORARY_MAPS == 0x40285335UL);
static_assert(sizeof(sumh_runtime_feature_state) == 32);
static_assert(sizeof(sumh_runtime_state_arg) == 240);
static_assert(offsetof(sumh_runtime_state_arg, entries) == 48);
static_assert(sizeof(sumh_runtime_set_arg) == 48);
static_assert(sizeof(sumh_kernel_build_cas_arg) == 168);
static_assert(offsetof(sumh_kernel_build_cas_arg, release) == 32);
static_assert(offsetof(sumh_kernel_build_cas_arg, version_string) == 97);
static_assert(sizeof(sumh_rule_inventory_entry) == 8272);
static_assert(sizeof(sumh_rule_inventory_arg) == 8352);
static_assert(offsetof(sumh_rule_inventory_arg, entry) == 80);
static_assert(SUMH_IOC_GET_RULE_INVENTORY == 0xe0a05334UL);

static_assert(static_cast<std::uint32_t>(MountHideMode::Normal) == SUMH_MOUNT_HIDE_MODE_NORMAL);
static_assert(static_cast<std::uint32_t>(MountHideMode::Aggressive) ==
              SUMH_MOUNT_HIDE_MODE_AGGRESSIVE);

extern "C" int ksu_sumh_ioctl(unsigned long cmd, void* arg);

namespace {
int execute(unsigned long cmd, void* arg) {
    return ksu_sumh_ioctl(cmd, arg);
}

bool ioctl_arg_ok(int rc, int arg_err) {
    if (rc != 0) {
        return false;
    }
    if (arg_err != 0) {
        errno = arg_err < 0 ? -arg_err : arg_err;
        return false;
    }
    return true;
}

std::string read_text(unsigned long cmd, std::size_t capacity) {
    std::string buffer(capacity, '\0');
    sumh_syscall_list_arg arg = {};
    arg.buf = buffer.data();
    arg.size = buffer.size();
    if (execute(cmd, &arg) != 0) {
        return {};
    }
    // These ioctls return a byte count, excluding a terminating NUL. Do not
    // depend on spare zero-filled bytes when a kernel fills the entire buffer.
    if (arg.size > buffer.size()) {
        errno = EPROTO;
        return {};
    }
    buffer.resize(arg.size);
    return buffer;
}
}  // namespace

VersionInfo version_info() {
    VersionInfo info;
    info.expected_protocol = SUMH_PROTOCOL_VERSION;
    info.process_uid = process_uid();
    info.process_euid = process_euid();

    int version = 0;
    if (execute(SUMH_IOC_GET_VERSION, &version) != 0) {
        info.last_errno = errno;
        info.status = Status::NotPresent;
        return info;
    }

    info.kernel_protocol = version;
    if (version < SUMH_PROTOCOL_VERSION) {
        info.status = Status::KernelTooOld;
    } else if (version > SUMH_PROTOCOL_VERSION) {
        info.status = Status::ClientTooOld;
    } else {
        info.status = Status::Available;
    }
    return info;
}

int process_uid() {
#if defined(__linux__)
    return static_cast<int>(getuid());
#else
    return -1;
#endif
}

int process_euid() {
#if defined(__linux__)
    return static_cast<int>(geteuid());
#else
    return -1;
#endif
}

bool is_available() {
    return version_info().status == Status::Available;
}

std::string active_rules() {
    return read_text(SUMH_IOC_LIST_RULES, 64UL * 1024);
}

std::string hooks() {
    return read_text(SUMH_IOC_GET_HOOKS, 8UL * 1024);
}

FeatureCapabilities feature_capabilities() {
    FeatureCapabilities capabilities;
    int bitmask = 0;
    if (execute(SUMH_IOC_GET_FEATURES, &bitmask) != 0) {
        capabilities.last_errno = errno != 0 ? errno : EIO;
        return capabilities;
    }
    capabilities.ok = true;
    capabilities.bitmask = bitmask;
    return capabilities;
}

int features() {
    return feature_capabilities().bitmask;
}

bool runtime_state(RuntimeState& state) {
    sumh_runtime_state_arg arg{};
    arg.size = sizeof(arg);
    arg.version = 1;
    if (execute(SUMH_IOC_GET_RUNTIME_STATE, &arg) != 0)
        return false;
    const auto invalid = [] {
        errno = EPROTO;
        return false;
    };
    constexpr std::uint32_t snapshot_flags = 3;  // Serialized control state; sampled readiness.
    if (arg.size != sizeof(arg) || arg.version != 1 || arg.count != SUMH_RUNTIME_STATE_COUNT ||
        !arg.generation || !arg.instance_id || arg.flags != snapshot_flags ||
        arg.mount_hide_mode > SUMH_MOUNT_HIDE_MODE_AGGRESSIVE ||
        std::any_of(std::begin(arg.reserved), std::end(arg.reserved),
                    [](auto value) { return value != 0; }))
        return invalid();
    RuntimeState parsed;
    parsed.generation = arg.generation;
    parsed.instance_id = arg.instance_id;
    parsed.flags = arg.flags;
    parsed.mount_hide_mode = arg.mount_hide_mode;
    std::set<std::uint32_t> ids;
    for (const auto& item : arg.entries) {
        if (item.id < SUMH_RUNTIME_DEBUG || item.id > SUMH_RUNTIME_KERNEL_BUILD ||
            !ids.insert(item.id).second || item.supported > 1 || item.configured > 1 ||
            item.active > 1 || item.degraded > 1 || item.flags || item.reserved ||
            item.last_error > 0 || item.last_error < -4095 ||
            (item.active && (!item.supported || !item.configured)) ||
            item.degraded != (item.configured && !item.active))
            return invalid();
        parsed.entries.push_back({static_cast<RuntimeFeature>(item.id), item.supported != 0,
                                  item.configured != 0, item.active != 0, item.degraded != 0,
                                  item.last_error});
    }
    state = std::move(parsed);
    return true;
}

bool set_runtime_feature(const RuntimeState& expected, RuntimeFeature id, bool enable,
                         std::uint32_t mount_hide_mode) {
    if (!expected.generation || !expected.instance_id || id < RuntimeFeature::Debug ||
        id > RuntimeFeature::StatfsSpoof || mount_hide_mode != SUMH_MOUNT_HIDE_MODE_NORMAL) {
        errno = EINVAL;
        return false;
    }
    sumh_runtime_set_arg arg{};
    arg.size = sizeof(arg);
    arg.version = 1;
    arg.id = static_cast<std::uint32_t>(id);
    arg.enable = enable;
    arg.expected_generation = expected.generation;
    arg.expected_instance_id = expected.instance_id;
    arg.mount_hide_mode = mount_hide_mode;
    return execute(SUMH_IOC_SET_RUNTIME_FEATURE, &arg) == 0;
}

bool rule_inventory(const InventoryRequest& request, InventoryPage& page) {
    if (request.category > SUMH_INVENTORY_VNODE ||
        (!!request.instance_id != !!request.generation) ||
        (request.cursor && (!request.instance_id || !request.category))) {
        errno = EINVAL;
        return false;
    }
    sumh_rule_inventory_arg arg{};
    arg.size = sizeof(arg);
    arg.version = SUMH_RULE_INVENTORY_VERSION;
    arg.category = request.category;
    arg.expected_instance_id = request.instance_id;
    arg.expected_generation = request.generation;
    arg.cursor = request.cursor;
    if (execute(SUMH_IOC_GET_RULE_INVENTORY, &arg) != 0)
        return false;
    const auto invalid = [] {
        errno = EPROTO;
        return false;
    };
    if (arg.size != sizeof(arg) || arg.version != SUMH_RULE_INVENTORY_VERSION ||
        arg.category != request.category || arg.cursor != request.cursor ||
        arg.expected_instance_id || arg.expected_generation || arg.reserved || !arg.instance_id ||
        !arg.generation || arg.count > 1 || arg.complete > 1 ||
        arg.flags != SUMH_INVENTORY_DEFINITIONS_COHERENT ||
        (request.instance_id &&
         (arg.instance_id != request.instance_id || arg.generation != request.generation)) ||
        (arg.count && arg.cursor == UINT64_MAX) || arg.next_cursor != arg.cursor + arg.count ||
        (!arg.count && !arg.complete) || (!arg.category && arg.count) ||
        (arg.category && !(arg.supported_categories & (1U << (arg.category - 1)))))
        return invalid();
    InventoryPage parsed;
    parsed.category = arg.category;
    parsed.supported_categories = arg.supported_categories;
    parsed.instance_id = arg.instance_id;
    parsed.generation = arg.generation;
    parsed.cursor = arg.cursor;
    parsed.next_cursor = arg.next_cursor;
    parsed.at_end = arg.complete;
    const auto& item = arg.entry;
    if (!arg.count) {
        const sumh_rule_inventory_entry empty{};
        if (std::memcmp(&item, &empty, sizeof(item)) != 0)
            return invalid();
    } else {
        const bool maps = arg.category == SUMH_INVENTORY_MAPS_MANAGED ||
                          arg.category == SUMH_INVENTORY_MAPS_TEMPORARY;
        const bool managed_hide = arg.category == SUMH_INVENTORY_USER_HIDE;
        const bool mapping = arg.category == SUMH_INVENTORY_MAPPING;
        const bool merge = arg.category == SUMH_INVENTORY_MERGE;
        auto owner = SUMH_INVENTORY_OWNER_UNATTRIBUTED;
        if (managed_hide)
            owner = SUMH_INVENTORY_OWNER_USER_HIDE;
        else if (arg.category == SUMH_INVENTORY_MAPS_MANAGED)
            owner = SUMH_INVENTORY_OWNER_MANAGED_MAPS;
        const auto id_kind = managed_hide ? SUMH_INVENTORY_ID_MANAGED_DEFINITION
                                          : SUMH_INVENTORY_ID_GENERATION_ORDINAL;
        const auto valid_path = [](const char* path, std::uint32_t length) {
            if (length >= SUMH_RULE_INVENTORY_PATH_MAX || std::memchr(path, 0, length))
                return false;
            return std::all_of(path + length, path + SUMH_RULE_INVENTORY_PATH_MAX,
                               [](char byte) { return byte == 0; });
        };
        if (arg.category > SUMH_INVENTORY_USER_HIDE || item.category != arg.category ||
            item.reserved || item.owner != static_cast<std::uint32_t>(owner) ||
            item.id_kind != static_cast<std::uint32_t>(id_kind) || !item.id ||
            (managed_hide ? !item.definition_generation
                          : (item.id != arg.cursor + 1 || item.definition_generation != 0)) ||
            (!mapping && item.type) ||
            (item.flags &
             ~(arg.category == SUMH_INVENTORY_LEGACY_HIDE ? SUMH_INVENTORY_LEGACY_STORAGE_MANAGED
                                                          : 0U)) ||
            !valid_path(item.path1, item.path1_len) || !valid_path(item.path2, item.path2_len) ||
            (maps && item.path1_len) || (!maps && !mapping && !merge && item.path2_len) ||
            (!maps && (item.target_ino || item.target_dev || item.spoofed_ino || item.spoofed_dev)))
            return invalid();
        InventoryEntry entry;
        entry.category = item.category;
        entry.owner = item.owner;
        entry.id_kind = item.id_kind;
        entry.type = item.type;
        entry.flags = item.flags;
        entry.id = item.id;
        entry.definition_generation = item.definition_generation;
        entry.target_ino = item.target_ino;
        entry.target_dev = item.target_dev;
        entry.spoofed_ino = item.spoofed_ino;
        entry.spoofed_dev = item.spoofed_dev;
        entry.path1.assign(item.path1, item.path1_len);
        entry.path2.assign(item.path2, item.path2_len);
        parsed.entries.push_back(std::move(entry));
    }
    page = std::move(parsed);
    return true;
}

std::vector<std::string> feature_names(int bitmask) {
    std::vector<std::string> names;
    if (bitmask & SUMH_FEATURE_MANAGED_HIDE)
        names.emplace_back("managed_hide");
    if (bitmask & SUMH_FEATURE_MOUNT_HIDE)
        names.emplace_back("mount_hide");
    if (bitmask & SUMH_FEATURE_MAPS_SPOOF)
        names.emplace_back("maps_spoof");
    if (bitmask & SUMH_FEATURE_MANAGED_MAPS)
        names.emplace_back("managed_maps");
    if (bitmask & SUMH_FEATURE_TEMPORARY_MAPS_DELETE)
        names.emplace_back("temporary_maps_delete");
    if (bitmask & SUMH_FEATURE_STATFS_SPOOF)
        names.emplace_back("statfs_spoof");
    if (bitmask & SUMH_FEATURE_KSTAT_SPOOF)
        names.emplace_back("kstat_spoof");
    if (bitmask & SUMH_FEATURE_MERGE_DIR)
        names.emplace_back("merge_dir");
    if (bitmask & SUMH_FEATURE_OVERLAY_XATTR_HIDE)
        names.emplace_back("overlay_xattr_hide");
    if (bitmask & SUMH_FEATURE_FAKE_MOUNTINFO)
        names.emplace_back("fake_mountinfo");
    if (bitmask & SUMH_FEATURE_MOUNT_HIDE_AGGRESSIVE)
        names.emplace_back("mount_hide_aggressive");
    if (bitmask & SUMH_FEATURE_KERNEL_BUILD_SPOOF)
        names.emplace_back("kernel_build_spoof");
    if (bitmask & SUMH_FEATURE_RUNTIME_STATE)
        names.emplace_back("runtime_state");
    if (bitmask & SUMH_FEATURE_RUNTIME_SET)
        names.emplace_back("runtime_set");
    if (bitmask & SUMH_FEATURE_KERNEL_BUILD_CAS)
        names.emplace_back("kernel_build_cas");
    if (bitmask & SUMH_FEATURE_RULE_INVENTORY)
        names.emplace_back("rule_inventory");
    return names;
}

namespace {
bool hide_request_path(sumh_user_hide_arg& arg, const std::string& path) {
    if (path.empty() || path.front() != '/' || path.find('\0') != std::string::npos) {
        errno = EINVAL;
        return false;
    }
    if (path.size() >= sizeof(arg.path)) {
        errno = ENAMETOOLONG;
        return false;
    }
    arg.size = sizeof(arg);
    arg.path_len = static_cast<std::uint32_t>(path.size());
    std::memcpy(arg.path, path.c_str(), path.size() + 1);
    return true;
}

bool hide_reply(const sumh_user_hide_arg& arg, UserHideRule& result) {
    if (arg.size != sizeof(arg) || arg.path_len >= sizeof(arg.path) || arg.path[arg.path_len] ||
        std::strlen(arg.path) != arg.path_len) {
        errno = EPROTO;
        return false;
    }
    result = {arg.rule_id, arg.generation, arg.management,
              arg.binding, arg.error,      std::string(arg.path, arg.path_len)};
    return true;
}
}  // namespace

bool user_hide_rules(std::vector<UserHideRule>& rules) {
    std::vector<UserHideRule> snapshot;
    std::uint64_t cursor = 0;
    for (;;) {
        sumh_user_hide_arg arg = {};
        arg.size = sizeof(arg);
        arg.rule_id = cursor;
        if (execute(SUMH_IOC_USER_HIDE_QUERY, &arg))
            return false;
        UserHideRule rule;
        if (!hide_reply(arg, rule))
            return false;
        if (!rule.id) {
            rules = std::move(snapshot);
            return true;
        }
        if (rule.id <= cursor || snapshot.size() >= 4096) {
            errno = EOVERFLOW;
            return false;
        }
        cursor = rule.id;
        snapshot.push_back(std::move(rule));
    }
}

bool upsert_user_hide(const std::string& path, UserHideRule* result) {
    sumh_user_hide_arg arg = {};
    if (!hide_request_path(arg, path) || execute(SUMH_IOC_USER_HIDE_UPSERT, &arg))
        return false;
    UserHideRule reply;
    if (!hide_reply(arg, reply))
        return false;
    if (result)
        *result = std::move(reply);
    return true;
}

bool delete_user_hide(const std::string& path, std::uint64_t id) {
    sumh_user_hide_arg arg = {};
    arg.rule_id = id;
    return hide_request_path(arg, path) && !execute(SUMH_IOC_USER_HIDE_DELETE, &arg);
}

bool managed_hide_mode(bool& managed) {
    const auto caps = feature_capabilities();
    if (!caps.ok) {
        errno = caps.last_errno;
        return false;
    }
    if (caps.bitmask & SUMH_FEATURE_MANAGED_HIDE) {
        managed = true;
        return true;
    }
    std::vector<UserHideRule> rules;
    if (user_hide_rules(rules)) {
        managed = !rules.empty();
        return true;
    }
    if (errno != EINVAL && errno != ENOTTY && errno != EOPNOTSUPP)
        return false;
    managed = false;
    return true;
}

bool retry_user_hide(const std::string& path) {
    std::vector<UserHideRule> rules;
    if (!user_hide_rules(rules))
        return false;
    for (const auto& rule : rules) {
        if (rule.path != path)
            continue;
        sumh_user_hide_arg arg = {};
        arg.rule_id = rule.id;
        return hide_request_path(arg, path) && !execute(SUMH_IOC_USER_HIDE_RETRY, &arg);
    }
    errno = ENOENT;
    return false;
}

std::vector<std::string> active_modules_from_rules(const std::string& rules) {
    constexpr char prefix[] = "/data/adb/modules/";
    constexpr std::size_t prefix_size = sizeof(prefix) - 1;
    // Views stay inside rules until the final, sorted result is materialized.
    // In particular, repeated module names need no temporary string allocation.
    std::set<std::string_view> modules;
    std::string_view remaining(rules);
    while (!remaining.empty()) {
        const auto line_end = remaining.find('\n');
        const auto line = remaining.substr(0, line_end);
        std::size_t pos = line.find(prefix);
        while (pos != std::string_view::npos) {
            const std::size_t start = pos + prefix_size;
            const std::size_t end = line.find('/', start);
            if (end != std::string_view::npos && end > start) {
                modules.insert(line.substr(start, end - start));
            }
            pos = line.find(prefix, start);
        }
        if (line_end == std::string_view::npos)
            break;
        remaining.remove_prefix(line_end + 1);
    }
    std::vector<std::string> result;
    result.reserve(modules.size());
    for (const auto module : modules)
        result.emplace_back(module);
    return result;
}

bool set_debug(bool enable) {
    int value = enable ? 1 : 0;
    return execute(SUMH_IOC_SET_DEBUG, &value) == 0;
}

bool set_stealth(bool enable) {
    int value = enable ? 1 : 0;
    return execute(SUMH_IOC_SET_STEALTH, &value) == 0;
}

bool fix_mounts() {
    return execute(SUMH_IOC_REORDER_MNT_ID, nullptr) == 0;
}

bool hide_overlay_xattrs(const std::string& path) {
    sumh_syscall_arg arg = {};
    arg.src = path.c_str();
    return execute(SUMH_IOC_HIDE_OVERLAY_XATTRS, &arg) == 0;
}

bool clear_overlay_xattr_hiding() {
    const auto caps = feature_capabilities();
    if (!caps.ok) {
        errno = caps.last_errno;
        return false;
    }
    if (!(caps.bitmask & SUMH_FEATURE_OVERLAY_XATTR_HIDE))
        return true;
    return hide_overlay_xattrs("");
}

bool set_mount_hide(bool enable, MountHideMode mode) {
    const auto caps = feature_capabilities();
    if (!caps.ok) {
        errno = caps.last_errno;
        return false;
    }
    const bool mode_supported = (caps.bitmask & SUMH_FEATURE_MOUNT_HIDE_AGGRESSIVE) != 0;

    if (enable && mode == MountHideMode::Aggressive && !mode_supported) {
        errno = EOPNOTSUPP;
        return false;
    }
    if (mode_supported) {
        int raw_mode = static_cast<int>(mode);
        if (execute(SUMH_IOC_SET_MOUNT_HIDE_MODE, &raw_mode) != 0)
            return false;
    }
    sumh_mount_hide_arg arg = {};
    arg.enable = enable ? 1 : 0;
    const int rc = execute(SUMH_IOC_SET_MOUNT_HIDE, &arg);
    return ioctl_arg_ok(rc, arg.err);
}

bool set_maps_spoof(bool enable) {
    sumh_maps_spoof_arg arg = {};
    arg.enable = enable ? 1 : 0;
    const int rc = execute(SUMH_IOC_SET_MAPS_SPOOF, &arg);
    return ioctl_arg_ok(rc, arg.err);
}

bool set_statfs_spoof(bool enable) {
    sumh_statfs_spoof_arg arg = {};
    arg.enable = enable ? 1 : 0;
    const int rc = execute(SUMH_IOC_SET_STATFS_SPOOF, &arg);
    return ioctl_arg_ok(rc, arg.err);
}

namespace {
bool valid_kernel_build_field(const std::string& value, bool required) {
    if (value.size() >= SUMH_KERNEL_BUILD_MAX) {
        errno = ENAMETOOLONG;
        return false;
    }
    if ((required && value.empty()) ||
        std::any_of(value.begin(), value.end(),
                    [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; })) {
        errno = EINVAL;
        return false;
    }
    return true;
}
}  // namespace

bool set_kernel_build(bool enable, const std::string& release, const std::string& version) {
    if (enable &&
        (!valid_kernel_build_field(release, false) || !valid_kernel_build_field(version, false)))
        return false;
    if (enable &&
        ((release.empty() && version.empty()) || release.find(' ') != std::string::npos)) {
        errno = EINVAL;
        return false;
    }
    const auto caps = feature_capabilities();
    if (!caps.ok) {
        errno = caps.last_errno;
        return false;
    }
    if (!(caps.bitmask & SUMH_FEATURE_KERNEL_BUILD_SPOOF)) {
        // An additive feature must not break defaults on existing kernels.
        if (!enable)
            return true;
        errno = EOPNOTSUPP;
        return false;
    }
    sumh_kernel_build_arg arg = {};
    arg.size = sizeof(arg);
    arg.enable = enable ? 1 : 0;
    if (enable) {
        KernelBuild original;
        if ((release.empty() || version.empty()) && !original_kernel_build(original))
            return false;
        const auto& resolved_release = release.empty() ? original.release : release;
        const auto& resolved_version = version.empty() ? original.version : version;
        std::memcpy(arg.release, resolved_release.c_str(), resolved_release.size() + 1);
        std::memcpy(arg.version, resolved_version.c_str(), resolved_version.size() + 1);
    }
    return execute(SUMH_IOC_SET_KERNEL_BUILD, &arg) == 0;
}

namespace {
bool read_kernel_build(unsigned long command, KernelBuild& state) {
    sumh_kernel_build_arg arg = {};
    arg.size = sizeof(arg);
    if (execute(command, &arg) != 0)
        return false;
    if (arg.size != sizeof(arg) || arg.enable > 1 ||
        !std::memchr(arg.release, '\0', sizeof(arg.release)) ||
        !std::memchr(arg.version, '\0', sizeof(arg.version))) {
        errno = EPROTO;
        return false;
    }
    KernelBuild result{arg.enable != 0, arg.release, arg.version};
    if (!valid_kernel_build_field(result.release, true) ||
        !valid_kernel_build_field(result.version, true)) {
        errno = EPROTO;
        return false;
    }
    state = std::move(result);
    return true;
}
}  // namespace

bool kernel_build(KernelBuild& state) {
    return read_kernel_build(SUMH_IOC_GET_KERNEL_BUILD, state);
}

bool original_kernel_build(KernelBuild& state) {
    return read_kernel_build(SUMH_IOC_GET_ORIGINAL_KERNEL_BUILD, state);
}

bool kernel_build_snapshot(KernelBuildSnapshot& snapshot, bool include_original) {
    KernelBuildSnapshot parsed;
    RuntimeState after;
    if (!runtime_state(parsed.runtime) || !kernel_build(parsed.current) ||
        (include_original && !original_kernel_build(parsed.original)) || !runtime_state(after))
        return false;
    if (parsed.runtime.instance_id != after.instance_id ||
        parsed.runtime.generation != after.generation) {
        errno = ESTALE;
        return false;
    }
    const auto* feature = after.find(RuntimeFeature::KernelBuild);
    if (!feature || feature->configured != parsed.current.enabled ||
        (include_original && parsed.original.enabled != parsed.current.enabled)) {
        errno = EPROTO;
        return false;
    }
    parsed.runtime = std::move(after);
    parsed.has_original = include_original;
    snapshot = std::move(parsed);
    return true;
}

bool set_kernel_build_conditional(const RuntimeState& expected, const KernelBuild& desired) {
    if (!expected.generation || !expected.instance_id) {
        errno = EINVAL;
        return false;
    }
    if (desired.enabled && (!valid_kernel_build_field(desired.release, true) ||
                            !valid_kernel_build_field(desired.version, true)))
        return false;
    sumh_kernel_build_cas_arg arg{};
    arg.size = sizeof(arg);
    arg.version = SUMH_RUNTIME_STATE_VERSION;
    arg.enable = desired.enabled;
    arg.expected_generation = expected.generation;
    arg.expected_instance_id = expected.instance_id;
    if (desired.enabled) {
        std::memcpy(arg.release, desired.release.c_str(), desired.release.size() + 1);
        std::memcpy(arg.version_string, desired.version.c_str(), desired.version.size() + 1);
    }
    return execute(SUMH_IOC_SET_KERNEL_BUILD_CAS, &arg) == 0;
}

bool clear_rules() {
    return execute(SUMH_IOC_CLEAR_ALL, nullptr) == 0;
}

bool add_rule(const std::string& target, const std::string& source, int type) {
    sumh_syscall_arg arg = {};
    arg.src = target.c_str();
    arg.target = source.c_str();
    arg.type = type;
    return execute(SUMH_IOC_ADD_RULE, &arg) == 0;
}

bool add_merge_rule(const std::string& target, const std::string& source) {
    sumh_syscall_arg arg = {};
    arg.src = target.c_str();
    arg.target = source.c_str();
    return execute(SUMH_IOC_ADD_MERGE_RULE, &arg) == 0;
}

bool hide_path(const std::string& path) {
    sumh_syscall_arg arg = {};
    arg.src = path.c_str();
    return execute(SUMH_IOC_HIDE_RULE, &arg) == 0;
}

bool delete_rule(const std::string& path) {
    sumh_syscall_arg arg = {};
    arg.src = path.c_str();
    return execute(SUMH_IOC_DEL_RULE, &arg) == 0;
}

bool add_maps_rule(unsigned long target_ino, unsigned long target_dev, unsigned long spoofed_ino,
                   unsigned long spoofed_dev, const std::string& spoofed_path) {
    sumh_maps_rule arg = {};
    if (spoofed_path.find('\0') != std::string::npos) {
        errno = EINVAL;
        return false;
    }
    if (spoofed_path.size() >= sizeof(arg.spoofed_pathname)) {
        errno = ENAMETOOLONG;
        return false;
    }
    arg.target_ino = target_ino;
    arg.target_dev = target_dev;
    arg.spoofed_ino = spoofed_ino;
    arg.spoofed_dev = spoofed_dev;
    std::memcpy(arg.spoofed_pathname, spoofed_path.c_str(), spoofed_path.size() + 1);
    const int rc = execute(SUMH_IOC_ADD_MAPS_RULE, &arg);
    return ioctl_arg_ok(rc, arg.err);
}

bool clear_maps_rules() {
    return execute(SUMH_IOC_CLEAR_MAPS_RULES, nullptr) == 0;
}

bool delete_temporary_maps_rule(std::uint64_t target_ino, std::uint64_t target_dev,
                                std::uint64_t instance_id, std::uint64_t generation,
                                bool& attempted) {
    attempted = false;
    if (!instance_id || !generation || target_dev > UINT32_MAX) {
        errno = EINVAL;
        return false;
    }
    const auto caps = feature_capabilities();
    if (!caps.ok) {
        errno = caps.last_errno;
        return false;
    }
    constexpr int required = SUMH_FEATURE_TEMPORARY_MAPS_DELETE | SUMH_FEATURE_RULE_INVENTORY;
    if ((caps.bitmask & required) != required) {
        errno = EOPNOTSUPP;
        return false;
    }
    sumh_maps_delete_arg arg{};
    arg.size = sizeof(arg);
    arg.version = SUMH_MAPS_DELETE_VERSION;
    arg.expected_instance_id = instance_id;
    arg.expected_generation = generation;
    arg.target_ino = target_ino;
    arg.target_dev = target_dev;
    attempted = true;
    errno = 0;
    if (execute(SUMH_IOC_DELETE_TEMPORARY_MAPS, &arg) == 0)
        return true;
    if (!errno)
        errno = EIO;
    return false;
}

bool replace_managed_maps_rules_checked(const std::vector<MapsRule>& rules, bool& submitted) {
    submitted = false;
    if (rules.size() > SUMH_MANAGED_MAPS_MAX_RULES) {
        errno = E2BIG;
        return false;
    }
    std::vector<sumh_maps_rule> encoded;
    encoded.reserve(rules.size());
    std::set<std::pair<unsigned long, unsigned long>> targets;
    for (const auto& rule : rules) {
        if (!rule.target_ino || !rule.target_dev || !rule.spoofed_ino || !rule.spoofed_dev ||
            rule.spoofed_path.empty() || rule.spoofed_path.front() != '/' ||
            rule.spoofed_path.find('\0') != std::string::npos ||
            rule.spoofed_path.find_first_of("\r\n") != std::string::npos ||
            !targets.emplace(rule.target_ino, rule.target_dev).second) {
            errno = EINVAL;
            return false;
        }
        if (rule.spoofed_path.size() >= SUMH_MAX_LEN_PATHNAME) {
            errno = ENAMETOOLONG;
            return false;
        }
        sumh_maps_rule item = {};
        item.target_ino = rule.target_ino;
        item.target_dev = rule.target_dev;
        item.spoofed_ino = rule.spoofed_ino;
        item.spoofed_dev = rule.spoofed_dev;
        std::memcpy(item.spoofed_pathname, rule.spoofed_path.c_str(), rule.spoofed_path.size() + 1);
        encoded.push_back(item);
    }
    const auto caps = feature_capabilities();
    if (!caps.ok) {
        errno = caps.last_errno;
        return false;
    }
    if (!(caps.bitmask & SUMH_FEATURE_MANAGED_MAPS)) {
        if (rules.empty())
            return true;
        errno = EOPNOTSUPP;
        return false;
    }
    sumh_managed_maps_arg arg = {};
    arg.size = sizeof(arg);
    arg.count = static_cast<std::uint32_t>(encoded.size());
    arg.rules = encoded.empty() ? 0 : reinterpret_cast<std::uintptr_t>(encoded.data());
    submitted = execute(SUMH_IOC_SET_MANAGED_MAPS, &arg) == 0;
    return submitted;
}

bool replace_managed_maps_rules(const std::vector<MapsRule>& rules) {
    bool submitted = false;
    return replace_managed_maps_rules_checked(rules, submitted);
}

int enabled_state() {
    int enabled = 0;
    return execute(SUMH_IOC_GET_ENABLED, &enabled) == 0 ? enabled : -1;
}

}  // namespace sumhp::sumh
