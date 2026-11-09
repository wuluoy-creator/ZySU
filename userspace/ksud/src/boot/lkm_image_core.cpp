#include "lkm_image_core.hpp"
#include "../utils.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <unordered_set>

namespace ksud::boot::lkm_image {
namespace {

constexpr std::array<std::uint8_t, 8> kBootMagic = {'A', 'N', 'D', 'R', 'O', 'I', 'D', '!'};
constexpr std::size_t kLegacyHeaderSize = 1632;
constexpr std::size_t kV1HeaderSize = 1648;
constexpr std::size_t kV2HeaderSize = 1660;
constexpr std::size_t kModernV3HeaderSize = 1580;
constexpr std::size_t kModernV4HeaderSize = 1584;
constexpr std::size_t kModernPageSize = 4096;
constexpr std::size_t kKernelSizeOffset = 8;
constexpr std::size_t kLegacyPageSizeOffset = 36;
constexpr std::size_t kHeaderVersionOffset = 40;
constexpr std::size_t kModernHeaderSizeOffset = 20;
constexpr std::size_t kV1RecoveryDtboSizeOffset = 1632;
constexpr std::size_t kV1RecoveryDtboOffsetOffset = 1636;
constexpr std::size_t kV2DtbSizeOffset = 1648;
constexpr std::size_t kV4SignatureSizeOffset = 1580;

constexpr std::size_t kArm64ImageHeaderSize = 64;
constexpr std::size_t kArm64TextOffsetOffset = 8;
constexpr std::size_t kArm64ImageSizeOffset = 16;
constexpr std::size_t kArm64FlagsOffset = 24;
constexpr std::size_t kArm64MagicOffset = 56;
constexpr std::array<std::uint8_t, 4> kArm64Magic = {'A', 'R', 'M', 0x64};

constexpr std::size_t kKallsymsAlignment = 8;
constexpr std::size_t kKallsymsTokenCount = 256;
constexpr std::size_t kKallsymsTokenIndexSize = kKallsymsTokenCount * 2;
constexpr std::size_t kKallsymsMaxTokenLength = 512;
constexpr std::size_t kKallsymsMarkerSearchWindow = std::size_t{4} * 1024 * 1024;
constexpr std::size_t kKallsymsNameTailSearch = 0x40000;
constexpr std::size_t kKallsymsMinMarkers = 8;
constexpr std::size_t kKallsymsMaxMarkers = 4096;

std::string hex_value(std::size_t value) {
    std::string text;
    text.reserve(2 + (sizeof(value) * 2));
    append_hex(&text, value);
    return text;
}

bool is_power_of_two(std::size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

bool all_zero(const std::uint8_t* data, std::size_t begin, std::size_t end) {
    return begin <= end &&
           std::all_of(data + begin, data + end, [](std::uint8_t byte) { return byte == 0; });
}

Result<std::size_t> append_aligned_block(std::size_t input_size, std::size_t offset,
                                         std::size_t block_size, std::size_t alignment,
                                         ByteRange& range) {
    auto block_end = checked_add(offset, block_size);
    if (!block_end) {
        return Result<std::size_t>::failure(block_end.error().code, block_end.error().message);
    }
    if (block_end.value() > input_size) {
        return Result<std::size_t>::failure(ErrorCode::kMalformedBootImage,
                                            "boot image block at " + hex_value(offset) +
                                                " with size " + hex_value(block_size) +
                                                " exceeds the input");
    }
    range = ByteRange{offset, block_size};
    auto next = checked_align_up(block_end.value(), alignment);
    if (!next) {
        return Result<std::size_t>::failure(next.error().code, next.error().message);
    }
    if (next.value() > input_size) {
        return Result<std::size_t>::failure(
            ErrorCode::kMalformedBootImage,
            "aligned boot image block ending at " + hex_value(next.value()) + " exceeds the input");
    }
    return next;
}

std::string normalize_symbol_name(const std::string& name) {
    std::size_t end = name.size();
    for (const char* marker : {"$", ".llvm."}) {
        const std::size_t position = name.find(marker);
        if (position != std::string::npos) {
            end = std::min(end, position);
        }
    }
    std::string normalized = name.substr(0, end);
    constexpr char kCfiSuffix[] = ".cfi_jt";
    if (normalized.size() >= sizeof(kCfiSuffix) - 1 &&
        normalized.compare(normalized.size() - (sizeof(kCfiSuffix) - 1), sizeof(kCfiSuffix) - 1,
                           kCfiSuffix) == 0) {
        normalized.resize(normalized.size() - (sizeof(kCfiSuffix) - 1));
    }
    return normalized;
}

}  // namespace

Result<std::size_t> checked_add(std::size_t left, std::size_t right) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return Result<std::size_t>::failure(ErrorCode::kOverflow, "size addition overflow");
    }
    return Result<std::size_t>::success(left + right);
}

Result<std::size_t> checked_mul(std::size_t left, std::size_t right) {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        return Result<std::size_t>::failure(ErrorCode::kOverflow, "size multiplication overflow");
    }
    return Result<std::size_t>::success(left * right);
}

Result<std::size_t> checked_align_up(std::size_t value, std::size_t alignment) {
    if (!is_power_of_two(alignment)) {
        return Result<std::size_t>::failure(ErrorCode::kInvalidArgument,
                                            "alignment must be a non-zero power of two");
    }
    auto adjusted = checked_add(value, alignment - 1);
    if (!adjusted) {
        return adjusted;
    }
    return Result<std::size_t>::success(adjusted.value() & ~(alignment - 1));
}

Result<ByteRange> checked_range(std::size_t buffer_size, std::size_t offset, std::size_t length) {
    auto end = checked_add(offset, length);
    if (!end) {
        return Result<ByteRange>::failure(end.error().code, end.error().message);
    }
    if (end.value() > buffer_size) {
        return Result<ByteRange>::failure(ErrorCode::kOutOfRange,
                                          "range at " + hex_value(offset) + " with size " +
                                              hex_value(length) + " exceeds buffer size " +
                                              hex_value(buffer_size));
    }
    return Result<ByteRange>::success(ByteRange{offset, length});
}

