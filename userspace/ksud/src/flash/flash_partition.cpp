#include "flash_partition.hpp"
#include <fcntl.h>
#include <linux/fs.h>
#include <mbedtls/sha256.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include "../boot/tools.hpp"
#include "../log.hpp"
#include "../utils.hpp"

// miniz is header-only in this context or linked
#define MINIZ_HEADER_FILE_ONLY
#include "miniz.h"

namespace ksud::flash {

namespace fs = std::filesystem;

namespace {

// Helper wrapper to match my previous logic
ExecResult exec_command_sync(const std::vector<std::string>& args) {
    return exec_command(args);
}

// Helper: Convert bytes to hex string
std::string bytes_to_hex(const unsigned char* data, size_t len) {
    static constexpr std::string_view hex_chars = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        result.push_back(hex_chars[(data[i] >> 4) & 0xF]);
        result.push_back(hex_chars[data[i] & 0xF]);
    }
    return result;
}

// Helper: Get file size (handles both regular files and block devices)
uint64_t get_file_size(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        LOGE("Failed to stat %s: %s", path.c_str(), strerror(errno));
        return 0;
    }

    // For block devices, use ioctl to get the size
    if (S_ISBLK(st.st_mode)) {
        const int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            LOGE("Failed to open block device %s: %s", path.c_str(), strerror(errno));
            return 0;
        }

        uint64_t size = 0;
        if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
            LOGE("Failed to get block device size for %s: %s", path.c_str(), strerror(errno));
            close(fd);
            return 0;
        }

        close(fd);
        LOGD("Block device %s size: %lu bytes", path.c_str(), (unsigned long)size);
        return size;
    }

    // For regular files, use st_size
    return st.st_size;
}

bool is_block_device_path(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISBLK(st.st_mode);
}

std::string get_block_device_slot_suffix(const std::string& block_device) {
    const std::string device_name = fs::path(block_device).filename().string();
    if (device_name.length() < 3) {
        return "";
    }
    const std::string suffix = device_name.substr(device_name.length() - 2);
    return suffix == "_a" || suffix == "_b" ? suffix : "";
}

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (const char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(c);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

// Helper: Execute command and get output
std::string exec_cmd(const std::string& cmd) {
    auto result = exec_command_sync({"/system/bin/sh", "-c", cmd});
    return trim(result.stdout_str);
}

}  // namespace

std::string get_current_slot_suffix() {
    auto result = exec_command_sync({"getprop", "ro.boot.slot_suffix"});
    return trim(result.stdout_str);
}

bool is_ab_device() {
    auto result = exec_command_sync({"getprop", "ro.build.ab_update"});
    if (trim(result.stdout_str) != "true") {
        return false;
    }
    return !get_current_slot_suffix().empty();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) partition_name then slot_suffix
std::string find_partition_block_device(const std::string& partition_name,
                                        const std::string& slot_suffix) {
    // 检查分区名是否以 _a 或 _b 结尾（slotful分区）
    bool is_slotful = false;
    if (partition_name.length() >= 2) {
        const std::string last_two = partition_name.substr(partition_name.length() - 2);
        if (last_two == "_a" || last_two == "_b") {
            is_slotful = true;
        }
    }

    // 确定要使用的槽位后缀
    std::string suffix;
    if (is_slotful) {
        // 如果分区名本身已经带了槽位后缀，不再添加
        suffix = "";
    } else if (!slot_suffix.empty()) {
        // 使用传入的槽位后缀
        suffix = slot_suffix;
    } else {
        // 使用当前槽位
        suffix = get_current_slot_suffix();
    }

    // Build candidate names
    std::vector<std::string> names_to_try;
    // 总是先尝试不带后缀的名字（slotless分区、或者已经带后缀的分区名）
    names_to_try.push_back(partition_name);
    // 如果分区名本身不带_a/_b后缀，且在AB设备上，再尝试带槽位后缀的版本
    if (!suffix.empty() && !is_slotful) {
        names_to_try.push_back(partition_name + suffix);
    }

    // Try multiple common locations
    const std::vector<std::string> base_paths = {
        "/dev/block/by-name/",
        "/dev/block/mapper/",
        "/dev/block/bootdevice/by-name/",
    };

    for (const auto& name : names_to_try) {
        for (const auto& base : base_paths) {
            const std::string path = base + name;
            if (is_block_device_path(path)) {
                LOGD("Found partition %s at %s", partition_name.c_str(), path.c_str());
                return path;
            }
        }
    }

    LOGW("Partition %s not found", partition_name.c_str());
    return "";
}

