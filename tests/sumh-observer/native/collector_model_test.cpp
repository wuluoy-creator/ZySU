// Included after the actual production collector by run_native_tests.py.
using namespace sumh_observer;
using namespace observer_model;
static unsigned checks;
#define CHECK(x)                                                               \
  do {                                                                         \
    ++checks;                                                                  \
    if (!(x))                                                                  \
      throw std::runtime_error("check failed at line " +                       \
                               std::to_string(__LINE__) + ": " #x);            \
  } while (false)
static std::string request() {
  auto v = json::Value::object();
  v["schema_version"] = 1;
  v["resource"] = "sumh_observer_request";
  v["run_id"] = runid;
  v["fixture_root"] = root;
  v["context"] = json::Value::object();
  v["context"]["boot_id"] = json::Value();
  v["context"]["config_revision"] = json::Value();
  return json::dump(v);
}
static json::Value sample(const std::string &role = "ordinary", int fd = -1) {
  const auto text = collect(request(), role, fd);
  json::Value v;
  std::string error;
  CHECK(json::Parser(text, true).parse(v, error));
  CHECK(text.size() <= kResultLimit);
  CHECK(v.at("request_sha256").s == sha256(request()));
  CHECK(v.at("fixture_manifest_sha256").s == sha256(fixture_marker(runid)));
  CHECK(v.at("observer_instance_id").s.size() == 32);
  std::set<std::string> ids;
  for (const auto &e : v.at("observations").a) {
    CHECK(ids.insert(e.at("id").s).second);
    CHECK(e.o.size() == 6);
    if (e.at("status").s == "ok")
      CHECK(e.at("complete").b && e.at("errno").is_null());
    if (e.at("status").s == "not_run")
      CHECK(!e.at("complete").b && e.at("errno").is_null() &&
            e.at("value").is_null());
  }
  CHECK(v.at("observations").a.size() ==
        ((role == "isolated" || fd >= 0) ? 26u : 22u));
  return v;
}
static const json::Value &obs(const json::Value &v, const std::string &id) {
  for (const auto &e : v.at("observations").a)
    if (e.at("id").s == id)
      return e;
  throw std::runtime_error("observation missing: " + id);
}
static bool read_any_fixture_content() {
  return std::any_of(read_paths.begin(), read_paths.end(),
                     [](const std::string &path) {
                       return path.compare(0, root.size(), root) == 0 &&
                              path != root + "/fixture.txt";
                     });
}
static void clean() {
  CHECK(fds.empty());
  CHECK(!maps);
  CHECK(!close_errors);
}
int main(int argc, char **argv) {
  try {
    reset();
    auto v = sample();
    CHECK(obs(v, "file.present.read").at("value").at("hex").s ==
          hex_bytes("SUMH observer present v1\n"));
    CHECK(obs(v, "file.missing.stat").at("status").s == "error" &&
          obs(v, "file.missing.stat").at("errno").n == ENOENT);
    CHECK(obs(v, "map.path.maps").at("value").at("matched_mapping").b);
    CHECK(obs(v, "identity.start_ticks").at("value").s == "1234567890123");
    CHECK(obs(v, "mount.info").at("value").at("lines_hex").a.size() == 2);
    CHECK(obs(v, "namespace.start.stat").at("value").at("ino").s == "9000");
    CHECK(map_calls == 1);
    const int normal_calls = calls;
    clean();
    reset();
    nodes[root + "/present.txt"].ino = UINT64_MAX;
    v = sample();
    CHECK(obs(v, "file.present.open_stat").at("value").at("ino").s ==
          "18446744073709551615");
    clean();
    model_stat timestamp{};
    timestamp.st_mtim.tv_sec = -1;
    timestamp.st_mtim.tv_nsec = 1;
    CHECK(stat_value(timestamp).at("mtime_ns").s == "-999999999");
    timestamp.st_mtim.tv_sec = 0;
    timestamp.st_mtim.tv_nsec = 1;
    CHECK(stat_value(timestamp).at("mtime_ns").s == "1");
    timestamp.st_mtim.tv_sec = -2;
    timestamp.st_mtim.tv_nsec = 0;
    CHECK(stat_value(timestamp).at("mtime_ns").s == "-2000000000");
    timestamp.st_size = -1;
    bool malformed_stat = false;
    try {
      (void)stat_value(timestamp);
    } catch (const SysError &e) {
      malformed_stat = e.code == EIO;
    }
    CHECK(malformed_stat);
    reset();
    int fd = model_open((root + "/map.bin").c_str(), O_RDONLY);
    fds[fd].offset = 77;
    v = sample("isolated", fd);
    CHECK(fds.count(fd) && fds[fd].offset == 77 && fds.size() == 1);
    CHECK(obs(v, "map.fd.read").at("source").s == "delegated_fd");
    CHECK(obs(v, "map.fd.maps").at("source").s == "self");
    CHECK(map_calls == 2);
    model_close(fd);
    clean();
    reset();
    v = sample("isolated");
    CHECK(obs(v, "map.fd.stat").at("status").s == "not_run");
    clean();
    reset();
    bool rejected = false;
    try {
      collect(request(), "root");
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    CHECK(rejected && !calls);
    uid = euid = 0;
    v = sample("root");
    CHECK(v.at("identity").at("uid").s == "0");
    clean();
    reset();
    nodes.erase(root + "/fixture.txt");
    v = sample();
    CHECK(!read_any_fixture_content());
    CHECK(obs(v, "fixture.marker").at("errno").n == ENOENT);
    CHECK(obs(v, "file.present.read").at("status").s == "not_run");
    clean();
    reset();
    nodes[root + "/fixture.txt"].content = "wrong";
    v = sample();
    CHECK(!read_any_fixture_content());
    clean();
    reset();
    nodes[files + "/sumh-observer"].mode = S_IFLNK;
    v = sample();
    CHECK(obs(v, "fixture.marker").at("errno").n == ELOOP);
    CHECK(!read_any_fixture_content());
    clean();
    reset();
    nodes[files].mode = S_IFLNK;
    v = sample();
    CHECK(obs(v, "fixture.marker").at("errno").n == ELOOP);
    CHECK(!read_any_fixture_content());
    clean();
    reset();
    add("/data/local", S_IFLNK, "/private");
    auto local_request = parse_request(request());
    const auto local = std::string("/data/local/tmp/sumh-observer/") + runid;
    auto local_bytes = request();
    local_bytes.replace(local_bytes.find(root), root.size(), local);
    auto local_result = collect(local_bytes, "ordinary");
    json::Value local_sample;
    std::string local_error;
    CHECK(json::Parser(local_result, true).parse(local_sample, local_error));
    CHECK(obs(local_sample, "fixture.marker").at("errno").n == ELOOP);
    clean();
    reset();
    add(root + "/unexpected-private", S_IFREG, "do not collect");
    v = sample();
    CHECK(!read_any_fixture_content());
    CHECK(obs(v, "directory.entries").at("errno").n == EINVAL);
    clean();
    reset();
    nodes[root + "/present.txt"].mode = S_IFLNK;
    v = sample();
    CHECK(!read_any_fixture_content());
    clean();
    reset();
    nodes[root + "/present.txt"].links = 2;
    v = sample();
    CHECK(!read_any_fixture_content());
    clean();
    reset();
    nodes[root + "/present.txt"].content.assign(4097, 'x');
    v = sample();
    CHECK(obs(v, "file.present.read").at("status").s == "truncated");
    CHECK(obs(v, "file.present.read").at("value").at("hex").s.size() == 8192);
    clean();
    reset();
    nodes[root + "/map.bin"].content[0] = 'z';
    v = sample();
    CHECK(obs(v, "map.path.mapping").at("errno").n == EINVAL);
    CHECK(!map_calls);
    clean();
    reset();
    fd = model_open((root + "/map.bin").c_str(), O_RDWR);
    v = sample("isolated", fd);
    CHECK(obs(v, "map.fd.stat").at("errno").n == EINVAL);
    CHECK(map_calls == 1);
    model_close(fd);
    clean();
    reset();
    fdopendir_failure = true;
    v = sample();
    clean();
    CHECK(!read_any_fixture_content());
    reset();
    timeout = true;
    v = sample();
    clean();
    CHECK(!read_any_fixture_content());
    CHECK(obs(v, "file.present.read").at("status").s == "not_run");
    for (int failure_index = 1; failure_index <= normal_calls;
         ++failure_index) {
      reset();
      fail_at = failure_index;
      try {
        (void)sample();
      } catch (const SysError &) {
      } // Top-level identity/random failure has no sample.
      clean();
    }
    reset();
    fd = model_open((root + "/map.bin").c_str(), O_RDONLY);
    (void)sample("isolated", fd);
    const int delegated_calls = calls;
    model_close(fd);
    clean();
    for (int failure_index = 2; failure_index <= delegated_calls;
         ++failure_index) {
      reset();
      fd = model_open((root + "/map.bin").c_str(), O_RDONLY);
      fail_at = failure_index;
      try {
        (void)sample("isolated", fd);
      } catch (const SysError &) {
      }
      CHECK(fds.count(fd) && fds.size() == 1);
      model_close(fd);
      clean();
    }
    reset();
    rejected = false;
    try {
      create_fixture(files, runid);
    } catch (const SysError &e) {
      rejected = e.code == EEXIST;
    }
    CHECK(rejected);
    clean();
    CHECK(nodes[root + "/present.txt"].content == "SUMH observer present v1\n");
    const auto new_id = std::string(32, 'b'),
               new_root = files + "/sumh-observer/" + new_id;
    reset();
    const auto created = create_fixture(files, new_id);
    CHECK(parse_request(created).run_id == new_id);
    CHECK(nodes[new_root + "/fixture.txt"].content == fixture_marker(new_id));
    CHECK(!nodes.count(new_root + "/missing.txt"));
    const int creation_calls = calls;
    clean();
    for (int failure_index = 1; failure_index <= creation_calls;
         ++failure_index) {
      reset();
      fail_at = failure_index;
      try {
        (void)create_fixture(files, new_id);
      } catch (const SysError &) {
      }
      clean();
      CHECK(nodes[root + "/present.txt"].content ==
            "SUMH observer present v1\n");
    }
    if (argc == 3 && std::string(argv[1]) == "--export") {
      const std::string destination = argv[2];
      auto save = [&](const std::string &filename, const std::string &bytes) {
        std::ofstream output(destination + "/" + filename, std::ios::binary);
        output << bytes;
        if (!output)
          throw std::runtime_error("model export failed");
      };
      save("request.json", request());
      for (const std::string role : {"ordinary", "isolated", "root"}) {
        reset();
        if (role == "root") {
          uid = euid = 0;
          actor_pid = 321;
        }
        if (role == "isolated") {
          uid = euid = 99001;
          actor_pid = 456;
        }
        const int delegated =
            role == "isolated"
                ? model_open((root + "/map.bin").c_str(), O_RDONLY)
                : -1;
        save(role + ".json", collect(request(), role, delegated));
        if (delegated >= 0)
          model_close(delegated);
        clean();
      }
      save("PROVENANCE.txt", "HOST SYSCALL MODEL OUTPUT. Synthetic evidence, "
                             "not an Android device run.\n");
    } else if (argc != 1)
      throw std::runtime_error("usage: collector-model [--export directory]");
    std::printf("PASS observer complete collector syscall model %u checks, %d "
                "ordinary + %d delegated + %d creation fault positions\n",
                checks, normal_calls, delegated_calls - 1, creation_calls);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
