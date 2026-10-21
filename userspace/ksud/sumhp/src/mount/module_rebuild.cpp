#include "mount/module_rebuild.hpp"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include "core/runtime.hpp"
#include "utils.hpp"

namespace sumhp::mount::sumh {
namespace {
class Descriptor {
public:
    explicit Descriptor(int value) : value_(value) {}
    ~Descriptor() {
        if (value_ >= 0)
            ::close(value_);
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&&) = delete;
    Descriptor& operator=(Descriptor&&) = delete;
    [[nodiscard]] int get() const { return value_; }

private:
    int value_;
};
std::filesystem::path history_path() {
    return runtime_data_dir() / "module_rebuild_history.json";
}
std::filesystem::path latch_path() {
    return runtime_data_dir() / "module_rebuild_intent.json";
}
constexpr std::size_t latch_max_bytes = 1024;
struct Latch {
    std::string id, boot_id;
};
std::string read_bytes(const std::filesystem::path& path, std::size_t capacity, std::string& bytes,
                       std::string& error) {
    const Descriptor fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (fd.get() < 0) {
        if (errno == ENOENT)
            return "missing";
        error = std::strerror(errno);
        return "unreadable";
    }
    struct stat metadata{};
    if (::fstat(fd.get(), &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 ||
        metadata.st_size < 0 || static_cast<std::uint64_t>(metadata.st_size) > capacity) {
        error = "invalid module rebuild file type, link count or capacity";
        errno = EIO;
        return "unreadable";
    }
    std::string result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    char buffer[4096];
    while (result.size() <= capacity) {
        if (std::chrono::steady_clock::now() >= deadline) {
            error = "module rebuild history read budget exceeded";
            errno = ETIMEDOUT;
            return "unreadable";
        }
        const auto n =
            ::read(fd.get(), buffer, std::min(sizeof(buffer), capacity + 1 - result.size()));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            error = std::strerror(errno);
            return "unreadable";
        }
        if (!n)
            break;
        result.append(buffer, static_cast<std::size_t>(n));
    }
    if (result.size() > capacity) {
        error = "module rebuild history exceeds capacity";
        errno = EOVERFLOW;
        return "unreadable";
    }
    bytes = std::move(result);
    return "valid";
}
bool write_history(const ModuleRebuildHistory& history, std::string& error) {
    const auto bytes = encode_module_rebuild_history(history);
    ModuleRebuildHistory checked;
    if (!parse_module_rebuild_history(bytes, checked, error)) {
        errno = EOVERFLOW;
        return false;
    }
    if (!ksud::write_file_durable(history_path(), bytes, error))
        return false;
    std::string actual;
    if (read_bytes(history_path(), MODULE_REBUILD_MAX_BYTES, actual, error) != "valid" ||
        actual != bytes) {
        error = "module rebuild receipt committed but readback unconfirmed";
        errno = EIO;
        return false;
    }
    error.clear();
    return true;
}
JsonValue latch_json(const Latch& latch) {
    auto value = JsonValue::object();
    value.o["schema_version"] = 1;
    value.o["resource"] = "module_rebuild_intent";
    value.o["id"] = latch.id;
    value.o["boot_id"] = latch.boot_id;
    return value;
}
std::string read_latch(Latch& out, std::string& error) {
    std::string bytes;
    auto status = read_bytes(latch_path(), latch_max_bytes, bytes, error);
    if (status != "valid")
        return status;
    JsonValue value;
    if (!json::Parser(bytes, true).parse(value, error) || !value.is_object() ||
        value.o.size() != 4 || !value.at("schema_version").is_number() ||
        value.at("schema_version").n != 1 || !value.at("resource").is_string() ||
        value.at("resource").s != "module_rebuild_intent" || !value.at("id").is_string() ||
        value.at("id").s.size() != 32 ||
        value.at("id").s.find_first_not_of("0123456789abcdef") != std::string::npos ||
        !value.at("boot_id").is_string() || value.at("boot_id").s.empty() ||
        value.at("boot_id").s.size() > 128 ||
        value.at("boot_id").s.find_first_not_of(
            "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ._:-") !=
            std::string::npos) {
        error = "module rebuild intent is invalid or unsupported";
        errno = EINVAL;
        return "corrupt_or_unsupported";
    }
    out = {value.at("id").s, value.at("boot_id").s};
    error.clear();
    return "valid";
}
bool write_latch(const ModuleRebuildRecord& record, std::string& error) {
    const auto bytes = stringify_json(latch_json({record.id, record.boot_id}));
    if (!ksud::write_file_durable(latch_path(), bytes, error))
        return false;
    std::string actual;
    if (read_bytes(latch_path(), latch_max_bytes, actual, error) != "valid" || actual != bytes) {
        error = "module rebuild intent committed but readback unconfirmed";
        errno = EIO;
        return false;
    }
    return true;
}
bool new_id(std::string& id) {
    unsigned char bytes[16];
    std::size_t used = 0;
    while (used != sizeof(bytes)) {
        const auto n = ::getrandom(bytes + used, sizeof(bytes) - used, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!errno)
                errno = EIO;
            return false;
        }
        used += static_cast<std::size_t>(n);
    }
    static constexpr char hex[] = "0123456789abcdef";
    for (auto byte : bytes) {
        id += hex[byte >> 4];
        id += hex[byte & 15];
    }
    return true;
}
bool allowed(const ModuleRebuildHistory& history, const Latch& latch, const std::string& boot,
             std::string& error) {
    if (boot.empty()) {
        error = "boot identity unavailable; module rebuild not started";
        errno = EIO;
        return false;
    }
    std::string blocking_id;
    bool blocked = false;
    if (latch.boot_id == boot) {
        blocking_id = latch.id;
        blocked = true;
    } else if (history.latest && history.latest->boot_id == boot &&
               history.latest->phase != "acknowledged") {
        blocking_id = history.latest->id;
        blocked = true;
    }
    if (blocked) {
        error = "module rebuild blocked by unresolved submission " + blocking_id +
                "; inspect module rebuild-status; no same-boot replay or cleanup is safe";
        errno = EBUSY;
        return false;
    }
    return true;
}
}  // namespace

