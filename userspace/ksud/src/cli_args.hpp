#pragma once

#include <map>
#include <string>
#include <vector>

namespace ksud {

struct CliArguments {
    std::string command;
    std::string path;
    std::vector<std::string> args;
    std::map<std::string, std::string> options;
    std::vector<std::string> option_args;
    bool verbose = false;
    bool forward_options = false;

    bool has(const std::string& name) const;
    std::string value(const std::string& name) const;
};

// Returns -1 to dispatch, otherwise the exit status after printing help or an error.
int parse_cli(const std::vector<std::string>& input, CliArguments& parsed);
// Same grammar/validation, without help/diagnostics or global color changes.
int parse_cli_quiet(const std::vector<std::string>& input, CliArguments& parsed);

}  // namespace ksud
