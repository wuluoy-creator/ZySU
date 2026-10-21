#include "mount/sumh.hpp"

#include "core/json.hpp"
#include "core/runtime.hpp"
#include "core/user_hide_store.hpp"
#include "mount/kernel_build_apply.hpp"
#include "mount/maps_history.hpp"
#include "mount/maps_snapshot.hpp"
#include "mount/module_rebuild.hpp"
#include "mount/mount_fs.hpp"
#include "mount/partition_alias.hpp"
#include "mount/runtime_apply.hpp"
#include "sumhp/embedded.hpp"
#include "sumhp/sumh_client.hpp"
#include "uapi/sumh.h"
#include "utils.hpp"

#include <dirent.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace sumhp::mount::sumh {

namespace fs = std::filesystem;
using fsutil::mlog;

namespace {

bool user_hide_restore_pending = false;

fs::path active_file() {
    return runtime_data_dir() / "run" / "sumh_active";
}
fs::path mapping_plan_file() {
    return runtime_data_dir() / "run" / "sumh_mapping_plan";
}

bool path_matches_rule(const std::string& candidate_path, const std::string& prefix) {
    if (candidate_path == prefix) {
        return true;
    }
    if (candidate_path.size() <= prefix.size() ||
        candidate_path.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    return prefix.back() == '/' || candidate_path[prefix.size()] == '/';
}

const ModuleRule* selected_rule(const std::string& path, const std::vector<ModuleRule>& rules) {
    const ModuleRule* selected = nullptr;
    for (const auto& rule : rules) {
        if (!rule.path.empty() && rule.path.front() == '/' && path_matches_rule(path, rule.path) &&
            (!selected || rule.path.size() >= selected->path.size()))
            selected = &rule;
    }
    return selected;
}

std::string effective_mode(const std::string& path, const std::vector<ModuleRule>& rules) {
    const auto* selected = selected_rule(path, rules);
    return selected ? selected->mode : "sumh";
}

struct PlannerTrace {
    ModulePlanDiagnostics* output = nullptr;
    std::string module_id;
    const std::set<std::string>* hidden = nullptr;
    std::string original_path, resolved_path, source;
};

void trace_cursor(PlannerTrace* trace, const std::string& original, const std::string& resolved,
                  const std::string& source) {
    if (!trace)
        return;
    trace->original_path = original;
    trace->resolved_path = resolved;
    trace->source = source;
}

void trace_event(PlannerTrace* trace, ModulePlanEvent event) {
    if (!trace)
        return;
    auto& output = *trace->output;
    event.module_id = trace->module_id;
    const auto bytes = sizeof(event) + event.module_id.size() + event.original_path.size() +
                       event.resolved_path.size() + event.source.size() + event.action.size() +
                       event.reason.size() + event.matched_rule_path.size() +
                       event.requested_mode.size() + event.effective_mode.size() +
                       event.hidden_parent.size() + event.previous_module_id.size() +
                       event.previous_source.size();
    if (output.events.size() >= ModulePlanDiagnostics::max_events ||
        bytes > ModulePlanDiagnostics::max_bytes - output.bytes) {
        output.events_truncated = true;
        return;
    }
    output.bytes += bytes;
    output.events.push_back(std::move(event));
}

bool trace_budget_available(PlannerTrace* trace, std::string& error) {
    if (trace && (trace->output->events_truncated || trace->output->observations_truncated)) {
        error = "module plan diagnostic capacity exceeded";
        return false;
    }
    return true;
}

bool trace_action_available(PlannerTrace* trace, const ModuleMountPlan& plan, std::string& error) {
    if (!trace_budget_available(trace, error))
        return false;
    if (trace && plan.mappings.size() + plan.hide.size() >= ModulePlanDiagnostics::max_actions) {
        trace->output->events_truncated = true;
        error = "module plan action capacity exceeded";
        return false;
    }
    return true;
}

ModulePlanEvent entry_event(PlannerTrace* trace, const std::string& original,
                            const std::string& resolved, const std::string& source,
                            const std::vector<ModuleRule>& rules, const std::string& action,
                            const std::string& reason) {
    ModulePlanEvent event;
    event.original_path = original;
    event.resolved_path = resolved;
    event.source = source;
    event.action = action;
    event.reason = reason;
    event.alias = original != resolved;
    const auto* selected = selected_rule(resolved, rules);
    event.matched_rule_path = selected ? selected->path : "";
    event.requested_mode = selected ? selected->mode : "sumh";
    event.effective_mode = event.requested_mode == "none" || event.requested_mode == "hide"
                               ? event.requested_mode
                               : "sumh";
    if (trace && trace->hidden) {
        for (const auto& path : *trace->hidden) {
            if (path_matches_rule(resolved, path)) {
                event.hidden_parent = path;
                break;
            }
        }
    }
    return event;
}

void trace_entry(PlannerTrace* trace, const std::string& original, const std::string& resolved,
                 const std::string& source, const std::vector<ModuleRule>& rules,
                 const std::string& action, const std::string& reason) {
    if (trace)
        trace_event(trace, entry_event(trace, original, resolved, source, rules, action, reason));
}

void trace_mapping(PlannerTrace* trace, const std::string& original, const std::string& resolved,
                   const std::string& source, const std::vector<ModuleRule>& rules, bool merge,
                   std::size_t index) {
    if (!trace)
        return;
    auto event = entry_event(trace, original, resolved, source, rules, merge ? "MERGE" : "ADD",
                             merge ? "eligible_directory" : "source_entry");
    event.mapping_index = index;
    if (!merge) {
        // Only earlier direct contributions are attributable here. A MERGE or
        // another backend can change the kernel view; do not call this a winner.
        for (auto it = trace->output->events.rbegin(); it != trace->output->events.rend(); ++it) {
            if (it->action == "ADD" && it->resolved_path == resolved) {
                event.has_previous_direct = true;
                event.previous_module_id = it->module_id;
                event.previous_source = it->source;
                break;
            }
        }
    }
    trace_event(trace, std::move(event));
}

bool trace_final_hidden(PlannerTrace* trace, const ModuleMountPlan& plan, std::string& error) {
    if (!trace)
        return true;
    auto& output = *trace->output;
    for (auto& event : output.events) {
        if (event.resolved_path.empty())
            continue;
        for (const auto& path : plan.hide) {
            if (!path_matches_rule(event.resolved_path, path))
                continue;
            const auto without_previous = output.bytes - event.hidden_parent.size();
            if (path.size() > ModulePlanDiagnostics::max_bytes - without_previous) {
                output.events_truncated = true;
                return trace_budget_available(trace, error);
            }
            output.bytes = without_previous + path.size();
            event.hidden_parent = path;
            break;
        }
    }
    return trace_budget_available(trace, error);
}

void trace_observation(PlannerTrace* trace, const fs::path& path, const std::string& metadata) {
    if (!trace)
        return;
    auto& output = *trace->output;
    const auto name = path.string();
    const auto bytes =
        sizeof(ModuleSourceObservation) + trace->module_id.size() + name.size() + metadata.size();
    if (output.observations.size() >= ModulePlanDiagnostics::max_observations ||
        bytes > ModulePlanDiagnostics::max_bytes - output.bytes) {
        output.observations_truncated = true;
        return;
    }
    output.bytes += bytes;
    output.observations.push_back({trace->module_id, name, metadata});
}

bool inspect_source(const fs::path& source, struct stat& metadata, PlannerTrace* trace,
                    std::string& error) {
    if (lstat(source.c_str(), &metadata) != 0) {
        error = "cannot inspect module entry: " + source.string() + ": " + std::strerror(errno);
        return false;
    }
    std::string link_text;
    if (S_ISLNK(metadata.st_mode)) {
        std::error_code ec;
        const auto link = fs::read_symlink(source, ec);
        if (ec) {
            error = "cannot read module symlink " + source.string() + ": " + ec.message();
            return false;
        }
        link_text = ":link=" + link.generic_string();
    }
    if (trace) {
        const std::string observed =
            std::to_string(metadata.st_dev) + ":" + std::to_string(metadata.st_ino) + ":" +
            std::to_string(metadata.st_mode) + ":" + std::to_string(metadata.st_size) + ":" +
            std::to_string(metadata.st_mtim.tv_sec) + ":" +
            std::to_string(metadata.st_mtim.tv_nsec) + ":" +
            std::to_string(metadata.st_ctim.tv_sec) + ":" +
            std::to_string(metadata.st_ctim.tv_nsec) + link_text;
        trace_observation(trace, source, observed);
    }
    return trace_budget_available(trace, error);
}

bool source_type_unchanged(const fs::path& source, const fs::file_status& status,
                           const struct stat& metadata, std::string& error) {
    if (fs::is_directory(status) != static_cast<bool>(S_ISDIR(metadata.st_mode)) ||
        fs::is_symlink(status) != static_cast<bool>(S_ISLNK(metadata.st_mode))) {
        error = "module entry type changed during preparation: " + source.string();
        return false;
    }
    return true;
}

bool has_nested_rule(const std::string& path, const std::vector<ModuleRule>& rules) {
    return std::any_of(rules.begin(), rules.end(), [&](const ModuleRule& rule) {
        return rule.path.size() > path.size() && path_matches_rule(rule.path, path);
    });
}

// Resolve symlinked partition paths (e.g. /system/vendor -> /vendor) without
// discarding a non-existent leaf that a module adds.
bool resolve_virtual_path(const std::string& value, std::string& resolved, std::string& error,
                          bool directory = false) {
    const fs::path path(value);
    const auto missing = [](const std::error_code& ec) {
        return !ec || ec == std::errc::no_such_file_or_directory ||
               ec == std::errc::not_a_directory;
    };
    const auto fail = [&](const std::error_code& ec) {
        error = "cannot resolve virtual path " + value + ": " + ec.message();
        return false;
    };
    std::error_code ec;
    if (directory) {
        const bool is_directory = fs::is_directory(path, ec);
        if (!missing(ec))
            return fail(ec);
        if (is_directory) {
            const auto canonical = fs::canonical(path, ec);
            if (ec)
                return fail(ec);
            resolved = canonical.generic_string();
            return true;
        }
    }
    if (!path.has_parent_path()) {
        resolved = value;
        return true;
    }
    fs::path current = path.parent_path();
    std::vector<fs::path> suffix;
    while (!current.empty()) {
        ec.clear();
        if (fs::exists(current, ec)) {
            current = fs::canonical(current, ec);
            if (ec)
                return fail(ec);
            break;
        }
        if (!missing(ec))
            return fail(ec);
        if (current == "/")
            break;
        suffix.push_back(current.filename());
        current = current.parent_path();
    }
    for (auto it = suffix.rbegin(); it != suffix.rend(); ++it)
        current /= *it;
    current /= path.filename();
    resolved = current.generic_string();
    return true;
}

bool resolve_module_rules(const std::vector<ModuleRule>& input, std::vector<ModuleRule>& output,
                          std::string& error, PlannerTrace* trace = nullptr) {
    // Legacy lists accepted duplicate literal paths; their last entry wins.
    // Resolve aliases after that reduction so upgrades preserve that contract.
    std::map<std::string, std::string> literal;
    for (const auto& rule : input) {
        const auto previous = literal.find(rule.path);
        if (previous != literal.end() && trace) {
            auto event =
                entry_event(trace, rule.path, rule.path, "", {}, "RULE", "literal_overridden");
            event.requested_mode = previous->second;
            event.effective_mode.clear();
            trace_event(trace, std::move(event));
        }
        literal[rule.path] = rule.mode;
        if (!trace_budget_available(trace, error))
            return false;
    }
    std::map<std::string, std::string> resolved;
    for (const auto& [literal_path, mode] : literal) {
        // A legacy trailing slash only matches descendants, not the directory
        // itself. Preserve that boundary while resolving its directory alias.
        const bool descendants_only = literal_path.size() > 1 && literal_path.back() == '/';
        std::string path;
        trace_cursor(trace, literal_path, "", "");
        if (!resolve_virtual_path(literal_path, path, error, true))
            return false;
        if (descendants_only && path.back() != '/')
            path += '/';
        const auto [it, inserted] = resolved.emplace(path, mode);
        if (trace) {
            auto event = entry_event(trace, literal_path, path, "", {}, "RULE",
                                     inserted ? "resolved" : "alias_duplicate");
            event.matched_rule_path = path;
            event.requested_mode = mode;
            event.effective_mode = mode;
            if (!inserted && it->second != mode)
                event.reason = "alias_conflict";
            trace_event(trace, std::move(event));
        }
        if (!inserted && it->second != mode) {
            error = "conflicting module rule aliases resolve to " + path;
            return false;
        }
        if (!trace_budget_available(trace, error))
            return false;
    }
    output.clear();
    for (const auto& [path, mode] : resolved)
        output.push_back({path, mode});
    return true;
}

// Bulk MERGE materialization cannot process whiteouts or apply per-entry
// partition aliases. Fall back to the same compiler's ADD/HIDE walk for those
// trees. Unsupported replacement metadata still fails before any ioctl.
bool merge_tree_supported(const fs::path& root, const std::string& virtual_root, bool& merge,
                          std::string& error, PlannerTrace* trace = nullptr,
                          const std::vector<ModuleRule>& rules = {}) {
    merge = true;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
        if (!trace_budget_available(trace, error))
            return false;
        const auto source = it->path();
        const auto virtual_path =
            (fs::path(virtual_root) / source.lexically_relative(root)).generic_string();
        trace_cursor(trace, virtual_path, "", source.string());
        const auto status = it->symlink_status(ec);
        if (ec)
            break;
        struct stat metadata = {};
        if (!inspect_source(source, metadata, trace, error) ||
            !source_type_unchanged(source, status, metadata, error))
            return false;
        bool opaque = false;
        if (source.filename() == ".replace" ||
            (fs::is_directory(status) &&
             (!fsutil::directory_is_opaque(source.string(), opaque) || opaque))) {
            error = "unsupported replacement metadata: " + source.string();
            return false;
        }
        bool whiteout = false;
        if (!fsutil::path_is_whiteout(source.string(), whiteout)) {
            error = "cannot read module entry metadata: " + source.string();
            return false;
        }
        trace_observation(trace, source, whiteout ? "whiteout=1" : "whiteout=0");
        if (fs::is_directory(status))
            trace_observation(trace, source, "opaque=0");
        std::string resolved;
        if (!resolve_virtual_path(virtual_path, resolved, error, fs::is_directory(status)))
            return false;
        trace_cursor(trace, virtual_path, resolved, source.string());
        const auto fallback = [&](const char* reason) {
            merge = false;
            trace_entry(trace, virtual_path, resolved, source.string(), rules, "WALK", reason);
        };
        if (whiteout)
            fallback("merge_whiteout");
        if (it.depth() >= 8)
            fallback("merge_depth_limit");
        if (fs::is_symlink(status))
            fallback("merge_symlink");
        if (resolved != virtual_path)
            fallback("merge_partition_alias");
    }
    if (ec)
        error = "scan failed for " + root.string() + ": " + ec.message();
    return !ec && trace_budget_available(trace, error);
}

bool compile_tree(const fs::path& source_root, const std::string& virtual_root,
                  const std::vector<ModuleRule>& rules, ModuleMountPlan& batch, std::string& error,
                  PlannerTrace* trace = nullptr) {
    if (!trace_budget_available(trace, error))
        return false;
    trace_cursor(trace, virtual_root, "", source_root.string());
    std::error_code ec;
    const auto root_status = fs::symlink_status(source_root, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(root_status))) {
        trace_observation(trace, source_root, "missing");
        trace_entry(trace, virtual_root, virtual_root, source_root.string(), rules, "SKIP",
                    "missing_partition");
        return trace_budget_available(trace, error);
    }
    if (ec) {
        error = "cannot inspect module partition " + source_root.string() + ": " + ec.message();
        return false;
    }
    if (!fs::is_directory(root_status)) {
        error = "module partition must be a real directory: " + source_root.string();
        return false;
    }
    struct stat root_metadata = {};
    if (!inspect_source(source_root, root_metadata, trace, error) ||
        !source_type_unchanged(source_root, root_status, root_metadata, error))
        return false;
    std::string resolved_root;
    if (!resolve_virtual_path(virtual_root, resolved_root, error, true))
        return false;
    trace_cursor(trace, virtual_root, resolved_root, source_root.string());
    const auto root_mode = effective_mode(resolved_root, rules);
    if (root_mode == "hide" || (root_mode == "none" && !has_nested_rule(resolved_root, rules))) {
        trace_entry(trace, virtual_root, resolved_root, source_root.string(), rules, "SKIP",
                    root_mode == "hide" ? "hidden_partition" : "excluded_partition");
        return trace_budget_available(trace, error);
    }
    bool root_opaque;
    if (!fsutil::directory_is_opaque(source_root.string(), root_opaque) || root_opaque) {
        error = "unsupported partition replacement: " + source_root.string();
        return false;
    }
    trace_observation(trace, source_root, "opaque=0");
    auto it = fs::recursive_directory_iterator(source_root, ec);
    const auto end = fs::recursive_directory_iterator();
    for (; it != end && !ec; it.increment(ec)) {
        if (!trace_budget_available(trace, error))
            return false;
        const fs::path source = it->path();
        const fs::path relative = source.lexically_relative(source_root);
        const auto original_path = (fs::path(virtual_root) / relative).generic_string();
        trace_cursor(trace, original_path, "", source.string());
        const auto source_status = it->symlink_status(ec);
        if (ec)
            break;
        struct stat st = {};
        if (!inspect_source(source, st, trace, error) ||
            !source_type_unchanged(source, source_status, st, error))
            return false;
        if (fs::is_symlink(source_status) && fsutil::module_partition_alias(source)) {
            trace_entry(trace, original_path, original_path, source.string(), rules, "SKIP",
                        "installer_partition_alias");
            continue;  // The installer alias is compiled through the top-level partition.
        }
        std::string virtual_path;
        if (!resolve_virtual_path(original_path, virtual_path, error,
                                  fs::is_directory(source_status)))
            return false;
        trace_cursor(trace, original_path, virtual_path, source.string());
        const std::string mode = effective_mode(virtual_path, rules);
        if (mode == "hide") {
            if (!batch.hide.count(virtual_path) && !trace_action_available(trace, batch, error))
                return false;
            batch.hide.insert(virtual_path);
            trace_entry(trace, original_path, virtual_path, source.string(), rules, "HIDE",
                        "selected_hide");
            // Use the already checked no-follow status. A second status read on
            // this branch could fail and have its error cleared by increment().
            if (fs::is_directory(source_status))
                it.disable_recursion_pending();
            continue;
        }
        if (mode == "none") {
            const bool descend = has_nested_rule(virtual_path, rules);
            if (fs::is_directory(source_status) && !descend)
                it.disable_recursion_pending();
            trace_entry(trace, original_path, virtual_path, source.string(), rules, "SKIP",
                        descend ? "excluded_with_nested_rules" : "excluded_path");
            continue;
        }
        if (mode != "sumh" && mode != "auto") {
            // Existing policy: selected SUMH modules never create mixed subtrees.
            trace_entry(trace, original_path, virtual_path, source.string(), rules, "POLICY",
                        "path_backend_coerced_to_sumh");
        }
        if (source.filename() == ".replace") {
            error = "unsupported replacement marker: " + source.string();
            return false;
        }
        bool whiteout = false;
        if (!fsutil::path_is_whiteout(source.string(), whiteout)) {
            error = "cannot read module entry metadata: " + source.string();
            return false;
        }
        trace_observation(trace, source, whiteout ? "whiteout=1" : "whiteout=0");
        if (whiteout) {
            if (!batch.hide.count(virtual_path) && !trace_action_available(trace, batch, error))
                return false;
            batch.hide.insert(virtual_path);
            trace_entry(trace, original_path, virtual_path, source.string(), rules, "HIDE",
                        "whiteout");
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            bool opaque = false;
            if (!fsutil::directory_is_opaque(source.string(), opaque) || opaque) {
                error = "unsupported replacement metadata: " + source.string();
                return false;
            }
            trace_observation(trace, source, "opaque=0");
            std::error_code target_ec;
            const bool target_directory = fs::is_directory(virtual_path, target_ec);
            if (target_ec && target_ec != std::errc::no_such_file_or_directory &&
                target_ec != std::errc::not_a_directory) {
                error =
                    "cannot inspect virtual directory " + virtual_path + ": " + target_ec.message();
                return false;
            }
            if (target_directory && !has_nested_rule(virtual_path, rules)) {
                bool merge = false;
                if (!merge_tree_supported(source, virtual_path, merge, error, trace, rules))
                    return false;
                if (merge) {
                    if (!trace_action_available(trace, batch, error))
                        return false;
                    trace_mapping(trace, original_path, virtual_path, source.string(), rules, true,
                                  batch.mappings.size());
                    batch.mappings.push_back({virtual_path, source.string(), true});
                    it.disable_recursion_pending();
                } else {
                    trace_entry(trace, original_path, virtual_path, source.string(), rules, "WALK",
                                "merge_requires_entry_walk");
                }
            } else {
                trace_entry(trace, original_path, virtual_path, source.string(), rules, "WALK",
                            target_directory ? "nested_path_rule" : "target_not_directory");
            }
            continue;
        }
        if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
            if (S_ISLNK(st.st_mode)) {
                std::error_code target_ec;
                const auto target_status = fs::symlink_status(virtual_path, target_ec);
                if (target_ec && target_ec != std::errc::no_such_file_or_directory &&
                    target_ec != std::errc::not_a_directory) {
                    error = "cannot inspect virtual symlink " + virtual_path + ": " +
                            target_ec.message();
                    return false;
                }
                if (!target_ec && fs::is_symlink(target_status)) {
                    std::error_code link_ec;
                    const fs::path module_link = fs::read_symlink(source, link_ec);
                    if (link_ec) {
                        error = "cannot read module symlink " + source.string() + ": " +
                                link_ec.message();
                        return false;
                    }
                    const fs::path system_link = fs::read_symlink(virtual_path, link_ec);
                    if (link_ec) {
                        error = "cannot read virtual symlink " + virtual_path + ": " +
                                link_ec.message();
                        return false;
                    }
                    const fs::path parent = fs::path(virtual_path).parent_path();
                    const auto destination = [&](const fs::path& link) {
                        return (link.is_absolute() ? link : parent / link).lexically_normal();
                    };
                    if (destination(module_link) == destination(system_link)) {
                        if (!trace)
                            mlog("sumh: preserving matching symlink alias " + virtual_path +
                                 " from " + source.string());
                        trace_entry(trace, original_path, virtual_path, source.string(), rules,
                                    "SKIP", "matching_system_symlink");
                        continue;
                    }
                }
                target_ec.clear();
                if (fs::is_directory(virtual_path, target_ec) && !target_ec) {
                    error = "refusing to replace directory with symlink " + virtual_path +
                            " from " + source.string();
                    return false;
                }
                if (target_ec && target_ec != std::errc::no_such_file_or_directory &&
                    target_ec != std::errc::not_a_directory) {
                    error = "cannot resolve virtual symlink " + virtual_path + ": " +
                            target_ec.message();
                    return false;
                }
            }
            if (!trace_action_available(trace, batch, error))
                return false;
            trace_mapping(trace, original_path, virtual_path, source.string(), rules, false,
                          batch.mappings.size());
            batch.mappings.push_back({virtual_path, source.string(), false});
        } else {
            trace_entry(trace, original_path, virtual_path, source.string(), rules, "SKIP",
                        "unsupported_source_type");
        }
    }
    if (ec)
        error = "scan failed for " + source_root.string() + ": " + ec.message();
    return !ec && trace_budget_available(trace, error);
}