std::string read_module_rebuild_history(ModuleRebuildHistory& out, std::string& error) {
    error.clear();
    std::string bytes;
    auto status = read_bytes(history_path(), MODULE_REBUILD_MAX_BYTES, bytes, error);
    if (status != "valid")
        return status;
    return parse_module_rebuild_history(bytes, out, error) ? "valid" : "corrupt_or_unsupported";
}
bool module_rebuild_allowed(std::string& error) {
    ModuleRebuildHistory history;
    const auto status = read_module_rebuild_history(history, error);
    if (status != "valid" && status != "missing") {
        errno = EIO;
        return false;
    }
    Latch latch;
    const auto latch_status = read_latch(latch, error);
    if (latch_status != "valid" && latch_status != "missing") {
        errno = EIO;
        return false;
    }
    return allowed(history, latch, runtime_boot_id(), error);
}
state::OperationResponse query_module_rebuild_status() {
    const auto before = runtime_boot_id();
    ModuleRebuildHistory history;
    std::string error;
    const auto status = read_module_rebuild_history(history, error);
    Latch latch;
    std::string latch_error;
    const auto latch_status = read_latch(latch, latch_error);
    const auto after = runtime_boot_id();
    auto result = JsonValue::object();
    result.o["schema_version"] = 1;
    result.o["resource"] = "module_rebuild_status";
    result.o["read_only"] = true;
    result.o["read_status"] = status;
    result.o["error"] = error;
    result.o["intent_read_status"] = latch_status;
    result.o["intent_error"] = latch_error;
    result.o["intent"] = latch_status == "valid" ? latch_json(latch) : JsonValue{};
    const bool context_ok = !before.empty() && before == after;
    result.o["boot_id"] = context_ok ? JsonValue(before) : JsonValue{};
    result.o["context_consistent"] = context_ok;
    result.o["runtime_verified"] = false;
    result.o["owner_verified"] = false;
    result.o["atomic_runtime_publication"] = false;
    result.o["history_snapshot_atomic"] = false;
    result.o["admission_is_authorization"] = false;
    result.o["durability_confirmation"] = "not_observable_from_read";
    result.o["evidence_scope"] = "diagnostic_plan_and_legacy_call_acknowledgments";
    const bool readable = (status == "valid" || status == "missing") &&
                          (latch_status == "valid" || latch_status == "missing");
    const bool same_boot =
        readable && history.latest && context_ok && history.latest->boot_id == before;
    result.o["same_boot"] =
        context_ok && readable && history.latest ? JsonValue(same_boot) : JsonValue{};
    std::string guard_error;
    const bool admission = context_ok && readable && allowed(history, latch, before, guard_error);
    result.o["rebuild_admission"] = "blocked_unresolved_same_boot";
    if (!context_ok || !readable)
        result.o["rebuild_admission"] = "unknown_blocked";
    else if (admission)
        result.o["rebuild_admission"] = "allowed_by_retained_history_only";
    result.o["guard_reason"] = guard_error;
    result.o["latest"] = history.latest ? module_rebuild_record_json(*history.latest) : JsonValue{};
    result.o["acknowledged"] =
        history.acknowledged ? module_rebuild_record_json(*history.acknowledged) : JsonValue{};
    return {context_ok && readable ? 0 : 1, std::move(result)};
}

