// Exercise the production upgrade using real temporary files. Only Android
// metadata and the platform atomic writer are substituted on the host.
#include "core/config_document.hpp"
#include "core/json.hpp"
#include "core/user_hide_store.hpp"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
#include "user_hide_digest_fixture.inc"
namespace {
bool fail_write = false;
bool fail_sync = false;

void put(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << contents;
    assert(output.good());
}

std::string get(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input.good());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}
}  // namespace

namespace ksud {
std::optional<std::string> read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        errno = EIO;
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

bool write_file_durable(const fs::path& path, const std::string& contents, std::string& error) {
    if (fail_write) {
        errno = ENOSPC;
        error = "file not replaced: ENOSPC";
        return false;
    }
    put(path, contents);
    if (fail_sync) {
        errno = EIO;
        error = "file replaced but durability is unconfirmed: EIO";
        return false;
    }
    return true;
}
}  // namespace ksud

namespace sumhp {
#include "sumh_config_document_under_test.inc"
bool prepare_private_directory(const fs::path& path, std::string&) {
    return fs::is_directory(fs::symlink_status(path));
}
bool prepare_private_file(const fs::path& path, std::string&) {
    return fs::is_regular_file(fs::symlink_status(path));
}
bool sync_private_directory(const fs::path& path, std::string& error) {
    assert(fs::is_directory(path));
    if (fail_sync) {
        errno = EIO;
        error = "injected directory sync failure";
        return false;
    }
    return true;
}
bool marker_matches_current_boot(const fs::path& path) {
    std::ifstream input(path);
    std::string id;
    return std::getline(input, id) && id == "current-boot";
}
#include "sumh_migration_under_test.inc"
}  // namespace sumhp

