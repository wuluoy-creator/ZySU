#pragma once

#include <cstddef>
#include "../../../../common/module_preview_request.hpp"
#include "core/json.hpp"
#include "mount/backend.hpp"

namespace sumhp::mount {
inline constexpr std::size_t MODULE_PREVIEW_INPUT_BYTES = module_preview::input_bytes;
inline constexpr std::size_t MODULE_PREVIEW_RESPONSE_BYTES = module_preview::response_bytes;

struct ModulePreviewRequest {
    std::string explain_path;
    std::string input_path;  // empty = saved only; '-' = bounded stdin input
    std::optional<std::string> inline_input =
        std::nullopt;  // private channel; exclusive with input_path
};
// args exclude the outer `sumhp` word. Only the exact routes are supported.
bool is_module_preview_prefix(const std::vector<std::string>& args);
bool parse_module_preview_arguments(const std::vector<std::string>& args,
                                    ModulePreviewRequest& request, std::string& error);
// Pure candidate validation/application to copies; never persistence or mount.
bool apply_module_preview_input(const std::string& bytes, Config& config, ModuleModeMap& modes,
                                ModuleRuleMap& rules, std::vector<std::string>& changed_ids,
                                std::string& error);
std::string module_preview_json(const ModulePreviewRequest& request);
}  // namespace sumhp::mount
