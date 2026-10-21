#include <algorithm>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include "core/config_document.hpp"
#include "core/profile.hpp"
#include "core/user_hide_store.hpp"
#include "durable_file.hpp"
#include "mount/backend.hpp"
#include "sumhp/config.hpp"
#include "uapi/sumh.h"
#include "userspace/common/utf8.hpp"

namespace sumhp {
#include "profile_config_validators.inc"
namespace mount {
#include "profile_module_validators.inc"
}
}  // namespace sumhp

namespace io_model {
constexpr int rdwr = 1, creat = 2, cloexec = 4, nofollow = 8, nonblock = 16, exclusive = 32,
              nb = 64;
constexpr unsigned regular = 0100000;
struct Metadata {
    unsigned st_mode = regular | 0600, st_nlink = 1, st_uid = 0;
};
struct File {
    std::string bytes;
    bool unreadable = false;
};
std::map<std::string, File> files;
std::map<int, bool> descriptors;
std::vector<std::string> hashed;
std::vector<std::string> syncs, events;
std::vector<std::pair<std::string, std::string>> writes;
int lock_fd = -1, next_fd = 3, opens = 0, reads = 0, prepares = 0, hash_calls = 0;
int fail_hash = 0, fail_write = 0, unconfirmed_write = 0, bad_readback_write = 0;
int fail_sync = 0;
bool fail_prepare = false, fail_open = false, fail_stat = false, fail_lock = false, collide = false;
Metadata metadata;
std::function<void()> on_read;
int assertions = 0, groups = 0;
void check(bool ok, const char* expression, int line) {
    ++assertions;
    if (!ok) {
        std::cerr << "assertion " << assertions << " line " << line << ": " << expression << "\n";
        std::abort();
    }
}
#define check(value) io_model::check((value), #value, __LINE__)
std::string key(const char* name) {
    return (std::filesystem::path("/fixture/data/adb/ksu/sumhp") / name).generic_string();
}
void put(const char* name, const std::string& bytes) {
    files[key(name)] = {bytes, false};
}
void reset() {
    assert(descriptors.empty());
    files.clear();
    hashed.clear();
    writes.clear();
    syncs.clear();
    events.clear();
    lock_fd = -1;
    next_fd = 3;
    opens = reads = prepares = hash_calls = 0;
    fail_hash = fail_write = unconfirmed_write = bad_readback_write = fail_sync = 0;
    fail_prepare = fail_open = fail_stat = fail_lock = collide = false;
    metadata = {};
    on_read = {};
}
template <typename Character>
int open(const Character* path, int flags, int mode) {
    events.emplace_back("open-lock");
    ++opens;
    check(std::filesystem::path(path).generic_string() == key("profiles.lock"));
    check(flags == (rdwr | creat | cloexec | nofollow | nonblock) && mode == 0600);
    if (fail_open) {
        errno = EACCES;
        return -1;
    }
    descriptors[next_fd] = true;
    return next_fd++;
}
int close(int fd) {
    check(descriptors.erase(fd) == 1);
    if (lock_fd == fd)
        lock_fd = -1;
    return 0;
}
int fstat(int fd, Metadata* value) {
    check(descriptors.count(fd));
    if (fail_stat) {
        errno = EIO;
        return -1;
    }
    *value = metadata;
    return 0;
}
int flock(int fd, int flags) {
    check(descriptors.count(fd) && flags == (exclusive | nb));
    if (fail_lock || lock_fd >= 0) {
        errno = EWOULDBLOCK;
        return -1;
    }
    lock_fd = fd;
    events.emplace_back("lock-held");
    return 0;
}
}  // namespace io_model

