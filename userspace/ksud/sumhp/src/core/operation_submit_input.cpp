#include <algorithm>
#include "../../../../common/temporary_maps_command.hpp"
#include "../../../../common/utf8.hpp"
#include "cli_args.hpp"
#include "core/operation_submit.hpp"
#include "core/state_ledger.hpp"
#include "core/user_hide_input.hpp"

namespace sumhp::state {
namespace {
bool input_path_valid(const std::string& path) {
    return path == "-" ||
           (!path.empty() && path.front() == '/' && path.size() < 4096 &&
            path.find('\0') == std::string::npos &&
            path.find_first_of("\r\n") == std::string::npos && ksud::text::valid_utf8(path));
}
bool validate_command(const std::vector<std::string>& args, std::vector<std::string>& effective) {
    if (maps_temporary::is_prefix(args)) {
        maps_temporary::DeleteRequest request;
        if (!maps_temporary::parse_tracked_delete(args, request))
            return false;
        effective = args;
        return true;
    }
    // Structured stable-hide arguments are data, including group names such
    // as --help or --automatic. Share the controller's exact grammar rather
    // than letting generic global-option parsing reinterpret those strings.
    if (hide::definition_command(args)) {
        hide::Request request;
        std::string error;
        if (!hide::parse_manager_arguments(args, request, error) ||
            hide::definition_query(request.action))
            return false;
        effective = args;
        return true;
    }
    if (!command_needs_journal(args) ||
        std::find(args.begin(), args.end(), "--automatic") != args.end())
        return false;
    auto cli = std::vector<std::string>{"sumhp"};
    cli.insert(cli.end(), args.begin(), args.end());
    // Same production syntax/semantics without rendering help or diagnostics.
    ksud::CliArguments parsed;
    if (ksud::parse_cli_quiet(cli, parsed) != -1 || parsed.command != "sumhp" ||
        !parsed.options.empty() || parsed.verbose || !parsed.option_args.empty() ||
        !command_needs_journal(parsed.args))
        return false;
    auto positional = args;
    const auto separator = std::find(positional.begin(), positional.end(), "--");
    if (separator != positional.end())
        positional.erase(separator);
    if (parsed.args != positional)
        return false;
    effective = std::move(parsed.args);
    return true;
}
}  // namespace

bool parse_operation_submit_arguments(const std::vector<std::string>& args,
                                      OperationSubmitRequest& request, std::string& error) {
    error = "usage: ksud sumhp operation prepare --input <absolute-file|-> | execute <id> --input "
            "<absolute-file|->";
    OperationSubmitRequest value;
    if (args.size() == 4 && args[0] == "operation" && args[1] == "prepare" && args[2] == "--input")
        value.input_path = args[3];
    else if (args.size() == 5 && args[0] == "operation" && args[1] == "execute" &&
             valid_operation_id(args[2]) && args[3] == "--input") {
        value.execute = true;
        value.id = args[2];
        value.input_path = args[4];
    } else
        return false;
    if (!input_path_valid(value.input_path))
        return false;
    request = std::move(value);
    error.clear();
    return true;
}

bool parse_operation_input(const std::string& bytes, OperationInput& input, std::string& error) {
    const auto fail = [&](const char* reason) {
        error = reason;
        return false;
    };
    if (bytes.size() > OPERATION_INPUT_BYTES || !ksud::text::valid_utf8(bytes))
        return fail("input_capacity_or_utf8");
    JsonValue root;
    if (!json::Parser(bytes, true).parse(root, error))
        return false;
    if (!root.is_object() || root.o.size() != 2 || !root.find("schema_version") ||
        !root.find("args"))
        return fail("invalid_input_fields");
    const auto& schema = root.o.at("schema_version");
    const auto& values = root.o.at("args");
    if (!schema.is_number() || schema.n != 1 || !values.is_array() || values.a.size() < 2 ||
        values.a.size() > 32)
        return fail("invalid_input_schema_or_arguments");
    std::vector<std::string> args;
    for (const auto& value : values.a) {
        if (!value.is_string() || value.s.find('\0') != std::string::npos)
            return fail("invalid_argument");
        args.push_back(value.s);
    }
    OperationInput parsed;
    if (!validate_command(args, parsed.args))
        return fail("command_not_supported_or_invalid");
    parsed.canonical_document = stringify_json(root);
    // The daemon outer frame escapes this document a second time. Validate it
    // before preparation so a valid reservation has a representable request.
    auto frame = JsonValue::array();
    for (const auto& item : std::vector<std::string>{"operation", "execute", std::string(32, 'a'),
                                                     parsed.canonical_document})
        frame.a.emplace_back(item);
    if (stringify_json(frame).size() + 1 > 64UL * 1024)
        return fail("daemon_frame_capacity");
    parsed.request_revision = revision_for_bytes("sumhp-operation-v1\n" + stringify_json(values));
    if (parsed.request_revision.empty())
        return fail("request_digest_unavailable");
    parsed.name = args[0] + " " + args[1];
    if (args[0] == "sumh" && args.size() > 2 && (args[1] == "maps" || args[1] == "kernel-build"))
        parsed.name += " " + args[2];
    input = std::move(parsed);
    error.clear();
    return true;
}
}  // namespace sumhp::state