Result<std::uint16_t> read_u16_le(const std::uint8_t* data, std::size_t size, std::size_t offset) {
    if (data == nullptr) {
        return Result<std::uint16_t>::failure(ErrorCode::kInvalidArgument, "input buffer is null");
    }
    auto range = checked_range(size, offset, sizeof(std::uint16_t));
    if (!range) {
        return Result<std::uint16_t>::failure(range.error().code, range.error().message);
    }
    return Result<std::uint16_t>::success(static_cast<std::uint16_t>(data[offset]) |
                                          (static_cast<std::uint16_t>(data[offset + 1]) << 8));
}

Result<std::uint32_t> read_u32_le(const std::uint8_t* data, std::size_t size, std::size_t offset) {
    if (data == nullptr) {
        return Result<std::uint32_t>::failure(ErrorCode::kInvalidArgument, "input buffer is null");
    }
    auto range = checked_range(size, offset, sizeof(std::uint32_t));
    if (!range) {
        return Result<std::uint32_t>::failure(range.error().code, range.error().message);
    }
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(data[offset + index]) << (index * 8);
    }
    return Result<std::uint32_t>::success(value);
}

Result<std::int32_t> read_i32_le(const std::uint8_t* data, std::size_t size, std::size_t offset) {
    auto value = read_u32_le(data, size, offset);
    if (!value) {
        return Result<std::int32_t>::failure(value.error().code, value.error().message);
    }
    std::int32_t signed_value = 0;
    const std::uint32_t raw = value.value();
    std::memcpy(&signed_value, &raw, sizeof(signed_value));
    return Result<std::int32_t>::success(signed_value);
}

Result<std::uint64_t> read_u64_le(const std::uint8_t* data, std::size_t size, std::size_t offset) {
    if (data == nullptr) {
        return Result<std::uint64_t>::failure(ErrorCode::kInvalidArgument, "input buffer is null");
    }
    auto range = checked_range(size, offset, sizeof(std::uint64_t));
    if (!range) {
        return Result<std::uint64_t>::failure(range.error().code, range.error().message);
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(data[offset + index]) << (index * 8);
    }
    return Result<std::uint64_t>::success(value);
}

Result<std::int64_t> read_i64_le(const std::uint8_t* data, std::size_t size, std::size_t offset) {
    auto value = read_u64_le(data, size, offset);
    if (!value) {
        return Result<std::int64_t>::failure(value.error().code, value.error().message);
    }
    std::int64_t signed_value = 0;
    const std::uint64_t raw = value.value();
    std::memcpy(&signed_value, &raw, sizeof(signed_value));
    return Result<std::int64_t>::success(signed_value);
}

