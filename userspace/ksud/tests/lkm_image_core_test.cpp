#include "../src/boot/lkm_image_core.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

namespace lkm = ksud::boot::lkm_image;

namespace {

std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

void put_u16(std::vector<std::uint8_t>& data, std::size_t offset, std::uint16_t value) {
    data[offset] = static_cast<std::uint8_t>(value);
    data[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put_u32(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        data[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

void put_u64(std::vector<std::uint8_t>& data, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        data[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

std::size_t append_aligned(std::vector<std::uint8_t>& image, std::size_t& position,
                           const std::vector<std::uint8_t>& bytes) {
    position = align_up(position, 8);
    const std::size_t offset = position;
    std::copy(bytes.begin(), bytes.end(), image.begin() + position);
    position += bytes.size();
    return offset;
}

std::vector<std::uint8_t> to_u32_bytes(std::uint32_t value) {
    std::vector<std::uint8_t> result(4);
    put_u32(result, 0, value);
    return result;
}

std::vector<std::uint8_t> to_u64_bytes(std::uint64_t value) {
    std::vector<std::uint8_t> result(8);
    put_u64(result, 0, value);
    return result;
}

struct FixtureSymbol {
    std::uint64_t address;
    std::uint8_t kind;
    std::string name;
};

std::pair<std::vector<std::uint8_t>, std::uint64_t> build_kallsyms_fixture(
    const std::string& layout, const std::vector<std::uint8_t>* first_token = nullptr,
    bool corrupt_index = false) {
    constexpr std::uint64_t kBase = 0xffffffc008000000ULL;
    constexpr std::size_t kImageSize = 0x200000;
    constexpr std::size_t kCount = 2049;

    std::vector<FixtureSymbol> entries{
        {kBase, 'T', "_text"},
        {kBase + 0x1000, 't', "load_module"},
    };
    for (std::size_t index = 0; index < kCount - 3; ++index) {
        entries.push_back({kBase + 0x2000 + (index * 0x20), 't',
                           "fixture_symbol_" + std::to_string(10000 + index)});
    }
    entries.push_back({kBase + kImageSize, 'B', "_end"});

    std::vector<std::uint8_t> token_table;
    std::vector<std::uint16_t> token_offsets;
    for (std::size_t value = 0; value <= 0xff; ++value) {
        token_offsets.push_back(static_cast<std::uint16_t>(token_table.size()));
        if (value == 0 && first_token) {
            token_table.insert(token_table.end(), first_token->begin(), first_token->end());
        } else {
            token_table.push_back(value >= 0x20 && value <= 0x7e ? static_cast<std::uint8_t>(value)
                                                                 : static_cast<std::uint8_t>('x'));
        }
        token_table.push_back(0);
    }
    std::vector<std::uint8_t> token_index(512);
    for (std::size_t index = 0; index < token_offsets.size(); ++index) {
        put_u16(token_index, index * 2, token_offsets[index]);
    }
    if (corrupt_index)
        put_u16(token_index, 0, 1);

    std::vector<std::uint8_t> names;
    std::vector<std::uint32_t> markers;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (index % 256 == 0) {
            markers.push_back(static_cast<std::uint32_t>(names.size()));
        }
        const FixtureSymbol& entry = entries[index];
        names.push_back(static_cast<std::uint8_t>(entry.name.size() + 1));
        names.push_back(entry.kind);
        names.insert(names.end(), entry.name.begin(), entry.name.end());
    }
    std::vector<std::uint8_t> marker_bytes(markers.size() * 4);
    for (std::size_t index = 0; index < markers.size(); ++index) {
        put_u32(marker_bytes, index * 4, markers[index]);
    }
    std::vector<std::uint8_t> offsets(entries.size() * 4);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        put_u32(offsets, index * 4, static_cast<std::uint32_t>(entries[index].address - kBase));
    }
    std::vector<std::uint8_t> sequences(entries.size() * 3);
    for (std::size_t index = 0; index < sequences.size(); ++index) {
        sequences[index] = static_cast<std::uint8_t>((index % 251) + 1);
    }

    std::vector<std::uint8_t> image(kImageSize);
    put_u64(image, 0x10, kImageSize);
    std::copy_n(reinterpret_cast<const std::uint8_t*>("ARM\x64"), 4, image.begin() + 0x38);
    std::size_t position = 0x10000;
    if (layout == "pre-6.4") {
        append_aligned(image, position, offsets);
        append_aligned(image, position, to_u64_bytes(kBase));
        append_aligned(image, position, to_u32_bytes(kCount));
        append_aligned(image, position, names);
        append_aligned(image, position, marker_bytes);
        append_aligned(image, position, sequences);
        append_aligned(image, position, token_table);
        append_aligned(image, position, token_index);
    } else {
        append_aligned(image, position, to_u32_bytes(kCount));
        append_aligned(image, position, names);
        append_aligned(image, position, marker_bytes);
        append_aligned(image, position, token_table);
        append_aligned(image, position, token_index);
        append_aligned(image, position, offsets);
        append_aligned(image, position, to_u64_bytes(kBase));
    }
    return {std::move(image), kBase};
}

void test_boot_v4_repack() {
    constexpr std::size_t kPage = 4096;
    constexpr std::size_t kKernelSize = 100;
    constexpr std::size_t kRamdiskSize = 200;
    constexpr std::size_t kSignatureSize = 64;
    const std::size_t ramdisk_offset = align_up(kPage + kKernelSize, kPage);
    const std::size_t signature_offset = align_up(ramdisk_offset + kRamdiskSize, kPage);
    const std::size_t payload_end = align_up(signature_offset + kSignatureSize, kPage);
    std::vector<std::uint8_t> boot(payload_end + 17, 0);
    std::copy_n(reinterpret_cast<const std::uint8_t*>("ANDROID!"), 8, boot.begin());
    put_u32(boot, 8, kKernelSize);
    put_u32(boot, 12, kRamdiskSize);
    put_u32(boot, 20, 1584);
    put_u32(boot, 40, 4);
    put_u32(boot, 1580, kSignatureSize);
    std::fill(boot.begin() + kPage, boot.begin() + kPage + kKernelSize, 0x11);
    std::fill(boot.begin() + ramdisk_offset, boot.begin() + ramdisk_offset + kRamdiskSize, 0x22);
    std::fill(boot.begin() + signature_offset, boot.begin() + signature_offset + kSignatureSize,
              0x33);
    std::fill(boot.begin() + payload_end, boot.end(), 0x44);

    auto info = lkm::parse_boot_image(boot.data(), boot.size());
    assert(info);
    assert(info.value().kind == lkm::BootHeaderKind::kV4);
    assert(info.value().kernel.offset == kPage);
    assert(info.value().payload_end == payload_end);

    std::vector<std::uint8_t> replacement(5000, 0x55);
    auto repacked =
        lkm::replace_boot_kernel(boot.data(), boot.size(), replacement.data(), replacement.size());
    assert(repacked);
    auto repacked_info = lkm::parse_boot_image(repacked.value().data(), repacked.value().size());
    assert(repacked_info);
    assert(repacked_info.value().kernel.size == replacement.size());
    assert(repacked.value()[repacked_info.value().ramdisk.offset] == 0x22);
    assert(repacked.value()[repacked_info.value().signature.offset] == 0x33);
    assert(repacked.value().back() == 0x44);
}

void test_boot_v1_recovery_offset_moves() {
    constexpr std::size_t kPage = 2048;
    constexpr std::size_t kKernelSize = 100;
    constexpr std::size_t kRamdiskSize = 100;
    constexpr std::size_t kRecoverySize = 64;
    const std::size_t recovery_offset =
        align_up(align_up(align_up(kPage + kKernelSize, kPage) + kRamdiskSize, kPage), kPage);
    const std::size_t payload_end = align_up(recovery_offset + kRecoverySize, kPage);
    std::vector<std::uint8_t> boot(payload_end, 0);
    std::copy_n(reinterpret_cast<const std::uint8_t*>("ANDROID!"), 8, boot.begin());
    put_u32(boot, 8, kKernelSize);
    put_u32(boot, 12, kRamdiskSize);
    put_u32(boot, 36, kPage);
    put_u32(boot, 40, 1);
    put_u32(boot, 1644, 1648);
    put_u32(boot, 1632, kRecoverySize);
    put_u64(boot, 1636, recovery_offset);
    std::fill(boot.begin() + recovery_offset, boot.begin() + recovery_offset + kRecoverySize, 0x77);

    auto info = lkm::parse_boot_image(boot);
    assert(info && info.value().recovery_dtbo_offset == recovery_offset);
    const std::vector<std::uint8_t> replacement(3000, 0x66);
    auto repacked = lkm::replace_boot_kernel(boot, replacement);
    assert(repacked);
    auto updated = lkm::parse_boot_image(repacked.value());
    assert(updated);
    assert(updated.value().recovery_dtbo_offset == recovery_offset + 2048);
    assert(repacked.value()[updated.value().recovery_dtbo.offset] == 0x77);
}

void test_checked_arithmetic() {
    assert(!lkm::checked_add(static_cast<std::size_t>(-1), 1));
    assert(!lkm::checked_mul(static_cast<std::size_t>(-1), 2));
    assert(!lkm::checked_align_up(3, 3));
    assert(!lkm::checked_range(4, 3, 2));
}

void test_kallsyms_layout(const std::string& layout) {
    auto fixture = build_kallsyms_fixture(layout);
    auto recovered = lkm::recover_arm64_kallsyms(fixture.first.data(), fixture.first.size());
    if (!recovered) {
        std::cerr << recovered.error().message << '\n';
    }
    assert(recovered);
    assert(recovered.value().layout == layout);
    assert(recovered.value().count == 2049);
    auto text = recovered.value().symbols.resolve("_text");
    assert(text && text.value().address == fixture.second);
    auto normalized = recovered.value().symbols.resolve("fixture_symbol_11024.llvm.1");
    assert(normalized);
    assert(normalized.value().address == fixture.second + 0x2000 + (1024ULL * 0x20));
}

void test_kallsyms_token_bounds() {
    for (const auto* layout : {"pre-6.4", "6.4+"}) {
        for (const std::size_t length : {0, 257, 512, 513}) {
            const std::vector<std::uint8_t> token(length, 'a');
            auto fixture = build_kallsyms_fixture(layout, &token);
            auto recovered =
                lkm::recover_arm64_kallsyms(fixture.first.data(), fixture.first.size());
            assert(static_cast<bool>(recovered) == (length <= 512));
            if (recovered) {
                assert(recovered.value().count == 2049);
                assert(recovered.value().symbols.resolve("_text").value().address ==
                       fixture.second);
            }
        }
        for (const auto& token : {std::vector<std::uint8_t>{0x80}, std::vector<std::uint8_t>{}}) {
            auto fixture = build_kallsyms_fixture(layout, &token, token.empty());
            assert(!lkm::recover_arm64_kallsyms(fixture.first.data(), fixture.first.size()));
        }
    }
}

}  // namespace

int main() {
    try {
        test_boot_v4_repack();
        test_boot_v1_recovery_offset_moves();
        test_checked_arithmetic();
        test_kallsyms_layout("pre-6.4");
        test_kallsyms_layout("6.4+");
        test_kallsyms_token_bounds();
        return 0;
    } catch (...) {
        return 1;
    }
}
