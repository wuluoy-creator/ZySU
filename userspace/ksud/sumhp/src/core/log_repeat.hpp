#pragma once

#include "core/log.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sumhp::logging {

using RepeatSink = std::function<bool(Level, const std::string&, const std::string&)>;

// Per-process, exact Warning/Error coalescing. The caller serializes access and
// supplies cleaned inputs (component <= 256 bytes, message <= 8192 bytes).
// Successful write means accepted by this filter, not necessarily persisted:
// pending repeats can be lost on abrupt exit. No background timer is created.
class RepeatFilter {
public:
    bool write(Level level, const std::string& component, const std::string& message,
               std::optional<std::uint64_t> monotonic_ms, const RepeatSink& sink);
    // Successfully drained identities are forgotten. Failed summaries retain
    // their count, and other identities are still attempted.
    bool flush(const RepeatSink& sink);
    void reset();

private:
    struct Entry {
        Level level;
        std::string component;
        std::string message;
        std::uint64_t window_start_ms;
        std::uint64_t repeats = 0;
        bool at_least = false;
    };

    static constexpr std::size_t kCapacity = 32;
    static constexpr std::uint64_t kWindowMs = 30000;
    static bool emit(const Entry& entry, const RepeatSink& sink);
    static void increment(Entry& entry);

    // Ordered least recently used first. Moving entries avoids an overflowing
    // logical-use counter, including when multiple calls share a timestamp.
    std::vector<Entry> entries_;
    std::optional<std::uint64_t> last_clock_ms_;
    bool clock_drain_pending_ = false;

    // Permits deterministic uint64 saturation testing without 2^64 writes.
    friend struct RepeatFilterTestAccess;
};

}  // namespace sumhp::logging
