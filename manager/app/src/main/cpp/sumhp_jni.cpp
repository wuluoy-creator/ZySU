#include <jni.h>

#include "sumhp/embedded_paths.hpp"
#include "sumhp/sumh_client.hpp"
#include "uapi/sumh.h"
#include "userspace/common/maps_preview_transport.hpp"
#include "userspace/common/profile_manager_transport.hpp"
#include "userspace/common/su_path.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/klog.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern "C" int ksu_grant_root(void);

namespace {
namespace sumh = sumhp::sumh;
constexpr const char *socket_path = "/data/adb/ksu/sumhp/sumhpd.sock";
constexpr const char *log_path = sumhp::embedded_log_file;
constexpr size_t response_limit = 1024UL * 1024;
std::mutex worker_mutex;

class Fd {
public:
  explicit Fd(int value) : value_(value) {}
  ~Fd() {
    if (value_ >= 0)
      close(value_);
  }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  Fd(Fd &&) = delete;
  Fd &operator=(Fd &&) = delete;
  [[nodiscard]] int get() const { return value_; }
  int release() {
    const int value = value_;
    value_ = -1;
    return value;
  }

private:
  int value_;
};

[[noreturn]] void failure(const char *operation, int error = errno) {
  throw std::runtime_error(std::string(operation) + ": " +
                           strerror(error ? error : EIO));
}

std::string quote(const std::string &input) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out = "\"";
  for (const unsigned char c : input) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else
      out += static_cast<char>(c);
  }
  return out + '"';
}

struct RootTask {
  std::function<std::string()> action;
  std::string output;
  std::string error;
};

void *root_entry(void *opaque) {
  const std::lock_guard<std::mutex> lock(worker_mutex);
  const std::unique_ptr<std::shared_ptr<RootTask>> holder(
      static_cast<std::shared_ptr<RootTask> *>(opaque));
  const auto &task = *holder;
  try {
    if (ksu_grant_root() != 0 || geteuid() != 0)
      failure("Manager root", EPERM);
    task->output = task->action();
  } catch (const std::exception &error) {
    task->error = error.what();
  } catch (...) {
    task->error = "Native SUMH operation failed";
  }
  return nullptr;
}

std::string root_call(std::function<std::string()> action) {
  // Serialize root-worker operations against the shared KSU driver fd.
  auto task = std::make_shared<RootTask>();
  task->action = std::move(action);
  auto *holder = new std::shared_ptr<RootTask>(task);
  pthread_t thread;
  int error = pthread_create(&thread, nullptr, root_entry, holder);
  if (error) {
    delete holder;
    failure("pthread_create", error);
  }
  error = pthread_join(thread, nullptr);
  if (error) {
    pthread_detach(thread);
    failure("pthread_join", error);
  }
  if (!task->error.empty())
    throw std::runtime_error(task->error);
  return task->output;
}

// MAPS_PREVIEW_WORKER_BEGIN: this fixed-purpose worker owns all state after a
// caller timeout. At most one detached worker can exist; root escalation may
// block in the kernel, so callers must not join it or queue additional workers.
constexpr auto preview_wait_budget = std::chrono::seconds(10);
struct PartitionsSaveInput {
  std::string revision;
  std::string partitions;
};
struct PreviewTask {
  std::string executable;
  sumhp::preview_transport::Service service =
      sumhp::preview_transport::Service::Maps;
  std::shared_ptr<const PartitionsSaveInput> save;
  std::shared_ptr<const std::string> hide_args;
  std::shared_ptr<const sumhp::module_preview::Input> module;
  std::chrono::steady_clock::time_point deadline;
  std::mutex mutex;
  std::condition_variable finished;
  bool done = false;
  std::string output;
};
std::mutex preview_slot_mutex;
std::shared_ptr<PreviewTask> preview_slot;