bool is_partition_logical(const std::string& partition_name) {
    // Check if partition is in mapper (logical partitions use device-mapper)
    const std::string block_dev = find_partition_block_device(partition_name);
    if (block_dev.empty()) {
        return false;
    }

    // Logical partitions are typically in /dev/block/mapper
    return block_dev.find("/dev/block/mapper/") == 0;
}

PartitionInfo get_partition_info(const std::string& partition_name,
                                 const std::string& slot_suffix) {
    PartitionInfo info;
    info.name = partition_name;
    info.block_device = find_partition_block_device(partition_name, slot_suffix);
    info.slot_suffix = get_block_device_slot_suffix(info.block_device);
    info.exists = !info.block_device.empty() && is_block_device_path(info.block_device);

    // 基于实际的块设备路径判断是否为逻辑分区
    info.is_logical =
        !info.block_device.empty() && info.block_device.find("/dev/block/mapper/") == 0;

    if (info.exists) {
        info.size = get_file_size(info.block_device);
    } else {
        info.size = 0;
    }

    return info;
}

std::vector<std::string> get_all_partitions(const std::string& slot_suffix) {
    std::vector<std::string> partitions;
    const std::string suffix = slot_suffix.empty() ? get_current_slot_suffix() : slot_suffix;

    // Scan /dev/block/by-name directory for physical partitions
    const std::string by_name_dir = "/dev/block/by-name";
    std::error_code scan_error;
    if (fs::exists(by_name_dir, scan_error) && !scan_error) {
        for (auto it = fs::directory_iterator(by_name_dir, scan_error);
             it != fs::directory_iterator() && !scan_error; it.increment(scan_error)) {
            const auto& entry = *it;
            if (!is_block_device_path(entry.path().string())) {
                LOGD("Skipping non-block by-name entry: %s", entry.path().c_str());
                continue;
            }

            std::string name = entry.path().filename().string();

            // 只有当名字确实以 _a 或 _b 结尾时才去掉槽位后缀
            if (!suffix.empty() && name.length() > 2) {
                const std::string last_two = name.substr(name.length() - 2);
                if (last_two == "_a" || last_two == "_b") {
                    // 确认这确实是槽位后缀
                    if (last_two == suffix) {
                        name = name.substr(0, name.length() - 2);
                    } else {
                        // 不是当前槽位的分区，跳过
                        continue;
                    }
                }
            }

            // Avoid duplicates
            if (std::find(partitions.begin(), partitions.end(), name) == partitions.end()) {
                partitions.push_back(name);
            }
        }
    } else if (scan_error) {
        LOGW("Failed to scan %s: %s", by_name_dir.c_str(), scan_error.message().c_str());
    } else {
        LOGW("Directory %s does not exist", by_name_dir.c_str());
    }

    // Scan /dev/block/mapper directory for logical partitions
    const std::string mapper_dir = "/dev/block/mapper";
    scan_error.clear();
    if (fs::exists(mapper_dir, scan_error) && !scan_error) {
        for (auto it = fs::directory_iterator(mapper_dir, scan_error);
             it != fs::directory_iterator() && !scan_error; it.increment(scan_error)) {
            const auto& entry = *it;
            if (!is_block_device_path(entry.path().string())) {
                LOGD("Skipping non-block mapper entry: %s", entry.path().c_str());
                continue;
            }

            std::string name = entry.path().filename().string();

            // Skip control devices and virtual partitions
            if (name == "control" || name.find("loop") == 0 ||
                name.find("-verity") != std::string::npos ||
                name.find("-cow") != std::string::npos) {
                continue;
            }

            // 只有当名字确实以 _a 或 _b 结尾时才去掉槽位后缀
            if (!suffix.empty() && name.length() > 2) {
                const std::string last_two = name.substr(name.length() - 2);
                if (last_two == "_a" || last_two == "_b") {
                    // 确认这确实是槽位后缀
                    if (last_two == suffix) {
                        name = name.substr(0, name.length() - 2);
                    } else {
                        // 不是当前槽位的分区，跳过
                        continue;
                    }
                }
            }

            // Avoid duplicates
            if (std::find(partitions.begin(), partitions.end(), name) == partitions.end()) {
                partitions.push_back(name);
            }
        }
    }
    if (scan_error) {
        LOGW("Failed to scan %s: %s", mapper_dir.c_str(), scan_error.message().c_str());
    }

    // Sort alphabetically
    std::sort(partitions.begin(), partitions.end());

    LOGD("Found %zu partitions in total", partitions.size());
    return partitions;
}

