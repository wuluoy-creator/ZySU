#include "core/log_repeat.hpp"

#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace sumhp::logging {
struct RepeatFilterTestAccess {
    static std::size_t size(const RepeatFilter& filter) { return filter.entries_.size(); }
    static void set_pending(RepeatFilter& filter, std::uint64_t count) {
        if (filter.entries_.size() != 1)
            throw std::runtime_error("saturation fixture requires one identity");
        filter.entries_.front().repeats = count;
    }
};
}  // namespace sumhp::logging

namespace {
using sumhp::logging::Level;
using sumhp::logging::RepeatFilter;
using sumhp::logging::RepeatFilterTestAccess;
using sumhp::logging::RepeatSink;

int checks = 0;
void check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition)
        throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
}
#define CHECK(expression) check((expression), #expression, __LINE__)

struct Record {
    Level level;
    std::string component;
    std::string message;
};
struct Sink {
    std::vector<Record> attempts;
    std::vector<Record> accepted;
    std::function<bool(const Record&)> reject;
    RepeatSink callback() {
        return [this](Level level, const std::string& component, const std::string& message) {
            Record record{level, component, message};
            attempts.push_back(record);
            if (reject && reject(record))
                return false;
            accepted.push_back(record);
            return true;
        };
    }
};

bool event(RepeatFilter& filter, Sink& sink, std::uint64_t time, int id) {
    return filter.write(Level::Error, "poll", "event-" + std::to_string(id), time, sink.callback());
}

void window_boundaries() {
    RepeatFilter filter;
    Sink sink;
    CHECK(event(filter, sink, 100, 0));
    CHECK(event(filter, sink, 101, 0));
    CHECK(event(filter, sink, 30099, 0));
    CHECK(sink.accepted.size() == 1);
    CHECK(event(filter, sink, 30100, 0));
    CHECK(sink.accepted.size() == 2);
    CHECK(sink.accepted.back().message == "[repeated 3 times] event-0");
    CHECK(sink.accepted.back().level == Level::Error);
    CHECK(sink.accepted.back().component == "poll");
    CHECK(event(filter, sink, 30101, 0));
    CHECK(event(filter, sink, 60100, 0));
    CHECK(sink.accepted.back().message == "[repeated 2 times] event-0");
    CHECK(filter.flush(sink.callback()));
    CHECK(sink.accepted.size() == 3);
}

void exact_identities_and_passthrough() {
    RepeatFilter filter;
    Sink sink;
    CHECK(filter.write(Level::Error, "one", "error", 0, sink.callback()));
    CHECK(filter.write(Level::Warning, "one", "error", 1, sink.callback()));
    CHECK(filter.write(Level::Error, "two", "error", 2, sink.callback()));
    CHECK(filter.write(Level::Error, "one", "Error", 3, sink.callback()));
    CHECK(sink.accepted.size() == 4);
    CHECK(filter.write(Level::Error, "one", "error", 4, sink.callback()));
    CHECK(filter.write(Level::Warning, "one", "error", 5, sink.callback()));
    CHECK(sink.accepted.size() == 4);
    for (Level level : {Level::Debug, Level::Info}) {
        CHECK(filter.write(level, "one", "error", 6, sink.callback()));
        CHECK(filter.write(level, "one", "error", 6, sink.callback()));
    }
    CHECK(sink.accepted.size() == 8);
    CHECK(RepeatFilterTestAccess::size(filter) == 4);
    CHECK(filter.flush(sink.callback()));
    CHECK(sink.accepted.size() == 10);
    CHECK(sink.accepted[8].level == Level::Error);
    CHECK(sink.accepted[9].level == Level::Warning);
    CHECK(sink.accepted[8].message == "[repeated 1 times] error");
}