Result<void> write_u32_le(std::uint8_t* data, std::size_t size, std::size_t offset,
                          std::uint32_t value) {
    if (data == nullptr) {
        return Result<void>::failure(ErrorCode::kInvalidArgument, "output buffer is null");
    }
    auto range = checked_range(size, offset, sizeof(value));
    if (!range) {
        return Result<void>::failure(range.error().code, range.error().message);
    }
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        data[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
    return Result<void>::success();
}

Result<void> write_u64_le(std::uint8_t* data, std::size_t size, std::size_t offset,
                          std::uint64_t value) {
    if (data == nullptr) {
        return Result<void>::failure(ErrorCode::kInvalidArgument, "output buffer is null");
    }
    auto range = checked_range(size, offset, sizeof(value));
    if (!range) {
        return Result<void>::failure(range.error().code, range.error().message);
    }
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        data[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
    return Result<void>::success();
}

Result<BootImageInfo> parse_boot_image(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr) {
        return Result<BootImageInfo>::failure(ErrorCode::kInvalidArgument,
                                              "boot image buffer is null");
    }
    if (size < kArm64ImageHeaderSize || !std::equal(kBootMagic.begin(), kBootMagic.end(), data)) {
        return Result<BootImageInfo>::failure(ErrorCode::kMalformedBootImage,
                                              "input is not an Android boot image");
    }

    auto raw_version = read_u32_le(data, size, kHeaderVersionOffset);
    if (!raw_version) {
        return Result<BootImageInfo>::failure(raw_version.error().code,
                                              raw_version.error().message);
    }
    auto kernel_size_value = read_u32_le(data, size, kKernelSizeOffset);
    if (!kernel_size_value) {
        return Result<BootImageInfo>::failure(kernel_size_value.error().code,
                                              kernel_size_value.error().message);
    }

    BootImageInfo info;
    std::size_t offset = 0;
    if (raw_version.value() == 3 || raw_version.value() == 4) {
        info.kind = raw_version.value() == 3 ? BootHeaderKind::kV3 : BootHeaderKind::kV4;
        info.header_version = raw_version.value();
        info.page_size = kModernPageSize;
        auto header_size = read_u32_le(data, size, kModernHeaderSizeOffset);
        auto ramdisk_size = read_u32_le(data, size, 12);
        if (!header_size || !ramdisk_size) {
            const Error& error = !header_size ? header_size.error() : ramdisk_size.error();
            return Result<BootImageInfo>::failure(error.code, error.message);
        }
        const std::size_t minimum_header_size =
            raw_version.value() == 3 ? kModernV3HeaderSize : kModernV4HeaderSize;
        if (header_size.value() < minimum_header_size || header_size.value() > kModernPageSize ||
            size < kModernPageSize) {
            return Result<BootImageInfo>::failure(
                ErrorCode::kMalformedBootImage,
                "invalid Android boot v" + std::to_string(raw_version.value()) + " header size");
        }
        info.header_size = header_size.value();
        info.header = ByteRange{0, kModernPageSize};
        offset = kModernPageSize;
        auto next = append_aligned_block(size, offset, kernel_size_value.value(), kModernPageSize,
                                         info.kernel);
        if (!next) {
            return Result<BootImageInfo>::failure(next.error().code, next.error().message);
        }
        offset = next.value();
        next =
            append_aligned_block(size, offset, ramdisk_size.value(), kModernPageSize, info.ramdisk);
        if (!next) {
            return Result<BootImageInfo>::failure(next.error().code, next.error().message);
        }
        offset = next.value();
        std::uint32_t signature_size = 0;
        if (raw_version.value() == 4) {
            auto value = read_u32_le(data, size, kV4SignatureSizeOffset);
            if (!value) {
                return Result<BootImageInfo>::failure(value.error().code, value.error().message);
            }
            signature_size = value.value();
        }
        next = append_aligned_block(size, offset, signature_size, kModernPageSize, info.signature);
        if (!next) {
            return Result<BootImageInfo>::failure(next.error().code, next.error().message);
        }
        info.payload_end = next.value();
        return Result<BootImageInfo>::success(info);
    }

    auto page_size_value = read_u32_le(data, size, kLegacyPageSizeOffset);
    auto ramdisk_size = read_u32_le(data, size, 16);
    auto second_size = read_u32_le(data, size, 24);
    if (!page_size_value) {
        return Result<BootImageInfo>::failure(page_size_value.error().code,
                                              page_size_value.error().message);
    }
    if (!ramdisk_size) {
        return Result<BootImageInfo>::failure(ramdisk_size.error().code,
                                              ramdisk_size.error().message);
    }
    if (!second_size) {
        return Result<BootImageInfo>::failure(second_size.error().code,
                                              second_size.error().message);
    }
    if (!is_power_of_two(page_size_value.value()) || page_size_value.value() < kLegacyHeaderSize ||
        page_size_value.value() > 64 * 1024 || page_size_value.value() > size) {
        return Result<BootImageInfo>::failure(ErrorCode::kMalformedBootImage,
                                              "invalid legacy boot page size");
    }

    info.kind = BootHeaderKind::kLegacy;
    info.header_version = raw_version.value() <= 2 ? raw_version.value() : 0;
    info.page_size = page_size_value.value();
    info.header_size = static_cast<std::uint32_t>(kLegacyHeaderSize);
    if (info.header_version >= 1) {
        auto declared_header_size = read_u32_le(data, size, 1644);
        if (!declared_header_size) {
            return Result<BootImageInfo>::failure(declared_header_size.error().code,
                                                  declared_header_size.error().message);
        }
        const std::size_t minimum = info.header_version == 1 ? kV1HeaderSize : kV2HeaderSize;
        if (declared_header_size.value() < minimum ||
            declared_header_size.value() > info.page_size) {
            return Result<BootImageInfo>::failure(ErrorCode::kMalformedBootImage,
                                                  "invalid legacy boot header size");
        }
        info.header_size = declared_header_size.value();
    }
    info.header = ByteRange{0, info.page_size};
    offset = info.page_size;
    auto next =
        append_aligned_block(size, offset, kernel_size_value.value(), info.page_size, info.kernel);
    if (!next) {
        return Result<BootImageInfo>::failure(next.error().code, next.error().message);
    }
    offset = next.value();
    next = append_aligned_block(size, offset, ramdisk_size.value(), info.page_size, info.ramdisk);
    if (!next) {
        return Result<BootImageInfo>::failure(next.error().code, next.error().message);
    }
    offset = next.value();
    next = append_aligned_block(size, offset, second_size.value(), info.page_size, info.second);
    if (!next) {
        return Result<BootImageInfo>::failure(next.error().code, next.error().message);
    }
    offset = next.value();

    const std::uint32_t extra_size = info.header_version == 0 ? raw_version.value() : 0;
    next = append_aligned_block(size, offset, extra_size, info.page_size, info.extra);
    if (!next) {
        return Result<BootImageInfo>::failure(next.error().code, next.error().message);
    }
    offset = next.value();

    if (info.header_version >= 1) {
        auto recovery_size = read_u32_le(data, size, kV1RecoveryDtboSizeOffset);
        auto recovery_offset = read_u64_le(data, size, kV1RecoveryDtboOffsetOffset);
        if (!recovery_size || !recovery_offset) {
            const Error& error = !recovery_size ? recovery_size.error() : recovery_offset.error();
            return Result<BootImageInfo>::failure(error.code, error.message);
        }
        info.recovery_dtbo_offset = recovery_offset.value();
        if (recovery_size.value() != 0) {
            if (recovery_offset.value() > std::numeric_limits<std::size_t>::max() ||
                recovery_offset.value() < offset || recovery_offset.value() % info.page_size != 0) {
                return Result<BootImageInfo>::failure(
                    ErrorCode::kMalformedBootImage,
                    "invalid recovery_dtbo offset in legacy boot image");
            }
            offset = static_cast<std::size_t>(recovery_offset.value());
        }
        next = append_aligned_block(size, offset, recovery_size.value(), info.page_size,
                                    info.recovery_dtbo);
        if (!next) {
            return Result<BootImageInfo>::failure(next.error().code, next.error().message);
        }
        offset = next.value();
    }

    if (info.header_version >= 2) {
        auto dtb_size = read_u32_le(data, size, kV2DtbSizeOffset);
        if (!dtb_size) {
            return Result<BootImageInfo>::failure(dtb_size.error().code, dtb_size.error().message);
        }
        next = append_aligned_block(size, offset, dtb_size.value(), info.page_size, info.dtb);
        if (!next) {
            return Result<BootImageInfo>::failure(next.error().code, next.error().message);
        }
        offset = next.value();
    }
    info.payload_end = offset;
    return Result<BootImageInfo>::success(info);
}

Result<std::vector<std::uint8_t>> extract_boot_kernel(const std::uint8_t* data, std::size_t size) {
    auto info = parse_boot_image(data, size);
    if (!info) {
        return Result<std::vector<std::uint8_t>>::failure(info.error().code, info.error().message);
    }
    const ByteRange range = info.value().kernel;
    return Result<std::vector<std::uint8_t>>::success(
        std::vector<std::uint8_t>(data + range.offset, data + range.offset + range.size));
}

Result<std::vector<std::uint8_t>> replace_boot_kernel(const std::uint8_t* data, std::size_t size,
                                                      const std::uint8_t* kernel,
                                                      std::size_t kernel_size) {
    if (data == nullptr || (kernel == nullptr && kernel_size != 0)) {
        return Result<std::vector<std::uint8_t>>::failure(
            ErrorCode::kInvalidArgument, "boot image or replacement kernel is null");
    }
    if (kernel_size > std::numeric_limits<std::uint32_t>::max()) {
        return Result<std::vector<std::uint8_t>>::failure(
            ErrorCode::kUnsupported, "replacement kernel exceeds the boot header limit");
    }
    auto info_result = parse_boot_image(data, size);
    if (!info_result) {
        return Result<std::vector<std::uint8_t>>::failure(info_result.error().code,
                                                          info_result.error().message);
    }
    const BootImageInfo& info = info_result.value();
    auto old_kernel_end = checked_add(info.kernel.offset, info.kernel.size);
    auto new_kernel_end = checked_add(info.kernel.offset, kernel_size);
    if (!old_kernel_end || !new_kernel_end) {
        const Error& error = !old_kernel_end ? old_kernel_end.error() : new_kernel_end.error();
        return Result<std::vector<std::uint8_t>>::failure(error.code, error.message);
    }
    auto old_suffix = checked_align_up(old_kernel_end.value(), info.page_size);
    auto new_suffix = checked_align_up(new_kernel_end.value(), info.page_size);
    if (!old_suffix || !new_suffix || old_suffix.value() > size) {
        const Error& error = !old_suffix ? old_suffix.error() : new_suffix.error();
        return Result<std::vector<std::uint8_t>>::failure(error.code, error.message);
    }
    auto output_size = checked_add(new_suffix.value(), size - old_suffix.value());
    if (!output_size) {
        return Result<std::vector<std::uint8_t>>::failure(output_size.error().code,
                                                          output_size.error().message);
    }

    std::vector<std::uint8_t> output(output_size.value(), 0);
    std::copy(data, data + info.kernel.offset, output.begin());
    if (kernel_size != 0) {
        std::copy(kernel, kernel + kernel_size, output.begin() + info.kernel.offset);
    }
    std::copy(data + old_suffix.value(), data + size, output.begin() + new_suffix.value());
    auto write_size = write_u32_le(output.data(), output.size(), kKernelSizeOffset,
                                   static_cast<std::uint32_t>(kernel_size));
    if (!write_size) {
        return Result<std::vector<std::uint8_t>>::failure(write_size.error().code,
                                                          write_size.error().message);
    }

    if (info.header_version >= 1 && info.recovery_dtbo_offset != 0) {
        if (info.recovery_dtbo_offset > std::numeric_limits<std::size_t>::max() ||
            info.recovery_dtbo_offset < old_suffix.value()) {
            return Result<std::vector<std::uint8_t>>::failure(
                ErrorCode::kMalformedBootImage,
                "recovery_dtbo unexpectedly precedes the kernel suffix");
        }
        auto shifted_offset =
            checked_add(new_suffix.value(),
                        static_cast<std::size_t>(info.recovery_dtbo_offset) - old_suffix.value());
        if (!shifted_offset) {
            return Result<std::vector<std::uint8_t>>::failure(shifted_offset.error().code,
                                                              shifted_offset.error().message);
        }
        auto write_offset = write_u64_le(output.data(), output.size(), kV1RecoveryDtboOffsetOffset,
                                         shifted_offset.value());
        if (!write_offset) {
            return Result<std::vector<std::uint8_t>>::failure(write_offset.error().code,
                                                              write_offset.error().message);
        }
    }
    return Result<std::vector<std::uint8_t>>::success(std::move(output));
}

Result<RawImageInfo> parse_arm64_image(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr) {
        return Result<RawImageInfo>::failure(ErrorCode::kInvalidArgument,
                                             "kernel Image buffer is null");
    }
    if (size < kArm64ImageHeaderSize ||
        !std::equal(kArm64Magic.begin(), kArm64Magic.end(), data + kArm64MagicOffset)) {
        return Result<RawImageInfo>::failure(ErrorCode::kMalformedArm64Image,
                                             "kernel input is not an uncompressed ARM64 Image");
    }
    auto text_offset = read_u64_le(data, size, kArm64TextOffsetOffset);
    auto image_size = read_u64_le(data, size, kArm64ImageSizeOffset);
    auto flags = read_u64_le(data, size, kArm64FlagsOffset);
    if (!text_offset) {
        return Result<RawImageInfo>::failure(text_offset.error().code, text_offset.error().message);
    }
    if (!image_size) {
        return Result<RawImageInfo>::failure(image_size.error().code, image_size.error().message);
    }
    if (!flags) {
        return Result<RawImageInfo>::failure(flags.error().code, flags.error().message);
    }
    if (image_size.value() == 0) {
        return Result<RawImageInfo>::failure(
            ErrorCode::kMalformedArm64Image,
            "ARM64 Image header has image_size=0; unsupported input");
    }
    if (image_size.value() > std::numeric_limits<std::size_t>::max()) {
        return Result<RawImageInfo>::failure(ErrorCode::kUnsupported,
                                             "ARM64 Image size does not fit the current process");
    }
    return Result<RawImageInfo>::success(RawImageInfo{
        text_offset.value(), static_cast<std::size_t>(image_size.value()), flags.value()});
}

