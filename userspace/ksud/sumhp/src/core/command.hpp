#pragma once

#include <string>
#include <vector>

namespace sumhp {

struct CommandResult {
    int exit_code = 0;
    int error_number = 0;
    std::string stdout_text;
    std::string stderr_text;
    std::string operation_id;
    bool operation_recorded = false;  // final durable write and readback confirmed
    bool command_executed = false;
    int command_exit_code = 0;
    int command_errno = 0;
    bool stdout_complete = true;
    bool stderr_complete = true;
    std::string operation_status;
    std::string observed_record_json;
};

int run_command(const std::vector<std::string>& args);
// Only the root-checked local operation route selects LocalTemporaryMaps.
// It requires an exact, canonical delete and a confirmed reservation.
enum class CaptureRoute { Regular, LocalTemporaryMaps };
CommandResult run_command_capture(const std::vector<std::string>& args,
                                  const std::string& prepared_id = {},
                                  const std::string& request_revision = {},
                                  CaptureRoute route = CaptureRoute::Regular);

}  // namespace sumhp
