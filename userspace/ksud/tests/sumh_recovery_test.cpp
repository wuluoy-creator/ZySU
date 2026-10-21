// Execute the production recovery state machine against a fault-injectable
// descriptor store. No root, device, mount operation or real reboot is used.
#include "core/json.hpp"
#include "mount/backend.hpp"

#include <cassert>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

namespace {
namespace fs = std::filesystem;
constexpr int O_RDONLY = 1, O_RDWR = 2, O_CREAT = 4, O_CLOEXEC = 8;
constexpr int O_NOFOLLOW = 16, O_NONBLOCK = 32, LOCK_EX = 1, LOCK_SH = 2;
constexpr int regular_mode = 1;
using ssize_t = std::ptrdiff_t;
struct RecoveryStat {
    int st_mode = regular_mode;
    int st_nlink = 1;
    int64_t st_size = 0;
};
struct File {
    std::string content;
    int mode = regular_mode;
    int links = 1;
};
struct Descriptor {
    std::string path;
    size_t offset = 0;
};
std::map<std::string, File> files;
std::map<int, Descriptor> descriptors;
int next_fd = 3;
std::string boot_id = "boot-a";
std::string failure_operation, failure_name;
int failure_errno = EIO;
int sync_calls = 0, fail_sync_call = 0;
bool fail_after_replace = false;

std::string path_key(const fs::path& path) {
    return path.generic_string();
}
std::string key(const std::string& name) {
    return path_key(fs::path("/fixture/run") / name);
}
void put(const std::string& name, const std::string& value) {
    files[key(name)].content = value;
}
bool inject(const std::string& operation, const std::string& path) {
    if (operation != failure_operation || (!failure_name.empty() && key(failure_name) != path))
        return false;
    errno = failure_errno;
    return true;
}
void fail(const std::string& operation, const std::string& name, int code) {
    failure_operation = operation;
    failure_name = name;
    failure_errno = code;
}
void reset() {
    files.clear();
    descriptors.clear();
    next_fd = 3;
    boot_id = "boot-a";
    failure_operation.clear();
    failure_name.clear();
    sync_calls = fail_sync_call = 0;
    fail_after_replace = false;
}

template <typename Character>
int fake_open(const Character* path, int flags, int = 0) {
    const auto name = path_key(fs::path(path));
    assert((flags & O_NOFOLLOW) && (flags & O_NONBLOCK) && (flags & O_CLOEXEC));
    if (inject("open", name))
        return -1;
    if (!files.count(name)) {
        if (flags & O_CREAT)
            files[name] = {};
        else {
            errno = ENOENT;
            return -1;
        }
    }
    descriptors[next_fd] = {name, 0};
    return next_fd++;
}
int fake_close(int fd) {
    descriptors.erase(fd);
    return 0;
}
int fake_fstat(int fd, RecoveryStat* st) {
    const auto& name = descriptors.at(fd).path;
    if (inject("stat", name))
        return -1;
    const auto& file = files.at(name);
    st->st_size = static_cast<int64_t>(file.content.size());
    st->st_mode = file.mode;
    st->st_nlink = file.links;
    return 0;
}
ssize_t fake_read(int fd, void* buffer, size_t size) {
    auto& descriptor = descriptors.at(fd);
    if (inject("read", descriptor.path))
        return -1;
    const auto& value = files.at(descriptor.path).content;
    const size_t length = std::min(size, value.size() - descriptor.offset);
    std::memcpy(buffer, value.data() + descriptor.offset, length);
    descriptor.offset += length;
    return static_cast<ssize_t>(length);
}
int fake_flock(int fd, int) {
    return inject("lock", descriptors.at(fd).path) ? -1 : 0;
}
template <typename Character>
int fake_unlink(const Character* path) {
    const auto name = path_key(fs::path(path));
    if (inject("unlink", name))
        return -1;
    if (!files.erase(name)) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}
}  // namespace