SymbolMap::SymbolMap(std::vector<MapSymbol> entries) : entries_(std::move(entries)) {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        by_name_[entries_[index].name].push_back(index);
        by_normalized_name_[normalize_symbol_name(entries_[index].name)].push_back(index);
    }
}

Result<SymbolMap> SymbolMap::create(std::vector<MapSymbol> entries) {
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [](const MapSymbol& entry) { return entry.address == 0; }),
                  entries.end());
    if (entries.empty()) {
        return Result<SymbolMap>::failure(ErrorCode::kKallsymsNotFound,
                                          "symbol map has no non-zero kernel addresses");
    }
    std::sort(entries.begin(), entries.end(), [](const MapSymbol& left, const MapSymbol& right) {
        if (left.address != right.address) {
            return left.address < right.address;
        }
        return left.name < right.name;
    });
    return Result<SymbolMap>::success(SymbolMap(std::move(entries)));
}

std::vector<MapSymbol> SymbolMap::variants(const std::string& requested_name) const {
    std::map<std::uint64_t, MapSymbol> unique;
    const auto found = by_normalized_name_.find(normalize_symbol_name(requested_name));
    if (found != by_normalized_name_.end()) {
        for (const std::size_t index : found->second) {
            unique.emplace(entries_[index].address, entries_[index]);
        }
    }
    std::vector<MapSymbol> variants;
    variants.reserve(unique.size());
    for (const auto& entry : unique) {
        variants.push_back(entry.second);
    }
    return variants;
}

