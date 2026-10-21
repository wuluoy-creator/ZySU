#include "core/user_hide.hpp"
#include <sys/random.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <map>
#include "core/capture_buffer.hpp"
#include "core/runtime.hpp"
#include "sumhp/sumh_client.hpp"
#include "uapi/sumh.h"

namespace sumhp::hide {
std::string manager_dispatch_json(const std::function<int()>& dispatch) {
    CaptureBuffer output(64UL * 1024);
    CaptureBuffer errors(64UL * 1024);
    auto* old_out = std::cout.rdbuf(&output);
    auto* old_err = std::cerr.rdbuf(&errors);
    class RestoreStreams {
    public:
        RestoreStreams(std::streambuf* output, std::streambuf* errors) : out(output), err(errors) {}
        RestoreStreams(const RestoreStreams&) = delete;
        RestoreStreams& operator=(const RestoreStreams&) = delete;
        RestoreStreams(RestoreStreams&&) = delete;
        RestoreStreams& operator=(RestoreStreams&&) = delete;
        ~RestoreStreams() {
            std::cout.rdbuf(out);
            std::cerr.rdbuf(err);
        }

    private:
        std::streambuf* out;
        std::streambuf* err;
    };
    const RestoreStreams restore{old_out, old_err};
    int code = 1;
    try {
        code = dispatch();
    } catch (...) {
        std::cerr << "User hide dispatch failed; query before retrying\n";
    }
    auto result = JsonValue::object();
    result.o["schema_version"] = 1;
    result.o["resource"] = "user_hide_dispatch";
    result.o["exit_code"] = code;
    result.o["stdout"] = output.str();
    result.o["stderr"] = errors.str();
    result.o["stdout_complete"] = output.complete();
    result.o["stderr_complete"] = errors.complete();
    return stringify_json(result);
}
namespace {
using Registered = std::vector<sumh::UserHideRule>;
JsonValue identity(const sumh::UserHideRule& rule) {
    auto value = JsonValue::object();
    value.o["id"] = std::to_string(rule.id);
    value.o["generation"] = std::to_string(rule.generation);
    value.o["management"] = static_cast<int>(rule.management);
    value.o["binding"] = static_cast<int>(rule.binding);
    value.o["errno"] = rule.error;
    return value;
}
std::map<std::string, sumh::UserHideRule> index_rules(const Registered& rules) {
    std::map<std::string, sumh::UserHideRule> result;
    for (const auto& rule : rules)
        result.emplace(rule.path, rule);
    return result;
}
void metadata(Definition& definition, const JsonValue& fields) {
    if (const auto* value = fields.find("name"))
        definition.name = value->s;
    if (const auto* value = fields.find("note"))
        definition.note = value->s;
    if (const auto* value = fields.find("group"))
        definition.group = value->s;
}
bool new_id(std::string& id) {
    unsigned char bytes[16];
    std::size_t used = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (used < sizeof(bytes)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            errno = ETIMEDOUT;
            return false;
        }
        const auto count = ::getrandom(bytes + used, sizeof(bytes) - used, GRND_NONBLOCK);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            if (!count)
                errno = EIO;
            return false;
        }
        used += static_cast<std::size_t>(count);
    }
    static constexpr char hex[] = "0123456789abcdef";
    id.clear();
    for (const auto c : bytes) {
        id += hex[c >> 4];
        id += hex[c & 15];
    }
    return true;
}
JsonValue receipt(const std::string& action) {
    auto result = JsonValue::object();
    result.o["schema_version"] = 1;
    result.o["resource"] = "user_hide_mutation";
    result.o["action"] = action;
    result.o["status"] = "rejected";
    result.o["reason"] = "";
    result.o["error"] = "";
    result.o["saved"] = false;
    result.o["revision"] = JsonValue{};
    result.o["previous_revision"] = JsonValue{};
    result.o["boot_id"] = runtime_boot_id();
    result.o["runtime_verified"] = false;
    result.o["verification_scope"] = "registered_membership_not_observer_visibility";
    result.o["affected_count"] = 0;
    result.o["confirmed_count"] = 0;
    result.o["failed_count"] = 0;
    result.o["unknown_count"] = 0;
    result.o["items_complete"] = true;
    result.o["items"] = JsonValue::array();
    return result;
}
int emit(const JsonValue& value, int code) {
    std::cout << stringify_json(value) << '\n';
    return std::cout.good() ? code : 1;
}
bool save(Transaction& transaction, Document& document, JsonValue& result) {
    std::string error;
    const auto saved = transaction.save(document, error);
    result.o["error"] = error;
    if (saved == SaveResult::NotSaved || saved == SaveResult::Unknown) {
        result.o["status"] = saved == SaveResult::Unknown ? "outcome_unknown" : "not_saved";
        result.o["reason"] = "definition_commit_unconfirmed";
        result.o["saved"] = saved == SaveResult::Unknown ? JsonValue{} : JsonValue(false);
        return false;
    }
    result.o["saved"] = true;
    result.o["revision"] = transaction.stored().revision;
    result.o["status"] = saved == SaveResult::Unchanged ? "unchanged" : "saved";
    return true;
}
int mutate(const Request& request, bool legacy = false, const std::string& path = {}) {
    auto result = receipt(request.action);
    Transaction transaction;
    std::string error;
    if (!transaction.begin(error)) {
        result.o["reason"] = "store_unavailable";
        result.o["error"] = error;
        return emit(result, 1);
    }
    result.o["previous_revision"] = transaction.stored().revision;
    if (!legacy && request.revision != transaction.stored().revision) {
        result.o["reason"] = "revision_conflict";
        return emit(result, 1);
    }
    auto document = transaction.stored().document;
    std::vector<Definition> selected;
    const bool create = request.action == "create" || request.action == "add";
    const bool remove = request.action == "remove-id" || request.action == "remove";
    const bool group = request.action == "enable-group" || request.action == "disable-group";
    if (request.action == "import-id") {
        const auto& target = request.fields.o.at("path").s;
        const auto by_id = std::find_if(document.rules.begin(), document.rules.end(),
                                        [&](const auto& d) { return d.id == request.selector; });
        const auto by_path = std::find_if(document.rules.begin(), document.rules.end(),
                                          [&](const auto& d) { return d.path == target; });
        const bool absent = request.fields.o.at("expect").s == "absent";
        if (absent ? by_id != document.rules.end() || by_path != document.rules.end()
                   : by_id == document.rules.end() || by_path != by_id) {
            result.o["reason"] = "definition_identity_conflict";
            return emit(result, 1);
        }
        Definition imported;
        imported.id = request.selector;
        imported.path = target;
        imported.enabled = request.fields.o.at("enabled").b;
        metadata(imported, request.fields);
        if (absent)
            document.rules.push_back(imported);
        else
            *by_id = imported;
        selected.push_back(std::move(imported));
        Document validated;
        if (!parse_document(encode_document(document), validated, error)) {
            result.o["reason"] = "definition_candidate_invalid";
            result.o["error"] = error;
            return emit(result, 1);
        }
    } else if (create) {
        const auto target = legacy ? path : request.fields.o.at("path").s;
        const auto found = std::find_if(document.rules.begin(), document.rules.end(),
                                        [&](const auto& d) { return d.path == target; });
        if (found != document.rules.end()) {
            if (!legacy || !found->enabled) {
                result.o["reason"] =
                    found->enabled ? "path_already_defined" : "definition_disabled_use_enable";
                return emit(result, 1);
            }
            selected.push_back(*found);
        } else {
            Definition added;
            added.path = target;
            if (!new_id(added.id) || std::any_of(document.rules.begin(), document.rules.end(),
                                                 [&](const auto& d) { return d.id == added.id; })) {
                result.o["reason"] = "definition_id_unavailable_or_collision";
                return emit(result, 1);
            }
            if (!legacy) {
                metadata(added, request.fields);
                if (const auto* enabled = request.fields.find("enabled"))
                    added.enabled = enabled->b;
            }
            document.rules.push_back(added);
            selected.push_back(added);
        }
    } else {
        for (auto& definition : document.rules) {
            bool differs;
            if (legacy)
                differs = definition.path != path;
            else
                differs = group ? definition.group != request.selector
                                : definition.id != request.selector;
            if (differs)
                continue;
            if (request.action == "annotate")
                metadata(definition, request.fields);
            if (request.action == "enable" || request.action == "enable-group")
                definition.enabled = true;
            if (request.action == "disable" || request.action == "disable-group")
                definition.enabled = false;
            selected.push_back(definition);
        }
        if (selected.empty() && (!legacy || request.action == "retry-path")) {
            result.o["reason"] = "definition_or_group_not_found";
            return emit(result, 1);
        }
        if (selected.empty() && legacy && remove)
            selected.push_back({"", path, false, {}, {}, {}});
        if (remove)
            document.rules.erase(
                std::remove_if(document.rules.begin(), document.rules.end(),
                               [&](const auto& d) {
                                   return std::any_of(selected.begin(), selected.end(),
                                                      [&](const auto& s) { return s.id == d.id; });
                               }),
                document.rules.end());
        if ((request.action == "retry" || request.action == "retry-path") &&
            !selected.front().enabled) {
            result.o["reason"] = "definition_disabled";
            return emit(result, 1);
        }
    }
    result.o["affected_count"] = static_cast<int>(selected.size());
    if (request.action == "annotate") {
        if (!save(transaction, document, result))
            return emit(result, 1);
        result.o["reason"] = "metadata_only";
        return emit(result, 0);
    }
    bool managed = false;
    const bool available = sumh::is_available();
    if ((available && !sumh::managed_hide_mode(managed)) || (!legacy && (!available || !managed))) {
        result.o["reason"] = "managed_ownership_unavailable";
        return emit(result, 1);
    }
    Registered before;
    if (managed && !sumh::user_hide_rules(before)) {
        result.o["reason"] = "runtime_query_failed_before_save";
        return emit(result, 1);
    }
    const auto previous = index_rules(before);
    if (!save(transaction, document, result))
        return emit(result, 1);
    // Old add/remove keep their old-kernel behavior, but never recreate a
    // disabled definition or drop its metadata through the path-only writer.
    if (!managed) {
        bool ok = true;
        if (available) {
            if (remove)
                ok = sumh::delete_rule(path);
            else
                ok = sumh::hide_path(path);
        }
        result.o["status"] = ok ? "saved_runtime_unverified" : "partial";
        result.o["reason"] = "legacy_runtime_has_no_owned_readback";
        return emit(result, ok ? 0 : 1);
    }
    std::vector<int> errors;
    errors.reserve(selected.size());
    for (const auto& definition : selected) {
        const auto existing = previous.find(definition.path);
        errno = 0;
        bool ok = true;
        if (remove || !definition.enabled) {
            if (existing != previous.end())
                ok = sumh::delete_user_hide(definition.path, existing->second.id);
        } else {
            // Existing path add historically also requests another binding
            // attempt. Preserve it without re-enabling a disabled definition.
            if (existing == previous.end() || request.action == "add")
                ok = sumh::upsert_user_hide(definition.path);
            if (ok && (request.action == "retry" || request.action == "retry-path"))
                ok = sumh::retry_user_hide(definition.path);
        }
        errors.push_back(ok ? 0 : errno ? errno : EIO);
    }
    Registered after;
    const bool queried = sumh::user_hide_rules(after);
    const auto current = index_rules(after);
    int confirmed = 0;
    int failed = 0;
    int unknown = 0;
    for (std::size_t i = 0; i < selected.size(); ++i) {
        const auto& definition = selected[i];
        const auto found = current.find(definition.path);
        const bool desired = definition.enabled && !remove;
        const bool matches = queried && (found != current.end()) == desired;
        std::string outcome;
        if (errors[i]) {
            outcome = "mutation_failed_or_partial";
            ++failed;
        } else if (!queried) {
            outcome = "outcome_unknown";
            ++unknown;
        } else if (!matches) {
            outcome = "runtime_differs";
            ++failed;
        } else {
            ++confirmed;
            if (!desired)
                outcome = "absent";
            else if (found->second.binding == SUMH_USER_HIDE_BOUND)
                outcome = "registered_bound";
            else
                outcome = found->second.binding == SUMH_USER_HIDE_ERROR ? "registered_binding_error"
                                                                        : "registered_pending";
        }
        if (i < HIDE_RESULT_ITEMS) {
            auto item = JsonValue::object();
            item.o["id"] = definition.id;
            item.o["outcome"] = outcome;
            item.o["errno"] = errors[i];
            item.o["membership_matches"] = queried ? JsonValue(matches) : JsonValue{};
            item.o["runtime"] =
                queried && found != current.end() ? identity(found->second) : JsonValue{};
            result.o["items"].a.push_back(std::move(item));
        }
    }
    result.o["items_complete"] = selected.size() <= HIDE_RESULT_ITEMS;
    result.o["confirmed_count"] = confirmed;
    result.o["failed_count"] = failed;
    result.o["unknown_count"] = unknown;
    result.o["status"] = (failed || unknown) ? "partial" : "applied";
    result.o["reason"] = "registration_only_binding_and_observers_separate";
    return emit(result, (failed || unknown) ? 1 : 0);
}
}  // namespace

