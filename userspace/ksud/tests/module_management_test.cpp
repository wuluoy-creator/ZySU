// Production functions are extracted by run_module_host_tests.py. The fake
// filesystem/process calls exercise failure paths without touching /data or
// requiring a device, privileges, or POSIX process support on the host.
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifdef WIFEXITED
#undef WIFEXITED
#undef WEXITSTATUS
#endif

namespace fixture {

constexpr auto MODULE_DIR = "/data/adb/modules/";
constexpr auto MODULE_CONFIG_DIR = "/data/adb/ksu/module_configs/";
constexpr auto PERSIST_CONFIG_NAME = "persist.config";
constexpr auto TEMP_CONFIG_NAME = "tmp.config";
constexpr auto REMOVE_FILE_NAME = "remove";
constexpr auto DISABLE_FILE_NAME = "disable";
constexpr auto BINARY_DIR = "/data/adb/ksu/bin/";
constexpr auto INSTALLER_SCRIPT_NAME = "installer.sh";
constexpr auto BUSYBOX_PATH = "/data/adb/ksu/bin/busybox";
constexpr int DT_UNKNOWN = 0, DT_DIR = 4, DT_LNK = 10, DT_REG = 8;
constexpr int AT_SYMLINK_NOFOLLOW = 0x100, O_CLOEXEC = 0x80000;
using pid_t = int;
using mode_t = unsigned int;
struct FILE {};
struct dirent {
    unsigned char d_type;
    char d_name[128];
};
struct DIR {
    size_t offset = 0;
};
struct stat {
    unsigned st_mode;
};

std::vector<dirent> entries;
std::map<std::string, unsigned> entry_modes;
std::set<std::string> files;
std::set<std::string> failed_flags;
std::map<std::string, std::string> contents;
std::map<std::string, mode_t> config_modes;
DIR directory;
int stat_calls = 0, rc_refreshes = 0;
bool missing_directory = false, save_ok = true;
bool wrapper_write_ok = true, wrapper_close_ok = true;
int closes = 0, waits = 0, forks = 0, unlinks = 0;
std::vector<int> wait_errors;
int exit_status = 0;
std::string module_env = "test_module";

void reset() {
    entries = {{DT_DIR, "first"},
               {DT_UNKNOWN, "second"},
               {DT_LNK, "linked"},
               {DT_DIR, ".hidden"},
               {DT_REG, "plain"}};
    entry_modes = {{"second", DT_DIR}};
    files = {std::string(MODULE_DIR) + "first", std::string(MODULE_DIR) + "second",
             std::string(BINARY_DIR) + INSTALLER_SCRIPT_NAME, BUSYBOX_PATH};
    failed_flags.clear();
    contents.clear();
    config_modes.clear();
    stat_calls = rc_refreshes = 0;
    closes = waits = forks = unlinks = 0;
    missing_directory = false;
    save_ok = wrapper_write_ok = wrapper_close_ok = true;
    wait_errors.clear();
    exit_status = 0;
    module_env = "test_module";
}

DIR* opendir(const char*) {
    directory.offset = 0;
    return missing_directory ? nullptr : &directory;
}
dirent* readdir(DIR* dir) {
    return dir->offset < entries.size() ? &entries[dir->offset++] : nullptr;
}
int closedir(DIR*) {
    return 0;
}
int dirfd(DIR*) {
    return 7;
}
int fstatat(int fd, const char* name, struct stat* output, int flags) {
    assert(fd == 7 && flags == AT_SYMLINK_NOFOLLOW);
    ++stat_calls;
    auto entry = entry_modes.find(name);
    if (entry == entry_modes.end())
        return -1;
    output->st_mode = entry->second;
    return 0;
}
bool S_ISDIR(unsigned mode) {
    return mode == DT_DIR;
}
bool file_exists(const std::string& path) {
    return files.count(path) != 0;
}
bool ensure_file_exists(const std::string& path) {
    if (failed_flags.count(path))
        return false;
    files.insert(path);
    return true;
}
bool ensure_dir_exists(const std::string&) {
    return true;
}
int regenerate_preinit() {
    return ++rc_refreshes, 0;
}
void warn_regenerate_preinit_failed(int) {}
template <typename... Args>
void log(const char*, Args...) {}
#define LOGE(...) log(__VA_ARGS__)
#define LOGW(...) log(__VA_ARGS__)
std::optional<std::string> read_file(const std::string& path) {
    const auto it = contents.find(path);
    return it == contents.end() ? std::nullopt : std::optional<std::string>{it->second};
}
int stat(const char* path, struct stat* output) {
    if (contents.count(path) == 0) {
        errno = ENOENT;
        return -1;
    }
    output->st_mode = config_modes.at(path);
    return 0;
}
bool write_file_atomic(const std::string& path, const std::string& content, mode_t mode) {
    if (!save_ok)
        return false;
    contents[path] = content;
    config_modes[path] = mode;
    return true;
}
template <typename Fn>
void for_each_line(std::string_view text, Fn fn) {
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        fn(line);
        if (end == std::string_view::npos)
            break;
        text.remove_prefix(end + 1);
    }
}
const char* getenv(const char* key) {
    assert(std::string_view(key) == "KSU_MODULE");
    return module_env.c_str();
}
namespace terminal {
int error(const char*, const char*) {
    return 1;
}
template <typename... Args>
int errorf(const char*, Args...) {
    return 1;
}
int file_error(const char*, const std::string&, int) {
    return 1;
}
}  // namespace terminal