Result<MapSymbol> SymbolMap::resolve(const std::string& requested_name) const {
    const auto exact_match = by_name_.find(requested_name);
    if (exact_match != by_name_.end()) {
        std::map<std::uint64_t, MapSymbol> exact;
        for (const std::size_t index : exact_match->second) {
            exact.emplace(entries_[index].address, entries_[index]);
        }
        if (exact.size() == 1) {
            return Result<MapSymbol>::success(exact.begin()->second);
        }
    }

    std::vector<MapSymbol> candidates = variants(requested_name);
    if (candidates.size() == 1) {
        return Result<MapSymbol>::success(std::move(candidates.front()));
    }
    if (candidates.empty()) {
        return Result<MapSymbol>::failure(ErrorCode::kKallsymsNotFound,
                                          "symbol \"" + requested_name + "\" was not found");
    }
    std::string message = "symbol \"";
    message += requested_name;
    message += "\" is not unique: ";
    const std::size_t rendered = std::min<std::size_t>(8, candidates.size());
    for (std::size_t index = 0; index < rendered; ++index) {
        if (index != 0) {
            message += ", ";
        }
        message += candidates[index].name;
        message += '@';
        append_hex(&message, candidates[index].address);
    }
    return Result<MapSymbol>::failure(ErrorCode::kAmbiguous, std::move(message));
}

std::optional<MapSymbol> SymbolMap::resolve_module_symbol(const std::string& requested_name) const {
    auto symbol = resolve(requested_name);
    if (!symbol) {
        return std::nullopt;
    }
    return symbol.take_value();
}

std::optional<std::uint64_t> SymbolMap::next_address(std::uint64_t address) const {
    const auto found =
        std::find_if(entries_.begin(), entries_.end(),
                     [address](const MapSymbol& entry) { return entry.address > address; });
    if (found == entries_.end()) {
        return std::nullopt;
    }
    return found->address;
}

namespace {

using KallsymsToken = std::vector<std::uint8_t>;
using KallsymsTokens = std::array<KallsymsToken, kKallsymsTokenCount>;

struct TokenTable {
    KallsymsTokens tokens;
    std::size_t table_offset = 0;
    std::size_t index_offset = 0;
};

struct NameSpan {
    std::size_t offset = 0;
    std::size_t size = 0;
};

bool operator==(const NameSpan& left, const NameSpan& right) {
    return left.offset == right.offset && left.size == right.size;
}

struct DecodedName {
    std::uint8_t kind = 0;
    std::string name;
};

struct NameTable {
    std::size_t num_syms_offset = 0;
    std::size_t names_offset = 0;
    std::size_t markers_offset = 0;
    std::vector<DecodedName> names;
};

std::optional<std::size_t> align_up_optional(std::size_t value, std::size_t alignment) {
    auto aligned = checked_align_up(value, alignment);
    if (!aligned) {
        return std::nullopt;
    }
    return aligned.value();
}

std::optional<std::size_t> find_subslice(const std::uint8_t* data, std::size_t size,
                                         const std::vector<std::uint8_t>& needle,
                                         std::size_t start) {
    if (needle.empty() || start > size || needle.size() > size - start) {
        return std::nullopt;
    }
    const auto* const found = std::search(data + start, data + size, needle.begin(), needle.end());
    if (found == data + size) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - data);
}

std::optional<TokenTable> parse_kallsyms_token_table_at(const std::uint8_t* image,
                                                        std::size_t image_size, std::size_t start,
                                                        std::size_t digit_offset) {
    if (start % kKallsymsAlignment != 0 || start > image_size) {
        return std::nullopt;
    }

    TokenTable table;
    table.table_offset = start;
    std::array<std::size_t, kKallsymsTokenCount> token_offsets{};
    std::array<std::size_t, kKallsymsTokenCount> token_starts{};
    std::size_t position = start;
    for (std::size_t index = 0; index < kKallsymsTokenCount; ++index) {
        token_starts[index] = position;
        const std::size_t maximum = kKallsymsMaxTokenLength + 1;
        const std::size_t available = image_size - position;
        const std::size_t search_size = std::min(maximum, available);
        const auto* const terminator =
            std::find(image + position, image + position + search_size, 0);
        if (terminator == image + position + search_size) {
            return std::nullopt;
        }
        const std::size_t length = static_cast<std::size_t>(terminator - (image + position));
        if (!std::all_of(image + position, image + position + length,
                         [](std::uint8_t byte) { return byte >= 0x20 && byte <= 0x7e; })) {
            return std::nullopt;
        }
        token_offsets[index] = position - start;
        table.tokens[index].assign(image + position, image + position + length);
        if (length == std::numeric_limits<std::size_t>::max() - position) {
            return std::nullopt;
        }
        position += length + 1;
    }

    if (token_starts[static_cast<std::size_t>('0')] != digit_offset) {
        return std::nullopt;
    }
    constexpr char kRequiredSingleByteTokens[] = "0123456789_abcdefghijklmnopqrstuvwxyzT";
    for (const char value : kRequiredSingleByteTokens) {
        if (value == '\0') {
            break;
        }
        const KallsymsToken& token = table.tokens[static_cast<std::uint8_t>(value)];
        if (token.size() != 1 || token.front() != static_cast<std::uint8_t>(value)) {
            return std::nullopt;
        }
    }

    const auto index_offset = align_up_optional(position, kKallsymsAlignment);
    if (!index_offset || *index_offset > image_size || !all_zero(image, position, *index_offset)) {
        return std::nullopt;
    }
    if (kKallsymsTokenIndexSize > image_size - *index_offset ||
        token_offsets.back() > std::numeric_limits<std::uint16_t>::max()) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < kKallsymsTokenCount; ++index) {
        auto actual = read_u16_le(image, image_size, *index_offset + (index * 2));
        if (!actual || actual.value() != token_offsets[index]) {
            return std::nullopt;
        }
    }
    table.index_offset = *index_offset;
    return table;
}

