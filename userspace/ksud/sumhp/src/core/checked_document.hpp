#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace sumhp::checked {
inline constexpr std::size_t document_limit = 4 * 1024 * 1024;
struct Document {
    std::string status, bytes;
    int error = 0;
    bool operator==(const Document& other) const {
        return status == other.status && bytes == other.bytes && error == other.error;
    }
};
// No mkdir, migration, logging or defaults. The caller owns namespace/locking.
inline Document read_document(const std::filesystem::path& path,
                              std::size_t limit = document_limit) {
    struct Fd {
        int value;
        ~Fd() {
            if (value >= 0)
                ::close(value);
        }
    } fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (fd.value < 0)
        return {errno == ENOENT ? "missing" : "unreadable", {}, errno ? errno : EIO};
    struct stat metadata{};
    if (::fstat(fd.value, &metadata) != 0)
        return {"unreadable", {}, errno ? errno : EIO};
    if (!S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 || metadata.st_size < 0)
        return {"unreadable", {}, EINVAL};
    if (static_cast<std::uint64_t>(metadata.st_size) > limit)
        return {"unreadable", {}, EOVERFLOW};
    Document out{"valid", {}, 0};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    char buffer[4096];
    while (out.bytes.size() <= limit) {
        if (std::chrono::steady_clock::now() >= deadline)
            return {"unreadable", {}, ETIMEDOUT};
        const auto count =
            ::read(fd.value, buffer, std::min(sizeof(buffer), limit + 1 - out.bytes.size()));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return {"unreadable", {}, errno ? errno : EIO};
        if (!count)
            break;
        out.bytes.append(buffer, static_cast<std::size_t>(count));
    }
    if (out.bytes.size() > limit)
        return {"unreadable", {}, EOVERFLOW};
    return out;
}
}  // namespace sumhp::checked
