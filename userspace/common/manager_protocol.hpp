#pragma once

#include <poll.h>
#include <sys/socket.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// Private, one-request protocol between the bundled Manager and its ksud.
// No listening socket, shell grammar, or third-party API is exposed.
namespace zysu::manager {
constexpr uint32_t magic = 0x4853595a;
constexpr uint32_t version = 1;
constexpr size_t max_payload = 1024 * 1024;
constexpr size_t max_fields = 6;
using Deadline = std::chrono::steady_clock::time_point;
enum class Operation : uint32_t {
    Verify = 1,
    Install,
    RepairLinks,
    Controller,
    Snapshot,
    RetryUserHide,
    SaveSuPath,
    ReadLog,
    StorageInfo,
    ClearLog,
    ClearMaps,
    AddMaps,
};

inline void fail(const char* message, int error = EPROTO) {
    throw std::runtime_error(std::string(message) + ": " + std::strerror(error));
}
inline void append_u32(std::string& out, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<char>((value >> shift) & 0xff));
}
inline uint32_t take_u32(const std::string& data, size_t& offset) {
    if (offset > data.size() || data.size() - offset < 4)
        fail("Truncated helper field");
    uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        value |= static_cast<uint32_t>(static_cast<unsigned char>(data[offset++])) << shift;
    return value;
}
inline std::string encode_fields(const std::vector<std::string>& fields) {
    if (fields.size() > max_fields)
        fail("Too many helper fields");
    std::string out;
    append_u32(out, static_cast<uint32_t>(fields.size()));
    for (const auto& field : fields) {
        if (field.size() > max_payload || out.size() + 4 + field.size() > max_payload)
            fail("Helper request too large", EOVERFLOW);
        append_u32(out, static_cast<uint32_t>(field.size()));
        out += field;
    }
    return out;
}
inline std::vector<std::string> decode_fields(const std::string& data) {
    if (data.size() > max_payload)
        fail("Helper request too large", EOVERFLOW);
    size_t offset = 0;
    const auto count = take_u32(data, offset);
    if (count > max_fields)
        fail("Too many helper fields");
    std::vector<std::string> fields;
    for (uint32_t i = 0; i < count; ++i) {
        const auto length = take_u32(data, offset);
        if (length > data.size() - offset)
            fail("Truncated helper field");
        fields.emplace_back(data.substr(offset, length));
        offset += length;
    }
    if (offset != data.size())
        fail("Trailing helper data");
    return fields;
}
inline void wait_io(int fd, short events, Deadline deadline) {
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0)
            fail("Helper deadline exceeded", ETIMEDOUT);
        pollfd item{fd, events, 0};
        const int ready = poll(&item, 1, static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            fail("Poll helper", errno);
        if (!ready)
            fail("Helper deadline exceeded", ETIMEDOUT);
        if (item.revents & POLLNVAL)
            fail("Invalid helper socket", EBADF);
        return;
    }
}
inline void send_bytes(int fd, const std::string& data, Deadline deadline) {
    size_t offset = 0;
    while (offset < data.size()) {
        wait_io(fd, POLLOUT, deadline);
        const auto count =
            send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count <= 0)
            fail("Send helper request", count < 0 ? errno : EPIPE);
        offset += static_cast<size_t>(count);
    }
}
inline std::string receive_bytes(int fd, size_t size, Deadline deadline) {
    std::string data(size, '\0');
    size_t offset = 0;
    while (offset < size) {
        wait_io(fd, POLLIN, deadline);
        const auto count = recv(fd, data.data() + offset, size - offset, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count <= 0)
            fail("Incomplete helper response", count < 0 ? errno : EPIPE);
        offset += static_cast<size_t>(count);
    }
    return data;
}
struct Frame {
    uint32_t code;
    std::string payload;
};
inline void send_frame(int fd, uint32_t code, const std::string& payload, Deadline deadline) {
    if (payload.size() > max_payload)
        fail("Helper response too large", EOVERFLOW);
    std::string header;
    append_u32(header, magic);
    append_u32(header, version);
    append_u32(header, code);
    append_u32(header, static_cast<uint32_t>(payload.size()));
    send_bytes(fd, header, deadline);
    send_bytes(fd, payload, deadline);
}
inline Frame receive_frame(int fd, Deadline deadline) {
    const auto header = receive_bytes(fd, 16, deadline);
    size_t offset = 0;
    if (take_u32(header, offset) != magic || take_u32(header, offset) != version)
        fail("Incompatible Manager helper");
    const auto code = take_u32(header, offset);
    const auto size = take_u32(header, offset);
    if (size > max_payload)
        fail("Helper response too large", EOVERFLOW);
    return {code, receive_bytes(fd, size, deadline)};
}
}  // namespace zysu::manager
