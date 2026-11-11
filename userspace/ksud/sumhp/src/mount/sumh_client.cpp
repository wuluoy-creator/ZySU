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
static_assert(sizeof(sumh_managed_kstat_arg) == 24);
static_assert(offsetof(sumh_managed_kstat_arg, rules) == 8);
static_assert(sizeof(sumh_raw_kstat_arg) == 4192);
static_assert(offsetof(sumh_raw_kstat_arg, ino) == 4104);
static_assert(offsetof(sumh_raw_kstat_arg, blocks) == 4184);
static_assert(sizeof(sumh_managed_kstat_status_arg) == 24);
static_assert(offsetof(sumh_managed_kstat_status_arg, generation) == 8);
static_assert(sizeof(sumh_managed_mount_arg) == 4112);
static_assert(offsetof(sumh_managed_mount_arg, path) == 16);
// This native unsigned-long payload is an LP64 ABI; the Android controller is arm64-only.
#if defined(__ANDROID__)
static_assert(sizeof(unsigned long) == 8);
#endif
#if defined(__LP64__)
static_assert(sizeof(sumh_managed_kstat_rule) == 376);
static_assert(offsetof(sumh_managed_kstat_rule, field_mask) == 368);
#endif

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
    if (bitmask & SUMH_FEATURE_STATFS_SPOOF)
        names.emplace_back("statfs_spoof");
    if (bitmask & SUMH_FEATURE_KSTAT_SPOOF)
        names.emplace_back("kstat_spoof");
    if (bitmask & SUMH_FEATURE_MANAGED_KSTAT)
        names.emplace_back("managed_kstat");
    if (bitmask & SUMH_FEATURE_MANAGED_MOUNTS)
        names.emplace_back("managed_mounts");
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

bool fix_mounts() {
    return execute(SUMH_IOC_REORDER_MNT_ID, nullptr) == 0;
}

namespace {
bool update_managed_mount(const std::string& path, std::uint32_t operation) {
    if (path.size() >= SUMH_MANAGED_MOUNT_PATH_MAX) {
        errno = ENAMETOOLONG;
        return false;
    }
    if (path.size() < 2 || path.front() != '/' || path.back() == '/' ||
        path.find('\0') != std::string::npos || path.find_first_of("\r\n") != std::string::npos) {
        errno = EINVAL;
        return false;
    }
    for (std::size_t start = 1; start < path.size();) {
        const auto slash = path.find('/', start);
        const auto end = slash == std::string::npos ? path.size() : slash;
        const std::string_view component(path.data() + start, end - start);
        if (component.empty() || component == "." || component == "..") {
            errno = EINVAL;
            return false;
        }
        start = end + 1;
    }
    const auto caps = feature_capabilities();
    if (!caps.ok) {
        errno = caps.last_errno;
        return false;
    }
    if (!(caps.bitmask & SUMH_FEATURE_MANAGED_MOUNTS)) {
        errno = EOPNOTSUPP;
        return false;
    }
    sumh_managed_mount_arg arg = {};
    arg.size = sizeof(arg);
    arg.operation = operation;
    std::memcpy(arg.path, path.c_str(), path.size() + 1);
    return execute(SUMH_IOC_UPDATE_MANAGED_MOUNT, &arg) == 0;
}
}  // namespace

bool register_managed_mount(const std::string& path) {
    return update_managed_mount(path, SUMH_MANAGED_MOUNT_REGISTER);
}

