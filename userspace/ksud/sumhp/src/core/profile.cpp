#include "core/profile.hpp"
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <iostream>
#include <set>
#include "core/checked_document.hpp"
#include "core/runtime.hpp"
#include "core/state_ledger.hpp"
#include "userspace/common/utf8.hpp"

namespace sumhp::profiles {
namespace {
JsonValue result(const Request& request, const std::string& status, const std::string& reason) {
    auto out = JsonValue::object();
    out.o["schema_version"] = 1;
    out.o["kind"] = "sumh.profile.result";
    out.o["action"] = request.action;
    out.o["id"] = request.id;
    out.o["status"] = status;
    out.o["reason"] = reason;
    out.o["saved"] = false;
    out.o["applied"] = false;
    out.o["profile_active"] = false;
    out.o["active_configuration_saved"] = false;
    out.o["can_activate"] = false;
    out.o["runtime_evaluated"] = false;
    out.o["global_atomic"] = false;
    return out;
}
bool exact_fields(const JsonValue& object, std::initializer_list<const char*> keys) {
    if (!object.is_object() || object.o.size() != keys.size())
        return false;
    return std::all_of(keys.begin(), keys.end(), [&](const char* key) { return object.find(key); });
}
bool revision_valid(const JsonValue& value) {
    return value.is_string() && value.s.size() == 74 && value.s.compare(0, 10, "sha256-v1:") == 0 &&
           value.s.find_first_not_of("0123456789abcdef", 10) == std::string::npos;
}
bool text_valid(const JsonValue& value, std::size_t limit, bool nonempty = false) {
    return value.is_string() && (!nonempty || !value.s.empty()) && value.s.size() <= limit &&
           value.s.find('\0') == std::string::npos && ksud::text::valid_utf8(value.s);
}
bool parse_request_input(const Request& request, const std::string& input, JsonValue& value,
                         std::string& error) {
    if (input.size() > PROFILE_INPUT_MAX_BYTES) {
        error = "input_capacity";
        return false;
    }
    if (request.action == "import-preview")
        return parse_profile(input, value, error);
    if (request.input_path.empty()) {
        error.clear();
        return true;
    }
    JsonValue parsed;
    if (!json::Parser(input, true).parse(parsed, error))
        return false;
    bool fields = false;
    if (request.action == "capture")
        fields = exact_fields(parsed, {"schema_version", "id", "bank_revision", "name", "note"});
    else if (request.action == "import")
        fields = exact_fields(parsed, {"schema_version", "id", "bank_revision", "profile"});
    else if (request.action == "clone")
        fields = exact_fields(parsed, {"schema_version", "id", "source_id", "bank_revision",
                                       "profile_revision", "name", "note"});
    else if (request.action == "rename")
        fields = exact_fields(
            parsed, {"schema_version", "id", "bank_revision", "profile_revision", "name", "note"});
    else if (request.action == "remove")
        fields =
            exact_fields(parsed, {"schema_version", "id", "bank_revision", "profile_revision"});
    if (!fields || !parsed.at("schema_version").is_number() || parsed.at("schema_version").n != 1 ||
        !parsed.at("id").is_string() || !valid_profile_id(parsed.at("id").s) ||
        !revision_valid(parsed.at("bank_revision"))) {
        error = "invalid_profile_request";
        return false;
    }
    if ((request.action == "clone" &&
         (!parsed.at("source_id").is_string() || !valid_profile_id(parsed.at("source_id").s))) ||
        ((request.action == "clone" || request.action == "rename" || request.action == "remove") &&
         !revision_valid(parsed.at("profile_revision"))) ||
        ((request.action == "capture" || request.action == "clone" || request.action == "rename") &&
         (!text_valid(parsed.at("name"), 128, true) || !text_valid(parsed.at("note"), 1024)))) {
        error = "invalid_profile_metadata_or_revision";
        return false;
    }
    if (request.action == "import") {
        JsonValue profile;
        if (!parse_profile(stringify_json(parsed.at("profile")), profile, error))
            return false;
        parsed.o["profile"] = std::move(profile);
    }
    value = std::move(parsed);
    error.clear();
    return true;
}
using Documents = std::array<checked::Document, 4>;
Documents read_sources() {
    return {checked::read_document(runtime_config_file()),
            checked::read_document(runtime_data_dir() / "module_mode.json"),
            checked::read_document(runtime_data_dir() / "module_rules.json"),
            checked::read_document(runtime_data_dir() / "user_hide_rules.json")};
}
bool capture_current(const std::string& name, const std::string& note, JsonValue& profile,
                     JsonValue& observation, std::string& error) {
    const auto before = read_sources();
    observation = JsonValue::object();
    observation.o["capture_consistency"] = "double_read_observation";
    observation.o["global_atomic"] = false;
    observation.o["runtime_verified"] = false;
    observation.o["source_files_backed_up"] = false;
    observation.o["settings_defaults_materialized"] = true;
    observation.o["sources"] = JsonValue::object();
    static const char* const names[] = {"config", "module_modes", "module_rules", "user_hide"};
    std::array<std::string, 4> bytes;
    for (std::size_t i = 0; i < before.size(); ++i) {
        auto row = JsonValue::object();
        row.o["status"] = before[i].status;
        row.o["errno"] = before[i].error;
        const auto revision =
            state::revision_for_bytes(std::string("sumh.profile.source.v1:") + names[i] + ":" +
                                      before[i].status + ":" + before[i].bytes);
        row.o["revision"] = revision;
        observation.o["sources"].o[names[i]] = std::move(row);
        if (before[i].status != "valid" && before[i].status != "missing") {
            error = std::string("source_unreadable:") + names[i];
            return false;
        }
        if (!revision_valid(JsonValue(revision))) {
            error = "source_revision_unavailable";
            return false;
        }
        if (before[i].status == "missing")
            bytes[i] = i == 3 ? "[]" : "{}";
        else
            bytes[i] = before[i].bytes;
    }
    JsonValue captured;
    if (!capture_profile(bytes[0], bytes[1], bytes[2], bytes[3], name, note, captured, error))
        return false;
    if (before != read_sources()) {
        observation.o["stable"] = false;
        error = "source_changed";
        return false;
    }
    observation.o["stable"] = true;
    profile = std::move(captured);
    return true;
}
JsonValue describe(const std::string& id, const JsonValue& profile) {
    auto out = JsonValue::object();
    out.o["id"] = id;
    out.o["name"] = profile.at("name");
    out.o["note"] = profile.at("note");
    out.o["profile_revision"] = profile_revision(profile);
    out.o["profile_active"] = false;
    return out;
}
JsonValue execute_local(const Request& request, JsonValue input) {
    std::string error;
    if (request.action == "capture" || request.action == "import" || request.action == "clone" ||
        request.action == "rename" || request.action == "remove") {
        JsonValue observation;
        JsonValue captured;
        if (request.action == "capture") {
            if (!capture_current(input.at("name").s, input.at("note").s, captured, observation,
                                 error)) {
                auto out = result(request, "not_saved", "capture_failed");
                out.o["id"] = input.at("id");
                out.o["detail"] = error;
                out.o["observation"] = observation;
                return out;
            }
            input.o.erase("name");
            input.o.erase("note");
            input.o["profile"] = std::move(captured);
        }
        input.o["action"] =
            (request.action == "capture" || request.action == "import") ? "create" : request.action;
        auto out = mutate_bank(input);
        out.o["requested_action"] = request.action;
        if (request.action == "capture")
            out.o["observation"] = std::move(observation);
        return out;
    }
    Bank bank;
    if (request.action != "import-preview" && !read_bank(bank, error)) {
        auto out = result(request, "error", "bank_unavailable");
        out.o["detail"] = error;
        return out;
    }
    auto out = result(request, "complete", "inactive_profile");
    if (request.action != "import-preview")
        out.o["bank_revision"] = bank.revision;
    if (request.manager && !request.expected_bank_revision.empty() &&
        request.expected_bank_revision != bank.revision) {
        out.o["status"] = "stale";
        out.o["reason"] = "bank_revision_conflict";
        return out;
    }
    if (request.action == "list") {
        auto rows = JsonValue::array();
        for (const auto& [id, profile] : bank.profiles.o) {
            auto row = describe(id, profile);
            if (!revision_valid(row.at("profile_revision"))) {
                out.o["status"] = "error";
                out.o["reason"] = "profile_revision_unavailable";
                return out;
            }
            rows.a.push_back(std::move(row));
        }
        out.o["profiles"] = std::move(rows);
        out.o["missing"] = bank.missing;
        return out;
    }
    JsonValue target;
    if (request.action == "import-preview")
        target = std::move(input);
    else {
        const auto* found = bank.profiles.find(request.id);
        if (!found) {
            out.o["status"] = "error";
            out.o["reason"] = "profile_not_found";
            return out;
        }
        target = *found;
        out.o["profile_revision"] = profile_revision(target);
        if (!revision_valid(out.at("profile_revision"))) {
            out.o["status"] = "error";
            out.o["reason"] = "profile_revision_unavailable";
            return out;
        }
        if (request.manager && request.expected_profile_revision != out.at("profile_revision").s) {
            out.o["status"] = "stale";
            out.o["reason"] = "profile_revision_conflict";
            return out;
        }
    }
    if (request.action == "export" && !request.manager)
        return target;
    if (request.action == "show" || request.action == "export") {
        out.o["profile"] = target;
        return out;
    }
    JsonValue current;
    JsonValue observation;
    if (!capture_current("Current saved configuration", "", current, observation, error)) {
        out.o["status"] = "error";
        out.o["reason"] = "capture_failed";
        out.o["detail"] = error;
        out.o["observation"] = std::move(observation);
        return out;
    }
    if (request.action == "preview") {
        Bank after;
        if (!read_bank(after, error) || bank.revision != after.revision) {
            out.o["status"] = "error";
            out.o["reason"] = "bank_changed";
            return out;
        }
    }
    out.o["configuration_diff_only"] = true;
    out.o["observation"] = std::move(observation);
    out.o["diff"] = diff_profiles(current, target, request.scopes);
    auto selected = JsonValue::array();
    for (const auto& scope : request.scopes)
        selected.a.emplace_back(scope);
    out.o["selected_resources"] = std::move(selected);
    out.o["reason"] = "activation_not_implemented";
    out.o["module_and_capability_applicability"] = "unknown_not_evaluated";
    out.o["runtime_ownership_and_generation"] = "not_evaluated";
    out.o["module_source_contents"] = "not_captured";
    return out;
}
}  // namespace

// Exact standalone route only: switching this process avoids a cancellable
// query worker killing a mutation after rename. No daemon/recovery is invoked.
JsonValue execute(const Request& request, const std::string& input) {
    JsonValue parsed;
    std::string error;
    if (!parse_request_input(request, input, parsed, error)) {
        auto out = result(request, "not_saved", "invalid_input");
        out.o["detail"] = error;
        return out;
    }
    class Fd {
        int value;

    public:
        explicit Fd(int descriptor) : value(descriptor) {}
        Fd(const Fd&) = delete;
        Fd& operator=(const Fd&) = delete;
        Fd(Fd&&) = delete;
        Fd& operator=(Fd&&) = delete;
        [[nodiscard]] int get() const { return value; }
        ~Fd() {
            if (value >= 0)
                ::close(value);
        }
    };
    const Fd original{::open("/proc/self/ns/mnt", O_RDONLY | O_CLOEXEC)};
    const Fd target{::open("/proc/1/ns/mnt", O_RDONLY | O_CLOEXEC)};
    auto fail = [&](const char* reason) {
        auto out = result(request, "not_saved", reason);
        out.o["errno"] = errno ? errno : EIO;
        return out;
    };
    if (original.get() < 0 || target.get() < 0)
        return fail("namespace_unavailable");
    struct stat own{};
    struct stat init{};
    if (::fstat(original.get(), &own) != 0 || ::fstat(target.get(), &init) != 0)
        return fail("namespace_identity_unknown");
    const bool changed = own.st_dev != init.st_dev || own.st_ino != init.st_ino;
    if (changed && ::setns(target.get(), CLONE_NEWNS) != 0)
        return fail("enter_init_namespace_failed");
    class Restore {
        int fd;
        bool needed;

    public:
        Restore(int descriptor, bool should_restore) : fd(descriptor), needed(should_restore) {}
        Restore(const Restore&) = delete;
        Restore& operator=(const Restore&) = delete;
        Restore(Restore&&) = delete;
        Restore& operator=(Restore&&) = delete;
        void dismiss() { needed = false; }
        ~Restore() {
            if (needed)
                (void)::setns(fd, CLONE_NEWNS);
        }
    } restore{original.get(), changed};
    auto out = execute_local(request, std::move(parsed));
    if (changed && ::setns(original.get(), CLONE_NEWNS) != 0) {
        const auto code = errno ? errno : EIO;
        if (request.action == "export" && !request.manager)
            out = result(request, "error", "namespace_restore_failed");
        out.o["namespace_restore_failed"] = true;
        out.o["namespace_restore_errno"] = code;
    } else
        restore.dismiss();
    return out;
}

std::string manager_dispatch(const std::string& input) {
    Request request;
    request.manager = true;
    auto reject = [&](const std::string& detail) {
        auto out = result(request, "not_saved", "invalid_input");
        out.o["detail"] = detail;
        return stringify_json(out);
    };
    JsonValue value;
    std::string error;
    if (input.empty() || input.size() > PROFILE_INPUT_MAX_BYTES || !ksud::text::valid_utf8(input) ||
        !json::Parser(input, true).parse(value, error) || !value.is_object() ||
        !value.at("schema_version").is_number() || value.at("schema_version").n != 1 ||
        !value.at("action").is_string())
        return reject("invalid_manager_document");
    request.action = value.at("action").s;
    const auto& action = request.action;
    const bool selected = action == "show" || action == "export" || action == "preview";
    const bool mutation = action == "capture" || action == "import" || action == "clone" ||
                          action == "rename" || action == "remove";
    bool fields = false;
    if (action == "capabilities" || action == "list")
        fields = exact_fields(value, {"schema_version", "action"});
    else if (action == "preview")
        fields = exact_fields(value, {"schema_version", "action", "id", "bank_revision",
                                      "profile_revision", "resources"});
    else if (selected)
        fields = exact_fields(
            value, {"schema_version", "action", "id", "bank_revision", "profile_revision"});
    else if (action == "import-preview")
        fields = exact_fields(value, {"schema_version", "action", "profile", "resources"});
    else if (mutation)
        fields = exact_fields(value, {"schema_version", "action", "input"});
    if (!fields)
        return reject("invalid_manager_fields_or_action");
    if (selected) {
        if (!value.at("id").is_string() || !valid_profile_id(value.at("id").s) ||
            !revision_valid(value.at("bank_revision")) ||
            !revision_valid(value.at("profile_revision")))
            return reject("invalid_manager_selection");
        request.id = value.at("id").s;
        request.expected_bank_revision = value.at("bank_revision").s;
        request.expected_profile_revision = value.at("profile_revision").s;
    }
    if (action == "preview" || action == "import-preview") {
        const auto& resources = value.at("resources");
        std::set<std::string> unique;
        if (!resources.is_array() || resources.a.empty() || resources.a.size() > 4)
            return reject("invalid_manager_resources");
        for (const auto& resource : resources.a) {
            if (!resource.is_string() ||
                (resource.s != "settings" && resource.s != "modules" && resource.s != "user_hide" &&
                 resource.s != "maps") ||
                !unique.insert(resource.s).second)
                return reject("invalid_manager_resources");
            request.scopes.push_back(resource.s);
        }
    }
    if (action == "capabilities") {
        auto out = JsonValue::object();
        out.o["schema_version"] = 1;
        out.o["kind"] = "sumh.profile.manager.capabilities";
        out.o["manager_schema"] = 1;
        out.o["actions"] = JsonValue::object();
        for (const auto* supported : {"list", "show", "export", "preview", "import-preview",
                                      "capture", "import", "clone", "rename", "remove"})
            out.o["actions"].o[supported] = true;
        auto limits = JsonValue::object();
        limits.o["profile_bytes"] = static_cast<int>(PROFILE_MAX_BYTES);
        limits.o["input_bytes"] = static_cast<int>(PROFILE_INPUT_MAX_BYTES);
        limits.o["response_bytes"] = static_cast<int>(PROFILE_RESPONSE_MAX_BYTES);
        limits.o["bank_profiles"] = static_cast<int>(BANK_MAX_PROFILES);
        limits.o["bank_bytes"] = static_cast<int>(BANK_MAX_BYTES);
        out.o["limits"] = std::move(limits);
        out.o["activate"] = false;
        out.o["restore"] = false;
        out.o["operation_journal"] = false;
        return stringify_json(out);
    }
    std::string payload;
    if (mutation || action == "import-preview") {
        request.input_path = "-";  // In-memory execute input; never opened as a path.
        // Bind even pre-store failures to the reviewed mutation target. Only a
        // syntactically valid ID is echoed; the complete existing validator
        // still decides whether this input can reach namespace/store work.
        if (mutation && value.at("input").at("id").is_string() &&
            valid_profile_id(value.at("input").at("id").s))
            request.id = value.at("input").at("id").s;
        payload = stringify_json(value.at(mutation ? "input" : "profile"));
    }
    auto out = execute(request, payload);
    auto bytes = stringify_json(out);
    if (bytes.size() > PROFILE_RESPONSE_MAX_BYTES)
        return stringify_json(result(request, "error", "response_capacity"));
    return bytes;
}

int command(const std::vector<std::string>& args) {
    Request request;
    std::string error;
    std::string input;
    if (!parse_arguments(args, request, error)) {
        auto out = result(request, "not_saved", error);
        std::cout << stringify_json(out) << '\n';
        std::cerr << "usage: ksud sumhp profile "
                     "list|show|export|preview|import-preview|capture|import|clone|rename|remove "
                     "(exact local route)\n";
        return 2;
    }
    if (!request.input_path.empty() && !read_profile_input(request.input_path, input, error)) {
        std::cout << stringify_json(result(request, "not_saved", error)) << '\n';
        return 1;
    }
    auto out = execute(request, input);
    auto bytes = stringify_json(out);
    if (bytes.size() > PROFILE_RESPONSE_MAX_BYTES) {
        // Never truncate a portable document/diff. Mutations return compact
        // receipts, so this limit cannot convert a successful write to failure.
        out = result(request, "error", "response_capacity");
        bytes = stringify_json(out);
    }
    std::cout << bytes << '\n';
    if (!std::cout.good() || out.at("namespace_restore_failed").bool_or(false))
        return 1;
    if (request.action == "export")
        return out.at("kind").s == "sumh.profile" ? 0 : 1;
    const auto status = out.at("status").s;
    return status == "complete" || status == "saved" || status == "unchanged" ? 0 : 1;
}
}  // namespace sumhp::profiles