bool disable_kernel_features(std::string* error = nullptr) {
    const auto disable = [&](bool result, const char* name) {
        if (!result) {
            if (error && error->empty()) {
                *error = std::string("failed to disable SUMH ") + name;
            }
        }
        return result;
    };
    return disable(::sumhp::sumh::set_debug(false), "kernel debug") &&
           disable(::sumhp::sumh::set_stealth(false), "stealth") &&
           disable(::sumhp::sumh::set_mount_hide(false), "mount hide") &&
           disable(::sumhp::sumh::set_maps_spoof(false), "maps spoof") &&
           disable(::sumhp::sumh::set_statfs_spoof(false), "statfs spoof") &&
           disable(::sumhp::sumh::set_kernel_build(false), "kernel build spoof");
}

}  // namespace

bool restore_managed_maps_rules(const Config& config, std::string& error,
                                ManagedMapsRestoreState* state, RestoreTrigger trigger) {
    if (state)
        *state = ManagedMapsRestoreState::Unchanged;
    if (!validate_maps_path_rules(config.maps_rules, error))
        return false;
    // Probe before resolving paths so an old kernel is reported accurately.
    const auto caps = ::sumhp::sumh::feature_capabilities();
    if (!caps.ok) {
        errno = caps.last_errno;
        error = "failed to query managed maps support: " + std::string(std::strerror(errno));
        return false;
    }
    if (!(caps.bitmask & SUMH_FEATURE_MANAGED_MAPS)) {
        if (config.maps_rules.empty())
            return true;
        errno = EOPNOTSUPP;
        error = "this kernel does not support managed maps rules";
        return false;
    }
    MapsSubmissionTracker submission;
    if (!submission.valid()) {
        error = "cannot allocate Maps submission status";
        return false;
    }
    fsutil::NamespaceOperationState namespace_state;
    const bool ok = fsutil::run_in_init_mount_ns(
        [&]() {
            std::vector<::sumhp::sumh::MapsRule> resolved;
            std::set<std::pair<unsigned long, unsigned long>> targets;
            for (const auto& rule : config.maps_rules) {
                const auto target = read_maps_identity(rule.target_path);
                const auto replacement = read_maps_identity(rule.replacement_path);
                if (target.kind != MapsIdentityKind::Regular ||
                    replacement.kind != MapsIdentityKind::Regular) {
                    const bool target_failed = target.kind != MapsIdentityKind::Regular;
                    errno = target_failed ? target.error : replacement.error;
                    error = "cannot resolve regular maps file " +
                            (target_failed ? rule.target_path : rule.replacement_path) + ": " +
                            std::strerror(errno);
                    return false;
                }
                ::sumhp::sumh::MapsRule item;
                item.target_ino = target.ino;
                item.target_dev = target.dev;
                item.spoofed_ino = replacement.ino;
                item.spoofed_dev = replacement.dev;
                if (!targets.emplace(item.target_ino, item.target_dev).second) {
                    errno = EEXIST;
                    error = "maps target paths resolve to the same file: " + rule.target_path;
                    return false;
                }
                item.spoofed_path = rule.replacement_path;
                resolved.push_back(std::move(item));
            }
            return replace_managed_maps_recorded(config.maps_rules, resolved, submission, error,
                                                 trigger);
        },
        error, &namespace_state);
    if (state) {
        *state = ManagedMapsRestoreState::Unchanged;
        if (ok)
            *state = ManagedMapsRestoreState::Replaced;
        else if (namespace_state == fsutil::NamespaceOperationState::Unknown)
            *state = ManagedMapsRestoreState::Unknown;
        else if (namespace_state == fsutil::NamespaceOperationState::CallbackFailed)
            *state = submission.state();
    }
    if (!ok && namespace_state == fsutil::NamespaceOperationState::Unknown)
        error += "; outcome_unknown: managed Maps replacement may have committed; not retried";
    if (!ok && error.empty())
        error = "failed to enter init mount namespace for managed maps rules";
    return ok;
}

