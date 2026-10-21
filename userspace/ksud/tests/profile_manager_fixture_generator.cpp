// Cross-language fixtures use production controller, store/model, validators
// and mbedTLS SHA256. Only namespace, filesystem/lock and durable IO are doubles.
#include <mbedtls/sha256.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include "core/config_document.hpp"
#include "core/profile.hpp"
#include "core/user_hide_store.hpp"
#include "durable_file.hpp"
#include "mount/backend.hpp"
#include "sumhp/config.hpp"
#include "uapi/sumh.h"
#include "userspace/common/utf8.hpp"

namespace fixture_io {
struct Metadata {
    unsigned st_mode = 0100600, st_nlink = 1, st_uid = 0, st_dev = 1, st_ino = 1;
};
std::map<std::string, std::string> files;
bool unreadable_source = false, namespace_failure = false, restore_failure = false,
     unconfirmed = false, fail_restore_after_write = false;
int next_fd = 10;
template <class Character>
int open(const Character*, int, int = 0) {
    return ++next_fd;
}
int close(int) {
    return 0;
}
int fstat(int fd, Metadata* value) {
    *value = {};
    value->st_ino = static_cast<unsigned>(fd);
    return 0;
}
int flock(int, int) {
    return 0;
}
int setns(int, int) {
    if (namespace_failure || restore_failure) {
        errno = EPERM;
        return -1;
    }
    return 0;
}
}  // namespace fixture_io
constexpr int O_RDONLY = 0, O_RDWR = 1, O_CREAT = 2, O_CLOEXEC = 4, O_NOFOLLOW = 8, O_NONBLOCK = 16;
constexpr int LOCK_EX = 32, LOCK_NB = 64;
#ifndef CLONE_NEWNS
constexpr int CLONE_NEWNS = 128;
#endif
#define S_ISREG(mode) (((mode) & 0170000) == 0100000)
namespace sumhp {
#include "profile_config_validators.inc"
namespace mount {
#include "profile_module_validators.inc"
}
std::filesystem::path runtime_data_dir() {
    return "/fixture/data/adb/ksu/sumhp";
}
std::filesystem::path runtime_config_file() {
    return runtime_data_dir() / "config.json";
}
bool prepare_private_directory(const std::filesystem::path&, std::string&) {
    return true;
}
bool sync_private_directory(const std::filesystem::path&, std::string&) {
    return true;
}
namespace checked {
struct Document {
    std::string status, bytes;
    int error = 0;
    bool operator==(const Document& other) const {
        return status == other.status && bytes == other.bytes && error == other.error;
    }
};
Document read_document(const std::filesystem::path& path, std::size_t limit = 4 * 1024 * 1024) {
    if (fixture_io::unreadable_source && path.filename() == "config.json")
        return {"unreadable", {}, EACCES};
    const auto found = fixture_io::files.find(path.generic_string());
    if (found == fixture_io::files.end())
        return {"missing", {}, ENOENT};
    if (found->second.size() > limit)
        return {"unreadable", {}, EOVERFLOW};
    return {"valid", found->second, 0};
}
}  // namespace checked
namespace state {
#include "profile_real_digest.inc"
}
namespace profiles {
bool read_profile_input(const std::string&, std::string&, std::string&) {
    throw std::runtime_error("CLI file input is not used by fixture generator");
}
}  // namespace profiles
}  // namespace sumhp
namespace ksud {
DurableWriteResult replace_file_durable(const std::filesystem::path& path, const std::string& bytes,
                                        mode_t) {
    fixture_io::files[path.generic_string()] = bytes;
    if (fixture_io::fail_restore_after_write)
        fixture_io::restore_failure = true;
    return {fixture_io::unconfirmed ? DurableWriteState::ReplacedDurabilityUnconfirmed
                                    : DurableWriteState::Complete,
            fixture_io::unconfirmed ? EIO : 0};
}
}  // namespace ksud
#include "profile_fixture_arguments.inc"
#include "profile_fixture_controller.inc"
#include "profile_fixture_store.inc"

