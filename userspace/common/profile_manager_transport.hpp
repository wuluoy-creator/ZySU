#pragma once

#include <functional>
#include <string>

namespace sumhp::profile_transport {
// Fixed standalone route, private regular single-link files, 8 MiB + 4096 input
// and 24 MiB output. Caller owns descriptors; JNI duplicates them before work.
// No daemon, shell, argv documents or path-bearing document access. Failure is
// never a historical mutation receipt: a dispatched writer may still finish.
// Never retries or kills a child; a live/unreaped child quarantines this process.
std::string request(const std::string& executable, int input_fd, int output_fd,
                    unsigned timeout_ms = 8000);
std::string failure_receipt(int error);
// stdin is a private seqpacket channel, fd 3 the inherited network-namespace
// abstract guard. Dispatch requires self-pidfd ACK and exact input DONE + EOF.
int serve(const std::function<std::string(const std::string&)>& callback);
}  // namespace sumhp::profile_transport