std::vector<TokenTable> find_kallsyms_token_tables(const std::uint8_t* image,
                                                   std::size_t image_size) {
    std::vector<std::uint8_t> digit_tokens;
    digit_tokens.reserve(20);
    for (std::uint8_t value = '0'; value <= '9'; ++value) {
        digit_tokens.push_back(value);
        digit_tokens.push_back(0);
    }

    std::map<std::size_t, TokenTable> candidates;
    std::size_t search_from = 0;
    while (true) {
        const auto digit_offset = find_subslice(image, image_size, digit_tokens, search_from);
        if (!digit_offset) {
            break;
        }
        constexpr std::size_t kMaximumPrefix =
            static_cast<std::size_t>('0') * (kKallsymsMaxTokenLength + 1);
        const std::size_t earliest =
            *digit_offset > kMaximumPrefix ? *digit_offset - kMaximumPrefix : 0;
        auto start = align_up_optional(earliest, kKallsymsAlignment);
        if (start) {
            while (*start <= *digit_offset) {
                auto table =
                    parse_kallsyms_token_table_at(image, image_size, *start, *digit_offset);
                if (table) {
                    candidates.emplace(*start, std::move(*table));
                }
                if (*start > std::numeric_limits<std::size_t>::max() - kKallsymsAlignment) {
                    break;
                }
                *start += kKallsymsAlignment;
            }
        }
        if (*digit_offset == std::numeric_limits<std::size_t>::max()) {
            break;
        }
        search_from = *digit_offset + 1;
        if (search_from >= image_size) {
            break;
        }
    }

    std::vector<TokenTable> tables;
    tables.reserve(candidates.size());
    for (auto& entry : candidates) {
        tables.push_back(std::move(entry.second));
    }
    return tables;
}

std::vector<std::uint32_t> read_kallsyms_markers(const std::uint8_t* image, std::size_t image_size,
                                                 std::size_t start, std::size_t limit) {
    if (start % kKallsymsAlignment != 0 || limit > image_size || start > limit ||
        limit - start < 8) {
        return {};
    }
    auto first = read_u32_le(image, image_size, start);
    auto second = read_u32_le(image, image_size, start + 4);
    if (!first || !second || first.value() != 0 || second.value() < 0x200 ||
        second.value() > 0x40000) {
        return {};
    }

    std::vector<std::uint32_t> markers{first.value(), second.value()};
    std::size_t position = start + 8;
    while (markers.size() < kKallsymsMaxMarkers && position <= limit && limit - position >= 4) {
        auto value = read_u32_le(image, image_size, position);
        if (!value || value.value() < markers.back()) {
            break;
        }
        const std::uint32_t delta = value.value() - markers.back();
        if (delta < 0x200 || delta > 0x40000) {
            break;
        }
        markers.push_back(value.value());
        position += 4;
    }
    if (markers.size() < kKallsymsMinMarkers) {
        return {};
    }
    return markers;
}

std::optional<std::vector<NameSpan>> parse_kallsyms_name_spans(
    const std::uint8_t* image, std::size_t image_size, std::size_t names_offset, std::size_t count,
    std::size_t markers_offset, const std::vector<std::uint32_t>& markers, bool uleb128_lengths) {
    if (count == 0 || count > image_size || markers.size() != (count + 255) / 256) {
        return std::nullopt;
    }

    std::vector<NameSpan> spans;
    spans.reserve(count);
    std::size_t position = names_offset;
    for (std::size_t index = 0; index < count; ++index) {
        if (index % 256 == 0) {
            if (position < names_offset || position - names_offset != markers[index / 256]) {
                return std::nullopt;
            }
        }
        if (position >= markers_offset || position >= image_size) {
            return std::nullopt;
        }
        std::size_t length = image[position++];
        if (uleb128_lengths && (length & 0x80) != 0) {
            if (position >= markers_offset || position >= image_size) {
                return std::nullopt;
            }
            const std::size_t high = image[position++];
            if ((high & 0x80) != 0) {
                return std::nullopt;
            }
            length = (length & 0x7f) | (high << 7);
        }
        if (length == 0 || length > 0x3fff || length > markers_offset - position ||
            length > image_size - position) {
            return std::nullopt;
        }
        spans.push_back(NameSpan{position, length});
        position += length;
    }

    const auto aligned = align_up_optional(position, kKallsymsAlignment);
    if (!aligned || *aligned != markers_offset || !all_zero(image, position, markers_offset)) {
        return std::nullopt;
    }
    return spans;
}

std::optional<std::vector<DecodedName>> decode_kallsyms_names(const std::uint8_t* image,
                                                              std::size_t image_size,
                                                              const std::vector<NameSpan>& spans,
                                                              const KallsymsTokens& tokens) {
    constexpr char kSymbolTypes[] = "aAbBcCdDeEfFgGiInNpPrRsStTuUvVwW?-";
    std::vector<DecodedName> decoded;
    decoded.reserve(spans.size());
    for (const NameSpan& span : spans) {
        if (span.offset > image_size || span.size > image_size - span.offset) {
            return std::nullopt;
        }
        std::size_t expanded_length = 0;
        for (std::size_t index = 0; index < span.size; ++index) {
            const auto& token = tokens[image[span.offset + index]];
            if (token.size() > std::numeric_limits<std::size_t>::max() - expanded_length) {
                return std::nullopt;
            }
            expanded_length += token.size();
        }
        if (expanded_length > 4096) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> expanded;
        expanded.reserve(expanded_length);
        for (std::size_t index = 0; index < span.size; ++index) {
            const auto& token = tokens[image[span.offset + index]];
            expanded.insert(expanded.end(), token.begin(), token.end());
        }
        if (expanded.size() < 2 ||
            std::strchr(kSymbolTypes, static_cast<char>(expanded.front())) == nullptr ||
            !std::all_of(expanded.begin() + 1, expanded.end(),
                         [](std::uint8_t byte) { return byte >= 0x21 && byte <= 0x7e; })) {
            return std::nullopt;
        }
        decoded.push_back(DecodedName{
            expanded.front(),
            std::string(expanded.begin() + 1, expanded.end()),
        });
    }

    std::unordered_set<std::string> names;
    names.reserve(decoded.size());
    for (const DecodedName& name : decoded) {
        names.insert(name.name);
    }
    for (const char* required : {"_text", "_end", "load_module"}) {
        if (names.find(required) == names.end()) {
            return std::nullopt;
        }
    }
    return decoded;
}