namespace {
using namespace sumhp;
using namespace sumhp::profiles;
std::filesystem::path output_dir;
JsonValue fixture_index = JsonValue::array();
void write(const std::string& name, const std::string& bytes) {
    std::ofstream out(output_dir / name, std::ios::binary);
    out << bytes;
    if (!out.good())
        throw std::runtime_error("fixture write failed");
}
JsonValue request(const char* action) {
    auto out = JsonValue::object();
    out.o["schema_version"] = 1;
    out.o["action"] = action;
    return out;
}
Bank bank() {
    Bank out;
    std::string error;
    if (!read_bank(out, error))
        throw std::runtime_error(error);
    return out;
}
JsonValue dispatch(const JsonValue& input) {
    JsonValue out;
    std::string error;
    if (!json::Parser(manager_dispatch(stringify_json(input)), true).parse(out, error))
        throw std::runtime_error(error);
    return out;
}
JsonValue record(const std::string& name, const JsonValue& input) {
    const auto output = dispatch(input);
    write(name + ".request.json", stringify_json(input));
    write(name + ".response.json", stringify_json(output));
    auto item = JsonValue::object();
    item.o["name"] = name;
    item.o["request_file"] = name + ".request.json";
    item.o["response_file"] = name + ".response.json";
    item.o["action"] = input.at("action");
    item.o["status"] = output.at("status");
    fixture_index.a.push_back(item);
    return output;
}
JsonValue selected(const char* action, const char* id) {
    const auto observed = bank();
    auto out = request(action);
    out.o["id"] = id;
    out.o["bank_revision"] = observed.revision;
    out.o["profile_revision"] = profile_revision(observed.profiles.at(id));
    return out;
}
JsonValue mutation(const char* action, const char* id) {
    auto out = request(action);
    out.o["input"] = JsonValue::object();
    auto& input = out.o["input"];
    input.o["schema_version"] = 1;
    input.o["id"] = id;
    input.o["bank_revision"] = bank().revision;
    return out;
}
JsonValue resources() {
    auto out = JsonValue::array();
    for (const auto* name : {"maps", "user_hide", "modules", "settings"})
        out.a.emplace_back(name);
    return out;
}
void canonical(const std::string& name, const JsonValue& input) {
    JsonValue out;
    std::string error;
    if (!parse_profile(stringify_json(input), out, error))
        throw std::runtime_error(error);
    write(name + ".profile.json", stringify_json(out));
    write(name + ".revision.txt", profile_revision(out));
}
}  // namespace
int main(int argc, char** argv) try {
    if (argc != 2)
        return 2;
    output_dir = argv[1];
    std::filesystem::create_directories(output_dir);
    using namespace sumhp;
    using namespace sumhp::profiles;
    auto vectors = JsonValue::array();
    for (const auto& bytes :
         {std::string{}, std::string("abc"), std::string("方案/😀\n\t\b\f\r\\\"")}) {
        auto row = JsonValue::object();
        row.o["input"] = bytes;
        row.o["revision"] = state::revision_for_bytes(bytes);
        vectors.a.push_back(row);
    }
    write("sha256-vectors.json", stringify_json(vectors));
    record("capabilities", request("capabilities"));
    record("empty-list", request("list"));
    JsonValue original;
    std::string error;
    if (!capture_profile("{}", "{}", "{}", "[]", "默认 Default ", "", original, error))
        throw std::runtime_error(error);
    canonical("default", original);
    auto input = mutation("capture", "default");
    input.o["input"].o["name"] = original.at("name");
    input.o["input"].o["note"] = "";
    record("capture-default", input);
    record("show-default", selected("show", "default"));
    auto unicode = original;
    unicode.o["name"] = "方案/😀 é \" ";
    unicode.o["note"] = "\b\f\n\r\t/\\\" 尾 ";
    auto& res = unicode.o["resources"];
    res.o["settings"].o["mountsource"] = "方案/😀\t尾 ";
    auto m = JsonValue::object();
    m.o["id"] = "z.module";
    m.o["mode"] = "sumh";
    m.o["rules"] = JsonValue::array();
    auto r = JsonValue::object();
    r.o["path"] = "/vendor/😀\t尾 ";
    r.o["mode"] = "none";
    m.o["rules"].a.push_back(r);
    res.o["modules"].a.push_back(m);
    m.o["id"] = "a.module";
    res.o["modules"].a.push_back(m);
    auto& hidden = res.o["user_hide"].o["rules"];
    hidden.a.push_back(hide::definition_json({
        std::string(32, 'b'),
        "/隐藏/😀\n尾 ",
        false,
        "名称/😀",
        "\t\r\n",
        "组/ ",
    }));
    hidden.a.push_back(
        hide::definition_json({std::string(32, 'a'), "/prior/same", true, "a", "", ""}));
    auto maps = JsonValue::object();
    maps.o["target_path"] = "/source/😀 尾 ";
    maps.o["replacement_path"] = "/visible/😀 尾 ";
    res.o["maps"].o["enabled"] = true;
    res.o["maps"].o["rules"].a.push_back(maps);
    JsonValue normalized;
    if (!parse_profile(stringify_json(unicode), normalized, error))
        throw std::runtime_error(error);
    canonical("unicode", unicode);
    auto imported = request("import-preview");
    imported.o["profile"] = unicode;
    imported.o["resources"] = resources();
    record("import-preview-unicode", imported);
    input = mutation("import", "unicode");
    input.o["input"].o["profile"] = unicode;
    record("import-unicode", input);
    record("populated-list", request("list"));
    record("show-unicode", selected("show", "unicode"));
    record("export-unicode", selected("export", "unicode"));
    auto preview = selected("preview", "unicode");
    preview.o["resources"] = resources();
    record("preview-unicode", preview);
    auto stale = selected("show", "unicode");
    stale.o["bank_revision"] = "sha256-v1:" + std::string(64, '0');
    record("stale-bank", stale);
    stale = selected("show", "unicode");
    stale.o["profile_revision"] = "sha256-v1:" + std::string(64, '0');
    record("stale-profile", stale);
    input = mutation("clone", "clone");
    input.o["input"].o["source_id"] = "unicode";
    input.o["input"].o["profile_revision"] = profile_revision(normalized);
    input.o["input"].o["name"] = "复制 😀";
    input.o["input"].o["note"] = "";
    record("clone-unicode", input);
    input = mutation("rename", "clone");
    input.o["input"].o["profile_revision"] = profile_revision(bank().profiles.at("clone"));
    input.o["input"].o["name"] = "改名/😀";
    input.o["input"].o["note"] = "note\n";
    record("rename-clone", input);
    input.o["input"].o["bank_revision"] = bank().revision;
    input.o["input"].o["profile_revision"] = profile_revision(bank().profiles.at("clone"));
    record("rename-unchanged", input);
    input = mutation("remove", "clone");
    input.o["input"].o["profile_revision"] = profile_revision(bank().profiles.at("clone"));
    record("remove-clone", input);
    input.o["input"].o["bank_revision"] = bank().revision;
    record("remove-missing", input);
    input = mutation("capture", "not-saved");
    input.o["input"].o["name"] = "capture";
    input.o["input"].o["note"] = "";
    fixture_io::unreadable_source = true;
    record("capture-source-failure", input);
    fixture_io::unreadable_source = false;
    fixture_io::namespace_failure = true;
    record("capture-namespace-failure", input);
    fixture_io::namespace_failure = false;
    input.o["input"].o["extra"] = true;
    record("capture-invalid-input", input);
    input.o["input"].o.erase("extra");
    fixture_io::unconfirmed = true;
    record("capture-unknown-durability", input);
    fixture_io::unconfirmed = false;
    input = mutation("capture", "restore-failed");
    input.o["input"].o["name"] = "saved, namespace unknown";
    input.o["input"].o["note"] = "";
    fixture_io::fail_restore_after_write = true;
    record("capture-namespace-restore-failure", input);
    fixture_io::fail_restore_after_write = false;
    fixture_io::restore_failure = false;
    // Identity-conflict diff uses actual saved source definitions, no rule write.
    auto prior = JsonValue::object();
    prior.o["schema_version"] = 1;
    prior.o["rules"] = JsonValue::array();
    prior.o["rules"].a.push_back(
        hide::definition_json({std::string(32, 'b'), "/different", true, "", "", ""}));
    prior.o["rules"].a.push_back(
        hide::definition_json({std::string(32, 'c'), "/prior/same", true, "", "", ""}));
    fixture_io::files[(runtime_data_dir() / "user_hide_rules.json").generic_string()] =
        stringify_json(prior);
    record("import-preview-hide-conflicts", imported);
    write("index.json", stringify_json(fixture_index));
    std::cout << fixture_index.a.size()
              << " production native request/response fixture pairs generated\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