void capacity_and_lru() {
    RepeatFilter filter;
    Sink sink;
    for (int id = 0; id < 32; ++id)
        CHECK(event(filter, sink, 0, id));
    CHECK(RepeatFilterTestAccess::size(filter) == 32);
    for (int id = 0; id < 32; ++id)
        CHECK(event(filter, sink, 1, id));
    CHECK(event(filter, sink, 1, 0));  // Equal timestamps still update recency.
    CHECK(sink.accepted.size() == 32);
    CHECK(event(filter, sink, 2, 32));
    CHECK(sink.accepted.size() == 34);
    CHECK(sink.accepted[32].message == "event-32");
    CHECK(sink.accepted[33].message == "[repeated 1 times] event-1");
    CHECK(RepeatFilterTestAccess::size(filter) == 32);
    CHECK(event(filter, sink, 3, 0));  // Retained entry is still suppressed.
    CHECK(sink.accepted.size() == 34);
    CHECK(event(filter, sink, 4, 1));  // Evicted identity is a visible first event.
    CHECK(sink.accepted[34].message == "event-1");
    CHECK(sink.accepted[35].message == "[repeated 1 times] event-2");
    CHECK(RepeatFilterTestAccess::size(filter) == 32);
}

void capacity_without_pending_counts() {
    RepeatFilter filter;
    Sink sink;
    for (int id = 0; id < 33; ++id)
        CHECK(event(filter, sink, 0, id));
    CHECK(sink.accepted.size() == 33);
    CHECK(event(filter, sink, 0, 0));
    CHECK(sink.accepted.size() == 34);
    CHECK(RepeatFilterTestAccess::size(filter) == 32);
}

void failed_eviction_keeps_pending_and_new_events_visible() {
    RepeatFilter filter;
    Sink sink;
    for (int id = 0; id < 32; ++id)
        CHECK(event(filter, sink, 0, id));
    for (int id = 0; id < 32; ++id)
        CHECK(event(filter, sink, 1, id));
    sink.reject = [](const Record& record) {
        return record.message == "[repeated 1 times] event-0";
    };
    CHECK(!event(filter, sink, 2, 32));
    CHECK(sink.accepted.back().message == "event-32");
    CHECK(RepeatFilterTestAccess::size(filter) == 32);
    CHECK(!event(filter, sink, 3, 32));
    CHECK(sink.accepted.size() == 34);  // New event was not remembered on failed eviction.
    sink.reject = {};
    CHECK(event(filter, sink, 4, 32));
    CHECK(sink.accepted.size() == 36);
    CHECK(sink.accepted.back().message == "[repeated 1 times] event-0");
    CHECK(event(filter, sink, 5, 32));
    CHECK(sink.accepted.size() == 36);
}

void rejected_first_event_is_not_remembered() {
    RepeatFilter filter;
    Sink sink;
    sink.reject = [](const Record&) { return true; };
    CHECK(!event(filter, sink, 0, 0));
    CHECK(RepeatFilterTestAccess::size(filter) == 0);
    sink.reject = {};
    CHECK(event(filter, sink, 1, 0));
    CHECK(sink.accepted.size() == 1);
    CHECK(event(filter, sink, 2, 0));
    CHECK(sink.accepted.size() == 1);
}

void failed_boundary_keeps_window_and_count() {
    RepeatFilter filter;
    Sink sink;
    CHECK(event(filter, sink, 0, 0));
    sink.reject = [](const Record& record) { return record.message.front() == '['; };
    CHECK(!event(filter, sink, 30000, 0));
    CHECK(sink.attempts.back().message == "[repeated 1 times] event-0");
    sink.reject = {};
    CHECK(event(filter, sink, 30001, 0));
    CHECK(sink.accepted.back().message == "[repeated 2 times] event-0");
    CHECK(event(filter, sink, 30002, 0));
    CHECK(sink.accepted.size() == 2);
    CHECK(filter.flush(sink.callback()));
    CHECK(sink.accepted.back().message == "[repeated 1 times] event-0");
}