namespace sumhp {
std::filesystem::path runtime_data_dir() {
    return "/fixture/data/adb/ksu/sumhp";
}
bool prepare_private_directory(const std::filesystem::path& path, std::string& error) {
    check(path == runtime_data_dir());
    ++io_model::prepares;
    io_model::events.emplace_back("prepare");
    if (io_model::fail_prepare) {
        error = "prepare fault";
        errno = EIO;
        return false;
    }
    return true;
}
bool sync_private_directory(const std::filesystem::path& path, std::string& error) {
    using namespace io_model;
    check(lock_fd >= 0);
    const auto paths = std::vector<std::string>{"/fixture/data/adb/ksu/sumhp",
                                                "/fixture/data/adb/ksu", "/fixture/data/adb"};
    check(path.generic_string() == paths[syncs.size() % 3]);
    syncs.push_back(path.generic_string());
    events.emplace_back("sync:" + path.generic_string());
    if (static_cast<int>(syncs.size()) == fail_sync) {
        error = "directory sync fault";
        errno = EIO;
        return false;
    }
    return true;
}
namespace state {
std::string revision_for_bytes(const std::string& bytes) {
    using namespace io_model;
    ++hash_calls;
    // Retain only short inputs: large capacity fixtures must not accidentally
    // turn diagnostic history into an unbounded second allocation.
    if (bytes.size() < 65536)
        hashed.push_back(bytes);
    if (fail_hash == hash_calls)
        return {};
    std::uint64_t hash = 1469598103934665603ULL;
    if (!collide)
        for (const unsigned char c : bytes)
            hash = (hash ^ c) * 1099511628211ULL;
    static constexpr char hex[] = "0123456789abcdef";
    std::string result = "sha256-v1:";
    for (unsigned i = 0; i < 32; ++i) {
        const auto value = static_cast<unsigned char>((hash >> ((i % 8) * 8)) ^ i);
        result += hex[value >> 4];
        result += hex[value & 15];
    }
    return result;
}
}  // namespace state
namespace checked {
struct Document {
    std::string status, bytes;
    int error = 0;
};
Document read_document(const std::filesystem::path& path, std::size_t limit) {
    using namespace io_model;
    check(path.generic_string() == key("profiles.json") && limit == profiles::BANK_MAX_BYTES);
    ++reads;
    if (on_read)
        on_read();
    const auto found = files.find(path.generic_string());
    if (found == files.end())
        return {"missing", {}, ENOENT};
    if (found->second.unreadable)
        return {"unreadable", {}, EACCES};
    if (found->second.bytes.size() > limit)
        return {"unreadable", {}, EOVERFLOW};
    return {"valid", found->second.bytes, 0};
}
}  // namespace checked
}  // namespace sumhp
namespace ksud {
DurableWriteResult replace_file_durable(const std::filesystem::path& path, const std::string& bytes,
                                        mode_t mode) {
    using namespace io_model;
    check(lock_fd >= 0 && mode == 0600 && path.generic_string() == key("profiles.json"));
    check(syncs.size() >= 3 && syncs.size() % 3 == 0);
    events.emplace_back("replace-bank");
    const auto name = path.generic_string();
    writes.emplace_back(name, bytes);
    const auto count = static_cast<int>(writes.size());
    if (count == fail_write)
        return {DurableWriteState::NotCommitted, EIO};
    files[name] = {bytes, false};
    if (count == bad_readback_write)
        files[name].bytes = "corrupted readback";
    return {count == unconfirmed_write ? DurableWriteState::ReplacedDurabilityUnconfirmed
                                       : DurableWriteState::Complete,
            count == unconfirmed_write ? EIO : 0};
}
}  // namespace ksud

#define O_RDWR io_model::rdwr
#define O_CREAT io_model::creat
#define O_CLOEXEC io_model::cloexec
#define O_NOFOLLOW io_model::nofollow
#define O_NONBLOCK io_model::nonblock
#define LOCK_EX io_model::exclusive
#define LOCK_NB io_model::nb
#define S_ISREG(mode) (((mode) & 0170000) == io_model::regular)
#include "profile_store_under_test.inc"
#undef O_RDWR
#undef O_CREAT
#undef O_CLOEXEC
#undef O_NOFOLLOW
#undef O_NONBLOCK
#undef LOCK_EX
#undef LOCK_NB
#undef S_ISREG

