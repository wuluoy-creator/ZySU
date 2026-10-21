// Native syscall model only; actual Fd/failure/clear_log bodies are included.
// No Android, Linux filesystem, process locking or hostile-root guarantee.
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace logclear {
constexpr int write_only = 1, read_write = 2, create = 4, trunc = 8,
              nonblock = 16, cloexec = 32, nofollow = 64, lock_ex = 1,
              lock_nb = 2;
constexpr unsigned regular_type = 0100000, directory_type = 0040000,
                   fifo_type = 0010000, symlink_type = 0120000,
                   type_mask = 0170000;
struct ModelStat {
  unsigned st_mode = regular_type;
  unsigned long st_nlink = 1;
  long long st_size = 200;
};
struct Node {
  ModelStat stat;
  bool exists = true;
  unsigned permissions = 0644;
};
struct Descriptor {
  int node = -1;
  bool live = false, inspected = false;
};
struct Open {
  std::string path;
  int flags;
  unsigned mode;
};
std::array<Node, 5> nodes;
std::array<Descriptor, 8> descriptors;
std::vector<Open> opens;
std::vector<int> closes;
std::vector<std::string> events;
int next_fd = 10, lock_owner = -1, truncate_calls = 0, formatted_errno = 0,
    checks = 0;
int lock_open_error = 0, log_open_error = 0, lock_stat_error = 0,
    log_stat_error = 0, flock_error = 0, lock_chmod_error = 0,
    log_chmod_error = 0, truncate_error = 0;
bool busy = false, fifo_reader = false, would_block = false,
     invalid_descriptor = false, unsafe_truncate = false;
std::string current_case, expected_failure;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};
void check(bool ok, const char *label) {
  ++checks;
  if (!ok)
    throw TestFailure(label);
}
Descriptor *descriptor(int fd) {
  if (fd < 10 || fd >= next_fd ||
      !descriptors[static_cast<unsigned>(fd - 10)].live) {
    invalid_descriptor = true;
    errno = EBADF;
    return nullptr;
  }
  return &descriptors[static_cast<unsigned>(fd - 10)];
}
int open(const char *path, int flags, ...) {
  unsigned mode = 0;
  if (flags & create) {
    va_list ap;
    va_start(ap, flags);
    mode = static_cast<unsigned>(va_arg(ap, int));
    va_end(ap);
  }
  opens.push_back({path, flags, mode});
  events.push_back(std::string("open:") + path);
  const int node = std::string(path) == "/private/current.log.lock" ? 0
                   : std::string(path) == "/private/current.log"    ? 1
                                                                    : -1;
  if (node < 0) {
    errno = EPERM;
    return -1;
  }
  const int fault = node == 0 ? lock_open_error : log_open_error;
  if (fault) {
    errno = fault;
    return -1;
  }
  Node &n = nodes[static_cast<unsigned>(node)];
  if (!n.exists) {
    if (flags & create) {
      n.exists = true;
      n.permissions = mode;
    } else {
      errno = ENOENT;
      return -1;
    }
  }
  const auto type = n.stat.st_mode & type_mask;
  if (type == symlink_type && (flags & nofollow)) {
    errno = ELOOP;
    return -1;
  }
  // Linux permits O_RDWR FIFO open without a peer. A write-only FIFO open
  // requires a reader: nonblocking fails with ENXIO, otherwise it would wait.
  if (type == fifo_type && (flags & 3) == write_only && !fifo_reader) {
    if (flags & nonblock)
      errno = ENXIO;
    else {
      would_block = true;
      errno = EWOULDBLOCK;
    }
    return -1;
  }
  const int fd = next_fd++;
  descriptors[static_cast<unsigned>(fd - 10)] = {node, true, false};
  if (flags & trunc)
    n.stat.st_size = 0;
  return fd;
}
int close(int fd) {
  auto *d = descriptor(fd);
  if (!d)
    return -1;
  closes.push_back(d->node);
  events.push_back("close:" + std::to_string(d->node));
  d->live = false;
  if (lock_owner == fd)
    lock_owner = -1;
  // Force errno contamination during unwinding: production failure must have
  // captured/formatted the real failing call before RAII closes descriptors.
  errno = ECANCELED;
  return 0;
}
int fstat(int fd, ModelStat *st) {
  auto *d = descriptor(fd);
  if (!d)
    return -1;
  events.push_back("stat:" + std::to_string(d->node));
  const int fault = d->node == 0 ? lock_stat_error : log_stat_error;
  if (fault) {
    errno = fault;
    return -1;
  }
  *st = nodes[static_cast<unsigned>(d->node)].stat;
  d->inspected = true;
  return 0;
}
int flock(int fd, int flags) {
  auto *d = descriptor(fd);
  if (!d)
    return -1;
  events.push_back("flock:" + std::to_string(flags));
  if (flock_error) {
    errno = flock_error;
    return -1;
  }
  if (busy) {
    if (!(flags & lock_nb))
      would_block = true;
    errno = EWOULDBLOCK;
    return -1;
  }
  lock_owner = fd;
  return 0;
}
int fchmod(int fd, unsigned mode) {
  auto *d = descriptor(fd);
  if (!d)
    return -1;
  events.push_back("chmod:" + std::to_string(d->node));
  const int fault = d->node == 0 ? lock_chmod_error : log_chmod_error;
  if (fault) {
    errno = fault;
    return -1;
  }
  nodes[static_cast<unsigned>(d->node)].permissions = mode;
  return 0;
}
int ftruncate(int fd, long long size) {
  auto *d = descriptor(fd);
  if (!d)
    return -1;
  ++truncate_calls;
  events.push_back("truncate:" + std::to_string(d->node));
  const auto &st = nodes[static_cast<unsigned>(d->node)].stat;
  if (d->node != 1 || !d->inspected || lock_owner < 0 ||
      (st.st_mode & type_mask) != regular_type || st.st_nlink != 1 ||
      st.st_size < 0)
    unsafe_truncate = true;
  if (truncate_error) {
    errno = truncate_error;
    return -1;
  }
  nodes[static_cast<unsigned>(d->node)].stat.st_size = size;
  return 0;
}
const char *error_message(int error) {
  formatted_errno = error;
  return std::strerror(error);
}
bool is_regular(unsigned mode) { return (mode & type_mask) == regular_type; }
void reset() {
  nodes = {};
  for (auto &n : nodes)
    n = Node{};
  nodes[2].stat.st_size = 301;
  nodes[3].stat.st_size = 302;
  nodes[4].stat.st_size = 303;
  descriptors = {};
  opens.clear();
  closes.clear();
  events.clear();
  next_fd = 10;
  lock_owner = -1;
  truncate_calls = formatted_errno = 0;
  lock_open_error = log_open_error = lock_stat_error = log_stat_error = 0;
  flock_error = lock_chmod_error = log_chmod_error = truncate_error = 0;
  busy = fifo_reader = would_block = invalid_descriptor = unsafe_truncate =
      false;
}
} // namespace logclear

