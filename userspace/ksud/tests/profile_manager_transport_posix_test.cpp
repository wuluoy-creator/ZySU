#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <string>
#include "userspace/common/json.hpp"
#include "userspace/common/profile_manager_transport.hpp"

namespace {
struct File {
    int fd;
    File() {
        char name[] = "/tmp/sumh-profile-transfer-XXXXXX";
        fd = ::mkstemp(name);
        assert(fd >= 0);
        assert(::unlink(name) == 0);
    }
    ~File() { ::close(fd); }
    void put(const std::string& bytes) {
        assert(::ftruncate(fd, 0) == 0);
        std::size_t at = 0;
        while (at < bytes.size()) {
            const auto n =
                ::pwrite(fd, bytes.data() + at, bytes.size() - at, static_cast<off_t>(at));
            assert(n > 0);
            at += static_cast<std::size_t>(n);
        }
    }
    std::string get() const {
        struct stat st{};
        assert(::fstat(fd, &st) == 0);
        std::string bytes(static_cast<std::size_t>(st.st_size), '\0');
        std::size_t at = 0;
        while (at < bytes.size()) {
            const auto n =
                ::pread(fd, bytes.data() + at, bytes.size() - at, static_cast<off_t>(at));
            assert(n > 0);
            at += static_cast<std::size_t>(n);
        }
        return bytes;
    }
};
}  // namespace
int main(int argc, char** argv) {
    namespace transport = sumhp::profile_transport;
    if (argc == 5) {
        assert(std::string(argv[1]) == "sumhp" && std::string(argv[2]) == "profile" &&
               std::string(argv[3]) == "manager" && std::string(argv[4]) == "--channel");
        return transport::serve([](const std::string& input) {
            if (const auto* marker = ::getenv("SUMH_PROFILE_TEST_MARKER")) {
                ::usleep(250000);
                const int fd = ::open(marker, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
                assert(fd >= 0);
                assert(::write(fd, "completed", 9) == 9);
                assert(::fsync(fd) == 0);
                ::close(fd);
            }
            if (input == "large-output")
                return std::string("{\"data\":\"") + std::string(24 * 1024 * 1024 - 11, 'x') +
                       "\"}";
            return std::string("{\"bytes\":") + std::to_string(input.size()) +
                   ",\"unicode\":\"方案\"}";
        });
    }
    char executable[4096];
    const auto count = ::readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    assert(count > 0);
    executable[count] = 0;
    File input, output;
    input.put("simple");
    auto receipt = json::parse(transport::request(executable, input.fd, output.fd));
    if (receipt.at("errno").n == ENOSYS || receipt.at("errno").n == EINVAL ||
        receipt.at("errno").n == EPERM) {
        std::cout << "SKIP: host policy lacks required pidfd/P_PIDFD support\n";
        return 0;
    }
    assert(receipt.at("complete").b && json::parse(output.get()).at("unicode").s == "方案");
    input.put(std::string(8 * 1024 * 1024 + 4096, 'x'));
    output.put("");
    receipt = json::parse(transport::request(executable, input.fd, output.fd));
    assert(receipt.at("complete").b &&
           json::parse(output.get()).at("bytes").n == 8 * 1024 * 1024 + 4096);
    input.put("large-output");
    output.put("");
    receipt = json::parse(transport::request(executable, input.fd, output.fd));
    assert(receipt.at("complete").b && output.get().size() == 24 * 1024 * 1024);
    char marker[] = "/tmp/sumh-profile-writer-XXXXXX";
    int reserved = ::mkstemp(marker);
    assert(reserved >= 0);
    ::close(reserved);
    assert(::unlink(marker) == 0);
    assert(::setenv("SUMH_PROFILE_TEST_MARKER", marker, 1) == 0);
    input.put("writer");
    output.put("");
    receipt = json::parse(transport::request(executable, input.fd, output.fd, 80));
    assert(!receipt.at("complete").b && receipt.at("errno").n == ETIMEDOUT && output.get().empty());
    assert(::unsetenv("SUMH_PROFILE_TEST_MARKER") == 0);
    receipt = json::parse(transport::request(executable, input.fd, output.fd));
    assert(receipt.at("errno").n == EBUSY);
    ::usleep(400000);
    reserved = ::open(marker, O_RDONLY | O_CLOEXEC);
    assert(reserved >= 0);
    char bytes[9];
    assert(::read(reserved, bytes, 9) == 9);
    assert(std::string(bytes, 9) == "completed");
    ::close(reserved);
    assert(::unlink(marker) == 0);
    receipt = json::parse(transport::request(executable, input.fd, output.fd));
    assert(receipt.at("complete").b);
    output.put("");
    assert(::fchmod(output.fd, 0644) == 0);
    receipt = json::parse(transport::request(executable, input.fd, output.fd));
    assert(!receipt.at("complete").b && receipt.at("errno").n == EINVAL);
    std::cout << "POSIX fixed exec, large FD transfer, writer survival and pidfd quarantine smoke "
                 "passed\n";
}
