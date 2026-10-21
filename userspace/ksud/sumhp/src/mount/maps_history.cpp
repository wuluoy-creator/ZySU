#include "mount/maps_history.hpp"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include "core/runtime.hpp"
#include "core/state_ledger.hpp"
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
    return runtime_data_dir() / "maps_submission_history.json";
}
std::string read_bytes(std::string& bytes, std::string& error) {
    const Descriptor fd(
        ::open(history_path().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (fd.get() < 0) {
        if (errno == ENOENT)
            return "missing";
        error = std::strerror(errno);
        return "unreadable";
    }
    struct stat metadata{};
    if (::fstat(fd.get(), &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 ||
        metadata.st_size < 0 ||
        static_cast<std::uint64_t>(metadata.st_size) > MAPS_HISTORY_MAX_BYTES) {
        error = "invalid Maps history file type, link count or capacity";
        errno = EIO;
        return "unreadable";
    }
    std::string result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    char buffer[4096];
    while (result.size() <= MAPS_HISTORY_MAX_BYTES) {
        if (std::chrono::steady_clock::now() >= deadline) {
            error = "Maps history read budget exceeded";
            errno = ETIMEDOUT;
            return "unreadable";
        }
        const auto n = ::read(fd.get(), buffer,
                              std::min(sizeof(buffer), MAPS_HISTORY_MAX_BYTES + 1 - result.size()));
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
    if (result.size() > MAPS_HISTORY_MAX_BYTES) {
        error = "Maps history exceeds capacity";
        errno = EOVERFLOW;
        return "unreadable";
    }
    bytes = std::move(result);
    return "valid";
}
bool write_history(const MapsHistory& value, std::string& error) {
    const auto bytes = encode_maps_history(value);
    MapsHistory checked;
    if (!parse_maps_history(bytes, checked, error)) {
        errno = EINVAL;
        return false;
    }
    if (!ksud::write_file_durable(history_path(), bytes, error))
        return false;
    std::string actual;
    if (read_bytes(actual, error) != "valid" || actual != bytes) {
        error = "Maps submission receipt committed but readback unconfirmed";
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
    static constexpr char digits[] = "0123456789abcdef";
    for (auto byte : bytes) {
        id += digits[byte >> 4];
        id += digits[byte & 15];
    }
    return true;
}
}  // namespace

std::string read_maps_history(MapsHistory& out, std::string& error) {
    error.clear();
    std::string bytes;
    auto status = read_bytes(bytes, error);
    if (status != "valid")
        return status;
    return parse_maps_history(bytes, out, error) ? "valid" : "corrupt_or_unsupported";
}

bool maps_automatic_restore_allowed(std::string& error) {
    MapsHistory history;
    const auto status = read_maps_history(history, error);
    if (status != "valid" && status != "missing") {
        errno = EIO;
        return false;
    }
    const auto boot = runtime_boot_id();
    if (boot.empty()) {
        error = "boot identity unavailable for automatic Maps restoration";
        errno = EIO;
        return false;
    }
    if (history.latest && history.latest->phase == "pending" && history.latest->boot_id == boot) {
        error = "automatic Maps restoration blocked by unresolved submission " +
                history.latest->id + "; query 'sumh maps preview' before an explicit new apply";
        errno = EBUSY;
        return false;
    }
    return true;
}

MapsSubmissionTracker::MapsSubmissionTracker() {
    void* value = ::mmap(nullptr, sizeof(unsigned), PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (value != MAP_FAILED) {
        shared_ = static_cast<unsigned*>(value);
        mark(ManagedMapsRestoreState::Unchanged);
    }
}
MapsSubmissionTracker::~MapsSubmissionTracker() {
    if (shared_)
        ::munmap(const_cast<unsigned*>(shared_), sizeof(unsigned));
}
bool MapsSubmissionTracker::valid() const {
    return shared_ != nullptr;
}
void MapsSubmissionTracker::mark(ManagedMapsRestoreState value) {
    if (shared_)
        *shared_ = static_cast<unsigned>(value);
}
ManagedMapsRestoreState MapsSubmissionTracker::state() const {
    if (!shared_)
        return ManagedMapsRestoreState::Unknown;
    const unsigned value = *shared_;
    if (value == static_cast<unsigned>(ManagedMapsRestoreState::Unchanged))
        return ManagedMapsRestoreState::Unchanged;
    if (value == static_cast<unsigned>(ManagedMapsRestoreState::Replaced))
        return ManagedMapsRestoreState::Replaced;
    return ManagedMapsRestoreState::Unknown;
}

bool replace_managed_maps_recorded(const std::vector<MapsPathRule>& definitions,
                                   const std::vector<::sumhp::sumh::MapsRule>& identities,
                                   MapsSubmissionTracker& tracker, std::string& error,
                                   RestoreTrigger trigger) {
    if (!tracker.valid() || definitions.size() != identities.size() ||
        !validate_maps_path_rules(definitions, error)) {
        if (error.empty())
            error = "invalid Maps submission tracking input";
        errno = EINVAL;
        return false;
    }
    if (!prepare_private_directory(runtime_data_dir(), error))
        return false;
    const Descriptor lock(::open((runtime_data_dir() / "maps_submission_history.lock").c_str(),
                                 O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
    struct stat metadata{};
    if (lock.get() < 0 || ::fstat(lock.get(), &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_nlink != 1) {
        error = "cannot open private Maps submission lock";
        errno = EIO;
        return false;
    }
    if (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
        error = "Maps submission already running or lock unavailable";
        return false;
    }
    MapsHistory history;
    const auto status = read_maps_history(history, error);
    if (status != "valid" && status != "missing") {
        errno = EIO;
        return false;
    }
    MapsSubmission entry;
    entry.boot_id = runtime_boot_id();
    entry.phase = "pending";
    if (history.latest && history.latest->phase == "pending" &&
        history.latest->boot_id == entry.boot_id) {
        if (trigger == RestoreTrigger::Automatic) {
            error = "automatic Maps restoration blocked by unresolved submission " +
                    history.latest->id + "; query 'sumh maps preview' before an explicit new apply";
            errno = EBUSY;
            return false;
        }
        entry.supersedes_pending_id = history.latest->id;
    }
    entry.definition_revision =
        state::revision_for_bytes(stringify_json(maps_path_rules_json(definitions)));
    struct stat own{};
    struct stat init{};
    if (entry.boot_id.empty() || !new_id(entry.id) || entry.definition_revision.empty() ||
        ::stat("/proc/self/ns/mnt", &own) != 0 || ::stat("/proc/1/ns/mnt", &init) != 0 ||
        !own.st_ino || own.st_ino != init.st_ino || own.st_dev != init.st_dev) {
        error = "cannot establish Maps submission identity or init namespace";
        errno = EIO;
        return false;
    }
    entry.namespace_ino = std::to_string(own.st_ino);
    entry.definitions = definitions;
    for (std::size_t i = 0; i < identities.size(); ++i) {
        const auto& value = identities[i];
        if (value.spoofed_path != definitions[i].replacement_path) {
            error = "Maps submission identity/path mismatch";
            errno = EINVAL;
            return false;
        }
        entry.identities.push_back(
            {value.target_ino, value.target_dev, value.spoofed_ino, value.spoofed_dev});
    }
    ::sumhp::sumh::RuntimeState before;
    if (::sumhp::sumh::runtime_state(before) && before.instance_id && before.generation) {
        entry.instance_id = std::to_string(before.instance_id);
        entry.generation_before = std::to_string(before.generation);
    }
    history.latest = entry;
    if (!write_history(history, error))
        return false;  // no kernel change yet
    tracker.mark(ManagedMapsRestoreState::Unknown);
    errno = 0;
    bool submitted = false;
    const bool accepted = ::sumhp::sumh::replace_managed_maps_rules_checked(identities, submitted);
    const bool applied = accepted && submitted;
    int failure = 0;
    if (!applied) {
        if (accepted)
            failure = EOPNOTSUPP;
        else
            failure = errno ? errno : EIO;
    }
    tracker.mark(applied ? ManagedMapsRestoreState::Replaced : ManagedMapsRestoreState::Unchanged);
    history.latest->phase = applied ? "acknowledged" : "rejected";
    history.latest->error = std::clamp(failure, 0, 4095);
    if (applied) {
        ::sumhp::sumh::RuntimeState after;
        // These are bracketing observations, NOT the ioctl's commit token.
        if (!entry.instance_id.empty() && ::sumhp::sumh::runtime_state(after) && after.generation &&
            std::to_string(after.instance_id) == entry.instance_id)
            history.latest->generation_after = std::to_string(after.generation);
        history.confirmed = history.latest;
    }
    std::string history_error;
    if (!write_history(history, history_error)) {
        error = std::string(applied ? "partial: managed Maps replaced; "
                                    : "managed Maps replacement rejected; ") +
                "submission result persistence unconfirmed: " + history_error;
        errno = EIO;
        return false;
    }
    if (!applied) {
        error = "failed to replace managed Maps rules: " + std::string(std::strerror(failure));
        errno = failure;
        return false;
    }
    error.clear();
    return true;
}
}  // namespace sumhp::mount::sumh
