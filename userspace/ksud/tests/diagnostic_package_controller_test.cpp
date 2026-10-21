#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include "core/diagnostic_package.hpp"
#include "core/diagnostics.hpp"

constexpr int O_RDONLY = 0, O_CLOEXEC = 8, CLONE_NEWNS = 16;
namespace fixture {
using V = sumhp::JsonValue;
unsigned checks = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        std::cerr << label << '\n';
        std::exit(1);
    }
}
const std::string id(32, 'a'), revision = "sha256-v1:" + std::string(64, 'b');
int opened = 0, closed = 0, reads = 0, queries = 0, projections = 0, shows = 0, lists = 0,
    creates = 0, removes = 0;
bool present = false, store_unreadable = false, input_failed = false, projection_failed = false;
bool enter_failed = false, restore_failed = false, shift_after_store = false, inside = false,
     same_namespace = false;
bool fail_after_save = false, opted = false;
std::string input, source_status, bytes;
V saved;
void reset() {
    opened = closed = reads = queries = projections = shows = lists = creates = removes = 0;
    present = store_unreadable = input_failed = projection_failed = enter_failed = restore_failed =
        shift_after_store = inside = same_namespace = fail_after_save = opted = false;
    input = "{\"schema_version\":1,\"scenario\":\"boot-protection\"}";
    source_status = "partial";
    bytes = "{\"stored\":\"exact 中文 bytes\"}\n";
    saved = V{};
}
sumhp::diagnostic_package::Reply reply(int code, const char* status) {
    auto value = V::object();
    value["status"] = status;
    value["id"] = id;
    return {code, value};
}
}  // namespace fixture
namespace package_os {
struct Metadata {
    unsigned st_dev = 7, st_ino = 11;
};
int open(const char* path, int flags) {
    using namespace fixture;
    ++opened;
    check(flags == (O_RDONLY | O_CLOEXEC), "namespace open flags");
    if (std::string(path) == "/proc/self/ns/mnt")
        return 10;
    check(std::string(path) == "/proc/1/ns/mnt", "fixed namespace paths");
    return 11;
}
int close(int fd) {
    fixture::check(fd == 10 || fd == 11, "namespace fd cleanup");
    ++fixture::closed;
    return 0;
}
int fstat(int fd, Metadata* out) {
    *out = {};
    if (fd == 10 && !fixture::same_namespace)
        out->st_ino = 10;
    return 0;
}
int setns(int fd, int flags) {
    using namespace fixture;
    check(flags == CLONE_NEWNS, "namespace type");
    if (fd == 11) {
        if (enter_failed) {
            errno = EPERM;
            return -1;
        }
        inside = true;
        return 0;
    }
    check(fd == 10, "restore original namespace");
    if (restore_failed) {
        errno = EPERM;
        return -1;
    }
    inside = false;
    return 0;
}
int stat(const char* path, Metadata* out) {
    using namespace fixture;
    check(std::string(path) == "/proc/self/ns/mnt" || std::string(path) == "/proc/1/ns/mnt",
          "fixed namespace observations");
    *out = {};
    if (shift_after_store && (creates || removes || lists))
        out->st_ino = 99;
    return 0;
}
}  // namespace package_os
namespace sumhp::diagnostics {
bool read_input(const std::string& path, std::string& out, std::string& reason) {
    ++fixture::reads;
    fixture::check(path == "-", "test stdin request");
    if (fixture::input_failed) {
        reason = "PRIVATE_INPUT_ERROR";
        return false;
    }
    out = fixture::input;
    return true;
}
std::string query_diagnostics(const std::string& input) {
    ++fixture::queries;
    fixture::check(input == fixture::input, "exact request preserved");
    return "PRIVATE_RAW_REPORT:" + fixture::source_status;
}
}  // namespace sumhp::diagnostics
namespace sumhp::diagnostic_package {
bool valid_id(const std::string& value) {
    return value.size() == 32 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool valid_revision(const std::string& value) {
    return value.size() == 74 && value.substr(0, 10) == "sha256-v1:" &&
           value.find_first_not_of("0123456789abcdef", 10) == std::string::npos;
}
bool build_package(const std::string& raw, bool include, JsonValue& out, std::string& reason) {
    using namespace fixture;
    ++projections;
    opted = include;
    check(raw == "PRIVATE_RAW_REPORT:" + source_status, "project exact sampled report");
    if (projection_failed) {
        reason = "PRIVATE_PROJECTION_ERROR";
        return false;
    }
    out = V::object();
    out["privacy"] = V::object();
    out["privacy"]["policy"] = include ? "identifiers_included" : "redacted";
    out["diagnostic"] = V::object();
    out["diagnostic"]["status"] = source_status;
    return true;
}
Reply list_packages() {
    ++fixture::lists;
    return fixture::reply(0, "observed");
}
Reply show_package(const std::string& wanted, const std::string& expected) {
    using namespace fixture;
    ++shows;
    check(wanted == id, "fixed selected package");
    if (store_unreadable)
        return reply(5, "not_saved");
    if (!present)
        return reply(3, "not_found");
    if (!expected.empty() && expected != revision)
        return reply(4, "conflict");
    auto out = reply(0, "observed");
    out.value["export_bytes"] = bytes;
    out.value["export_byte_count"] = static_cast<int>(bytes.size());
    out.value["package_revision"] = revision;
    out.value["package"] = saved;
    return out;
}
Reply create_package(const std::string& wanted, const JsonValue& package) {
    using namespace fixture;
    ++creates;
    check(wanted == id && !present, "create one fresh ID");
    saved = package;
    present = true;
    if (fail_after_save)
        throw std::runtime_error("PRIVATE_EXCEPTION_TEXT");
    auto out = reply(0, "saved");
    out.value["diagnostic_status"] = package.at("diagnostic").at("status");
    return out;
}
Reply remove_package(const std::string& wanted, const std::string& expected) {
    using namespace fixture;
    ++removes;
    check(wanted == id, "targeted remove ID");
    if (expected != revision)
        return reply(4, "conflict");
    present = false;
    return reply(0, "deleted");
}
}  // namespace sumhp::diagnostic_package
#include "diagnostic_package_controller_under_test.inc"

int main() {
    using namespace fixture;
    using sumhp::diagnostic_package::command;
    std::ostringstream output;
    auto* original = std::cout.rdbuf(output.rdbuf());
    auto call = [&](const std::vector<std::string>& args) {
        output.str({});
        output.clear();
        return command(args);
    };
    const std::vector<std::string> create = {"doctor", "package", "create", id, "--input", "-"};
    reset();
    check(call(create) == 0 && queries == 1 && projections == 1 && creates == 1 && !opted,
          "default create samples and redacts once");
    check(saved.at("privacy").at("policy").s == "redacted" &&
              output.str().find("partial") != std::string::npos,
          "partial source status preserved separately");
    check(opened == closed && !inside, "namespace restored and handles closed");
    auto opt = create;
    opt.push_back("--include-identifiers");
    reset();
    source_status = "unavailable";
    check(call(opt) == 0 && opted && saved.at("privacy").at("policy").s == "identifiers_included",
          "explicit identifier opt in");
    reset();
    present = true;
    check(call(create) == 4 && !queries && !projections && !creates, "same ID never resamples");
    reset();
    store_unreadable = true;
    check(call(create) == 5 && !queries && !creates, "unreadable store blocks sampling");
    reset();
    projection_failed = true;
    check(call(create) == 5 && queries == 1 && !creates,
          "no raw fallback after projection failure");
    check(output.str().find("PRIVATE_") == std::string::npos, "failure does not leak raw text");
    reset();
    input_failed = true;
    check(call(create) == 2 && !opened && !queries, "failed input before namespace IO");
    reset();
    input = "{}";
    check(call(create) == 2 && !opened && !queries, "invalid doctor request rejected early");
    for (const auto& args : std::vector<std::vector<std::string>>{
             {},
             {"doctor", "package"},
             {"doctor", "package", "unknown"},
             {"doctor", "package", "list", "extra"},
             {"doctor", "package", "create", "../../secret", "--input", "-"},
             {"doctor", "package", "create", id, "--input", "-", "--raw"},
             {"doctor", "package", "export", id},
             {"doctor", "package", "remove", id, "--revision", "bad"},
             {"doctor", "package", "preview", id, "--include-identifiers"},
             {"doctor", "package", "manager", "--channel"}}) {
        reset();
        check(call(args) == 2 && !reads && !opened && !queries && !creates && !removes,
              "malformed grammar has no side effects");
    }
    reset();
    present = true;
    check(call({"doctor", "package", "preview", id}) == 0 && !queries && !reads && !projections,
          "preview only stored content");
    auto preview = json::parse(output.str());
    check(preview.at("export_bytes").s == bytes &&
              preview.at("export_byte_count").n == bytes.size() &&
              preview.at("package_revision").s == revision,
          "complete exact preview content and revision");
    check(call({"doctor", "package", "export", id, "--revision", revision}) == 0 &&
              output.str() == bytes && !queries && !projections,
          "export byte-for-byte including final LF");
    auto other = "sha256-v1:" + std::string(64, 'c');
    check(call({"doctor", "package", "export", id, "--revision", other}) == 4 &&
              output.str() != bytes && present,
          "stale export returns no payload");
    check(
        call({"doctor", "package", "remove", id, "--revision", other}) == 4 && present && !queries,
        "stale removal retains record");
    check(call({"doctor", "package", "remove", id, "--revision", revision}) == 0 && !present &&
              !queries,
          "targeted removal without sampling");
    reset();
    check(call({"doctor", "package", "list"}) == 0 && lists == 1 && !queries && !reads,
          "read-only list");
    reset();
    enter_failed = true;
    check(call(create) == 5 && !shows && !queries && !creates && opened == closed,
          "failed namespace entry before storage");
    reset();
    restore_failed = true;
    check(call(create) == 5 && creates == 1 && present &&
              output.str().find("unknown") != std::string::npos,
          "restore failure after save is unknown");
    reset();
    shift_after_store = true;
    check(call(create) == 5 && present && output.str().find("unknown") != std::string::npos,
          "post-save namespace change is unknown");
    reset();
    present = true;
    restore_failed = true;
    check(call({"doctor", "package", "export", id, "--revision", revision}) == 5 &&
              output.str() != bytes,
          "namespace failure suppresses export payload");
    reset();
    same_namespace = true;
    check(call(create) == 0 && creates == 1 && !inside, "already in init namespace");
    reset();
    fail_after_save = true;
    check(call(create) == 5 && present && !output.str().empty() &&
              output.str().find("PRIVATE_") == std::string::npos,
          "post-save exception fixed unknown receipt");
    reset();
    std::cout.setstate(std::ios::badbit);
    check(command(create) == 5 && present && creates == 1,
          "broken output does not delete successful create");
    std::cout.clear();
    reset();
    present = true;
    std::cout.setstate(std::ios::badbit);
    check(command({"doctor", "package", "export", id, "--revision", revision}) == 5 && present &&
              !queries,
          "failed export retains package");
    std::cout.clear();
    namespace manager = sumhp::diagnostic_package_manager;
    const auto request_for = [&](const std::string& action) {
        auto value = V::object();
        value["schema_version"] = 1;
        value["action"] = action;
        if (action != "list" && action != "capabilities")
            value["id"] = id;
        if (action == "export" || action == "remove")
            value["revision"] = revision;
        if (action == "create") {
            value["include_identifiers"] = false;
            value["diagnostic"] =
                json::parse(R"({"schema_version":1,"scenario":"boot-protection"})");
        }
        return value;
    };
    const auto manager_call = [&](const V& value) {
        manager::Request request;
        check(manager::parse(json::dump(value), request), "valid Manager request");
        if (request.action == "create")
            input = request.diagnostic;
        const auto raw = sumhp::diagnostic_package::manager_dispatch(request);
        check(raw.size() <= manager::response_bytes && raw.find('\n') == std::string::npos,
              "compact bounded dispatch");
        auto out = json::parse(raw);
        check(out.at("schema_version").n == 1 &&
                  out.at("resource").s == "diagnostic_package_dispatch" &&
                  out.at("action").s == request.action,
              "dispatch action/version binding");
        check(request.id.empty() ? out.at("id").is_null() : out.at("id").s == id,
              "dispatch ID binding");
        return out;
    };
    reset();
    auto capabilities = manager_call(request_for("capabilities"));
    const auto& capability = capabilities.at("result");
    check(capabilities.at("exit_code").n == 0 && !opened && !shows && !queries && !lists &&
              !creates && !removes,
          "capabilities does not enter namespace or touch store/sample");
    check(
        capability.at("kind").s == "sumh.diagnostic.package.capabilities" &&
            capability.at("manager_schema").n == 1 && capability.at("package_schema").n == 1 &&
            capability.at("default_privacy").s == "redacted" &&
            capability.at("package_bytes").n == 98304 && capability.at("bank_bytes").n == 1048576 &&
            capability.at("package_count_limit").n == 8 && capability.at("identifiers_opt_in").b &&
            capability.at("sampling_read_only").b && capability.at("storage_mutations").b,
        "fixed accurate capabilities");
    for (const auto* action : {"create", "list", "preview", "export", "remove"})
        check(capability.at(action).b, "advertised package action");
    reset();
    auto created = manager_call(request_for("create"));
    check(created.at("exit_code").n == 0 && queries == 1 && creates == 1 && !opted && !reads &&
              opened == closed && !inside,
          "Manager create shares controller/store/model with inline request");
    auto duplicate_create = manager_call(request_for("create"));
    check(duplicate_create.at("exit_code").n == 4 && queries == 1 && creates == 1,
          "Manager duplicate ID never resamples");
    auto previewed = manager_call(request_for("preview"));
    check(previewed.at("exit_code").n == 0 && !previewed.at("result").contains("package") &&
              previewed.at("result").at("export_bytes").s == bytes &&
              previewed.at("result").at("export_byte_count").n == bytes.size() &&
              previewed.at("result").at("package_revision").s == revision,
          "Manager preview retains exact bytes/count/revision without redundant package");
    auto exported = manager_call(request_for("export"));
    check(exported.at("exit_code").n == 0 && exported.at("result").at("export_bytes").s == bytes &&
              !exported.at("result").contains("package") && queries == 1,
          "Manager export wraps exact bytes with no sampling or projection");
    auto stale = request_for("export");
    stale["revision"] = other;
    check(manager_call(stale).at("exit_code").n == 4 && present,
          "Manager stale export does not expose bytes");
    stale["action"] = "remove";
    check(manager_call(stale).at("exit_code").n == 4 && present,
          "Manager stale removal retains source");
    check(manager_call(request_for("remove")).at("exit_code").n == 0 && !present && queries == 1,
          "Manager removes only selected reviewed copy");
    reset();
    check(manager_call(request_for("list")).at("exit_code").n == 0 && lists == 1 && !reads &&
              !queries,
          "Manager list only storage query");
    reset();
    auto included = request_for("create");
    included["include_identifiers"] = true;
    check(manager_call(included).at("exit_code").n == 0 && opted,
          "Manager explicit identifier opt in");
    reset();
    fail_after_save = true;
    auto unknown = manager_call(request_for("create"));
    check(unknown.at("exit_code").n == 5 && unknown.at("result").at("status").s == "unknown" &&
              present && json::dump(unknown).find("PRIVATE_") == std::string::npos,
          "Manager postcommit exception fixed unknown receipt");
    reset();
    restore_failed = true;
    unknown = manager_call(request_for("create"));
    check(unknown.at("exit_code").n == 5 && unknown.at("result").at("status").s == "unknown" &&
              present,
          "Manager postcommit namespace restore uncertainty is unknown");
    reset();
    present = true;
    restore_failed = true;
    unknown = manager_call(request_for("export"));
    check(unknown.at("exit_code").n == 5 && !unknown.at("result").contains("export_bytes"),
          "namespace failure suppresses private export too");
    for (const auto& action : {"capabilities", "list", "preview", "export", "remove", "create"}) {
        manager::Request parsed;
        const auto value = request_for(action);
        const auto raw = json::dump(value);
        check(manager::parse(raw, parsed), "shared grammar accepts supported action");
        check(manager::parse_channel_arguments({"doctor", "package", "manager", raw, "--channel"},
                                               parsed),
              "exact private argv grammar");
        for (const auto& args : std::vector<std::vector<std::string>>{
                 {"doctor", "package", "manager", raw},
                 {"doctor", "package", "manager", raw, "--channel", "extra"},
                 {"doctor", "manager", raw, "--channel"},
                 {"doctor", "package", "manager", raw, "--input"}})
            check(!manager::parse_channel_arguments(args, parsed),
                  "reject private route extras/missing fields");
        auto padded = raw;
        padded.resize(manager::input_bytes, ' ');
        check(manager::parse(padded, parsed) && !manager::parse(padded + " ", parsed),
              "wrapper exact capacity");
        auto bad = value;
        bad["extra"] = false;
        check(!manager::parse(json::dump(bad), parsed), "unknown fields fail closed");
        bad = value;
        bad["schema_version"] = 2;
        check(!manager::parse(json::dump(bad), parsed), "future wrapper version rejected");
        bad = value;
        bad["schema_version"] = true;
        check(!manager::parse(json::dump(bad), parsed), "boolean is not schema number");
    }
    manager::Request untouched;
    untouched.action = "sentinel";
    std::vector<std::string> invalid = {
        "{}",
        "[]",
        "null",
        "{} {}",
        std::string("{}\0", 3),
        std::string(1, char(0xff)),
        R"({"schema_version":1,"action":"list","action":"list"})",
        R"({"schema_version":1,"action":"list","id":null})",
        R"({"schema_version":1,"action":"create","id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","include_identifiers":false,"diagnostic":{"schema_version":1,"scenario":"boot-protection","scenario":"boot-protection"}})"};
    for (const auto& field : {"id", "include_identifiers", "diagnostic"}) {
        auto bad = request_for("create");
        bad.o.erase(field);
        invalid.push_back(json::dump(bad));
        bad = request_for("create");
        bad[field] = 1;
        invalid.push_back(json::dump(bad));
    }
    auto bad = request_for("preview");
    bad["id"] = std::string(32, 'A');
    invalid.push_back(json::dump(bad));
    bad = request_for("preview");
    bad["revision"] = revision;
    invalid.push_back(json::dump(bad));
    bad = request_for("export");
    bad.o.erase("revision");
    invalid.push_back(json::dump(bad));
    bad = request_for("export");
    bad["revision"] = "sha256-v1:" + std::string(64, 'A');
    invalid.push_back(json::dump(bad));
    bad = request_for("create");
    bad["diagnostic"]["schema_version"] = 2;
    invalid.push_back(json::dump(bad));
    bad = request_for("create");
    bad["diagnostic"]["scenario"] = "module-fallback";
    bad["diagnostic"]["module_id"] = std::string(4096, 'a');
    invalid.push_back(json::dump(bad));
    for (const auto& raw : invalid)
        check(!manager::parse(raw, untouched) && untouched.action == "sentinel",
              "malformed Manager request preserves output");
    for (const bool operation : {false, true}) {
        const std::string prefix =
            R"({"schema_version":1,"scenario":"module-fallback","module_id":")";
        const std::string suffix =
            std::string("\"") + (operation ? ",\"operation_id\":\"" + id + "\"" : "") + "}";
        for (const std::size_t limit : {std::size_t{4095}, std::size_t{4096}, std::size_t{4097}}) {
            const auto selector = std::string(limit - prefix.size() - suffix.size(), 'a');
            const auto diagnostic = prefix + selector + suffix;
            const auto wrapper =
                std::string("{\"schema_version\":1,\"action\":\"create\",\"id\":\"") + id +
                "\",\"include_identifiers\":false,\"diagnostic\":" + diagnostic + "}";
            check(diagnostic.size() == limit && wrapper.size() < 8192,
                  "independently compact Kotlin-equivalent boundary request");
            manager::Request parsed;
            parsed.action = "unchanged";
            const bool accepted = manager::parse(wrapper, parsed);
            check(accepted == (limit <= 4096),
                  "compact diagnostic exact4096 accepted and plus1 rejected");
            if (accepted) {
                auto child = json::parse(parsed.diagnostic);
                check(parsed.diagnostic.size() == limit && child.at("module_id").s == selector &&
                          child.contains("operation_id") == operation,
                      "normalization preserves scalar content and compact byte budget");
            } else
                check(parsed.action == "unchanged", "diagnostic capacity failure preserves output");
        }
    }
    // Whitespace in a string must remain meaningful: an invalid selector cannot
    // become valid by stripping spaces while compacting the enclosing object.
    bad = request_for("create");
    bad["diagnostic"]["scenario"] = "module-fallback";
    bad["diagnostic"]["module_id"] = "module name";
    check(!manager::parse(json::dump(bad), untouched) && untouched.action == "sentinel",
          "string whitespace not removed by compact normalization");
    std::cout.rdbuf(original);
    std::cout << checks
              << " package CLI/Manager orchestration and grammar checks passed (mocked "
                 "OS/store/query/model).\n";
}
