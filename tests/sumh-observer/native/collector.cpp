#include "collector.hpp"
#include "protocol.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#if defined(__ANDROID__) && __ANDROID_API__ < 31
#error "Observer artifacts require Android API 31+"
#endif

#ifndef SUMH_OBSERVER_BUILD_ID
#error "CMake must bind the collector to a source digest"
#endif
namespace sumh_observer {
namespace {
class Fd {
  int value_;

public:
  explicit Fd(int fd = -1) : value_(fd) {}
  ~Fd() {
    if (value_ >= 0)
      close(value_);
  }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  Fd(Fd &&other) noexcept : value_(other.release()) {}
  Fd &operator=(Fd &&other) noexcept {
    if (this != &other) {
      if (value_ >= 0)
        close(value_);
      value_ = other.release();
    }
    return *this;
  }
  int get() const { return value_; }
  int release() {
    const int fd = value_;
    value_ = -1;
    return fd;
  }
};
struct SysError : std::runtime_error {
  int code;
  explicit SysError(int e)
      : std::runtime_error(std::strerror(e > 0 ? e : EIO)),
        code(e > 0 ? e : EIO) {}
};
std::uint64_t now() {
  struct timespec t{};
  if (clock_gettime(CLOCK_BOOTTIME, &t))
    throw SysError(errno);
  return static_cast<std::uint64_t>(t.tv_sec) * 1000000000u +
         static_cast<unsigned>(t.tv_nsec);
}
class Deadline {
  std::uint64_t until_;

public:
  explicit Deadline(std::uint64_t start) : until_(start + 5000000000ull) {}
  bool expired() const { return now() >= until_; }
  void check() const {
    if (expired())
      throw SysError(ETIMEDOUT);
  }
};
json::Value stat_value(const struct stat &s) {
  if (s.st_size < 0 || s.st_mtim.tv_nsec < 0 || s.st_mtim.tv_nsec >= 1000000000)
    throw SysError(EIO);
  auto v = json::Value::object();
  v["dev"] = std::to_string(static_cast<std::uint64_t>(s.st_dev));
  v["ino"] = std::to_string(static_cast<std::uint64_t>(s.st_ino));
  v["mode"] = std::to_string(static_cast<unsigned>(s.st_mode));
  v["size"] = std::to_string(static_cast<std::int64_t>(s.st_size));
  // Decimal arithmetic avoids overflowing int64 at the far ends of time_t.
  const auto sec = static_cast<std::int64_t>(s.st_mtim.tv_sec);
  const auto nanos = static_cast<unsigned>(s.st_mtim.tv_nsec);
  std::ostringstream timestamp;
  if (sec < 0 && nanos) {
    const std::uint64_t magnitude = static_cast<std::uint64_t>(-(sec + 1));
    timestamp << '-' << magnitude << std::setw(9) << std::setfill('0')
              << (1000000000u - nanos);
  } else {
    timestamp << sec << std::setw(9) << std::setfill('0') << nanos;
  }
  auto decimal = timestamp.str();
  const bool negative = !decimal.empty() && decimal[0] == '-';
  auto first = decimal.find_first_not_of('0', negative ? 1 : 0);
  if (first == std::string::npos)
    decimal = "0";
  else
    decimal = (negative ? "-" : "") + decimal.substr(first);
  v["mtime_ns"] = decimal;
  return v;
}
json::Value envelope(const std::string &id, const std::string &source,
                     const std::string &status, bool complete, int err,
                     const json::Value &value = {}) {
  auto v = json::Value::object();
  v["id"] = id;
  v["source"] = source;
  v["status"] = status;
  v["complete"] = complete;
  v["errno"] =
      err > 0 && status != "not_run" ? json::Value(err) : json::Value();
  v["value"] = status == "not_run" ? json::Value() : value;
  return v;
}
json::Value hex_value(const std::string &bytes) {
  auto value = json::Value::object();
  value["hex"] = hex_bytes(bytes);
  return value;
}
class Collector {
  Request request_;
  std::uint64_t started_;
  Deadline deadline_;
  json::Value observations_ = json::Value::array();
  std::set<std::string> ids_;

public:
  explicit Collector(Request request)
      : request_(std::move(request)), started_(now()), deadline_(started_) {}
  void add(const json::Value &e) {
    if (!ids_.insert(e.at("id").s).second)
      throw std::logic_error("duplicate observation");
    observations_.push_back(e);
  }
  void skipped(const std::string &id, const std::string &source) {
    add(envelope(id, source, "not_run", false, 0));
  }
  template <class F>
  void probe(const std::string &id, const std::string &source, F function) {
    try {
      deadline_.check();
      add(envelope(id, source, "ok", true, 0, function()));
    } catch (const SysError &e) {
      add(envelope(id, source, e.code == ETIMEDOUT ? "not_run" : "error",
                   e.code != ETIMEDOUT, e.code));
    }
  }
  Bytes bytes(int fd, std::size_t limit) {
    return read_bounded(
        [fd](char *buffer, std::size_t size, int &err) {
          const auto count = ::read(fd, buffer, size);
          if (count < 0)
            err = errno;
          return count;
        },
        [this] { return deadline_.expired(); }, limit);
  }
  std::string exact_read(int fd, std::size_t limit) {
    auto b = bytes(fd, limit);
    if (b.error)
      throw SysError(b.error);
    if (!b.complete)
      throw SysError(EFBIG);
    return b.bytes;
  }
  void read_probe(const std::string &id, const std::string &source, int fd) {
    const auto b = bytes(fd, kReadLimit);
    if (b.error)
      add(envelope(id, source, b.error == ETIMEDOUT ? "not_run" : "error",
                   b.error != ETIMEDOUT, b.error));
    else
      add(envelope(id, source, b.complete ? "ok" : "truncated", b.complete, 0,
                   hex_value(b.bytes)));
  }
  Fd open_absolute(const char *path, int flags = O_RDONLY) {
    deadline_.check();
    Fd fd(open(path, flags | O_CLOEXEC));
    if (fd.get() < 0)
      throw SysError(errno);
    return fd;
  }
  Fd open_child(int parent, const char *name, bool directory = false) {
    deadline_.check();
    Fd fd(openat(parent, name,
                 O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
                     (directory ? O_DIRECTORY : O_NONBLOCK)));
    if (fd.get() < 0)
      throw SysError(errno);
    struct stat s{};
    if (fstat(fd.get(), &s))
      throw SysError(errno);
    if (directory ? !S_ISDIR(s.st_mode)
                  : (!S_ISREG(s.st_mode) || s.st_nlink != 1))
      throw SysError(EINVAL);
    return fd;
  }
  Fd open_root() {
    Fd current;
    std::size_t at;
    if (request_.fixture_root.compare(0, 16, "/data/local/tmp/") != 0) {
      const auto owned = request_.fixture_root.rfind("/sumh-observer/");
      const auto files = request_.fixture_root.substr(0, owned);
      // Only Android's fixed platform-owned app ancestry may contain an
      // alias. Every component below this final filesDir stays no-follow.
      current =
          open_absolute(files.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
      at = owned + 1;
    } else {
      current = open_absolute("/", O_RDONLY | O_DIRECTORY);
      at = 1;
    }
    while (at < request_.fixture_root.size()) {
      auto end = request_.fixture_root.find('/', at);
      if (end == std::string::npos)
        end = request_.fixture_root.size();
      current =
          open_child(current.get(),
                     request_.fixture_root.substr(at, end - at).c_str(), true);
      at = end + 1;
    }
    return current;
  }
  void read_absolute(const std::string &id, const char *path,
                     std::size_t limit) {
    try {
      auto fd = open_absolute(path);
      auto b = bytes(fd.get(), limit);
      if (b.error)
        add(envelope(id, "self", b.error == ETIMEDOUT ? "not_run" : "error",
                     b.error != ETIMEDOUT, b.error));
      else
        add(envelope(id, "self", b.complete ? "ok" : "truncated", b.complete, 0,
                     hex_value(b.bytes)));
    } catch (const SysError &e) {
      add(envelope(id, "self", e.code == ETIMEDOUT ? "not_run" : "error",
                   e.code != ETIMEDOUT, e.code));
    }
  }
  void namespace_sample(const std::string &phase) {
    const std::string prefix = "namespace." + phase;
    probe(prefix + ".link", "self", [&] {
      std::array<char, 1024> buffer{};
      auto count = readlink("/proc/self/ns/mnt", buffer.data(), buffer.size());
      if (count < 0)
        throw SysError(errno);
      if (static_cast<std::size_t>(count) == buffer.size())
        throw SysError(ENAMETOOLONG);
      return hex_value(
          std::string(buffer.data(), static_cast<std::size_t>(count)));
    });
    probe(prefix + ".stat", "self", [&] {
      struct stat s{};
      if (stat("/proc/self/ns/mnt", &s))
        throw SysError(errno);
      return stat_value(s);
    });
    probe(prefix + ".fd", "self", [&] {
      auto fd = open_absolute("/proc/self/ns/mnt");
      struct stat s{};
      if (fstat(fd.get(), &s))
        throw SysError(errno);
      return stat_value(s);
    });
  }
  void identity_probes() {
    probe("identity.start_ticks", "self", [&] {
      auto fd = open_absolute("/proc/self/stat");
      const auto text = exact_read(fd.get(), 4096);
      const auto close = text.rfind(')');
      if (close == std::string::npos || close + 2 >= text.size())
        throw SysError(EIO);
      std::istringstream fields(text.substr(close + 2));
      std::string value;
      for (unsigned field = 3; field <= 22; ++field)
        if (!(fields >> value))
          throw SysError(EIO);
      if (value.empty() || !std::all_of(value.begin(), value.end(), [](char c) {
            return c >= '0' && c <= '9';
          }))
        throw SysError(EIO);
      return json::Value(value);
    });
    read_absolute("identity.selinux", "/proc/self/attr/current", 4096);
    read_absolute("boot.id", "/proc/sys/kernel/random/boot_id", 128);
  }
  json::Value directory(int parent, bool validate) {
    auto fd = open_child(parent, ".", true);
    DIR *raw = fdopendir(fd.get());
    if (!raw)
      throw SysError(errno);
    (void)fd.release();
    std::unique_ptr<DIR, decltype(&closedir)> dir(raw, &closedir);
    auto result = json::Value::object();
    result["entries"] = json::Value::array();
    result["sorted"] = false;
    const std::set<std::string> names = {
        "fixture.txt",    "present.txt", "missing.txt", "dir",
        "unicode-界.txt", "map.bin",     "link.txt"};
    while (true) {
      deadline_.check();
      errno = 0;
      auto entry = readdir(dir.get());
      if (!entry) {
        if (errno)
          throw SysError(errno);
        break;
      }
      const std::string name(entry->d_name);
      if (name == "." || name == "..")
        continue;
      if (result.at("entries").a.size() >= kDirentLimit)
        throw SysError(EOVERFLOW);
      if (validate) {
        if (!names.count(name))
          throw SysError(EINVAL);
        struct stat s{};
        if (fstatat(parent, name.c_str(), &s, AT_SYMLINK_NOFOLLOW))
          throw SysError(errno);
        if (name == "dir"        ? !S_ISDIR(s.st_mode)
            : name == "link.txt" ? !S_ISLNK(s.st_mode)
                                 : (!S_ISREG(s.st_mode) || s.st_nlink != 1))
          throw SysError(EINVAL);
      }
      auto e = json::Value::object();
      e["name_hex"] = hex_bytes(name);
      e["ino"] = std::to_string(static_cast<std::uint64_t>(entry->d_ino));
      e["type"] = static_cast<int>(entry->d_type);
      result["entries"].push_back(e);
    }
    return result;
  }
  void scan(const std::string &id, const std::string &source, const char *path,
            std::uint64_t begin = 0, std::uint64_t end = 0) {
    try {
      auto fd = open_absolute(path);
      auto s = scan_lines(
          [&](char *b, std::size_t size, int &err) {
            const auto n = read(fd.get(), b, size);
            if (n < 0)
              err = errno;
            return n;
          },
          [&] { return deadline_.expired(); },
          [&](const std::string &line) {
            return begin ? maps_overlap(line, begin, end)
                         : mount_relevant(line, request_.fixture_root,
                                          id == "mount.info");
          });
      auto value = json::Value::object();
      value["lines_hex"] = json::Value::array();
      for (const auto &line : s.lines)
        value["lines_hex"].push_back(hex_bytes(line));
      value["scanned_bytes"] = std::to_string(s.scanned);
      if (begin)
        value["matched_mapping"] = !s.lines.empty();
      if (s.error)
        add(envelope(id, source, s.error == ETIMEDOUT ? "not_run" : "error",
                     s.error != ETIMEDOUT, s.error));
      else
        add(envelope(id, source, s.complete ? "ok" : "truncated", s.complete, 0,
                     value));
    } catch (const SysError &e) {
      add(envelope(id, source, e.code == ETIMEDOUT ? "not_run" : "error",
                   e.code != ETIMEDOUT, e.code));
    }
  }
  void map_probes(const std::string &prefix, const std::string &source, int raw,
                  bool include_read) {
    bool valid = false;
    probe(prefix + ".stat", source, [&] {
      struct stat s{};
      if (fstat(raw, &s))
        throw SysError(errno);
      if (!S_ISREG(s.st_mode) || s.st_size != 4096)
        throw SysError(EINVAL);
      const int flags = fcntl(raw, F_GETFL);
      if (flags < 0)
        throw SysError(errno);
      if ((flags & O_ACCMODE) != O_RDONLY)
        throw SysError(EINVAL);
      valid = true;
      return stat_value(s);
    });
    if (!valid) {
      if (include_read)
        skipped(prefix + ".read", source);
      skipped(prefix + ".mapping", source);
      skipped(prefix + ".maps", "self");
      return;
    }
    std::string content;
    int content_error = 0;
    try {
      // pread on a duplicated delegated FD leaves the caller's offset
      // unchanged.
      std::size_t offset = 0;
      std::array<char, 4096> bytes{};
      while (offset != bytes.size()) {
        deadline_.check();
        auto n = pread(raw, bytes.data() + offset, bytes.size() - offset,
                       static_cast<off_t>(offset));
        if (n < 0) {
          if (errno == EINTR)
            continue;
          throw SysError(errno);
        }
        if (!n)
          throw SysError(EIO);
        offset += static_cast<std::size_t>(n);
      }
      content.assign(bytes.data(), bytes.size());
      if (content != map_content())
        throw SysError(EINVAL);
    } catch (const SysError &e) {
      content_error = e.code;
    }
    if (include_read) {
      if (content_error)
        add(envelope(prefix + ".read", source,
                     content_error == ETIMEDOUT ? "not_run" : "error",
                     content_error != ETIMEDOUT, content_error));
      else
        add(envelope(prefix + ".read", source, "ok", true, 0,
                     hex_value(content)));
    }
    if (content_error) {
      add(envelope(prefix + ".mapping", source,
                   content_error == ETIMEDOUT ? "not_run" : "error",
                   content_error != ETIMEDOUT, content_error));
      skipped(prefix + ".maps", "self");
      return;
    }
    void *mapping = MAP_FAILED;
    struct MappingGuard {
      void *&value;
      ~MappingGuard() {
        if (value != MAP_FAILED)
          munmap(value, 4096);
      }
    } guard{mapping};
    probe(prefix + ".mapping", source, [&] {
      mapping = mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE, raw, 0);
      if (mapping == MAP_FAILED)
        throw SysError(errno);
      const auto address = reinterpret_cast<std::uintptr_t>(mapping);
      std::ostringstream start, end;
      start << std::hex << address;
      end << std::hex << address + 4096;
      auto value = json::Value::object();
      value["address_start"] = start.str();
      value["address_end"] = end.str();
      value["length"] = "4096";
      return value;
    });
    if (mapping == MAP_FAILED)
      skipped(prefix + ".maps", "self");
    else
      scan(prefix + ".maps", "self", "/proc/self/maps",
           reinterpret_cast<std::uintptr_t>(mapping),
           reinterpret_cast<std::uintptr_t>(mapping) + 4096);
  }
  void path_probes() {
    Fd root;
    bool marker_ok = false;
    probe("fixture.marker", "path", [&] {
      root = open_root();
      auto fd = open_child(root.get(), "fixture.txt");
      const auto marker =
          exact_read(fd.get(), fixture_marker(request_.run_id).size());
      marker_ok = marker == fixture_marker(request_.run_id);
      return hex_value(marker);
    });
    bool topology_ok = false;
    if (marker_ok) {
      try {
        deadline_.check();
        auto entries = directory(root.get(), true);
        topology_ok = true;
        add(envelope("directory.entries", "path", "ok", true, 0, entries));
      } catch (const SysError &e) {
        add(envelope("directory.entries", "path",
                     e.code == EOVERFLOW   ? "truncated"
                     : e.code == ETIMEDOUT ? "not_run"
                                           : "error",
                     e.code != EOVERFLOW && e.code != ETIMEDOUT, e.code));
      }
    } else
      skipped("directory.entries", "path");
    if (!topology_ok) {
      for (const char *id :
           {"file.present.stat", "file.present.open_stat", "file.present.read",
            "file.missing.stat", "file.unicode.read", "link.target",
            "map.path.stat", "map.path.mapping"})
        skipped(id, "path");
      skipped("map.path.maps", "self");
      return;
    }
    probe("file.present.stat", "path", [&] {
      struct stat s{};
      if (fstatat(root.get(), "present.txt", &s, AT_SYMLINK_NOFOLLOW))
        throw SysError(errno);
      return stat_value(s);
    });
    Fd present;
    probe("file.present.open_stat", "path", [&] {
      present = open_child(root.get(), "present.txt");
      struct stat s{};
      if (fstat(present.get(), &s))
        throw SysError(errno);
      return stat_value(s);
    });
    if (present.get() < 0)
      skipped("file.present.read", "path");
    else
      read_probe("file.present.read", "path", present.get());
    probe("file.missing.stat", "path", [&] {
      struct stat s{};
      if (fstatat(root.get(), "missing.txt", &s, AT_SYMLINK_NOFOLLOW))
        throw SysError(errno);
      return stat_value(s);
    });
    try {
      auto unicode = open_child(root.get(), "unicode-界.txt");
      read_probe("file.unicode.read", "path", unicode.get());
    } catch (const SysError &e) {
      add(envelope("file.unicode.read", "path",
                   e.code == ETIMEDOUT ? "not_run" : "error",
                   e.code != ETIMEDOUT, e.code));
    }
    probe("link.target", "path", [&] {
      std::array<char, 1024> bytes{};
      auto count =
          readlinkat(root.get(), "link.txt", bytes.data(), bytes.size());
      if (count < 0)
        throw SysError(errno);
      if (static_cast<std::size_t>(count) == bytes.size())
        throw SysError(ENAMETOOLONG);
      return hex_value(
          std::string(bytes.data(), static_cast<std::size_t>(count)));
    });
    try {
      auto map = open_child(root.get(), "map.bin");
      map_probes("map.path", "path", map.get(), false);
    } catch (const SysError &e) {
      add(envelope("map.path.stat", "path",
                   e.code == ETIMEDOUT ? "not_run" : "error",
                   e.code != ETIMEDOUT, e.code));
      skipped("map.path.mapping", "path");
      skipped("map.path.maps", "self");
    }
  }
  void delegated_probes(int raw) {
    if (raw < 0) {
      for (const char *suffix : {"stat", "read", "mapping"})
        skipped(std::string("map.fd.") + suffix, "delegated_fd");
      skipped("map.fd.maps", "self");
      return;
    }
    try {
      deadline_.check();
      Fd copy(fcntl(raw, F_DUPFD_CLOEXEC, 0));
      if (copy.get() < 0)
        throw SysError(errno);
      map_probes("map.fd", "delegated_fd", copy.get(), true);
    } catch (const SysError &e) {
      add(envelope("map.fd.stat", "delegated_fd",
                   e.code == ETIMEDOUT ? "not_run" : "error",
                   e.code != ETIMEDOUT, e.code));
      skipped("map.fd.read", "delegated_fd");
      skipped("map.fd.mapping", "delegated_fd");
      skipped("map.fd.maps", "self");
    }
  }
  std::string finish(const std::string &request_bytes, const std::string &role,
                     int delegated_fd) {
    auto result = json::Value::object();
    result["schema_version"] = 1;
    result["resource"] = "sumh_observer_sample";
    result["run_id"] = request_.run_id;
    result["request_sha256"] = sha256(request_bytes);
    result["fixture_manifest_sha256"] = sha256(fixture_marker(request_.run_id));
    result["collector_build_id"] = SUMH_OBSERVER_BUILD_ID;
    result["requested_role"] = role;
    result["non_atomic"] = true;
    result["context"] = request_.context;
    std::array<char, 16> random{};
    std::size_t offset = 0;
    while (offset < random.size()) {
      deadline_.check();
      const auto n =
          getrandom(random.data() + offset, random.size() - offset, 0);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        throw SysError(errno);
      }
      if (!n)
        throw SysError(EIO);
      offset += static_cast<std::size_t>(n);
    }
    result["observer_instance_id"] =
        hex_bytes(std::string(random.data(), random.size()));
    auto identity = json::Value::object();
    identity["uid"] = std::to_string(getuid());
    identity["euid"] = std::to_string(geteuid());
    identity["gid"] = std::to_string(getgid());
    identity["egid"] = std::to_string(getegid());
    identity["pid"] = std::to_string(getpid());
    identity["ppid"] = std::to_string(getppid());
    identity["tid"] = std::to_string(syscall(SYS_gettid));
    identity["groups"] = json::Value::array();
    int count = getgroups(0, nullptr);
    if (count < 0 || count > 65536)
      throw SysError(count < 0 ? errno : E2BIG);
    std::vector<gid_t> groups(static_cast<std::size_t>(count));
    if (count) {
      count = getgroups(count, groups.data());
      if (count < 0)
        throw SysError(errno);
    }
    for (int i = 0; i < count; ++i)
      identity["groups"].push_back(
          std::to_string(groups[static_cast<std::size_t>(i)]));
    result["identity"] = identity;
    result["started_boottime_ns"] = std::to_string(started_);
    identity_probes();
    namespace_sample("start");
    path_probes();
    if (role == "isolated" || delegated_fd >= 0)
      delegated_probes(delegated_fd);
    scan("mount.info", "self", "/proc/self/mountinfo");
    scan("mount.mounts", "self", "/proc/self/mounts");
    namespace_sample("end");
    result["finished_boottime_ns"] = std::to_string(now());
    result["observations"] = observations_;
    // Each stream may preserve 64 KiB raw data. Cap the total result by
    // explicitly making large raw sections incomplete, never by clipping JSON.
    auto output = json::dump(result);
    if (output.size() > kResultLimit) {
      for (auto &e : result["observations"].a) {
        if (e.at("value").is_object() && e.at("value").contains("lines_hex")) {
          e["value"]["lines_hex"] = json::Value::array();
          if (e["value"].contains("matched_mapping"))
            e["value"]["matched_mapping"] = false;
          e["status"] = "truncated";
          e["complete"] = false;
          e["errno"] = json::Value();
        }
      }
      output = json::dump(result);
    }
    if (output.size() > kResultLimit)
      throw std::runtime_error("result exceeds protocol budget");
    return output;
  }
};

Fd create_open_directory(int parent, const char *name, bool exclusive) {
  if (mkdirat(parent, name, 0700) && (exclusive || errno != EEXIST))
    throw SysError(errno);
  Fd fd(openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (fd.get() < 0)
    throw SysError(errno);
  struct stat s{};
  if (fstat(fd.get(), &s))
    throw SysError(errno);
  if (s.st_uid != geteuid() || !S_ISDIR(s.st_mode))
    throw SysError(EPERM);
  return fd;
}
void create_file(int root, const char *name, const std::string &bytes) {
  Fd fd(openat(root, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
               0600));
  if (fd.get() < 0)
    throw SysError(errno);
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    auto n = write(fd.get(), bytes.data() + offset, bytes.size() - offset);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      throw SysError(errno);
    }
    if (!n)
      throw SysError(EIO);
    offset += static_cast<std::size_t>(n);
  }
  if (fsync(fd.get()))
    throw SysError(errno);
}
} // namespace
std::string collect(const std::string &request, const std::string &role,
                    int delegated_fd) {
  if (role != "ordinary" && role != "isolated" && role != "root")
    throw std::invalid_argument("invalid observer role");
  if (role == "root" && (getuid() != 0 || geteuid() != 0))
    throw std::invalid_argument("root role requires real and effective UID 0");
  return Collector(parse_request(request)).finish(request, role, delegated_fd);
}
std::string create_fixture(const std::string &files_dir,
                           const std::string &run_id) {
  const auto alias = std::string("/data/data/") + kPackage + "/files";
  const auto canonical =
      files_dir == alias ? std::string("/data/user/0/") + kPackage + "/files"
                         : files_dir;
  if (!valid_files_dir(canonical) || !valid_run_id(run_id))
    throw std::invalid_argument("invalid APK filesDir or run ID");
  Fd current(
      open(files_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (current.get() < 0)
    throw SysError(errno);
  struct stat s{};
  if (fstat(current.get(), &s))
    throw SysError(errno);
  if (s.st_uid != geteuid())
    throw SysError(EPERM);
  auto fixtures = create_open_directory(current.get(), "sumh-observer", false);
  auto run = create_open_directory(fixtures.get(), run_id.c_str(), true);
  create_file(run.get(), "present.txt", "SUMH observer present v1\n");
  auto child = create_open_directory(run.get(), "dir", true);
  create_file(child.get(), "child.txt", "SUMH observer child v1\n");
  create_file(run.get(), "unicode-界.txt", "SUMH observer unicode v1\n");
  create_file(run.get(), "map.bin", map_content());
  if (symlinkat("present.txt", run.get(), "link.txt"))
    throw SysError(errno);
  // Publish the marker last, so a failed construction cannot be sampled as a
  // completed synthetic fixture. Retain partial runs for explicit app cleanup.
  create_file(run.get(), "fixture.txt", fixture_marker(run_id));
  if (fsync(run.get()) || fsync(fixtures.get()))
    throw SysError(errno);
  auto request = json::Value::object();
  request["schema_version"] = 1;
  request["resource"] = "sumh_observer_request";
  request["run_id"] = run_id;
  request["fixture_root"] = canonical + "/sumh-observer/" + run_id;
  request["context"] = json::Value::object();
  request["context"]["boot_id"] = json::Value();
  request["context"]["config_revision"] = json::Value();
  return json::dump(request);
}
} // namespace sumh_observer