bool is_dangerous_partition(const std::string& partition_name) {
    return std::any_of(std::begin(DANGEROUS_PARTITIONS), std::end(DANGEROUS_PARTITIONS),
                       [&partition_name](const char* p) { return partition_name == p; });
}

bool is_excluded_from_batch(const std::string& partition_name) {
    return std::any_of(std::begin(EXCLUDED_FROM_BATCH), std::end(EXCLUDED_FROM_BATCH),
                       [&partition_name](const char* p) { return partition_name == p; });
}

std::vector<std::string> get_available_partitions(bool scan_all, const std::string& slot_suffix) {
    std::vector<std::string> available;
    const std::string effective_slot =
        slot_suffix.empty() ? get_current_slot_suffix() : slot_suffix;

    if (scan_all) {
        // Scan all partitions from /dev/block/by-name
        auto all_partitions = get_all_partitions(effective_slot);
        for (const auto& name : all_partitions) {
            const std::string block_dev = find_partition_block_device(name, effective_slot);
            if (!block_dev.empty() && is_block_device_path(block_dev)) {
                available.push_back(name);
            }
        }
    } else {
        // Only check common partitions (silently skip if not found)
        for (const char* name : COMMON_PARTITIONS) {
            const std::string block_dev = find_partition_block_device(name, effective_slot);
            if (!block_dev.empty() && is_block_device_path(block_dev)) {
                available.push_back(name);
            }
            // 找不到也不报错，有些设备可能没有某些常用分区（如recovery）
        }
    }

    return available;
}

