#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <set>
#include "../../../../common/utf8.hpp"
#include "core/checked_document.hpp"
#include "core/profile.hpp"
#include "core/runtime.hpp"
#include "core/state_ledger.hpp"
#include "durable_file.hpp"

namespace sumhp::profiles {
namespace {
std::filesystem::path bank_path() {
    return runtime_data_dir() / "profiles.json";
}
bool fields(const JsonValue& value, const std::set<std::string>& expected) {
    if (!value.is_object() || value.o.size() != expected.size())
        return false;
    return std::all_of(value.o.begin(), value.o.end(),
                       [&](const auto& item) { return expected.count(item.first) != 0; });
}
bool valid_revision(const std::string& revision) {
    return revision.size() == 74 && revision.compare(0, 10, "sha256-v1:") == 0 &&
           revision.find_first_not_of("0123456789abcdef", 10) == std::string::npos;
}
std::string bank_revision(const std::string& bytes, bool missing) {
    const auto value = state::revision_for_bytes(
        "sumhp-profile-bank-v1\n" + (missing ? std::string("missing") : "present\n" + bytes));
    return valid_revision(value) ? value : std::string{};
}
JsonValue bank_document(const JsonValue& profiles) {
    auto document = JsonValue::object();
    document.o["schema_version"] = 1;
    document.o["kind"] = "sumh.profile.bank";
    document.o["profiles"] = profiles;
    return document;
}
bool parse_bank(const std::string& bytes, JsonValue& profiles, std::string& error) {
    JsonValue input;
    if (bytes.size() > BANK_MAX_BYTES || !ksud::text::valid_utf8(bytes) ||
        !json::Parser(bytes, true).parse(input, error) ||
        !fields(input, {"schema_version", "kind", "profiles"}) ||
        !input.at("schema_version").is_number() || input.at("schema_version").n != 1 ||
        !input.at("kind").is_string() || input.at("kind").s != "sumh.profile.bank" ||
        !input.at("profiles").is_object() || input.at("profiles").o.size() > BANK_MAX_PROFILES) {
        if (error.empty())
            error = "invalid or unsupported profile bank format or capacity";
        return false;
    }
    auto candidate = JsonValue::object();
    for (const auto& item : input.at("profiles").o) {
        JsonValue profile;
        if (!valid_profile_id(item.first) ||
            !parse_profile(stringify_json(item.second), profile, error)) {
            if (error.empty())
                error = "invalid profile ID in bank";
            return false;
        }
        candidate.o.emplace(item.first, std::move(profile));
    }
    if (stringify_json(bank_document(candidate)).size() + 1 > BANK_MAX_BYTES) {
        error = "canonical profile bank exceeds byte capacity";
        return false;
    }
    profiles = std::move(candidate);
    error.clear();
    return true;
}
bool metadata_text(const JsonValue& value, std::size_t maximum, bool nonempty) {
    return value.is_string() && (!nonempty || !value.s.empty()) && value.s.size() <= maximum &&
           value.s.find('\0') == std::string::npos && ksud::text::valid_utf8(value.s);
}
bool validate_request(const JsonValue& request, JsonValue& profile, std::string& error) {
    const auto fail = [&](const char* detail) {
        error = detail;
        return false;
    };
    if (!request.is_object() || !request.at("schema_version").is_number() ||
        request.at("schema_version").n != 1 || !request.at("action").is_string() ||
        !request.at("id").is_string() || !valid_profile_id(request.at("id").s) ||
        !request.at("bank_revision").is_string() || !valid_revision(request.at("bank_revision").s))
        return fail("invalid profile mutation identity, revision or schema");
    const auto& action = request.at("action").s;
    std::set<std::string> expected{"schema_version", "action", "id", "bank_revision"};
    if (action == "create") {
        expected.emplace("profile");
    } else if (action == "clone" || action == "rename" || action == "remove") {
        expected.emplace("profile_revision");
        if (!request.at("profile_revision").is_string() ||
            !valid_revision(request.at("profile_revision").s))
            return fail("invalid expected profile revision");
        if (action != "remove") {
            expected.emplace("name");
            expected.emplace("note");
            if (!metadata_text(request.at("name"), 128, true) ||
                !metadata_text(request.at("note"), 1024, false))
                return fail("invalid profile name or note");
        }
        if (action == "clone") {
            expected.emplace("source_id");
            if (!request.at("source_id").is_string() ||
                !valid_profile_id(request.at("source_id").s))
                return fail("invalid source profile ID");
        }
    } else
        return fail("unsupported profile mutation action");
    if (!fields(request, expected) || stringify_json(request).size() > PROFILE_INPUT_MAX_BYTES)
        return fail("invalid profile mutation fields or capacity");
    if (action == "create" && !parse_profile(stringify_json(request.at("profile")), profile, error))
        return false;
    error.clear();
    return true;
}
class BankLock {
    int descriptor = -1;

public:
    BankLock() = default;
    BankLock(const BankLock&) = delete;
    BankLock& operator=(const BankLock&) = delete;
    BankLock(BankLock&&) = delete;
    BankLock& operator=(BankLock&&) = delete;
    ~BankLock() {
        if (descriptor >= 0)
            ::close(descriptor);
    }
    bool acquire(std::string& error) {
        if (!prepare_private_directory(runtime_data_dir(), error))
            return false;
        descriptor = ::open((runtime_data_dir() / "profiles.lock").c_str(),
                            O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        if (descriptor < 0) {
            error = "cannot open private profile bank lock";
            return false;
        }
        struct stat metadata{};
        if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
            metadata.st_nlink != 1 || metadata.st_uid != 0 || (metadata.st_mode & 07777) != 0600) {
            error = "invalid private profile bank lock";
            errno = EIO;
            return false;
        }
        if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
            error = "profile bank is busy or unavailable";
            return false;
        }
        return true;
    }
};
JsonValue receipt(const JsonValue& request) {
    auto result = JsonValue::object();
    result.o["schema_version"] = 1;
    result.o["kind"] = "sumh.profile.mutation";
    result.o["action"] = request.at("action").is_string() ? request.at("action") : JsonValue{};
    result.o["id"] = request.at("id").is_string() && valid_profile_id(request.at("id").s)
                         ? request.at("id")
                         : JsonValue{};
    result.o["status"] = "not_saved";
    result.o["reason"] = "invalid_request";
    result.o["detail"] = "";
    result.o["bank_revision"] = JsonValue{};
    result.o["profile_revision"] = JsonValue{};
    result.o["previous_bank_revision"] = JsonValue{};
    result.o["saved"] = false;
    result.o["applied"] = false;
    result.o["active_configuration_saved"] = false;
    result.o["profile_active"] = false;
    result.o["durability_confirmed"] = false;
    result.o["readback_confirmed"] = false;
    return result;
}
}  // namespace

