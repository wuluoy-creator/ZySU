// Production atomic/durable replacement against a descriptor and crash model.
// Every syscall can fail independently; no Android/device or POSIX host needed.
#include "durable_file.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr int O_RDONLY = 1, O_DIRECTORY = 2, O_CLOEXEC = 4, O_NOFOLLOW = 8;
constexpr int directory_fd = 41, file_fd = 42;
std::string visible, staging, synced_file, durable_name;
std::vector<std::string> calls;
std::string fail_at, interrupt_at;
bool directory_open, file_open, temporary_exists;
mode_t staged_mode, final_mode;
size_t max_write;
size_t write_calls;

void reset() {
    visible = synced_file = durable_name = "old";
    staging.clear();
    calls.clear();
    fail_at.clear();
    interrupt_at.clear();
    directory_open = file_open = temporary_exists = false;
    staged_mode = final_mode = 0600;
    max_write = 2;
    write_calls = 0;
}
bool fail(const std::string& operation) {
    calls.push_back(operation);
    if (interrupt_at == operation) {
        interrupt_at.clear();
        errno = EINTR;
        return true;
    }
    if (fail_at == operation) {
        errno = EIO;
        return true;
    }
    return false;
}
template <typename Character>
int fake_open(const Character*, int flags) {
    assert(flags == (O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (fail("open-directory"))
        return -1;
    directory_open = true;
    return directory_fd;
}
int fake_mkostemp(char*, int flags) {
    assert(flags == O_CLOEXEC && directory_open);
    if (fail("create"))
        return -1;
    temporary_exists = file_open = true;
    staging.clear();
    return file_fd;
}
std::ptrdiff_t fake_write(int fd, const char* data, size_t length) {
    assert(fd == file_fd && file_open);
    if (fail("write"))
        return -1;
    if (++write_calls == 2 && fail_at == "partial-write") {
        errno = EIO;
        return -1;
    }
    if (fail_at == "zero-write")
        return 0;
    const auto count = std::min(length, max_write);
    staging.append(data, count);
    return static_cast<std::ptrdiff_t>(count);
}
int fake_fchmod(int fd, mode_t mode) {
    assert(fd == file_fd && file_open);
    if (fail("chmod"))
        return -1;
    staged_mode = mode;
    return 0;
}
int fake_fsync(int fd) {
    assert(fd == file_fd ? file_open : directory_open);
    if (fail(fd == file_fd ? "sync-file" : "sync-directory"))
        return -1;
    if (fd == file_fd)
        synced_file = staging;
    else
        durable_name = visible;
    return 0;
}
int fake_close(int fd) {
    auto& opened = fd == file_fd ? file_open : directory_open;
    assert(opened);
    opened = false;  // close must not be retried after EINTR/error.
    return fail(fd == file_fd ? "close-file" : "close-directory") ? -1 : 0;
}
template <typename Character>
int fake_rename(const char*, const Character*) {
    assert(temporary_exists && !file_open && synced_file == staging);
    if (fail("rename"))
        return -1;
    visible = staging;
    final_mode = staged_mode;
    temporary_exists = false;
    return 0;
}
int fake_unlink(const char*) {
    assert(temporary_exists);
    temporary_exists = false;
    calls.push_back("unlink");
    errno = EACCES;  // cleanup must not overwrite the original failure.
    return 0;
}
}  // namespace

#define open fake_open
#define mkostemp fake_mkostemp
#define write fake_write
#define fchmod fake_fchmod
#define fsync fake_fsync
#define close fake_close
#define rename fake_rename
#define unlink fake_unlink
namespace ksud {
#include "durable_file_under_test.inc"
}
#undef open
#undef mkostemp
#undef write
#undef fchmod
#undef fsync
#undef close
#undef rename
#undef unlink

int main() {
    using State = ksud::DurableWriteState;
    const std::filesystem::path path("/state/config.json");
    const std::string content = "complete new value";
    for (const std::string operation :
         {"open-directory", "create", "write", "zero-write", "partial-write", "chmod", "sync-file",
          "close-file", "rename", "sync-directory", "close-directory"}) {
        reset();
        fail_at = operation;
        const auto result = ksud::replace_file_durable(path, content, 0640);
        const bool replaced = operation == "sync-directory" || operation == "close-directory";
        assert(result.state ==
               (replaced ? State::ReplacedDurabilityUnconfirmed : State::NotCommitted));
        assert(result.replaced() == replaced && !result.complete());
        assert(result.error_number == EIO && errno == EIO);
        assert(visible == (replaced ? content : "old"));
        assert(final_mode == (replaced ? 0640 : 0600));
        assert(!temporary_exists && !file_open && !directory_open);
        // A simulated crash chooses only a whole old or whole new name.
        assert(durable_name == "old" || durable_name == content);
        if (operation == "close-directory")
            assert(durable_name == content);
    }
    for (const std::string operation : {"write", "sync-file", "sync-directory"}) {
        reset();
        interrupt_at = operation;
        const auto result = ksud::replace_file_durable(path, content);
        assert(result.complete() && result.error_number == 0);
        assert(visible == content && durable_name == content);
        assert(!temporary_exists && !file_open && !directory_open);
    }
    reset();
    assert(ksud::replace_file_durable(path, "").complete());
    assert(visible.empty() && durable_name.empty());
    for (const bool after_rename : {false, true}) {
        reset();
        fail_at = after_rename ? "sync-directory" : "rename";
        std::string error;
        assert(!ksud::write_file_durable(path, content, error));
        assert(error.find(after_rename ? "file replaced but durability is unconfirmed"
                                       : "file not replaced") != std::string::npos);
        assert(error.find(path.string()) != std::string::npos);
    }
    reset();
    std::string error = "old failure";
    assert(ksud::write_file_durable("config.json", content, error));
    assert(error.empty() && durable_name == content);
    std::cout << "durable_file_test: all checks passed\n";
}