void flush_retries_only_failed_summaries() {
    RepeatFilter filter;
    Sink sink;
    CHECK(event(filter, sink, 0, 0));
    CHECK(event(filter, sink, 0, 1));
    CHECK(event(filter, sink, 0, 2));
    CHECK(event(filter, sink, 1, 0));
    CHECK(event(filter, sink, 1, 1));
    sink.reject = [](const Record& record) {
        return record.message == "[repeated 1 times] event-0";
    };
    CHECK(!filter.flush(sink.callback()));
    CHECK(RepeatFilterTestAccess::size(filter) == 1);
    CHECK(sink.accepted.size() == 4);
    CHECK(sink.accepted.back().message == "[repeated 1 times] event-1");
    sink.reject = {};
    CHECK(filter.flush(sink.callback()));
    CHECK(sink.accepted.size() == 5);
    CHECK(sink.accepted.back().message == "[repeated 1 times] event-0");
    CHECK(filter.flush(sink.callback()));
    CHECK(sink.accepted.size() == 5);
    CHECK(event(filter, sink, 2, 0));
    CHECK(sink.accepted.back().message == "event-0");
}

void reset_discards_pending_state() {
    RepeatFilter filter;
    Sink sink;
    CHECK(event(filter, sink, 100, 0));
    CHECK(event(filter, sink, 101, 0));
    filter.reset();
    CHECK(filter.flush(sink.callback()));
    CHECK(sink.accepted.size() == 1);
    CHECK(event(filter, sink, 0, 0));  // A new boot's lower clock starts afresh.
    CHECK(event(filter, sink, 1, 0));
    CHECK(sink.accepted.size() == 2);
}

void missing_clock_flushes_and_bypasses() {
    RepeatFilter filter;
    Sink sink;
    CHECK(event(filter, sink, 100, 0));
    CHECK(event(filter, sink, 101, 0));
    CHECK(filter.write(Level::Error, "poll", "event-0", std::nullopt, sink.callback()));
    CHECK(sink.accepted.size() == 3);
    CHECK(sink.accepted[1].message == "[repeated 1 times] event-0");
    CHECK(sink.accepted[2].message == "event-0");
    CHECK(filter.write(Level::Error, "poll", "event-0", std::nullopt, sink.callback()));
    CHECK(sink.accepted.size() == 4);
    CHECK(event(filter, sink, 0, 0));
    CHECK(event(filter, sink, 1, 0));
    CHECK(sink.accepted.size() == 5);
}

void regressed_clock_and_failed_drain_do_not_suppress() {
    RepeatFilter filter;
    Sink sink;
    CHECK(event(filter, sink, 100, 0));
    CHECK(event(filter, sink, 101, 0));
    sink.reject = [](const Record& record) { return record.message.front() == '['; };
    CHECK(!event(filter, sink, 1, 0));
    CHECK(sink.accepted.size() == 2);
    CHECK(sink.accepted.back().message == "event-0");
    CHECK(!event(filter, sink, 2, 1));
    CHECK(sink.accepted.size() == 3);
    CHECK(sink.accepted.back().message == "event-1");
    sink.reject = {};
    CHECK(event(filter, sink, 3, 0));
    CHECK(sink.accepted[3].message == "[repeated 1 times] event-0");
    CHECK(sink.accepted[4].message == "event-0");
    CHECK(event(filter, sink, 4, 0));
    CHECK(event(filter, sink, 5, 0));
    CHECK(sink.accepted.size() == 6);
}

void invalid_clock_still_reports_current_sink_failure() {
    RepeatFilter filter;
    Sink sink;
    CHECK(event(filter, sink, 0, 0));
    CHECK(event(filter, sink, 1, 0));
    sink.reject = [](const Record& record) { return record.message == "current"; };
    CHECK(!filter.write(Level::Info, "poll", "current", std::nullopt, sink.callback()));
    CHECK(sink.accepted.size() == 2);
    CHECK(sink.attempts.back().message == "current");
    CHECK(RepeatFilterTestAccess::size(filter) == 0);
}