std::string preview_root_call(
    const std::string &executable,
    sumhp::preview_transport::Service service =
        sumhp::preview_transport::Service::Maps,
    std::shared_ptr<const PartitionsSaveInput> save = {},
    std::shared_ptr<const std::string> hide_args = {},
    std::shared_ptr<const sumhp::module_preview::Input> module = {}) {
  if (service != sumhp::preview_transport::Service::Maps &&
      service != sumhp::preview_transport::Service::PartitionDiscovery &&
      service != sumhp::preview_transport::Service::UserHide &&
      service != sumhp::preview_transport::Service::Operation &&
      service != sumhp::preview_transport::Service::ModulePreview &&
      service != sumhp::preview_transport::Service::Capabilities &&
      service != sumhp::preview_transport::Service::TemporaryMaps &&
      service != sumhp::preview_transport::Service::DiagnosticPackages)
    return sumhp::preview_transport::failure_envelope(EINVAL);
  if ((service == sumhp::preview_transport::Service::UserHide ||
       service == sumhp::preview_transport::Service::Operation ||
       service == sumhp::preview_transport::Service::TemporaryMaps ||
       service == sumhp::preview_transport::Service::DiagnosticPackages) !=
          static_cast<bool>(hide_args) ||
      (save &&
       (service != sumhp::preview_transport::Service::PartitionDiscovery ||
        hide_args)) ||
      (service == sumhp::preview_transport::Service::ModulePreview) !=
          static_cast<bool>(module) ||
      (module && (save || hide_args || !sumhp::module_preview::valid(*module))))
    return sumhp::preview_transport::failure_envelope(EINVAL);
  if (service == sumhp::preview_transport::Service::TemporaryMaps) {
    sumhp::temporary_maps::Request request;
    if (!sumhp::temporary_maps::parse(*hide_args, request))
      return sumhp::preview_transport::failure_envelope(EINVAL);
  }
  if (service == sumhp::preview_transport::Service::DiagnosticPackages) {
    sumhp::diagnostic_package_manager::Request request;
    if (!sumhp::diagnostic_package_manager::parse(*hide_args, request))
      return sumhp::preview_transport::failure_envelope(EINVAL);
  }
  auto task = std::make_shared<PreviewTask>();
  task->executable = executable;
  task->service = service;
  task->save = std::move(save);
  task->hide_args = std::move(hide_args);
  task->module = std::move(module);
  task->deadline = std::chrono::steady_clock::now() + preview_wait_budget;
  {
    const std::unique_lock<std::mutex> slot_lock(preview_slot_mutex,
                                                 std::try_to_lock);
    if (!slot_lock.owns_lock() || preview_slot)
      return sumhp::preview_transport::failure_envelope(EBUSY);
    preview_slot = task;
  }
  try {
    std::thread([task] {
      int error = 0;
      std::string output;
      try {
        // The ordinary worker may be busy with a mutation. Never queue a
        // fixed-service request behind an unbounded root-worker lock
        // acquisition.
        const std::unique_lock<std::mutex> root_lock(worker_mutex,
                                                     std::try_to_lock);
        if (!root_lock.owns_lock())
          error = EBUSY;
        else if (ksu_grant_root() != 0 || geteuid() != 0)
          error = EPERM;
        else {
          const auto remaining =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  task->deadline - std::chrono::steady_clock::now())
                  .count();
          if (remaining <= 0)
            error = ETIMEDOUT;
          else if (task->module)
            output = sumhp::preview_transport::request_module_preview(
                task->executable, *task->module,
                static_cast<unsigned>(std::min<std::int64_t>(remaining, 8000)));
          else if (task->hide_args &&
                   task->service ==
                       sumhp::preview_transport::Service::DiagnosticPackages)
            output = sumhp::preview_transport::request_diagnostic_packages(
                task->executable, *task->hide_args,
                static_cast<unsigned>(std::min<std::int64_t>(remaining, 8000)));
          else if (task->hide_args &&
                   task->service ==
                       sumhp::preview_transport::Service::TemporaryMaps)
            output = sumhp::preview_transport::request_temporary_maps(
                task->executable, *task->hide_args,
                static_cast<unsigned>(std::min<std::int64_t>(remaining, 8000)));
          else if (task->hide_args &&
                   task->service ==
                       sumhp::preview_transport::Service::Operation)
            output = sumhp::preview_transport::request_operation(
                task->executable, *task->hide_args,
                static_cast<unsigned>(std::min<std::int64_t>(remaining, 8000)));
          else if (task->hide_args)
            output = sumhp::preview_transport::request_user_hide(
                task->executable, *task->hide_args,
                static_cast<unsigned>(std::min<std::int64_t>(remaining, 8000)));
          else if (task->save)
            output = sumhp::preview_transport::request_partitions_save(
                task->executable, task->save->revision, task->save->partitions,
                static_cast<unsigned>(std::min<std::int64_t>(remaining, 8000)));
          else
            output = sumhp::preview_transport::request(
                task->executable,
                static_cast<unsigned>(std::min<std::int64_t>(remaining, 8000)),
                task->service);
        }
      } catch (...) {
        error = EIO;
      }
      if (error)
        output = sumhp::preview_transport::failure_envelope(error);
      {
        const std::lock_guard<std::mutex> result_lock(task->mutex);
        task->output = std::move(output);
        task->done = true;
      }
      {
        const std::lock_guard<std::mutex> slot_lock(preview_slot_mutex);
        preview_slot.reset();
      }
      task->finished.notify_all();
    }).detach();
  } catch (...) {
    const std::lock_guard<std::mutex> slot_lock(preview_slot_mutex);
    preview_slot.reset();
    return sumhp::preview_transport::failure_envelope(EAGAIN);
  }
  std::unique_lock<std::mutex> result_lock(task->mutex);
  if (!task->finished.wait_until(result_lock, task->deadline,
                                 [&] { return task->done; }))
    return sumhp::preview_transport::failure_envelope(ETIMEDOUT);
  return task->output;
}
// MAPS_PREVIEW_WORKER_END

