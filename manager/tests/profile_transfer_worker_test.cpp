#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
using uid_t = unsigned int;
constexpr int F_DUPFD_CLOEXEC = 700;
constexpr int F_GETFL = 701;
#else
#include <sys/types.h>
#endif
#ifndef O_ACCMODE
#define O_ACCMODE (O_RDONLY | O_WRONLY | O_RDWR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & 0170000) == 0100000)
#endif
struct ProfileStat {
  unsigned st_mode = 0100600;
  unsigned st_nlink = 1;
  uid_t st_uid = 123;
  unsigned st_dev = 1, st_ino = 1;
  std::int64_t st_size = 1;
};
namespace model {
struct Inode {
  ProfileStat identity;
  int writes = 0;
};
struct Entry {
  std::shared_ptr<Inode> file;
  int flags = O_RDWR;
};
std::mutex mutex;
std::map<int, Entry> files;
int sequence = 100;
int close(int fd) {
  std::lock_guard<std::mutex> lock(mutex);
  return files.erase(fd) ? 0 : -1;
}
int fcntl(int fd, int op, int = 0) {
  std::lock_guard<std::mutex> lock(mutex);
  const auto found = files.find(fd);
  if (found == files.end()) {
    errno = EBADF;
    return -1;
  }
  if (op == F_GETFL)
    return found->second.flags;
  if (op == F_DUPFD_CLOEXEC) {
    files[++sequence] = found->second;
    return sequence;
  }
  errno = EINVAL;
  return -1;
}
int fstat(int fd, ProfileStat *result) {
  std::lock_guard<std::mutex> lock(mutex);
  const auto found = files.find(fd);
  if (found == files.end())
    return -1;
  *result = found->second.file->identity;
  return 0;
}
uid_t getuid() { return 123; }
uid_t geteuid() { return 0; }
std::shared_ptr<Inode> install(int fd, unsigned inode, std::int64_t size,
                               int flags = O_RDWR) {
  auto file = std::make_shared<Inode>();
  file->identity.st_ino = inode;
  file->identity.st_size = size;
  std::lock_guard<std::mutex> lock(mutex);
  files[fd] = {file, flags};
  return file;
}
std::atomic<int> grants{0}, requests{0}, request_delay{0}, root_delay{0};
std::atomic<int> grant_result{0};
} // namespace model
class Fd {
public:
  explicit Fd(int value) : value_(value) {}
  ~Fd() {
    if (value_ >= 0)
      model::close(value_);
  }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  int get() const { return value_; }
  int release() {
    int saved = value_;
    value_ = -1;
    return saved;
  }

private:
  int value_;
};
int ksu_grant_root() {
  ++model::grants;
  std::this_thread::sleep_for(
      std::chrono::milliseconds(model::root_delay.load()));
  return model::grant_result.load();
}
std::mutex worker_mutex;
constexpr auto preview_wait_budget = std::chrono::milliseconds(45);
namespace sumhp::profile_transport {
std::string failure_receipt(int code) {
  return "failure:" + std::to_string(code);
}
std::string request(const std::string &, int input, int output,
                    unsigned timeout) {
  if (timeout > 45)
    throw std::runtime_error("deadline not propagated");
  ++model::requests;
  std::this_thread::sleep_for(
      std::chrono::milliseconds(model::request_delay.load()));
  std::lock_guard<std::mutex> lock(model::mutex);
  if (!model::files.count(input) || !model::files.count(output))
    throw std::runtime_error("lost descriptor");
  ++model::files.at(output).file->writes;
  return "complete";
}
} // namespace sumhp::profile_transport
#include "profile_transfer_worker_under_test.inc"
int checks = 0;
void check(bool ok, const char *message) {
  ++checks;
  if (!ok)
    throw std::runtime_error(message);
}
bool idle() {
  std::lock_guard<std::mutex> lock(profile_slot_mutex);
  return !profile_slot;
}
void drain() {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!idle() && std::chrono::steady_clock::now() < end)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  check(idle(), "worker must release slot");
}
void originals() {
  model::install(10, 10, 42, O_RDONLY);
  model::install(11, 11, 0);
}
int main() {
  try {
    originals();
    check(profile_root_transfer("/private/ksud", 10, 11) == "complete",
          "normal transfer");
    drain();
    {
      std::lock_guard<std::mutex> lock(model::mutex);
      check(model::files.size() == 2, "owned duplicates released");
    }
    check(profile_root_transfer("relative", 10, 11) == "failure:22",
          "absolute executable required");
    check(profile_root_transfer(std::string("/x\0y", 4), 10, 11) ==
              "failure:22",
          "NUL executable rejected");
    check(profile_root_transfer("/ksud", -1, 11) == "failure:9",
          "invalid input fd");
    check(profile_root_transfer("/ksud", 10, -1) == "failure:9",
          "invalid output fd");
    auto output = model::install(11, 11, 1);
    check(profile_root_transfer("/ksud", 10, 11) == "failure:22",
          "nonempty output rejected");
    output = model::install(11, 11, 0, O_RDONLY);
    check(profile_root_transfer("/ksud", 10, 11) == "failure:22",
          "readonly output rejected");
    output = model::install(11, 11, 0);
    output->identity.st_uid = 999;
    check(profile_root_transfer("/ksud", 10, 11) == "failure:22",
          "other UID rejected");
    output->identity.st_uid = 123;
    output->identity.st_mode = 0100644;
    check(profile_root_transfer("/ksud", 10, 11) == "failure:22",
          "public inode rejected");
    output->identity.st_mode = 0100600;
    output->identity.st_nlink = 2;
    check(profile_root_transfer("/ksud", 10, 11) == "failure:22",
          "hardlinked inode rejected");
    output->identity.st_nlink = 1;
    output->identity.st_mode = 0010600;
    check(profile_root_transfer("/ksud", 10, 11) == "failure:22",
          "nonregular output rejected");
    originals();
    auto input = model::install(10, 10, 8 * 1024 * 1024 + 4097, O_RDONLY);
    check(profile_root_transfer("/ksud", 10, 11) == "failure:22",
          "input capacity enforced");
    input->identity.st_size = 0;
    check(profile_root_transfer("/ksud", 10, 11) == "failure:22",
          "empty input rejected");
    input->identity.st_size = 42;
    model::install(11, 11, 0, O_RDWR | O_APPEND);
    check(profile_root_transfer("/ksud", 10, 11) == "failure:22",
          "append output rejected");
    originals();
    model::grant_result = 1;
    check(profile_root_transfer("/ksud", 10, 11) == "failure:1",
          "root denial explicit");
    drain();
    model::grant_result = 0;
    {
      std::lock_guard<std::mutex> lock(worker_mutex);
      check(profile_root_transfer("/ksud", 10, 11) == "failure:16",
            "busy root worker never queues");
    }
    drain();
    originals();
    output = model::install(11, 11, 0);
    model::request_delay = 130;
    check(profile_root_transfer("/ksud", 10, 11) ==
              sumhp::profile_transport::failure_receipt(ETIMEDOUT),
          "caller has bounded timeout");
    model::close(10);
    model::close(11);
    auto replacement = model::install(11, 99, 0);
    model::install(10, 98, 42, O_RDONLY);
    check(profile_root_transfer("/ksud", 10, 11) == "failure:16",
          "timed-out worker keeps one slot");
    drain();
    model::request_delay = 0;
    check(output->writes == 1 && replacement->writes == 0,
          "late completion owns original inode despite fd reuse");
    const int previous = model::requests.load();
    model::root_delay = 130;
    check(profile_root_transfer("/ksud", 10, 11) ==
              sumhp::profile_transport::failure_receipt(ETIMEDOUT),
          "root grant wait bounded");
    drain();
    model::root_delay = 0;
    check(model::requests.load() == previous,
          "deadline elapsed before root must prevent dispatch");
    {
      std::lock_guard<std::mutex> lock(model::mutex);
      check(model::files.size() == 2,
            "all duplicates released after delayed tasks");
    }
    std::cout << checks << " profile worker checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