namespace sumhp {
fs::path runtime_data_dir() {
    return "/fixture";
}
std::string runtime_boot_id() {
    return boot_id;
}
bool prepare_private_directory(const fs::path& path, std::string& error) {
    if (!inject("mkdir", path_key(path)))
        return true;
    error = std::strerror(errno);
    return false;
}
bool sync_private_directory(const fs::path&, std::string& error) {
    if (++sync_calls != fail_sync_call)
        return true;
    error = "injected directory fsync EIO";
    return false;
}
}  // namespace sumhp
namespace ksud {
bool write_file_durable(const fs::path& path, const std::string& content, std::string& error, int) {
    const auto name = path_key(path);
    if (!fail_after_replace && inject("write", name)) {
        error = "file not replaced: " + std::string(std::strerror(errno));
        return false;
    }
    files[name] = {content};
    if ((fail_after_replace && inject("write", name)) ||
        !sumhp::sync_private_directory(path.parent_path(), error)) {
        error = "file replaced but durability is unconfirmed: " + error;
        return false;
    }
    return true;
}
}  // namespace ksud

#define open fake_open
#define close fake_close
#define fstat fake_fstat
#define stat RecoveryStat
#define read fake_read
#define flock fake_flock
#define unlink fake_unlink
#define S_ISREG(mode) ((mode) == regular_mode)
#include "sumh_recovery_under_test.inc"
#undef open
#undef close
#undef fstat
#undef stat
#undef read
#undef flock
#undef unlink
#undef S_ISREG

namespace sumhp {
std::string arg_or_default(const std::vector<std::string>& args, size_t index,
                           const std::string& fallback) {
    return index < args.size() ? args[index] : fallback;
}
#include "sumh_recovery_command_under_test.inc"
}  // namespace sumhp