void saturated_count_is_explicit_and_prefix_preserves_identity() {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    RepeatFilter filter;
    Sink sink;
    const std::string component = "组件";
    const std::string message = "路径 /data/测试/文件 ERROR  ";
    CHECK(filter.write(Level::Warning, component, message, 0, sink.callback()));
    RepeatFilterTestAccess::set_pending(filter, maximum - 1);
    CHECK(filter.write(Level::Warning, component, message, 30000, sink.callback()));
    CHECK(sink.accepted.back().message == "[repeated 18446744073709551615 times] " + message);
    RepeatFilterTestAccess::set_pending(filter, maximum);
    sink.reject = [](const Record& record) { return record.message.front() == '['; };
    CHECK(!filter.write(Level::Warning, component, message, 60000, sink.callback()));
    CHECK(sink.attempts.back().message ==
          "[repeated at least 18446744073709551615 times] " + message);
    sink.reject = {};
    CHECK(filter.flush(sink.callback()));
    CHECK(sink.accepted.back().message ==
          "[repeated at least 18446744073709551615 times] " + message);
    CHECK(sink.accepted.back().level == Level::Warning);
    CHECK(sink.accepted.back().component == component);
}

void maximum_clock_does_not_wrap_elapsed_time() {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    RepeatFilter filter;
    Sink sink;
    CHECK(event(filter, sink, maximum - 30000, 0));
    CHECK(event(filter, sink, maximum - 1, 0));
    CHECK(sink.accepted.size() == 1);
    CHECK(event(filter, sink, maximum, 0));
    CHECK(sink.accepted.back().message == "[repeated 2 times] event-0");
    CHECK(event(filter, sink, 0, 0));
    CHECK(sink.accepted.back().message == "event-0");
}

void bounded_inputs_and_empty_messages_are_preserved() {
    RepeatFilter filter;
    Sink sink;
    const std::string component(256, 'c');
    const std::string message(8192, 'm');
    CHECK(filter.write(Level::Error, component, message, 0, sink.callback()));
    CHECK(sink.accepted.back().component == component);
    CHECK(sink.accepted.back().message == message);
    CHECK(filter.write(Level::Error, component, message, 1, sink.callback()));
    CHECK(filter.write(Level::Error, component, "", 2, sink.callback()));
    CHECK(sink.accepted.back().message.empty());
    CHECK(filter.write(Level::Error, component, " ", 3, sink.callback()));
    CHECK(sink.accepted.back().message == " ");
    CHECK(filter.flush(sink.callback()));
    CHECK(sink.accepted.back().message == "[repeated 1 times] " + message);
}

}  // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"window_boundaries", window_boundaries},
        {"exact_identities_and_passthrough", exact_identities_and_passthrough},
        {"capacity_and_lru", capacity_and_lru},
        {"capacity_without_pending_counts", capacity_without_pending_counts},
        {"failed_eviction_keeps_pending_and_new_events_visible",
         failed_eviction_keeps_pending_and_new_events_visible},
        {"rejected_first_event_is_not_remembered", rejected_first_event_is_not_remembered},
        {"failed_boundary_keeps_window_and_count", failed_boundary_keeps_window_and_count},
        {"flush_retries_only_failed_summaries", flush_retries_only_failed_summaries},
        {"reset_discards_pending_state", reset_discards_pending_state},
        {"missing_clock_flushes_and_bypasses", missing_clock_flushes_and_bypasses},
        {"regressed_clock_and_failed_drain_do_not_suppress",
         regressed_clock_and_failed_drain_do_not_suppress},
        {"invalid_clock_still_reports_current_sink_failure",
         invalid_clock_still_reports_current_sink_failure},
        {"saturated_count_is_explicit_and_prefix_preserves_identity",
         saturated_count_is_explicit_and_prefix_preserves_identity},
        {"maximum_clock_does_not_wrap_elapsed_time", maximum_clock_does_not_wrap_elapsed_time},
        {"bounded_inputs_and_empty_messages_are_preserved",
         bounded_inputs_and_empty_messages_are_preserved},
    };
    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "PASS " << test.first << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() << " cases, " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
