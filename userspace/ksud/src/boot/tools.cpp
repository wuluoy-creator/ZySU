#include "tools.hpp"
#include "../log.hpp"
#include "../utils.hpp"

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ksud {

// Find magiskboot binary: always use current process (multi-call ksud embeds magiskboot).
std::string find_magiskboot() {
    std::array<char, PATH_MAX> self_path{};
    const ssize_t self_len = readlink("/proc/self/exe", self_path.data(), self_path.size() - 1);
    if (self_len <= 0 || static_cast<size_t>(self_len) >= self_path.size()) {
        LOGE("magiskboot (self): readlink /proc/self/exe failed");
        return "";
    }
    self_path[static_cast<size_t>(self_len)] = '\0';
    if (access(self_path.data(), X_OK) != 0) {
        LOGE("magiskboot (self): not executable: %s", self_path.data());
        return "";
    }
    printf("- Using magiskboot: %s (self)\n", self_path.data());
    return {self_path.data()};
}

// Copy a whole image, replacing the previous `dd` exec. Beyond dropping a PATH
// lookup and a fork+exec of toybox per call, this removes a real hazard: the old
// code retried WITHOUT conv=fsync when the first attempt failed, so a boot
// partition could be left unflushed before a reboot. Here the fsync is not
// optional.
//
// copy_file_range/sendfile are not usable: either endpoint may be a block device
// (a boot partition), which neither supports.
bool exec_dd(const std::string& input, const std::string& output) {
    const int in_fd = open(input.c_str(), O_RDONLY | O_CLOEXEC);
    if (in_fd < 0) {
        LOGE("dd: cannot open %s: %s", input.c_str(), strerror(errno));
        return false;
    }

    // Open without truncation so aliases (including hard links and partition
    // symlinks) can be checked against the actual opened source first.
    const int out_fd = open(output.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (out_fd < 0) {
        LOGE("dd: cannot open %s for writing: %s", output.c_str(), strerror(errno));
        close(in_fd);
        return false;
    }

    struct stat src_st{};
    struct stat dst_st{};
    if (fstat(in_fd, &src_st) != 0 || fstat(out_fd, &dst_st) != 0) {
        LOGE("dd: cannot stat source or destination: %s", strerror(errno));
        close(out_fd);
        close(in_fd);
        return false;
    }
    if ((src_st.st_dev == dst_st.st_dev && src_st.st_ino == dst_st.st_ino) ||
        (S_ISBLK(src_st.st_mode) && S_ISBLK(dst_st.st_mode) && src_st.st_rdev == dst_st.st_rdev)) {
        LOGE("dd: source and destination refer to the same file or device");
        close(out_fd);
        close(in_fd);
        return false;
    }
    if (S_ISBLK(dst_st.st_mode)) {
        const auto block_size = [](int fd, std::uint64_t* size) {
            int result;
            do {
                result = ioctl(fd, BLKGETSIZE64, size);
            } while (result < 0 && errno == EINTR);
            return result == 0;
        };
        std::uint64_t capacity = 0;
        std::uint64_t image_size = 0;
        bool known_image_size = false;
        if (S_ISREG(src_st.st_mode) && src_st.st_size >= 0) {
            image_size = static_cast<std::uint64_t>(src_st.st_size);
            known_image_size = true;
        } else if (S_ISBLK(src_st.st_mode)) {
            known_image_size = block_size(in_fd, &image_size);
        }
        if (!known_image_size || !block_size(out_fd, &capacity) || image_size == 0 ||
            image_size > capacity) {
            LOGE("dd: image size is unknown, empty, or exceeds the capacity of %s", output.c_str());
            close(out_fd);
            close(in_fd);
            return false;
        }
    }
    // Only regular images are truncated; partitions retain their capacity.
    if (S_ISREG(dst_st.st_mode) && ftruncate(out_fd, 0) != 0) {
        LOGE("dd: cannot truncate %s: %s", output.c_str(), strerror(errno));
        close(out_fd);
        close(in_fd);
        return false;
    }

    // Matches the old bs=4M throughput without a 4 MiB buffer.
    constexpr size_t kChunk = 1024UL * 1024;
    std::vector<char> buf(kChunk);
    bool ok = true;
    for (;;) {
        const ssize_t n = read(in_fd, buf.data(), buf.size());
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOGE("dd: read %s failed: %s", input.c_str(), strerror(errno));
            ok = false;
            break;
        }
        size_t written = 0;
        while (written < static_cast<size_t>(n)) {
            const ssize_t w = write(out_fd, buf.data() + written, static_cast<size_t>(n) - written);
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                LOGE("dd: write %s failed: %s", output.c_str(), strerror(errno));
                ok = false;
                break;
            }
            if (w == 0) {
                // Not reachable for a non-zero count per POSIX, but a driver that
                // does it would spin this loop forever with the partition half
                // written. Fail the flash instead of hanging.
                LOGE("dd: write %s made no progress", output.c_str());
                ok = false;
                break;
            }
            written += static_cast<size_t>(w);
        }
        if (!ok) {
            break;
        }
    }

    // Flush before reporting success: callers may reboot straight after this.
    int sync_result = 0;
    if (ok) {
        do {
            sync_result = fsync(out_fd);
        } while (sync_result < 0 && errno == EINTR);
    }
    if (sync_result != 0) {
        LOGE("dd: fsync %s failed: %s", output.c_str(), strerror(errno));
        ok = false;
    }
    if (close(out_fd) != 0 && ok) {
        LOGE("dd: close %s failed: %s", output.c_str(), strerror(errno));
        ok = false;
    }
    close(in_fd);
    return ok;
}

// `blockdev --setrw` is a single BLKROSET ioctl. Spawning a process to reach it
// also meant a PATH lookup, and the exit code came back as an opaque number with
// the real errno buried in the child's stderr.
bool set_block_device_rw(const std::string& device) {
    const int fd = open(device.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGW("setrw: cannot open %s: %s", device.c_str(), strerror(errno));
        return false;
    }
    int read_only = 0;
    const bool ok = ioctl(fd, BLKROSET, &read_only) == 0;
    if (!ok) {
        LOGW("setrw: BLKROSET on %s refused: %s", device.c_str(), strerror(errno));
    }
    close(fd);
    return ok;
}

}  // namespace ksud
