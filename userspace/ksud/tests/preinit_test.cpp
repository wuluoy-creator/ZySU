#include "module/preinit.hpp"
#include "module/preinit_platform.hpp"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <map>
#include <stdexcept>

namespace fs = std::filesystem;
namespace preinit = ksud::preinit;
namespace {
uint64_t available_bytes = 32ULL << 20;
unsigned syncs = 0;
bool fail_file_sync = false;
std::map<fs::path, unsigned> modes;
void put(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << content;
    assert(stream.good());
}
std::string get(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
}  // namespace

namespace ksud::preinit::platform {
void secure(const fs::path& path, unsigned mode, const char*) {
    modes[path] = mode & 0755;
}
unsigned mode(const fs::path& path) {
    const auto it = modes.find(path);
    return it == modes.end() ? (fs::is_directory(path) ? 0755 : 0644) : it->second;
}
void sync_path(const fs::path& path) {
    if (fail_file_sync && path.filename() == "preinit.conf")
        throw std::runtime_error("injected fsync failure");
    ++syncs;
}
void replace_file(const fs::path& from, const fs::path& to) {
    // Windows rename cannot replace a destination; production uses POSIX rename.
    fs::remove(to);
    fs::rename(from, to);
}
uint64_t available(const fs::path&) {
    return available_bytes;
}
uint64_t allocated(const fs::path&) {
    return 4096;
}
}  // namespace ksud::preinit::platform

int main(int argc, char** argv) {
    assert(argc == 2);
    const fs::path base = argv[1];
    const auto metadata = base / "metadata";
    const auto stale = base / "stale";
    const auto status = base / "working/status";
    const auto module = base / "modules/example";
    const auto conf = module / "preinit/preinit.conf";
    const auto staged = metadata / "preinit/example/preinit.conf";
    const auto counter = metadata / ".preinit_bootcount";
    preinit::Modules modules{{"example", module}};
    const auto run = [&](bool supported = true) {
        return preinit::sync(metadata, {stale}, modules, "metadata", supported, status);
    };
    const auto state = [&] { return preinit::module_states(metadata, status).at("example").state; };

    put(conf, "write /proc/example 1\n");
    put(stale / "preinit/obsolete/preinit.conf", "old");
    assert(run() == 0 && state() == "ok");
    assert(get(staged) == get(conf) && !fs::exists(stale / "preinit"));

    // Successful boots clear in-progress attempts; a parked counter survives refresh
    // and successful boot until content really changes or the user resets it.
    put(counter, "2");
    preinit::on_boot_completed(metadata);
    assert(!fs::exists(counter));
    put(counter, "4");
    const auto before = fs::last_write_time(staged);
    syncs = 0;
    assert(run() == 0 && state() == "bootloop");
    assert(fs::last_write_time(staged) == before && syncs == 1);
    preinit::on_boot_completed(metadata);
    assert(get(counter) == "4");
    assert(preinit::reset_bootcount(metadata) == 0 && state() == "ok");
    put(counter, "4");
    put(conf, "write /proc/example 2\n");
    assert(run() == 0 && state() == "ok" && !fs::exists(counter));

    // Failed replacement must not leave either stale active actions or temporary copies.
    put(conf, "write /proc/example 3\n");
    fail_file_sync = true;
    assert(run() == 0 && state() == "error");
    fail_file_sync = false;
    assert(!fs::exists(metadata / "preinit/example"));
    assert(!fs::exists(metadata / "preinit/.example.tmp"));
    assert(run() == 0 && state() == "ok");

    available_bytes = preinit::RESERVE - 1;
    assert(run() == 0 && state() == "no_space" && !fs::exists(staged));
    available_bytes = 32ULL << 20;
    assert(run() == 0 && state() == "ok");
    put(module / "preinit/too-large", std::string(preinit::LIMIT_TOTAL + 1, 'x'));
    assert(run() == 0 && state() == "no_space" && !fs::exists(staged));
    fs::remove(module / "preinit/too-large");

    // Empty files and their long manifest entries must count towards the
    // reserve, even though their payload itself occupies no blocks.
    for (unsigned i = 0; i < 500; ++i)
        put(module / "preinit" / (std::string(80, 'a') + std::to_string(i)), "");
    available_bytes = preinit::RESERVE + 16384;
    assert(run() == 0 && state() == "no_space" && !fs::exists(staged));
    for (unsigned i = 0; i < 500; ++i)
        fs::remove(module / "preinit" / (std::string(80, 'a') + std::to_string(i)));
    available_bytes = 32ULL << 20;

    assert(run(false) == 0 && state() == "unsupported" && !fs::exists(metadata / "preinit"));
    assert(run() == 0);
    fs::remove(conf);
    assert(run() == 0 && state() == "error" && !fs::exists(staged));
    put(conf, "write /proc/example 1\n");
    assert(run() == 0);
    modules.clear();
    assert(run() == 0 && preinit::module_states(metadata, status).empty());
    assert(!fs::exists(metadata / "preinit"));

    // Reject links instead of copying files outside a module. Windows may lack
    // permission to create symlinks, so this case always runs in Linux CI.
    std::error_code error;
    fs::create_symlink(conf, module / "preinit/link", error);
    if (!error) {
        modules["example"] = module;
        assert(run() == 0 && state() == "error" && !fs::exists(staged));
    }
    puts("PASS preinit synchronization, limits, failed copies and boot-loop recovery");
}
