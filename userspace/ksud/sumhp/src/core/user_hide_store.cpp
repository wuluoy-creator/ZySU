#include "core/user_hide_store.hpp"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include "core/checked_document.hpp"
#include "core/runtime.hpp"
#include "core/state_ledger.hpp"
#include "durable_file.hpp"

namespace sumhp::hide {
namespace {
std::filesystem::path store_path() {
    return runtime_data_dir() / "user_hide_rules.json";
}
bool revision(const std::string& bytes, bool missing, std::string& result, std::string& error) {
    const auto digest = state::revision_for_bytes(
        "sumhp-user-hide-document-v1\n" + (missing ? std::string("missing") : "present\n" + bytes));
    if (digest.size() != 74 || digest.compare(0, 10, "sha256-v1:") != 0 ||
        digest.find_first_not_of("0123456789abcdef", 10) != std::string::npos) {
        error = "user hide document revision unavailable";
        errno = EIO;
        return false;
    }
    result = digest;
    return true;
}
bool current_matches(const Stored& expected, std::string& error) {
    Stored actual;
    if (!read_store(actual, error))
        return false;
    if (actual.missing != expected.missing || actual.bytes != expected.bytes ||
        actual.revision != expected.revision) {
        error = "user hide definitions changed during transaction";
        errno = EAGAIN;
        return false;
    }
    return true;
}
bool backup_legacy(const Stored& original, std::string& error) {
    const auto path = runtime_data_dir() / "user_hide_rules.legacy.bak";
    const auto backup = checked::read_document(path, HIDE_MAX_BYTES);
    if (backup.status != "missing" &&
        (backup.status != "valid" || backup.bytes != original.bytes)) {
        error = "legacy user hide backup differs or is unreadable; preserving both files";
        errno = EIO;
        return false;
    }
    // An identical visible backup may follow an unconfirmed previous fsync.
    // Re-sync it, never overwrite different bytes, and confirm exact readback.
    const auto saved = ksud::replace_file_durable(path, original.bytes);
    if (!saved.complete()) {
        error = "legacy user hide backup durability unconfirmed";
        errno = saved.error_number ? saved.error_number : EIO;
        return false;
    }
    const auto readback = checked::read_document(path, HIDE_MAX_BYTES);
    if (readback.status != "valid" || readback.bytes != original.bytes) {
        error = "legacy user hide backup readback unconfirmed";
        errno = EIO;
        return false;
    }
    return true;
}
}  // namespace

bool read_store(Stored& stored, std::string& error) {
    const auto source = checked::read_document(store_path(), HIDE_MAX_BYTES);
    if (source.status != "missing" && source.status != "valid") {
        error = "cannot read user hide definitions";
        errno = source.error ? source.error : EIO;
        return false;
    }
    Stored parsed;
    parsed.missing = source.status == "missing";
    parsed.bytes = source.bytes;
    if (!parsed.missing && !parse_document(parsed.bytes, parsed.document, error)) {
        errno = EINVAL;
        return false;
    }
    if (!revision(parsed.bytes, parsed.missing, parsed.revision, error))
        return false;
    stored = std::move(parsed);
    error.clear();
    return true;
}

struct Transaction::Impl {
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() {
        if (lock_fd >= 0)
            ::close(lock_fd);
    }

private:
    friend class Transaction;
    int lock_fd = -1;
    bool attempted = false;
    bool active = false;
    bool uncertain = false;
    Stored stored;
};
Transaction::Transaction() : impl_(std::make_unique<Impl>()) {}
Transaction::~Transaction() = default;
bool Transaction::begin(std::string& error) {
    if (impl_->attempted) {
        error = "user hide transaction cannot acquire twice";
        errno = EALREADY;
        return false;
    }
    impl_->attempted = true;
    if (!prepare_private_directory(runtime_data_dir(), error))
        return false;
    impl_->lock_fd = ::open((runtime_data_dir() / "user_hide_rules.lock").c_str(),
                            O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (impl_->lock_fd < 0) {
        error = "cannot open user hide transaction lock";
        return false;
    }
    struct stat metadata{};
    if (::fstat(impl_->lock_fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_nlink != 1 || metadata.st_uid != 0 || (metadata.st_mode & 07777) != 0600) {
        error = "invalid private user hide transaction lock";
        errno = EIO;
        return false;
    }
    if (::flock(impl_->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        error = "user hide transaction is busy or unavailable";
        return false;
    }
    if (!read_store(impl_->stored, error))
        return false;
    impl_->active = true;
    error.clear();
    return true;
}
const Stored& Transaction::stored() const {
    return impl_->stored;
}
SaveResult Transaction::save(const Document& document, std::string& error) {
    if (impl_->uncertain) {
        error = "previous user hide save is unconfirmed; query before a new transaction";
        errno = EIO;
        return SaveResult::Unknown;
    }
    if (!impl_->active) {
        error = "user hide transaction not active";
        errno = EINVAL;
        return SaveResult::NotSaved;
    }
    if (document.rules.size() > HIDE_MAX_RULES ||
        (document.source_schema != 0 && document.source_schema != 1)) {
        error = "invalid candidate user hide format or capacity";
        errno = EINVAL;
        return SaveResult::NotSaved;
    }
    Stored candidate;
    candidate.bytes = encode_document(document);
    if (!parse_document(candidate.bytes, candidate.document, error)) {
        errno = EINVAL;
        return SaveResult::NotSaved;
    }
    if (!current_matches(impl_->stored, error))
        return SaveResult::NotSaved;
    if ((impl_->stored.missing && document.rules.empty()) ||
        (!impl_->stored.missing && impl_->stored.document.source_schema == 1 &&
         encode_document(impl_->stored.document) == candidate.bytes)) {
        error.clear();
        return SaveResult::Unchanged;
    }
    if (!revision(candidate.bytes, false, candidate.revision, error))
        return SaveResult::NotSaved;
    if (!impl_->stored.missing && impl_->stored.document.source_schema == 0) {
        if (!backup_legacy(impl_->stored, error) || !current_matches(impl_->stored, error))
            return SaveResult::NotSaved;
    }
    const auto saved = ksud::replace_file_durable(store_path(), candidate.bytes);
    if (!saved.complete()) {
        error = saved.replaced() ? "user hide definitions replaced but durability unconfirmed"
                                 : "user hide definitions were not saved";
        errno = saved.error_number ? saved.error_number : EIO;
        impl_->uncertain = saved.replaced();
        return impl_->uncertain ? SaveResult::Unknown : SaveResult::NotSaved;
    }
    const auto readback = checked::read_document(store_path(), HIDE_MAX_BYTES);
    if (readback.status != "valid" || readback.bytes != candidate.bytes) {
        error = "user hide definitions replaced but readback unconfirmed";
        errno = EIO;
        impl_->uncertain = true;
        return SaveResult::Unknown;
    }
    impl_->stored = std::move(candidate);
    error.clear();
    return SaveResult::Saved;
}
}  // namespace sumhp::hide