// Profile documents and diffs have their own file budget. Only the small
// completion receipt crosses JNI; a partial output inode is never a result.
// Own both descriptors before spawning so a caller timeout/close cannot
// redirect late native writes into a newly reused Java descriptor.
// PROFILE_TRANSFER_WORKER_BEGIN
struct ProfileTransferTask {
  ProfileTransferTask(std::string path, int input, int output) noexcept
      : executable(std::move(path)), input_fd(input), output_fd(output) {}

private:
  friend std::string profile_root_transfer(const std::string &executable,
                                           int input, int output);
  std::string executable;
  Fd input_fd, output_fd;
  std::chrono::steady_clock::time_point deadline;
  std::mutex mutex;
  std::condition_variable finished;
  bool done = false;
  std::string receipt;
};
std::mutex profile_slot_mutex;
std::shared_ptr<ProfileTransferTask> profile_slot;

bool private_profile_file(int fd, uid_t owner, bool output,
                          struct stat &identity) {
  const int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fstat(fd, &identity) != 0)
    return false;
  const int access = flags & O_ACCMODE;
  return S_ISREG(identity.st_mode) && identity.st_nlink == 1 &&
         identity.st_uid == owner && (identity.st_mode & 0077) == 0 &&
         !(flags & O_APPEND) &&
         (output ? (access == O_WRONLY || access == O_RDWR) &&
                       identity.st_size == 0
                 : (access == O_RDONLY || access == O_RDWR) &&
                       identity.st_size > 0 &&
                       identity.st_size <= 8 * 1024 * 1024 + 4096);
}

std::string profile_root_transfer(const std::string &executable, int input,
                                  int output) {
  if (executable.empty() || executable.front() != '/' ||
      executable.find('\0') != std::string::npos)
    return sumhp::profile_transport::failure_receipt(EINVAL);
  const uid_t owner = getuid();
  Fd owned_input(fcntl(input, F_DUPFD_CLOEXEC, 3));
  if (owned_input.get() < 0)
    return sumhp::profile_transport::failure_receipt(errno);
  Fd owned_output(fcntl(output, F_DUPFD_CLOEXEC, 3));
  if (owned_output.get() < 0)
    return sumhp::profile_transport::failure_receipt(errno);
  struct stat input_identity{};
  struct stat output_identity{};
  if (!private_profile_file(owned_input.get(), owner, false, input_identity) ||
      !private_profile_file(owned_output.get(), owner, true, output_identity) ||
      (input_identity.st_dev == output_identity.st_dev &&
       input_identity.st_ino == output_identity.st_ino))
    return sumhp::profile_transport::failure_receipt(EINVAL);
  // Allocate before releasing either descriptor: allocation failure must not
  // leak one of the caller-independent file references.
  auto task = std::make_shared<ProfileTransferTask>(
      executable, owned_input.get(), owned_output.get());
  (void)owned_input.release();
  (void)owned_output.release();
  task->deadline = std::chrono::steady_clock::now() + preview_wait_budget;
  {
    const std::unique_lock<std::mutex> lock(profile_slot_mutex,
                                            std::try_to_lock);
    if (!lock.owns_lock() || profile_slot)
      return sumhp::profile_transport::failure_receipt(EBUSY);
    profile_slot = task;
  }
  try {
    std::thread([task] {
      std::string receipt;
      int error = 0;
      try {
        const std::unique_lock<std::mutex> root_lock(worker_mutex,
                                                     std::try_to_lock);
        if (!root_lock.owns_lock())
          error = EBUSY;
        else if (ksu_grant_root() != 0 || geteuid() != 0)
          error = EPERM;
        else {
          const auto remaining =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  task->deadline - std::chrono::steady_clock::now())
                  .count();
          if (remaining <= 0)
            error = ETIMEDOUT;
          else
            receipt = sumhp::profile_transport::request(
                task->executable, task->input_fd.get(), task->output_fd.get(),
                static_cast<unsigned>(std::min<std::int64_t>(remaining, 8000)));
        }
      } catch (...) {
        error = EIO;
      }
      if (error)
        receipt = sumhp::profile_transport::failure_receipt(error);
      {
        const std::lock_guard<std::mutex> lock(task->mutex);
        task->receipt = std::move(receipt);
        task->done = true;
      }
      {
        const std::lock_guard<std::mutex> lock(profile_slot_mutex);
        profile_slot.reset();
      }
      task->finished.notify_all();
    }).detach();
  } catch (...) {
    const std::lock_guard<std::mutex> lock(profile_slot_mutex);
    profile_slot.reset();
    return sumhp::profile_transport::failure_receipt(EAGAIN);
  }
  std::unique_lock<std::mutex> lock(task->mutex);
  if (!task->finished.wait_until(lock, task->deadline,
                                 [&] { return task->done; }))
    return sumhp::profile_transport::failure_receipt(ETIMEDOUT);
  return task->receipt;
}
// PROFILE_TRANSFER_WORKER_END

