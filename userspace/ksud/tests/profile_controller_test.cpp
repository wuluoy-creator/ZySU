#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../common/utf8.hpp"
#include "core/profile.hpp"

constexpr int O_RDONLY = 0, O_CLOEXEC = 4, CLONE_NEWNS = 16;
struct ProfileStat {
    unsigned st_dev = 0, st_ino = 0;
};
namespace sumhp::checked {
struct Document {
    std::string status, bytes;
    int error = 0;
    bool operator==(const Document& other) const {
        return status == other.status && bytes == other.bytes && error == other.error;
    }
};
Document read_document(const std::filesystem::path&);
}  // namespace sumhp::checked
namespace controller_model {
using sumhp::JsonValue;
using sumhp::checked::Document;
int assertions = 0, cases = 0;
int opened = 0, closed = 0, setns_calls = 0, bank_reads = 0, source_reads = 0;
int captures = 0, mutations = 0, diffs = 0, input_reads = 0, profile_parses = 0;
int open_failure = 0, stat_failure = 0, enter_failure = 0, restore_failures = 0;
bool same_namespace = false, bank_failure = false, bank_change = false, source_change = false;
bool capture_failure = false, parse_failure = false, input_failure = false, digest_failure = false;
bool profile_digest_failure = false, throw_mutation = false, huge_diff = false;
std::string supplied_input, mutation_status, capture_name, capture_note;
std::vector<int> switches;
std::vector<std::string> capture_bytes, selected;
JsonValue last_mutation;
sumhp::profiles::Bank bank;
std::map<std::string, Document> sources;
const std::string revision = "sha256-v1:" + std::string(64, 'a');
const std::string changed_revision = "sha256-v1:" + std::string(64, 'b');
void check(bool value) {
    ++assertions;
    if (!value) {
        std::cerr << "controller check " << assertions << " case " << cases << " failed\n";
        std::abort();
    }
}
JsonValue profile(const std::string& name = "Saved") {
    auto v = JsonValue::object();
    v.o["schema_version"] = 1;
    v.o["kind"] = "sumh.profile";
    v.o["name"] = name;
    v.o["note"] = "note ";
    v.o["resources"] = JsonValue::object();
    return v;
}
void reset() {
    ++cases;
    opened = closed = setns_calls = bank_reads = source_reads = captures = mutations = diffs =
        input_reads = profile_parses = 0;
    open_failure = stat_failure = enter_failure = restore_failures = 0;
    same_namespace = bank_failure = bank_change = source_change = capture_failure = parse_failure =
        input_failure = digest_failure = false;
    profile_digest_failure = throw_mutation = huge_diff = false;
    supplied_input.clear();
    mutation_status = "saved";
    capture_name.clear();
    capture_note.clear();
    switches.clear();
    capture_bytes.clear();
    selected.clear();
    last_mutation = JsonValue{};
    sources = {{"config.json", {"missing", {}, 0}},
               {"module_mode.json", {"missing", {}, 0}},
               {"module_rules.json", {"missing", {}, 0}},
               {"user_hide_rules.json", {"missing", {}, 0}}};
    bank = {};
    bank.revision = revision;
    bank.missing = true;
}
int open(const char* path, int flags) {
    check(flags == (O_RDONLY | O_CLOEXEC));
    ++opened;
    check(std::string(path) == (opened == 1 ? "/proc/self/ns/mnt" : "/proc/1/ns/mnt"));
    if (open_failure == opened) {
        errno = EACCES;
        return -1;
    }
    return opened + 10;
}
int close(int fd) {
    check(fd == 11 || fd == 12);
    ++closed;
    return 0;
}
int fstat(int fd, ProfileStat* out) {
    if (stat_failure == fd - 10) {
        errno = EIO;
        return -1;
    }
    *out = {1, same_namespace ? 1U : static_cast<unsigned>(fd)};
    return 0;
}
int setns(int fd, int kind) {
    check(kind == CLONE_NEWNS);
    ++setns_calls;
    switches.push_back(fd);
    if (fd == 12 && enter_failure) {
        errno = EPERM;
        return -1;
    }
    if (fd == 11 && restore_failures) {
        --restore_failures;
        errno = EIO;
        return -1;
    }
    return 0;
}
JsonValue mutation_request(const std::string& action) {
    auto v = JsonValue::object();
    v.o["schema_version"] = 1;
    v.o["id"] = "new-profile";
    v.o["bank_revision"] = revision;
    if (action == "capture" || action == "clone" || action == "rename") {
        v.o["name"] = "名称 \n";
        v.o["note"] = "note\r\n ";
    }
    if (action == "clone" || action == "rename" || action == "remove")
        v.o["profile_revision"] = revision;
    if (action == "clone")
        v.o["source_id"] = "prior";
    if (action == "import")
        v.o["profile"] = profile();
    return v;
}
struct Streams {
    std::ostringstream output, error;
    std::streambuf* old_out = std::cout.rdbuf(output.rdbuf());
    std::streambuf* old_err = std::cerr.rdbuf(error.rdbuf());
    ~Streams() {
        std::cout.rdbuf(old_out);
        std::cerr.rdbuf(old_err);
        std::cout.clear();
        std::cerr.clear();
    }
};
}  // namespace controller_model
namespace sumhp {
std::filesystem::path runtime_data_dir() {
    return "/runtime";
}
std::filesystem::path runtime_config_file() {
    return "/runtime/config.json";
}
namespace checked {
Document read_document(const std::filesystem::path& path) {
    using namespace controller_model;
    ++source_reads;
    auto result = sources.at(path.filename().string());
    if (source_change && source_reads == 5)
        result.bytes += "changed";
    return result;
}
}  // namespace checked
namespace state {
std::string revision_for_bytes(const std::string& bytes) {
    controller_model::check(bytes.rfind("sumh.profile.source.v1:", 0) == 0);
    return controller_model::digest_failure ? std::string{} : controller_model::revision;
}
}  // namespace state
namespace profiles {
#include "profile_id_under_test.inc"
bool parse_profile(const std::string& bytes, JsonValue& output, std::string& error) {
    using namespace controller_model;
    ++profile_parses;
    if (parse_failure || !json::Parser(bytes, true).parse(output, error) ||
        output.at("kind").s != "sumh.profile") {
        error = "profile_rejected";
        return false;
    }
    return true;
}
bool capture_profile(const std::string& config, const std::string& modes, const std::string& rules,
                     const std::string& hidden, const std::string& name, const std::string& note,
                     JsonValue& output, std::string& error) {
    using namespace controller_model;
    ++captures;
    capture_bytes = {config, modes, rules, hidden};
    capture_name = name;
    capture_note = note;
    if (capture_failure) {
        error = "source_model_rejected";
        return false;
    }
    output = profile(name);
    return true;
}
bool read_bank(Bank& output, std::string& error) {
    using namespace controller_model;
    ++bank_reads;
    if (bank_failure) {
        error = "bank_rejected";
        return false;
    }
    output = bank;
    if (bank_change && bank_reads > 1)
        output.revision = changed_revision;
    return true;
}
std::string profile_revision(const JsonValue&) {
    return controller_model::profile_digest_failure ? std::string{} : controller_model::revision;
}
JsonValue mutate_bank(const JsonValue& input) {
    using namespace controller_model;
    ++mutations;
    last_mutation = input;
    if (throw_mutation)
        throw std::runtime_error("store boundary failure");
    auto out = JsonValue::object();
    out.o["kind"] = "sumh.profile.mutation";
    out.o["id"] = input.at("id");
    out.o["status"] = mutation_status;
    out.o["saved"] = mutation_status == "saved";
    out.o["active_configuration_saved"] = false;
    out.o["profile_active"] = false;
    out.o["applied"] = false;
    return out;
}
JsonValue diff_profiles(const JsonValue&, const JsonValue&,
                        const std::vector<std::string>& scopes) {
    using namespace controller_model;
    ++diffs;
    selected = scopes;
    auto out = JsonValue::object();
    out.o["count"] = 7;
    if (huge_diff)
        out.o["payload"] = std::string(PROFILE_RESPONSE_MAX_BYTES, 'x');
    return out;
}
bool read_profile_input(const std::string&, std::string& output, std::string& error) {
    using namespace controller_model;
    ++input_reads;
    if (input_failure) {
        error = "input_failed";
        return false;
    }
    output = supplied_input;
    return true;
}
}  // namespace profiles
}  // namespace sumhp
#include "profile_arguments_under_test.inc"
#include "profile_controller_under_test.inc"