bool apply_maps_config(const Config& config, std::string& error, bool restore_rules,
                       FeatureApplyOutcome* outcome, RestoreTrigger trigger) {
    error.clear();
    FeatureApplyOutcome result;
    result.feature = "maps_spoof";
    const auto finish = [&](FeatureApplyState state, const std::string& detail, int code = 0) {
        result.state = state;
        result.detail = detail;
        result.error_number = code;
        if (outcome)
            *outcome = result;
        if (state != FeatureApplyState::Applied) {
            error = detail;
            errno = code;
            return false;
        }
        return true;
    };
    // A prior unresolved submission must not be silently retried by daemon,
    // boot or module lifecycle recovery, including an automatic switch/clear.
    if (trigger == RestoreTrigger::Automatic && !maps_automatic_restore_allowed(error))
        return finish(FeatureApplyState::Failed, error, errno ? errno : EIO);
    // A rejected enable must not replace the managed set first. The final
    // kernel setter still checks readiness, which may change after preflight.
    const auto caps = ::sumhp::sumh::feature_capabilities();
    if (!caps.ok)
        return finish(FeatureApplyState::Failed, "failed to query maps spoof support",
                      caps.last_errno ? caps.last_errno : EIO);
    if (config.enable_maps_spoof) {
        if (!(caps.bitmask & SUMH_FEATURE_MAPS_SPOOF))
            return finish(FeatureApplyState::Unsupported, "this kernel does not support maps spoof",
                          EOPNOTSUPP);
    }
    ManagedMapsRestoreState managed_state = ManagedMapsRestoreState::Unchanged;
    if (restore_rules && config.enable_maps_spoof &&
        !restore_managed_maps_rules(config, error, &managed_state, trigger)) {
        const int code = errno ? errno : EIO;
        FeatureApplyState state = FeatureApplyState::Failed;
        if (managed_state == ManagedMapsRestoreState::Unknown)
            state = FeatureApplyState::Unknown;
        else if (managed_state == ManagedMapsRestoreState::Replaced)
            state = FeatureApplyState::Partial;
        return finish(state, error, code);
    }
    const int runtime_caps = SUMH_FEATURE_RUNTIME_STATE | SUMH_FEATURE_RUNTIME_SET;
    const bool conditional = (caps.bitmask & runtime_caps) == runtime_caps;
    if (conditional) {
        result = apply_runtime_feature(::sumhp::sumh::RuntimeFeature::MapsSpoof,
                                       config.enable_maps_spoof);
        if (result.state != FeatureApplyState::Applied) {
            if (managed_state == ManagedMapsRestoreState::Replaced) {
                result.detail = std::string("partial: managed rules replaced; switch=") +
                                feature_apply_state_name(result.state) + "; " + result.detail;
                result.state = FeatureApplyState::Partial;
            }
            return finish(result.state, result.detail, result.error_number);
        }
    } else if (!::sumhp::sumh::set_maps_spoof(config.enable_maps_spoof)) {
        const int code = errno ? errno : EIO;
        std::string detail = std::string("failed to ") +
                             (config.enable_maps_spoof ? "enable" : "disable") +
                             " SUMH maps spoof: " + std::strerror(code);
        if (managed_state == ManagedMapsRestoreState::Replaced) {
            detail += "; partial: managed rules replaced, switch outcome unknown; "
                      "previous managed identities cannot be queried for rollback";
            return finish(FeatureApplyState::Partial, detail, code);
        }
        detail += "; outcome_unknown: switch has no readback in this ABI";
        return finish(FeatureApplyState::Unknown, detail, code);
    }
    // Disabling must work even if a saved path has disappeared. Discard stale
    // managed identities while retaining saved paths and unmanaged numeric rules.
    if (restore_rules && !config.enable_maps_spoof) {
        Config cleared = config;
        cleared.maps_rules.clear();
        if (!restore_managed_maps_rules(cleared, error, &managed_state, trigger)) {
            const int code = errno ? errno : EIO;
            return finish(
                FeatureApplyState::Partial,
                "partial: maps spoof disabled; managed set clear or receipt unconfirmed: " + error,
                code);
        }
    }
    return finish(FeatureApplyState::Applied, result.detail);
}