std::vector<NameTable> find_kallsyms_names(const std::uint8_t* image, std::size_t image_size,
                                           const TokenTable& token_table) {
    const std::size_t lower = token_table.table_offset > kKallsymsMarkerSearchWindow
                                  ? token_table.table_offset - kKallsymsMarkerSearchWindow
                                  : 0;
    std::vector<std::pair<std::size_t, std::vector<std::uint32_t>>> marker_candidates;
    auto position = align_up_optional(lower, kKallsymsAlignment);
    while (position && *position <= token_table.table_offset &&
           token_table.table_offset - *position >= 8) {
        auto markers =
            read_kallsyms_markers(image, image_size, *position, token_table.table_offset);
        if (!markers.empty()) {
            marker_candidates.emplace_back(*position, std::move(markers));
        }
        if (*position > std::numeric_limits<std::size_t>::max() - kKallsymsAlignment) {
            break;
        }
        *position += kKallsymsAlignment;
    }

    std::vector<NameTable> recovered;
    for (const auto& marker_candidate : marker_candidates) {
        const std::size_t markers_offset = marker_candidate.first;
        const std::vector<std::uint32_t>& markers = marker_candidate.second;
        const std::size_t minimum_count = ((markers.size() - 1) * 256) + 1;
        const std::size_t maximum_count = markers.size() * 256;
        if (markers.back() > markers_offset) {
            continue;
        }
        const std::size_t approximate_names = markers_offset - markers.back();
        const std::size_t search_start = approximate_names > kKallsymsNameTailSearch
                                             ? approximate_names - kKallsymsNameTailSearch
                                             : 0;
        std::size_t num_syms_offset = approximate_names - (approximate_names % kKallsymsAlignment);
        while (true) {
            if (num_syms_offset <= image_size && image_size - num_syms_offset >= 8) {
                auto count_value = read_u32_le(image, image_size, num_syms_offset);
                const std::size_t count = count_value ? count_value.value() : 0;
                if (count >= minimum_count && count <= maximum_count &&
                    all_zero(image, num_syms_offset + 4, num_syms_offset + 8)) {
                    const std::size_t names_offset = num_syms_offset + kKallsymsAlignment;
                    std::vector<std::vector<NameSpan>> seen_spans;
                    for (const bool uleb128_lengths : {true, false}) {
                        auto spans =
                            parse_kallsyms_name_spans(image, image_size, names_offset, count,
                                                      markers_offset, markers, uleb128_lengths);
                        if (!spans || std::find(seen_spans.begin(), seen_spans.end(), *spans) !=
                                          seen_spans.end()) {
                            continue;
                        }
                        seen_spans.push_back(*spans);
                        auto decoded =
                            decode_kallsyms_names(image, image_size, *spans, token_table.tokens);
                        if (decoded) {
                            recovered.push_back(NameTable{
                                num_syms_offset,
                                names_offset,
                                markers_offset,
                                std::move(*decoded),
                            });
                        }
                    }
                }
            }
            if (num_syms_offset < search_start + kKallsymsAlignment) {
                break;
            }
            num_syms_offset -= kKallsymsAlignment;
        }
    }
    return recovered;
}

bool is_arm64_kernel_address(std::uint64_t address) {
    return (address >> 48) == 0xffff && address % 4096 == 0;
}