state::OperationResponse query_status(const Request& request) {
    Stored stored;
    std::string error;
    auto root = JsonValue::object();
    root.o["schema_version"] = 1;
    root.o["resource"] = "user_hide_definitions";
    root.o["read_only"] = true;
    root.o["runtime_consistency"] = "per_rule_sampled";
    root.o["runtime_verified"] = false;
    root.o["scope"] = "caller_filesystem_definitions_and_kernel_registered_membership";
    root.o["boot_id"] = runtime_boot_id();
    root.o["status"] = "unavailable";
    root.o["error"] = "";
    root.o["revision"] = JsonValue{};
    root.o["items"] = JsonValue::array();
    root.o["complete"] = false;
    if (!read_store(stored, error)) {
        root.o["error"] = error;
        return {1, std::move(root)};
    }
    root.o["revision"] = stored.revision;
    if ((!request.revision.empty() && request.revision != stored.revision) ||
        request.cursor > stored.document.rules.size()) {
        root.o["status"] = "stale_or_invalid_cursor";
        return {1, std::move(root)};
    }
    bool managed = false;
    const bool available = sumh::is_available();
    const bool known = available && sumh::managed_hide_mode(managed);
    Registered rules;
    const bool queried = known && managed && sumh::user_hide_rules(rules);
    const auto registered = index_rules(rules);
    root.o["managed_ownership"] = known ? JsonValue(managed) : JsonValue{};
    if (queried)
        root.o["runtime_query"] = "observed";
    else
        root.o["runtime_query"] = known && !managed ? "unsupported" : "unavailable";
    root.o["source_schema"] = stored.document.source_schema;
    root.o["count"] = static_cast<int>(stored.document.rules.size());
    root.o["cursor"] = static_cast<int>(request.cursor);
    const auto limit = std::min(stored.document.rules.size(), request.cursor + HIDE_STATUS_PAGE);
    auto end = request.cursor;
    for (std::size_t i = request.cursor; i < limit; ++i) {
        const auto& definition = stored.document.rules[i];
        const auto found = registered.find(definition.path);
        auto item = JsonValue::object();
        item.o["id"] = definition.id;
        item.o["path"] = definition.path;
        item.o["enabled"] = definition.enabled;
        item.o["name"] = definition.name;
        item.o["note"] = definition.note;
        item.o["group"] = definition.group;
        item.o["registered"] = queried ? JsonValue(found != registered.end()) : JsonValue{};
        item.o["runtime"] =
            queried && found != registered.end() ? identity(found->second) : JsonValue{};
        if (!queried)
            item.o["state"] = "unknown";
        else if (!definition.enabled)
            item.o["state"] = found == registered.end() ? "disabled" : "disabled_pending_removal";
        else if (found == registered.end())
            item.o["state"] = "unregistered";
        else if (found->second.management == SUMH_USER_HIDE_SUSPENDED)
            item.o["state"] = "globally_suspended";
        else if (found->second.binding == SUMH_USER_HIDE_BOUND)
            item.o["state"] = "bound";
        else
            item.o["state"] =
                found->second.binding == SUMH_USER_HIDE_ERROR ? "binding_error" : "pending";
        root.o["items"].a.push_back(std::move(item));
        // Keep the whole escaped page below the existing 56 KiB resource
        // budget, including worst-case control characters in saved paths.
        if (stringify_json(root).size() > 54UL * 1024) {
            root.o["items"].a.pop_back();
            break;
        }
        end = i + 1;
    }
    Stored after;
    if (!read_store(after, error) || after.revision != stored.revision) {
        root.o["status"] = "changed_or_unavailable";
        root.o["error"] = error;
        root.o["items"] = JsonValue::array();
        return {1, std::move(root)};
    }
    root.o["status"] = "observed";
    root.o["next_cursor"] = static_cast<int>(end);
    root.o["at_end"] = end == stored.document.rules.size();
    root.o["complete"] = request.cursor == 0 && end == stored.document.rules.size();
    return {0, std::move(root)};
}
int print_status(const Request& request) {
    const auto result = query_status(request);
    return emit(result.value, result.exit_code);
}

