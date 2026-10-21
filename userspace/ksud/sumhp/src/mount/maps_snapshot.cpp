#include "mount/maps_snapshot.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <tuple>
#include <type_traits>
#include "mount/query_worker.hpp"
#include "uapi/sumh.h"

namespace sumhp::mount::sumh {

MapsFileIdentity read_maps_identity(const std::string& path) {
    struct stat info{};
    MapsFileIdentity result;
    if (::stat(path.c_str(), &info) != 0) {
        result.error = errno ? errno : EIO;
        result.kind = result.error == ENOENT || result.error == ENOTDIR ? MapsIdentityKind::Missing
                                                                        : MapsIdentityKind::Error;
        return result;
    }
    if (!S_ISREG(info.st_mode)) {
        result.kind = MapsIdentityKind::NotRegular;
        result.error = EINVAL;
        return result;
    }
    const auto device_major = static_cast<std::uint64_t>(major(info.st_dev));
    const auto device_minor = static_cast<std::uint64_t>(minor(info.st_dev));
    const auto device = (device_major << 20) | device_minor;
    if (!info.st_ino || static_cast<std::uint64_t>(info.st_ino) > ULONG_MAX ||
        device_major > 0xfff || device_minor > 0xfffff || !device) {
        result.kind = MapsIdentityKind::Unrepresentable;
        result.error = EOVERFLOW;
        return result;
    }
    result.kind = MapsIdentityKind::Regular;
    result.ino = info.st_ino;
    result.dev = device;
    return result;
}

namespace {
constexpr std::size_t capacity = SUMH_MANAGED_MAPS_MAX_RULES;
struct StoredMapsRecord {
    std::uint64_t id, target_ino, target_dev, spoofed_ino, spoofed_dev;
    std::uint32_t path_len;
    char path[256];
};
struct SharedSample {
    std::uint64_t instance{}, generation{}, namespace_ino{};
    std::uint32_t count{}, supported_categories{}, identities_sampled{}, inventory_complete{},
        stable{}, finished{};
    int inventory_error{};
    MapsPathSample paths[capacity];
    StoredMapsRecord records[capacity]{};
};
static_assert(std::is_trivially_copyable_v<SharedSample>);
static_assert(sizeof(SharedSample) < 128UL * 1024);

bool namespace_identity(std::uint64_t& identity) {
    struct stat own{};
    struct stat init{};
    if (::stat("/proc/self/ns/mnt", &own) != 0 || ::stat("/proc/1/ns/mnt", &init) != 0)
        return false;
    if (own.st_ino != init.st_ino || own.st_dev != init.st_dev || !own.st_ino) {
        errno = ESTALE;
        return false;
    }
    identity = own.st_ino;
    return true;
}

// This callback is executed only in the forked namespace worker. Publishing a
// finished flag is meaningful only after confirmed successful worker completion.
bool sample_in_namespace(const Config& config, bool resolve_disabled,
                         const ::sumhp::sumh::RuntimeState* expected, SharedSample& out) {
    using namespace ::sumhp::sumh;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const auto budget = [&] {
        if (std::chrono::steady_clock::now() < deadline)
            return true;
        errno = ETIMEDOUT;
        return false;
    };
    if (!namespace_identity(out.namespace_ino))
        return false;
    InventoryRequest request{SUMH_INVENTORY_MAPS_MANAGED, expected ? expected->instance_id : 0,
                             expected ? expected->generation : 0, 0};
    InventoryPage page;
    // Direct capability-specific query: EOPNOTSUPP/ENOTTY is explicit unknown;
    // do not fall back to legacy text or an ioctl with lazy restore effects.
    if (!rule_inventory(request, page))
        out.inventory_error = errno ? errno : EIO;
    else {
        out.instance = page.instance_id;
        out.generation = page.generation;
        request.instance_id = page.instance_id;
        request.generation = page.generation;
        const auto supported = out.supported_categories = page.supported_categories;
        std::set<std::pair<std::uint64_t, std::uint64_t>> keys;
        for (;;) {
            if (!budget()) {
                out.inventory_error = errno;
                break;
            }
            if (page.supported_categories != supported) {
                out.inventory_error = EPROTO;
                break;
            }
            if (!page.entries.empty()) {
                const auto& entry = page.entries.front();
                if (out.count == capacity || entry.path2.size() >= sizeof(out.records[0].path)) {
                    out.inventory_error = EOVERFLOW;
                    break;
                }
                if (!keys.emplace(entry.target_ino, entry.target_dev).second) {
                    out.inventory_error = EPROTO;
                    break;
                }
                auto& record = out.records[out.count++];
                record.id = entry.id;
                record.target_ino = entry.target_ino;
                record.target_dev = entry.target_dev;
                record.spoofed_ino = entry.spoofed_ino;
                record.spoofed_dev = entry.spoofed_dev;
                record.path_len = entry.path2.size();
                std::memcpy(record.path, entry.path2.data(), record.path_len);
            }
            if (page.at_end) {
                out.inventory_complete = 1;
                break;
            }
            request.cursor = page.next_cursor;
            if (!rule_inventory(request, page)) {
                out.inventory_error = errno ? errno : EIO;
                break;
            }
        }
    }
    const bool resolve = config.enable_maps_spoof || resolve_disabled;
    if (resolve) {
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> targets;
        for (std::size_t i = 0; i < config.maps_rules.size(); ++i) {
            if (!budget())
                return false;
            auto& row = out.paths[i];
            row.target = read_maps_identity(config.maps_rules[i].target_path);
            row.replacement = read_maps_identity(config.maps_rules[i].replacement_path);
            if (row.target.kind == MapsIdentityKind::Regular) {
                const auto [previous, unique] =
                    targets.emplace(std::make_pair(row.target.ino, row.target.dev), i);
                if (!unique) {
                    row.collision = true;
                    out.paths[previous->second].collision = true;
                }
            }
        }
        out.identities_sampled = 1;
        for (std::size_t i = 0; i < config.maps_rules.size(); ++i) {
            if (!budget())
                return false;
            out.paths[i].stable =
                out.paths[i].target == read_maps_identity(config.maps_rules[i].target_path) &&
                out.paths[i].replacement ==
                    read_maps_identity(config.maps_rules[i].replacement_path);
        }
    }
    bool stable = !resolve || std::all_of(out.paths, out.paths + config.maps_rules.size(),
                                          [](const auto& row) { return row.stable; });
    std::uint64_t after_namespace = 0;
    if (!namespace_identity(after_namespace) || after_namespace != out.namespace_ino)
        stable = false;
    if (out.inventory_complete) {
        InventoryPage after;
        if (!budget() || !rule_inventory({0, out.instance, out.generation, 0}, after)) {
            out.inventory_error = errno ? errno : EIO;
            out.inventory_complete = 0;
            stable = false;
        } else if (after.supported_categories != out.supported_categories) {
            out.inventory_error = EPROTO;
            out.inventory_complete = 0;
            stable = false;
        }
    }
    out.stable = stable;
    out.finished = 1;
    return true;
}
}  // namespace

MapsSnapshot sample_maps(const Config& config, bool resolve_disabled,
                         const ::sumhp::sumh::RuntimeState* expected) {
    MapsSnapshot result;
    std::string error;
    if (!validate_maps_path_rules(config.maps_rules, error)) {
        result.reason = "saved_maps_definitions_invalid";
        result.error = EINVAL;
        return result;
    }
    if (expected && (!expected->instance_id || !expected->generation)) {
        result.reason = "runtime_token_invalid";
        result.error = EINVAL;
        return result;
    }
    auto* const shared = static_cast<SharedSample*>(::mmap(
        nullptr, sizeof(SharedSample), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (shared == MAP_FAILED) {
        result.reason = "maps_sample_allocation_failed";
        result.error = errno;
        return result;
    }
    new (shared) SharedSample{};
    const auto cleanup = [](SharedSample* value) {
        value->~SharedSample();
        ::munmap(value, sizeof(SharedSample));
    };
    const std::unique_ptr<SharedSample, decltype(cleanup)> owner(shared, cleanup);
    const bool completed = fsutil::run_query_in_init_mount_ns(
        [&] { return sample_in_namespace(config, resolve_disabled, expected, *shared); });
    if (!completed || shared->finished != 1) {
        result.reason = "maps_namespace_sample_failed";
        result.error = errno ? errno : EIO;
    } else {
        result.instance_id = shared->instance;
        result.generation = shared->generation;
        result.namespace_ino = shared->namespace_ino;
        result.identities_sampled = shared->identities_sampled;
        result.definitions_complete = shared->inventory_complete;
        result.stable = shared->stable;
        result.error = shared->inventory_error;
        result.reason = "maps_sampled";
        if (!result.stable)
            result.reason = "maps_sample_changed";
        else if (result.error)
            result.reason = "managed_maps_inventory_failed";
        if (result.identities_sampled)
            result.paths.assign(shared->paths, shared->paths + config.maps_rules.size());
        // A prefix is never published as a complete/reconcilable runtime set.
        if (result.definitions_complete && result.stable) {
            for (std::uint32_t i = 0; i < shared->count; ++i) {
                const auto& record = shared->records[i];
                ::sumhp::sumh::InventoryEntry entry;
                entry.category = SUMH_INVENTORY_MAPS_MANAGED;
                entry.owner = SUMH_INVENTORY_OWNER_MANAGED_MAPS;
                entry.id_kind = SUMH_INVENTORY_ID_GENERATION_ORDINAL;
                entry.id = record.id;
                entry.target_ino = record.target_ino;
                entry.target_dev = record.target_dev;
                entry.spoofed_ino = record.spoofed_ino;
                entry.spoofed_dev = record.spoofed_dev;
                entry.path2.assign(record.path, record.path_len);
                result.registered.push_back(std::move(entry));
            }
        }
    }
    return result;
}

MapsComparison compare_maps(const Config& config, const MapsSnapshot& snapshot) {
    if (!snapshot.stable || !snapshot.definitions_complete)
        return {"unknown", snapshot.reason};
    if (!config.enable_maps_spoof)
        return snapshot.registered.empty()
                   ? MapsComparison{"matches", "managed_maps_cleared_while_disabled"}
                   : MapsComparison{"drift", "managed_maps_retained_while_disabled"};
    if (!snapshot.identities_sampled || snapshot.paths.size() != config.maps_rules.size())
        return {"unknown", "maps_identities_not_sampled"};
    using Key = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::string>;
    std::set<Key> desired;
    std::set<Key> registered;
    for (std::size_t i = 0; i < config.maps_rules.size(); ++i) {
        const auto& row = snapshot.paths[i];
        if (!row.stable)
            return {"unknown", "maps_sample_changed"};
        if (row.collision)
            return {"drift", "maps_target_identity_collision"};
        if (row.target.kind != MapsIdentityKind::Regular ||
            row.replacement.kind != MapsIdentityKind::Regular)
            return {"unknown", "maps_saved_paths_unresolved"};
        desired.emplace(row.target.ino, row.target.dev, row.replacement.ino, row.replacement.dev,
                        config.maps_rules[i].replacement_path);
    }
    for (const auto& entry : snapshot.registered)
        registered.emplace(entry.target_ino, entry.target_dev, entry.spoofed_ino, entry.spoofed_dev,
                           entry.path2);
    return desired == registered
               ? MapsComparison{"matches", "managed_maps_current_identities_match"}
               : MapsComparison{"drift", "managed_maps_current_identities_differ"};
}
}  // namespace sumhp::mount::sumh
