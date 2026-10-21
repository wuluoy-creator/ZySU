#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "core/json.hpp"

namespace sumhp::profiles {
inline constexpr std::size_t PROFILE_MAX_BYTES = 8 * 1024 * 1024;
inline constexpr std::size_t PROFILE_INPUT_MAX_BYTES = PROFILE_MAX_BYTES + 4096;
inline constexpr std::size_t BANK_MAX_BYTES = 32 * 1024 * 1024;
inline constexpr std::size_t BANK_MAX_PROFILES = 16;
inline constexpr std::size_t PROFILE_RESPONSE_MAX_BYTES = 24 * 1024 * 1024;
bool valid_profile_id(const std::string& id);
bool parse_profile(const std::string& bytes, JsonValue& output, std::string& error);
bool capture_profile(const std::string& config, const std::string& modes, const std::string& rules,
                     const std::string& hidden, const std::string& name, const std::string& note,
                     JsonValue& output, std::string& error);
JsonValue diff_profiles(const JsonValue& current, const JsonValue& target,
                        const std::vector<std::string>& scopes);
struct Bank {
    JsonValue profiles = JsonValue::object();
    std::string revision, bytes;
    bool missing = false;
};
bool read_bank(Bank& bank, std::string& error);
std::string profile_revision(const JsonValue& profile);
JsonValue mutate_bank(const JsonValue& request);
bool read_profile_input(const std::string& path, std::string& bytes, std::string& error);
struct Request {
    std::string action, id, input_path;
    std::vector<std::string> scopes;
    // Manager reviewed reads bind selection and revisions to one bank snapshot.
    std::string expected_bank_revision, expected_profile_revision;
    bool manager = false;
};
bool parse_arguments(const std::vector<std::string>& args, Request& request, std::string& error);
JsonValue execute(const Request& request, const std::string& input);
int command(const std::vector<std::string>& args);
std::string manager_dispatch(const std::string& input);
}  // namespace sumhp::profiles