int main() {
    using namespace controller_model;
    using namespace sumhp;
    using namespace sumhp::profiles;
    std::string error;
    Request request;
    const auto parsed = [&](const std::vector<std::string>& args) {
        Request out;
        check(parse_arguments(args, out, error));
        return out;
    };
    for (const auto* action : {"list", "show", "export", "preview", "import-preview", "capture",
                               "import", "clone", "rename", "remove"}) {
        reset();
        std::vector<std::string> args{"profile", action};
        if (std::string(action) == "show" || std::string(action) == "export" ||
            std::string(action) == "preview")
            args.push_back("chosen");
        if (std::string(action) == "import-preview" || std::string(action) == "capture" ||
            std::string(action) == "import" || std::string(action) == "clone" ||
            std::string(action) == "rename" || std::string(action) == "remove")
            args.insert(args.end(), {"--input", "/path with spaces\n.json"});
        if (std::string(action) == "preview" || std::string(action) == "import-preview")
            args.insert(args.end(), {"--resources", "all"});
        request = parsed(args);
        check(request.action == action);
        if (!request.scopes.empty())
            check(request.scopes.size() == 4);
        args.push_back("extra");
        Request unchanged;
        unchanged.action = "sentinel";
        check(!parse_arguments(args, unchanged, error) && unchanged.action == "sentinel");
    }
    const std::vector<std::vector<std::string>> invalid = {
        {},
        {"profile"},
        {"profile", "activate"},
        {"profile", "list", "x"},
        {"profile", "show", "../x"},
        {"profile", "show", "UPPER"},
        {"profile", "capture", "--input", "relative"},
        {"profile", "import-preview", "--input", "-", "--resources", ""},
        {"profile", "preview", "x", "--resources", "maps,maps"},
        {"profile", "preview", "x", "--resources", "settings,"},
        {"profile", "preview", "x", "--resources", "all,maps"},
        {"profile", "preview", "x", "--resources", "modules, maps"},
        {"profile", "preview", "x", "--resources", "kernel"},
        {"profile", "clone", "--input", std::string("/x\0y", 4)}};
    for (const auto& args : invalid) {
        reset();
        request.action = "sentinel";
        check(!parse_arguments(args, request, error) && request.action == "sentinel");
    }
    reset();
    request = parsed({"profile", "list"});
    auto out = execute(request, "");
    check(out.at("status").s == "complete" && out.at("profiles").a.empty() && out.at("missing").b);
    check(opened == 2 && closed == 2 && switches == std::vector<int>({12, 11}) && bank_reads == 1 &&
          source_reads == 0 && mutations == 0);
    reset();
    same_namespace = true;
    out = execute(request, "");
    check(out.at("status").s == "complete" && setns_calls == 0 && closed == 2);
    for (int failure = 0; failure < 5; ++failure) {
        reset();
        if (failure < 2)
            open_failure = failure + 1;
        else if (failure < 4)
            stat_failure = failure - 1;
        else
            enter_failure = 1;
        out = execute(request, "");
        check(out.at("status").s == "not_saved" && bank_reads == 0 && source_reads == 0 &&
              mutations == 0);
        check(closed == (failure < 2 ? 1 : 2));
    }
    for (const auto* action : {"capture", "import", "clone", "rename", "remove"}) {
        reset();
        request = parsed({"profile", action, "--input", "-"});
        auto input = mutation_request(action);
        out = execute(request, stringify_json(input));
        check(out.at("status").s == "saved" && mutations == 1 && closed == 2);
        check(last_mutation.at("action").s ==
              ((std::string(action) == "capture" || std::string(action) == "import") ? "create"
                                                                                     : action));
        check(!out.at("active_configuration_saved").b && !out.at("applied").b &&
              !out.at("profile_active").b);
        if (std::string(action) == "capture") {
            check(source_reads == 8 &&
                  capture_bytes == std::vector<std::string>({"{}", "{}", "{}", "[]"}));
            check(capture_name == input.at("name").s && capture_note == input.at("note").s);
            check(out.at("observation").at("stable").b &&
                  !out.at("observation").at("global_atomic").b);
            check(out.at("observation").at("capture_consistency").s == "double_read_observation");
        } else
            check(source_reads == 0 && captures == 0);
        input.o["unexpected"] = true;
        reset();
        out = execute(request, stringify_json(input));
        check(out.at("reason").s == "invalid_input" && opened == 0 && mutations == 0);
    }
    request = parsed({"profile", "capture", "--input", "-"});
    const auto capture = stringify_json(mutation_request("capture"));
    for (const auto& name :
         {"config.json", "module_mode.json", "module_rules.json", "user_hide_rules.json"}) {
        reset();
        sources[name] = {"unreadable", {}, EACCES};
        out = execute(request, capture);
        check(out.at("reason").s == "capture_failed" &&
              out.at("detail").s.rfind("source_unreadable:", 0) == 0 && mutations == 0 &&
              captures == 0);
    }
    reset();
    digest_failure = true;
    out = execute(request, capture);
    check(out.at("detail").s == "source_revision_unavailable" && mutations == 0);
    reset();
    capture_failure = true;
    out = execute(request, capture);
    check(out.at("detail").s == "source_model_rejected" && mutations == 0 && source_reads == 4);
    reset();
    source_change = true;
    out = execute(request, capture);
    check(out.at("detail").s == "source_changed" && !out.at("observation").at("stable").b &&
          mutations == 0);
    reset();
    sources["config.json"] = {"valid", "config bytes", 0};
    sources["user_hide_rules.json"] = {"valid", "hide bytes", 0};
    out = execute(request, capture);
    check(capture_bytes[0] == "config bytes" && capture_bytes[3] == "hide bytes" && mutations == 1);
    reset();
    auto duplicate = capture;
    duplicate.insert(1, "\"id\":\"duplicate\",");
    out = execute(request, duplicate);
    check(out.at("reason").s == "invalid_input" && opened == 0);
    reset();
    out = execute(request, std::string(PROFILE_INPUT_MAX_BYTES + 1, ' '));
    check(out.at("reason").s == "invalid_input" && opened == 0);
    reset();
    auto bad = mutation_request("capture");
    bad.o["schema_version"] = true;
    out = execute(request, stringify_json(bad));
    check(out.at("reason").s == "invalid_input" && opened == 0);
    reset();
    bad = mutation_request("capture");
    bad.o["bank_revision"] = "sha256-v1:wrong";
    out = execute(request, stringify_json(bad));
    check(out.at("reason").s == "invalid_input" && opened == 0);
    for (const auto* action : {"show", "export", "preview"}) {
        reset();
        std::vector<std::string> args{"profile", action, "chosen"};
        if (std::string(action) == "preview")
            args.insert(args.end(), {"--resources", "maps,settings"});
        request = parsed(args);
        bank.profiles.o["chosen"] = profile();
        bank.missing = false;
        out = execute(request, "");
        if (std::string(action) == "export")
            check(stringify_json(out) == stringify_json(profile()));
        else
            check(out.at("status").s == "complete" && out.at("profile_revision").s == revision);
        if (std::string(action) == "preview") {
            check(source_reads == 8 && bank_reads == 2 && diffs == 1 &&
                  selected == std::vector<std::string>({"maps", "settings"}));
            check(out.at("configuration_diff_only").b && !out.at("can_activate").b &&
                  !out.at("runtime_evaluated").b);
            check(out.at("module_and_capability_applicability").s == "unknown_not_evaluated");
        } else
            check(source_reads == 0 && diffs == 0);
        reset();
        out = execute(request, "");
        check(out.at("reason").s == "profile_not_found" && source_reads == 0);
        reset();
        bank_failure = true;
        out = execute(request, "");
        check(out.at("reason").s == "bank_unavailable" && source_reads == 0);
    }
    reset();
    request = parsed({"profile", "preview", "chosen", "--resources", "maps"});
    bank.profiles.o["chosen"] = profile();
    bank_change = true;
    out = execute(request, "");
    check(out.at("reason").s == "bank_changed" && diffs == 0 && mutations == 0);
    reset();
    request = parsed({"profile", "import-preview", "--input", "-", "--resources", "user_hide"});
    out = execute(request, stringify_json(profile()));
    check(out.at("status").s == "complete" && bank_reads == 0 && source_reads == 8 &&
          mutations == 0 && selected == std::vector<std::string>({"user_hide"}));
    reset();
    parse_failure = true;
    out = execute(request, stringify_json(profile()));
    check(out.at("reason").s == "invalid_input" && opened == 0);
    reset();
    request = parsed({"profile", "import", "--input", "-"});
    parse_failure = true;
    out = execute(request, stringify_json(mutation_request("import")));
    check(out.at("reason").s == "invalid_input" && mutations == 0 && opened == 0);
    reset();
    request = parsed({"profile", "capture", "--input", "-"});
    restore_failures = 1;
    out = execute(request, capture);
    check(out.at("namespace_restore_failed").b && out.at("saved").b &&
          out.at("status").s == "saved" && mutations == 1 &&
          switches == std::vector<int>({12, 11, 11}));
    reset();
    request = parsed({"profile", "export", "chosen"});
    bank.profiles.o["chosen"] = profile();
    restore_failures = 1;
    out = execute(request, "");
    check(out.at("kind").s == "sumh.profile.result" &&
          out.at("reason").s == "namespace_restore_failed");
    reset();
    request = parsed({"profile", "capture", "--input", "-"});
    throw_mutation = true;
    bool threw = false;
    try {
        (void)execute(request, capture);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw && mutations == 1 && switches == std::vector<int>({12, 11}) && closed == 2);
    for (const auto* status : {"saved", "unchanged", "unknown", "not_saved"}) {
        reset();
        supplied_input = capture;
        mutation_status = status;
        Streams streams;
        const auto code = command({"profile", "capture", "--input", "-"});
        check(code ==
              ((std::string(status) == "saved" || std::string(status) == "unchanged") ? 0 : 1));
        check(mutations == 1 && input_reads == 1);
        JsonValue receipt;
        check(json::Parser(streams.output.str(), true).parse(receipt, error));
        check(receipt.at("status").s == status);
    }
    reset();
    input_failure = true;
    {
        Streams streams;
        check(command({"profile", "capture", "--input", "-"}) == 1 && opened == 0 &&
              mutations == 0);
    }
    reset();
    {
        Streams streams;
        check(command({"profile", "list", "extra"}) == 2 && opened == 0 && mutations == 0 &&
              !streams.error.str().empty());
    }
    reset();
    supplied_input = capture;
    {
        Streams streams;
        std::cout.setstate(std::ios::badbit);
        check(command({"profile", "capture", "--input", "-"}) == 1 && mutations == 1);
    }
    reset();
    supplied_input = stringify_json(profile());
    huge_diff = true;
    {
        Streams streams;
        check(command({"profile", "import-preview", "--input", "-", "--resources", "all"}) == 1);
        JsonValue receipt;
        check(json::Parser(streams.output.str(), true).parse(receipt, error) &&
              receipt.at("reason").s == "response_capacity" && streams.output.str().size() < 1024);
    }
    // Per-record digest failure must not masquerade as a complete query with
    // an unusable revision. The bank's earlier digest may have succeeded.
    for (const auto* action : {"list", "show", "preview"}) {
        reset();
        bank.profiles.o["chosen"] = profile();
        profile_digest_failure = true;
        std::vector<std::string> args{"profile", action};
        if (std::string(action) != "list")
            args.push_back("chosen");
        if (std::string(action) == "preview")
            args.insert(args.end(), {"--resources", "all"});
        request = parsed(args);
        out = execute(request, "");
        check(out.at("status").s != "complete" &&
              out.at("reason").s == "profile_revision_unavailable" && mutations == 0);
    }
    const auto manager = [&](const JsonValue& document) {
        JsonValue response;
        std::string problem;
        check(json::Parser(manager_dispatch(stringify_json(document)), true)
                  .parse(response, problem));
        return response;
    };
    auto doc = JsonValue::object();
    doc.o["schema_version"] = 1;
    doc.o["action"] = "capabilities";
    reset();
    out = manager(doc);
    check(out.at("kind").s == "sumh.profile.manager.capabilities" && opened == 0 &&
          bank_reads == 0);
    check(out.at("actions").at("import").b && !out.at("activate").b && !out.at("restore").b &&
          !out.at("operation_journal").b);
    check(out.at("limits").at("input_bytes").n == PROFILE_INPUT_MAX_BYTES &&
          out.at("limits").at("response_bytes").n == PROFILE_RESPONSE_MAX_BYTES);
    for (const auto* action : {"show", "export", "preview"}) {
        doc = JsonValue::object();
        doc.o["schema_version"] = 1;
        doc.o["action"] = action;
        doc.o["id"] = "chosen";
        doc.o["bank_revision"] = revision;
        doc.o["profile_revision"] = revision;
        if (std::string(action) == "preview") {
            doc.o["resources"] = JsonValue::array();
            doc.o["resources"].a.emplace_back("maps");
        }
        reset();
        bank.profiles.o["chosen"] = profile();
        out = manager(doc);
        check(out.at("kind").s == "sumh.profile.result" && out.at("action").s == action &&
              out.at("status").s == "complete");
        if (std::string(action) != "preview")
            check(out.at("profile").at("kind").s == "sumh.profile" && bank_reads == 1);
        reset();
        bank.profiles.o["chosen"] = profile();
        doc.o["bank_revision"] = changed_revision;
        out = manager(doc);
        check(out.at("status").s == "stale" && out.at("reason").s == "bank_revision_conflict" &&
              bank_reads == 1 && source_reads == 0);
        reset();
        bank.profiles.o["chosen"] = profile();
        doc.o["bank_revision"] = revision;
        doc.o["profile_revision"] = changed_revision;
        out = manager(doc);
        check(out.at("status").s == "stale" && out.at("reason").s == "profile_revision_conflict" &&
              bank_reads == 1 && source_reads == 0);
        reset();
        doc.o["profile_revision"] = revision;
        doc.o["path"] = "/do/not/open";
        out = manager(doc);
        check(out.at("reason").s == "invalid_input" && opened == 0);
    }
    for (const auto* action : {"capture", "import", "clone", "rename", "remove"}) {
        reset();
        doc = JsonValue::object();
        doc.o["schema_version"] = 1;
        doc.o["action"] = action;
        doc.o["input"] = mutation_request(action);
        out = manager(doc);
        check(out.at("requested_action").s == action && mutations == 1 && input_reads == 0);
        reset();
        restore_failures = 1;
        out = manager(doc);
        check(out.at("namespace_restore_failed").b && out.at("status").s == "saved" &&
              mutations == 1);
        reset();
        enter_failure = 1;
        out = manager(doc);
        check(out.at("reason").s == "enter_init_namespace_failed" &&
              out.at("id").s == "new-profile" && mutations == 0);
        reset();
        doc.o["input"].o["extra"] = true;
        out = manager(doc);
        check(out.at("reason").s == "invalid_input" && out.at("id").s == "new-profile" &&
              mutations == 0 && opened == 0);
        reset();
        doc.o["input"].o["id"] = "../untrusted";
        out = manager(doc);
        check(out.at("id").s.empty() && out.at("reason").s == "invalid_input" && opened == 0);
    }
    reset();
    doc = JsonValue::object();
    doc.o["schema_version"] = 1;
    doc.o["action"] = "capture";
    doc.o["input"] = mutation_request("capture");
    capture_failure = true;
    out = manager(doc);
    check(out.at("id").s == "new-profile" && out.at("reason").s == "capture_failed" &&
          mutations == 0);
    reset();
    doc.o["action"] = "remove";
    doc.o["input"] = mutation_request("remove");
    mutation_status = "not_saved";
    out = manager(doc);
    check(out.at("id").s == "new-profile" && out.at("requested_action").s == "remove" &&
          out.at("status").s == "not_saved");
    doc = JsonValue::object();
    doc.o["schema_version"] = 1;
    doc.o["action"] = "import-preview";
    doc.o["profile"] = profile();
    doc.o["resources"] = JsonValue::array();
    doc.o["resources"].a.emplace_back("maps");
    reset();
    out = manager(doc);
    check(out.at("status").s == "complete" && bank_reads == 0 && mutations == 0 &&
          input_reads == 0);
    reset();
    parse_failure = true;
    out = manager(doc);
    check(out.at("reason").s == "invalid_input" && opened == 0);
    reset();
    doc.o["resources"].a.emplace_back("maps");
    out = manager(doc);
    check(out.at("reason").s == "invalid_input" && opened == 0);
    for (const auto* action : {"activate", "restore", "manager", "clear", "Capture"}) {
        reset();
        doc = JsonValue::object();
        doc.o["schema_version"] = 1;
        doc.o["action"] = action;
        out = manager(doc);
        check(out.at("reason").s == "invalid_input" && opened == 0 && mutations == 0);
    }
    for (const auto& raw :
         {std::string("{\"schema_version\":1,\"action\":\"list\",\"action\":\"capture\"}"),
          std::string("{\"schema_version\":2,\"action\":\"list\"}"),
          std::string("{\"schema_version\":1,\"action\":\"\xff\"}"),
          std::string(PROFILE_INPUT_MAX_BYTES + 1, ' ')}) {
        reset();
        check(manager_dispatch(raw).find("invalid_input") != std::string::npos && opened == 0 &&
              mutations == 0);
    }
    std::cout << cases << " controller scenarios / " << assertions << " assertions passed\n";
}