namespace under_test {
#include "sumh_log_clear_under_test.inc"
}

namespace logclear {
std::string invoke() {
  try {
    under_test::clear_log("/private/current.log");
    return {};
  } catch (const std::runtime_error &e) {
    return e.what();
  }
}
void closed() {
  for (const auto &d : descriptors)
    check(!d.live, "all_descriptors_closed");
  check(lock_owner == -1 && !invalid_descriptor, "lock_released_once");
  check(nodes[2].stat.st_size == 301 && nodes[3].stat.st_size == 302 &&
            nodes[4].stat.st_size == 303,
        "sidecars_untouched");
  for (const auto &o : opens)
    check(o.path == "/private/current.log" ||
              o.path == "/private/current.log.lock",
          "only_current_and_lock_opened");
}
void checked_error(const std::string &text, int expected, const char *label) {
  check(!text.empty() && formatted_errno == expected, label);
  check(text.find(std::strerror(expected)) != std::string::npos,
        "failure_message_keeps_errno");
  closed();
}
void error(int expected, const char *label) {
  checked_error(invoke(), expected, label);
}
void success() {
  reset();
  check(invoke().empty(), "regular_clear_succeeds");
  check(truncate_calls == 1 && nodes[1].stat.st_size == 0 && !unsafe_truncate,
        "single_validated_truncate");
  check(nodes[0].permissions == 0600 && nodes[1].permissions == 0600,
        "permissions_private");
  check(opens.size() == 2 && closes == std::vector<int>({1, 0}),
        "reverse_descriptor_lifetime");
  check(opens[0].flags ==
                (create | read_write | nonblock | cloexec | nofollow) &&
            opens[0].mode == 0600,
        "lock_open_contract");
  check(opens[1].flags == (write_only | nonblock | cloexec | nofollow),
        "target_open_contract");
  check(events == std::vector<std::string>(
                      {"open:/private/current.log.lock", "stat:0", "flock:3",
                       "chmod:0", "open:/private/current.log", "stat:1",
                       "chmod:1", "truncate:1", "close:1", "close:0"}),
        "validated_locked_operation_order");
  closed();
}
void links() {
  for (unsigned count : {0u, 2u, 17u})
    for (int node : {0, 1}) {
      reset();
      nodes[static_cast<unsigned>(node)].stat.st_nlink = count;
      error(EINVAL, node == 0 ? "lock_link_count_rejected"
                              : "target_link_count_rejected");
      check(nodes[static_cast<unsigned>(node)].permissions == 0644,
            "invalid_link_inode_not_chmodded");
      check(!truncate_calls && !unsafe_truncate && nodes[1].stat.st_size == 200,
            "invalid_links_never_truncated");
      if (!node)
        check(opens.size() == 1, "invalid_lock_prevents_target_open");
    }
}
void types() {
  for (unsigned type : {directory_type, fifo_type, symlink_type})
    for (int node : {0, 1}) {
      reset();
      nodes[static_cast<unsigned>(node)].stat.st_mode = type;
      const auto text = invoke();
      check(!would_block, "special_file_open_never_waits");
      checked_error(text,
                    type == symlink_type             ? ELOOP
                    : type == fifo_type && node == 1 ? ENXIO
                                                     : EINVAL,
                    "nonregular_rejected");
      check(nodes[static_cast<unsigned>(node)].permissions == 0644,
            "nonregular_inode_not_chmodded");
      check(!truncate_calls && nodes[1].stat.st_size == 200,
            "special_file_never_truncated");
    }
  reset();
  fifo_reader = true;
  nodes[1].stat.st_mode = fifo_type;
  error(EINVAL, "fifo_with_reader_fails_descriptor_validation");
  check(!would_block && !truncate_calls && nodes[1].permissions == 0644,
        "fifo_with_reader_unchanged");
}
void negative_size() {
  for (int node : {0, 1}) {
    reset();
    nodes[static_cast<unsigned>(node)].stat.st_size = -1;
    error(EINVAL, "negative_size_rejected");
    check(!truncate_calls, "negative_size_never_truncated");
    check(nodes[static_cast<unsigned>(node)].permissions == 0644,
          "negative_size_inode_not_chmodded");
  }
}
void absent() {
  reset();
  nodes[1].exists = false;
  check(invoke().empty(), "absent_target_noop");
  check(!nodes[1].exists && !truncate_calls && opens.size() == 2,
        "absent_target_not_created");
  closed();
  reset();
  nodes[0].exists = false;
  check(invoke().empty() && nodes[0].exists && nodes[0].permissions == 0600,
        "lock_created_private");
  closed();
}
void failures() {
  struct Fault {
    int *value;
    int error;
    const char *label;
    int expected_opens;
  };
  const std::array<Fault, 8> faults{{
      {&lock_open_error, EACCES, "lock_open_errno", 1},
      {&lock_stat_error, EIO, "lock_stat_errno", 1},
      {&flock_error, EINTR, "flock_errno", 1},
      {&lock_chmod_error, EPERM, "lock_chmod_errno", 1},
      {&log_open_error, EACCES, "target_open_errno", 2},
      {&log_stat_error, EIO, "target_stat_errno", 2},
      {&log_chmod_error, EPERM, "target_chmod_errno", 2},
      {&truncate_error, ENOSPC, "truncate_errno", 2},
  }};
  for (const auto &fault : faults) {
    reset();
    *fault.value = fault.error;
    error(fault.error, fault.label);
    check(opens.size() == static_cast<unsigned>(fault.expected_opens),
          "failure_stops_later_opens");
    // Injected truncate failure changes no bytes in this model. This does
    // not establish real-filesystem error atomicity or durability.
    check(nodes[1].stat.st_size == 200, "modeled_failure_retains_log_bytes");
    check(truncate_calls == (fault.value == &truncate_error ? 1 : 0),
          "failure_stops_before_truncate");
  }
}
void contention() {
  reset();
  busy = true;
  error(EWOULDBLOCK, "lock_contention_errno");
  check(!would_block && opens.size() == 1 && !truncate_calls,
        "contention_nonblocking_before_target");
}
void fd_and_failure() {
  reset();
  {
    under_test::Fd invalid(-1);
  }
  check(closes.empty(), "negative_fd_not_closed");
  const int fd = open("/private/current.log", write_only);
  {
    under_test::Fd owned(fd);
    check(owned.get() == fd, "fd_accessor");
    check(owned.release() == fd, "fd_release");
  }
  check(closes.empty(), "released_fd_not_double_closed");
  close(fd);
  closed();
  reset();
  errno = 0;
  try {
    under_test::failure("fallback");
  } catch (const std::runtime_error &e) {
    check(formatted_errno == EIO && std::string(e.what()).find(std::strerror(
                                        EIO)) != std::string::npos,
          "zero_errno_fallback");
  }
}
} // namespace logclear

int main(int argc, char **argv) {
  using namespace logclear;
  const std::string selected = argc > 1 ? argv[1] : "all";
  expected_failure = argc > 2 ? argv[2] : "";
  try {
    const std::array<std::pair<const char *, void (*)()>, 8> cases{
        {{"success", success},
         {"links", links},
         {"types", types},
         {"negative_size", negative_size},
         {"absent", absent},
         {"failures", failures},
         {"contention", contention},
         {"fd_and_failure", fd_and_failure}}};
    bool found = false;
    for (const auto &item : cases)
      if (selected == "all" || selected == item.first) {
        found = true;
        current_case = item.first;
        item.second();
      }
    check(found, "known_scenario");
    std::printf("PASS clear_log: %d checks\n", checks);
    return 0;
  } catch (const TestFailure &e) {
    std::printf("CHECK FAILED [%s]: %s\n", current_case.c_str(), e.what());
    return expected_failure == e.what() ? 253 : 1;
  } catch (const std::exception &e) {
    std::printf("UNEXPECTED: %s\n", e.what());
    return 2;
  }
}