bool unregister_managed_mount(const std::string& path) {
    return update_managed_mount(path, SUMH_MANAGED_MOUNT_UNREGISTER);
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

bool set_maps_spoof(bool enable) {
    sumh_maps_spoof_arg arg = {};
    arg.enable = enable ? 1 : 0;
    const int rc = execute(SUMH_IOC_SET_MAPS_SPOOF, &arg);
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

bool replace_managed_maps_rules(const std::vector<MapsRule>& rules) {
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
    return execute(SUMH_IOC_SET_MANAGED_MAPS, &arg) == 0;
}

bool replace_managed_kstat_rules(const std::vector<KstatRule>& rules) {
    if (rules.size() > SUMH_MANAGED_KSTAT_MAX_RULES) {
        errno = E2BIG;
        return false;
    }
    std::vector<sumh_managed_kstat_rule> encoded;
    encoded.reserve(rules.size());
    std::set<std::string> targets;
    for (const auto& rule : rules) {
        if (rule.target_path.empty() || rule.target_path.front() != '/' ||
            rule.target_path.find_first_of("\0\r\n", 0, 3) != std::string::npos ||
            !targets.insert(rule.target_path).second || !rule.field_mask ||
            (rule.field_mask & ~SUMH_KSTAT_FIELD_ALL) || rule.spoofed_dev > 0xffffffffULL ||
            rule.spoofed_blksize > 0xffffffffULL || rule.spoofed_size < 0 ||
            rule.spoofed_atime_nsec < 0 || rule.spoofed_atime_nsec >= 1000000000L ||
            rule.spoofed_mtime_nsec < 0 || rule.spoofed_mtime_nsec >= 1000000000L ||
            rule.spoofed_ctime_nsec < 0 || rule.spoofed_ctime_nsec >= 1000000000L) {
            errno = EINVAL;
            return false;
        }
        if (rule.target_path.size() >= SUMH_MAX_LEN_PATHNAME) {
            errno = ENAMETOOLONG;
            return false;
        }
        sumh_managed_kstat_rule item = {};
        auto& values = item.values;
        std::memcpy(values.target_pathname, rule.target_path.c_str(), rule.target_path.size() + 1);
        values.spoofed_ino = rule.spoofed_ino;
        values.spoofed_dev = rule.spoofed_dev;
        values.spoofed_nlink = rule.spoofed_nlink;
        values.spoofed_size = rule.spoofed_size;
        values.spoofed_atime_sec = rule.spoofed_atime_sec;
        values.spoofed_atime_nsec = rule.spoofed_atime_nsec;
        values.spoofed_mtime_sec = rule.spoofed_mtime_sec;
        values.spoofed_mtime_nsec = rule.spoofed_mtime_nsec;
        values.spoofed_ctime_sec = rule.spoofed_ctime_sec;
        values.spoofed_ctime_nsec = rule.spoofed_ctime_nsec;
        values.spoofed_blksize = rule.spoofed_blksize;
        values.spoofed_blocks = rule.spoofed_blocks;
        item.field_mask = rule.field_mask;
        encoded.push_back(item);
    }
    const auto caps = feature_capabilities();
    if (!caps.ok) {
        errno = caps.last_errno;
        return false;
    }
    if (!(caps.bitmask & SUMH_FEATURE_MANAGED_KSTAT)) {
        if (rules.empty())
            return true;
        errno = EOPNOTSUPP;
        return false;
    }
    sumh_managed_kstat_arg arg = {};
    arg.size = sizeof(arg);
    arg.count = static_cast<std::uint32_t>(encoded.size());
    arg.rules = encoded.empty() ? 0 : reinterpret_cast<std::uintptr_t>(encoded.data());
    return execute(SUMH_IOC_SET_MANAGED_KSTAT, &arg) == 0;
}

bool raw_kstat(const std::string& source_path, RawKstat& result) {
    if (source_path.empty() || source_path.front() != '/' ||
        source_path.find_first_of("\0\r\n", 0, 3) != std::string::npos) {
        errno = EINVAL;
        return false;
    }
    if (source_path.size() >= SUMH_USER_HIDE_PATH_MAX) {
        errno = ENAMETOOLONG;
        return false;
    }
    sumh_raw_kstat_arg arg = {};
    arg.size = sizeof(arg);
    std::memcpy(arg.source_path, source_path.c_str(), source_path.size() + 1);
    if (execute(SUMH_IOC_GET_RAW_KSTAT, &arg) != 0)
        return false;
    if (arg.size != sizeof(arg) || arg.reserved || arg.reserved2 || arg.atime_nsec < 0 ||
        arg.atime_nsec >= 1000000000 || arg.mtime_nsec < 0 || arg.mtime_nsec >= 1000000000 ||
        arg.ctime_nsec < 0 || arg.ctime_nsec >= 1000000000) {
        errno = EPROTO;
        return false;
    }
    result = {arg.ino,       arg.dev,        arg.nlink,     arg.size_bytes,
              arg.atime_sec, arg.atime_nsec, arg.mtime_sec, arg.mtime_nsec,
              arg.ctime_sec, arg.ctime_nsec, arg.blksize,   arg.blocks};
    return true;
}

bool managed_kstat_status(ManagedKstatStatus& result) {
    sumh_managed_kstat_status_arg arg = {};
    arg.size = sizeof(arg);
    if (execute(SUMH_IOC_GET_MANAGED_KSTAT_STATUS, &arg) != 0)
        return false;
    if (arg.size != sizeof(arg) || arg.count > SUMH_MANAGED_KSTAT_MAX_RULES || arg.reserved[0] ||
        arg.reserved[1]) {
        errno = EPROTO;
        return false;
    }
    result = {arg.count, arg.generation};
    return true;
}

int enabled_state() {
    int enabled = 0;
    return execute(SUMH_IOC_GET_ENABLED, &enabled) == 0 ? enabled : -1;
}

}  // namespace sumhp::sumh