void start_controller(const std::string &path) {
  if (path.empty() || path.front() != '/' ||
      path.find('\0') != std::string::npos)
    failure("Invalid ksud path", EINVAL);
  posix_spawn_file_actions_t actions;
  int error = posix_spawn_file_actions_init(&actions);
  if (error)
    failure("spawn actions", error);
  for (int fd = 0; fd <= 2 && !error; ++fd)
    error =
        posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", O_RDWR, 0);
  char name[] = "ksud";
  char sumhp[] = "sumhp";
  char daemon[] = "daemon";
  char start[] = "start";
  char *const argv[] = {name, sumhp, daemon, start, nullptr};
  pid_t pid = -1;
  if (!error)
    error = posix_spawn(&pid, path.c_str(), &actions, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  if (error)
    failure("Start SUMHP", error);
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0)
    failure("Wait for SUMHP");
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    failure("SUMHP startup failed", EIO);
}

std::string save_su_path(const std::string &ksud, const std::string &path) {
  if (ksud.empty() || ksud.front() != '/' ||
      ksud.find('\0') != std::string::npos)
    failure("Invalid ksud path", EINVAL);
  const int validation = ksud::validate_su_path(path);
  if (validation)
    failure("Invalid su path", -validation);

  std::array<int, 2> output{};
  if (pipe2(output.data(), O_CLOEXEC) != 0)
    failure("su path pipe");
  const Fd reader(output[0]);
  Fd writer(output[1]);
  posix_spawn_file_actions_t actions;
  int error = posix_spawn_file_actions_init(&actions);
  if (error)
    failure("spawn actions", error);
  error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                           O_RDONLY, 0);
  if (!error)
    error =
        posix_spawn_file_actions_adddup2(&actions, writer.get(), STDOUT_FILENO);
  if (!error)
    error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                             "/dev/null", O_WRONLY, 0);
  std::array<std::string, 5> arguments{ksud, "su-path", "set", "--json", path};
  std::array<char *, 6> argv{arguments[0].data(), arguments[1].data(),
                             arguments[2].data(), arguments[3].data(),
                             arguments[4].data(), nullptr};
  pid_t pid = -1;
  if (!error)
    error = posix_spawn(&pid, ksud.c_str(), &actions, nullptr, argv.data(),
                        environ);
  posix_spawn_file_actions_destroy(&actions);
  if (error)
    failure("Start su path controller", error);
  close(writer.release());

  std::string text;
  std::array<char, 1024> buffer{};
  for (;;) {
    const auto count = read(reader.get(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR)
        continue;
      error = errno;
      break;
    }
    if (!count)
      break;
    if (text.size() < 16384)
      text.append(buffer.data(), static_cast<size_t>(count));
  }
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0)
    failure("Wait for su path controller");
  if (error)
    failure("Read su path result", error);
  if (!WIFEXITED(status) || WEXITSTATUS(status) > 1 || text.empty())
    failure("Invalid su path response", EIO);
  return text;
}

int connect_controller(const char *path) {
  Fd fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (fd.get() < 0)
    failure("socket");
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(address.sun_path))
    failure("socket path", ENAMETOOLONG);
  memcpy(address.sun_path, path, strlen(path) + 1);
  if (connect(fd.get(), reinterpret_cast<sockaddr *>(&address),
              sizeof(address)) != 0)
    return -errno;
  ucred peer{};
  socklen_t size = sizeof(peer);
  if (getsockopt(fd.get(), SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 ||
      peer.uid != 0)
    failure("Untrusted SUMHP peer", EPERM);
  return fd.release();
}

using Deadline = std::chrono::steady_clock::time_point;
void wait_io(int fd, short events, Deadline deadline) {
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    if (left <= 0)
      failure("SUMHP response deadline", ETIMEDOUT);
    pollfd descriptor{fd, events, 0};
    const int result = poll(&descriptor, 1, static_cast<int>(left));
    if (result < 0 && errno == EINTR)
      continue;
    if (result < 0)
      failure("poll SUMHP");
    if (!result)
      failure("SUMHP response deadline", ETIMEDOUT);
    if (descriptor.revents & POLLNVAL)
      failure("SUMHP socket", EBADF);
    return;
  }
}

