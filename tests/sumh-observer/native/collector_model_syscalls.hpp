#pragma once
// Portable syscall double for compiling the complete production collector on
// Windows. Models lifecycle/error handling, never Android policy or kernel VFS.
#include "protocol.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef ELOOP
#define ELOOP 40
#endif
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_ACCMODE 3
#define O_CREAT 64
#define O_EXCL 128
#define O_NONBLOCK 2048
#define O_DIRECTORY 65536
#define O_NOFOLLOW 131072
#define O_CLOEXEC 524288
#define AT_SYMLINK_NOFOLLOW 256
#define F_GETFL 3
#define F_DUPFD_CLOEXEC 1030
#define PROT_READ 1
#define MAP_PRIVATE 2
#define MAP_FAILED reinterpret_cast<void *>(-1)
#define S_IFREG 0100000u
#define S_IFDIR 0040000u
#define S_IFLNK 0120000u
#define S_ISREG(m) (((m) & 0170000u) == S_IFREG)
#define S_ISDIR(m) (((m) & 0170000u) == S_IFDIR)
#define S_ISLNK(m) (((m) & 0170000u) == S_IFLNK)
#define CLOCK_BOOTTIME 7
#define SYS_gettid 186
using model_gid_t = unsigned;
struct model_stat {
  std::uint64_t st_dev = 3, st_ino = 0, st_nlink = 1;
  unsigned st_uid = 10001, st_mode = 0;
  std::int64_t st_size = 0;
  struct timespec st_mtim{0, 0};
};
struct model_dirent {
  std::uint64_t d_ino;
  unsigned char d_type;
  char d_name[256];
};
struct model_DIR {
  int fd;
  std::vector<std::string> names;
  std::size_t at = 0;
  model_dirent current{};
};
namespace observer_model {
struct Node {
  unsigned mode;
  std::string content;
  std::uint64_t ino;
  unsigned links = 1;
};
struct Handle {
  std::string path;
  std::size_t offset;
  int flags;
};
inline std::map<std::string, Node> nodes;
inline std::map<int, Handle> fds;
inline int nextfd = 10, calls = 0, fail_at = 0, close_errors = 0, maps = 0,
           map_calls = 0;
inline int uid = 10001, euid = 10001;
inline int actor_pid = 123;
inline bool timeout = false, fdopendir_failure = false;
inline std::uint64_t ticks = 0;
inline std::vector<std::string> read_paths;
inline const std::string runid(32, 'a');
inline const std::string files =
    "/data/user/0/com.zying.zysu.sumhobservertest/files";
inline const std::string root = files + "/sumh-observer/" + runid;
inline bool fault() { return ++calls == fail_at; }
inline int failure(int e = EIO) {
  errno = e;
  return -1;
}
inline std::string parent(const std::string &path) {
  auto split = path.rfind('/');
  return split == 0 ? "/" : path.substr(0, split);
}
inline std::string base(const std::string &path) {
  return path.substr(path.rfind('/') + 1);
}
inline std::string join(const std::string &parent, const char *child) {
  return std::string(child) == "."
             ? parent
             : parent + (parent == "/" ? "" : "/") + child;
}
inline void add(const std::string &path, unsigned mode,
                const std::string &content = {}) {
  nodes[path] = {mode, content, static_cast<std::uint64_t>(nodes.size() + 100)};
}
inline void reset() {
  nodes.clear();
  fds.clear();
  read_paths.clear();
  nextfd = 10;
  calls = 0;
  fail_at = 0;
  close_errors = 0;
  maps = 0;
  map_calls = 0;
  uid = euid = 10001;
  actor_pid = 123;
  timeout = false;
  ticks = 0;
  fdopendir_failure = false;
  add("/", S_IFDIR);
  for (const std::string path : {"/data", "/data/user", "/data/user/0",
                                 "/data/user/0/com.zying.zysu.sumhobservertest",
                                 "/proc", "/proc/self", "/proc/self/ns"})
    add(path, S_IFDIR);
  add(files, S_IFDIR);
  add(files + "/sumh-observer", S_IFDIR);
  add(root, S_IFDIR);
  add(root + "/fixture.txt", S_IFREG, sumh_observer::fixture_marker(runid));
  add(root + "/present.txt", S_IFREG, "SUMH observer present v1\n");
  add(root + "/unicode-界.txt", S_IFREG, "SUMH observer unicode v1\n");
  add(root + "/map.bin", S_IFREG, sumh_observer::map_content());
  add(root + "/link.txt", S_IFLNK, "present.txt");
  add(root + "/dir", S_IFDIR);
  add(root + "/dir/child.txt", S_IFREG, "SUMH observer child v1\n");
  add("/proc/self/ns/mnt", S_IFREG, "mnt:[9000]");
  nodes["/proc/self/ns/mnt"].ino = 9000;
  add("/proc/self/attr/current", S_IFREG, "u:r:untrusted_app:s0\n");
  add("/proc/sys/kernel/random/boot_id", S_IFREG,
      "11111111-2222-3333-4444-555555555555\n");
  std::string stat = "123 (a tricky ) name) S";
  for (int i = 4; i <= 22; ++i)
    stat += i == 22 ? " 1234567890123" : " 0";
  add("/proc/self/stat", S_IFREG, stat + "\n");
  add("/proc/self/maps", S_IFREG,
      "100000-101000 r--p 00000000 03:01 100 /synthetic/map.bin\n200000-201000 "
      "r--p 0 0:0 1 /unrelated\n");
  add("/proc/self/mountinfo", S_IFREG,
      "1 0 0:1 / / rw - tmpfs none rw\n2 1 0:2 / /data rw - ext4 /dev/x rw\n3 "
      "1 0:3 / /unrelated rw - tmpfs none rw\n");
  add("/proc/self/mounts", S_IFREG,
      "none / tmpfs rw 0 0\n/dev/x /data ext4 rw 0 0\nnone /unrelated tmpfs rw "
      "0 0\n");
}
inline int opening(const std::string &path, int flags) {
  auto found = nodes.find(path);
  if ((flags & O_CREAT) && found == nodes.end()) {
    add(path, S_IFREG);
    found = nodes.find(path);
  } else if ((flags & O_CREAT) && (flags & O_EXCL))
    return failure(EEXIST);
  if (found == nodes.end())
    return failure(ENOENT);
  if ((flags & O_NOFOLLOW) && S_ISLNK(found->second.mode))
    return failure(ELOOP);
  if ((flags & O_DIRECTORY) && !S_ISDIR(found->second.mode))
    return failure(ENOTDIR);
  const int fd = nextfd++;
  fds[fd] = {path, 0, flags};
  return fd;
}
inline int fill_stat(const std::string &path, model_stat *s) {
  auto it = nodes.find(path);
  if (it == nodes.end())
    return failure(ENOENT);
  *s = model_stat{};
  s->st_mode = it->second.mode;
  s->st_ino = it->second.ino;
  s->st_size = static_cast<std::int64_t>(it->second.content.size());
  s->st_nlink = it->second.links;
  s->st_uid = static_cast<unsigned>(euid);
  return 0;
}
} // namespace observer_model
inline int model_open(const char *path, int flags, ...) {
  using namespace observer_model;
  if (fault())
    return failure();
  return opening(path, flags);
}
inline int model_openat(int parent, const char *path, int flags, ...) {
  using namespace observer_model;
  if (fault())
    return failure();
  if (!fds.count(parent))
    return failure(EBADF);
  return opening(join(fds[parent].path, path), flags);
}
inline int model_close(int fd) {
  using namespace observer_model;
  if (!fds.erase(fd)) {
    ++close_errors;
    return failure(EBADF);
  }
  return 0;
}
inline int model_fstat(int fd, model_stat *s) {
  using namespace observer_model;
  if (fault())
    return failure();
  if (!fds.count(fd))
    return failure(EBADF);
  return fill_stat(fds[fd].path, s);
}
inline int model_stat_call(const char *path, model_stat *s) {
  using namespace observer_model;
  if (fault())
    return failure();
  return fill_stat(path, s);
}
inline int model_fstatat(int parent, const char *path, model_stat *s,
                         int flags) {
  using namespace observer_model;
  if (flags != AT_SYMLINK_NOFOLLOW)
    throw std::runtime_error("fstatat followed a link");
  if (fault())
    return failure();
  if (!fds.count(parent))
    return failure(EBADF);
  return fill_stat(join(fds[parent].path, path), s);
}
inline std::ptrdiff_t model_read(int fd, char *buffer, std::size_t size) {
  using namespace observer_model;
  if (fault())
    return failure();
  if (!fds.count(fd))
    return failure(EBADF);
  auto &handle = fds[fd];
  read_paths.push_back(handle.path);
  const auto &content = nodes.at(handle.path).content;
  const auto count =
      std::min(size, content.size() - std::min(content.size(), handle.offset));
  std::memcpy(buffer, content.data() + std::min(content.size(), handle.offset),
              count);
  handle.offset += count;
  return static_cast<std::ptrdiff_t>(count);
}
inline std::ptrdiff_t model_pread(int fd, char *buffer, std::size_t size,
                                  std::int64_t offset) {
  using namespace observer_model;
  if (fault())
    return failure();
  if (!fds.count(fd))
    return failure(EBADF);
  read_paths.push_back(fds[fd].path);
  const auto &content = nodes.at(fds[fd].path).content;
  const auto at = std::min(content.size(), static_cast<std::size_t>(offset));
  const auto count = std::min(size, content.size() - at);
  std::memcpy(buffer, content.data() + at, count);
  return static_cast<std::ptrdiff_t>(count);
}
inline std::ptrdiff_t model_write(int fd, const char *buffer,
                                  std::size_t size) {
  using namespace observer_model;
  if (fault())
    return failure();
  if (!fds.count(fd))
    return failure(EBADF);
  nodes[fds[fd].path].content.append(buffer, size);
  return static_cast<std::ptrdiff_t>(size);
}
inline std::ptrdiff_t model_readlink(const char *path, char *buffer,
                                     std::size_t size) {
  using namespace observer_model;
  if (fault())
    return failure();
  const auto it = nodes.find(path);
  if (it == nodes.end())
    return failure(ENOENT);
  const auto count = std::min(size, it->second.content.size());
  std::memcpy(buffer, it->second.content.data(), count);
  return static_cast<std::ptrdiff_t>(count);
}
inline std::ptrdiff_t model_readlinkat(int fd, const char *name, char *buffer,
                                       std::size_t size) {
  using namespace observer_model;
  if (!fds.count(fd))
    return failure(EBADF);
  return model_readlink(join(fds[fd].path, name).c_str(), buffer, size);
}
inline model_DIR *model_fdopendir(int fd) {
  using namespace observer_model;
  if (fault() || fdopendir_failure) {
    failure();
    return nullptr;
  }
  auto *out = new model_DIR;
  out->fd = fd;
  for (const auto &item : nodes)
    if (parent(item.first) == fds.at(fd).path)
      out->names.push_back(base(item.first));
  return out;
}
inline model_dirent *model_readdir(model_DIR *dir) {
  using namespace observer_model;
  if (fault()) {
    failure();
    return nullptr;
  }
  if (dir->at == dir->names.size())
    return nullptr;
  const auto &name = dir->names[dir->at++];
  const auto &node = nodes.at(join(fds.at(dir->fd).path, name.c_str()));
  dir->current = {};
  dir->current.d_ino = node.ino;
  dir->current.d_type = S_ISREG(node.mode) ? 8 : S_ISDIR(node.mode) ? 4 : 10;
  std::strcpy(dir->current.d_name, name.c_str());
  return &dir->current;
}
inline int model_closedir(model_DIR *dir) {
  const int result = model_close(dir->fd);
  delete dir;
  return result;
}
inline int model_fcntl(int fd, int cmd, ...) {
  using namespace observer_model;
  if (fault())
    return failure();
  if (!fds.count(fd))
    return failure(EBADF);
  if (cmd == F_GETFL)
    return fds.at(fd).flags;
  if (cmd != F_DUPFD_CLOEXEC)
    throw std::runtime_error("unexpected fcntl");
  const int copy = nextfd++;
  fds[copy] = fds.at(fd);
  return copy;
}
inline void *model_mmap(void *address, std::size_t size, int protection,
                        int flags, int fd, std::int64_t offset) {
  using namespace observer_model;
  if (address || size != 4096 || protection != PROT_READ ||
      flags != MAP_PRIVATE || offset || !fds.count(fd))
    throw std::runtime_error("unsafe mapping");
  if (fault()) {
    failure();
    return MAP_FAILED;
  }
  ++maps;
  ++map_calls;
  return reinterpret_cast<void *>(0x100000);
}
inline int model_munmap(void *address, std::size_t size) {
  using namespace observer_model;
  if (address != reinterpret_cast<void *>(0x100000) || size != 4096 ||
      maps != 1)
    throw std::runtime_error("invalid unmap");
  --maps;
  return 0;
}
inline int model_clock_gettime(int clock, struct timespec *value) {
  using namespace observer_model;
  if (clock != CLOCK_BOOTTIME)
    throw std::runtime_error("wrong clock");
  ticks += timeout ? 1000000000 : 1000;
  value->tv_sec = static_cast<time_t>(ticks / 1000000000);
  value->tv_nsec = static_cast<long>(ticks % 1000000000);
  return 0;
}
inline std::ptrdiff_t model_getrandom(char *buffer, std::size_t size,
                                      int flags) {
  using namespace observer_model;
  if (flags)
    throw std::runtime_error("random flags");
  if (fault())
    return failure();
  std::memset(buffer, uid & 255, size);
  return static_cast<std::ptrdiff_t>(size);
}
inline unsigned model_getuid() {
  return static_cast<unsigned>(observer_model::uid);
}
inline unsigned model_geteuid() {
  return static_cast<unsigned>(observer_model::euid);
}
inline unsigned model_getgid() {
  return static_cast<unsigned>(observer_model::uid);
}
inline unsigned model_getegid() {
  return static_cast<unsigned>(observer_model::euid);
}
inline int model_getpid() { return observer_model::actor_pid; }
inline int model_getppid() { return 1; }
inline long model_syscall(int n) {
  if (n != SYS_gettid)
    throw std::runtime_error("wrong syscall");
  return 123;
}
inline int model_getgroups(int count, model_gid_t *groups) {
  using namespace observer_model;
  if (fault())
    return failure();
  if (!count)
    return 2;
  groups[0] = 10001;
  groups[1] = 9997;
  return 2;
}
inline int model_mkdirat(int fd, const char *name, unsigned mode) {
  using namespace observer_model;
  if (mode != 0700)
    throw std::runtime_error("broad directory permission");
  if (fault())
    return failure();
  if (!fds.count(fd))
    return failure(EBADF);
  const auto path = join(fds[fd].path, name);
  if (nodes.count(path))
    return failure(EEXIST);
  add(path, S_IFDIR);
  return 0;
}
inline int model_symlinkat(const char *target, int fd, const char *name) {
  using namespace observer_model;
  if (fault())
    return failure();
  if (!fds.count(fd))
    return failure(EBADF);
  const auto path = join(fds[fd].path, name);
  if (nodes.count(path))
    return failure(EEXIST);
  add(path, S_IFLNK, target);
  return 0;
}
inline int model_fsync(int) {
  return observer_model::fault() ? observer_model::failure() : 0;
}

#define gid_t model_gid_t
#define stat(...) model_stat_call(__VA_ARGS__)
#define DIR model_DIR
#define close model_close
#define open model_open
#define openat model_openat
#define fstat model_fstat
#define fstatat model_fstatat
#define read model_read
#define pread model_pread
#define write model_write
#define readlink model_readlink
#define readlinkat model_readlinkat
#define fdopendir model_fdopendir
#define readdir model_readdir
#define closedir model_closedir
#define fcntl model_fcntl
#define mmap model_mmap
#define munmap model_munmap
#define clock_gettime model_clock_gettime
#define getrandom model_getrandom
#define getuid model_getuid
#define geteuid model_geteuid
#define getgid model_getgid
#define getegid model_getegid
#define getpid model_getpid
#define getppid model_getppid
#define syscall model_syscall
#define getgroups model_getgroups
#define mkdirat model_mkdirat
#define symlinkat model_symlinkat
#define fsync model_fsync