bool apply_feature_config(const Config& config, std::string& error, bool apply_immediately,
                          bool apply_maps, FeatureApplyReport* output) {
    error.clear();
    FeatureApplyReport report;
    const auto finish = [&]() {
        if (output)
            *output = report;
        if (report.ok())
            return true;
        error = "SUMH feature apply: " + report.summary();
        for (const auto& outcome : report.outcomes) {
            if (outcome.error_number) {
                errno = outcome.error_number;
                break;
            }
        }
        return false;
    };
    // GET_FEATURES failure is unknown support, not proof that all flags are off.
    // No setter runs until capability and rollback-readable state are sampled.
    const auto caps = ::sumhp::sumh::feature_capabilities();
    if (!caps.ok) {
        report.outcomes.push_back({"capabilities", FeatureApplyState::Failed,
                                   caps.last_errno ? caps.last_errno : EIO,
                                   "preflight failed; no feature mutations attempted"});
        return finish();
    }
    const bool apply_build = apply_immediately || !config.enable_kernel_build_spoof ||
                             config.kernel_build_apply_stage == "post-fs-data" ||
                             ksud::getprop("sys.boot_completed") == "1";

    mlog("apply features: mount_hide=" + std::to_string(config.enable_mount_hide) +
         " mode=" + config.mount_hide_mode + " maps=" + std::to_string(config.enable_maps_spoof) +
         " statfs=" + std::to_string(config.enable_statfs_spoof) +
         " kernel_build=" + std::to_string(config.enable_kernel_build_spoof) +
         " overlay_xattrs=" + std::to_string(config.enable_overlay_xattr_hide));
    // Each feature is independent: a failed setter never resets successful or
    // pre-existing unrelated features. Legacy flags have no reliable getter, so
    // a transport failure cannot be represented as a confirmed unchanged state.
    const int runtime_caps = SUMH_FEATURE_RUNTIME_STATE | SUMH_FEATURE_RUNTIME_SET;
    const bool conditional = (caps.bitmask & runtime_caps) == runtime_caps;
    const auto apply = [&](const char* name, ::sumhp::sumh::RuntimeFeature id, bool enable,
                           int capability, std::uint32_t mode, const auto& setter) {
        FeatureApplyOutcome result;
        result.feature = name;
        if (enable && capability && !(caps.bitmask & capability)) {
            result.state = FeatureApplyState::Unsupported;
            result.error_number = EOPNOTSUPP;
            result.detail = "preflight rejected enable; unchanged";
        } else if (conditional) {
            result = apply_runtime_feature(id, enable, mode);
        } else if (setter()) {
            result.state = FeatureApplyState::Applied;
        } else {
            result.state = FeatureApplyState::Unknown;
            result.error_number = errno ? errno : EIO;
            result.detail = std::string(std::strerror(result.error_number)) +
                            "; no state getter for rollback; unrelated features retained";
        }
        report.outcomes.push_back(std::move(result));
    };
    apply("kernel_debug", ::sumhp::sumh::RuntimeFeature::Debug, config.enable_kernel_debug, 0, 0,
          [&]() { return ::sumhp::sumh::set_debug(config.enable_kernel_debug); });
    apply("stealth", ::sumhp::sumh::RuntimeFeature::Stealth, config.enable_stealth, 0, 0,
          [&]() { return ::sumhp::sumh::set_stealth(config.enable_stealth); });
    auto mode = config.mount_hide_mode == "aggressive" ? ::sumhp::sumh::MountHideMode::Aggressive
                                                       : ::sumhp::sumh::MountHideMode::Normal;
    bool fallback = mode == ::sumhp::sumh::MountHideMode::Aggressive &&
                    !(caps.bitmask & SUMH_FEATURE_MOUNT_HIDE_AGGRESSIVE);
    if (fallback)
        mode = ::sumhp::sumh::MountHideMode::Normal;
    apply("mount_hide", ::sumhp::sumh::RuntimeFeature::MountHide, config.enable_mount_hide,
          SUMH_FEATURE_MOUNT_HIDE, static_cast<std::uint32_t>(mode), [&]() {
              bool ok = ::sumhp::sumh::set_mount_hide(config.enable_mount_hide, mode);
              if (!ok && mode == ::sumhp::sumh::MountHideMode::Aggressive && errno == EOPNOTSUPP) {
                  fallback = true;
                  ok = ::sumhp::sumh::set_mount_hide(config.enable_mount_hide,
                                                     ::sumhp::sumh::MountHideMode::Normal);
              }
              return ok;
          });
    if (fallback && report.outcomes.back().state == FeatureApplyState::Applied) {
        report.outcomes.back().detail = "aggressive mode unsupported; normal mode applied";
        mlog(report.outcomes.back().detail);
    }
    apply("statfs_spoof", ::sumhp::sumh::RuntimeFeature::StatfsSpoof, config.enable_statfs_spoof,
          SUMH_FEATURE_STATFS_SPOOF, 0,
          [&]() { return ::sumhp::sumh::set_statfs_spoof(config.enable_statfs_spoof); });

    FeatureApplyOutcome build;
    build.feature = "kernel_build_spoof";
    if (!apply_build) {
        build.state = FeatureApplyState::Deferred;
        build.detail = "waiting for configured boot stage";
    } else {
        build =
            apply_kernel_build_config({config.enable_kernel_build_spoof,
                                       config.kernel_build_release, config.kernel_build_version},
                                      caps);
    }
    report.outcomes.push_back(std::move(build));
    if (apply_maps) {
        FeatureApplyOutcome maps;
        std::string maps_error;
        (void)apply_maps_config(
            config, maps_error, apply_immediately || ksud::getprop("sys.boot_completed") == "1",
            &maps, apply_immediately ? RestoreTrigger::UserRequested : RestoreTrigger::Automatic);
        report.outcomes.push_back(std::move(maps));
    }
    return finish();
}