int print_definitions(const Request& request) {
    Stored stored;
    std::string error;
    auto root = JsonValue::object();
    root.o["schema_version"] = 1;
    root.o["resource"] = "user_hide_definition_export_page";
    root.o["read_only"] = true;
    root.o["runtime_verified"] = false;
    root.o["scope"] = "persistent_user_hide_definitions";
    const auto boot = runtime_boot_id();
    root.o["boot_id"] = boot;
    root.o["status"] = "unavailable";
    root.o["error"] = "";
    root.o["revision"] = JsonValue{};
    root.o["source_schema"] = JsonValue{};
    root.o["count"] = 0;
    root.o["cursor"] = static_cast<int>(request.cursor);
    root.o["next_cursor"] = static_cast<int>(request.cursor);
    root.o["at_end"] = false;
    root.o["complete"] = false;
    root.o["items"] = JsonValue::array();
    if (!read_store(stored, error)) {
        root.o["error"] = error;
        return emit(root, 1);
    }
    root.o["revision"] = stored.revision;
    root.o["source_schema"] = stored.document.source_schema;
    root.o["count"] = static_cast<int>(stored.document.rules.size());
    if ((!request.revision.empty() && request.revision != stored.revision) ||
        request.cursor > stored.document.rules.size()) {
        root.o["status"] = "stale_or_invalid_cursor";
        return emit(root, 1);
    }
    const auto limit = std::min(stored.document.rules.size(), request.cursor + HIDE_EXPORT_PAGE);
    auto end = request.cursor;
    for (std::size_t i = request.cursor; i < limit; ++i) {
        root.o["items"].a.push_back(definition_json(stored.document.rules[i]));
        if (stringify_json(root).size() > 54UL * 1024) {
            root.o["items"].a.pop_back();
            break;
        }
        end = i + 1;
    }
    Stored after;
    if (!read_store(after, error) || after.revision != stored.revision ||
        runtime_boot_id() != boot) {
        root.o["status"] = "changed_or_unavailable";
        root.o["error"] = error;
        root.o["items"] = JsonValue::array();
        return emit(root, 1);
    }
    root.o["status"] = "observed";
    root.o["next_cursor"] = static_cast<int>(end);
    root.o["at_end"] = end == stored.document.rules.size();
    root.o["complete"] = request.cursor == 0 && end == stored.document.rules.size();
    return emit(root, 0);
}
int print_export() {
    Stored stored;
    Stored after;
    std::string error;
    std::string bytes;
    if (!read_store(stored, error) || !encode_portable_document(stored.document, bytes, error)) {
        std::cerr << "cannot export user hide definitions: " << error << '\n';
        return 1;
    }
    if (!read_store(after, error) || after.revision != stored.revision) {
        std::cerr << "user hide definitions changed or became unavailable during export: " << error
                  << '\n';
        return 1;
    }
    std::cout << bytes;
    return std::cout.good() ? 0 : 1;
}
int run_definition_command(const std::vector<std::string>& args) {
    Request request;
    std::string error;
    if (!parse_request(args, request, error)) {
        auto value = receipt(args.size() > 1 ? args[1] : "");
        value.o["reason"] = "invalid_arguments";
        value.o["error"] = error;
        return emit(value, 2);
    }
    if (definition_query(request.action)) {
        auto value = receipt(request.action);
        value.o["reason"] = "exact_local_query_required";
        return emit(value, 2);
    }
    return mutate(request);
}
int run_path_command(const std::vector<std::string>& args) {
    if (args.size() != 3 || (args[1] != "add" && args[1] != "remove" && args[1] != "retry-path") ||
        !valid_path(args[2])) {
        std::cerr << "hide add/remove/retry-path requires one valid absolute path\n";
        return 2;
    }
    // Keep legacy stdout empty. Structured receipts are available through the
    // new definition commands; errors still propagate to old Manager callers.
    std::stringbuf output;
    auto* old = std::cout.rdbuf(&output);
    class Restore {
    public:
        explicit Restore(std::streambuf* previous) : old(previous) {}
        Restore(const Restore&) = delete;
        Restore& operator=(const Restore&) = delete;
        Restore(Restore&&) = delete;
        Restore& operator=(Restore&&) = delete;
        ~Restore() { std::cout.rdbuf(old); }

    private:
        std::streambuf* old;
    };
    const Restore restore{old};
    Request request;
    request.action = args[1];
    const auto code = mutate(request, true, args[2]);
    if (code)
        std::cerr << output.str();
    return code;
}
}  // namespace sumhp::hide
