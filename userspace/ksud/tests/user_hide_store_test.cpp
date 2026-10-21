#include "core/user_hide_store.hpp"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include "durable_file.hpp"

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
std::vector<std::pair<std::string, std::string>> writes;
int lock_fd = -1, next_fd = 3, opens = 0, reads = 0, prepares = 0, hash_calls = 0;
int fail_hash = 0, fail_write = 0, unconfirmed_write = 0, bad_readback_write = 0;
bool fail_prepare = false, fail_open = false, fail_stat = false, fail_lock = false, collide = false;
Metadata metadata;
std::function<void(const std::string&)> on_read;
std::function<void(const std::string&)> after_write;
int assertions = 0;
void check(bool ok, const char* expression, int line) {
    ++assertions;
    if (!ok) {
        std::cerr << "assertion " << assertions << " line " << line << ": " << expression << "\n";
        std::abort();
    }
}
#define check(value) io_model::check((value), #value, __LINE__)
std::string key(const char* name) {
    return (std::filesystem::path("/fixture") / name).generic_string();
}
void put(const char* name, const std::string& bytes) {
    files[key(name)] = {bytes, false};
}
void reset() {
    assert(descriptors.empty());
    files.clear();
    hashed.clear();
    writes.clear();
    lock_fd = -1;
    next_fd = 3;
    opens = reads = prepares = hash_calls = 0;
    fail_hash = fail_write = unconfirmed_write = bad_readback_write = 0;
    fail_prepare = fail_open = fail_stat = fail_lock = collide = false;
    metadata = {};
    on_read = {};
    after_write = {};
}
template <typename Character>
int open(const Character* path, int flags, int mode) {
    ++opens;
    assert(std::filesystem::path(path).generic_string() == key("user_hide_rules.lock"));
    assert(flags == (rdwr | creat | cloexec | nofollow | nonblock) && mode == 0600);
    if (fail_open) {
        errno = EACCES;
        return -1;
    }
    descriptors[next_fd] = true;
    return next_fd++;
}
int close(int fd) {
    assert(descriptors.erase(fd) == 1);
    if (lock_fd == fd)
        lock_fd = -1;
    return 0;
}
int fstat(int fd, Metadata* value) {
    assert(descriptors.count(fd));
    if (fail_stat) {
        errno = EIO;
        return -1;
    }
    *value = metadata;
    return 0;
}
int flock(int fd, int flags) {
    assert(descriptors.count(fd) && flags == (exclusive | nb));
    if (fail_lock || lock_fd >= 0) {
        errno = EWOULDBLOCK;
        return -1;
    }
    lock_fd = fd;
    return 0;
}
}  // namespace io_model

