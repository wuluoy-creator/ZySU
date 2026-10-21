#include "collector.hpp"
#include "protocol.hpp"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <string>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 3 || std::strcmp(argv[1], "--request")) {
    std::fputs("usage: sumh-observer --request <file|->\n", stderr);
    return 2;
  }
  const bool standard = std::strcmp(argv[2], "-") == 0;
  const int fd = standard ? STDIN_FILENO : open(argv[2], O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    std::fprintf(stderr, "request open failed: %s\n", std::strerror(errno));
    return 2;
  }
  struct Close {
    int fd;
    bool owned;
    ~Close() {
      if (owned)
        close(fd);
    }
  } guard{fd, !standard};
  try {
    const auto input = sumh_observer::read_bounded(
        [fd](char *bytes, std::size_t size, int &err) {
          const auto n = read(fd, bytes, size);
          if (n < 0)
            err = errno;
          return n;
        },
        [] { return false; }, sumh_observer::kRequestLimit);
    if (input.error || !input.complete) {
      std::fprintf(stderr, "request read failed or exceeds %zu bytes\n",
                   sumh_observer::kRequestLimit);
      return 2;
    }
    const auto output = sumh_observer::collect(input.bytes, "root");
    std::size_t offset = 0;
    while (offset < output.size()) {
      const auto n =
          write(STDOUT_FILENO, output.data() + offset, output.size() - offset);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        std::fputs("result write failed\n", stderr);
        return 1;
      }
      if (!n) {
        std::fputs("result write made no progress\n", stderr);
        return 1;
      }
      offset += static_cast<std::size_t>(n);
    }
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "collection rejected: %s\n", e.what());
    return 1;
  }
}
