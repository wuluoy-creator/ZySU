#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include "core/capture_buffer.hpp"
#include "core/user_hide.hpp"
#include "sumhp/sumh_client.hpp"
#include "uapi/sumh.h"

constexpr int GRND_NONBLOCK = 1;
namespace fixture {
using namespace sumhp;
hide::Stored stored;
std::vector<sumh::UserHideRule> registered;
std::vector<std::string> calls;
std::set<std::string> failed_paths;
std::map<std::string, std::uint64_t> module_hides;
std::uint64_t next_rule_id = 5;
int checks = 0, locks = 0, writes = 0, reads = 0, queries = 0, random_calls = 0;
bool lock_ok = true, read_ok = true, available = true, managed = true, caps_ok = true,
     query_ok = true, random_ok = true;
bool readback_fail = false, mutation_ok = true, apply_effect = true, change_read = false;
bool change_boot = false;
hide::SaveResult save_result = hide::SaveResult::Saved;
const std::string first(32, '1'), second(32, '2'), third(32, '3');
std::string rev(int value) {
    return "sha256-v1:" + std::string(63, 'a') + std::to_string(value);
}
void check(bool value) {
    ++checks;
    if (!value) {
        std::cerr << "controller assertion " << checks << " failed\n";
        std::abort();
    }
}
std::ptrdiff_t getrandom(void* bytes, std::size_t size, unsigned flags) {
    ++random_calls;
    assert(flags == GRND_NONBLOCK);
    if (!random_ok) {
        errno = EIO;
        return -1;
    }
    std::memset(bytes, 0xaa, size);
    return static_cast<std::ptrdiff_t>(size);
}
void reset() {
    stored = {};
    stored.document.source_schema = 1;
    stored.revision = rev(1);
    stored.document.rules = {{first, "/one", true, "one", "note", "group"},
                             {second, "/two", true, "two", "", "group"},
                             {third, "/three", false, "three", "", "other"}};
    registered = {{1, 1, 0, SUMH_USER_HIDE_BOUND, 0, "/one"},
                  {2, 1, 0, SUMH_USER_HIDE_PENDING, 0, "/two"}};
    assert(locks == 0);
    calls.clear();
    failed_paths.clear();
    module_hides.clear();
    next_rule_id = 5;
    locks = writes = reads = queries = random_calls = 0;
    lock_ok = read_ok = available = managed = caps_ok = query_ok = random_ok = mutation_ok =
        apply_effect = true;
    readback_fail = change_read = change_boot = false;
    save_result = hide::SaveResult::Saved;
}
}  // namespace fixture
namespace sumhp {
std::string runtime_boot_id() {
    return fixture::change_boot && fixture::reads > 1 ? "next-boot" : "boot";
}
namespace state {
std::string revision_for_bytes(const std::string& bytes) {
    // Distinct deterministic fixture digest; cryptographic primitive is not tested here.
    std::uint64_t value = 1469598103934665603ULL;
    for (unsigned char c : bytes)
        value = (value ^ c) * 1099511628211ULL;
    std::ostringstream out;
    out << std::hex << value;
    return "sha256-v1:" + out.str() + std::string(64 - out.str().size(), '0');
}
}  // namespace state
namespace hide {
struct Transaction::Impl {
    bool locked = false;
    Stored initial;
};
Transaction::Transaction() : impl_(std::make_unique<Impl>()) {}
Transaction::~Transaction() {
    if (impl_->locked)
        --fixture::locks;
}
bool Transaction::begin(std::string& error) {
    if (!fixture::lock_ok || fixture::locks || impl_->locked) {
        error = "busy";
        return false;
    }
    ++fixture::locks;
    impl_->locked = true;
    impl_->initial = fixture::stored;
    return true;
}
const Stored& Transaction::stored() const {
    return impl_->initial;
}
SaveResult Transaction::save(const Document& document, std::string& error) {
    assert(impl_->locked && fixture::locks == 1);
    ++fixture::writes;
    Document validated;
    if (!parse_document(encode_document(document), validated, error))
        return SaveResult::NotSaved;
    if (fixture::save_result == SaveResult::Saved) {
        fixture::stored.document = validated;
        fixture::stored.revision = fixture::rev(2);
        impl_->initial = fixture::stored;
    }
    return fixture::save_result;
}
bool read_store(Stored& out, std::string& error) {
    ++fixture::reads;
    if (!fixture::read_ok) {
        error = "unreadable";
        return false;
    }
    out = fixture::stored;
    if (fixture::change_read && fixture::reads > 1)
        out.revision = fixture::rev(9);
    return true;
}
}  // namespace hide
namespace sumh {
bool is_available() {
    return fixture::available;
}
bool managed_hide_mode(bool& out) {
    out = fixture::managed;
    return fixture::caps_ok;
}
bool user_hide_rules(std::vector<UserHideRule>& out) {
    ++fixture::queries;
    if (!fixture::query_ok || (fixture::readback_fail && fixture::queries > 1))
        return false;
    out = fixture::registered;
    return true;
}
bool delete_user_hide(const std::string& path, std::uint64_t id) {
    assert(fixture::locks == 1 && id != 0);
    fixture::calls.push_back("delete:" + path + ":" + std::to_string(id));
    if (!fixture::mutation_ok || fixture::failed_paths.count(path)) {
        errno = EIO;
        return false;
    }
    const auto found = std::find_if(fixture::registered.begin(), fixture::registered.end(),
                                    [&](const auto& r) { return r.path == path && r.id == id; });
    if (found == fixture::registered.end()) {
        errno = ENOENT;
        return false;
    }
    if (fixture::apply_effect)
        fixture::registered.erase(
            std::remove_if(fixture::registered.begin(), fixture::registered.end(),
                           [&](const auto& r) { return r.path == path && r.id == id; }),
            fixture::registered.end());
    return true;
}
bool upsert_user_hide(const std::string& path, UserHideRule*) {
    assert(fixture::locks == 1);
    fixture::calls.push_back("upsert:" + path);
    if (!fixture::mutation_ok || fixture::failed_paths.count(path)) {
        errno = EIO;
        return false;
    }
    if (fixture::apply_effect) {
        const auto found = std::find_if(fixture::registered.begin(), fixture::registered.end(),
                                        [&](const auto& r) { return r.path == path; });
        if (found != fixture::registered.end()) {
            ++found->generation;
            found->binding = SUMH_USER_HIDE_PENDING;
            found->error = 0;
        } else
            fixture::registered.push_back(
                {fixture::next_rule_id++, 1, SUMH_USER_HIDE_LIVE, SUMH_USER_HIDE_PENDING, 0, path});
    }
    return true;
}
bool retry_user_hide(const std::string& path) {
    assert(fixture::locks == 1);
    fixture::calls.push_back("retry:" + path);
    if (!fixture::mutation_ok || fixture::failed_paths.count(path)) {
        errno = EIO;
        return false;
    }
    if (std::none_of(fixture::registered.begin(), fixture::registered.end(),
                     [&](const auto& r) { return r.path == path; })) {
        errno = ENOENT;
        return false;
    }
    return true;
}
bool hide_path(const std::string& path) {
    assert(fixture::locks == 1);
    fixture::calls.push_back("legacy-add:" + path);
    return fixture::mutation_ok;
}
bool delete_rule(const std::string& path) {
    assert(fixture::locks == 1);
    fixture::calls.push_back("legacy-delete:" + path);
    return fixture::mutation_ok;
}
}  // namespace sumh
}  // namespace sumhp
#include "user_hide_controller_under_test.inc"
namespace {
using namespace sumhp;
using fixture::check;
std::pair<int, JsonValue> run(const std::vector<std::string>& args, bool legacy = false) {
    std::ostringstream output;
    auto* old = std::cout.rdbuf(output.rdbuf());
    const int code = legacy ? hide::run_path_command(args) : hide::run_definition_command(args);
    std::cout.rdbuf(old);
    JsonValue value;
    std::string error;
    if (!legacy)
        check(json::Parser(output.str(), true).parse(value, error));
    else
        check(output.str().empty());
    check(fixture::locks == 0);
    return {code, value};
}
JsonValue status(const std::vector<std::string>& args = {"hide", "status"}, int expected = 0) {
    hide::Request request;
    std::string error;
    check(hide::parse_request(args, request, error));
    std::ostringstream output;
    auto* old = std::cout.rdbuf(output.rdbuf());
    const auto code = hide::print_status(request);
    std::cout.rdbuf(old);
    check(code == expected && output.str().size() < 56 * 1024);
    JsonValue value;
    check(json::Parser(output.str(), true).parse(value, error));
    return value;
}
void test_input() {
    hide::Request request;
    std::string error;
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"hide", "create", fixture::rev(1),
              "{\"path\":\"/路径 \",\"enabled\":false,\"group\":\" x \",\"note\":\"a\\nb\"}"},
             {"hide", "annotate", fixture::first, fixture::rev(1), "{\"name\":\"new\"}"},
             {"hide", "enable-group", "", fixture::rev(1)},
             {"hide", "disable", fixture::first, fixture::rev(1)},
             {"hide", "status", fixture::rev(1), "4096"}})
        check(hide::parse_request(args, request, error));
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"hide", "create", fixture::rev(1), "{\"path\":\"relative\"}"},
             {"hide", "create", fixture::rev(1), "{\"path\":\"/a\",\"enabled\":0}"},
             {"hide", "create", fixture::rev(1), "{\"path\":\"/a\",\"path\":\"/b\"}"},
             {"hide", "annotate", fixture::first, fixture::rev(1), "{\"enabled\":false}"},
             {"hide", "annotate", fixture::first, fixture::rev(1), "{}"},
             {"hide", "disable", "bad", fixture::rev(1)},
             {"hide", "enable", fixture::first, "future"},
             {"hide", "status", fixture::rev(1), "01"},
             {"hide", "status", fixture::rev(1), "4097"},
             {"hide", "status", "0"}}) {
        request.selector = "sentinel";
        check(!hide::parse_request(args, request, error) && request.selector == "sentinel");
    }
}
void test_mutations() {
    using namespace fixture;
    reset();
    auto value = run({"hide", "disable", first, rev(1)});
    check(value.first == 0 && value.second.o.at("status").s == "applied");
    check(!stored.document.rules[0].enabled && stored.document.rules[0].id == first &&
          stored.document.rules[0].note == "note");
    check(calls == std::vector<std::string>{"delete:/one:1"} && registered.size() == 1);
    reset();
    value = run({"hide", "disable-group", "group", rev(1)});
    check(value.first == 0 && calls.size() == 2 && !stored.document.rules[0].enabled &&
          !stored.document.rules[1].enabled && !stored.document.rules[2].enabled);
    reset();
    value = run({"hide", "enable", third, rev(1)});
    check(value.first == 0 && calls == std::vector<std::string>{"upsert:/three"} &&
          value.second.o.at("items").a[0].o.at("outcome").s == "registered_pending");
    reset();
    value = run({"hide", "retry", third, rev(1)});
    check(value.first == 1 && writes == 0 && calls.empty());
    reset();
    registered.clear();
    value = run({"hide", "retry", first, rev(1)});
    check(value.first == 0 && calls == std::vector<std::string>({"upsert:/one", "retry:/one"}));
    reset();
    available = false;
    value = run(
        {"hide", "annotate", first, rev(1), "{\"group\":\"new group\",\"note\":\"备注\\nline\"}"});
    check(value.first == 0 && calls.empty() && queries == 0 &&
          stored.document.rules[0].group == "new group");
    reset();
    value = run({"hide", "remove-id", third, rev(1)});
    check(value.first == 0 && calls.empty() && stored.document.rules.size() == 2);
    reset();
    value =
        run({"hide", "create", rev(1), "{\"path\":\"/new\",\"enabled\":false,\"group\":\"g\"}"});
    check(value.first == 0 && calls.empty() && stored.document.rules.size() == 4 &&
          !stored.document.rules.back().enabled);
    reset();
    value = run({"hide", "create", rev(1), "{\"path\":\"/one\"}"});
    check(value.first == 1 && writes == 0 && random_calls == 0);
    reset();
    value = run({"hide", "disable", first, rev(9)});
    check(value.first == 1 && writes == 0 && queries == 0 && calls.empty());
    for (int fault = 0; fault < 6; ++fault) {
        reset();
        if (fault == 0)
            lock_ok = false;
        if (fault == 1)
            available = false;
        if (fault == 2)
            managed = false;
        if (fault == 3)
            caps_ok = false;
        if (fault == 4)
            query_ok = false;
        if (fault == 5)
            random_ok = false;
        value = run(fault == 5
                        ? std::vector<std::string>{"hide", "create", rev(1), "{\"path\":\"/new\"}"}
                        : std::vector<std::string>{"hide", "disable", first, rev(1)});
        check(value.first == 1 && writes == 0 && calls.empty());
    }
    for (const auto outcome : {hide::SaveResult::NotSaved, hide::SaveResult::Unknown}) {
        reset();
        save_result = outcome;
        value = run({"hide", "disable", first, rev(1)});
        check(value.first == 1 && calls.empty() && stored.document.rules[0].enabled);
        check(value.second.o.at("status").s ==
              (outcome == hide::SaveResult::Unknown ? "outcome_unknown" : "not_saved"));
    }
    reset();
    mutation_ok = false;
    value = run({"hide", "disable", first, rev(1)});
    check(value.first == 1 && !stored.document.rules[0].enabled &&
          value.second.o.at("status").s == "partial" && value.second.o.at("failed_count").n == 1);
    reset();
    readback_fail = true;
    value = run({"hide", "disable", first, rev(1)});
    check(value.first == 1 && value.second.o.at("unknown_count").n == 1);
    reset();
    apply_effect = false;
    value = run({"hide", "disable", first, rev(1)});
    check(value.first == 1 && value.second.o.at("failed_count").n == 1);
    reset();
    value = run({"hide", "add", "/three"}, true);
    check(value.first == 1 && calls.empty() && writes == 0);
    reset();
    save_result = hide::SaveResult::Unchanged;
    value = run({"hide", "add", "/one"}, true);
    check(value.first == 0 && stored.document.rules[0].note == "note");
    reset();
    managed = false;
    value = run({"hide", "remove", "/missing"}, true);
    check(value.first == 0 && calls == std::vector<std::string>{"legacy-delete:/missing"});
    reset();
    stored.document.rules.clear();
    registered.clear();
    for (int i = 0; i < 70; ++i) {
        std::ostringstream id;
        id << std::hex << i;
        const auto digits = id.str();
        stored.document.rules.push_back({std::string(32 - digits.size(), '0') + digits,
                                         "/" + std::to_string(i), true, "", "", "group"});
    }
    value = run({"hide", "disable-group", "group", rev(1)});
    check(value.first == 0 && value.second.o.at("confirmed_count").n == 70 &&
          value.second.o.at("items").a.size() == 64 && !value.second.o.at("items_complete").b);
}
void test_status() {
    using namespace fixture;
    reset();
    auto value = status();
    check(value.o.at("items").a[2].o.at("state").s == "disabled" && value.o.at("complete").b &&
          writes == 0 && locks == 0 && calls.empty());
    reset();
    registered.push_back({8, 1, 0, 0, 0, "/three"});
    value = status();
    check(value.o.at("items").a[2].o.at("state").s == "disabled_pending_removal");
    reset();
    registered[0].management = SUMH_USER_HIDE_SUSPENDED;
    value = status();
    check(value.o.at("items").a[0].o.at("state").s == "globally_suspended");
    reset();
    query_ok = false;
    value = status();
    check(value.o.at("items").a[0].o.at("registered").is_null());
    reset();
    value = status({"hide", "status", rev(9), "0"}, 1);
    check(value.o.at("items").a.empty() && queries == 0);
    reset();
    change_read = true;
    value = status({"hide", "status"}, 1);
    check(value.o.at("status").s == "changed_or_unavailable" && value.o.at("items").a.empty());
    reset();
    stored.document.rules.clear();
    for (int i = 0; i < 16; ++i)
        stored.document.rules.push_back(
            {std::string(31, 'a') + "0123456789abcdef"[i], "/" + std::string(4094, '\1'), true,
             std::string(128, '\2'), std::string(1024, '\3'), std::string(128, '\4')});
    value = status();
    check(value.o.at("items").a.size() >= 1 && value.o.at("items").a.size() < 16 &&
          !value.o.at("complete").b);
    const auto cursor = std::to_string(static_cast<int>(value.o.at("next_cursor").n));
    value = status({"hide", "status", rev(1), cursor});
    check(value.o.at("cursor").n > 0 && !value.o.at("complete").b);
}
void test_path_retry_and_owned_groups() {
    using namespace fixture;
    reset();
    save_result = hide::SaveResult::Unchanged;
    const auto before = hide::encode_document(stored.document);
    auto value = run({"hide", "add", "/one"}, true);
    check(value.first == 0 && calls == std::vector<std::string>{"upsert:/one"});
    check(registered.size() == 2 && registered[0].id == 1 && registered[0].generation == 2 &&
          registered[0].binding == SUMH_USER_HIDE_PENDING);
    check(hide::encode_document(stored.document) == before && random_calls == 0);
    calls.clear();
    value = run({"hide", "add", "/one"}, true);
    check(value.first == 0 && registered.size() == 2 && registered[0].generation == 3 &&
          calls == std::vector<std::string>{"upsert:/one"});

    reset();
    save_result = hide::SaveResult::Unchanged;
    value = run({"hide", "retry-path", "/one"}, true);
    check(value.first == 0 && calls == std::vector<std::string>{"retry:/one"} &&
          registered.size() == 2);
    check(stored.document.rules[0].id == first && stored.document.rules[0].enabled &&
          stored.document.rules[0].note == "note");
    reset();
    registered.clear();
    value = run({"hide", "retry-path", "/one"}, true);
    check(value.first == 0 && calls == std::vector<std::string>({"upsert:/one", "retry:/one"}) &&
          registered.size() == 1);
    for (const auto* target : {"/missing", "/three"}) {
        reset();
        value = run({"hide", "retry-path", target}, true);
        check(value.first == 1 && writes == 0 && queries == 0 && calls.empty() &&
              random_calls == 0 && stored.document.rules.size() == 3);
        check(!stored.document.rules[2].enabled);
    }
    reset();
    managed = false;
    value = run({"hide", "retry-path", "/one"}, true);
    check(value.first == 0 && calls == std::vector<std::string>{"legacy-add:/one"} && queries == 0);
    reset();
    available = false;
    value = run({"hide", "retry-path", "/one"}, true);
    check(value.first == 0 && calls.empty() && queries == 0 && stored.document.rules[0].enabled);
    reset();
    managed = false;
    value = run({"hide", "retry-path", "/three"}, true);
    check(value.first == 1 && calls.empty() && writes == 0);
    for (const auto result : {hide::SaveResult::NotSaved, hide::SaveResult::Unknown}) {
        reset();
        save_result = result;
        value = run({"hide", "retry-path", "/one"}, true);
        check(value.first == 1 && calls.empty());
    }
    reset();
    failed_paths.insert("/one");
    value = run({"hide", "retry-path", "/one"}, true);
    check(value.first == 1 && calls == std::vector<std::string>{"retry:/one"});

    reset();
    module_hides = {{"/one", 77}, {"/two", 88}};
    registered[0].management = SUMH_USER_HIDE_SUSPENDED;
    registered[1].management = SUMH_USER_HIDE_SUSPENDED;
    value = run({"hide", "disable", first, rev(1)});
    check(value.first == 0 && calls == std::vector<std::string>{"delete:/one:1"});
    check(registered.size() == 1 && registered[0].path == "/two" &&
          registered[0].management == SUMH_USER_HIDE_SUSPENDED);
    check(module_hides == std::map<std::string, std::uint64_t>({{"/one", 77}, {"/two", 88}}));
    check(stored.document.rules[0].id == first && !stored.document.rules[0].enabled &&
          stored.document.rules[1].enabled);
    check(value.second.o.at("boot_id").s == "boot" && !value.second.o.at("runtime_verified").b);

    reset();
    failed_paths.insert("/two");
    module_hides = {{"/one", 77}, {"/two", 88}};
    value = run({"hide", "disable-group", "group", rev(1)});
    check(value.first == 1 && value.second.o.at("status").s == "partial" &&
          value.second.o.at("saved").b);
    check(value.second.o.at("affected_count").n == 2 &&
          value.second.o.at("confirmed_count").n == 1 && value.second.o.at("failed_count").n == 1 &&
          value.second.o.at("unknown_count").n == 0);
    check(calls == std::vector<std::string>({"delete:/one:1", "delete:/two:2"}) &&
          registered.size() == 1 && registered[0].id == 2);
    check(!stored.document.rules[0].enabled && !stored.document.rules[1].enabled &&
          !stored.document.rules[2].enabled);
    check(module_hides.size() == 2 && module_hides.at("/one") == 77 &&
          module_hides.at("/two") == 88);
    const auto& items = value.second.o.at("items").a;
    check(items[0].o.at("id").s == first && items[0].o.at("outcome").s == "absent" &&
          items[0].o.at("membership_matches").b);
    check(items[1].o.at("id").s == second &&
          items[1].o.at("outcome").s == "mutation_failed_or_partial" &&
          items[1].o.at("errno").n == EIO);
    check(!items[1].o.at("membership_matches").b && items[1].o.at("runtime").o.at("id").s == "2");

    reset();
    readback_fail = true;
    value = run({"hide", "disable-group", "group", rev(1)});
    check(value.first == 1 && value.second.o.at("status").s == "partial" &&
          value.second.o.at("saved").b && registered.empty());
    check(value.second.o.at("confirmed_count").n == 0 && value.second.o.at("failed_count").n == 0 &&
          value.second.o.at("unknown_count").n == 2);
    for (const auto& item : value.second.o.at("items").a)
        check(item.o.at("outcome").s == "outcome_unknown" &&
              item.o.at("membership_matches").is_null() && item.o.at("runtime").is_null());
}
void test_dispatch_capture() {
    auto* previous_out = std::cout.rdbuf();
    auto* previous_err = std::cerr.rdbuf();
    JsonValue value;
    std::string error;
    const auto parse = [&](const std::string& bytes) {
        check(json::Parser(bytes, true).parse(value, error));
        check(std::cout.rdbuf() == previous_out && std::cerr.rdbuf() == previous_err);
        check(value.o.at("schema_version").n == 1 &&
              value.o.at("resource").s == "user_hide_dispatch");
    };
    parse(hide::manager_dispatch_json([] {
        std::cout << "{\"status\":\"partial\"}\n";
        std::cerr << "diagnostic\n";
        return 9;
    }));
    check(value.o.at("exit_code").n == 9 && value.o.at("stdout").s == "{\"status\":\"partial\"}\n");
    check(value.o.at("stderr").s == "diagnostic\n" && value.o.at("stdout_complete").b &&
          value.o.at("stderr_complete").b);
    parse(hide::manager_dispatch_json([] {
        std::cout << std::string(64 * 1024, '\1');
        std::cerr << std::string(64 * 1024 + 1, 'x');
        return 0;
    }));
    check(value.o.at("stdout").s.size() == 64 * 1024 && value.o.at("stdout_complete").b);
    check(value.o.at("stderr").s.size() == 64 * 1024 && !value.o.at("stderr_complete").b);
    parse(hide::manager_dispatch_json([] {
        std::cout << std::string(64 * 1024 + 1, 'x');
        return 0;
    }));
    check(value.o.at("stdout").s.size() == 64 * 1024 && !value.o.at("stdout_complete").b);
    parse(hide::manager_dispatch_json([]() -> int {
        std::cout << "incomplete";
        throw std::runtime_error("injected");
    }));
    check(value.o.at("exit_code").n == 1 && value.o.at("stdout").s == "incomplete" &&
          !value.o.at("stderr").s.empty());
    parse(hide::manager_dispatch_json([]() -> int { throw 7; }));
    check(value.o.at("exit_code").n == 1 && value.o.at("stdout").s.empty());
}
std::string imported_fields(const std::string& path, bool enabled,
                            const std::string& expected = "absent") {
    auto fields = JsonValue::object();
    fields.o["path"] = path;
    fields.o["enabled"] = enabled;
    fields.o["expect"] = expected;
    fields.o["name"] = "名称 ";
    fields.o["note"] = "line\n\r\t😀 ";
    fields.o["group"] = "--help";
    return stringify_json(fields);
}
void test_import_id() {
    using namespace fixture;
    const std::string imported(32, '4');
    reset();
    auto value = run({"hide", "import-id", imported, rev(1), imported_fields("/new\n ", false)});
    check(value.first == 0 && value.second.o.at("action").s == "import-id" &&
          value.second.o.at("affected_count").n == 1);
    check(stored.document.rules.size() == 4 && stored.document.rules.back().id == imported &&
          !stored.document.rules.back().enabled);
    check(stored.document.rules.back().path == "/new\n " &&
          stored.document.rules.back().note == "line\n\r\t😀 " && calls.empty() &&
          random_calls == 0);
    check(value.second.o.at("items").a.front().o.at("id").s == imported &&
          value.second.o.at("items").a.front().o.at("outcome").s == "absent");
    reset();
    value = run({"hide", "import-id", imported, rev(1), imported_fields("/new", true)});
    check(value.first == 0 && calls == std::vector<std::string>{"upsert:/new"} &&
          stored.document.rules.back().enabled);
    reset();
    module_hides = {{"/one", 77}};
    value =
        run({"hide", "import-id", first, rev(1), imported_fields("/one", false, "same_identity")});
    check(value.first == 0 && calls == std::vector<std::string>{"delete:/one:1"} &&
          module_hides.at("/one") == 77);
    check(stored.document.rules[0].id == first && stored.document.rules[0].name == "名称 " &&
          stored.document.rules[1].id == second && stored.document.rules.size() == 3);
    for (const auto& collision : std::vector<std::tuple<std::string, std::string, std::string>>{
             {first, "/one", "absent"},
             {imported, "/one", "absent"},
             {first, "/new", "absent"},
             {first, "/two", "same_identity"},
             {first, "/new", "same_identity"},
             {imported, "/one", "same_identity"},
             {imported, "/new", "same_identity"}}) {
        reset();
        value = run({"hide", "import-id", std::get<0>(collision), rev(1),
                     imported_fields(std::get<1>(collision), false, std::get<2>(collision))});
        check(value.first == 1 && writes == 0 && queries == 0 && calls.empty() &&
              value.second.o.at("reason").s == "definition_identity_conflict");
    }
    reset();
    value = run({"hide", "import-id", imported, rev(9), imported_fields("/new", false)});
    check(value.first == 1 && writes == 0 && queries == 0);
    for (int fault = 0; fault < 7; ++fault) {
        reset();
        if (fault == 0)
            available = false;
        if (fault == 1)
            managed = false;
        if (fault == 2)
            caps_ok = false;
        if (fault == 3)
            query_ok = false;
        if (fault == 4)
            lock_ok = false;
        if (fault == 5)
            save_result = hide::SaveResult::NotSaved;
        if (fault == 6)
            save_result = hide::SaveResult::Unknown;
        value = run({"hide", "import-id", imported, rev(1), imported_fields("/new", true)});
        check(value.first == 1 && calls.empty() && stored.document.rules.size() == 3);
    }
    reset();
    mutation_ok = false;
    value = run({"hide", "import-id", imported, rev(1), imported_fields("/new", true)});
    check(value.first == 1 && value.second.o.at("status").s == "partial" &&
          value.second.o.at("saved").b && stored.document.rules.size() == 4);
    reset();
    readback_fail = true;
    value = run({"hide", "import-id", imported, rev(1), imported_fields("/new", true)});
    check(value.first == 1 && value.second.o.at("unknown_count").n == 1);
    reset();
    stored.document.rules.clear();
    for (int i = 0; i < 4096; ++i) {
        std::ostringstream id;
        id << std::hex << i;
        const auto digits = id.str();
        stored.document.rules.push_back({std::string(32 - digits.size(), '0') + digits,
                                         "/" + std::to_string(i),
                                         false,
                                         {},
                                         {},
                                         {}});
    }
    value = run({"hide", "import-id", imported, rev(1), imported_fields("/new", false)});
    check(value.first == 1 && queries == 0 && writes == 0 && calls.empty());
    hide::Request request;
    std::string error;
    const auto valid = json::parse(imported_fields("/new", false));
    for (const auto& field : valid.o) {
        auto missing = valid;
        missing.o.erase(field.first);
        check(!hide::parse_request({"hide", "import-id", imported, rev(1), stringify_json(missing)},
                                   request, error));
    }
    for (const auto& key :
         std::vector<std::string>{"path", "enabled", "name", "note", "group", "expect"}) {
        auto bad = valid;
        bad.o[key] = JsonValue{};
        check(!hide::parse_request({"hide", "import-id", imported, rev(1), stringify_json(bad)},
                                   request, error));
    }
    auto bad = valid;
    bad.o["expect"] = "overwrite";
    check(!hide::parse_request({"hide", "import-id", imported, rev(1), stringify_json(bad)},
                               request, error));
    bad = valid;
    bad.o["extra"] = false;
    check(!hide::parse_request({"hide", "import-id", imported, rev(1), stringify_json(bad)},
                               request, error));
    check(!hide::parse_request(
        {"hide", "import-id", imported, rev(1), "{\"path\":\"/a\",\"path\":\"/b\"}"}, request,
        error));
}
void test_definition_export() {
    using namespace fixture;
    const auto page = [](const std::vector<std::string>& args, int expected = 0) {
        hide::Request request;
        std::string error;
        check(hide::parse_request(args, request, error));
        std::ostringstream output;
        auto* old = std::cout.rdbuf(output.rdbuf());
        const int code = hide::print_definitions(request);
        std::cout.rdbuf(old);
        check(code == expected && output.str().size() < 56 * 1024);
        JsonValue result;
        check(json::Parser(output.str(), true).parse(result, error));
        check(result.o.size() == 16 && !result.o.at("runtime_verified").b &&
              result.o.at("read_only").b);
        check(writes == 0 && queries == 0 && locks == 0 && calls.empty());
        return result;
    };
    reset();
    available = false;
    managed = false;
    query_ok = false;
    auto value = page({"hide", "definitions"});
    check(value.o.at("items").a.size() == 3 && value.o.at("items").a[0].o.size() == 6 &&
          value.o.at("complete").b && value.o.at("at_end").b);
    check(value.o.at("items").a[2].o.at("enabled").b == false &&
          value.o.at("resource").s == "user_hide_definition_export_page");
    value = page({"hide", "definitions", rev(1), "3"});
    check(value.o.at("items").a.empty() && value.o.at("at_end").b && !value.o.at("complete").b);
    reset();
    value = page({"hide", "definitions", rev(9), "0"}, 1);
    check(value.o.at("items").a.empty() && value.o.at("status").s == "stale_or_invalid_cursor");
    reset();
    value = page({"hide", "definitions", rev(1), "4"}, 1);
    check(!value.o.at("at_end").b);
    reset();
    read_ok = false;
    value = page({"hide", "definitions"}, 1);
    check(value.o.at("revision").is_null() && value.o.at("source_schema").is_null());
    for (int fault = 0; fault < 2; ++fault) {
        reset();
        change_read = fault == 0;
        change_boot = fault == 1;
        value = page({"hide", "definitions"}, 1);
        check(value.o.at("items").a.empty() && value.o.at("status").s == "changed_or_unavailable");
    }
    reset();
    stored.document.rules.clear();
    for (int i = 0; i < 65; ++i)
        stored.document.rules.push_back({first, "/" + std::to_string(i), false, {}, {}, {}});
    value = page({"hide", "definitions"});
    check(value.o.at("items").a.size() == 64 && value.o.at("next_cursor").n == 64 &&
          !value.o.at("at_end").b);
    value = page({"hide", "definitions", rev(1), "64"});
    check(value.o.at("items").a.size() == 1 && value.o.at("at_end").b);
    reset();
    stored.document.rules.assign(
        4, {first, "/" + std::string(4094, '\1'), false, std::string(128, '\2'),
            std::string(1024, '\3'), std::string(128, '\4')});
    value = page({"hide", "definitions"});
    check(value.o.at("items").a.size() == 1 && value.o.at("next_cursor").n == 1);
    for (int fault = 0; fault < 3; ++fault) {
        reset();
        available = false;
        query_ok = false;
        if (fault == 1)
            change_read = true;
        if (fault == 2)
            read_ok = false;
        std::ostringstream output;
        auto* old = std::cout.rdbuf(output.rdbuf());
        const auto code = hide::print_export();
        std::cout.rdbuf(old);
        check(code == (fault ? 1 : 0) && writes == 0 && queries == 0 && locks == 0 &&
              calls.empty());
        if (fault)
            check(output.str().empty());
        else {
            hide::Document parsed;
            std::string error;
            check(hide::parse_portable_document(output.str(), parsed, error) &&
                  parsed.rules.size() == 3 && parsed.rules[0].id == first);
        }
    }
}
}  // namespace
int main() {
    test_input();
    test_mutations();
    test_status();
    test_path_retry_and_owned_groups();
    test_dispatch_capture();
    test_import_id();
    test_definition_export();
    std::cout << fixture::checks << " user hide controller assertions passed\n";
}