std::string exchange(int fd, const std::string &request) {
  if (request.empty() || request.size() > 60UL * 1024 ||
      request.find_first_of("\r\n") != std::string::npos ||
      request.find('\0') != std::string::npos)
    failure("Invalid SUMHP request", EINVAL);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  const auto wire = request + '\n';
  size_t sent = 0;
  while (sent < wire.size()) {
    wait_io(fd, POLLOUT, deadline);
    const auto count =
        send(fd, wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
    if (count < 0 && (errno == EINTR || errno == EAGAIN))
      continue;
    if (count <= 0)
      failure("Send SUMHP request");
    sent += static_cast<size_t>(count);
  }
  shutdown(fd, SHUT_WR);
  std::string response;
  std::array<char, 4096> buffer{};
  while (response.size() < response_limit) {
    wait_io(fd, POLLIN, deadline);
    const auto count =
        recv(fd, buffer.data(),
             std::min(buffer.size(), response_limit - response.size()), 0);
    if (count < 0 && (errno == EINTR || errno == EAGAIN))
      continue;
    if (count < 0)
      failure("Read SUMHP response");
    if (count == 0)
      failure("Incomplete SUMHP response", EPROTO);
    response.append(buffer.data(), static_cast<size_t>(count));
    if (response.back() == '\n')
      return response;
  }
  failure("SUMHP response too large", EOVERFLOW);
}

std::string request_controller(const std::string &ksud,
                               const std::string &request) {
  int connected = connect_controller(socket_path);
  if (connected == -ENOENT || connected == -ECONNREFUSED) {
    start_controller(ksud);
    connected = connect_controller(socket_path);
  }
  if (connected < 0)
    failure("Connect SUMHP", -connected);
  const Fd fd(connected);
  // Never replay a mutation after any request bytes might have been delivered.
  return exchange(fd.get(), request);
}

std::string read_tail(const char *path, size_t limit, size_t lines) {
  const Fd fd(open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (fd.get() < 0) {
    if (errno == ENOENT)
      return {};
    failure("Open log");
  }
  struct stat st{};
  if (fstat(fd.get(), &st) || !S_ISREG(st.st_mode))
    failure("Log is not a regular file", EINVAL);
  const auto start = st.st_size > static_cast<off_t>(limit)
                         ? st.st_size - static_cast<off_t>(limit)
                         : 0;
  if (lseek(fd.get(), start, SEEK_SET) < 0)
    failure("Seek log");
  std::string text(limit, '\0');
  size_t used = 0;
  while (used < limit) {
    const auto count = read(fd.get(), text.data() + used, limit - used);
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0)
      failure("Read log");
    if (!count)
      break;
    used += static_cast<size_t>(count);
  }
  text.resize(used);
  if (start > 0) {
    const auto newline = text.find('\n');
    text.erase(0, newline == std::string::npos ? text.size() : newline + 1);
  }
  size_t seen = 0;
  for (size_t i = text.size(); i > 0; --i) {
    if (text[i - 1] == '\n' && i != text.size() && ++seen == lines)
      return text.substr(i);
  }
  return text;
}

std::string kernel_snapshot() {
  const auto version = sumh::version_info();
  const bool available = version.status == sumh::Status::Available;
  utsname uts{};
  if (uname(&uts))
    failure("uname");
  auto out = std::string("{\"kernel\":") + quote(uts.release) +
             ",\"sumh_available\":" + (available ? "true" : "false") +
             ",\"kernel_version\":" + std::to_string(version.kernel_protocol);
  if (!available)
    return out + ",\"features\":{\"bitmask\":0,\"names\":[]},\"hooks\":\"\","
                 "\"rules\":\"\"}";
  const auto caps = sumh::feature_capabilities();
  if (!caps.ok)
    failure("SUMH features", caps.last_errno);
  const int enabled = sumh::enabled_state();
  if (enabled < 0)
    failure("SUMH state");
  out += ",\"features\":{\"bitmask\":" + std::to_string(caps.bitmask) +
         ",\"names\":[";
  bool first = true;
  for (const auto &name : sumh::feature_names(caps.bitmask)) {
    if (!first)
      out += ',';
    first = false;
    out += quote(name);
  }
  out += "]},\"enabled\":";
  out += enabled ? "true" : "false";
  std::vector<sumh::UserHideRule> user_rules;
  const bool queried = sumh::user_hide_rules(user_rules);
  if (queried &&
      ((caps.bitmask & SUMH_FEATURE_MANAGED_HIDE) || !user_rules.empty())) {
    out += ",\"user_hide\":[";
    bool first_rule = true;
    for (const auto &rule : user_rules) {
      if (!first_rule)
        out += ',';
      first_rule = false;
      out += "{\"path\":" + quote(rule.path) +
             ",\"id\":" + std::to_string(rule.id) +
             ",\"management\":" + std::to_string(rule.management) +
             ",\"binding\":" + std::to_string(rule.binding) +
             ",\"error\":" + std::to_string(rule.error) + "}";
    }
    out += ']';
  } else if (caps.bitmask & SUMH_FEATURE_MANAGED_HIDE) {
    failure("SUMH user hide state");
  }
  errno = 0;
  const auto hooks = sumh::hooks();
  if (hooks.empty() && errno)
    failure("SUMH hooks");
  errno = 0;
  const auto rules = sumh::active_rules();
  if (rules.empty() && errno)
    failure("SUMH rules");
  out += ",\"hooks\":" + quote(hooks) + ",\"rules\":" + quote(rules) + "}";
  return out;
}

void clear_log(const char *path) {
  const std::string lock_path = std::string(path) + ".lock";
  const Fd lock(open(lock_path.c_str(),
                     O_CREAT | O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW,
                     0600));
  if (lock.get() < 0)
    failure("Open daemon log lock");
  struct stat lock_info{};
  if (fstat(lock.get(), &lock_info))
    failure("Inspect daemon log lock");
  if (!S_ISREG(lock_info.st_mode) || lock_info.st_nlink != 1 ||
      lock_info.st_size < 0)
    failure("Invalid daemon log lock", EINVAL);
  // Refuse a busy writer instead of holding the Manager root worker
  // indefinitely. File opens are also nonblocking so an unexpected FIFO cannot
  // wait for a peer.
  if (flock(lock.get(), LOCK_EX | LOCK_NB))
    failure("Lock daemon log");
  if (fchmod(lock.get(), 0600))
    failure("Protect daemon log lock");
  const Fd fd(open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
  if (fd.get() < 0) {
    if (errno == ENOENT)
      return;
    failure("Open daemon log");
  }
  struct stat info{};
  if (fstat(fd.get(), &info))
    failure("Inspect daemon log");
  // Match the writer's descriptor contract before changing permissions or size.
  // Clearing never unlinks/replaces the file or repairs a rejected log path.
  if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size < 0)
    failure("Invalid daemon log", EINVAL);
  if (fchmod(fd.get(), 0600))
    failure("Protect daemon log");
  if (ftruncate(fd.get(), 0))
    failure("Clear daemon log");
}

std::string bytes(JNIEnv *env, jbyteArray array,
                  std::size_t limit = response_limit) {
  if (!array)
    failure("Missing data", EINVAL);
  const jsize size = env->GetArrayLength(array);
  if (size > static_cast<jsize>(limit))
    failure("JNI input too large", EOVERFLOW);
  std::string result(static_cast<size_t>(size), '\0');
  env->GetByteArrayRegion(array, 0, size,
                          reinterpret_cast<jbyte *>(result.data()));
  if (env->ExceptionCheck())
    failure("Read JNI input", EINVAL);
  return result;
}

jbyteArray result(JNIEnv *env, std::function<std::string()> action) {
  try {
    const auto output = root_call(std::move(action));
    if (output.size() > response_limit)
      failure("JNI result too large", EOVERFLOW);
    auto *array = env->NewByteArray(static_cast<jsize>(output.size()));
    if (array)
      env->SetByteArrayRegion(array, 0, static_cast<jsize>(output.size()),
                              reinterpret_cast<const jbyte *>(output.data()));
    return array;
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    return nullptr;
  }
}
} // namespace

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_zying_zysu_Natives_sumhpRequest(JNIEnv *env, jobject /* thiz */,
                                         jbyteArray ksud, jbyteArray request) {
  try {
    const auto input =
        std::make_shared<const std::pair<std::string, std::string>>(
            bytes(env, ksud), bytes(env, request));
    return result(env, [input] {
      return request_controller(input->first, input->second);
    });
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    return nullptr;
  }
}

