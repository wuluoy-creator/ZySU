// The runner extracts the production ZIP reader and links real miniz. Only the
// POSIX file adapter and subprocess boundary are replaced for portable tests.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "miniz.h"

namespace fixture {

constexpr int O_RDONLY = 0, O_CLOEXEC = 0;
constexpr auto BUSYBOX_PATH = "/data/adb/ksu/bin/busybox";
using ssize_t = std::ptrdiff_t;
using off_t = std::int64_t;
struct stat {
    unsigned st_mode = 1;
    off_t st_size = 0;
};
struct ExecResult {
    int exit_code;
    std::string stdout_str;
    std::string stderr_str;
    int error_number = 0;
};

std::string opened_bytes;
bool opened = false;
int closes = 0;
std::vector<std::vector<std::string>> commands;
std::string logs;
ExecResult child_result{0, "", ""};

std::string load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int open(const char* path, int) {
    if (!std::filesystem::is_regular_file(path)) {
        errno = ENOENT;
        return -1;
    }
    opened_bytes = load(path);
    opened = true;
    return 17;
}

int fstat(int fd, struct stat* output) {
    if (!opened || fd != 17) {
        errno = EBADF;
        return -1;
    }
    output->st_size = static_cast<off_t>(opened_bytes.size());
    return 0;
}

bool S_ISREG(unsigned mode) {
    return mode == 1;
}

ssize_t pread(int fd, void* buffer, size_t count, off_t offset) {
    if (!opened || fd != 17 || offset < 0) {
        errno = EBADF;
        return -1;
    }
    const auto start = static_cast<size_t>(offset);
    const size_t copied =
        start < opened_bytes.size() ? std::min(count, opened_bytes.size() - start) : 0;
    if (copied != 0)
        std::memcpy(buffer, opened_bytes.data() + start, copied);
    return static_cast<ssize_t>(copied);
}

int close(int fd) {
    if (fd != 17 || !opened)
        std::abort();
    opened = false;
    ++closes;
    return 0;
}

void log(const char* format, ...) {
    char buffer[4096];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    logs.append(buffer).push_back('\n');
}
#define LOGE(...) log(__VA_ARGS__)
#define LOGW(...) log(__VA_ARGS__)
#define LOGI(...) log(__VA_ARGS__)

ExecResult exec_command(const std::vector<std::string>& args, std::chrono::milliseconds timeout) {
    if (timeout != std::chrono::seconds(30))
        std::abort();
    commands.push_back(args);
    return child_result;
}

#include "zip_entry_under_test.inc"

void reset(const std::string& expected) {
    if (opened)
        std::abort();
    closes = 0;
    commands.clear();
    logs.clear();
    child_result = {0, expected, ""};
}

}  // namespace fixture

void require(bool condition, const std::string& name) {
    if (!condition) {
        std::cerr << "FAIL " << name << '\n' << fixture::logs;
        std::exit(1);
    }
}

int main(int argc, char** argv) {
    require(argc == 2 || argc == 4, "fixture directory [archive expected-content]");
    const std::filesystem::path directory(argv[1]);
    const auto expected = fixture::load(directory / "expected.prop");
    size_t passed = 0;
    const auto run = [&](const std::string& name, bool success, size_t subprocesses) {
        const auto path = (directory / (name + ".zip")).string();
        const auto result = fixture::read_zip_entry(path, "module.prop");
        require(result.has_value() == success, name + ": success status");
        if (success)
            require(*result == expected, name + ": exact output");
        require(fixture::commands.size() == subprocesses, name + ": fallback count");
        if (subprocesses != 0) {
            require(fixture::commands[0] == std::vector<std::string>{fixture::BUSYBOX_PATH, "unzip",
                                                                     "-p", path, "module.prop"},
                    name + ": direct argv invocation");
        }
        require(!fixture::opened && fixture::closes == 1, name + ": descriptor closed");
        ++passed;
    };

    for (const auto* name : {"stored", "deflate"}) {
        fixture::reset(expected);
        run(name, true, 0);
    }
    fixture::reset(expected);
    fixture::child_result.stderr_str = "id=do_not_parse_stderr\n";
    run("xz", true, 1);
    fixture::reset(expected);
    run("xz spaces ; $()", true, 1);
    fixture::reset(expected);
    run("bzip2", true, 1);

    fixture::reset(expected);
    const auto original_directory = std::filesystem::current_path();
    std::filesystem::current_path(directory);
    const auto relative = fixture::read_zip_entry("-module.zip", "module.prop");
    std::filesystem::current_path(original_directory);
    require(relative && *relative == expected, "leading-dash ZIP path");
    require(fixture::commands ==
                std::vector<std::vector<std::string>>{
                    {fixture::BUSYBOX_PATH, "unzip", "-p", "./-module.zip", "module.prop"}},
            "leading-dash ZIP is not parsed as an option");
    require(!fixture::opened && fixture::closes == 1, "leading-dash ZIP closes fd");
    ++passed;

    for (const auto* name :
         {"missing", "nested", "invalid", "crc_bad", "encrypted", "encrypted_xz", "truncated"}) {
        fixture::reset(expected);
        run(name, false, 0);
        require(!fixture::logs.empty(), std::string(name) + ": diagnostic logged");
    }
    fixture::reset(expected);
    fixture::child_result.exit_code = 1;
    fixture::child_result.stderr_str = "unzip: unsupported method 99\n";
    run("unknown", false, 1);

    fixture::reset(expected);
    fixture::child_result.exit_code = 2;
    fixture::child_result.stderr_str = "unzip: inflate error\n";
    run("xz", false, 1);

    fixture::reset(expected);
    fixture::child_result.error_number = EIO;
    run("xz", false, 1);

    fixture::reset(expected);
    fixture::child_result.stdout_str.pop_back();
    run("xz", false, 1);

    fixture::reset(expected);
    fixture::child_result.stdout_str[0] ^= 1;
    run("xz", false, 1);

    fixture::reset(expected);
    fixture::child_result.stdout_str.clear();
    run("xz", false, 1);

    fixture::reset(expected);
    fixture::child_result.stdout_str += expected;
    run("xz", false, 1);

    if (argc == 4) {
        const auto archive_expected = fixture::load(argv[3]);
        fixture::reset(archive_expected);
        const auto actual = fixture::read_zip_entry(argv[2], "module.prop");
        require(actual && *actual == archive_expected, "supplied archive contents");
        require(fixture::commands.size() == 1, "supplied archive uses fallback");
        require(!fixture::opened && fixture::closes == 1, "supplied archive closes fd");
        ++passed;
    }
    std::cout << "ZIP entry regression tests passed: " << passed << " cases\n";
}
