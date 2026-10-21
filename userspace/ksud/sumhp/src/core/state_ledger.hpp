#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "core/json.hpp"

namespace sumhp::state {

struct FeatureObservation {
    std::string id;
    std::string desired_revision;
    std::string status = "unknown";  // matches, drift, unknown, unsupported
    std::string reason;
    JsonValue detail = JsonValue::object();
};
struct Snapshot {
    std::string boot_id;
    std::string instance_id;
    std::string generation;
    std::vector<FeatureObservation> features;
    JsonValue runtime = JsonValue::object();
};
struct Verification {
    std::string revision;
    std::string boot_id;
    std::string instance_id;
    std::string generation;
};
struct Operation {
    std::string id;
    std::string name;
    std::string phase;  // prepared (not executed), started (unknown), or finished
    std::string boot_id;
    int exit_code = 0;
    int error_number = 0;
    std::string detail;
    std::string request_revision{};  // empty for ordinary/legacy commands
};
struct Ledger {
    Operation last;
    std::map<std::string, Verification> verified;
    std::vector<Operation> history;  // oldest first; excludes last
    bool history_pruned = false;
    int source_schema = 3;
};

inline constexpr std::size_t LEDGER_MAX_BYTES = 32 * 1024;
inline constexpr std::size_t LEDGER_MAX_OPERATIONS = 16;
// All IDs are fixed, not derived from a user path. Unknown IDs/schema are read
// failures rather than permission to rewrite a future document.
bool parse_ledger(const std::string& bytes, Ledger& ledger, std::string& error);
std::string encode_ledger(const Ledger& ledger);
bool valid_operation_id(const std::string& id);
void trim_ledger_history(Ledger& ledger);
JsonValue operation_record_json(const Operation& operation, const std::string& current_boot);
// Observation only: no directory creation, migration, locks or kernel sampling.
std::string read_operation_history(Ledger& ledger, std::string& error);
void finish_ledger(Ledger& ledger, const Snapshot& current, int exit_code, int error_number,
                   const std::string& detail);
JsonValue state_json(const Snapshot& current, const Ledger* ledger,
                     const std::string& ledger_status, const std::string& error);
std::string revision_for_bytes(const std::string& bytes);
Snapshot collect_snapshot();

enum class ReservedStart { Started, Existing, NotRetained, Conflict, StaleBoot, Failed };

// Constructor does no IO. Begin durably records intent before a command is
// permitted to mutate. A failed finish can leave either started or finished
// bytes visible (replacement may precede fsync failure). Query before deciding;
// the journal never retries or rolls back the operation.
class Journal {
public:
    Journal();
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
    bool begin(const std::string& name, std::string& error);
    // Reservation writes metadata only; it never samples or executes a target.
    bool prepare(const std::string& name, const std::string& request_revision, std::string& error);
    ReservedStart begin_prepared(const std::string& id, const std::string& name,
                                 const std::string& request_revision, std::string& error);
    bool finish(int exit_code, int error_number, const std::string& detail, std::string& error);
    // Complete an exact local command without sampling unrelated saved or
    // kernel state. Existing verified observations remain historical entries.
    bool finish_record_only(int exit_code, int error_number, const std::string& detail,
                            std::string& error);
    // Reserved stable-hide and exact temporary Maps removals retain a typed
    // result. Failure does not prevent summary finish or permit replay.
    bool retain_resource(const std::vector<std::string>& args, const std::string& output,
                         bool complete, int exit_code, int error_number, std::string& error);
    const std::string& operation_id() const;
    // Retained record sampled while holding the ledger lock. Null when no
    // trustworthy matched record is available; never a runtime observation.
    const Operation* observed_operation() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
int print_state_json();
bool command_needs_journal(const std::vector<std::string>& args);

}  // namespace sumhp::state
