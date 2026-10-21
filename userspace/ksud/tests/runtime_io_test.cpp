#include "log.hpp"
#include "pipe.hpp"
#include "utils.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace ksud {
void log_e(const char*, ...) {}

// The process runner calls the real fork/pipe/poll/wait code while this entry
// point controls output and timing without linking all embedded boot tools.
int run_magiskboot_main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "wait") == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        std::this_thread::sleep_for(120ms);
        return 23;
    }
    if (argc == 2 && std::strcmp(argv[1], "large") == 0) {
        const std::string chunk(16384, 'x');
        for (int i = 0; i < 80; ++i) {
            if (fwrite(chunk.data(), 1, chunk.size(), stdout) != chunk.size())
                return 1;
        }
        return 0;
    }
    fputs("captured output", stdout);
    fputs("captured error", stderr);
    return 7;
}

#include "runtime_io_under_test.inc"
}  // namespace ksud

namespace {
std::string contents(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void test_copy(const fs::path& directory) {
    const auto source = directory / "source";
    const auto target = directory / "target";
    const auto hardlink = directory / "hardlink";
    const auto symlink = directory / "symlink";
    const std::string input(131073, 's');
    std::ofstream(source, std::ios::binary) << input;
    fs::create_hard_link(source, hardlink);
    fs::create_symlink(source, symlink);
    for (const auto& alias : {source, hardlink, symlink}) {
        errno = 0;
        assert(!ksud::copy_file_data(source, alias));
        assert(errno == EINVAL);
        assert(contents(source) == input);
    }

    std::ofstream(target, std::ios::binary) << std::string(input.size() + 128, 't');
    assert(ksud::copy_file_data(source, target));
    assert(contents(target) == input);
    std::ofstream(source, std::ios::binary) << "replacement";
    assert(!ksud::copy_file_data(source, target, 0644, false));
    assert(contents(target) == input);
    assert(ksud::copy_file_data(source, target));
    assert(contents(target) == "replacement");
}

void test_binary(const fs::path& directory) {
    const auto path = directory / "nested" / "binary";
    const std::string data(262145, 'b');
    assert(ksud::ensure_binary(path, reinterpret_cast<const uint8_t*>(data.data()), data.size()));
    assert(contents(path) == data);
    assert(ksud::ensure_binary(path, reinterpret_cast<const uint8_t*>("x"), 1, true));
    assert(contents(path) == data);
    assert(!ksud::ensure_binary(path, nullptr, 1));
    assert(contents(path) == data);
}

void test_atomic_write(const fs::path& directory) {
    const auto path = directory / "atomic-state";
    assert(ksud::write_file_atomic(path, "original"));
    struct stat status{};
    assert(stat(path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600);

    // Explicit modes survive restrictive caller umasks and repeated replacement.
    const mode_t previous_umask = ::umask(0077);
    const bool replaced = ksud::write_file_atomic(path, "replacement", 0644);
    ::umask(previous_umask);
    assert(replaced && contents(path) == "replacement");
    assert(stat(path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0644);
    assert(ksud::write_file_atomic(path, "private", 0640));
    assert(stat(path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0640);

    // An already-open reader retains the complete previous version.
    const int reader = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    assert(reader >= 0);
    assert(ksud::write_file_atomic(path, "next", status.st_mode & 0777));
    char old_content[16]{};
    const ssize_t count = read(reader, old_content, sizeof(old_content));
    close(reader);
    assert(count == 7 && std::string(old_content, static_cast<size_t>(count)) == "private");
    assert(contents(path) == "next");
    assert(stat(path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0640);

    // Force a short write followed by EFBIG after the staging file was created.
    struct rlimit old_limit{};
    assert(getrlimit(RLIMIT_FSIZE, &old_limit) == 0);
    struct sigaction action{};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    struct sigaction old_action{};
    assert(sigaction(SIGXFSZ, &action, &old_action) == 0);
    auto limited = old_limit;
    limited.rlim_cur = 4;
    assert(setrlimit(RLIMIT_FSIZE, &limited) == 0);
    const bool failed_write = ksud::write_file_atomic(path, "too large", 0600);
    const int write_error = errno;
    assert(setrlimit(RLIMIT_FSIZE, &old_limit) == 0);
    assert(sigaction(SIGXFSZ, &old_action, nullptr) == 0);
    assert(!failed_write && write_error == EFBIG);
    assert(contents(path) == "next");
    assert(stat(path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0640);

    // Rename failure also removes the staged file and leaves existing data.
    const auto blocked = directory / "atomic-directory";
    fs::create_directory(blocked);
    std::ofstream(blocked / "keep") << "retained";
    assert(!ksud::write_file_atomic(blocked, "cannot replace a directory"));
    assert(contents(blocked / "keep") == "retained");
    for (const auto& entry : fs::directory_iterator(directory)) {
        const auto filename = entry.path().filename().string();
        assert(filename.find("atomic-state.tmp.") != 0);
        assert(filename.find("atomic-directory.tmp.") != 0);
    }
    assert(ksud::write_file_atomic(path, ""));
    assert(contents(path).empty());
    assert(stat(path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600);

    const auto durable = ksud::replace_file_durable(path, "durable", 0640);
    assert(durable.complete() && durable.replaced() && durable.error_number == 0);
    assert(contents(path) == "durable");
    assert(stat(path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0640);
    const auto missing = ksud::replace_file_durable(directory / "absent" / "state", "value");
    assert(!missing.replaced() && missing.error_number == ENOENT);
    std::string error;
    assert(!ksud::write_file_durable(blocked, "value", error));
    assert(error.find("file not replaced") != std::string::npos);
    assert(contents(blocked / "keep") == "retained");
}

void test_commands(const fs::path& directory) {
    const auto normal = ksud::exec_command({"sh", "-c", "printf out; printf err >&2; exit 9"});
    assert(normal.exit_code == 9 && normal.error_number == 0);
    assert(normal.stdout_str == "out" && normal.stderr_str == "err");
    const auto missing = ksud::exec_command({"/definitely-not-a-ksud-test-command"});
    assert(missing.exit_code == 127 && missing.error_number == ENOENT);
    assert(ksud::exec_command({"sh"}, directory / "missing").error_number == ENOENT);
    assert(ksud::exec_command({std::string("sh\0extra", 8)}).error_number == EINVAL);

    const auto capture = ksud::exec_command(
        {"sh", "-c",
         "i=0; while [ $i -lt 4096 ]; do printf 12345678901234567890123456789012; "
         "printf abcdefghijklmnopqrstuvwxyz123456 >&2; i=$((i+1)); done"},
        10s);
    assert(capture.exit_code == 0 && capture.error_number == 0);
    assert(capture.stdout_str.size() == 4096 * 32 && capture.stderr_str.size() == 4096 * 32);

    const auto marker = directory / "timed-helper-completed";
    const auto start = std::chrono::steady_clock::now();
    const auto timeout = ksud::exec_command(
        {"sh", "-c", "(sleep 0.4; printf bad > \"$1\") & wait", "sh", marker}, 100ms);
    assert(timeout.error_number == ETIMEDOUT);
    assert(std::chrono::steady_clock::now() - start < 2s);
    std::this_thread::sleep_for(450ms);
    assert(!fs::exists(marker));
}

void test_closed_stdio() {
    for (unsigned mask = 0; mask < 8; ++mask) {
        std::array<int, 3> saved{};
        for (int i = 0; i < 3; ++i) {
            saved[i] = dup(i);
            assert(saved[i] > STDERR_FILENO);
        }
        for (int i = 0; i < 3; ++i) {
            if ((mask & (1U << i)) != 0)
                close(i);
        }
        auto external = ksud::exec_command({"sh", "-c", "printf out; printf err >&2"});
        auto builtin = ksud::exec_command_magiskboot("", {});
        for (int i = 0; i < 3; ++i) {
            assert(dup2(saved[i], i) == i);
            close(saved[i]);
        }
        clearerr(stdout);
        clearerr(stderr);
        assert(external.exit_code == 0 && external.error_number == 0);
        assert(external.stdout_str == "out" && external.stderr_str == "err");
        assert(builtin.exit_code == 7 && builtin.error_number == 0);
        assert(builtin.stdout_str == "captured output" && builtin.stderr_str == "captured error");
    }
}

volatile sig_atomic_t signals_received = 0;
void on_alarm(int) {
    ++signals_received;
}

void test_interrupted_wait() {
    struct sigaction action{};
    action.sa_handler = on_alarm;
    sigemptyset(&action.sa_mask);
    struct sigaction previous{};
    assert(sigaction(SIGALRM, &action, &previous) == 0);
    struct itimerval timer{};
    timer.it_value.tv_usec = 5000;
    timer.it_interval.tv_usec = 5000;
    assert(setitimer(ITIMER_REAL, &timer, nullptr) == 0);
    const auto result = ksud::exec_command_magiskboot("", {"wait"});
    timer = {};
    assert(setitimer(ITIMER_REAL, &timer, nullptr) == 0);
    assert(sigaction(SIGALRM, &previous, nullptr) == 0);
    assert(signals_received > 0);
    assert(result.exit_code == 23 && result.error_number == 0);
}

void test_pipe_failure_cleanup() {
    struct rlimit old_limit{};
    assert(getrlimit(RLIMIT_NOFILE, &old_limit) == 0);
    struct rlimit limit = old_limit;
    // Existing stdio plus one pipe fits, but a second pipe cannot be created.
    limit.rlim_cur = 6;
    assert(setrlimit(RLIMIT_NOFILE, &limit) == 0);
    const auto result = ksud::exec_command_magiskboot("", {});
    const int next = open("/dev/null", O_RDONLY);
    const bool no_leak = next == 3;
    if (next >= 0)
        close(next);
    assert(setrlimit(RLIMIT_NOFILE, &old_limit) == 0);
    assert(result.exit_code == -1 && result.error_number == EMFILE && no_leak);
}
}  // namespace

int main() {
    char temporary[] = "/tmp/ksud-runtime-test-XXXXXX";
    assert(mkdtemp(temporary) != nullptr);
    const fs::path directory(temporary);
    test_copy(directory);
    test_binary(directory);
    test_atomic_write(directory);
    test_commands(directory);
    test_closed_stdio();
    test_interrupted_wait();
    test_pipe_failure_cleanup();
    const auto large = ksud::exec_command_magiskboot("", {"large"});
    assert(large.exit_code == 0 && large.stdout_str.size() == 1024 * 1024);
    assert(ksud::exec_command_magiskboot("", {std::string("x\0y", 3)}).error_number == EINVAL);
    fs::remove_all(directory);
    puts("ksud runtime I/O regressions passed");
}