bool reset_feature_state(std::string& error) {
    user_hide_restore_pending = false;
    return disable_kernel_features(&error);
}

namespace {
bool restore_user_hide_rules(std::string& error, bool retry,
                             RestoreTrigger trigger = RestoreTrigger::Automatic) {
    error.clear();
    const RestoreContext context{ksud::is_safe_mode(), ksud::getprop("sys.boot_completed") == "1",
                                 ::sumhp::sumh::is_available(),
                                 !::sumhp::embedded_external_mount_owner().empty()};
    if (!should_restore(context, RestoreFeature::UserHide, trigger)) {
        user_hide_restore_pending = false;
        if (trigger == RestoreTrigger::Automatic)
            return true;
        error = context.kernel_available ? "user hide restore requires boot completion"
                                         : "SUMH is unavailable for user hide restore";
        return false;
    }
    // Hold the same definition lock as enable/disable until registration sync
    // finishes, so a delayed automatic restore cannot resurrect a disabled rule.
    ::sumhp::hide::Transaction definitions;
    if (!definitions.begin(error)) {
        user_hide_restore_pending = true;
        return false;
    }
    const auto paths = ::sumhp::hide::enabled_paths(definitions.stored().document);
    bool managed = false;
    if (!::sumhp::sumh::managed_hide_mode(managed)) {
        user_hide_restore_pending = true;
        error = "failed to query user hide ownership";
        return false;
    }
    if (managed) {
        // The kernel retries binding registered definitions. Failed queries or
        // mutations still need userspace to retry synchronizing those definitions.
        user_hide_restore_pending = true;
        std::vector<::sumhp::sumh::UserHideRule> registered;
        if (!::sumhp::sumh::user_hide_rules(registered)) {
            error = "failed to query managed user hide rules";
            return false;
        }
        const std::set<std::string> desired_paths(paths.begin(), paths.end());
        std::set<std::string> registered_paths;
        bool ok = true;
        for (const auto& rule : registered) {
            registered_paths.insert(rule.path);
            if (desired_paths.count(rule.path) == 0)
                ok = ::sumhp::sumh::delete_user_hide(rule.path, rule.id) && ok;
        }
        for (const auto& path : paths) {
            if (registered_paths.insert(path).second)
                ok = ::sumhp::sumh::upsert_user_hide(path) && ok;
        }
        user_hide_restore_pending = !ok;
        if (!ok)
            error = "failed to synchronize managed user hide rules; module rules remain active";
        return ok;
    }
    const int enabled = ::sumhp::sumh::enabled_state();
    if (enabled == 0) {
        user_hide_restore_pending = false;
        return true;
    }
    if (enabled < 0) {
        user_hide_restore_pending = true;
        error = "failed to query SUMH state before restoring user hide rules";
        if (!retry)
            mlog("sumh: " + error, logging::Level::Error);
        return false;
    }
    const bool ok = fsutil::run_in_init_mount_ns([retry, &paths]() {
        bool restored = true;
        for (const auto& path : paths) {
            struct stat parent = {};
            const bool parent_ready =
                !retry || (stat(fs::path(path).parent_path().c_str(), &parent) == 0 &&
                           S_ISDIR(parent.st_mode));
            const bool hidden = parent_ready && ::sumhp::sumh::hide_path(path);
            if (!retry)
                mlog("user hide path=" + path +
                         (hidden ? " ok" : " failed errno=" + std::to_string(errno)),
                     hidden ? logging::Level::Debug : logging::Level::Error);
            restored = hidden && restored;
        }
        return restored;
    });
    user_hide_restore_pending = !ok;
    if (!ok) {
        error = "user hide rules pending retry; module rules remain active";
        if (!retry)
            mlog("sumh: " + error, logging::Level::Error);
    } else if (retry) {
        mlog("sumh: pending user hide rules restored");
    }
    return ok;
}
}  // namespace

