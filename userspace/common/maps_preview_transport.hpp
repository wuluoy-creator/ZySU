#pragma once

#include <functional>
#include <string>
#include "diagnostic_package_request.hpp"
#include "diagnostics_request.hpp"
#include "module_preview_request.hpp"
#include "temporary_maps_request.hpp"

namespace sumhp::preview_transport {
enum class Service {
    Maps,
    PartitionDiscovery,
    UserHide,
    Operation,
    ModulePreview,
    Capabilities,
    TemporaryMaps,
    Diagnostics,
    DiagnosticPackages
};
// Fixed-purpose read-only exec transport. Never starts the SUMHP daemon.
// Returns the Manager envelope; incomplete output is discarded. Requires
// pidfd + P_PIDFD waitid. One unreaped/no-handle child quarantines this process's
// slot; there is no numeric-PID fallback or unbounded cleanup wait.
std::string request(const std::string& executable, unsigned timeout_ms = 8000,
                    Service service = Service::Maps);

// Save-only CAS mutation with fixed argv. Timeout/disconnect is an unknown
// outcome; callers must query current state and must never automatically replay.
std::string request_partitions_save(const std::string& executable, const std::string& revision,
                                    const std::string& partitions_json, unsigned timeout_ms = 8000);

// Strict shared hide-definition grammar; no legacy path commands or shell.
// Status is local; explicit mutations use normal daemon/journal dispatch.
// Timeout/disconnect is unknown and must never trigger automatic replay.
std::string request_user_hide(const std::string& executable, const std::string& arguments_json,
                              unsigned timeout_ms = 8000);
// Separate reservation/execute calls; prepare/queries are local, execute never
// starts a daemon or retries. Payload is the strict hide-only Manager object.
std::string request_operation(const std::string& executable, const std::string& request_json,
                              unsigned timeout_ms = 8000);
// Exact temporary Maps queries/deletion only. A lost response never retries.
std::string request_temporary_maps(const std::string& executable, const std::string& request_json,
                                   unsigned timeout_ms = 8000);
// Standalone ksud only: fixed /proc/self/exe with argv[0]="ksud". The parent
// reserves the diagnostic slot before spawn; forked samplers inherit its guard.
// Not a Manager JNI entry point (/proc/self/exe there is app_process).
std::string request_diagnostics(const std::string& request_json, unsigned timeout_ms = 8000);
// Manager always supplies its packaged standalone ksud. Package mutations have
// unknown outcome after disconnect and must be resolved by querying the ID.
std::string request_diagnostic_packages(const std::string& executable,
                                        const std::string& request_json,
                                        unsigned timeout_ms = 8000);
// Read-only production Planner with immutable inline input. No staging files,
// daemon or recovery. null candidate is saved-only; empty candidate is invalid.
std::string request_module_preview(const std::string& executable,
                                   const module_preview::Input& input, unsigned timeout_ms = 8000);

// Exact internal allowlisted `--channel` query/save routes only, after root
// authorization. stdin is the private seqpacket channel. No preview callback
// runs until the parent owns this process's self-opened pidfd and acknowledges.
int serve(const std::function<std::string()>& preview, Service service = Service::Maps);

// Hold this FD for the entire direct preview and inherit it across sampling
// forks. A stuck orphan retains the abstract-socket reservation, preventing
// repeated short-lived CLI calls from spawning more sampling children. Scope:
// one network namespace, not all independently created network namespaces.
// No filesystem path is created. Caller closes the returned owned FD.
int acquire_preview_guard();

// Same bounded envelope for JNI scheduling/root setup failures.
std::string failure_envelope(int error);
}  // namespace sumhp::preview_transport