std::string flash_physical_partition(const std::string& image_path, const std::string& block_device,
                                     bool verify_hash) {
    LOGI("Flashing %s to %s (physical)", image_path.c_str(), block_device.c_str());

    std::error_code image_error;
    if (!fs::exists(image_path, image_error) || image_error) {
        LOGE("Image file not found: %s", image_path.c_str());
        return "";
    }

    if (!is_block_device_path(block_device)) {
        LOGE("Block device not found: %s", block_device.c_str());
        return "";
    }

    // Check sizes
    const uint64_t image_size = get_file_size(image_path);
    const uint64_t partition_size = get_file_size(block_device);

    if (image_size == 0 || partition_size == 0) {
        LOGE("Image or partition has an invalid zero size");
        return "";
    }

    if (image_size > partition_size) {
        LOGE("Image size (%lu) exceeds partition size (%lu)", image_size, partition_size);
        return "";
    }

    // Open both ends before modifying the target. This avoids clearing a
    // partition only to discover that the source cannot be read.
    const int input_fd = open(image_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (input_fd < 0) {
        LOGE("Failed to open image file: %s", strerror(errno));
        return "";
    }

    const int fd = open(block_device.c_str(), O_WRONLY | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        LOGE("Failed to open block device for writing: %s", strerror(errno));
        close(input_fd);
        return "";
    }

    std::array<char, 65536> buffer{};
    bool success = true;
    mbedtls_sha256_context source_hash_context{};
    std::array<unsigned char, 32> source_digest{};
    mbedtls_sha256_init(&source_hash_context);
    if (verify_hash && mbedtls_sha256_starts(&source_hash_context, 0) != 0) {
        LOGE("Failed to initialize source SHA256");
        mbedtls_sha256_free(&source_hash_context);
        close(fd);
        close(input_fd);
        return "";
    }

    for (;;) {
        const ssize_t read_result = read(input_fd, buffer.data(), buffer.size());
        if (read_result == 0) {
            break;
        }
        if (read_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOGE("Failed while reading image file: %s", strerror(errno));
            success = false;
            break;
        }
        const size_t bytes_read = static_cast<size_t>(read_result);

        if (verify_hash &&
            mbedtls_sha256_update(&source_hash_context,
                                  reinterpret_cast<const unsigned char*>(buffer.data()),
                                  bytes_read) != 0) {
            LOGE("Failed to update source SHA256");
            success = false;
            break;
        }

        size_t written = 0;
        while (written < bytes_read) {
            const ssize_t result = write(fd, buffer.data() + written, bytes_read - written);
            if (result <= 0) {
                LOGE("Write failed: %s", strerror(errno));
                success = false;
                break;
            }
            written += static_cast<size_t>(result);
        }
        if (!success) {
            break;
        }
    }

    // Clear only the unwritten tail after the image has been copied. This keeps
    // stale bytes out without destructively zeroing the target before preflight.
    if (success && image_size < partition_size) {
        LOGD("Zeroing unwritten partition tail");
        std::array<char, 4096> zero_buffer{};
        uint64_t remaining = partition_size - image_size;
        while (remaining > 0) {
            const size_t requested =
                static_cast<size_t>(std::min<uint64_t>(zero_buffer.size(), remaining));
            size_t written = 0;
            while (written < requested) {
                const ssize_t result = write(fd, zero_buffer.data() + written, requested - written);
                if (result <= 0) {
                    LOGE("Failed to zero partition tail: %s", strerror(errno));
                    success = false;
                    break;
                }
                written += static_cast<size_t>(result);
            }
            if (!success) {
                break;
            }
            remaining -= requested;
        }
    }

    if (fsync(fd) != 0) {
        LOGE("Failed to sync block device: %s", strerror(errno));
        success = false;
    }
    close(fd);
    close(input_fd);

    if (verify_hash && mbedtls_sha256_finish(&source_hash_context, source_digest.data()) != 0) {
        LOGE("Failed to finish source SHA256");
        success = false;
    }
    mbedtls_sha256_free(&source_hash_context);

    if (!success) {
        return "";
    }

    if (!verify_hash) {
        LOGI("Flash complete (no verification)");
        sync();
        return "success";
    }

    // Read back exactly the bytes written and compare hashes. The previous
    // implementation only hashed the source and retained the entire image in
    // memory, which could OOM on large dynamic partitions without verifying
    // what reached the block device.
    const int verify_fd = open(block_device.c_str(), O_RDONLY);
    if (verify_fd < 0) {
        LOGE("Failed to open block device for verification: %s", strerror(errno));
        return "";
    }

    mbedtls_sha256_context target_hash_context{};
    std::array<unsigned char, 32> target_digest{};
    mbedtls_sha256_init(&target_hash_context);
    if (mbedtls_sha256_starts(&target_hash_context, 0) != 0) {
        LOGE("Failed to initialize target SHA256");
        mbedtls_sha256_free(&target_hash_context);
        close(verify_fd);
        return "";
    }

    uint64_t remaining = image_size;
    while (remaining > 0) {
        const size_t requested = static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining));
        const ssize_t bytes_read = read(verify_fd, buffer.data(), requested);
        if (bytes_read <= 0) {
            LOGE("Failed to read flashed data for verification: %s", strerror(errno));
            success = false;
            break;
        }
        if (mbedtls_sha256_update(&target_hash_context,
                                  reinterpret_cast<const unsigned char*>(buffer.data()),
                                  static_cast<size_t>(bytes_read)) != 0) {
            LOGE("Failed to update target SHA256");
            success = false;
            break;
        }
        remaining -= static_cast<uint64_t>(bytes_read);
    }
    close(verify_fd);

    if (success && mbedtls_sha256_finish(&target_hash_context, target_digest.data()) != 0) {
        LOGE("Failed to finish target SHA256");
        success = false;
    }
    mbedtls_sha256_free(&target_hash_context);

    if (!success || source_digest != target_digest) {
        LOGE("Flash verification failed: source and target SHA256 differ");
        return "";
    }

    const std::string hash = bytes_to_hex(source_digest.data(), source_digest.size());
    LOGI("Flash verified, SHA256: %s", hash.c_str());
    sync();
    return hash;
}