char* realpath(const char* path, char* resolved) {
    std::strcpy(resolved, path);
    return resolved;
}
int mkostemp(char*, int flags) {
    assert(flags == O_CLOEXEC);
    return 8;
}
FILE* fdopen(int, const char*) {
    return reinterpret_cast<FILE*>(1);
}
int close(int) {
    return 0;
}
int fputs(const char*, FILE*) {
    return wrapper_write_ok ? 0 : EOF;
}
int fclose(FILE*) {
    ++closes;
    return wrapper_close_ok ? 0 : EOF;
}
int chmod(const char*, unsigned) {
    return 0;
}
int unlink(const char* path) {
    ++unlinks;
    contents.erase(path);
    return 0;
}
struct CommonScriptEnv {};
CommonScriptEnv build_common_script_env() {
    return {};
}
void apply_common_script_env(const CommonScriptEnv&, const char*) {}
std::string build_install_wrapper_script(bool) {
    return "#!/system/bin/sh\n";
}
pid_t fork() {
    ++forks;
    return 42;
}
int setenv(const char*, const char*, int) {
    return 0;
}
int execl(const char*, const char*, const char*, std::nullptr_t) {
    return 0;
}
void _exit(int) {
    std::abort();
}
pid_t waitpid(pid_t pid, int* status, int) {
    assert(pid == 42 && unlinks == 0);
    const auto index = static_cast<size_t>(waits++);
    if (index < wait_errors.size() && wait_errors[index] != 0) {
        errno = wait_errors[index];
        return -1;
    }
    *status = exit_status;
    return pid;
}
bool WIFEXITED(int status) {
    return status >= 0;
}
int WEXITSTATUS(int status) {
    return status;
}

#include "module_under_test.inc"

void test_directory_and_ids() {
    reset();
    assert(is_module_directory(&directory, entries[0]));
    assert(stat_calls == 0);
    assert(is_module_directory(&directory, entries[1]));
    assert(stat_calls == 1);
    assert(!is_module_directory(&directory, entries[2]));
    assert(stat_calls == 1);
    entry_modes["second"] = DT_LNK;
    assert(!is_module_directory(&directory, entries[1]));
    entry_modes.clear();
    assert(!is_module_directory(&directory, entries[1]));
    for (auto id : {"module", "My_Module-1.2", "a0"})
        assert(validate_module_id(id));
    for (auto id : {"", "a", "../other", "a/b", "1module", "a b", "a\n", "\xc3\xa9"})
        assert(!validate_module_id(id));
}