bool read_bank(Bank& bank, std::string& error) {
    const auto source = checked::read_document(bank_path(), BANK_MAX_BYTES);
    if (source.status != "valid" && source.status != "missing") {
        error = "cannot read profile bank";
        errno = source.error ? source.error : EIO;
        return false;
    }
    Bank candidate;
    candidate.missing = source.status == "missing";
    candidate.bytes = source.bytes;
    if (!candidate.missing && !parse_bank(candidate.bytes, candidate.profiles, error)) {
        errno = EINVAL;
        return false;
    }
    candidate.revision = bank_revision(candidate.bytes, candidate.missing);
    if (candidate.revision.empty()) {
        error = "profile bank revision unavailable";
        errno = EIO;
        return false;
    }
    bank = std::move(candidate);
    error.clear();
    return true;
}

std::string profile_revision(const JsonValue& profile) {
    JsonValue canonical;
    std::string error;
    if (!parse_profile(stringify_json(profile), canonical, error))
        return {};
    const auto revision =
        state::revision_for_bytes("sumhp-profile-document-v1\n" + stringify_json(canonical));
    return valid_revision(revision) ? revision : std::string{};
}

JsonValue mutate_bank(const JsonValue& request) {
    auto result = receipt(request);
    const auto finish = [&](const char* status, const char* reason, const std::string& detail) {
        result.o["status"] = status;
        result.o["reason"] = reason;
        result.o["detail"] = detail;
        return result;
    };
    std::string error;
    JsonValue supplied;
    if (!validate_request(request, supplied, error))
        return finish("not_saved", "invalid_request", error);
    const auto& action = request.at("action").s;
    const auto& id = request.at("id").s;
    BankLock lock;
    if (!lock.acquire(error))
        return finish("not_saved", "bank_lock_unavailable", error);
    // The installed /data/adb anchor predates this store. prepare may create
    // both ksu and sumhp, whereas replace_file_durable only syncs sumhp.
    // Sync each child before its parent entry while holding the bank lock.
    // Always do this, including when another first creator made the directory.
    const auto directory = runtime_data_dir();
    for (const auto& path :
         {directory, directory.parent_path(), directory.parent_path().parent_path()}) {
        if (!sync_private_directory(path, error))
            return finish("not_saved", "bank_directory_durability_unconfirmed", error);
    }
    Bank before;
    if (!read_bank(before, error))
        return finish("not_saved", "bank_unreadable", error);
    result.o["previous_bank_revision"] = before.revision;
    result.o["bank_revision"] = before.revision;
    if (before.revision != request.at("bank_revision").s)
        return finish("not_saved", "bank_revision_conflict",
                      "query the bank before a new explicit request");
    auto profiles = before.profiles;
    if (action == "create" || action == "clone") {
        if (profiles.contains(id))
            return finish("not_saved", "profile_id_conflict", "destination profile already exists");
        if (profiles.o.size() >= BANK_MAX_PROFILES)
            return finish("not_saved", "bank_capacity", "profile count limit reached");
    }
    if (action == "create") {
        profiles.o.emplace(id, std::move(supplied));
    } else {
        const auto& source_id = action == "clone" ? request.at("source_id").s : id;
        const auto found = profiles.o.find(source_id);
        if (found == profiles.o.end())
            return finish("not_saved", "profile_missing", "source profile does not exist");
        const auto revision = profile_revision(found->second);
        if (revision.empty())
            return finish("not_saved", "revision_unavailable", "profile revision unavailable");
        if (action != "clone")
            result.o["profile_revision"] = revision;
        if (revision != request.at("profile_revision").s)
            return finish("not_saved", "profile_revision_conflict",
                          "query the source profile before a new explicit request");
        if (action == "remove") {
            profiles.o.erase(found);
        } else {
            auto candidate = found->second;
            candidate.o["name"] = request.at("name");
            candidate.o["note"] = request.at("note");
            JsonValue validated;
            if (!parse_profile(stringify_json(candidate), validated, error))
                return finish("not_saved", "invalid_profile", error);
            profiles.o[id] = std::move(validated);
        }
    }
    const auto bytes = stringify_json(bank_document(profiles)) + "\n";
    if (bytes.size() > BANK_MAX_BYTES)
        return finish("not_saved", "bank_capacity", "profile bank byte limit reached");
    const auto candidate_revision = bank_revision(bytes, false);
    const auto candidate_profile_revision =
        action == "remove" ? std::string{} : profile_revision(profiles.at(id));
    if (candidate_revision.empty() || (action != "remove" && candidate_profile_revision.empty()))
        return finish("not_saved", "revision_unavailable", "candidate revision unavailable");
    // The lock serializes cooperating writers; exact bytes also catch an
    // observed noncooperating replacement. This is not a filesystem CAS.
    Bank observed;
    if (!read_bank(observed, error))
        return finish("not_saved", "bank_recheck_failed", error);
    if (observed.missing != before.missing || observed.bytes != before.bytes ||
        observed.revision != before.revision)
        return finish("not_saved", "bank_changed",
                      "profile bank changed during request; query before retrying");
    if (stringify_json(profiles) == stringify_json(before.profiles)) {
        result.o["saved"] = true;
        result.o["readback_confirmed"] = true;
        result.o["profile_revision"] = candidate_profile_revision.empty()
                                           ? JsonValue{}
                                           : JsonValue(candidate_profile_revision);
        return finish(
            "unchanged", "already_saved",
            "existing profile confirmed; directories synced, no profile file replaced or synced");
    }
    const auto saved = ksud::replace_file_durable(bank_path(), bytes);
    result.o["durability_confirmed"] = saved.complete();
    if (!saved.replaced())
        return finish("not_saved", "bank_not_replaced", "profile bank was not replaced");
    // After replacement a failure is unknown, including fsync or readback;
    // there is deliberately no rollback, retry, cleanup or active-state write.
    result.o["bank_revision"] = JsonValue{};
    result.o["profile_revision"] = JsonValue{};
    const auto readback = checked::read_document(bank_path(), BANK_MAX_BYTES);
    if (readback.status == "valid" && readback.bytes == bytes) {
        result.o["readback_confirmed"] = true;
        result.o["bank_revision"] = candidate_revision;
        result.o["profile_revision"] = candidate_profile_revision.empty()
                                           ? JsonValue{}
                                           : JsonValue(candidate_profile_revision);
    }
    if (!saved.complete())
        return finish("unknown", "replaced_durability_unconfirmed",
                      "query the known profile ID; do not automatically repeat the mutation");
    if (!result.at("readback_confirmed").b)
        return finish("unknown", "replaced_readback_unconfirmed",
                      "query the known profile ID; replacement readback did not match");
    result.o["saved"] = true;
    return finish("saved", "inactive_profile_saved",
                  "active configuration and runtime were not changed");
}
}  // namespace sumhp::profiles