bool restore_persisted_hide_rules(std::string& error, RestoreTrigger trigger) {
    return restore_user_hide_rules(error, false, trigger);
}

bool has_pending_hide_rules() {
    return user_hide_restore_pending;
}

void retry_pending_hide_rules() {
    if (!user_hide_restore_pending)
        return;
    Config config;
    std::string error;
    if (!read_config_file(config, error))
        return;
    (void)restore_user_hide_rules(error, true);
}

bool reset_runtime_state(std::string& error) {
    // A negative legacy call may already have changed state. Stop rather than
    // compounding an uncertain feature reset with a destructive global clear.
    if (!reset_feature_state(error)) {
        const int failure = errno;
        invalidate_active_state();
        errno = failure;
        return false;
    }
    bool ok = true;
    if (!::sumhp::sumh::clear_rules()) {
        if (error.empty()) {
            error = "failed to clear SUMH rules";
        }
        ok = false;
    }
    const int failure = errno;
    invalidate_active_state();
    errno = failure;
    return ok;
}

bool prepare_modules_impl(const std::vector<ModuleEntry>& modules, const Config& config,
                          const ModuleRuleMap& rules, ModuleMountPlan& plan, std::string& error,
                          ModulePlanDiagnostics* diagnostics) {
    if (diagnostics)
        *diagnostics = {};
    ModuleMountPlan batch;
    PlannerTrace state;
    state.output = diagnostics;
    state.hidden = &batch.hide;
    auto* trace = diagnostics ? &state : nullptr;
    const auto fail = [&]() {
        if (trace) {
            auto event = entry_event(trace, trace->original_path, trace->resolved_path,
                                     trace->source, {}, "ERROR", "preparation_failed");
            trace_event(trace, std::move(event));
        }
        return false;
    };
    if (!::sumhp::sumh::is_available()) {
        error = "SUMH backend requested but protocol is unavailable";
        return fail();
    }
    const auto partitions = fsutil::managed_partitions(config.partitions);
    ModuleRuleMap resolved_rules;
    for (const auto& module : modules) {
        if (trace)
            trace->module_id = module.id;
        const auto rule_it = rules.find(module.id);
        if (rule_it == rules.end())
            continue;
        auto& module_rules = resolved_rules[module.id];
        if (!resolve_module_rules(rule_it->second, module_rules, error, trace)) {
            error.insert(0, ": ");
            error.insert(0, module.id);
            return fail();
        }
        for (const auto& rule : module_rules) {
            if (rule.mode == "hide") {
                if (!batch.hide.count(rule.path) && !trace_action_available(trace, batch, error))
                    return fail();
                batch.hide.insert(rule.path);
                trace_entry(trace, rule.path, rule.path, "", module_rules, "HIDE",
                            "explicit_definition");
            }
        }
    }
    // Kernel rules use last-write-wins semantics. Enumerated modules are sorted
    // by id, so emit them backwards to retain the existing module priority.
    for (auto it = modules.rbegin(); it != modules.rend(); ++it) {
        if (trace)
            trace->module_id = it->id;
        const auto& module_rules = resolved_rules[it->id];
        for (const auto& partition : partitions) {
            if (!compile_tree(it->path / partition, "/" + partition, module_rules, batch, error,
                              trace)) {
                error.insert(0, ": ");
                error.insert(0, it->id);
                return fail();
            }
        }
    }
    if (!trace_budget_available(trace, error) || !trace_final_hidden(trace, batch, error))
        return fail();
    plan = std::move(batch);
    if (diagnostics)
        diagnostics->complete =
            !diagnostics->events_truncated && !diagnostics->observations_truncated;
    error.clear();
    return true;
}