std::optional<SymbolMap> decode_kallsyms_addresses(const std::uint8_t* image,
                                                   std::size_t image_size,
                                                   std::size_t declared_image_size,
                                                   const std::vector<DecodedName>& names,
                                                   std::size_t offsets_offset,
                                                   std::size_t relative_base_offset) {
    const std::size_t count = names.size();
    auto offsets_size = checked_mul(count, 4);
    if (!offsets_size) {
        return std::nullopt;
    }
    auto offsets_end = checked_add(offsets_offset, offsets_size.value());
    if (!offsets_end || offsets_end.value() > image_size ||
        relative_base_offset < offsets_end.value() || relative_base_offset > image_size ||
        image_size - relative_base_offset < 8 ||
        !all_zero(image, offsets_end.value(), relative_base_offset)) {
        return std::nullopt;
    }
    auto relative_base = read_u64_le(image, image_size, relative_base_offset);
    if (!relative_base || !is_arm64_kernel_address(relative_base.value())) {
        return std::nullopt;
    }

    std::vector<std::int32_t> signed_offsets;
    signed_offsets.reserve(count);
    std::size_t negative_count = 0;
    for (std::size_t index = 0; index < count; ++index) {
        auto offset = read_i32_le(image, image_size, offsets_offset + (index * 4));
        if (!offset) {
            return std::nullopt;
        }
        signed_offsets.push_back(offset.value());
        negative_count += offset.value() < 0 ? 1 : 0;
    }

    std::vector<std::uint64_t> addresses;
    addresses.reserve(count);
    const bool absolute_percpu = negative_count >= (count + 1) / 2;
    for (std::size_t index = 0; index < count; ++index) {
        if (absolute_percpu) {
            const std::int32_t offset = signed_offsets[index];
            if (offset < 0) {
                const std::uint64_t magnitude =
                    static_cast<std::uint64_t>(-static_cast<std::int64_t>(offset));
                if (magnitude == 0 || magnitude - 1 > std::numeric_limits<std::uint64_t>::max() -
                                                          relative_base.value()) {
                    return std::nullopt;
                }
                addresses.push_back(relative_base.value() + magnitude - 1);
            } else {
                addresses.push_back(static_cast<std::uint64_t>(offset));
            }
        } else {
            auto raw = read_u32_le(image, image_size, offsets_offset + (index * 4));
            if (!raw ||
                raw.value() > std::numeric_limits<std::uint64_t>::max() - relative_base.value()) {
                return std::nullopt;
            }
            addresses.push_back(relative_base.value() + raw.value());
        }
    }
    if (!std::is_sorted(addresses.begin(), addresses.end())) {
        return std::nullopt;
    }

    std::vector<std::uint64_t> text_addresses;
    std::vector<std::uint64_t> end_addresses;
    for (std::size_t index = 0; index < count; ++index) {
        if (names[index].name == "_text") {
            text_addresses.push_back(addresses[index]);
        } else if (names[index].name == "_end") {
            end_addresses.push_back(addresses[index]);
        }
    }
    if (text_addresses.size() != 1 || end_addresses.size() != 1 ||
        text_addresses.front() != relative_base.value() ||
        end_addresses.front() < text_addresses.front() ||
        end_addresses.front() - text_addresses.front() != declared_image_size) {
        return std::nullopt;
    }

    std::size_t ordinary_count = 0;
    std::size_t in_image_count = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if ((names[index].kind == 'A' || names[index].kind == 'a') || addresses[index] == 0) {
            continue;
        }
        ++ordinary_count;
        if (addresses[index] >= text_addresses.front() &&
            addresses[index] <= end_addresses.front()) {
            ++in_image_count;
        }
    }
    if (ordinary_count == 0 || in_image_count * 100 < ordinary_count * 90) {
        return std::nullopt;
    }

    std::vector<MapSymbol> entries;
    entries.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        entries.push_back(MapSymbol{addresses[index], names[index].name});
    }
    auto symbols = SymbolMap::create(std::move(entries));
    if (!symbols) {
        return std::nullopt;
    }
    return symbols.take_value();
}

}  // namespace

std::vector<RecoveredKallsyms> recover_arm64_kallsyms_candidates(const std::uint8_t* data,
                                                                 std::size_t size) {
    if (data == nullptr) {
        return {};
    }
    auto raw_image = parse_arm64_image(data, size);
    if (!raw_image) {
        return {};
    }

    std::vector<RecoveredKallsyms> candidates;
    const std::vector<TokenTable> token_tables = find_kallsyms_token_tables(data, size);
    for (const TokenTable& token_table : token_tables) {
        const std::vector<NameTable> name_tables = find_kallsyms_names(data, size, token_table);
        for (const NameTable& name_table : name_tables) {
            const std::size_t count = name_table.names.size();
            auto offset_bytes = checked_mul(count, 4);
            if (!offset_bytes) {
                continue;
            }
            auto aligned_offset_bytes = checked_align_up(offset_bytes.value(), kKallsymsAlignment);
            if (!aligned_offset_bytes || name_table.num_syms_offset < 8) {
                continue;
            }
            const std::size_t old_relative_base_offset = name_table.num_syms_offset - 8;
            if (old_relative_base_offset < aligned_offset_bytes.value()) {
                continue;
            }
            const std::size_t old_offsets_offset =
                old_relative_base_offset - aligned_offset_bytes.value();

            auto token_index_end = checked_add(token_table.index_offset, kKallsymsTokenIndexSize);
            if (!token_index_end) {
                continue;
            }
            auto new_offsets_offset = checked_align_up(token_index_end.value(), kKallsymsAlignment);
            if (!new_offsets_offset) {
                continue;
            }
            auto new_offsets_end = checked_add(new_offsets_offset.value(), offset_bytes.value());
            if (!new_offsets_end) {
                continue;
            }
            auto new_relative_base_offset =
                checked_align_up(new_offsets_end.value(), kKallsymsAlignment);
            if (!new_relative_base_offset) {
                continue;
            }

            struct Layout {
                const char* name;
                std::size_t offsets_offset;
                std::size_t relative_base_offset;
            };
            const std::array<Layout, 2> layouts{{
                {"pre-6.4", old_offsets_offset, old_relative_base_offset},
                {"6.4+", new_offsets_offset.value(), new_relative_base_offset.value()},
            }};
            for (const Layout& layout : layouts) {
                auto symbols = decode_kallsyms_addresses(data, size, raw_image.value().image_size,
                                                         name_table.names, layout.offsets_offset,
                                                         layout.relative_base_offset);
                if (!symbols) {
                    continue;
                }
                candidates.push_back(RecoveredKallsyms{
                    std::move(*symbols),
                    layout.name,
                    count,
                    token_table.table_offset,
                    token_table.index_offset,
                    name_table.names_offset,
                    name_table.markers_offset,
                    layout.offsets_offset,
                    layout.relative_base_offset,
                });
            }
        }
    }
    return candidates;
}

Result<RecoveredKallsyms> recover_arm64_kallsyms(const std::uint8_t* data, std::size_t size) {
    auto image = parse_arm64_image(data, size);
    if (!image) {
        return Result<RecoveredKallsyms>::failure(image.error().code, image.error().message);
    }
    std::vector<RecoveredKallsyms> candidates = recover_arm64_kallsyms_candidates(data, size);
    if (candidates.empty()) {
        return Result<RecoveredKallsyms>::failure(
            ErrorCode::kKallsymsNotFound,
            "cannot recover GKI kallsyms from ARM64 Image; CONFIG_KALLSYMS_ALL is required");
    }
    if (candidates.size() != 1) {
        return Result<RecoveredKallsyms>::failure(
            ErrorCode::kAmbiguous, "cannot uniquely recover GKI kallsyms from ARM64 Image (" +
                                       std::to_string(candidates.size()) + " candidates)");
    }
    RecoveredKallsyms recovered = std::move(candidates.front());
    return Result<RecoveredKallsyms>::success(std::move(recovered));
}

}  // namespace ksud::boot::lkm_image