namespace sumhp {
std::filesystem::path runtime_data_dir() {
    return "/fixture";
}
bool prepare_private_directory(const std::filesystem::path& path, std::string& error) {
    assert(path == runtime_data_dir());
    ++io_model::prepares;
    if (io_model::fail_prepare) {
        error = "prepare fault";
        errno = EIO;
        return false;
    }
    return true;
}
namespace state {
std::string revision_for_bytes(const std::string& bytes) {
    using namespace io_model;
    ++hash_calls;
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
    assert(limit == hide::HIDE_MAX_BYTES);
    ++reads;
    const auto name = path.generic_string();
    if (on_read)
        on_read(name);
    const auto found = files.find(name);
    if (found == files.end())
        return {"missing", {}, ENOENT};
    if (found->second.unreadable)
        return {"unreadable", {}, EACCES};
    if (found->second.bytes.size() > limit)
        return {"unreadable", {}, EOVERFLOW};
    return {"valid", found->second.bytes, 0};
}
}  // namespace checked
namespace logging {
enum class Level { Warning, Error };
void write(Level, const std::string&, const std::string&) {}
}  // namespace logging
}  // namespace sumhp
namespace ksud {
DurableWriteResult replace_file_durable(const std::filesystem::path& path, const std::string& bytes,
                                        mode_t mode) {
    using namespace io_model;
    assert(lock_fd >= 0 && mode == 0600);
    const auto name = path.generic_string();
    writes.emplace_back(name, bytes);
    const auto count = static_cast<int>(writes.size());
    if (count == fail_write)
        return {DurableWriteState::NotCommitted, EIO};
    files[name] = {bytes, false};
    if (count == bad_readback_write)
        files[name].bytes = "corrupted readback";
    if (after_write)
        after_write(name);
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
#include "user_hide_store_under_test.inc"
#undef O_RDWR
#undef O_CREAT
#undef O_CLOEXEC
#undef O_NOFOLLOW
#undef O_NONBLOCK
#undef LOCK_EX
#undef LOCK_NB
#undef S_ISREG
namespace sumhp {
#include "user_hide_wrappers_under_test.inc"
}

namespace {
using namespace sumhp;
using namespace sumhp::hide;
using namespace io_model;
Definition rule(std::string path = "/one", char id = 'a', bool enabled = true) {
    return {std::string(32, id), std::move(path), enabled, "name", "note", "group"};
}
Document doc() {
    Document value;
    value.rules.push_back(rule());
    return value;
}
std::string legacy(const std::vector<std::string>& paths) {
    auto value = JsonValue::array();
    for (const auto& path : paths)
        value.a.emplace_back(path);
    return stringify_json(value);
}
void model_tests() {
    reset();
    std::string error;
    Document value;
    value.rules.push_back(rule("/sentinel"));
    const auto unchanged = encode_document(value);
    for (const auto& bad : std::vector<std::string>{
             "", "null", "{}", "[true]", "[\"relative\"]", "[\"\"]",
             "{\"schema_version\":2,\"rules\":[]}", "{\"schema_version\":1,\"rules\":[],\"x\":0}",
             "{\"schema_version\":1,\"schema_version\":1,\"rules\":[]}",
             "{\"schema_version\":1,\"rules\":{}}", "[\"/a\"] trailing",
             std::string("[\"/") + char(0xff) + "\"]", "[\"/a\\u0000b\"]"}) {
        check(!parse_document(bad, value, error) && !error.empty() &&
              encode_document(value) == unchanged);
    }
    check(parse_document(legacy({"/中文 ", "/a\nb", "/中文 ", "//raw/../path"}), value, error));
    check(value.source_schema == 0 && value.rules.size() == 3 &&
          enabled_paths(value) == std::vector<std::string>({"/中文 ", "/a\nb", "//raw/../path"}));
    check(value.rules[0].enabled && value.rules[0].name.empty() && value.rules[0].note.empty() &&
          value.rules[0].group.empty());
    const auto id = value.rules[0].id;
    check(hashed.front() == "sumhp-user-hide-legacy-v1\n" + json_quote("/中文 "));
    Document again;
    check(parse_document(legacy({"/a\nb", "/中文 "}), again, error) && again.rules[1].id == id);
    check(parse_document(encode_document(value), again, error) && again.source_schema == 1 &&
          again.rules[0].id == id);
    again.rules[0].enabled = false;
    check(enabled_paths(again).size() == 2 && enabled_paths(again).front() == "/a\nb");
    check(valid_path("/") && valid_path("/" + std::string(4094, 'x')) &&
          !valid_path("/" + std::string(4095, 'x')));
    check(valid_id(std::string(32, '0')) && !valid_id(std::string(32, 'A')) &&
          !valid_id(std::string(31, 'a')));
    check(!valid_path(std::string("/x\0y", 4)) && !valid_path(std::string("/") + char(0xff)));
    auto valid = doc();
    auto json = document_json(valid);
    for (const auto* key : {"id", "path", "enabled", "name", "note", "group"}) {
        auto broken = json;
        broken.o["rules"].a[0].o.erase(key);
        check(!parse_document(stringify_json(broken), value, error));
    }
    for (const auto& change : std::vector<std::function<void(JsonValue&)>>{
             [](auto& r) { r.o["x"] = 0; }, [](auto& r) { r.o["enabled"] = "true"; },
             [](auto& r) { r.o["id"] = std::string(32, 'A'); },
             [](auto& r) { r.o["path"] = "relative"; },
             [](auto& r) { r.o["name"] = std::string(129, 'x'); },
             [](auto& r) { r.o["group"] = std::string(129, 'x'); },
             [](auto& r) { r.o["note"] = std::string(1025, 'x'); },
             [](auto& r) { r.o["note"] = std::string("a\0b", 3); }}) {
        auto broken = json;
        change(broken.o["rules"].a[0]);
        check(!parse_document(stringify_json(broken), value, error));
    }
    valid.rules[0].name = std::string(128, 'x');
    valid.rules[0].group = std::string(128, 'g');
    valid.rules[0].note = std::string(1024, '\n');
    check(parse_document(encode_document(valid), value, error));
    valid.rules[0].name.clear();
    for (int i = 0; i < 64; ++i)
        valid.rules[0].name += "é";
    check(parse_document(encode_document(valid), value, error));
    valid.rules[0].name += "é";
    check(!parse_document(encode_document(valid), value, error));
    valid.rules[0].name = "name";
    valid.rules.push_back(rule("/two"));
    check(!parse_document(encode_document(valid), value, error));
    valid.rules[1] = rule("/one", 'b');
    check(!parse_document(encode_document(valid), value, error));
    collide = true;
    check(!parse_document(legacy({"/one", "/two"}), value, error));
    check(parse_document(legacy({"/one", "/one"}), value, error) && value.rules.size() == 1);
    collide = false;
    check(parse_document(legacy(std::vector<std::string>(HIDE_MAX_RULES + 1, "/one")), value,
                         error) &&
          value.rules.size() == 1);
    fail_hash = hash_calls + 1;
    check(!parse_document(legacy({"/one"}), value, error));
    fail_hash = 0;
    valid = doc();
    const auto bytes = encode_document(valid);
    check(parse_document(bytes + std::string(HIDE_MAX_BYTES - bytes.size(), ' '), value, error));
    check(
        !parse_document(bytes + std::string(HIDE_MAX_BYTES - bytes.size() + 1, ' '), value, error));
    valid.rules.clear();
    for (std::size_t i = 0; i < HIDE_MAX_RULES; ++i) {
        auto r = rule("/" + std::to_string(i));
        const auto suffix = std::to_string(i);
        r.id = std::string(32 - suffix.size(), '0') + suffix;
        valid.rules.push_back(r);
    }
    check(parse_document(encode_document(valid), value, error) &&
          value.rules.size() == HIDE_MAX_RULES);
    auto over_encoded = valid;
    for (auto& entry : over_encoded.rules)
        entry.note = std::string(HIDE_NOTE_MAX_BYTES, 'n');
    check(encode_document(over_encoded).size() > HIDE_MAX_BYTES &&
          !parse_document(encode_document(over_encoded), value, error));
    std::vector<std::string> long_legacy;
    for (std::size_t i = 0; i < HIDE_MAX_RULES; ++i)
        long_legacy.push_back("/" + std::string(985, 'x') + std::to_string(i));
    check(legacy(long_legacy).size() < HIDE_MAX_BYTES &&
          !parse_document(legacy(long_legacy), value, error));
    valid.rules.push_back(rule("/overflow", 'b'));
    check(!parse_document(encode_document(valid), value, error));
    check(reads == 0 && opens == 0 && prepares == 0 && writes.empty());
}
void read_tests() {
    reset();
    std::string error;
    Stored value;
    check(read_store(value, error) && value.missing && value.document.rules.empty());
    check(hashed.back() == "sumhp-user-hide-document-v1\nmissing" && opens == 0 && prepares == 0 &&
          writes.empty());
    const auto missing_revision = value.revision;
    put("user_hide_rules.json", "[]");
    check(read_store(value, error) && !value.missing && value.document.source_schema == 0);
    check(value.revision != missing_revision &&
          hashed.back() == "sumhp-user-hide-document-v1\npresent\n[]");
    const auto bytes = value.bytes, revision_value = value.revision;
    put("user_hide_rules.json", "");
    check(!read_store(value, error) && value.bytes == bytes && value.revision == revision_value);
    files[key("user_hide_rules.json")].unreadable = true;
    check(!read_store(value, error) && value.bytes == bytes);
    put("user_hide_rules.json", encode_document(doc()));
    fail_hash = hash_calls + 1;
    check(!read_store(value, error) && value.bytes == bytes && value.revision == revision_value);
    fail_hash = 0;
    check(read_store(value, error) && value.document.source_schema == 1);
    check(opens == 0 && prepares == 0 && writes.empty());
}
void transaction_tests() {
    reset();
    std::string error;
    {
        Transaction tx;
        check(opens == 0 && reads == 0 && prepares == 0);
        check(tx.save(doc(), error) == SaveResult::NotSaved);
        check(tx.begin(error) && lock_fd >= 0);
        check(!tx.begin(error) && errno == EALREADY);
        Transaction second;
        check(!second.begin(error) && writes.empty());
        check(tx.save({}, error) == SaveResult::Unchanged && tx.stored().missing && writes.empty());
        check(tx.save(doc(), error) == SaveResult::Saved && !tx.stored().missing &&
              tx.stored().document.source_schema == 1);
        check(lock_fd >= 0 && writes.size() == 1 && tx.stored().bytes == encode_document(doc()));
        check(tx.save(doc(), error) == SaveResult::Unchanged && writes.size() == 1);
        auto changed = doc();
        changed.rules[0].enabled = false;
        check(tx.save(changed, error) == SaveResult::Saved);
        check(enabled_paths(tx.stored().document).empty() &&
              tx.stored().document.rules[0].id == std::string(32, 'a'));
    }
    check(lock_fd < 0 && descriptors.empty());
    for (int fault = 0; fault < 8; ++fault) {
        reset();
        if (fault == 0)
            fail_prepare = true;
        if (fault == 1)
            fail_open = true;
        if (fault == 2)
            fail_stat = true;
        if (fault == 3)
            metadata.st_mode = 0040600;
        if (fault == 4)
            metadata.st_nlink = 2;
        if (fault == 5)
            metadata.st_uid = 1000;
        if (fault == 6)
            metadata.st_mode = regular | 0644;
        if (fault == 7)
            fail_lock = true;
        {
            Transaction tx;
            check(!tx.begin(error) && !error.empty() && reads == 0 && writes.empty());
        }
        check(descriptors.empty() && lock_fd < 0);
    }
    reset();
    put("user_hide_rules.json", "future format");
    {
        Transaction tx;
        check(!tx.begin(error) && writes.empty());
    }
    reset();
    const auto formatted = stringify_json(document_json(doc()), 2) + "\n";
    put("user_hide_rules.json", formatted);
    {
        Transaction tx;
        check(tx.begin(error));
        check(tx.save(doc(), error) == SaveResult::Unchanged);
        check(writes.empty() && tx.stored().bytes == formatted &&
              files.at(key("user_hide_rules.json")).bytes == formatted);
    }
    reset();
    put("user_hide_rules.json", encode_document(doc()));
    {
        Transaction tx;
        check(tx.begin(error));
        const auto before = tx.stored().bytes;
        auto invalid = doc();
        invalid.rules[0].id = "bad";
        check(tx.save(invalid, error) == SaveResult::NotSaved && writes.empty());
        invalid = doc();
        invalid.source_schema = 9;
        check(tx.save(invalid, error) == SaveResult::NotSaved && writes.empty());
        put("user_hide_rules.json", before + " ");
        check(tx.save(doc(), error) == SaveResult::NotSaved && writes.empty());
    }
    reset();
    {
        Transaction tx;
        check(tx.begin(error));
        put("user_hide_rules.json", encode_document(doc()));
        check(tx.save(doc(), error) == SaveResult::NotSaved && writes.empty());
    }
    reset();
    put("user_hide_rules.json", encode_document(doc()));
    {
        Transaction tx;
        check(tx.begin(error));
        files.erase(key("user_hide_rules.json"));
        check(tx.save(doc(), error) == SaveResult::NotSaved && writes.empty());
    }
    reset();
    put("user_hide_rules.json", encode_document(doc()));
    {
        Transaction tx;
        check(tx.begin(error));
        files[key("user_hide_rules.json")].unreadable = true;
        check(tx.save(doc(), error) == SaveResult::NotSaved && writes.empty());
    }
    for (int fault = 1; fault <= 3; ++fault) {
        reset();
        put("user_hide_rules.json", encode_document(doc()));
        {
            Transaction tx;
            check(tx.begin(error));
            const auto before = tx.stored().revision;
            auto changed = doc();
            changed.rules[0].note = "updated";
            if (fault == 1)
                fail_write = 1;
            if (fault == 2)
                unconfirmed_write = 1;
            if (fault == 3)
                bad_readback_write = 1;
            check(tx.save(changed, error) ==
                  (fault == 1 ? SaveResult::NotSaved : SaveResult::Unknown));
            check(tx.stored().revision == before && lock_fd >= 0);
            if (fault != 1)
                check(tx.save(changed, error) == SaveResult::Unknown && writes.size() == 1);
        }
    }
    reset();
    {
        Transaction tx;
        check(tx.begin(error));
        fail_hash = hash_calls + 1;
        check(tx.save(doc(), error) == SaveResult::NotSaved && writes.empty());
    }
    reset();
    {
        Transaction tx;
        check(tx.begin(error));
        fail_hash = hash_calls + 2;
        check(tx.save(doc(), error) == SaveResult::NotSaved && writes.empty());
    }
}
void migration_tests() {
    reset();
    std::string error;
    const auto raw = " [ \"/one\", \"/one\", \"/two\" ] \n";
    put("user_hide_rules.json", raw);
    {
        Transaction tx;
        check(tx.begin(error) && tx.stored().document.source_schema == 0 &&
              tx.stored().document.rules.size() == 2);
        const auto stable = tx.stored().document.rules[0].id;
        check(tx.save(tx.stored().document, error) == SaveResult::Saved);
        check(writes.size() == 2 && files.at(key("user_hide_rules.legacy.bak")).bytes == raw);
        check(tx.stored().document.source_schema == 1 &&
              tx.stored().document.rules[0].id == stable);
    }
    for (int fault = 0; fault < 6; ++fault) {
        reset();
        put("user_hide_rules.json", raw);
        if (fault == 0)
            put("user_hide_rules.legacy.bak", "different");
        if (fault == 1) {
            put("user_hide_rules.legacy.bak", raw);
            files[key("user_hide_rules.legacy.bak")].unreadable = true;
        }
        if (fault == 2)
            fail_write = 1;
        if (fault == 3)
            unconfirmed_write = 1;
        if (fault == 4)
            bad_readback_write = 1;
        if (fault == 5)
            after_write = [](const std::string& path) {
                if (path == key("user_hide_rules.legacy.bak"))
                    put("user_hide_rules.json", "[]");
            };
        {
            Transaction tx;
            check(tx.begin(error));
            check(tx.save(tx.stored().document, error) == SaveResult::NotSaved);
            check(files.at(key("user_hide_rules.json")).bytes == (fault == 5 ? "[]" : raw));
            check(writes.size() == static_cast<std::size_t>(fault < 2 ? 0 : 1));
        }
    }
    reset();
    put("user_hide_rules.json", raw);
    put("user_hide_rules.legacy.bak", raw);
    {
        Transaction tx;
        check(tx.begin(error));
        check(tx.save(tx.stored().document, error) == SaveResult::Saved && writes.size() == 2);
    }
    for (int fault = 1; fault <= 3; ++fault) {
        reset();
        put("user_hide_rules.json", raw);
        {
            Transaction tx;
            check(tx.begin(error));
            if (fault == 1)
                fail_write = 2;
            if (fault == 2)
                unconfirmed_write = 2;
            if (fault == 3)
                bad_readback_write = 2;
            check(tx.save(tx.stored().document, error) ==
                  (fault == 1 ? SaveResult::NotSaved : SaveResult::Unknown));
            check(files.at(key("user_hide_rules.legacy.bak")).bytes == raw &&
                  tx.stored().document.source_schema == 0);
            check(writes.size() == 2);
        }
    }
    reset();
    put("user_hide_rules.json", "[]");
    {
        Transaction tx;
        check(tx.begin(error));
        check(tx.save({}, error) == SaveResult::Saved && writes.size() == 2);
        check(files.at(key("user_hide_rules.legacy.bak")).bytes == "[]" &&
              tx.stored().document.source_schema == 1);
    }
    reset();
    put("user_hide_rules.json", raw);
    {
        Transaction tx;
        check(tx.begin(error));
        auto invalid = doc();
        invalid.rules[0].note = std::string(1025, 'x');
        check(tx.save(invalid, error) == SaveResult::NotSaved && writes.empty() &&
              !files.count(key("user_hide_rules.legacy.bak")));
    }
}
void wrapper_tests() {
    reset();
    std::string error;
    std::vector<std::string> paths = {"/sentinel"};
    check(load_user_hide_rules(paths, error) && paths.empty() && opens == 0 && prepares == 0);
    put("user_hide_rules.json", "future");
    paths = {"/sentinel"};
    check(!load_user_hide_rules(paths, error) && paths == std::vector<std::string>({"/sentinel"}));
    reset();
    put("user_hide_rules.json", legacy({"/one"}));
    check(save_user_hide_rules({"/one", "/two"}, error));
    Stored source;
    check(read_store(source, error) && source.document.source_schema == 1 &&
          source.document.rules.size() == 2);
    auto disabled = source.document;
    disabled.rules[1].enabled = false;
    disabled.rules[1].note = "preserve me";
    put("user_hide_rules.json", encode_document(disabled));
    check(load_user_hide_rules(paths, error) && paths == std::vector<std::string>({"/one"}));
    const auto before = files.at(key("user_hide_rules.json")).bytes;
    const auto count = writes.size();
    check(save_user_hide_rules({"/one"}, error) && writes.size() == count);
    check(!save_user_hide_rules({}, error) && errno == EOPNOTSUPP &&
          files.at(key("user_hide_rules.json")).bytes == before);
    check(!save_user_hide_rules({"/one", "/two"}, error) &&
          files.at(key("user_hide_rules.json")).bytes == before);
    check(!save_user_hide_rules({"relative"}, error) &&
          files.at(key("user_hide_rules.json")).bytes == before);
    check(load_user_hide_rules() == std::vector<std::string>({"/one"}));
}
void portable_tests() {
    reset();
    std::string error, bytes;
    Document value, original = doc();
    original.rules[0].path = "/中文\n\r\t😀 ";
    original.rules[0].name = " 名称 ";
    original.rules[0].note = "\"\\\b\f\1\n ";
    original.rules[0].group = "--automatic";
    original.rules[0].enabled = false;
    check(encode_portable_document(original, bytes, error));
    check(parse_portable_document(bytes, value, error) &&
          encode_document(value) == encode_document(original));
    auto root = json::parse(bytes);
    check(root.o.size() == 3 && root.o.at("kind").s == "sumh_user_hide_definitions" &&
          root.o.at("rules").a[0].o.size() == 6);
    for (const auto& invalid : std::vector<std::string>{
             "[]", legacy({"/one"}), encode_document(original), bytes + "{}",
             std::string(1, char(0xff)), std::string(HIDE_EXCHANGE_MAX_BYTES + 1, ' '),
             "{\"schema_version\":1,\"schema_version\":1}"}) {
        const auto before = encode_document(value);
        check(!parse_portable_document(invalid, value, error) && encode_document(value) == before);
    }
    for (const auto& key : std::vector<std::string>{"schema_version", "kind", "rules"}) {
        auto invalid = root;
        invalid.o.erase(key);
        check(!parse_portable_document(stringify_json(invalid), value, error));
    }
    for (int fault = 0; fault < 9; ++fault) {
        auto invalid = root;
        if (fault == 0)
            invalid.o["schema_version"] = 2;
        if (fault == 1)
            invalid.o["kind"] = "other";
        if (fault == 2)
            invalid.o["extra"] = true;
        if (fault == 3)
            invalid.o["rules"].a.push_back(invalid.o["rules"].a.front());
        if (fault == 4)
            invalid.o["rules"].a.front().o["enabled"] = 0;
        if (fault == 5)
            invalid.o["rules"].a.front().o["path"] = "relative";
        if (fault == 6)
            invalid.o["rules"].a.front().o["note"] = std::string(1025, 'x');
        if (fault == 7)
            invalid.o["rules"].a.front().o["path"] = std::string("/a\0b", 4);
        if (fault == 8)
            invalid.o["rules"] = JsonValue::object();
        check(!parse_portable_document(stringify_json(invalid), value, error));
    }
    Document boundary;
    for (int i = 0; i < 4096; ++i) {
        std::ostringstream id;
        id << std::hex << i;
        const auto digits = id.str();
        boundary.rules.push_back({std::string(32 - digits.size(), '0') + digits,
                                  "/" + std::to_string(i),
                                  false,
                                  {},
                                  {},
                                  {}});
    }
    check(encode_portable_document(boundary, bytes, error) &&
          parse_portable_document(bytes, value, error) && value.rules.size() == 4096);
    auto remaining = HIDE_MAX_BYTES - encode_document(boundary).size();
    for (auto& row : boundary.rules) {
        const auto extra = std::min(remaining, 4095 - row.path.size());
        row.path.append(extra, 'x');
        remaining -= extra;
    }
    check(remaining == 0 && encode_document(boundary).size() == HIDE_MAX_BYTES);
    check(encode_portable_document(boundary, bytes, error) && bytes.size() > HIDE_MAX_BYTES &&
          bytes.size() <= HIDE_EXCHANGE_MAX_BYTES);
    check(parse_portable_document(bytes, value, error) &&
          encode_document(value) == encode_document(boundary));
    auto expanded = json::parse(bytes);
    expanded.o["rules"].a.back().o["note"] = "x";
    check(!parse_portable_document(stringify_json(expanded), value, error));
    const auto before = bytes;
    boundary.rules.back().note = "x";
    check(!encode_portable_document(boundary, bytes, error) && bytes == before);
    boundary.rules.back().note.clear();
    boundary.rules.push_back({std::string(32, 'f'), "/overflow", false, {}, {}, {}});
    check(!encode_portable_document(boundary, bytes, error));
    check(parse_document(legacy({"/legacy\n "}), value, error));
    const auto id = value.rules[0].id;
    check(encode_portable_document(value, bytes, error) &&
          parse_portable_document(bytes, value, error) && value.source_schema == 1 &&
          value.rules[0].id == id && value.rules[0].enabled);
    check(writes.empty() && opens == 0 && prepares == 0);
}
}  // namespace
int main() {
    model_tests();
    read_tests();
    transaction_tests();
    migration_tests();
    wrapper_tests();
    portable_tests();
    check(descriptors.empty() && lock_fd < 0);
    std::cout << io_model::assertions << " user hide model/store/wrapper assertions passed\n";
}
