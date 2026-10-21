#include "collector.hpp"
#include "protocol.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using namespace sumh_observer;
static unsigned checks;
#define CHECK(x)                                                               \
  do {                                                                         \
    ++checks;                                                                  \
    if (!(x))                                                                  \
      throw std::runtime_error("check failed at line " +                       \
                               std::to_string(__LINE__) + ": " #x);            \
  } while (false)
static const std::string id(32, 'a');
static json::Value request_value() {
  auto v = json::Value::object();
  v["schema_version"] = 1;
  v["resource"] = "sumh_observer_request";
  v["run_id"] = id;
  v["fixture_root"] =
      "/data/user/0/com.zying.zysu.sumhobservertest/files/sumh-observer/" + id;
  v["context"] = json::Value::object();
  v["context"]["boot_id"] = json::Value();
  v["context"]["config_revision"] = json::Value();
  return v;
}
static bool rejects(const std::string &s) {
  try {
    validate_request(s);
    return false;
  } catch (const std::invalid_argument &) {
    return true;
  }
}
static Reader source(const std::string &bytes, std::size_t step = 7,
                     int fail = 0) {
  return [bytes, step, at = std::size_t(0), fail,
          called = false](char *target, std::size_t size,
                          int &error) mutable -> std::ptrdiff_t {
    if (fail && !called) {
      called = true;
      error = fail;
      return -1;
    }
    const auto n = std::min({size, step, bytes.size() - at});
    std::memcpy(target, bytes.data() + at, n);
    at += n;
    return static_cast<std::ptrdiff_t>(n);
  };
}
int main() {
  try {
    auto v = request_value();
    const auto raw = json::dump(v);
    CHECK(parse_request(raw).run_id == id);
    for (const std::string version :
         {"1.0", "1e0", "1E+0", "1.00000000000000000001"}) {
      auto changed = raw;
      const auto pos = changed.find("\"schema_version\":1");
      CHECK(pos != std::string::npos);
      changed.replace(pos, 18, "\"schema_version\":" + version);
      CHECK(rejects(changed));
    }
    auto escaped_version = raw;
    escaped_version.replace(escaped_version.find("schema_version"), 14,
                            "\\u0073chema_version");
    CHECK(!rejects(escaped_version));
    auto exponent = escaped_version;
    exponent.replace(exponent.find("version\":1") + 9, 1, "1e0");
    CHECK(rejects(exponent));
    CHECK(sha256("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256(std::string(1000000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK(sha256(raw) != sha256(raw + "\n"));
    CHECK(hex_bytes(std::string("\0\xff", 2)) == "00ff");
    CHECK(fixture_marker(id) == "sumh-observer-fixture-v1\n" + id + "\n");
    CHECK(map_content().size() == 4096 &&
          map_content().substr(4080) == "0123456789abcdef");
    for (const std::string root :
         {"/data/user/0", "/data/user/10", "/data/user_de/2"})
      CHECK(valid_fixture_root(
          root + "/com.zying.zysu.sumhobservertest/files/sumh-observer/" + id,
          id));
    CHECK(valid_fixture_root("/data/local/tmp/sumh-observer/" + id, id));
    for (const std::string root :
         {"/data/user/00", "/data/user/-1", "/data/user/+1", "/data/user/../0",
          "/data/user/ 0", "/data/user/0/", "/data/user/99999999999"})
      CHECK(!valid_fixture_root(
          root + "/com.zying.zysu.sumhobservertest/files/sumh-observer/" + id,
          id));
    for (const std::string &suffix :
         std::vector<std::string>{"/", "/../other", "//", std::string("\0", 1)})
      CHECK(!valid_fixture_root(v.at("fixture_root").s + suffix, id));
    CHECK(!valid_fixture_root("/data/local/tmp/sumh-observer/" +
                                  std::string(32, 'A'),
                              std::string(32, 'A')));
    CHECK(!valid_fixture_root("/data/local/tmp/sumh-observer/" + id,
                              std::string(32, 'b')));
    CHECK(rejects(""));
    CHECK(rejects("[]"));
    CHECK(rejects(raw + "x"));
    CHECK(!rejects(raw + std::string(kRequestLimit - raw.size(), ' ')));
    CHECK(rejects(raw + std::string(kRequestLimit + 1 - raw.size(), ' ')));
    CHECK(rejects(std::string("\xff", 1) + raw));
    CHECK(rejects("{\"schema_version\":1,\"schema_version\":1}"));
    CHECK(rejects("{\"schema_version\":1,\"\\u0073chema_version\":1}"));
    for (const std::string key :
         {"schema_version", "resource", "run_id", "fixture_root", "context"}) {
      auto bad = v;
      bad.o.erase(key);
      CHECK(rejects(json::dump(bad)));
      bad = v;
      bad[key] = true;
      CHECK(rejects(json::dump(bad)));
    }
    auto bad = v;
    bad["unknown"] = 1;
    CHECK(rejects(json::dump(bad)));
    bad = v;
    bad["schema_version"] = 2;
    CHECK(rejects(json::dump(bad)));
    bad = v;
    bad["context"]["extra"] = "x";
    CHECK(rejects(json::dump(bad)));
    bad = v;
    bad["context"]["boot_id"] = "11111111-2222-3333-4444-555555555555";
    bad["context"]["config_revision"] = "sha256-v1:" + std::string(64, 'b');
    CHECK(!rejects(json::dump(bad)));
    for (const std::string &text : std::vector<std::string>{
             "", "ABC", "11111111-2222-3333-4444-55555555555A",
             std::string(257, 'a')}) {
      bad = v;
      bad["context"]["boot_id"] = text;
      CHECK(rejects(json::dump(bad)));
    }
    bad = v;
    bad["context"]["config_revision"] = "sha256-v1:" + std::string(63, 'a');
    CHECK(rejects(json::dump(bad)));
    bad = v;
    bad["context"]["config_revision"] = "sha256-v1:" + std::string(64, 'A');
    CHECK(rejects(json::dump(bad)));
    CHECK(rejects(raw.substr(0, raw.size() - 1) + ",\"junk\":1e999}"));
    CHECK(rejects(raw.substr(0, raw.size() - 1) + ",\"junk\":\"\\ud800\"}"));
    CHECK(maps_overlap("1000-2000 r--p 00000000 00:00 1 /fixture\n", 0x1000,
                       0x2000));
    CHECK(maps_overlap("1000-1800 r--p 0 0:0 1 x", 0x1400, 0x2000));
    CHECK(!maps_overlap("1000-2000 r--p", 0x2000, 0x3000));
    CHECK(!maps_overlap("10000000000000000-2000 r--p", 0x1000, 0x2000));
    CHECK(!maps_overlap("2000-1000 r--p", 0x1000, 0x2000));
    CHECK(!maps_overlap("bogus", 0x1000, 0x2000));
    CHECK(mount_relevant("1 0 0:1 / / rw - tmpfs none rw\n",
                         v.at("fixture_root").s, true));
    CHECK(mount_relevant("1 0 0:1 / /data rw - ext4 /dev/x rw\n",
                         v.at("fixture_root").s, true));
    CHECK(!mount_relevant("1 0 0:1 / /data/private rw - ext4 /dev/x rw\n",
                          v.at("fixture_root").s, true));
    CHECK(!mount_relevant("1 0 0:1 / /dat rw - ext4 /dev/x rw\n",
                          v.at("fixture_root").s, true));
    CHECK(mount_relevant("/dev/x /data ext4 rw 0 0\n", v.at("fixture_root").s,
                         false));
    CHECK(mount_relevant("/dev/x /path\\040space ext4 rw 0 0\n",
                         "/path space/child", false));
    CHECK(!mount_relevant("/dev/x /path\\999bad ext4 rw 0 0\n",
                          "/pathbad/child", false));
    const auto alive = [] { return false; };
    for (std::size_t step : {1u, 3u, 1024u}) {
      auto b = read_bounded(source("abcdef", step), alive, 6);
      CHECK(b.complete && b.bytes == "abcdef" && !b.error);
      b = read_bounded(source("abcdefg", step), alive, 6);
      CHECK(!b.complete && b.bytes == "abcdef" && !b.error);
      auto s = scan_lines(source("keep\nskip\nkeep2", step), alive,
                          [](const std::string &line) {
                            return line.compare(0, 4, "keep") == 0;
                          });
      CHECK(s.complete && s.scanned == 15 && s.lines.size() == 2 &&
            s.lines[0] == "keep\n" && s.lines[1] == "keep2");
    }
    auto b = read_bounded(source("abc", 1, EINTR), alive, 3);
    CHECK(b.complete && b.bytes == "abc");
    b = read_bounded(source("abc", 1, EACCES), alive, 3);
    CHECK(!b.complete && b.error == EACCES);
    b = read_bounded(source("abc"), [] { return true; }, 3);
    CHECK(!b.complete && b.bytes.empty() && b.error == ETIMEDOUT);
    b = read_bounded(source(""), alive, 0);
    CHECK(b.complete && b.bytes.empty());
    auto s = scan_lines(source(std::string(kScanLimit, 'a'), 1024), alive,
                        [](const std::string &) { return true; });
    CHECK(!s.complete && s.scanned == kScanLimit && s.lines.empty());
    s = scan_lines(source("a\nb\n", 1, EINTR), alive,
                   [](const std::string &) { return true; });
    CHECK(s.complete && s.lines.size() == 2);
    s = scan_lines(source("a", 1, EIO), alive,
                   [](const std::string &) { return true; });
    CHECK(!s.complete && s.error == EIO && s.lines.empty());
    s = scan_lines(
        source("a"), [] { return true; },
        [](const std::string &) { return true; });
    CHECK(!s.complete && s.error == ETIMEDOUT && !s.scanned);
    const Reader overread = [](char *, std::size_t n, int &) {
      return static_cast<std::ptrdiff_t>(n + 1);
    };
    CHECK(read_bounded(overread, alive, 4).error == EIO);
    CHECK(scan_lines(overread, alive, [](const std::string &) {
            return true;
          }).error == EIO);
    std::printf("PASS observer native protocol/stream %u checks\n", checks);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
