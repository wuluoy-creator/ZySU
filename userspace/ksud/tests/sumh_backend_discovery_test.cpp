// Production discovery loop, real host trees and fault-injected filesystem edges.
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>
#include "mount/backend.hpp"

namespace {
namespace fs = std::filesystem;
fs::path fixture;
std::string fault;
fs::path fault_path;
int scans = 0;
int checks = 0;

void check(bool value) {
    assert(value);
    ++checks;
}
void reset_fault() {
    fault.clear();
    fault_path.clear();
    scans = 0;
}
bool fail(const std::string& phase, const fs::path& path, std::error_code& ec) {
    if (fault == phase && (fault_path.empty() || fault_path == path)) {
        ec = std::make_error_code(std::errc::permission_denied);
        return true;
    }
    return false;
}
}  // namespace

namespace discovery_model {
fs::file_status status(const fs::path& path, std::error_code& ec) {
    if (fail("root", path, ec))
        return {};
    return fs::status(path, ec);
}
bool exists(const fs::file_status& status) {
    return fs::exists(status);
}
bool exists(const fs::path& path, std::error_code& ec) {
    if (fail("exists", path, ec))
        return false;
    return fs::exists(path, ec);
}
struct directory_entry {
    fs::directory_entry inner;
    const fs::path& path() const { return inner.path(); }
    bool is_directory(std::error_code& ec) const {
        if (fail("entry", path(), ec))
            return false;
        return inner.is_directory(ec);
    }
};
class directory_iterator {
    fs::directory_iterator inner_;
    mutable directory_entry entry_;

public:
    directory_iterator() = default;
    directory_iterator(const fs::path& path, std::error_code& ec) {
        if (!fail("open", path, ec))
            inner_ = fs::directory_iterator(path, ec);
    }
    const directory_entry& operator*() const {
        entry_.inner = *inner_;
        return entry_;
    }
    bool operator!=(const directory_iterator& other) const { return inner_ != other.inner_; }
    directory_iterator& increment(std::error_code& ec) {
        ++scans;
        if (fault == "iterate") {
            // Real directory_iterator can become end at the same time that it
            // reports an error. The post-loop ec check is essential.
            inner_ = fs::directory_iterator{};
            ec = std::make_error_code(std::errc::io_error);
        } else {
            inner_.increment(ec);
        }
        return *this;
    }
};
}  // namespace discovery_model

namespace sumhp {
fs::path runtime_modules_dir() {
    return fixture / "modules";
}
fs::path runtime_data_dir() {
    return fixture / "controller";
}
}  // namespace sumhp
namespace sumhp::mount {
namespace fs = std::filesystem;
#include "sumh_backend_discovery_under_test.inc"
}  // namespace sumhp::mount

int main() {
    using namespace sumhp::mount;
    fixture = fs::temp_directory_path() /
              ("sumh-discovery-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::vector<ModuleEntry> output{{"preserved", "/unchanged"}};
    std::string error;
    check(!enumerate_mountable_modules_checked(output, error));
    check(output.size() == 1 && output[0].id == "preserved" && !error.empty());

    const auto modules = sumhp::runtime_modules_dir();
    fs::create_directories(fixture);
    std::ofstream(modules).put('x');
    check(!enumerate_mountable_modules_checked(output, error));
    check(output[0].id == "preserved" && errno == ENOTDIR);
    fs::remove(modules);
    fs::create_directories(modules);
    check(enumerate_mountable_modules_checked(output, error) && output.empty() && error.empty());

    const auto make_module = [&](const std::string& id) {
        fs::create_directories(modules / id);
        std::ofstream(modules / id / "module.prop") << "id=" << id;
    };
    for (const auto* id : {"zulu", "alpha", "disabled", "removed", "skip", "hot"})
        make_module(id);
    fs::create_directories(modules / "no-prop");
    std::ofstream(modules / "not-directory").put('x');
    std::ofstream(modules / "disabled" / "disable").put('x');
    // Marker presence retains prior semantics, including directory markers.
    fs::create_directories(modules / "removed" / "remove");
    std::ofstream(modules / "skip" / "skip_mount").put('x');
    const auto hot = sumhp::runtime_data_dir() / "run" / "hot_unmounted";
    fs::create_directories(hot);
    std::ofstream(hot / "hot").put('x');
    check(enumerate_mountable_modules_checked(output, error));
    check(output.size() == 2 && output[0].id == "alpha" && output[1].id == "zulu");
    check(output[0].path == modules / "alpha");

    const auto expect_unchanged = [&] {
        output = {{"preserved", "/unchanged"}};
        check(!enumerate_mountable_modules_checked(output, error));
        check(output.size() == 1 && output[0].id == "preserved" && !error.empty());
    };
    for (const auto* phase : {"root", "open", "entry", "iterate"}) {
        reset_fault();
        fault = phase;
        expect_unchanged();
    }
    for (const auto& path :
         {modules / "alpha" / "module.prop", modules / "alpha" / "disable",
          modules / "alpha" / "remove", modules / "alpha" / "skip_mount", hot / "alpha"}) {
        reset_fault();
        fault = "exists";
        fault_path = path;
        expect_unchanged();
        check(error.find(path.string()) != std::string::npos);
    }
    reset_fault();
    output = {{"preserved", "/unchanged"}};
    check(!enumerate_mountable_modules_checked(output, error, 0));
    check(errno == E2BIG && output[0].id == "preserved");
    check(!enumerate_mountable_modules_checked(output, error, 7));
    check(errno == E2BIG && output[0].id == "preserved");
    check(enumerate_mountable_modules_checked(output, error, 8));
    check(output.size() == 2);
    fault = "iterate";
    check(enumerate_mountable_modules().empty());  // compatibility wrapper only
    reset_fault();
    check(enumerate_mountable_modules().size() == 2);

    // Follow directory symlinks exactly as the original discovery did.
    std::error_code ec;
    fs::create_directory_symlink(modules / "alpha", modules / "linked", ec);
    if (!ec) {
        check(enumerate_mountable_modules_checked(output, error));
        check(output.size() == 3 && output[1].id == "linked");
    } else {
        std::cout << "symlink fixture unavailable: " << ec.message() << '\n';
    }
    fs::remove_all(fixture);
    std::cout << checks << " production module discovery checks passed\n";
}
