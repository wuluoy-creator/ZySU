#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include "manager_helper.hpp"
#include "userspace/common/manager_protocol.hpp"

namespace {
#define KSUD_PATH "/data/adb/ksud"
#define KSU_WORKING_DIR "/data/adb/ksu"
#define KSU_BINARY_DIR "/data/adb/ksu/bin"
#define KSU_LOG_DIR "/data/adb/ksu/log"
#define KSUD_MAX_SIZE (128ULL * 1024ULL * 1024ULL)

const char kSelinuxXattr[] = "security.selinux";
const char kKsuFileContext[] = "u:object_r:ksu_file:s0";

bool read_exact_at(int descriptor, void* buffer, size_t size, off_t offset) {
    size_t total = 0;
    while (total < size) {
        ssize_t const count = pread(descriptor, static_cast<char*>(buffer) + total, size - total,
                                    offset + (off_t)total);
        if (count > 0) {
            total += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool write_all(int descriptor, const void* buffer, size_t size) {
    size_t total = 0;
    while (total < size) {
        ssize_t const count =
            write(descriptor, static_cast<const char*>(buffer) + total, size - total);
        if (count > 0) {
            total += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool has_daemon_context(int descriptor) {
    char context[128] = {0};
    ssize_t const size = fgetxattr(descriptor, kSelinuxXattr, context, sizeof(context) - 1);
    if (size <= 0 || (size_t)size >= sizeof(context)) {
        return false;
    }
    context[size] = '\0';
    return strcmp(context, kKsuFileContext) == 0;
}

int verify_ksud(const char* source_path) {
    int const source = open(source_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source < 0) {
        return -1;
    }

    struct stat source_status = {};
    if (fstat(source, &source_status) != 0 || !S_ISREG(source_status.st_mode) ||
        source_status.st_size <= 0 || (uint64_t)source_status.st_size > KSUD_MAX_SIZE) {
        close(source);
        return -1;
    }

    int const installed = open(KSUD_PATH, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (installed < 0) {
        close(source);
        return 0;
    }

    struct stat installed_status = {};
    bool matches = fstat(installed, &installed_status) == 0 && S_ISREG(installed_status.st_mode) &&
                   installed_status.st_uid == 0 && installed_status.st_gid == 0 &&
                   (installed_status.st_mode & 0777) == 0755 &&
                   installed_status.st_size == source_status.st_size &&
                   has_daemon_context(installed);

    uint8_t source_buffer[65536];
    uint8_t installed_buffer[65536];
    off_t offset = 0;
    while (matches && offset < source_status.st_size) {
        size_t count = (size_t)(source_status.st_size - offset);
        count = std::min(count, sizeof(source_buffer));
        matches = read_exact_at(source, source_buffer, count, offset) &&
                  read_exact_at(installed, installed_buffer, count, offset) &&
                  memcmp(source_buffer, installed_buffer, count) == 0;
        offset += (off_t)count;
    }

    close(installed);
    close(source);
    return matches ? 1 : 0;
}

bool ensure_directory(const char* path, mode_t mode) {
    bool const created = mkdir(path, mode) == 0;
    if (!created) {
        if (errno != EEXIST) {
            return false;
        }
    }
    struct stat status = {};
    if (lstat(path, &status) != 0 || !S_ISDIR(status.st_mode)) {
        return false;
    }
    return !created || (chown(path, 0, 0) == 0 && chmod(path, mode) == 0);
}

bool repair_tool_links() {
    if (!ensure_directory(KSU_WORKING_DIR, 0755) || !ensure_directory(KSU_BINARY_DIR, 0755) ||
        !ensure_directory(KSU_LOG_DIR, 0755)) {
        return false;
    }

    int const directory = open(KSU_BINARY_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) {
        return false;
    }

    static const char* const applets[] = {"ksud", "magiskboot", "bootctl", "resetprop"};
    bool success = true;
    for (const auto* applet : applets) {
        char temporary[NAME_MAX];
        int const length = snprintf(temporary, sizeof(temporary), ".%s.zysu.tmp.%ld.%ld", applet,
                                    (long)getpid(), syscall(__NR_gettid));
        if (length <= 0 || (size_t)length >= sizeof(temporary)) {
            success = false;
            break;
        }
        unlinkat(directory, temporary, 0);
        if (symlinkat(KSUD_PATH, directory, temporary) != 0 ||
            renameat(directory, temporary, directory, applet) != 0) {
            unlinkat(directory, temporary, 0);
            success = false;
            break;
        }
    }
    if (success) {
        success = fsync(directory) == 0;
    }
    close(directory);
    return success;
}

bool copy_source_to_temp(int source, int directory, const char* temporary) {
    int const output =
        openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0) {
        return false;
    }

    uint8_t buffer[65536];
    bool success = true;
    for (;;) {
        ssize_t const count = read(source, buffer, sizeof(buffer));
        if (count > 0) {
            if (!write_all(output, buffer, (size_t)count)) {
                success = false;
                break;
            }
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            success = false;
        }
        break;
    }

    if (success) {
        success =
            fchown(output, 0, 0) == 0 && fchmod(output, 0755) == 0 &&
            fsetxattr(output, kSelinuxXattr, kKsuFileContext, sizeof(kKsuFileContext), 0) == 0 &&
            fsync(output) == 0;
    }
    close(output);
    return success;
}

bool install_ksud(const char* source_path) {
    int const source = open(source_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source < 0) {
        return false;
    }
    struct stat status = {};
    if (fstat(source, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0 ||
        (uint64_t)status.st_size > KSUD_MAX_SIZE) {
        close(source);
        return false;
    }

    int const directory = open("/data/adb", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) {
        close(source);
        return false;
    }

    char temporary[NAME_MAX] = {0};
    int const length = snprintf(temporary, sizeof(temporary), ".ksud.zysu.tmp.%ld.%ld",
                                (long)getpid(), syscall(__NR_gettid));
    bool success = length > 0 && (size_t)length < sizeof(temporary);
    if (success) {
        unlinkat(directory, temporary, 0);
        success = copy_source_to_temp(source, directory, temporary);
    }
    close(source);

    if (success) {
        success = renameat(directory, temporary, directory, "ksud") == 0 && fsync(directory) == 0;
    }
    if (!success) {
        unlinkat(directory, temporary, 0);
    }
    close(directory);

    if (!success) {
        return false;
    }
    (void)repair_tool_links();
    return verify_ksud(source_path) == 1;
}

}  // namespace
namespace ksud {
std::string manager_integrity(zysu::manager::Operation operation, const std::string& source) {
    using zysu::manager::Operation;
    switch (operation) {
    case Operation::Verify: {
        const int result = verify_ksud(source.c_str());
        if (result < 0)
            return "0";
        return result ? "1" : "2";
    }
    case Operation::Install:
        return install_ksud(source.c_str()) ? "1" : "0";
    case Operation::RepairLinks:
        return repair_tool_links() ? "1" : "0";
    default:
        zysu::manager::fail("Invalid integrity operation", EINVAL);
    }
    return "0";
}
}  // namespace ksud
