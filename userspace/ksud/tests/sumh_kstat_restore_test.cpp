// Snapshot all source metadata before submitting a managed Kstat batch.
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

namespace {
std::map<std::string, sumhp::sumh::RawKstat> files;
bool namespace_ok = true;
bool replace_ok = true;
int replace_calls = 0;
int raw_calls = 0;
sumhp::sumh::FeatureCapabilities caps{true, 0, SUMH_FEATURE_MANAGED_KSTAT};
std::vector<sumhp::sumh::KstatRule> installed{{"/previous"}};
}  // namespace

namespace sumhp {
#include "sumh_maps_validate_under_test.inc"
}  // namespace sumhp
namespace sumhp::sumh {
FeatureCapabilities feature_capabilities() {
    return caps;
}
bool raw_kstat(const std::string& source_path, RawKstat& result) {
    ++raw_calls;
    const auto found = files.find(source_path);
    if (found == files.end()) {
        errno = ENOENT;
        return false;
    }
    result = found->second;
    return true;
}
bool replace_managed_kstat_rules(const std::vector<KstatRule>& rules) {
    ++replace_calls;
    std::set<std::pair<std::uint64_t, std::uint64_t>> targets;
    for (const auto& rule : rules) {
        const auto found = files.find(rule.target_path);
        if (found == files.end()) {
            errno = ENOENT;
            return false;
        }
        if (!targets.emplace(found->second.ino, found->second.dev).second) {
            errno = EEXIST;
            return false;
        }
    }
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
#include "sumh_kstat_restore_under_test.inc"
}  // namespace sumhp::mount::sumh

int main() {
    using sumhp::mount::sumh::restore_managed_kstat_rules;
    sumhp::Config config;
    std::string error;
    caps = {true, 0, 0};
    assert(restore_managed_kstat_rules(config, error));
    config.kstat_rules = {{"/target", "/source", "identity", {}}};
    assert(!restore_managed_kstat_rules(config, error));
    assert(errno == EOPNOTSUPP && replace_calls == 0);
    caps = {true, 0, SUMH_FEATURE_MANAGED_KSTAT};
    files["/target"] = {123, (259U << 20) | 4097U};
    files["/source"] = {456, (8U << 20) | 17U};
    config.kstat_rules.push_back({"/missing", "/source", "full", {}});
    assert(!restore_managed_kstat_rules(config, error));
    assert(errno == ENOENT && replace_calls == 1 && installed[0].target_path == "/previous");
    assert(raw_calls == 2);  // Only sources are queried for unprojected metadata.
    files["/missing"] = files["/target"];
    assert(!restore_managed_kstat_rules(config, error));
    assert(errno == EEXIST && replace_calls == 2);
    files["/missing"].ino = 789;
    namespace_ok = false;
    assert(!restore_managed_kstat_rules(config, error) && replace_calls == 2);
    namespace_ok = true;
    replace_ok = false;
    assert(!restore_managed_kstat_rules(config, error));
    assert(errno == EIO && replace_calls == 3 && installed[0].target_path == "/previous");
    replace_ok = true;
    assert(restore_managed_kstat_rules(config, error));
    assert(replace_calls == 4 && installed.size() == 2);
    assert(installed[0].field_mask == (SUMH_KSTAT_FIELD_DEV | SUMH_KSTAT_FIELD_INO));
    assert(installed[0].spoofed_dev == ((8UL << 20) | 17));
    assert(installed[0].spoofed_ino == 456 && installed[0].spoofed_size == 0);
    assert(installed[1].field_mask == SUMH_KSTAT_FIELD_ALL);
    assert(installed[1].spoofed_size == 0 && installed[1].spoofed_nlink == 0);
    // A second apply must not resolve target identities through spoofed stat.
    const int before_repeat = raw_calls;
    assert(restore_managed_kstat_rules(config, error));
    assert(raw_calls == before_repeat + 2 && replace_calls == 5);
    files["/source"].size = 4096;
    files["/source"].mtime_sec = 47;
    files["/source"].mtime_nsec = 9;
    assert(restore_managed_kstat_rules(config, error));
    assert(installed[1].spoofed_size == 4096 && installed[1].spoofed_mtime_sec == 47);
    assert(installed[1].spoofed_mtime_nsec == 9);
    files["/source"].size = -1;
    assert(!restore_managed_kstat_rules(config, error));
    assert(errno == EOVERFLOW && replace_calls == 6);
    config.kstat_rules = {
        {"/target", "", "custom", {{"size", "0"}, {"mtime.sec", "-1"}, {"mtime.nsec", "0"}}}};
    const int before_custom = raw_calls;
    assert(restore_managed_kstat_rules(config, error));
    assert(raw_calls == before_custom && replace_calls == 7);
    assert(installed[0].field_mask == (SUMH_KSTAT_FIELD_SIZE | SUMH_KSTAT_FIELD_MTIME));
    assert(installed[0].spoofed_size == 0 && installed[0].spoofed_mtime_sec == -1);
    config.kstat_rules.clear();
    assert(restore_managed_kstat_rules(config, error));
    assert(replace_calls == 8 && installed.empty());
    std::cout << "sumh_kstat_restore_test: all checks passed\n";
}
