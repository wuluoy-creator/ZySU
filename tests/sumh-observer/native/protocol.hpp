#pragma once
#include "json.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sumh_observer {
constexpr std::size_t kRequestLimit = 32768;
constexpr std::size_t kResultLimit = 262144;
constexpr std::size_t kScanLimit = 65536;
constexpr std::size_t kReadLimit = 4096;
constexpr std::size_t kDirentLimit = 256;
constexpr const char *kPackage = "com.zying.zysu.sumhobservertest";
struct Request {
  std::string run_id, fixture_root;
  json::Value context;
};
Request parse_request(const std::string &bytes);
bool valid_run_id(const std::string &id);
bool valid_fixture_root(const std::string &root, const std::string &id);
bool valid_files_dir(const std::string &root);
std::string fixture_marker(const std::string &id);
std::string map_content();
std::string hex_bytes(const std::string &bytes);
std::string sha256(const std::string &bytes);
bool maps_overlap(const std::string &line, std::uint64_t begin,
                  std::uint64_t end);
bool mount_relevant(const std::string &line, const std::string &fixture,
                    bool mountinfo);
// Reader returns byte count, zero EOF, or -1 with err. Real I/O and host
// doubles share this bounded parser. Deadline is checked before each blocking
// call.
using Reader = std::function<std::ptrdiff_t(char *, std::size_t, int &)>;
using Expired = std::function<bool()>;
struct Scan {
  std::vector<std::string> lines;
  std::size_t scanned = 0;
  bool complete = false;
  int error = 0;
};
Scan scan_lines(const Reader &read, const Expired &expired,
                const std::function<bool(const std::string &)> &keep);
struct Bytes {
  std::string bytes;
  bool complete = false;
  int error = 0;
};
Bytes read_bounded(const Reader &read, const Expired &expired,
                   std::size_t limit);
} // namespace sumh_observer
