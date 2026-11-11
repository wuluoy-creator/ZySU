#ifndef __KSU_UAPI_BOOT_CONFIG_H
#define __KSU_UAPI_BOOT_CONFIG_H

#include "feature.h"

/* Shared by the C kernel parser and C++ boot patcher. Fixed
 * isolation/security/SUMH features and read-only status IDs deliberately cannot
 * be forced. */
#define KSU_BOOT_CONFIGURABLE_FEATURES(X)                                      \
  X(KSU_FEATURE_SULOG, "sulog")                                                \
  X(KSU_FEATURE_ADB_ROOT, "adb_root")                                          \
  X(KSU_FEATURE_SELINUX_HIDE, "selinux_hide")                                  \
  X(KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT, "webview_zygote_umount")                \
  X(KSU_FEATURE_SHELL_SU_ADB_ONLY, "shell_su_adb_only")                        \
  X(KSU_FEATURE_HIDE_BOOTLOADER, "hide_bootloader")                            \
  X(KSU_FEATURE_UNSHARE_MNT, "unshare_mnt")                                    \
  X(KSU_FEATURE_UNICODE_PATH_GUARD, "unicode_path_guard")

#define KSU_BLOCK_MODULES_SIZE 256U
#define KSU_BLOCK_MODULE_NAME_MAX 64U
#define KSU_FORCE_FEATURES_SIZE 232U

#endif
