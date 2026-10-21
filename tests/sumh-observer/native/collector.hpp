#pragma once
#include <string>

namespace sumh_observer {
// Parser-only validation, with no filesystem access or implicit collection.
void validate_request(const std::string &request);
// Throws std::exception only for an invalid request or failure of the sample
// envelope itself. Individual observations carry their own errno/completeness.
std::string collect(const std::string &request, const std::string &role,
                    int delegated_fd = -1);
// APK-only entry point. Retains a partially created run on failure; never
// overwrites an existing run and never recursively removes any directory.
std::string create_fixture(const std::string &files_dir,
                           const std::string &run_id);
} // namespace sumh_observer