int main() {
    const auto root = fs::temp_directory_path() /
                      ("sumh-migration-test-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto legacy = root / "kagami";
    const auto current = root / "sumhp";
    const auto reset = [&] {
        fs::remove_all(root);
        fs::create_directories(current / "run");
        fail_write = false;
        fail_sync = false;
    };
    std::string error;
    reset();
    assert(sumhp::migrate_legacy_state(legacy, current, error));
    assert(!fs::exists(legacy));
    put(legacy / "config.json", R"({"mount_backend":"kasumi","work_dir":"/dev/kagami",
        "mirror_dir":"/dev/kagami/mirror","debug":true})");
    put(legacy / "module_mode.json", R"({"first":"kasumi","other":"overlay"})");
    put(legacy / "module_rules.json", R"({"first":[{"path":"/system/kasumi","mode":"kasumi"},
        {"path":"/vendor","mode":"hide"}],"configured":{"mode":"kasumi","rules":[
        {"path":"/system/keep","mode":"kasumi"}]},"custom":"unchanged"})");
    put(legacy / "user_hide_rules.json", "[\"/data/kasumi\"]\n");
    put(legacy / "mirror.img", "image contents");
    put(legacy / "run/boot_attempts", "2\n");
    put(legacy / "run/mount_disabled", "disabled\n");
    assert(sumhp::migrate_legacy_state(legacy, current, error));
    auto config = json::parse(get(current / "config.json"));
    assert(config.find("mount_backend")->s == "sumh");
    assert(config.find("work_dir")->s == "/dev/sumhp");
    assert(config.find("mirror_dir")->s == "/dev/sumhp/mirror");
    assert(config.find("debug")->b);
    auto modes = json::parse(get(current / "module_mode.json"));
    assert(modes.find("first")->s == "sumh" && modes.find("other")->s == "overlay");
    auto rules = json::parse(get(current / "module_rules.json"));
    assert(rules.find("first")->a[0].find("mode")->s == "sumh");
    assert(rules.find("first")->a[0].find("path")->s == "/system/kasumi");
    assert(rules.find("first")->a[1].find("mode")->s == "hide");
    assert(rules.find("configured")->find("mode")->s == "sumh");
    assert(rules.find("configured")->find("rules")->a[0].find("mode")->s == "sumh");
    assert(get(current / "user_hide_rules.json") == "[\"/data/kasumi\"]\n");
    assert(get(current / "mirror.img") == "image contents");
    assert(get(current / "run/boot_attempts") == "2\n");
    assert(get(current / "run/mount_disabled") == "disabled\n");
    assert(!fs::exists(legacy));
    const auto canonical = get(current / "config.json");
    assert(sumhp::migrate_legacy_state(legacy, current, error));
    assert(get(current / "config.json") == canonical);

    // Current settings win; unknown data and obsolete live state are untouched.
    put(legacy / "config.json", R"({"mount_backend":"magic"})");
    put(legacy / "private-data", "keep");
    put(legacy / "run/kasumi_active", "previous boot");
    put(legacy / "kagamid.pid", "123");
    assert(sumhp::migrate_legacy_state(legacy, current, error));
    assert(get(current / "config.json") == canonical);
    assert(get(legacy / "config.json") == R"({"mount_backend":"magic"})");
    assert(fs::exists(legacy / "private-data"));
    assert(!fs::exists(current / "run/kasumi_active"));
    assert(!fs::exists(current / "kagamid.pid"));

    // A manually relocated old configuration is upgraded as well.
    reset();
    put(current / "config.json", R"({"mount_backend":"kasumi","work_dir":"/dev/kagami-custom",
        "mirror_dir":"/custom/kagami"})");
    assert(sumhp::migrate_legacy_state(legacy, current, error));
    config = json::parse(get(current / "config.json"));
    assert(config.find("mount_backend")->s == "sumh");
    assert(config.find("work_dir")->s == "/dev/kagami-custom");
    assert(config.find("mirror_dir")->s == "/custom/kagami");

    // The retired fixed mirror default must retain its automatic semantics.
    reset();
    const std::string future_config =
        R"({"schema_version":2,"mount_backend":"kasumi","future":9007199254740993})";
    put(current / "config.json", future_config);
    assert(sumhp::migrate_legacy_state(legacy, current, error));
    assert(get(current / "config.json") == future_config);

    reset();
    const std::string unknown_legacy = R"({"mount_backend":"kasumi","future":9007199254740993})";
    put(current / "config.json", unknown_legacy);
    assert(sumhp::migrate_legacy_state(legacy, current, error));
    assert(get(current / "config.json") == unknown_legacy);

    // A future document imported from the legacy directory is copied exactly,
    // so the version-aware loader can report it without destroying its data.
    reset();
    put(legacy / "config.json", future_config);
    assert(sumhp::migrate_legacy_state(legacy, current, error));
    assert(get(current / "config.json") == future_config);

    // The retired fixed mirror default must retain its automatic semantics.
    reset();
    put(legacy / "config.json", R"({"mirror_dir":"/dev/kagami_mirror"})");
    assert(sumhp::migrate_legacy_state(legacy, current, error));
    config = json::parse(get(current / "config.json"));
    assert(config.find("mirror_dir")->s == "/dev/sumhp_mirror");

    // A daemon may have exited while its mounts still exist in this boot.
    reset();
    put(legacy / "config.json", R"({"mount_backend":"kasumi"})");
    put(legacy / "run/mount_orchestrator_boot", "current-boot\n");
    assert(!sumhp::migrate_legacy_state(legacy, current, error));
    assert(error.find("restart") != std::string::npos);
    assert(fs::exists(legacy / "config.json") && !fs::exists(current / "config.json"));
    put(legacy / "run/mount_orchestrator_boot", "previous-boot\n");
    assert(sumhp::migrate_legacy_state(legacy, current, error));
    assert(!fs::exists(current / "run/mount_orchestrator_boot"));

    // Current malformed settings must remain repairable through the daemon.
    reset();
    put(current / "config.json", "{invalid");
    put(legacy / "config.json", R"({"mount_backend":"kasumi"})");
    assert(sumhp::migrate_legacy_state(legacy, current, error));
    assert(error.empty() && get(current / "config.json") == "{invalid");
    assert(fs::exists(legacy / "config.json"));

    reset();
    put(legacy / "config.json", "{invalid");
    assert(!sumhp::migrate_legacy_state(legacy, current, error));
    assert(!error.empty());
    assert(get(legacy / "config.json") == "{invalid");
    assert(!fs::exists(current / "config.json"));
    put(legacy / "config.json", R"({"mount_backend":"kasumi"})");
    fail_write = true;
    assert(!sumhp::migrate_legacy_state(legacy, current, error));
    assert(fs::exists(legacy / "config.json"));
    assert(!fs::exists(current / "config.json"));
    fail_write = false;
    assert(sumhp::migrate_legacy_state(legacy, current, error));

    // A failed directory sync cannot discard the old configuration copy.
    reset();
    put(legacy / "config.json", R"({"mount_backend":"kasumi"})");
    fail_sync = true;
    assert(!sumhp::migrate_legacy_state(legacy, current, error));
    assert(get(legacy / "config.json") == R"({"mount_backend":"kasumi"})");
    assert(json::parse(get(current / "config.json")).find("mount_backend")->s == "sumh");
    fail_sync = false;
    assert(sumhp::migrate_legacy_state(legacy, current, error));

    reset();
    put(legacy / "config.json", R"({"mount_backend":"kasumi"})");
    fs::create_hard_link(legacy / "config.json", root / "linked-config.json");
    assert(!sumhp::migrate_legacy_state(legacy, current, error));
    assert(fs::exists(legacy / "config.json"));
    assert(!fs::exists(current / "config.json"));
    reset();
    fs::create_directories(legacy / "config.json");
    assert(!sumhp::migrate_legacy_state(legacy, current, error));
    assert(!fs::exists(current / "config.json"));

    // Relocation preserves versioned hide metadata and disabled definitions;
    // format conversion is reserved for an explicit definition mutation.
    reset();
    sumhp::hide::Document hidden;
    hidden.rules = {{std::string(32, 'a'), "/one", false, "name", "note", "group"}};
    const auto hidden_bytes = sumhp::hide::encode_document(hidden);
    put(legacy / "user_hide_rules.json", hidden_bytes);
    assert(sumhp::migrate_legacy_state(legacy, current, error));
    assert(get(current / "user_hide_rules.json") == hidden_bytes);
    reset();
    const auto unknown_hide = "{\"schema_version\":2,\"rules\":[]}";
    put(current / "user_hide_rules.json", unknown_hide);
    assert(sumhp::migrate_legacy_state(legacy, current, error));
    assert(get(current / "user_hide_rules.json") == unknown_hide);
    reset();
    put(legacy / "user_hide_rules.json", unknown_hide);
    assert(!sumhp::migrate_legacy_state(legacy, current, error));
    assert(get(legacy / "user_hide_rules.json") == unknown_hide &&
           !fs::exists(current / "user_hide_rules.json"));

    fs::remove_all(root);
    std::cout << "SUMHP configuration migration tests passed\n";
}