namespace {
using namespace sumhp;
using namespace sumhp::profiles;
using namespace io_model;
JsonValue profile(const std::string& name = "中文 name ",
                  const std::string& note = "$(literal)\n ") {
    JsonValue value;
    std::string error;
    check(capture_profile("{}", "{}", "{}", "[]", name, note, value, error));
    return value;
}
JsonValue bank_json(const JsonValue& entries) {
    auto value = JsonValue::object();
    value.o["schema_version"] = 1;
    value.o["kind"] = "sumh.profile.bank";
    value.o["profiles"] = entries;
    return value;
}
Bank bank() {
    Bank value;
    std::string error;
    check(read_bank(value, error));
    return value;
}
JsonValue request(const char* action, const std::string& id, const Bank& observed) {
    auto value = JsonValue::object();
    value.o["schema_version"] = 1;
    value.o["action"] = action;
    value.o["id"] = id;
    value.o["bank_revision"] = observed.revision;
    return value;
}
JsonValue create(const std::string& id = "one") {
    auto value = request("create", id, bank());
    value.o["profile"] = profile();
    return value;
}
JsonValue alter(const char* action, const std::string& id = "one") {
    const auto observed = bank();
    auto value = request(action, id, observed);
    value.o["profile_revision"] = profile_revision(observed.profiles.at(id));
    if (std::string(action) != "remove") {
        value.o["name"] = observed.profiles.at(id).at("name");
        value.o["note"] = observed.profiles.at(id).at("note");
    }
    return value;
}
void result(const JsonValue& value, const char* status, const char* reason) {
    check(value.at("status").s == status && value.at("reason").s == reason);
    check(!value.at("applied").b && !value.at("active_configuration_saved").b &&
          !value.at("profile_active").b);
    check(value.at("schema_version").n == 1 && value.at("kind").s == "sumh.profile.mutation");
    check(descriptors.empty() && lock_fd < 0);
}
void read_tests() {
    ++groups;
    reset();
    std::string error;
    Bank value;
    check(read_bank(value, error) && value.missing && value.profiles.o.empty());
    check(hashed.back() == "sumhp-profile-bank-v1\nmissing");
    const auto missing = value.revision;
    put("profiles.json", stringify_json(bank_json(JsonValue::object())));
    check(read_bank(value, error) && !value.missing && value.revision != missing);
    const auto saved = value;
    for (const auto& invalid : std::vector<std::string>{
             "", "null", "{}",
             "{\"schema_version\":2,\"kind\":\"sumh.profile.bank\",\"profiles\":{}}",
             "{\"schema_version\":1,\"schema_version\":1,\"kind\":\"sumh.profile.bank\","
             "\"profiles\":{}}",
             "{\"schema_version\":1,\"kind\":\"sumh.profile.bank\",\"profiles\":{},\"future\":"
             "1}"}) {
        put("profiles.json", invalid);
        check(!read_bank(value, error) && value.bytes == saved.bytes &&
              value.revision == saved.revision);
    }
    put("profiles.json", saved.bytes);
    files[key("profiles.json")].unreadable = true;
    check(!read_bank(value, error) && value.bytes == saved.bytes);
    put("profiles.json", saved.bytes);
    fail_hash = hash_calls + 1;
    check(!read_bank(value, error) && value.bytes == saved.bytes);
    fail_hash = 0;
    auto entries = JsonValue::object();
    entries.o["../bad"] = profile();
    put("profiles.json", stringify_json(bank_json(entries)));
    check(!read_bank(value, error));
    entries = JsonValue::object();
    entries.o["one"] = profile();
    entries.o["one"].o["boot_id"] = "forbidden";
    put("profiles.json", stringify_json(bank_json(entries)));
    check(!read_bank(value, error));
    put("profiles.json", std::string(BANK_MAX_BYTES + 1, ' '));
    check(!read_bank(value, error) && errno == EOVERFLOW);
    check(opens == 0 && prepares == 0 && writes.empty() && syncs.empty());
}
void validation_tests() {
    ++groups;
    reset();
    auto valid = create();
    const int read_count = reads;
    std::vector<JsonValue> bad{JsonValue{}, JsonValue::array()};
    for (const auto* field : {"schema_version", "action", "id", "bank_revision", "profile"}) {
        auto value = valid;
        value.o.erase(field);
        bad.push_back(value);
    }
    for (const auto& action : {"activate", "restore", "delete", "CREATE"}) {
        auto value = valid;
        value.o["action"] = action;
        bad.push_back(value);
    }
    for (const auto& id : std::vector<std::string>{"", ".", "../x", "UPPER", "-first", "with space",
                                                   std::string(65, 'a'), std::string("a\0b", 3)}) {
        auto value = valid;
        value.o["id"] = id;
        bad.push_back(value);
    }
    for (const auto& rev :
         {"sha256-v1:",
          "SHA256-V1:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}) {
        auto value = valid;
        value.o["bank_revision"] = rev;
        bad.push_back(value);
    }
    auto value = valid;
    value.o["future"] = true;
    bad.push_back(value);
    value = valid;
    value.o["schema_version"] = 1.5;
    bad.push_back(value);
    value = valid;
    value.o["profile"].o["inode"] = 42;
    bad.push_back(value);
    value = valid;
    value.o["profile"].o["name"] = std::string("\xff", 1);
    bad.push_back(value);
    for (const auto& invalid : bad)
        result(mutate_bank(invalid), "not_saved", "invalid_request");
    check(reads == read_count && prepares == 0 && opens == 0 && writes.empty() && syncs.empty());
    for (const auto* action : {"rename", "clone", "remove"}) {
        value = request(action, "one", bank());
        value.o["profile_revision"] = valid.at("bank_revision");
        if (std::string(action) != "remove") {
            value.o["name"] = "ok";
            value.o["note"] = "";
        }
        if (std::string(action) == "clone")
            value.o["source_id"] = "one";
        auto invalid = value;
        invalid.o["profile_revision"] = "bad";
        result(mutate_bank(invalid), "not_saved", "invalid_request");
        if (std::string(action) != "remove") {
            for (const auto& name :
                 std::vector<std::string>{"", std::string(129, 'a'), std::string("a\0b", 3)}) {
                invalid = value;
                invalid.o["name"] = name;
                result(mutate_bank(invalid), "not_saved", "invalid_request");
            }
            invalid = value;
            invalid.o["note"] = std::string(1025, 'x');
            result(mutate_bank(invalid), "not_saved", "invalid_request");
        }
    }
    check(prepares == 0 && writes.empty() && syncs.empty());
}
void CRUD_tests() {
    ++groups;
    reset();
    put("config.json", "active-config-sentinel");
    put("module_rules.json", "active-modules-sentinel");
    put("user_hide_rules.json", "active-hide-sentinel");
    const auto input = create();
    auto out = mutate_bank(input);
    result(out, "saved", "inactive_profile_saved");
    check(out.at("saved").b && out.at("durability_confirmed").b && out.at("readback_confirmed").b);
    auto saved = bank();
    check(saved.profiles.o.size() == 1 && out.at("bank_revision").s == saved.revision);
    check(out.at("profile_revision").s == profile_revision(saved.profiles.at("one")));
    auto duplicate = create();
    result(mutate_bank(duplicate), "not_saved", "profile_id_conflict");
    result(mutate_bank(input), "not_saved", "bank_revision_conflict");
    auto rename = alter("rename");
    const auto writes_before = writes.size();
    out = mutate_bank(rename);
    result(out, "unchanged", "already_saved");
    check(out.at("saved").b && out.at("readback_confirmed").b &&
          !out.at("durability_confirmed").b && writes.size() == writes_before);
    rename.o["name"] = "renamed 中文 ";
    rename.o["note"] = "line\n trailing ";
    out = mutate_bank(rename);
    result(out, "saved", "inactive_profile_saved");
    check(bank().profiles.at("one").at("name").s == "renamed 中文 ");
    auto clone = alter("clone");
    clone.o["source_id"] = "one";
    clone.o["id"] = "two";
    const auto source = bank().profiles.at("one");
    out = mutate_bank(clone);
    result(out, "saved", "inactive_profile_saved");
    saved = bank();
    check(saved.profiles.o.size() == 2 &&
          stringify_json(saved.profiles.at("two")) == stringify_json(source));
    auto remove = alter("remove", "one");
    out = mutate_bank(remove);
    result(out, "saved", "inactive_profile_saved");
    check(out.at("profile_revision").is_null() && !bank().profiles.contains("one") &&
          bank().profiles.contains("two"));
    result(mutate_bank(remove), "not_saved", "bank_revision_conflict");
    remove.o["bank_revision"] = bank().revision;
    result(mutate_bank(remove), "not_saved", "profile_missing");
    for (const auto* name : {"config.json", "module_rules.json", "user_hide_rules.json"})
        check(files.count(key(name)) == 1);
    check(files[key("config.json")].bytes == "active-config-sentinel" &&
          files[key("module_rules.json")].bytes == "active-modules-sentinel" &&
          files[key("user_hide_rules.json")].bytes == "active-hide-sentinel");
    for (const auto& written : writes)
        check(written.first == key("profiles.json"));
}
void conflicts_and_corruption_tests() {
    ++groups;
    reset();
    result(mutate_bank(create()), "saved", "inactive_profile_saved");
    auto rename = alter("rename");
    rename.o["profile_revision"] = rename.at("bank_revision");
    result(mutate_bank(rename), "not_saved", "profile_revision_conflict");
    auto clone = alter("clone");
    clone.o["source_id"] = "absent";
    clone.o["id"] = "new";
    result(mutate_bank(clone), "not_saved", "profile_missing");
    clone.o["source_id"] = "one";
    clone.o["profile_revision"] = clone.at("bank_revision");
    result(mutate_bank(clone), "not_saved", "profile_revision_conflict");
    const auto input = create("second");
    const auto writes_before = writes.size();
    for (const auto& invalid :
         {"broken", "{\"schema_version\":2,\"kind\":\"sumh.profile.bank\",\"profiles\":{}}"}) {
        put("profiles.json", invalid);
        result(mutate_bank(input), "not_saved", "bank_unreadable");
        check(files[key("profiles.json")].bytes == invalid && writes.size() == writes_before);
    }
}
void lock_tests() {
    ++groups;
    for (int fault = 0; fault < 10; ++fault) {
        reset();
        const auto input = create();
        if (fault == 0)
            fail_prepare = true;
        if (fault == 1)
            fail_open = true;
        if (fault == 2)
            fail_stat = true;
        if (fault == 3)
            fail_lock = true;
        if (fault == 4)
            metadata.st_mode = 0120000 | 0600;
        if (fault == 5)
            metadata.st_nlink = 2;
        if (fault == 6)
            metadata.st_uid = 1000;
        if (fault == 7)
            metadata.st_mode = regular | 0644;
        if (fault == 8)
            metadata.st_mode = regular | 04600;
        if (fault == 9)
            metadata.st_mode = 0010000 | 0600;
        result(mutate_bank(input), "not_saved", "bank_lock_unavailable");
        check(writes.empty() && !files.count(key("profiles.json")));
        check(syncs.empty());
    }
}
void directory_durability_tests() {
    ++groups;
    for (int fault = 1; fault <= 3; ++fault) {
        reset();
        const auto input = create();
        fail_sync = fault;
        put("config.json", "active-config-sentinel");
        const auto out = mutate_bank(input);
        result(out, "not_saved", "bank_directory_durability_unconfirmed");
        check(syncs.size() == static_cast<std::size_t>(fault) && writes.empty() &&
              !files.count(key("profiles.json")));
        check(files[key("config.json")].bytes == "active-config-sentinel");
        check(!out.at("saved").b && !out.at("durability_confirmed").b &&
              !out.at("readback_confirmed").b);
        check(events.size() == static_cast<std::size_t>(3 + fault));
        check(events[0] == "prepare" && events[1] == "open-lock" && events[2] == "lock-held");
    }
    reset();
    const auto input = create();
    result(mutate_bank(input), "saved", "inactive_profile_saved");
    check(events == std::vector<std::string>(
                        {"prepare", "open-lock", "lock-held", "sync:/fixture/data/adb/ksu/sumhp",
                         "sync:/fixture/data/adb/ksu", "sync:/fixture/data/adb", "replace-bank"}));
    const auto previous_syncs = syncs.size();
    auto rename = alter("rename");
    rename.o["name"] = "new name";
    result(mutate_bank(rename), "saved", "inactive_profile_saved");
    check(syncs.size() == previous_syncs + 3 && writes.size() == 2);
    const auto latest_syncs = syncs.size();
    result(mutate_bank(alter("rename")), "unchanged", "already_saved");
    check(syncs.size() == latest_syncs + 3 && writes.size() == 2);
}
void durability_tests() {
    ++groups;
    for (int fault = 0; fault < 4; ++fault) {
        reset();
        const auto input = create();
        if (fault == 0)
            fail_write = 1;
        if (fault == 1)
            unconfirmed_write = 1;
        if (fault == 2)
            bad_readback_write = 1;
        if (fault == 3) {
            unconfirmed_write = 1;
            bad_readback_write = 1;
        }
        const auto out = mutate_bank(input);
        result(out, fault == 0 ? "not_saved" : "unknown",
               fault == 0   ? "bank_not_replaced"
               : fault == 2 ? "replaced_readback_unconfirmed"
                            : "replaced_durability_unconfirmed");
        check(writes.size() == 1 && !out.at("saved").b && out.at("id").s == "one");
        check(out.at("durability_confirmed").b == (fault == 2));
        check(out.at("readback_confirmed").b == (fault == 1));
        if (fault == 0)
            check(!files.count(key("profiles.json")));
        if (fault == 1) {
            const auto observed = bank();
            check(observed.profiles.contains("one") &&
                  observed.revision == out.at("bank_revision").s);
        }
        if (fault >= 2)
            check(out.at("bank_revision").is_null() && out.at("profile_revision").is_null());
    }
}
void race_tests() {
    ++groups;
    for (int fault = 0; fault < 3; ++fault) {
        reset();
        if (fault == 2) {
            collide = true;
            put("profiles.json", stringify_json(bank_json(JsonValue::object())));
        }
        auto input = create();
        reads = 0;
        on_read = [fault] {
            if (reads == 2) {
                if (fault == 1)
                    put("profiles.json", "broken");
                else if (fault == 2)
                    files[key("profiles.json")].bytes += " ";
                else
                    put("profiles.json", stringify_json(bank_json(JsonValue::object())));
            }
        };
        result(mutate_bank(input), "not_saved",
               fault == 1 ? "bank_recheck_failed" : "bank_changed");
        check(writes.empty());
    }
    reset();
    auto input = create();
    fail_hash = hash_calls + 1;
    result(mutate_bank(input), "not_saved", "bank_unreadable");
    check(writes.empty());
    reset();
    input = create();
    fail_hash = hash_calls + 2;
    result(mutate_bank(input), "not_saved", "revision_unavailable");
    check(writes.empty());
}
void count_capacity_tests() {
    ++groups;
    reset();
    auto entries = JsonValue::object();
    const auto sample = profile();
    for (std::size_t i = 0; i < BANK_MAX_PROFILES; ++i)
        entries.o["id" + std::to_string(i)] = sample;
    put("profiles.json", stringify_json(bank_json(entries)));
    check(bank().profiles.o.size() == BANK_MAX_PROFILES);
    result(mutate_bank(create("overflow")), "not_saved", "bank_capacity");
    check(writes.empty());
    auto clone = alter("clone", "id0");
    clone.o["source_id"] = "id0";
    clone.o["id"] = "clone";
    result(mutate_bank(clone), "not_saved", "bank_capacity");
    check(writes.empty());
    entries.o["overflow"] = sample;
    put("profiles.json", stringify_json(bank_json(entries)));
    Bank observed;
    std::string error;
    check(!read_bank(observed, error));
}
void byte_capacity_tests() {
    ++groups;
    reset();
    auto sample = profile();
    auto& definitions = sample.o["resources"].o["user_hide"].o["rules"].a;
    for (int i = 0; i < 2800; ++i) {
        const auto suffix = std::to_string(i);
        hide::Definition item{std::string(32 - suffix.size(), '0') + suffix,
                              "/synthetic/" + suffix,
                              true,
                              "name",
                              std::string(1000, 'n'),
                              "group"};
        definitions.push_back(hide::definition_json(item));
    }
    JsonValue validated;
    std::string error;
    check(parse_profile(stringify_json(sample), validated, error));
    auto entries = JsonValue::object();
    while (true) {
        auto next = entries;
        next.o["id" + std::to_string(entries.o.size())] = validated;
        if (stringify_json(bank_json(next)).size() + 1 > BANK_MAX_BYTES)
            break;
        entries = std::move(next);
    }
    check(!entries.o.empty() && entries.o.size() < BANK_MAX_PROFILES);
    const auto bytes = stringify_json(bank_json(entries)) + "\n";
    put("profiles.json", bytes);
    auto input = request("create", "overflow", bank());
    input.o["profile"] = validated;
    result(mutate_bank(input), "not_saved", "bank_capacity");
    check(writes.empty() && files[key("profiles.json")].bytes == bytes);
}
}  // namespace
int main() {
    read_tests();
    validation_tests();
    CRUD_tests();
    conflicts_and_corruption_tests();
    lock_tests();
    directory_durability_tests();
    durability_tests();
    race_tests();
    count_capacity_tests();
    byte_capacity_tests();
    std::cout << "profile store: " << groups << " groups / " << assertions << " checks passed\n";
}