int print_module_rebuild_status() {
    const auto result = query_module_rebuild_status();
    std::cout << stringify_json(result.value) << '\n';
    return std::cout.good() ? result.exit_code : 1;
}

bool rebuild_modules_recorded(const ModuleMountPlan& plan,
                              const std::vector<std::string>& module_ids,
                              const std::function<bool(std::string&)>& reset,
                              const std::function<bool(const ModuleMountPlan::Mapping&)>& add,
                              const std::function<bool(const std::string&)>& hide,
                              ModuleRebuildOutcome& outcome, std::string& error) {
    outcome = ModuleRebuildOutcome::Unchanged;
    if (!reset || !add || !hide) {
        error = "invalid module rebuild callbacks";
        errno = EINVAL;
        return false;
    }
    if (module_ids.size() > MODULE_REBUILD_MAX_ACTIONS ||
        plan.mappings.size() > MODULE_REBUILD_MAX_ACTIONS ||
        plan.hide.size() > MODULE_REBUILD_MAX_ACTIONS - plan.mappings.size()) {
        error = "module rebuild action capacity exceeded";
        errno = EOVERFLOW;
        return false;
    }
    ModuleRebuildRecord entry;
    entry.id = std::string(32, '0');
    entry.boot_id = runtime_boot_id();
    entry.module_ids = module_ids;
    entry.plan = plan;
    // Validate and bound even the first candidate before creating journal files.
    ModuleRebuildHistory candidate;
    candidate.latest = entry;
    if (encode_module_rebuild_history(candidate).empty()) {
        error = "invalid or oversized module rebuild plan";
        errno = EOVERFLOW;
        return false;
    }
    if (!prepare_private_directory(runtime_data_dir(), error))
        return false;
    const Descriptor lock(::open((runtime_data_dir() / "module_rebuild_history.lock").c_str(),
                                 O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
    struct stat metadata{};
    if (lock.get() < 0 || ::fstat(lock.get(), &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_nlink != 1) {
        error = "cannot open private module rebuild lock";
        errno = EIO;
        return false;
    }
    if (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
        error = "module rebuild running or lock unavailable";
        return false;
    }
    ModuleRebuildHistory history;
    const auto status = read_module_rebuild_history(history, error);
    if (status != "valid" && status != "missing") {
        errno = EIO;
        return false;
    }
    Latch latch;
    const auto latch_status = read_latch(latch, error);
    if (latch_status != "valid" && latch_status != "missing") {
        errno = EIO;
        return false;
    }
    if (!allowed(history, latch, entry.boot_id, error))
        return false;
    entry.id.clear();
    if (!new_id(entry.id)) {
        error = "cannot allocate module rebuild identity";
        return false;
    }
    if ((history.latest && history.latest->id == entry.id) ||
        (history.acknowledged && history.acknowledged->id == entry.id)) {
        error = "module rebuild identity collision";
        errno = EEXIST;
        return false;
    }
    history.latest = entry;
    // Admission must reserve enough space for both new acknowledged copies,
    // not just the smaller pending document with the previous acknowledgment.
    auto complete = entry;
    complete.phase = "acknowledged";
    complete.reset_acknowledged = true;
    complete.mappings_acknowledged = plan.mappings.size();
    complete.hides_acknowledged = plan.hide.size();
    ModuleRebuildHistory terminal;
    terminal.latest = complete;
    terminal.acknowledged = complete;
    // A smaller candidate plus a larger previous acknowledged plan can make
    // the partial receipt the largest document. Reserve maximal counters,
    // errno, and longest stage text while retaining that previous plan too.
    auto worst_partial = complete;
    worst_partial.phase = "partial_or_unknown";
    worst_partial.failed_stage = "exception";
    worst_partial.error_number = 4095;
    auto partial_history = history;
    partial_history.latest = worst_partial;
    if (encode_module_rebuild_history(terminal).empty() ||
        encode_module_rebuild_history(partial_history).empty()) {
        error = "module rebuild terminal receipt capacity exceeded";
        errno = EOVERFLOW;
        return false;
    }
    if (!write_history(history, error) || !write_latch(entry, error))
        return false;
    if (runtime_boot_id() != entry.boot_id) {
        error = "boot context changed after intent; no reset attempted";
        errno = ESTALE;
        return false;
    }
    outcome = ModuleRebuildOutcome::PartialOrUnknown;
    auto& current = *history.latest;
    std::string failure_detail;
    try {
        errno = 0;
        if (!reset(failure_detail)) {
            current.failed_stage = "reset";
            current.error_number = errno ? errno : EIO;
        } else {
            current.reset_acknowledged = true;
            for (const auto& mapping : plan.mappings) {
                errno = 0;
                if (!add(mapping)) {
                    current.failed_stage = "mapping";
                    current.failed_index = static_cast<int>(current.mappings_acknowledged);
                    current.error_number = errno ? errno : EIO;
                    break;
                }
                ++current.mappings_acknowledged;
            }
            if (current.failed_stage.empty())
                for (const auto& path : plan.hide) {
                    errno = 0;
                    if (!hide(path)) {
                        current.failed_stage = "hide";
                        current.failed_index = static_cast<int>(current.hides_acknowledged);
                        current.error_number = errno ? errno : EIO;
                        break;
                    }
                    ++current.hides_acknowledged;
                }
        }
    } catch (...) {
        current.failed_stage = "exception";
        current.failed_index = -1;
        current.error_number = EIO;
    }
    current.error_number = std::clamp(current.error_number, 0, 4095);
    const bool acknowledged = current.failed_stage.empty();
    current.phase = acknowledged ? "acknowledged" : "partial_or_unknown";
    if (acknowledged) {
        outcome = ModuleRebuildOutcome::Applied;
        history.acknowledged = current;
    }
    std::string receipt_error;
    if (!write_history(history, receipt_error)) {
        error = std::string(acknowledged ? "module calls acknowledged; "
                                         : "module rebuild partial or unknown; ") +
                "receipt persistence unconfirmed: " + receipt_error;
        errno = EIO;
        return false;
    }
    if (!acknowledged) {
        error = "module rebuild partial or unknown at " + current.failed_stage;
        if (current.failed_index >= 0)
            error += " index " + std::to_string(current.failed_index);
        if (!failure_detail.empty())
            error += ": " + failure_detail;
        error += "; no following mutations, cleanup or retry attempted";
        errno = current.error_number;
        return false;
    }
    // A visible acknowledged file alone cannot prove that its earlier final
    // fsync/readback succeeded. Retain this independently durable intent on
    // every failed receipt; only this confirmed path may remove it.
    if (::unlink(latch_path().c_str()) != 0 || !sync_private_directory(runtime_data_dir(), error)) {
        if (error.empty())
            error = "module calls acknowledged; intent removal unconfirmed";
        else
            error = "module calls acknowledged; intent removal unconfirmed: " + error;
        return false;
    }
    error.clear();
    return true;
}
}  // namespace sumhp::mount::sumh