std::string flash_logical_partition(const std::string& image_path,
                                    const std::string& partition_name,
                                    const std::string& slot_suffix, bool verify_hash) {
    LOGI("Flashing %s to %s%s (logical)", image_path.c_str(), partition_name.c_str(),
         slot_suffix.c_str());

    const uint64_t image_size = get_file_size(image_path);
    if (image_size == 0) {
        LOGE("Invalid image file: %s", image_path.c_str());
        return "";
    }

    const std::string full_partition = partition_name + slot_suffix;
    const std::string temp_partition = partition_name + "_kf";

    // Try to create temporary partition
    LOGD("Creating temporary partition %s", temp_partition.c_str());
    auto cmd = "lptools create " + temp_partition + " " + std::to_string(image_size);
    if (exec_cmd(cmd).find("Created") == std::string::npos) {
        LOGW("Failed to create temp partition, trying resize method");

        // Fallback: resize existing partition
        cmd = "lptools unmap " + full_partition;
        if (exec_cmd(cmd).empty()) {
            LOGE("Failed to unmap %s", full_partition.c_str());
            return "";
        }

        cmd = "lptools resize " + full_partition + " " + std::to_string(image_size);
        if (exec_cmd(cmd).empty()) {
            LOGE("Failed to resize %s", full_partition.c_str());
            return "";
        }

        cmd = "lptools map " + full_partition;
        if (exec_cmd(cmd).empty()) {
            LOGE("Failed to remap %s", full_partition.c_str());
            return "";
        }

        const std::string block_dev = "/dev/block/mapper/" + full_partition;
        return flash_physical_partition(image_path, block_dev, verify_hash);
    }

    // Unmap and remap temp partition
    exec_cmd("lptools unmap " + temp_partition);
    exec_cmd("lptools map " + temp_partition);

    const std::string temp_block_dev = "/dev/block/mapper/" + temp_partition;
    std::string hash = flash_physical_partition(image_path, temp_block_dev, verify_hash);

    if (hash.empty()) {
        LOGE("Failed to flash temporary partition");
        exec_cmd("lptools remove " + temp_partition);
        return "";
    }

    // Replace original partition with temp
    LOGD("Replacing %s with %s", full_partition.c_str(), temp_partition.c_str());
    cmd = "lptools replace " + temp_partition + " " + full_partition;
    if (exec_cmd(cmd).empty()) {
        LOGE("Failed to replace partition");
        exec_cmd("lptools remove " + temp_partition);
        return "";
    }

    return hash;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) image_path vs partition_name are distinct
