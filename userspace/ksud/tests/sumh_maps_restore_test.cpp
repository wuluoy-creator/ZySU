// Exercise complete production path resolution without requiring a real
// Android mount namespace. A failed batch must never reach the replace call.
#include "core/json.hpp"
#include "sumhp/config.hpp"
#include "sumhp/sumh_client.hpp"
#include "uapi/sumh.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#define stat maps_test_stat
#undef S_ISREG
#define S_ISREG maps_test_is_regular
#undef major
#define major maps_test_major
#undef minor
#define minor maps_test_minor

struct stat {
    unsigned int st_mode = 1;
    std::uint64_t st_ino = 0;
    std::uint64_t st_dev = 0;
};
namespace {
std::map<std::string, struct stat> files;
bool namespace_ok = true;
bool replace_ok = true;
int replace_calls = 0;
int stat_calls = 0;
sumhp::sumh::FeatureCapabilities caps{true, 0, SUMH_FEATURE_MANAGED_MAPS};
std::vector<sumhp::sumh::MapsRule> installed{{99, 1, 88, 2, "/keep"}};
constexpr std::uint64_t host_dev(unsigned int device_major, unsigned int device_minor) {
    return (std::uint64_t(device_major) << 32) | device_minor;
}
}  // namespace
int stat(const char* path, struct stat* result) {
    ++stat_calls;
    const auto found = files.find(path);
    if (found == files.end()) {
        errno = ENOENT;
        return -1;
    }
    *result = found->second;
    return 0;
}
bool S_ISREG(unsigned int mode) {
    return mode == 1;
}
unsigned long major(std::uint64_t value) {
    return value >> 32;
}
unsigned long minor(std::uint64_t value) {
    return value & 0xffffffff;
}

namespace sumhp {
#include "sumh_maps_validate_under_test.inc"
}  // namespace sumhp
namespace sumhp::sumh {
FeatureCapabilities feature_capabilities() {
    return caps;
}
bool replace_managed_maps_rules(const std::vector<MapsRule>& rules) {
    ++replace_calls;
    if (!replace_ok) {
        errno = EIO;
        return false;
    }
    installed = rules;
    return true;
}
}  // namespace sumhp::sumh
namespace sumhp::mount::sumh {
namespace fsutil {
template <typename Callback>
bool run_in_init_mount_ns(Callback callback, std::string&) {
    return namespace_ok && callback();
}
}  // namespace fsutil
#include "sumh_maps_restore_under_test.inc"
}  // namespace sumhp::mount::sumh

int main() {
    using sumhp::mount::sumh::restore_managed_maps_rules;
    sumhp::Config config;
    std::string error;
    caps = {true, 0, 0};
    assert(restore_managed_maps_rules(config, error));
    assert(replace_calls == 0 && stat_calls == 0 && installed[0].target_ino == 99);
    config.maps_rules = {{"/target", "/replacement"}};
    assert(!restore_managed_maps_rules(config, error));
    assert(errno == EOPNOTSUPP && replace_calls == 0 && stat_calls == 0);
    config.maps_rules.clear();
    caps = {false, EACCES, 0};
    assert(!restore_managed_maps_rules(config, error));
    assert(errno == EACCES && replace_calls == 0);
    caps = {true, 0, SUMH_FEATURE_MANAGED_MAPS};
    config.maps_rules = {{"/target", "/replacement"}, {"/late", "/replacement"}};
    files["/target"] = {1, 123, host_dev(259, 4097)};
    files["/replacement"] = {1, 456, host_dev(8, 17)};
    assert(!restore_managed_maps_rules(config, error));
    assert(errno == ENOENT && replace_calls == 0 && installed[0].target_ino == 99);
    files["/late"] = {2, 789, host_dev(8, 1)};
    assert(!restore_managed_maps_rules(config, error));
    assert(errno == EINVAL && replace_calls == 0);
    files["/late"] = files["/target"];
    assert(!restore_managed_maps_rules(config, error));
    assert(errno == EEXIST && replace_calls == 0);
    files["/late"] = {1, 789, host_dev(8, 1)};
    namespace_ok = false;
    error.clear();
    assert(!restore_managed_maps_rules(config, error));
    assert(!error.empty() && replace_calls == 0);
    namespace_ok = true;
    replace_ok = false;
    assert(!restore_managed_maps_rules(config, error));
    assert(errno == EIO && replace_calls == 1 && installed[0].target_ino == 99);
    replace_ok = true;
    assert(restore_managed_maps_rules(config, error));
    assert(replace_calls == 2 && installed.size() == 2);
    assert(installed[0].target_ino == 123 && installed[0].target_dev == ((259UL << 20) | 4097));
    assert(installed[0].spoofed_ino == 456 && installed[0].spoofed_dev == ((8UL << 20) | 17));
    assert(installed[0].spoofed_path == "/replacement");
    files["/target"].st_ino = 321;
    assert(restore_managed_maps_rules(config, error));
    assert(installed[0].target_ino == 321 && replace_calls == 3);
    files["/target"].st_dev = host_dev(0x1000, 0);
    assert(!restore_managed_maps_rules(config, error));
    assert(errno == EOVERFLOW && replace_calls == 3);
    config.maps_rules.clear();
    assert(restore_managed_maps_rules(config, error));
    assert(replace_calls == 4 && installed.empty());
    std::cout << "sumh_maps_restore_test: all checks passed\n";
}