namespace {
jbyteArray
fixed_preview(JNIEnv *env, jbyteArray ksud,
              sumhp::preview_transport::Service service,
              std::shared_ptr<const PartitionsSaveInput> save = {},
              std::shared_ptr<const std::string> hide_args = {},
              std::shared_ptr<const sumhp::module_preview::Input> module = {}) {
  try {
    const auto executable = bytes(env, ksud);
    if (executable.empty() || executable.front() != '/' ||
        executable.size() > 4096 || executable.find('\0') != std::string::npos)
      failure("Invalid ksud preview path", EINVAL);
    // Deliberately bypass result()/root_call(), their join and daemon routing.
    const auto output =
        preview_root_call(executable, service, std::move(save),
                          std::move(hide_args), std::move(module));
    // 512 KiB raw JSON can expand when embedded in the transport JSON string.
    std::size_t limit = 4UL * 1024 * 1024;
    if (service == sumhp::preview_transport::Service::ModulePreview)
      limit = sumhp::module_preview::envelope_bytes;
    else if (service == sumhp::preview_transport::Service::TemporaryMaps)
      limit = sumhp::temporary_maps::envelope_bytes;
    else if (service == sumhp::preview_transport::Service::DiagnosticPackages)
      limit = sumhp::diagnostic_package_manager::envelope_bytes;
    if (output.size() > limit)
      failure("Preview envelope too large", EOVERFLOW);
    auto *array = env->NewByteArray(static_cast<jsize>(output.size()));
    if (array)
      env->SetByteArrayRegion(array, 0, static_cast<jsize>(output.size()),
                              reinterpret_cast<const jbyte *>(output.data()));
    return array;
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    return nullptr;
  }
}
} // namespace

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_zying_zysu_Natives_sumhProfilesTransfer(JNIEnv *env,
                                                 jobject /* thiz */,
                                                 jbyteArray ksud, jint input_fd,
                                                 jint output_fd) {
  try {
    const auto executable = bytes(env, ksud, 4096);
    const auto receipt = profile_root_transfer(executable, input_fd, output_fd);
    if (receipt.size() > 1024)
      failure("Profile transfer receipt capacity", EOVERFLOW);
    auto *array = env->NewByteArray(static_cast<jsize>(receipt.size()));
    if (array)
      env->SetByteArrayRegion(array, 0, static_cast<jsize>(receipt.size()),
                              reinterpret_cast<const jbyte *>(receipt.data()));
    return array;
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_zying_zysu_Natives_sumhMapsPreview(JNIEnv *env, jobject /* thiz */,
                                            jbyteArray ksud) {
  return fixed_preview(env, ksud, sumhp::preview_transport::Service::Maps);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_zying_zysu_Natives_sumhTemporaryMaps(JNIEnv *env, jobject /* thiz */,
                                              jbyteArray ksud,
                                              jbyteArray request) {
  try {
    auto input = std::make_shared<const std::string>(
        bytes(env, request, sumhp::temporary_maps::input_bytes));
    return fixed_preview(env, ksud,
                         sumhp::preview_transport::Service::TemporaryMaps, {},
                         std::move(input));
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_zying_zysu_Natives_sumhDiagnosticPackages(JNIEnv *env,
                                                   jobject /* thiz */,
                                                   jbyteArray ksud,
                                                   jbyteArray request) {
  try {
    auto input = std::make_shared<const std::string>(
        bytes(env, request, sumhp::diagnostic_package_manager::input_bytes));
    return fixed_preview(env, ksud,
                         sumhp::preview_transport::Service::DiagnosticPackages,
                         {}, std::move(input));
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_zying_zysu_Natives_sumhCapabilities(JNIEnv *env, jobject /* thiz */,
                                             jbyteArray ksud) {
  return fixed_preview(env, ksud,
                       sumhp::preview_transport::Service::Capabilities);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_zying_zysu_Natives_sumhPartitionDiscovery(JNIEnv *env,
                                                   jobject /* thiz */,
                                                   jbyteArray ksud) {
  return fixed_preview(env, ksud,
                       sumhp::preview_transport::Service::PartitionDiscovery);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_zying_zysu_Natives_sumhUserHide(JNIEnv *env, jobject /* thiz */,
                                         jbyteArray ksud,
                                         jbyteArray arguments) {
  try {
    auto input =
        std::make_shared<const std::string>(bytes(env, arguments, 48UL * 1024));
    return fixed_preview(env, ksud, sumhp::preview_transport::Service::UserHide,
                         {}, std::move(input));
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_zying_zysu_Natives_sumhOperation(JNIEnv *env, jobject /* thiz */,
                                          jbyteArray ksud, jbyteArray request) {
  try {
    auto input =
        std::make_shared<const std::string>(bytes(env, request, 48UL * 1024));
    return fixed_preview(env, ksud,
                         sumhp::preview_transport::Service::Operation, {},
                         std::move(input));
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_zying_zysu_Natives_sumhModulePreview(JNIEnv *env, jobject /* thiz */,
                                              jbyteArray ksud,
                                              jbyteArray candidate,
                                              jbyteArray explain_path) {
  try {
    sumhp::module_preview::Input input;
    if (candidate)
      input.candidate =
          bytes(env, candidate, sumhp::module_preview::input_bytes);
    input.explain_path = bytes(env, explain_path, 4095);
    return fixed_preview(
        env, ksud, sumhp::preview_transport::Service::ModulePreview, {}, {},
        std::make_shared<const sumhp::module_preview::Input>(std::move(input)));
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_zying_zysu_Natives_sumhPartitionSave(JNIEnv *env, jobject /* thiz */,
                                              jbyteArray ksud,
                                              jbyteArray revision,
                                              jbyteArray partitions) {
  try {
    auto save = std::make_shared<const PartitionsSaveInput>(
        PartitionsSaveInput{bytes(env, revision), bytes(env, partitions)});
    return fixed_preview(env, ksud,
                         sumhp::preview_transport::Service::PartitionDiscovery,
                         std::move(save));
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_zying_zysu_Natives_sumhKernelSnapshot(JNIEnv *env,
                                               jobject /* thiz */) {
  return result(env, kernel_snapshot);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_zying_zysu_Natives_sumhRetryUserHide(JNIEnv *env, jobject /* thiz */,
                                              jbyteArray path) {
  try {
    const auto input = std::make_shared<const std::string>(bytes(env, path));
    return result(env, [input] {
      if (!sumh::retry_user_hide(*input))
        failure("Retry user hide rule");
      return std::string("{}");
    });
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_com_zying_zysu_Natives_saveSuPath(
    JNIEnv *env, jobject /* thiz */, jbyteArray ksud, jbyteArray path) {
  try {
    const auto input =
        std::make_shared<const std::pair<std::string, std::string>>(
            bytes(env, ksud), bytes(env, path));
    return result(
        env, [input] { return save_su_path(input->first, input->second); });
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_zying_zysu_Natives_sumhIsInitialized(JNIEnv * /*env*/,
                                              jobject /*thiz*/) {
  return sumh::is_available() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL Java_com_zying_zysu_Natives_sumhRuntimeState(
    JNIEnv * /*env*/, jobject /*thiz*/) {
  return sumh::enabled_state();
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_com_zying_zysu_Natives_sumhReadLog(
    JNIEnv *env, jobject /* thiz */, jboolean kernel) {
  return result(env, [kernel] {
    if (!kernel)
      return read_tail(log_path, 256UL * 1024, 1000);
    std::string buffer(response_limit, '\0');
    const int count =
        klogctl(3, buffer.data(), static_cast<int>(buffer.size()));
    if (count < 0)
      failure("Read kernel log");
    buffer.resize(static_cast<size_t>(count));
    return buffer;
  });
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_zying_zysu_Natives_sumhStorageInfo(JNIEnv *env, jobject /* thiz */,
                                            jbyteArray path) {
  try {
    const auto target = std::make_shared<const std::string>(bytes(env, path));
    return result(env, [target] {
      if (target->empty() || target->front() != '/' ||
          target->find('\0') != std::string::npos)
        failure("Invalid storage path", EINVAL);
      struct statfs stats{};
      if (statfs(target->c_str(), &stats))
        failure("statfs");
      const auto size = stats.f_blocks * stats.f_bsize;
      const auto avail = stats.f_bavail * stats.f_bsize;
      std::string type = "unknown";
      if (stats.f_type == 0x01021994)
        type = "tmpfs";
      else if (stats.f_type == 0xef53)
        type = "ext4";
      else if (stats.f_type == 0xe0f5e1e2)
        type = "erofs";
      return "{\"size\":" + std::to_string(size) +
             ",\"avail\":" + std::to_string(avail) +
             ",\"type\":" + quote(type) + "}";
    });
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_com_zying_zysu_Natives_sumhClearLog(JNIEnv *env, jobject /* thiz */) {
  auto *unused = result(env, [] {
    clear_log(log_path);
    return std::string{};
  });
  if (unused)
    env->DeleteLocalRef(unused);
}

extern "C" JNIEXPORT void JNICALL
Java_com_zying_zysu_Natives_sumhClearMapsRules(JNIEnv *env,
                                               jobject /* thiz */) {
  auto *unused = result(env, [] {
    if (!sumh::clear_maps_rules())
      failure("Clear Maps rules");
    return std::string{};
  });
  if (unused)
    env->DeleteLocalRef(unused);
}

extern "C" JNIEXPORT void JNICALL Java_com_zying_zysu_Natives_sumhAddMapsRule(
    JNIEnv *env, jobject /* thiz */, jlongArray numbers, jbyteArray path) {
  try {
    if (!numbers || env->GetArrayLength(numbers) != 4)
      failure("Invalid Maps fields", EINVAL);
    std::array<jlong, 4> values{};
    env->GetLongArrayRegion(numbers, 0, 4, values.data());
    const auto target = std::make_shared<const std::string>(bytes(env, path));
    if (target->empty() || target->front() != '/' ||
        target->find('\0') != std::string::npos)
      failure("Invalid Maps path", EINVAL);
    auto *unused = result(env, [values, target] {
      if (!sumh::add_maps_rule(static_cast<unsigned long>(values[0]),
                               static_cast<unsigned long>(values[1]),
                               static_cast<unsigned long>(values[2]),
                               static_cast<unsigned long>(values[3]), *target))
        failure("Add Maps rule");
      return std::string{};
    });
    if (unused)
      env->DeleteLocalRef(unused);
  } catch (const std::exception &error) {
    if (!env->ExceptionCheck())
      env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
  }
}
