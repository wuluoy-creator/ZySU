#include "core/json.hpp"
#include "core/user_hide_store.hpp"
#include "uapi/sumh.h"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {
std::optional<std::string> contents;
int read_error = 0;
}  // namespace

#include "user_hide_digest_fixture.inc"

namespace sumhp {
std::filesystem::path runtime_data_dir() {
    return "/mock/runtime";
}
namespace checked {
struct Document {
    std::string status, bytes;
    int error = 0;
};
Document read_document(const std::filesystem::path&, std::size_t limit) {
    assert(limit == hide::HIDE_MAX_BYTES);
    if (contents)
        return {"valid", *contents, 0};
    return {read_error == ENOENT ? "missing" : "unreadable", {}, read_error ? read_error : EIO};
}
}  // namespace checked
#include "sumh_persistence_under_test.inc"
}  // namespace sumhp
#include "sumh_hide_read_under_test.inc"

int main() {
    std::vector<std::string> rules = {"/retained"};
    std::string error;
    for (const int code : {EACCES, EIO, 0}) {
        contents.reset();
        read_error = code;
        assert(!sumhp::load_user_hide_rules(rules, error));
        assert(!error.empty() && rules == std::vector<std::string>{"/retained"});
    }

    read_error = 0;
    for (const auto* invalid :
         {"", "[", "null", "{}", "[\"/good\", null]", "[\"/good\", \"relative\"]", "[\"\"]",
          "[\"/path\\u0000suffix\"]"}) {
        contents = invalid;
        assert(!sumhp::load_user_hide_rules(rules, error));
        assert(rules == std::vector<std::string>{"/retained"});
    }
    contents = "[\"/" + std::string(SUMH_USER_HIDE_PATH_MAX - 1, 'x') + "\"]";
    assert(!sumhp::load_user_hide_rules(rules, error));
    assert(rules == std::vector<std::string>{"/retained"});

    contents = "[\"/first\", \"/second\", \"/first\"]";
    assert(sumhp::load_user_hide_rules(rules, error));
    assert((rules == std::vector<std::string>{"/first", "/second"}));
    sumhp::hide::Document rich;
    rich.rules = {{std::string(32, 'a'), "/first", true, "name", "note", "group"},
                  {std::string(32, 'b'), "/disabled", false, "retained", "note", "group"}};
    contents = sumhp::hide::encode_document(rich);
    assert(sumhp::load_user_hide_rules(rules, error) &&
           rules == std::vector<std::string>{"/first"});
    contents = "{\"schema_version\":2,\"rules\":[]}";
    assert(!sumhp::load_user_hide_rules(rules, error) &&
           rules == std::vector<std::string>{"/first"});

    contents = "[]";
    assert(sumhp::load_user_hide_rules(rules, error));
    assert(rules.empty());

    rules = {"/old"};
    contents.reset();
    read_error = ENOENT;
    assert(sumhp::load_user_hide_rules(rules, error));
    assert(rules.empty());
    std::cout << "sumh_persistence_test: all checks passed\n";
}