bool flash_partition(const std::string& image_path, const std::string& partition_name,
                     const std::string& slot_suffix, bool verify_hash) {
    // Use provided slot, or auto-detect if empty
    const std::string suffix = slot_suffix.empty() ? get_current_slot_suffix() : slot_suffix;

    const PartitionInfo info = get_partition_info(partition_name, suffix);
    if (!info.exists) {
        LOGE("Partition %s not found", partition_name.c_str());
        return false;
    }

    std::string hash;
    if (info.is_logical) {
        hash = flash_logical_partition(image_path, partition_name, suffix, verify_hash);
    } else {
        hash = flash_physical_partition(image_path, info.block_device, verify_hash);
    }

    return !hash.empty();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) partition_name vs output_path are distinct
bool backup_partition(const std::string& partition_name, const std::string& output_path,
                      const std::string& slot_suffix) {
    // Use provided slot, or auto-detect if empty
    const std::string suffix = slot_suffix.empty() ? get_current_slot_suffix() : slot_suffix;

    const PartitionInfo info = get_partition_info(partition_name, suffix);
    if (!info.exists) {
        LOGE("Partition %s not found", partition_name.c_str());
        return false;
    }

    LOGI("Backing up %s to %s", partition_name.c_str(), output_path.c_str());

    const fs::path parent = fs::path(output_path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) {
            LOGE("Failed to create backup directory %s: %s", parent.c_str(), ec.message().c_str());
            return false;
        }
    }

    // Use dd for backup and require both a successful command and an exact-size
    // output. A truncated non-empty file must never be reported as a valid backup.
    const std::string cmd = "dd if=" + shell_quote(info.block_device) +
                            " of=" + shell_quote(output_path) + " bs=4096 2>/dev/null && sync";
    const auto result = exec_command_sync({"/system/bin/sh", "-c", cmd});
    const uint64_t output_size = get_file_size(output_path);

    if (result.exit_code == 0 && info.size > 0 && output_size == info.size) {
        LOGI("Backup complete: %s", output_path.c_str());
        return true;
    }

    LOGE("Backup failed: exit=%d, expected=%lu, actual=%lu", result.exit_code,
         static_cast<unsigned long>(info.size), static_cast<unsigned long>(output_size));
    std::error_code remove_error;
    fs::remove(output_path, remove_error);
    return false;
}

bool map_logical_partitions(const std::string& slot_suffix) {
    LOGI("Mapping logical partitions for slot %s", slot_suffix.c_str());

    // Get all partitions from mapper directory
    const std::string mapper_dir = "/dev/block/mapper";
    std::error_code mapper_error;
    if (!fs::exists(mapper_dir, mapper_error) || mapper_error) {
        LOGE("Mapper directory does not exist: %s", mapper_error.message().c_str());
        return false;
    }

    std::vector<std::string> logical_partitions;
    for (auto it = fs::directory_iterator(mapper_dir, mapper_error);
         it != fs::directory_iterator() && !mapper_error; it.increment(mapper_error)) {
        const auto& entry = *it;
        if (!is_block_device_path(entry.path().string())) {
            LOGD("Skipping non-block mapper entry while mapping: %s", entry.path().c_str());
            continue;
        }

        const std::string name = entry.path().filename().string();

        // Skip control devices
        if (name == "control" || name.find("loop") == 0) {
            continue;
        }

        // Only consider partitions for the target slot
        if (!slot_suffix.empty() && name.length() > slot_suffix.length()) {
            const size_t pos = name.find(slot_suffix);
            if (pos != std::string::npos && pos == name.length() - slot_suffix.length()) {
                logical_partitions.push_back(name);
            }
        }
    }
    if (mapper_error) {
        LOGE("Failed to scan mapper directory: %s", mapper_error.message().c_str());
        return false;
    }

    if (logical_partitions.empty()) {
        LOGW("No logical partitions found for slot %s", slot_suffix.c_str());
    }

    // Try to map logical partitions using lptools/dmctl
    int success_count = 0;
    int total_count = 0;

    // Super partition is slotless, don't pass slot_suffix
    const std::string super_device = find_partition_block_device("super", "");
    if (super_device.empty()) {
        LOGW("Super partition not found");
    } else {
        LOGI("Super partition: %s", super_device.c_str());
    }

    // Get list of all potential logical partitions
    const std::array<const char*, 7> common_logical = {
        "system", "vendor", "product", "odm", "system_ext", "vendor_dlkm", "odm_dlkm"};

    for (const char* part_base : common_logical) {
        const std::string part_name = std::string(part_base) + slot_suffix;
        total_count++;

        // Check if already mapped
        const std::string mapped_path = "/dev/block/mapper/" + part_name;
        if (is_block_device_path(mapped_path)) {
            LOGD("Partition %s already mapped", part_name.c_str());
            success_count++;
            continue;
        }

        // Try to map using dmctl (device-mapper control)
        const std::string cmd = "dmctl create " + part_name;
        (void)exec_cmd(cmd);

        if (is_block_device_path(mapped_path)) {
            LOGI("Successfully mapped %s", part_name.c_str());
            success_count++;
        } else {
            LOGD("Could not map %s (may not exist)", part_name.c_str());
        }
    }

    LOGI("Mapped %d/%d logical partitions for slot %s", success_count, total_count,
         slot_suffix.c_str());
    return success_count > 0;
}

