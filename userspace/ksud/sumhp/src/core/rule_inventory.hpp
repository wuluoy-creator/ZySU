#pragma once

#include <cerrno>
#include <charconv>
#include <ostream>
#include <sstream>
#include "core/json.hpp"
#include "sumhp/sumh_client.hpp"
#include "uapi/sumh.h"

namespace sumhp::inventory {

inline const char* category_name(std::uint32_t category) {
    static constexpr const char* names[] = {
        "metadata", "mapping",   "legacy_hide",   "merge", "maps_managed", "maps_temporary",
        "inject",   "user_hide", "overlay_xattr", "kstat", "vnode"};
    return category <= SUMH_INVENTORY_VNODE ? names[category] : "unknown";
}

// Linux filenames are byte strings. Invalid UTF-8 is encoded losslessly, not
// replaced with U+FFFD or copied into an invalid JSON response.
inline bool valid_utf8(const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto first = static_cast<unsigned char>(bytes[offset++]);
        if (first < 0x80)
            continue;
        unsigned length;
        std::uint32_t value, minimum;
        if (first >= 0xc2 && first <= 0xdf) {
            length = 1;
            value = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 2;
            value = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 3;
            value = first & 7;
            minimum = 0x10000;
        } else
            return false;
        if (length > bytes.size() - offset)
            return false;
        while (length--) {
            const auto next = static_cast<unsigned char>(bytes[offset++]);
            if ((next & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}

inline std::string bytes_json(const std::string& bytes) {
    const bool utf8 = valid_utf8(bytes);
    std::string encoded;
    if (!utf8) {
        static constexpr char hex[] = "0123456789abcdef";
        encoded.reserve(bytes.size() * 2);
        for (unsigned char byte : bytes) {
            encoded += hex[byte >> 4];
            encoded += hex[byte & 15];
        }
    }
    return std::string("{\"encoding\":") + json_quote(utf8 ? "utf-8" : "hex") +
           ",\"value\":" + json_quote(utf8 ? bytes : encoded) +
           ",\"byte_length\":" + std::to_string(bytes.size()) + "}";
}

inline bool parse_request(const std::vector<std::string>& args, sumh::InventoryRequest& request) {
    if (args.size() != 2 && args.size() != 3 && args.size() != 6)
        return false;
    sumh::InventoryRequest parsed;
    if (args.size() >= 3) {
        bool found = false;
        for (std::uint32_t category = 0; category <= SUMH_INVENTORY_VNODE; ++category) {
            if (args[2] == category_name(category)) {
                parsed.category = category;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    if (args.size() == 6) {
        const auto number = [](const std::string& text, std::uint64_t& value) {
            if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
                return false;
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            return result.ec == std::errc{} && result.ptr == text.data() + text.size();
        };
        if (!number(args[3], parsed.instance_id) || !number(args[4], parsed.generation) ||
            !number(args[5], parsed.cursor) || !parsed.instance_id || !parsed.generation ||
            (!parsed.category && parsed.cursor))
            return false;
    }
    request = parsed;
    return true;
}

inline void print_entry(std::ostream& out, const sumh::InventoryEntry& entry) {
    out << "{\"category\":" << json_quote(category_name(entry.category)) << ",\"owner\":"
        << json_quote(entry.owner == SUMH_INVENTORY_OWNER_USER_HIDE
                          ? "user_hide"
                          : (entry.owner == SUMH_INVENTORY_OWNER_MANAGED_MAPS ? "managed_maps"
                                                                              : "unattributed"))
        << ",\"id_kind\":"
        << json_quote(entry.id_kind == SUMH_INVENTORY_ID_MANAGED_DEFINITION ? "managed_definition"
                                                                            : "generation_ordinal")
        << ",\"id\":" << json_quote(std::to_string(entry.id))
        << ",\"definition_generation\":" << json_quote(std::to_string(entry.definition_generation))
        << ",\"mutation_handle\":false";
    switch (entry.category) {
    case SUMH_INVENTORY_MAPPING:
    case SUMH_INVENTORY_MERGE:
        out << ",\"visible_path\":" << bytes_json(entry.path1)
            << ",\"backing_path\":" << bytes_json(entry.path2);
        if (entry.category == SUMH_INVENTORY_MAPPING)
            out << ",\"type\":" << entry.type;
        break;
    case SUMH_INVENTORY_MAPS_MANAGED:
    case SUMH_INVENTORY_MAPS_TEMPORARY:
        out << ",\"target_ino\":" << json_quote(std::to_string(entry.target_ino))
            << ",\"target_dev\":" << json_quote(std::to_string(entry.target_dev))
            << ",\"spoofed_ino\":" << json_quote(std::to_string(entry.spoofed_ino))
            << ",\"spoofed_dev\":" << json_quote(std::to_string(entry.spoofed_dev))
            << ",\"spoofed_path\":" << bytes_json(entry.path2);
        break;
    default:
        out << ",\"path\":" << bytes_json(entry.path1);
        if (entry.category == SUMH_INVENTORY_LEGACY_HIDE)
            out << ",\"storage_managed\":" << (entry.flags ? "true" : "false");
    }
    out << '}';
}

// One bounded page; collection budgets and cancellation belong to the caller.
// No automatic retry, restore, normalize, or mutation is performed here.
inline int command(const std::vector<std::string>& args, const std::string& boot_id,
                   std::ostream& out, bool legacy_fallback = true) {
    sumh::InventoryRequest request;
    sumh::InventoryPage page;
    const char* status = "error";
    const char* reason = "invalid_request";
    int error = EINVAL;
    bool legacy_read = false;
    std::string legacy;
    int legacy_error = 0;
    if (parse_request(args, request)) {
        const auto version = sumh::version_info();
        if (version.status != sumh::Status::Available) {
            status = "unavailable";
            reason = version.status == sumh::Status::NotPresent ? "transport_unavailable"
                                                                : "protocol_mismatch";
            error = version.last_errno ? version.last_errno : EPROTONOSUPPORT;
        } else {
            const auto caps = sumh::feature_capabilities();
            if (!caps.ok) {
                reason = "capability_query_failed";
                error = caps.last_errno ? caps.last_errno : EIO;
            } else if (!(caps.bitmask & SUMH_FEATURE_RULE_INVENTORY)) {
                status = "unsupported";
                reason = "rule_inventory_not_advertised";
                error = 0;
                if (legacy_fallback) {
                    errno = 0;
                    legacy = sumh::active_rules();
                    legacy_error = errno;
                    legacy_read = true;
                }
            } else if (!sumh::rule_inventory(request, page)) {
                error = errno ? errno : EIO;
                status =
                    error == ESTALE ? "stale" : (error == EOPNOTSUPP ? "unsupported" : "error");
                reason = error == ESTALE ? "snapshot_changed"
                                         : (error == EOPNOTSUPP ? "category_not_supported"
                                                                : "inventory_query_failed");
            } else {
                status = "known";
                reason = "registered_definitions";
                error = 0;
            }
        }
    }
    const bool known = std::string(status) == "known";
    out << "{\"schema_version\":1,\"status\":" << json_quote(status)
        << ",\"reason\":" << json_quote(reason) << ",\"errno\":" << error
        << ",\"source\":\"kernel_ioctl\",\"observation\":\"registered_definitions_only\""
        << ",\"boot_id\":" << (boot_id.empty() ? "null" : json_quote(boot_id))
        << ",\"category\":" << json_quote(category_name(request.category))
        << ",\"page_capacity\":1,\"complete\":"
        << (known && page.category && !page.cursor && page.at_end ? "true" : "false")
        << ",\"complete_scope\":\"selected_category_from_cursor_zero\",\"page\":";
    if (!known)
        out << "null";
    else {
        out << "{\"instance_id\":" << json_quote(std::to_string(page.instance_id))
            << ",\"generation\":" << json_quote(std::to_string(page.generation))
            << ",\"cursor\":" << json_quote(std::to_string(page.cursor))
            << ",\"next_cursor\":" << json_quote(std::to_string(page.next_cursor))
            << ",\"at_end\":" << (page.at_end ? "true" : "false")
            << ",\"consistency\":\"definitions_coherent\",\"supported_categories\":"
            << page.supported_categories << ",\"entries\":[";
        if (!page.entries.empty())
            print_entry(out, page.entries.front());
        out << "]}";
    }
    out << ",\"legacy\":";
    if (!legacy_read)
        out << "null";
    else
        out << "{\"complete\":false,\"scope\":\"unstructured_legacy_text\",\"errno\":"
            << legacy_error << ",\"raw\":" << (legacy_error ? "null" : bytes_json(legacy)) << '}';
    out << "}\n";
    out.flush();
    if (!out.good()) {
        errno = EIO;
        return 1;
    }
    errno = error;
    return known || (std::string(status) == "unsupported" && !error) ? 0 : 1;
}

}  // namespace sumhp::inventory