bool prepare_modules(const std::vector<ModuleEntry>& modules, const Config& config,
                     const ModuleRuleMap& rules, ModuleMountPlan& plan, std::string& error) {
    return prepare_modules_impl(modules, config, rules, plan, error, nullptr);
}

bool resolve_module_plan_path(const std::string& value, std::string& resolved, std::string& error) {
    return resolve_virtual_path(value, resolved, error, true);
}

bool prepare_modules_diagnostic(const std::vector<ModuleEntry>& modules, const Config& config,
                                const ModuleRuleMap& rules, ModuleMountPlan& plan,
                                ModulePlanDiagnostics& diagnostics, std::string& error) {
    return prepare_modules_impl(modules, config, rules, plan, error, &diagnostics);
}

bool apply_modules(const ModuleMountPlan& batch) {
    bool ok = true;
    size_t add_count = 0;
    size_t merge_count = 0;
    for (const auto& rule : batch.mappings) {
        const bool added = rule.merge ? ::sumhp::sumh::add_merge_rule(rule.path, rule.source)
                                      : ::sumhp::sumh::add_rule(rule.path, rule.source, 0);
        const int failure = errno;
        rule.merge ? ++merge_count : ++add_count;
        mlog(std::string(rule.merge ? "merge" : "add") + " rule target=" + rule.path + " source=" +
                 rule.source + (added ? " ok" : " failed errno=" + std::to_string(failure)),
             added ? logging::Level::Debug : logging::Level::Error);
        if (!added) {
            invalidate_active_state();
            errno = failure;
            return false;
        }
    }
    for (const auto& path : batch.hide) {
        const bool hidden = ::sumhp::sumh::hide_path(path);
        const int failure = errno;
        mlog("hide path=" + path + (hidden ? " ok" : " failed errno=" + std::to_string(failure)),
             hidden ? logging::Level::Debug : logging::Level::Error);
        if (!hidden) {
            invalidate_active_state();
            errno = failure;
            return false;
        }
    }
    std::error_code ec;
    fs::create_directories(active_file().parent_path(), ec);
    if (ok && !write_boot_marker(active_file())) {
        ok = false;
        mlog("sumh: failed to record restored runtime state", logging::Level::Error);
    }
    if (ok) {
        mlog("sumh: installed add=" + std::to_string(add_count) + " merge=" +
             std::to_string(merge_count) + " hide=" + std::to_string(batch.hide.size()));
    } else {
        fs::remove(active_file(), ec);
        mlog("sumh: one or more rule operations failed", logging::Level::Error);
    }
    return ok;
}

