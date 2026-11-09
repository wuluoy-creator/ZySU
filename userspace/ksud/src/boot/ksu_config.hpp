#pragma once

#include "../../../../uapi/boot_config.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ksud {

// Shared by ramdisk patching and ZySU's embedded direct-LKM config. Parse before
// touching an image; never silently truncate names or module parameters.
struct KsuConfigArgs {
    bool allow_shell = false;
    bool no_custom_rc = false;
    bool no_preinit = false;
    std::map<uint32_t, uint64_t> forced_features;
    std::vector<std::string> blocked_modules;

    bool add_force_feature(std::string_view entry) {
        const auto sep = entry.find('=');
        if (sep == std::string_view::npos || sep == 0 || sep + 1 == entry.size())
            return false;
        const auto name = entry.substr(0, sep);
        uint32_t id = 0;
        bool known = false;
#define KSU_MATCH_FEATURE(feature_id, feature_name)                   \
    if (name == feature_name || name == std::to_string(feature_id)) { \
        id = feature_id;                                              \
        known = true;                                                 \
    }
        KSU_BOOT_CONFIGURABLE_FEATURES(KSU_MATCH_FEATURE)
#undef KSU_MATCH_FEATURE
        if (!known)
            return false;
        auto number = entry.substr(sep + 1);
        int base = 10;
        if (number.size() > 2 && number[0] == '0' && (number[1] == 'x' || number[1] == 'X')) {
            base = 16;
            number.remove_prefix(2);
        }
        uint64_t value = 0;
        const auto result =
            std::from_chars(number.data(), number.data() + number.size(), value, base);
        if (result.ec != std::errc{} || result.ptr != number.data() + number.size())
            return false;
        auto previous = forced_features;
        forced_features[id] = value;
        if (force_features().size() >= KSU_FORCE_FEATURES_SIZE) {
            forced_features = std::move(previous);
            return false;
        }
        return true;
    }

    bool add_block_modules(std::string_view entry) {
        auto candidate = blocked_modules;
        do {
            const auto sep = entry.find(',');
            auto name = entry.substr(0, sep);
            while (!name.empty() && name.front() == ' ')
                name.remove_prefix(1);
            while (!name.empty() && name.back() == ' ')
                name.remove_suffix(1);
            if (name.empty() || name.size() > KSU_BLOCK_MODULE_NAME_MAX ||
                !std::all_of(name.begin(), name.end(), [](char c) {
                    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_';
                }))
                return false;
            const std::string module(name);
            if (std::find(candidate.begin(), candidate.end(), module) == candidate.end())
                candidate.push_back(module);
            if (sep == std::string_view::npos)
                break;
            entry.remove_prefix(sep + 1);
        } while (true);
        if (join_modules(candidate).size() >= KSU_BLOCK_MODULES_SIZE)
            return false;
        blocked_modules = std::move(candidate);
        return true;
    }

    std::string force_features() const {
        std::string result;
        for (const auto& [id, value] : forced_features) {
            if (!result.empty())
                result += ',';
            result += std::to_string(id) + ':' + std::to_string(value);
        }
        return result;
    }

    std::string block_modules() const { return join_modules(blocked_modules); }

    std::string module_params(bool bundled = false) const {
        std::string result;
        const auto append = [&result](const std::string& param) {
            if (!result.empty())
                result += ' ';
            result += param;
        };
        if (bundled)
            append("bundled=1");
        if (allow_shell)
            append("allow_shell=1");
        if (no_custom_rc)
            append("norc=1");
        if (no_preinit)
            append("preinit=0");
        if (!forced_features.empty())
            append("force_feature=" + force_features());
        if (!blocked_modules.empty())
            append("block_modules=" + block_modules());
        return result;
    }

private:
    static std::string join_modules(const std::vector<std::string>& modules) {
        std::string result;
        for (const auto& name : modules) {
            if (!result.empty())
                result += ',';
            result += name;
        }
        return result;
    }
};

}  // namespace ksud
