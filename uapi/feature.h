#ifndef __KSU_UAPI_FEATURE_H
#define __KSU_UAPI_FEATURE_H

#ifdef __cplusplus
extern "C" {
#endif // #ifdef __cplusplus

enum ksu_feature_id {
  // Legacy provider is fixed off; enabling it is rejected.
  KSU_FEATURE_SU_COMPAT = 0,
  // Feature 1 is retired; do not reuse it.
  KSU_FEATURE_SULOG = 2,
  KSU_FEATURE_ADB_ROOT = 3,
  KSU_FEATURE_SELINUX_HIDE = 4,
  KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT = 5,
  KSU_FEATURE_SHELL_SU_ADB_ONLY = 6,
  // Always enabled; retained as a read-only feature for ABI compatibility.
  KSU_FEATURE_ENHANCED_SECURITY = 100,
  // Feature 101 is retired; do not reuse it.
  // ZySU extensions number from 100 up; 0-99 reserved for upstream KSU.
  // The default root profile always carries NO_NEW_PRIVS; read-only.
  KSU_FEATURE_DEFAULT_NO_NEW_PRIVS = 102,
  // Feature 103 is retired; do not reuse it.
  KSU_FEATURE_HIDE_BOOTLOADER = 104,
  // Fixed SU provider; 1 refreshes its binding, 0 is rejected.
  KSU_FEATURE_SUMH_SUCOMPAT = 105,
  // Always-on VFS engine; reports readiness and rejects attempts to disable.
  KSU_FEATURE_SUMH = 106,
  KSU_FEATURE_UNSHARE_MNT = 107,
  // Read-only runtime state; feature 4 remains the persisted requested setting.
  KSU_FEATURE_SELINUX_HIDE_STATUS = 108,
  // Opt-in casefold/FUSE path mitigation; disabled by default.
  KSU_FEATURE_UNICODE_PATH_GUARD = 109,
  // Read-only hook readiness and runtime state for feature 109.
  KSU_FEATURE_UNICODE_PATH_GUARD_STATUS = 110,

  KSU_FEATURE_MAX
};

enum ksu_selinux_hide_status {
  KSU_SELINUX_HIDE_DISABLED = 0,
  KSU_SELINUX_HIDE_ACTIVE = 1,
  KSU_SELINUX_HIDE_PENDING_REBOOT = 2,
  KSU_SELINUX_HIDE_FAILED = 3,
};

enum ksu_unicode_path_guard_status {
  KSU_UNICODE_PATH_GUARD_DISABLED = 0,
  KSU_UNICODE_PATH_GUARD_ACTIVE = 1,
  KSU_UNICODE_PATH_GUARD_UNSUPPORTED = 2,
};

#ifdef __cplusplus
}
#endif // #ifdef __cplusplus

#endif // #ifndef __KSU_UAPI_FEATURE_H