std::string get_avb_status() {
    // Check AVB flags in vbmeta
    const std::string vbmeta_device = find_partition_block_device("vbmeta", "");
    if (vbmeta_device.empty()) {
        LOGW("vbmeta partition not found");
        return "";
    }

    // Read vbmeta header flags (offset 123-126)
    const int fd = open(vbmeta_device.c_str(), O_RDONLY);
    if (fd < 0) {
        LOGE("Failed to open vbmeta: %s", strerror(errno));
        return "";
    }

    std::array<unsigned char, 4> flags{};
    if (lseek(fd, 123, SEEK_SET) != 123 || read(fd, flags.data(), flags.size()) != 4) {
        LOGE("Failed to read vbmeta flags");
        close(fd);
        return "";
    }
    close(fd);

    // Check if verification is disabled (flags = 2 or 3)
    if (flags[0] == 0 && flags[1] == 0 && flags[2] == 0 && (flags[3] == 2 || flags[3] == 3)) {
        return "disabled";
    }

    return "enabled";
}

bool patch_vbmeta_disable_verification() {
    const std::string vbmeta_device = find_partition_block_device("vbmeta", "");
    if (vbmeta_device.empty()) {
        LOGE("vbmeta partition not found");
        return false;
    }

    LOGI("Patching vbmeta to disable verification: %s", vbmeta_device.c_str());

    const int fd = open(vbmeta_device.c_str(), O_RDWR);
    if (fd < 0) {
        LOGE("Failed to open vbmeta: %s", strerror(errno));
        return false;
    }

    // Set flags at offset 123 to disable verification (value = 3)
    std::array<unsigned char, 4> flags = {0, 0, 0, 3};

    if (lseek(fd, 123, SEEK_SET) != 123 || write(fd, flags.data(), flags.size()) != 4) {
        LOGE("Failed to write vbmeta flags");
        close(fd);
        return false;
    }

    fsync(fd);
    close(fd);
    sync();

    LOGI("vbmeta patched successfully");
    return true;
}

