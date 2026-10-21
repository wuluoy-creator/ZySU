// Exercise the production post-fs-data coordinator against boot-stage and
// kernel-event doubles. No Android services, mounts or root access are used.
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "module/module.hpp"

namespace fixture {
enum class Event { Mount, UmountConfig, PostMount, VfsRefresh, ModuleMounted };
bool uapi_matches;
bool magisk_present;
bool safe_mode;
bool mount_ok;
bool vfs_refresh_ok;
bool kernel_mounts_ready;
bool native_mount_present;
int notifications;
int skipped_mounts;
std::vector<Event> events;

void reset() {
    uapi_matches = mount_ok = vfs_refresh_ok = true;
    magisk_present = safe_mode = kernel_mounts_ready = native_mount_present = false;
    notifications = skipped_mounts = 0;
    events.clear();
}

std::size_t position(Event event) {
    const auto found = std::find(events.begin(), events.end(), event);
    assert(found != events.end());
    return static_cast<std::size_t>(found - events.begin());
}

void expect_mount_handoff() {
    assert(notifications == 1);
    assert(kernel_mounts_ready && native_mount_present);
    assert(position(Event::Mount) < position(Event::UmountConfig));
    assert(position(Event::UmountConfig) < position(Event::PostMount));
    assert(position(Event::PostMount) < position(Event::VfsRefresh));
    assert(position(Event::VfsRefresh) < position(Event::ModuleMounted));
}

void expect_skipped_handoff() {
    assert(notifications == 0 && skipped_mounts == 1);
    assert(!kernel_mounts_ready && !native_mount_present);
    assert(events.empty());
}
}  // namespace fixture

namespace sumhp {
void embedded_post_fs_data() {}
void embedded_restore_kernel_build() {}
void embedded_mount_skipped(const std::string&) {
    ++fixture::skipped_mounts;
}
}  // namespace sumhp

namespace ksud {
constexpr auto BOOT_STAGE_TIMEOUT = std::chrono::seconds(35);
constexpr const char* WORKING_DIR = "/fixture/work";
constexpr const char* MODULE_DIR = "/fixture/modules";
constexpr const char* LOG_DIR = "/fixture/log";
constexpr const char* PROFILE_DIR = "/fixture/profiles";

bool ensure_uapi_version_matched() {
    return fixture::uapi_matches;
}
int set_init_pgrp() {
    return 0;
}
void report_post_fs_data() {}
bool start_ksud_integrity_monitor() {
    return true;
}
void umask(unsigned int) {}
void clear_all_temp_configs() {}
void catch_bootlog(const char*, const std::vector<const char*>&) {}
bool is_safe_mode() {
    return fixture::safe_mode;
}
bool has_magisk() {
    return fixture::magisk_present;
}
int exec_common_scripts(const std::string&, ScriptWait) {
    return 0;
}
void ensure_dir_exists(const char*) {}
int ensure_binaries(bool) {
    return 0;
}
bool restorecon() {
    return true;
}
bool restorecon(const char*, bool) {
    return true;
}
int disable_all_modules() {
    return 0;
}
int handle_updated_modules() {
    return 0;
}
int prune_modules() {
    return 0;
}
int regenerate_preinit_rc() {
    return 0;
}
int load_sepolicy_rule() {
    return 0;
}
void apply_profile_sepolies() {}
int init_features() {
    return 0;
}
void ensure_sulogd_running_if_enabled() {}
int metamodule_exec_stage_script(const std::string&, ScriptWait) {
    return 0;
}
int exec_stage_script(const std::string&, ScriptWait) {
    return 0;
}
int load_system_prop() {
    return 0;
}

int metamodule_exec_mount_script() {
    fixture::events.push_back(fixture::Event::Mount);
    // A backend can leave earlier successful mounts attached even if another
    // backend fails. Both outcomes need the kernel's per-app cleanup enabled.
    fixture::native_mount_present = true;
    return fixture::mount_ok ? 0 : 1;
}

int umount_apply_config() {
    fixture::events.push_back(fixture::Event::UmountConfig);
    return 0;
}

void run_stage(const std::string& stage, ScriptWait) {
    assert(stage == "post-mount");
    fixture::events.push_back(fixture::Event::PostMount);
}

int refresh_sucompat_vfs() {
    fixture::events.push_back(fixture::Event::VfsRefresh);
    return fixture::vfs_refresh_ok ? 0 : 1;
}

void report_module_mounted() {
    fixture::events.push_back(fixture::Event::ModuleMounted);
    ++fixture::notifications;
    // Stand in for EVENT_MODULE_MOUNTED's kernel readiness latch. Merely
    // registering unmount paths above must not make this latch true.
    fixture::kernel_mounts_ready = true;
}

void hide_bootloader_status() {}
int chdir(const char*) {
    return 0;
}

#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)
#include "boot_mount_notify_under_test.inc"
}  // namespace ksud

int main() {
    using namespace fixture;

    reset();
    assert(ksud::on_post_fs_data() == 0);
    expect_mount_handoff();

    reset();
    mount_ok = false;
    assert(ksud::on_post_fs_data() == 0);
    expect_mount_handoff();

    // An unrelated late VFS refresh failure cannot strand mounted modules
    // behind the kernel's "no modules mounted yet" fast path either.
    reset();
    vfs_refresh_ok = false;
    assert(ksud::on_post_fs_data() == 0);
    expect_mount_handoff();

    reset();
    uapi_matches = false;
    assert(ksud::on_post_fs_data() == 0);
    expect_skipped_handoff();

    reset();
    magisk_present = true;
    assert(ksud::on_post_fs_data() == 0);
    expect_skipped_handoff();

    reset();
    safe_mode = true;
    assert(ksud::on_post_fs_data() == 0);
    expect_skipped_handoff();

    std::puts("PASS boot mount notification ordering, partial failures and skipped stages");
}
