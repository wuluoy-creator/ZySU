#include "preinit_platform.hpp"
#include "../core/restorecon.hpp"
#include "../log.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <system_error>

namespace ksud::preinit::platform {
namespace {
void fail(const char* action, const std::filesystem::path& path) {
    throw std::system_error(errno, std::generic_category(),
                            std::string(action) + " " + path.string());
}
}  // namespace

void secure(const std::filesystem::path& path, unsigned mode, const char* context) {
    if (lchown(path.c_str(), 0, 0) != 0)
        fail("chown", path);
    if (chmod(path.c_str(), mode & 0755) != 0)
        fail("chmod", path);
    if (!lsetfilecon(path, context))
        LOGW("preinit: failed to label %s", path.c_str());
}

void sync_path(const std::filesystem::path& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        fail("open for sync", path);
    int result;
    do {
        result = fsync(fd);
    } while (result < 0 && errno == EINTR);
    const int saved = errno;
    close(fd);
    if (result < 0) {
        errno = saved;
        fail("fsync", path);
    }
}

void replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (rename(from.c_str(), to.c_str()) != 0)
        fail("rename", to);
}

uint64_t available(const std::filesystem::path& path) {
    struct statvfs status{};
    if (statvfs(path.c_str(), &status) != 0)
        fail("statvfs", path);
    return static_cast<uint64_t>(status.f_bavail) * status.f_frsize;
}

uint64_t allocated(const std::filesystem::path& path) {
    struct stat status{};
    if (lstat(path.c_str(), &status) != 0)
        fail("lstat", path);
    return static_cast<uint64_t>(status.st_blocks) * 512;
}

unsigned mode(const std::filesystem::path& path) {
    struct stat status{};
    if (lstat(path.c_str(), &status) != 0)
        fail("lstat", path);
    return status.st_mode & 07777;
}
}  // namespace ksud::preinit::platform