std::string get_kernel_version(const std::string& slot_suffix) {
    std::string boot_partition_name = "boot";
    if (!find_partition_block_device("init_boot", slot_suffix).empty()) {
        boot_partition_name = "init_boot";
    }

    const std::string device = find_partition_block_device(boot_partition_name, slot_suffix);
    if (device.empty()) {
        LOGE("Could not find boot partition device for slot '%s'", slot_suffix.c_str());
        return "";
    }

    LOGI("Reading kernel version from partition: %s (%s)", device.c_str(),
         boot_partition_name.c_str());

    // Create a temporary directory for unpacking
    std::array<char, 64> tmp_dir_template{};
    (void)strcpy(tmp_dir_template.data(), "/data/local/tmp/ksu_unpack_XXXXXX");
    if (mkdtemp(tmp_dir_template.data()) == nullptr) {
        LOGE("Failed to create temp directory: %s", strerror(errno));
        return "";
    }
    const std::string workdir = tmp_dir_template.data();

    // Resolve the embedded magiskboot applet.
    const std::string magiskboot = find_magiskboot();
    if (magiskboot.empty()) {
        LOGE("magiskboot not found");
        exec_command_sync({"rm", "-rf", workdir});
        return "";
    }

    LOGI("Using magiskboot: %s", magiskboot.c_str());

    // Unpack boot image in the workdir
    const std::string kernel_path = workdir + "/kernel";
    auto unpack_result = exec_command_magiskboot(magiskboot, {"unpack", device}, workdir);

    std::string result;
    if (unpack_result.exit_code == 0) {
        LOGI("Boot image unpacked successfully");

        // Search the unpacked kernel directly. Spawning `strings` first meant a
        // fork+exec and then parsing its full captured stdout; the data is
        // already local and the marker can straddle at most two read chunks.
        const int kernel_fd = open(kernel_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (kernel_fd < 0) {
            LOGE("Failed to open kernel file: %s", strerror(errno));
        } else {
            constexpr std::string_view kMarker = "Linux version ";
            constexpr size_t kMaxScan = 64UL * 1024 * 1024;
            std::array<char, 65536> scan_buffer{};
            std::string pending;
            pending.reserve(scan_buffer.size() + 512);
            size_t total_read = 0;
            while (total_read < kMaxScan && result.empty()) {
                const size_t requested = std::min(scan_buffer.size(), kMaxScan - total_read);
                const ssize_t n = read(kernel_fd, scan_buffer.data(), requested);
                if (n == 0)
                    break;
                if (n < 0) {
                    if (errno == EINTR)
                        continue;
                    LOGE("Failed to read kernel file: %s", strerror(errno));
                    break;
                }
                total_read += static_cast<size_t>(n);
                pending.append(scan_buffer.data(), static_cast<size_t>(n));

                const size_t pos = pending.find(kMarker);
                if (pos != std::string::npos) {
                    const size_t nul = pending.find('\0', pos);
                    const size_t newline = pending.find('\n', pos);
                    const size_t end = std::min(nul, newline);
                    if (end != std::string::npos) {
                        result.assign(pending, pos, end - pos);
                        break;
                    }
                    // Preserve a partial version line until the next chunk.
                    if (pos > 0)
                        pending.erase(0, pos);
                } else if (pending.size() > kMarker.size() - 1) {
                    pending.erase(0, pending.size() - (kMarker.size() - 1));
                }
            }
            close(kernel_fd);
            if (!result.empty())
                LOGI("Found kernel version: %s", result.c_str());
        }
    } else {
        LOGE("magiskboot unpack failed with code %d: %s", unpack_result.exit_code,
             unpack_result.stderr_str.c_str());
        LOGE("stdout: %s", unpack_result.stdout_str.c_str());
    }

    // Cleanup
    exec_command_sync({"rm", "-rf", workdir});

    if (result.empty()) {
        LOGE("Failed to get kernel version");
    }

    return result;
}

std::string get_boot_slot_info() {
    if (!is_ab_device()) {
        return "{\"is_ab\":false}";
    }

    const std::string current_slot = get_current_slot_suffix();
    const std::string other_slot = (current_slot == "_a") ? "_b" : "_a";

    // Get slot info from properties
    auto result_a = exec_command_sync({"getprop", "ro.boot.slot_suffix"});
    auto unbootable = exec_command_sync({"getprop", "ro.boot.slot.unbootable"});
    auto successful = exec_command_sync({"getprop", "ro.boot.slot.successful"});

    std::string json = "{";
    json += "\"is_ab\":true,";
    json += "\"current_slot\":\"" + current_slot + "\",";
    json += "\"other_slot\":\"" + other_slot + "\"";
    json += "}";

    return json;
}

}  // namespace ksud::flash
