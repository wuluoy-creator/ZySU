#include "protocol.hpp"
#include "utf8.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <mbedtls/sha256.h>
#include <set>
#include <stdexcept>

namespace sumh_observer {
namespace {
void exact_keys(const json::Value &value, const std::set<std::string> &keys) {
  if (!value.is_object() || value.o.size() != keys.size())
    throw std::invalid_argument("invalid object keys");
  for (const auto &kv : value.o)
    if (!keys.count(kv.first))
      throw std::invalid_argument("unknown object key");
}
void literal_version_token(const std::string &bytes) {
  // The validated request schema has exactly one numeric value. Scan outside
  // JSON strings after parsing (including escaped key spellings) so the shared
  // double-backed codec cannot erase the distinction between 1, 1.0 and 1e0.
  bool in_string = false, escaped = false;
  unsigned numbers = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const char c = bytes[i];
    if (in_string) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        in_string = false;
      continue;
    }
    if (c == '"') {
      in_string = true;
      continue;
    }
    if (c != '-' && (c < '0' || c > '9'))
      continue;
    const auto end = bytes.find_first_of(" \t\n\r,]}", i);
    const auto length = (end == std::string::npos ? bytes.size() : end) - i;
    if (length != 1 || c != '1' || ++numbers != 1)
      throw std::invalid_argument("schema_version must be the integer token 1");
    i += length - 1;
  }
  if (numbers != 1)
    throw std::invalid_argument("schema_version must be the integer token 1");
}
bool decimal(const std::string &s) {
  return !s.empty() && (s.size() == 1 || s[0] != '0') &&
         std::all_of(s.begin(), s.end(),
                     [](char c) { return c >= '0' && c <= '9'; });
}
bool hex_number(const std::string &s, std::uint64_t &out) {
  if (s.empty() || s.size() > 16)
    return false;
  out = 0;
  for (const char c : s) {
    const unsigned v = c >= '0' && c <= '9'   ? c - '0'
                       : c >= 'a' && c <= 'f' ? c - 'a' + 10
                       : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                              : 16;
    if (v == 16)
      return false;
    out = (out << 4) | v;
  }
  return true;
}
std::vector<std::string> words(const std::string &s) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (at < s.size()) {
    at = s.find_first_not_of(" \t\r\n", at);
    if (at == std::string::npos)
      break;
    auto end = s.find_first_of(" \t\r\n", at);
    if (end == std::string::npos)
      end = s.size();
    out.push_back(s.substr(at, end - at));
    at = end;
  }
  return out;
}
bool unescape_mount(const std::string &text, std::string &out) {
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '\\') {
      out += text[i];
      continue;
    }
    if (i + 3 >= text.size())
      return false;
    const auto code = text.substr(i + 1, 3);
    if (code == "040")
      out += ' ';
    else if (code == "011")
      out += '\t';
    else if (code == "012")
      out += '\n';
    else if (code == "134")
      out += '\\';
    else
      return false;
    i += 3;
  }
  return true;
}
} // namespace
bool valid_run_id(const std::string &id) {
  return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
bool valid_files_dir(const std::string &root) {
  for (const char *prefix : {"/data/user/", "/data/user_de/"}) {
    const std::string p(prefix),
        suffix = std::string("/") + kPackage + "/files";
    if (root.compare(0, p.size(), p) || root.size() <= p.size() + suffix.size())
      continue;
    if (root.compare(root.size() - suffix.size(), suffix.size(), suffix))
      continue;
    const auto user =
        root.substr(p.size(), root.size() - p.size() - suffix.size());
    if (user.size() <= 10 && decimal(user))
      return true;
  }
  return false;
}
bool valid_fixture_root(const std::string &root, const std::string &id) {
  if (!valid_run_id(id) || root.size() > 1024 ||
      root.find('\0') != std::string::npos)
    return false;
  if (root == "/data/local/tmp/sumh-observer/" + id)
    return true;
  const auto suffix = "/sumh-observer/" + id;
  return root.size() > suffix.size() &&
         root.compare(root.size() - suffix.size(), suffix.size(), suffix) ==
             0 &&
         valid_files_dir(root.substr(0, root.size() - suffix.size()));
}
Request parse_request(const std::string &bytes) {
  if (bytes.empty() || bytes.size() > kRequestLimit ||
      !ksud::text::valid_utf8(bytes))
    throw std::invalid_argument("request must be bounded UTF-8 JSON");
  json::Value root;
  std::string error;
  if (!json::Parser(bytes, true).parse(root, error))
    throw std::invalid_argument("invalid request JSON: " + error);
  exact_keys(root, {"schema_version", "resource", "run_id", "fixture_root",
                    "context"});
  if (!root.at("schema_version").is_number() ||
      root.at("schema_version").n != 1 || !root.at("resource").is_string() ||
      root.at("resource").s != "sumh_observer_request" ||
      !root.at("run_id").is_string() || !root.at("fixture_root").is_string())
    throw std::invalid_argument("invalid request type or version");
  Request request{root.at("run_id").s, root.at("fixture_root").s,
                  root.at("context")};
  if (!valid_fixture_root(request.fixture_root, request.run_id))
    throw std::invalid_argument(
        "fixture root is outside the fixed synthetic scope");
  exact_keys(request.context, {"boot_id", "config_revision"});
  for (const auto &kv : request.context.o)
    if (!kv.second.is_null() &&
        (!kv.second.is_string() || kv.second.s.size() > 256 ||
         kv.second.s.find('\0') != std::string::npos ||
         !ksud::text::valid_utf8(kv.second.s)))
      throw std::invalid_argument("invalid supplied context");
  const auto &boot = request.context.at("boot_id");
  if (boot.is_string()) {
    if (boot.s.size() != 36)
      throw std::invalid_argument("invalid supplied boot UUID");
    for (std::size_t i = 0; i != 36; ++i) {
      const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
      if (dash ? boot.s[i] != '-'
               : !((boot.s[i] >= '0' && boot.s[i] <= '9') ||
                   (boot.s[i] >= 'a' && boot.s[i] <= 'f')))
        throw std::invalid_argument("invalid supplied boot UUID");
    }
  }
  const auto &revision = request.context.at("config_revision");
  if (revision.is_string()) {
    if (revision.s.size() != 74 ||
        revision.s.compare(0, 10, "sha256-v1:") != 0 ||
        !std::all_of(revision.s.begin() + 10, revision.s.end(), [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
      throw std::invalid_argument("invalid supplied revision");
  }
  literal_version_token(bytes);
  return request;
}
void validate_request(const std::string &bytes) { (void)parse_request(bytes); }
std::string fixture_marker(const std::string &id) {
  return "sumh-observer-fixture-v1\n" + id + "\n";
}
std::string map_content() {
  std::string bytes;
  bytes.reserve(4096);
  for (std::size_t i = 0; i != 4096; ++i)
    bytes += "0123456789abcdef"[i % 16];
  return bytes;
}
std::string hex_bytes(const std::string &bytes) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out += digits[c >> 4];
    out += digits[c & 15];
  }
  return out;
}
std::string sha256(const std::string &bytes) {
  std::array<unsigned char, 32> out{};
  if (mbedtls_sha256(reinterpret_cast<const unsigned char *>(bytes.data()),
                     bytes.size(), out.data(), 0))
    throw std::runtime_error("SHA256 failed");
  return hex_bytes(
      std::string(reinterpret_cast<const char *>(out.data()), out.size()));
}
bool maps_overlap(const std::string &line, std::uint64_t begin,
                  std::uint64_t end) {
  auto space = line.find(' '), dash = line.find('-');
  if (space == std::string::npos || dash == std::string::npos || dash > space ||
      begin >= end)
    return false;
  std::uint64_t a, b;
  return hex_number(line.substr(0, dash), a) &&
         hex_number(line.substr(dash + 1, space - dash - 1), b) && a < b &&
         a < end && b > begin;
}
bool mount_relevant(const std::string &line, const std::string &fixture,
                    bool mountinfo) {
  const auto fields = words(line);
  if (fields.size() < (mountinfo ? 10u : 6u))
    return false;
  if (mountinfo &&
      std::find(fields.begin() + 6, fields.end(), "-") == fields.end())
    return false;
  std::string point;
  if (!unescape_mount(fields[mountinfo ? 4 : 1], point) || point.empty() ||
      point[0] != '/')
    return false;
  return point == "/" || point == fixture ||
         (fixture.size() > point.size() &&
          fixture.compare(0, point.size(), point) == 0 &&
          fixture[point.size()] == '/');
}
Bytes read_bounded(const Reader &read, const Expired &expired,
                   std::size_t limit) {
  Bytes result;
  std::array<char, 1024> block{};
  while (result.bytes.size() <= limit) {
    if (expired()) {
      result.error = ETIMEDOUT;
      return result;
    }
    const auto wanted = std::min(block.size(), limit + 1 - result.bytes.size());
    int err = 0;
    auto count = read(block.data(), wanted, err);
    if (count < 0) {
      if (err == EINTR)
        continue;
      result.error = err > 0 ? err : EIO;
      return result;
    }
    if (static_cast<std::size_t>(count) > wanted) {
      result.error = EIO;
      return result;
    }
    if (count == 0) {
      result.complete = true;
      return result;
    }
    result.bytes.append(block.data(), static_cast<std::size_t>(count));
  }
  result.bytes.resize(limit);
  return result;
}
Scan scan_lines(const Reader &read, const Expired &expired,
                const std::function<bool(const std::string &)> &keep) {
  Scan result;
  std::string pending;
  std::array<char, 1024> block{};
  while (result.scanned < kScanLimit) {
    if (expired()) {
      result.error = ETIMEDOUT;
      return result;
    }
    int err = 0;
    const auto wanted = std::min(block.size(), kScanLimit - result.scanned);
    auto count = read(block.data(), wanted, err);
    if (count < 0) {
      if (err == EINTR)
        continue;
      result.error = err > 0 ? err : EIO;
      return result;
    }
    if (static_cast<std::size_t>(count) > wanted) {
      result.error = EIO;
      return result;
    }
    if (!count) {
      if (!pending.empty() && keep(pending))
        result.lines.push_back(pending);
      result.complete = true;
      return result;
    }
    result.scanned += static_cast<std::size_t>(count);
    for (std::ptrdiff_t i = 0; i < count; ++i) {
      pending += block[static_cast<std::size_t>(i)];
      if (block[static_cast<std::size_t>(i)] == '\n') {
        if (keep(pending))
          result.lines.push_back(pending);
        pending.clear();
      }
    }
  }
  // Exactly reaching the cap is conservatively incomplete: do not read a
  // 65537th byte just to prove EOF, and never publish a partial final line.
  return result;
}
} // namespace sumh_observer
