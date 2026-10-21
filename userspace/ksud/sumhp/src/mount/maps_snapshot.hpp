#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "sumhp/config.hpp"
#include "sumhp/sumh_client.hpp"

namespace sumhp::mount::sumh {

enum class MapsIdentityKind : std::uint32_t {
    Unknown,
    Regular,
    Missing,
    NotRegular,
    Unrepresentable,
    Error
};
struct MapsFileIdentity {
    std::uint64_t ino = 0, dev = 0;
    MapsIdentityKind kind = MapsIdentityKind::Unknown;
    int error = 0;
    bool operator==(const MapsFileIdentity& other) const {
        return ino == other.ino && dev == other.dev && kind == other.kind && error == other.error;
    }
};
// Shared by real apply and read-only sampling. Follows symlinks as apply does;
// converts libc dev_t to the kernel's 12:20 encoding and rejects non-files.
MapsFileIdentity read_maps_identity(const std::string& path);

struct MapsPathSample {
    MapsFileIdentity target, replacement;
    bool collision = false;
    bool stable = false;
};
struct MapsSnapshot {
    std::uint64_t instance_id = 0, generation = 0, namespace_ino = 0;
    bool identities_sampled = false, definitions_complete = false, stable = false;
    std::string reason = "not_sampled";
    int error = 0;
    std::vector<MapsPathSample> paths;
    std::vector<::sumhp::sumh::InventoryEntry> registered;
};
// Fixed shared result capacity, at most 256 managed records and saved paths.
// Queries in init's mount namespace; checks filesystem identities twice and
// brackets the definition inventory. No writes, restore, or retry. A dedicated
// read-only worker limits parent waiting; the whole controller/transport still
// needs C04's end-to-end deadline implementation.
MapsSnapshot sample_maps(const Config& config, bool resolve_disabled = false,
                         const ::sumhp::sumh::RuntimeState* expected = nullptr);

struct MapsComparison {
    std::string status, reason;
};
MapsComparison compare_maps(const Config& config, const MapsSnapshot& snapshot);

}  // namespace sumhp::mount::sumh
