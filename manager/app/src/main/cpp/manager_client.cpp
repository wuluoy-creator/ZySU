#include "manager_client.hpp"

#include <csignal>
#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace zysu::manager {
namespace {
class Socket {
public:
  explicit Socket(int fd) : fd_(fd) {}
  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;
  Socket(Socket &&) = delete;
  Socket &operator=(Socket &&) = delete;
  ~Socket() {
    if (fd_ >= 0)
      close(fd_);
  }
  [[nodiscard]] int get() const { return fd_; }
  void reset() {
    close(fd_);
    fd_ = -1;
  }

private:
  int fd_;
};
class Child {
public:
  explicit Child(pid_t pid) : pid_(pid) {}
  Child(const Child &) = delete;
  Child &operator=(const Child &) = delete;
  Child(Child &&) = delete;
  Child &operator=(Child &&) = delete;
  ~Child() {
    if (pid_ > 0) {
      // Best effort while the child still has our UID. After elevation,
      // only its own watchdog can kill it; never block the app waiting.
      kill(-pid_, SIGKILL);
      pid_t waited;
      do {
        waited = waitpid(pid_, nullptr, WNOHANG);
      } while (waited < 0 && errno == EINTR);
      if (waited == 0) {
        try {
          std::thread([pid = pid_] {
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
            }
          }).detach();
        } catch (...) { // NOLINT(bugprone-empty-catch): destructors cannot
                        // propagate resource exhaustion.
          // Resource exhaustion must not turn an RPC timeout into an
          // unbounded UI wait. Process exit remains the final reaper.
        }
      }
    }
  }
  void finish(Deadline deadline) {
    for (;;) {
      int status = 0;
      const auto waited = waitpid(pid_, &status, WNOHANG);
      if (waited == pid_) {
        pid_ = -1;
        if (!WIFEXITED(status) || WEXITSTATUS(status))
          fail("Helper failed", EIO);
        return;
      }
      if (waited < 0 && errno != EINTR) {
        const int error = errno;
        if (error == ECHILD)
          pid_ = -1;
        fail("Wait helper", error);
      }
      if (std::chrono::steady_clock::now() >= deadline)
        fail("Helper exit deadline exceeded", ETIMEDOUT);
      poll(nullptr, 0, 10);
    }
  }

private:
  pid_t pid_;
};
std::string bundled_helper() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void *>(&call), &info) || !info.dli_fname)
    fail("Locate bundled helper", ENOENT);
  const std::string library(info.dli_fname);
  const auto slash = library.rfind('/');
  if (slash == std::string::npos || library.front() != '/')
    fail("Invalid bundled helper path", EINVAL);
  return library.substr(0, slash + 1) + ZYSU_KSUD_LIBRARY_NAME;
}
} // namespace

std::string call(Operation operation, const std::vector<std::string> &fields) {
  const auto payload = encode_fields(fields);
  auto path = bundled_helper();
  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets))
    fail("Create helper socket", errno);
  const Socket parent(sockets[0]);
  Socket child(sockets[1]);
  // Ensure the descriptors cannot collide with stdin/stdout dup2 actions.
  const int endpoint = fcntl(child.get(), F_DUPFD_CLOEXEC, 3);
  if (endpoint < 0)
    fail("Duplicate helper socket", errno);
  Socket channel(endpoint);
  child.reset();
  posix_spawn_file_actions_t actions;
  int error = posix_spawn_file_actions_init(&actions);
  if (error)
    fail("Initialize helper actions", error);
  error =
      posix_spawn_file_actions_adddup2(&actions, channel.get(), STDIN_FILENO);
  if (!error)
    error = posix_spawn_file_actions_adddup2(&actions, channel.get(),
                                             STDOUT_FILENO);
  if (!error)
    error = posix_spawn_file_actions_addclose(&actions, channel.get());
  if (!error)
    error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                             "/dev/null", O_WRONLY, 0);
  posix_spawnattr_t attributes;
  const int attr_error = posix_spawnattr_init(&attributes);
  if (!error)
    error = attr_error;
  if (!error)
    error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  if (!error)
    error = posix_spawnattr_setpgroup(&attributes, 0);
  char command[] = "manager-helper";
  // POSIX spawn requires mutable-character pointers for argv and envp.
  // NOLINTNEXTLINE(misc-const-correctness)
  char *const argv[] = {path.data(), command, nullptr};
  // No caller-controlled loader variables or shell environment reach root.
  char env_path[] = "PATH=/system/bin:/system/xbin";
  char env_lang[] = "LANG=C";
  // NOLINTNEXTLINE(misc-const-correctness)
  char *const environment[] = {env_path, env_lang, nullptr};
  pid_t pid = -1;
  if (!error)
    error = posix_spawn(&pid, path.c_str(), &actions, &attributes, argv,
                        environment);
  if (!attr_error)
    posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  if (error)
    fail("Start bundled helper", error);
  Child process(pid);
  channel.reset();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  // Once sent, requests are never retried: an operation may already have
  // committed.
  send_frame(parent.get(), static_cast<uint32_t>(operation), payload, deadline);
  shutdown(parent.get(), SHUT_WR);
  const auto response = receive_frame(parent.get(), deadline);
  process.finish(deadline);
  if (response.code)
    throw std::runtime_error(response.payload);
  return response.payload;
}
} // namespace zysu::manager
