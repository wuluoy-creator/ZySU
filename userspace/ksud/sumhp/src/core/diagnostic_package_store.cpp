#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <set>
#include "../../../../common/utf8.hpp"
#include "core/diagnostic_package.hpp"
#include "core/runtime.hpp"
#include "core/state_ledger.hpp"

namespace sumhp::diagnostic_package {
namespace {
class Descriptor {
public:
    explicit Descriptor(int value = -1) : value_(value) {}
    ~Descriptor() {
        if (value_ >= 0)
            ::close(value_);
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&&) = delete;
    Descriptor& operator=(Descriptor&&) = delete;
    [[nodiscard]] int get() const { return value_; }
    bool close_checked() {
        const int fd = value_;
        value_ = -1;
        return fd < 0 || ::close(fd) == 0;
    }

private:
    int value_;
};
std::filesystem::path bank_path() {
    return runtime_data_dir() / "diagnostic_packages.json";
}
std::filesystem::path stage_path() {
    return runtime_data_dir() / "diagnostic_packages.stage";
}
bool regular_private(const struct stat& value) {
    return S_ISREG(value.st_mode) && value.st_nlink == 1 && value.st_uid == 0 &&
           (value.st_mode & 07777) == 0600 && value.st_size >= 0;
}
bool same_file(const struct stat& left, const struct stat& right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           left.st_mode == right.st_mode && left.st_uid == right.st_uid &&
           left.st_nlink == right.st_nlink && left.st_size == right.st_size &&
           left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
           left.st_mtim.tv_nsec == right.st_mtim.tv_nsec;
}
// Validate the installed adb anchor and its two private-state descendants.
// Android's shared /data has different permissions; it is not this store's
// owned directory. Missing descendants are accepted only for a missing bank.
bool parents(bool allow_missing) {
    const auto directory = runtime_data_dir();
    for (const auto& path :
         {directory.parent_path().parent_path(), directory.parent_path(), directory}) {
        struct stat value{};
        if (::lstat(path.c_str(), &value) != 0) {
            if (allow_missing && errno == ENOENT)
                continue;
            return false;
        }
        if (!S_ISDIR(value.st_mode) || value.st_uid != 0 || (value.st_mode & 0022) != 0 ||
            (path == directory && (value.st_mode & 07777) != 0700))
            return false;
    }
    return true;
}
struct Source {
    bool missing = true;
    std::string bytes;
};
bool read_source(Source& output) {
    if (!parents(true))
        return false;
    const auto path = bank_path();
    Descriptor file{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (file.get() < 0) {
        if (errno != ENOENT)
            return false;
        output = {};
        return true;
    }
    if (!parents(false))
        return false;
    struct stat before{};
    if (::fstat(file.get(), &before) != 0 || !regular_private(before) ||
        static_cast<unsigned long long>(before.st_size) > bank_limit)
        return false;
    Source candidate{false, {}};
    char buffer[4096];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (candidate.bytes.size() <= bank_limit) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        const auto count = ::read(
            file.get(), buffer, std::min(sizeof(buffer), bank_limit + 1 - candidate.bytes.size()));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return false;
        if (count == 0)
            break;
        candidate.bytes.append(buffer, static_cast<std::size_t>(count));
    }
    struct stat after{};
    struct stat current{};
    if (candidate.bytes.size() > bank_limit ||
        candidate.bytes.size() != static_cast<std::size_t>(before.st_size) ||
        ::fstat(file.get(), &after) != 0 || ::lstat(path.c_str(), &current) != 0 ||
        !same_file(before, after) || !same_file(after, current) || !parents(false) ||
        !file.close_checked())
        return false;
    output = std::move(candidate);
    return true;
}
bool fields(const JsonValue& value, const std::set<std::string>& expected) {
    if (!value.is_object() || value.o.size() != expected.size())
        return false;
    return std::all_of(value.o.begin(), value.o.end(),
                       [&](const auto& item) { return expected.count(item.first) != 0; });
}
JsonValue bank_document(const JsonValue& packages) {
    auto value = JsonValue::object();
    value.o["schema_version"] = 1;
    value.o["kind"] = "sumh.diagnostic.package.bank";
    value.o["packages"] = packages;
    return value;
}
bool read_bank(Source& source, JsonValue& packages) {
    Source next;
    if (!read_source(next))
        return false;
    auto parsed = JsonValue::object();
    if (!next.missing) {
        JsonValue bank;
        std::string ignored;
        if (!ksud::text::valid_utf8(next.bytes) ||
            !json::Parser(next.bytes, true).parse(bank, ignored) ||
            !fields(bank, {"schema_version", "kind", "packages"}) ||
            !bank.at("schema_version").is_number() || bank.at("schema_version").n != 1 ||
            !bank.at("kind").is_string() || bank.at("kind").s != "sumh.diagnostic.package.bank" ||
            !bank.at("packages").is_object() || bank.at("packages").o.size() > package_count_limit)
            return false;
        for (const auto& item : bank.at("packages").o) {
            JsonValue canonical;
            if (!valid_id(item.first) ||
                !parse_package(stringify_json(item.second), canonical, ignored))
                return false;
            if (stringify_json(canonical) != stringify_json(item.second) ||
                stringify_json(canonical).size() + 1 > package_limit)
                return false;
            parsed.o.emplace(item.first, std::move(canonical));
        }
        if (stringify_json(bank_document(parsed)).size() + 1 > bank_limit)
            return false;
    }
    source = std::move(next);
    packages = std::move(parsed);
    return true;
}
std::string export_bytes(const JsonValue& package) {
    return stringify_json(package) + "\n";
}
std::string revision(const JsonValue& package) {
    const auto value = state::revision_for_bytes(export_bytes(package));
    return valid_revision(value) ? value : std::string{};
}
Reply receipt(const char* action, const std::string& id = {}) {
    Reply result;
    result.value = JsonValue::object();
    auto& value = result.value.o;
    value["schema_version"] = 1;
    value["kind"] = "sumh.diagnostic.package.result";
    value["action"] = action;
    value["id"] = valid_id(id) ? JsonValue(id) : JsonValue{};
    value["status"] = "not_saved";
    value["reason"] = "storage_unavailable";
    value["package_revision"] = JsonValue{};
    value["privacy_policy"] = JsonValue{};
    value["diagnostic_status"] = JsonValue{};
    value["saved"] = false;
    value["deleted"] = false;
    value["durability_confirmed"] = false;
    value["readback_confirmed"] = false;
    value["active_configuration_changed"] = false;
    value["runtime_changed"] = false;
    return result;
}
Reply finish(Reply result, int code, const char* status, const char* reason) {
    result.exit_code = code;
    result.value.o["status"] = status;
    result.value.o["reason"] = reason;
    return result;
}
void metadata(JsonValue& value, const JsonValue& package) {
    value.o["privacy_policy"] = package.at("privacy").at("policy");
    value.o["diagnostic_status"] = package.at("diagnostic").at("status");
}
bool sync_descriptor(int fd) {
    int result;
    do {
        result = ::fsync(fd);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}
bool reclaim_stage() {
    const auto path = stage_path();
    struct stat named{};
    if (::lstat(path.c_str(), &named) != 0)
        return errno == ENOENT;
    if (!regular_private(named) || static_cast<unsigned long long>(named.st_size) > bank_limit)
        return false;
    Descriptor file{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    struct stat opened{};
    if (file.get() < 0 || ::fstat(file.get(), &opened) != 0 || !same_file(named, opened) ||
        !file.close_checked())
        return false;
    return ::unlink(path.c_str()) == 0;
}
enum class Save : std::uint8_t { NotCommitted, Unconfirmed, Complete };
Save replace_bank(const std::string& bytes) {
    if (bytes.size() > bank_limit || !parents(false))
        return Save::NotCommitted;
    Descriptor directory{
        ::open(runtime_data_dir().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (directory.get() < 0 || !reclaim_stage())
        return Save::NotCommitted;
    const auto stage = stage_path();
    Descriptor file{::open(
        stage.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (file.get() < 0)
        return Save::NotCommitted;
    struct stat initial{};
    bool ok = ::fstat(file.get(), &initial) == 0 && regular_private(initial);
    std::size_t offset = 0;
    while (ok && offset < bytes.size()) {
        const auto count = ::write(file.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            ok = false;
            break;
        }
        offset += static_cast<std::size_t>(count);
    }
    if (ok)
        ok = sync_descriptor(file.get());
    if (!file.close_checked())
        ok = false;
    if (ok)
        ok = ::rename(stage.c_str(), bank_path().c_str()) == 0;
    if (!ok) {
        (void)::unlink(stage.c_str());
        return Save::NotCommitted;
    }
    if (!sync_descriptor(directory.get()) || !directory.close_checked())
        return Save::Unconfirmed;
    return Save::Complete;
}
Reply mutate(const std::string& id, const JsonValue* supplied, const std::string& expected) {
    const bool creating = supplied != nullptr;
    auto result = receipt(creating ? "create" : "remove", id);
    JsonValue package;
    std::string ignored;
    if (!valid_id(id) || (!creating && !valid_revision(expected)) ||
        (creating && (!parse_package(stringify_json(*supplied), package, ignored) ||
                      stringify_json(package).size() + 1 > package_limit)))
        return finish(std::move(result), 2, "invalid", "invalid_request");
    if (creating)
        metadata(result.value, package);
    // Preflight avoids following a known unsafe ancestor in the mkdir helper.
    if (!parents(true) || !prepare_private_directory(runtime_data_dir(), ignored) ||
        !parents(false))
        return finish(std::move(result), 5, "not_saved", "private_directory_unavailable");
    Descriptor const lock{::open((runtime_data_dir() / "diagnostic_packages.lock").c_str(),
                                 O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600)};
    struct stat lock_metadata{};
    if (lock.get() < 0 || ::fstat(lock.get(), &lock_metadata) != 0 ||
        !regular_private(lock_metadata) || ::flock(lock.get(), LOCK_EX | LOCK_NB) != 0)
        return finish(std::move(result), 5, "not_saved", "store_lock_unavailable");
    Source before;
    JsonValue packages;
    if (!read_bank(before, packages))
        return finish(std::move(result), 5, "not_saved", "store_unreadable");
    if (creating && packages.contains(id))
        return finish(std::move(result), 4, "conflict", "id_exists");
    if (!creating && !packages.contains(id))
        return finish(std::move(result), 3, "not_found", "package_missing");
    if (creating && packages.o.size() >= package_count_limit)
        return finish(std::move(result), 5, "not_saved", "store_capacity");
    if (!creating)
        package = packages.at(id);
    metadata(result.value, package);
    const auto package_revision = revision(package);
    if (package_revision.empty())
        return finish(std::move(result), 5, "not_saved", "revision_unavailable");
    result.value.o["package_revision"] = package_revision;
    if (!creating && expected != package_revision)
        return finish(std::move(result), 4, "conflict", "revision_conflict");
    if (creating)
        packages.o.emplace(id, package);
    else
        packages.o.erase(id);
    const auto bytes = stringify_json(bank_document(packages)) + "\n";
    if (bytes.size() > bank_limit)
        return finish(std::move(result), 5, "not_saved", "store_capacity");
    const auto directory = runtime_data_dir();
    for (const auto& path :
         {directory, directory.parent_path(), directory.parent_path().parent_path()}) {
        if (!sync_private_directory(path, ignored))
            return finish(std::move(result), 5, "not_saved", "directory_durability_unconfirmed");
    }
    Source observed;
    JsonValue observed_packages;
    if (!read_bank(observed, observed_packages))
        return finish(std::move(result), 5, "not_saved", "store_recheck_failed");
    if (observed.missing != before.missing || observed.bytes != before.bytes)
        return finish(std::move(result), 4, "conflict", "store_changed");
    const auto saved = replace_bank(bytes);
    if (saved == Save::NotCommitted)
        return finish(std::move(result), 5, "not_saved", "store_not_replaced");
    result.value.o["durability_confirmed"] = saved == Save::Complete;
    Source readback;
    JsonValue readback_packages;
    const bool matched =
        read_bank(readback, readback_packages) && !readback.missing && readback.bytes == bytes;
    result.value.o["readback_confirmed"] = matched;
    if (!matched)
        result.value.o["package_revision"] = JsonValue{};
    if (saved == Save::Unconfirmed)
        return finish(std::move(result), 5, "unknown", "replaced_durability_unconfirmed");
    if (!matched)
        return finish(std::move(result), 5, "unknown", "replaced_readback_unconfirmed");
    result.value.o[creating ? "saved" : "deleted"] = true;
    return finish(std::move(result), 0, creating ? "saved" : "deleted",
                  creating ? "package_saved" : "package_deleted");
}
}  // namespace

Reply list_packages() {
    auto result = receipt("list");
    Source source;
    JsonValue packages;
    if (!read_bank(source, packages))
        return finish(std::move(result), 5, "unavailable", "store_unreadable");
    auto items = JsonValue::array();
    for (const auto& item : packages.o) {
        const auto package_revision = revision(item.second);
        if (package_revision.empty())
            return finish(std::move(result), 5, "unavailable", "revision_unavailable");
        auto entry = JsonValue::object();
        entry.o["id"] = item.first;
        entry.o["package_revision"] = package_revision;
        metadata(entry, item.second);
        entry.o["scenario"] = item.second.at("diagnostic").at("scenario");
        entry.o["export_byte_count"] = static_cast<double>(export_bytes(item.second).size());
        items.push_back(entry);
    }
    result.value.o["items"] = std::move(items);
    return finish(std::move(result), 0, "observed", "store_observed");
}
Reply show_package(const std::string& id, const std::string& expected_revision) {
    auto result = receipt("show", id);
    if (!valid_id(id) || (!expected_revision.empty() && !valid_revision(expected_revision)))
        return finish(std::move(result), 2, "invalid", "invalid_request");
    Source source;
    JsonValue packages;
    if (!read_bank(source, packages))
        return finish(std::move(result), 5, "unavailable", "store_unreadable");
    if (!packages.contains(id))
        return finish(std::move(result), 3, "not_found", "package_missing");
    const auto& package = packages.at(id);
    const auto package_revision = revision(package);
    if (package_revision.empty())
        return finish(std::move(result), 5, "unavailable", "revision_unavailable");
    if (!expected_revision.empty() && expected_revision != package_revision)
        return finish(std::move(result), 4, "conflict", "revision_conflict");
    result.value.o["package_revision"] = package_revision;
    result.value.o["package"] = package;
    metadata(result.value, package);
    const auto bytes = export_bytes(package);
    result.value.o["export_bytes"] = bytes;
    result.value.o["export_byte_count"] = static_cast<double>(bytes.size());
    return finish(std::move(result), 0, "observed", "package_observed");
}
Reply create_package(const std::string& id, const JsonValue& package) {
    return mutate(id, &package, {});
}
Reply remove_package(const std::string& id, const std::string& expected_revision) {
    return mutate(id, nullptr, expected_revision);
}
}  // namespace sumhp::diagnostic_package