bool mount_modules(const std::vector<ModuleEntry>& modules, const Config& config,
                   const ModuleRuleMap& rules) {
    ModuleMountPlan plan;
    std::string error;
    if (!prepare_modules(modules, config, rules, plan, error)) {
        mlog("sumh: " + error, logging::Level::Error);
        return false;
    }
    return apply_modules(plan);
}

bool unmount_all(const Config& config) {
    (void)config;  // shared storage is released centrally after OverlayFS too.
    bool ok = true;
    if (::sumhp::sumh::is_available()) {
        std::string error;
        ok = reset_runtime_state(error);
        if (!ok) {
            mlog("sumh: " + error);
        }
    }
    std::error_code ec;
    fs::remove(active_file(), ec);
    clear_replayable_mappings();
    return ok;
}

bool rebuild_modules_in_init_ns(const std::vector<ModuleEntry>& modules, const Config& config,
                                const ModuleRuleMap& rules, ModuleRebuildOutcome& outcome,
                                std::string& error) {
    outcome = ModuleRebuildOutcome::Unchanged;
    if (!module_rebuild_allowed(error))
        return false;
    class SharedOutcome {
        ModuleRebuildOutcome* value = nullptr;

    public:
        SharedOutcome(const SharedOutcome&) = delete;
        SharedOutcome& operator=(const SharedOutcome&) = delete;
        SharedOutcome(SharedOutcome&&) = delete;
        SharedOutcome& operator=(SharedOutcome&&) = delete;
        [[nodiscard]] ModuleRebuildOutcome* get() const { return value; }
        SharedOutcome() {
            void* allocation = ::mmap(nullptr, sizeof(ModuleRebuildOutcome), PROT_READ | PROT_WRITE,
                                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
            if (allocation != MAP_FAILED) {
                value = static_cast<ModuleRebuildOutcome*>(allocation);
                *value = ModuleRebuildOutcome::Unchanged;
            }
        }
        ~SharedOutcome() {
            if (value)
                ::munmap(value, sizeof(ModuleRebuildOutcome));
        }
    } const shared;
    if (!shared.get()) {
        error = "cannot allocate module rebuild outcome channel";
        return false;
    }
    fsutil::NamespaceOperationState child = fsutil::NamespaceOperationState::NotStarted;
    const bool ok = fsutil::run_in_init_mount_ns(
        [&] {
            ModuleMountPlan plan;
            if (!prepare_modules(modules, config, rules, plan, error))
                return false;
            std::vector<std::string> ids;
            ids.reserve(modules.size());
            for (const auto& module : modules)
                ids.push_back(module.id);
            const bool applied = rebuild_modules_recorded(
                plan, ids, reset_runtime_state,
                [](const ModuleMountPlan::Mapping& rule) {
                    const bool added = rule.merge
                                           ? ::sumhp::sumh::add_merge_rule(rule.path, rule.source)
                                           : ::sumhp::sumh::add_rule(rule.path, rule.source, 0);
                    const int failure = errno;
                    mlog(std::string(rule.merge ? "merge" : "add") + " rule target=" + rule.path +
                             " source=" + rule.source +
                             (added ? " acknowledged"
                                    : " partial_or_unknown errno=" + std::to_string(failure)),
                         added ? logging::Level::Debug : logging::Level::Error);
                    errno = failure;
                    return added;
                },
                [](const std::string& path) {
                    const bool hidden = ::sumhp::sumh::hide_path(path);
                    const int failure = errno;
                    mlog("hide path=" + path +
                             (hidden ? " acknowledged"
                                     : " partial_or_unknown errno=" + std::to_string(failure)),
                         hidden ? logging::Level::Debug : logging::Level::Error);
                    errno = failure;
                    return hidden;
                },
                *shared.get(), error);
            if (!applied)
                return false;
            // Failure to publish controller metadata is not authority to clear
            // mappings that the kernel already acknowledged.
            std::error_code ec;
            fs::create_directories(active_file().parent_path(), ec);
            if (ec || !write_boot_marker(active_file())) {
                error = "module rules acknowledged; active marker persistence unconfirmed";
                errno = EIO;
                return false;
            }
            return true;
        },
        error, &child);
    if (child == fsutil::NamespaceOperationState::Completed ||
        child == fsutil::NamespaceOperationState::CallbackFailed)
        outcome = *shared.get();
    else if (child != fsutil::NamespaceOperationState::NotStarted)
        outcome = ModuleRebuildOutcome::PartialOrUnknown;
    return ok;
}

bool is_active() {
    return marker_matches_current_boot(active_file());
}

void invalidate_active_state() {
    user_hide_restore_pending = false;
    std::error_code ec;
    fs::remove(active_file(), ec);
}

std::vector<std::string> replayable_module_ids() {
    std::vector<std::string> ids;
    if (!marker_matches_current_boot(mapping_plan_file())) {
        return ids;
    }
    std::ifstream in(mapping_plan_file());
    std::string line;
    std::getline(in, line);  // boot id
    while (std::getline(in, line)) {
        if (!line.empty()) {
            ids.push_back(line);
        }
    }
    return ids;
}

bool has_replayable_mappings() {
    return !replayable_module_ids().empty();
}

bool record_replayable_mappings(const std::vector<ModuleEntry>& modules) {
    if (modules.empty()) {
        clear_replayable_mappings();
        return true;
    }
    std::ostringstream ids;
    for (const auto& module : modules) {
        ids << module.id << "\n";
    }
    if (write_boot_marker(mapping_plan_file(), ids.str())) {
        return true;
    }
    clear_replayable_mappings();
    return false;
}

void clear_replayable_mappings() {
    std::error_code ec;
    fs::remove(mapping_plan_file(), ec);
}

}  // namespace sumhp::mount::sumh
