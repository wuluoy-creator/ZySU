#include "core/log_repeat.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace sumhp::logging {

void RepeatFilter::increment(Entry& entry) {
    if (entry.repeats == std::numeric_limits<std::uint64_t>::max()) {
        entry.at_least = true;
    } else {
        ++entry.repeats;
    }
}

bool RepeatFilter::emit(const Entry& entry, const RepeatSink& sink) {
    if (entry.repeats == 0)
        return true;
    // Put the count before the original text: the record encoder may truncate
    // the end of a maximum-size message. The first immediate record is excluded.
    const std::string summary = "[repeated " + std::string(entry.at_least ? "at least " : "") +
                                std::to_string(entry.repeats) + " times] " + entry.message;
    return sink(entry.level, entry.component, summary);
}

bool RepeatFilter::write(Level level, const std::string& component, const std::string& message,
                         std::optional<std::uint64_t> monotonic_ms, const RepeatSink& sink) {
    const bool clock_invalid = !monotonic_ms || (last_clock_ms_ && *monotonic_ms < *last_clock_ms_);
    last_clock_ms_ = monotonic_ms;
    if (clock_invalid || clock_drain_pending_) {
        // Never suppress an event against an unknown or regressed time base.
        // If draining fails, continue bypassing aggregation until it succeeds.
        const bool drained = flush(sink);
        clock_drain_pending_ = !drained;
        const bool written = sink(level, component, message);
        return drained && written;
    }

    if (level != Level::Warning && level != Level::Error)
        return sink(level, component, message);

    auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
        return entry.level == level && entry.component == component && entry.message == message;
    });
    if (found != entries_.end()) {
        increment(*found);
        // Make the current identity most recently used even when its eventual
        // sink write fails: the event was observed, and its count is retained.
        std::rotate(found, found + 1, entries_.end());
        auto& entry = entries_.back();
        if (*monotonic_ms - entry.window_start_ms < kWindowMs)
            return true;
        if (!emit(entry, sink))
            return false;
        entry.repeats = 0;
        entry.at_least = false;
        entry.window_start_ms = *monotonic_ms;
        return true;
    }

    // New/distinct events stay visible even if an old pending summary cannot
    // be evicted. Never remember an event which the sink rejected.
    if (!sink(level, component, message))
        return false;
    if (entries_.size() == kCapacity) {
        if (!emit(entries_.front(), sink))
            return false;
        entries_.erase(entries_.begin());
    }
    entries_.push_back({level, component, message, *monotonic_ms});
    return true;
}

bool RepeatFilter::flush(const RepeatSink& sink) {
    bool success = true;
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        if (emit(*entry, sink)) {
            entry = entries_.erase(entry);
        } else {
            success = false;
            ++entry;
        }
    }
    if (success)
        clock_drain_pending_ = false;
    return success;
}

void RepeatFilter::reset() {
    entries_.clear();
    last_clock_ms_.reset();
    clock_drain_pending_ = false;
}

}  // namespace sumhp::logging
