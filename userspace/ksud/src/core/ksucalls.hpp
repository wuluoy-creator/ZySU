#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Kernel uapi headers — single source of truth for ioctl numbers,
// struct layouts, and feature IDs.  Guarded for userspace by
// #ifndef __KERNEL__ blocks in each uapi header.
extern "C" {
#include "uapi/supercall.h"
}

namespace ksud {

// C++ convenience aliases (keep callers unchanged)
using GetInfoCmd = ksu_get_info_cmd;
using ReportEventCmd = ksu_report_event_cmd;
using SetSepolicyCmd = ksu_set_sepolicy_cmd;
using CheckSafemodeCmd = ksu_check_safemode_cmd;
using GetFeatureCmd = ksu_get_feature_cmd;
using SetFeatureCmd = ksu_set_feature_cmd;
using GetWrapperFdCmd = ksu_get_wrapper_fd_cmd;
using GetSulogFdCmd = ksu_get_sulog_fd_cmd;
using ManageMarkCmd = ksu_manage_mark_cmd;
using NukeExt4SysfsCmd = ksu_nuke_ext4_sysfs_cmd;

// API functions
// Install the SIGSYS guard used only while trying the legacy reboot syscall
// fallback for driver-fd retrieval. Other SIGSYS deliveries retain the
// previously installed disposition.
void setup_sigsys_handler();
int ksuctl(int request, void* arg);
// Claim the scoped fd installed after sucompat's exec; it is already CLOEXEC
// for the target shell. Call only during fresh-exec, single-threaded su startup.
// Returns 1 when claimed, 0 when absent, or -errno.
int claim_inherited_su_driver_fd();

int32_t get_version();
uint32_t get_flags();
uint32_t get_uapi_version();
constexpr uint32_t uapi_version() {
    return KERNEL_SU_UAPI_VERSION;
}
bool is_lkm();
bool is_lkm_bundled();
bool is_late_load();
bool is_preinit_supported();
// Zero addresses denote symbols absent from the running kernel image.
std::optional<std::vector<uint64_t>> lookup_symbols(const std::vector<std::string>& names);
const char* runtime_mode();
bool ensure_uapi_version_matched(std::string* error = nullptr);

int grant_root();
int set_ksu_no_new_privs();
void report_post_fs_data();
// Returns 1 to start services, 0 to skip, or -1 on error (errno is set).
int report_services();
void report_boot_complete();
void report_module_mounted();
bool check_kernel_safemode();

int set_sepolicy(const void* payload, uint64_t payload_len);

// Feature management
// Returns: pair<value, supported>
std::pair<uint64_t, bool> get_feature(uint32_t feature_id);
bool is_feature_forced(uint32_t feature_id);
int set_feature(uint32_t feature_id, uint64_t value);
int get_su_path_config(ksu_su_path_config* config);
int set_su_path_config(const ksu_su_path_config& config);

int get_wrapped_fd(int fd);
int get_sulog_fd();

// Mark management
uint32_t mark_get(int32_t pid);
int mark_set(int32_t pid);
int mark_unset(int32_t pid);
int mark_refresh();

int nuke_ext4_sysfs(const std::string& mnt);
int set_init_pgrp();

int get_manager_uid();

}  // namespace ksud
