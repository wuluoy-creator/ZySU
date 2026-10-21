#pragma once

#include <sys/types.h>
#include <filesystem>
#include <string>

namespace ksud {

enum class DurableWriteState {
    NotCommitted,
    ReplacedDurabilityUnconfirmed,
    Complete,
};

struct DurableWriteResult {
    DurableWriteState state;
    int error_number;

    [[nodiscard]] bool replaced() const { return state != DurableWriteState::NotCommitted; }
    [[nodiscard]] bool complete() const { return state == DurableWriteState::Complete; }
};

// The parent directory must already exist. A successful result confirms both
// the file contents and the rename with fsync. A failure after rename MUST NOT
// be treated as an unchanged file or trigger an unconditional rollback/replay.
[[nodiscard]] DurableWriteResult replace_file_durable(const std::filesystem::path& path,
                                                      const std::string& content,
                                                      mode_t mode = 0600);

// Convenience API for callers that propagate a human-readable error. On a
// post-rename failure the error explicitly says the file has been replaced.
bool write_file_durable(const std::filesystem::path& path, const std::string& content,
                        std::string& error, mode_t mode = 0600);

}  // namespace ksud
