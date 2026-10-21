#pragma once

#include <cstdint>
#include <string>

namespace ksud::text {
inline bool valid_utf8(const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto first = static_cast<unsigned char>(bytes[offset++]);
        if (first < 0x80)
            continue;
        unsigned length;
        std::uint32_t value;
        std::uint32_t minimum;
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
}  // namespace ksud::text