void test_batch_changes() {
    reset();
    assert(disable_all_modules() == 0);
    assert(files.count(std::string(MODULE_DIR) + "first/disable"));
    assert(files.count(std::string(MODULE_DIR) + "second/disable"));
    assert(!files.count(std::string(MODULE_DIR) + "linked/disable"));
    assert(rc_refreshes == 1);
    reset();
    failed_flags.insert(std::string(MODULE_DIR) + "first/remove");
    assert(uninstall_all_modules() == 0);
    assert(!files.count(std::string(MODULE_DIR) + "first/remove"));
    assert(files.count(std::string(MODULE_DIR) + "second/remove"));
    assert(rc_refreshes == 1);
    reset();
    failed_flags = {std::string(MODULE_DIR) + "first/disable",
                    std::string(MODULE_DIR) + "second/disable"};
    assert(disable_all_modules() == 0);
    assert(rc_refreshes == 0);
    reset();
    missing_directory = true;
    assert(disable_all_modules() == 0 && uninstall_all_modules() == 0);
    assert(rc_refreshes == 0);
    reset();
    assert(module_disable("first") == 0 && module_uninstall("second") == 0);
    assert(rc_refreshes == 2);
}

void test_installer_cleanup_and_wait() {
    reset();
    wrapper_write_ok = false;
    assert(!exec_install_script("/module.zip", false, "module"));
    assert(closes == 1 && unlinks == 1 && forks == 0);
    reset();
    wrapper_close_ok = false;
    assert(!exec_install_script("/module.zip", false, "module"));
    assert(closes == 1 && unlinks == 1 && forks == 0);
    reset();
    wait_errors = {EINTR, EINTR, 0};
    assert(exec_install_script("/module.zip", false, "module"));
    assert(waits == 3 && unlinks == 1 && closes == 1);
    reset();
    wait_errors = {ECHILD};
    assert(!exec_install_script("/module.zip", false, "module"));
    assert(waits == 1 && unlinks == 1);
    reset();
    exit_status = 1;
    assert(!exec_install_script("/module.zip", false, "module"));
}

void test_config_validation() {
    reset();
    const std::string persist = std::string(MODULE_CONFIG_DIR) + "test_module/persist.config";
    const std::string temporary = std::string(MODULE_CONFIG_DIR) + "test_module/tmp.config";
    assert(module_config_handle({"set", "manage.test", "true"}) == 0);
    assert(contents[persist] == "manage.test=true\n");
    assert(config_modes[persist] == 0644);
    config_modes[persist] = 0600;
    assert(module_config_handle({"set", "manage.test", "true"}) == 0);
    assert(config_modes[persist] == 0600);
    assert(module_config_handle({"set", "manage.test", "false", "--temp"}) == 0);
    assert(contents[temporary] == "manage.test=false\n");
    assert(module_config_handle({"get", "manage.test"}) == 0);
    assert(module_config_handle({"set", "url", "https://example.com?a=b"}) == 0);
    const auto before = contents;
    for (auto id : {"../escape", "a/b", "", "1bad"}) {
        module_env = id;
        assert(module_config_handle({"set", "key", "value"}) != 0);
    }
    module_env = "test_module";
    for (const auto& args :
         std::vector<std::vector<std::string>>{{"set", "", "value"},
                                               {"set", "key=value", "value"},
                                               {"set", "key\nother", "value"},
                                               {"set", "key", "value\nother=true"},
                                               {"set", "key", "value\r"}})
        assert(module_config_handle(args) != 0);
    assert(contents == before);
    assert(config_modes[persist] == 0600);
    save_ok = false;
    assert(module_config_handle({"set", "manage.test", "changed"}) != 0);
    assert(contents == before);
    save_ok = true;
    assert(module_config_handle({"delete", "manage.test", "--temp"}) == 0);
    assert(contents[temporary].empty());
    assert(contents[persist].find("manage.test=true\n") != std::string::npos);
}
}  // namespace fixture

int main() {
    fixture::test_directory_and_ids();
    fixture::test_batch_changes();
    fixture::test_installer_cleanup_and_wait();
    fixture::test_config_validation();
    std::cout << "module management regressions passed\n";
}