int main() {
    using namespace sumhp::mount;
    std::string error;
    bool disabled = false, ok = false;

    reset();
    auto status = recovery_status_json(&ok);
    assert(ok && files.empty());  // status is read-only, including first launch
    assert(status.find("\"counter_state\":\"missing\"") != std::string::npos);
    assert(recovery_begin_mount(disabled, error) && !disabled);
    status = recovery_status_json(&ok);
    assert(ok && status.find("\"boot_attempts\":1") != std::string::npos);
    assert(status.find("\"phase\":\"mount-attempt\"") != std::string::npos);
    assert(status.find("\"boot_id\":\"boot-a\"") != std::string::npos);
    boot_id = "boot-b";
    assert(recovery_begin_mount(disabled, error) && !disabled);
    boot_id = "boot-c";
    assert(recovery_begin_mount(disabled, error) && !disabled);
    boot_id = "boot-d";
    assert(recovery_begin_mount(disabled, error) && disabled);
    assert(files.count(key("mount_disabled")));
    put("mount_orchestrator_boot", "boot-d\n");
    assert(recovery_boot_completed(error));
    status = recovery_status_json(&ok);
    assert(ok && status.find("\"boot_attempts\":0") != std::string::npos);
    assert(status.find("\"mounting_disabled\":true") != std::string::npos);
    assert(recovery_reset(error));
    assert(!files.count(key("mount_disabled")));
    status = recovery_status_json(&ok);
    assert(ok && status.find("\"phase\":\"reset\"") != std::string::npos);

    // Accept legacy records, but never turn malformed or unreadable data into 0.
    for (const std::string invalid : {"", "-1\n", "junk\n", "2junk\n", "9999999999999\n",
                                      "2\nunknown-format\n", "2\nsumhp-recovery-v1\n"}) {
        reset();
        put("boot_attempts", invalid);
        assert(!recovery_begin_mount(disabled, error) && disabled && !error.empty());
        status = recovery_status_json(&ok);
        assert(!ok && status.find("\"boot_attempts\":null") != std::string::npos);
        assert(status.find("\"counter_state\":\"corrupt\"") != std::string::npos);
        assert(recovery_reset(error));  // explicit reset repairs a malformed counter
    }
    reset();
    put("boot_attempts", "2\n");
    assert(recovery_begin_mount(disabled, error) && !disabled);
    assert(files.at(key("boot_attempts")).content.rfind("3\n", 0) == 0);
    put("mount_disabled", "\n");
    assert(recovery_begin_mount(disabled, error) && disabled);

    for (int code : {ENOSPC, EIO, EROFS}) {
        reset();
        put("boot_attempts", "2\n");
        fail("write", "boot_attempts", code);
        assert(!recovery_begin_mount(disabled, error) && disabled);
        assert(files.at(key("boot_attempts")).content == "2\n");
        assert(!recovery_reset(error));
        put("mount_orchestrator_boot", "boot-a\n");
        assert(!recovery_boot_completed(error));

        reset();
        put("boot_attempts", "3\n");
        fail("write", "mount_disabled", code);
        assert(!recovery_begin_mount(disabled, error) && disabled);
        assert(!files.count(key("mount_disabled")));
        assert(files.at(key("boot_attempts")).content == "3\n");

        reset();
        put("mount_disabled", "\n");
        fail("unlink", "mount_disabled", code);
        assert(!recovery_reset(error));
        assert(files.count(key("mount_disabled")));
        failure_operation.clear();
        assert(recovery_reset(error));
    }
    for (const std::string operation : {"open", "stat", "read"}) {
        reset();
        put("boot_attempts", "2\n");
        fail(operation, "boot_attempts", EIO);
        assert(!recovery_begin_mount(disabled, error) && disabled);
        status = recovery_status_json(&ok);
        assert(!ok && status.find("\"counter_state\":\"unreadable\"") != std::string::npos);
    }
    reset();
    put("mount_disabled", "\n");
    fail("open", "mount_disabled", EACCES);
    assert(!recovery_begin_mount(disabled, error) && disabled);
    status = recovery_status_json(&ok);
    assert(!ok && status.find("\"mounting_disabled\":null") != std::string::npos);

    reset();
    put("boot_attempts", "2\n");
    fail_sync_call = 1;
    assert(!recovery_begin_mount(disabled, error) && disabled);
    assert(error.find("durability") != std::string::npos);
    // Rename may already be visible after a failed directory sync; no false success.
    assert(files.at(key("boot_attempts")).content.rfind("3\n", 0) == 0);

    reset();
    put("mount_disabled", "\n");
    fail_sync_call = 2;
    assert(!recovery_reset(error) && !files.count(key("mount_disabled")));
    assert(error.find("durability") != std::string::npos);
    assert(recovery_reset(error));

    reset();
    put("boot_attempts", "2\n");
    put("mount_orchestrator_boot", "previous-boot\n");
    assert(recovery_boot_completed(error));
    assert(files.at(key("boot_attempts")).content == "2\n");
    put("mount_orchestrator_boot", "boot-a\n");
    assert(recovery_boot_completed(error));
    assert(files.at(key("boot_attempts")).content.rfind("0\n", 0) == 0);

    reset();
    boot_id.clear();
    assert(!recovery_begin_mount(disabled, error) && disabled);
    assert(!recovery_reset(error));
    assert(descriptors.empty());

    reset();
    put("boot_attempts", std::string(4097, '1'));
    assert(!recovery_begin_mount(disabled, error) && disabled);
    reset();
    put("boot_attempts", "2\n");
    files.at(key("boot_attempts")).mode = 0;  // Reject directories/FIFOs/device records.
    assert(!recovery_begin_mount(disabled, error) && disabled);
    reset();
    put("recovery.lock", "");
    files.at(key("recovery.lock")).links = 2;
    assert(!recovery_begin_mount(disabled, error) && disabled);

    // Exercise the actual command handler, including nonzero status/JSON errors.
    reset();
    put("mount_disabled", "\n");
    fail("unlink", "mount_disabled", EROFS);
    std::ostringstream output, diagnostics;
    auto* old_out = std::cout.rdbuf(output.rdbuf());
    auto* old_err = std::cerr.rdbuf(diagnostics.rdbuf());
    assert(sumhp::handle_recovery({"recovery", "reset"}) == 1);
    assert(output.str().find("\"ok\":false") != std::string::npos);
    assert(!diagnostics.str().empty());
    failure_operation.clear();
    output.str("");
    assert(sumhp::handle_recovery({"recovery", "reset"}) == 0);
    assert(output.str().find("\"ok\":true") != std::string::npos);
    put("boot_attempts", "corrupt\n");
    assert(sumhp::handle_recovery({"recovery", "status"}) == 1);
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    std::cout << "SUMH recovery persistence and failure regressions passed\n";
}
