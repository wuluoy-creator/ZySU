#ifndef _SUMH_UAPI_H
#define _SUMH_UAPI_H

#ifdef __KERNEL__
#include <linux/bits.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <linux/types.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>
#endif // #ifdef __KERNEL__

/* Protocol 2 fixes built-in protections on and retires their setters. */
#define SUMH_PROTOCOL_VERSION 2

#define SUMH_MAX_LEN_PATHNAME 256

/*
 * SUMH inode marking bits (stored in inode->i_mapping->flags)
 * Using high bits to avoid conflict with kernel AS_* flags and SUSFS bits
 * SUSFS uses bits 33-39, we use 40+
 */
#ifdef __KERNEL__
#define AS_FLAGS_SUMH_HIDE 40
#define BIT_SUMH_HIDE BIT(40)
/* Marks a directory as containing hidden entries (for fast filldir skip) */
#define AS_FLAGS_SUMH_DIR_HAS_HIDDEN 41
#define BIT_SUMH_DIR_HAS_HIDDEN BIT(41)
/* Marks an inode for kstat spoofing */
#define AS_FLAGS_SUMH_SPOOF_KSTAT 42
#define BIT_SUMH_SPOOF_KSTAT BIT(42)
/* Marks a directory as having inject/merge rules (fast path for iterate_dir) */
#define AS_FLAGS_SUMH_DIR_HAS_INJECT 43
#define BIT_SUMH_DIR_HAS_INJECT BIT(43)
/* Marks an inode as having shadow inode_operations installed (lookup-time i_op
 * override) */
#define AS_FLAGS_SUMH_IOP_INSTALLED 44
#define BIT_SUMH_IOP_INSTALLED BIT(44)
/* Marks a directory inode as having shadow file_operations installed for
 * readdir */
#define AS_FLAGS_SUMH_FOP_INSTALLED 45
#define BIT_SUMH_FOP_INSTALLED BIT(45)
#endif // #ifdef __KERNEL__

struct sumh_syscall_arg {
  const char *src;
  const char *target;
  int type;
};

struct sumh_syscall_list_arg {
  char *buf; // Keep as char* for output buffer
  size_t size;
};

/*
 * Legacy explicit kstat overrides. Zero scalar fields preserve the original
 * value; a zero timestamp pair preserves that timestamp. ADD and UPDATE upsert.
 * Similar to susfs sus_kstat but with SUMH conventions
 */
struct sumh_spoof_kstat {
  unsigned long
      target_ino; /* Inode-only wildcard when target_pathname is empty */
  /* A nonempty absolute path must resolve now and overrides target_ino with
   * its current identity. Reissue either command after replacement/remount. */
  char target_pathname[SUMH_MAX_LEN_PATHNAME];
  unsigned long spoofed_ino;     /* Spoofed inode number */
  unsigned long spoofed_dev;     /* Spoofed device number */
  unsigned int spoofed_nlink;    /* Spoofed link count */
  long long spoofed_size;        /* Spoofed file size */
  long spoofed_atime_sec;        /* Spoofed access time (seconds) */
  long spoofed_atime_nsec;       /* Spoofed access time (nanoseconds) */
  long spoofed_mtime_sec;        /* Spoofed modification time (seconds) */
  long spoofed_mtime_nsec;       /* Spoofed modification time (nanoseconds) */
  long spoofed_ctime_sec;        /* Spoofed change time (seconds) */
  long spoofed_ctime_nsec;       /* Spoofed change time (nanoseconds) */
  unsigned long spoofed_blksize; /* Spoofed block size */
  unsigned long long spoofed_blocks; /* Spoofed block count */
  int is_static; /* Retained for ABI compatibility; no automatic rebinding */
  int err;       /* Error code for userspace feedback */
};

#define SUMH_MANAGED_KSTAT_MAX_RULES 32
#define SUMH_KSTAT_FIELD_DEV (1U << 0)
#define SUMH_KSTAT_FIELD_INO (1U << 1)
#define SUMH_KSTAT_FIELD_NLINK (1U << 2)
#define SUMH_KSTAT_FIELD_SIZE (1U << 3)
#define SUMH_KSTAT_FIELD_ATIME (1U << 4)
#define SUMH_KSTAT_FIELD_MTIME (1U << 5)
#define SUMH_KSTAT_FIELD_CTIME (1U << 6)
#define SUMH_KSTAT_FIELD_BLKSIZE (1U << 7)
#define SUMH_KSTAT_FIELD_BLOCKS (1U << 8)
#define SUMH_KSTAT_FIELD_ALL ((1U << 9) - 1)

/* Atomically replaces controller-owned path rules. Only masked fields apply,
 * including zero values; legacy ADD/UPDATE keep zero-as-unchanged.
 * The target path must currently resolve. count == 0 clears only this set.
 */
struct sumh_managed_kstat_rule {
  struct sumh_spoof_kstat values;
  __u32 field_mask;
  __u32 reserved;
};

struct sumh_managed_kstat_arg {
  __u32 size;
  __u32 count;
  __aligned_u64 rules;
  __u32 reserved[2];
};

/* Live kernel snapshot only; generation is scoped to this module lifetime. */
struct sumh_managed_kstat_status_arg {
  __u32 size;
  __u32 count;
  __aligned_u64 generation;
  __u32 reserved[2];
};

/*
 * Feature flags for SUMH_CMD_GET_FEATURES
 */
#define SUMH_FEATURE_KSTAT_SPOOF (1 << 0)
/* Bit 1 remains reserved for the removed uname feature. */
/* Bit 2 remains reserved for the removed cmdline feature. */
#define SUMH_FEATURE_MERGE_DIR (1 << 5)
#define SUMH_FEATURE_MOUNT_HIDE                                                \
  (1 << 6) /* use shared proc mount snapshots for isolated readers */
#define SUMH_FEATURE_MAPS_SPOOF                                                \
  (1 << 7) /* spoof ino/dev/pathname in /proc/pid/maps (read buffer filter) */
#define SUMH_FEATURE_STATFS_SPOOF                                              \
  (1 << 8) /* spoof statfs f_type so direct matches resolved                   \
              (INCONSISTENT_MOUNT) */
#define SUMH_FEATURE_FAKE_MOUNTINFO                                            \
  (1 << 9) /* shared donor mount views with normalized propagation IDs */
#define SUMH_FEATURE_MOUNT_HIDE_AGGRESSIVE                                     \
  (1 << 12) /* mount-ns link projection */
#define SUMH_FEATURE_OVERLAY_XATTR_HIDE (1 << 13)
#define SUMH_FEATURE_MANAGED_HIDE (1 << 14)
#define SUMH_FEATURE_KERNEL_BUILD_SPOOF (1 << 15)
#define SUMH_FEATURE_MANAGED_MAPS (1 << 16)
#define SUMH_FEATURE_MANAGED_KSTAT (1 << 17)

#define SUMH_KERNEL_BUILD_MAX 65

/* SUMH protocol 1. Spoofs release/version in the initial UTS
 * namespace. SET requires size == sizeof(struct), enable 0/1 and zero reserved
 * fields. Enabled strings are nonempty, NUL terminated, at most 64 bytes and
 * contain no control bytes; unused string bytes must be zero. Disabling
 * restores the values captured before the first enable. GET returns the
 * current effective strings, including the real values while disabled.
 */
struct sumh_kernel_build_arg {
  __u32 size;
  __u32 enable;
  __u32 reserved[2];
  char release[SUMH_KERNEL_BUILD_MAX];
  char version[SUMH_KERNEL_BUILD_MAX];
  char reserved_tail[2];
};

#define SUMH_USER_HIDE_PATH_MAX 4096
#define SUMH_USER_HIDE_LIVE 0
#define SUMH_USER_HIDE_SUSPENDED 1
#define SUMH_USER_HIDE_PENDING 0
#define SUMH_USER_HIDE_BINDING 1
#define SUMH_USER_HIDE_BOUND 2
#define SUMH_USER_HIDE_ERROR 3

/* SUMH protocol 1. QUERY returns the first ID greater than rule_id;
 * rule_id == 0 in the reply marks the end. Paths include a terminating NUL,
 * excluded from path_len. Input flags and reserved fields must be zero. */
struct sumh_user_hide_arg {
  __u32 size;
  __u32 flags;
  __aligned_u64 rule_id;
  __aligned_u64 generation;
  __aligned_u64 reserved_id;
  __u32 management;
  __u32 binding;
  __s32 error;
  __u32 path_len;
  __aligned_u64 reserved[2];
  char path[SUMH_USER_HIDE_PATH_MAX];
};

/* Raw metadata of a source path in the caller's mount namespace. Fixed-width
 * fields make the reply independent of the native kstat/unsigned-long ABI.
 * dev is the kernel's packed dev_t value, not a userspace dev_t encoding.
 */
struct sumh_raw_kstat_arg {
  __u32 size;
  __u32 reserved;
  char source_path[SUMH_USER_HIDE_PATH_MAX];
  __aligned_u64 ino;
  __u32 dev;
  __u32 nlink;
  __s64 size_bytes;
  __s64 atime_sec;
  __s64 atime_nsec;
  __s64 mtime_sec;
  __s64 mtime_nsec;
  __s64 ctime_sec;
  __s64 ctime_nsec;
  __u32 blksize;
  __u32 reserved2;
  __aligned_u64 blocks;
};

#define SUMH_MOUNT_HIDE_MODE_NORMAL 0
#define SUMH_MOUNT_HIDE_MODE_AGGRESSIVE 1

/*
 * Maps spoof rule: when a /proc/pid/maps line has (target_ino[, target_dev]),
 * replace ino/dev/pathname with spoofed values. target_dev 0 = match any dev.
 */
struct sumh_maps_rule {
  unsigned long target_ino;
  unsigned long target_dev; /* 0 = match any device */
  unsigned long spoofed_ino;
  unsigned long spoofed_dev;
  char spoofed_pathname[SUMH_MAX_LEN_PATHNAME];
  int err;
};

#define SUMH_MANAGED_MAPS_MAX_RULES 256

/* Atomically replace the controller-owned maps rules. rules points to count
 * sumh_maps_rule entries; count 0 clears only this owned set. size must equal
 * sizeof(struct), reserved fields and each rule's err must be zero. Keys
 * (target_ino, target_dev) must be unique and target_ino nonzero. Device values
 * use the kernel's 32-bit MKDEV encoding (12 major and 20 minor bits).
 * Pathnames must be NUL terminated and contain no CR/LF. Exact device matches
 * take precedence over wildcard matches. Legacy ADD rules remain separate and
 * win over managed rules with the same key; legacy CLEAR removes both sets.
 */
struct sumh_managed_maps_arg {
  __u32 size;
  __u32 count;
  __aligned_u64 rules;
  __u32 reserved[2];
};

/*
 * Feature config structs - enable + reserved for future custom rules.
 * mount_hide: path_pattern empty = hide all overlay; non-empty = hide only
 * matching (future). maps_spoof: rules via ADD_MAPS_RULE; struct allows future
 * inline rule. statfs_spoof: path empty = auto spoof; non-empty = custom
 * path->f_type (future).
 */
struct sumh_mount_hide_arg {
  int enable;
  char path_pattern[SUMH_MAX_LEN_PATHNAME]; /* reserved: empty = all overlay */
  int err;
};

struct sumh_maps_spoof_arg {
  int enable;
  /* reserved for future: inline rule, batch config */
  char reserved[sizeof(struct sumh_maps_rule)];
  int err;
};

struct sumh_statfs_spoof_arg {
  int enable;
  char path[SUMH_MAX_LEN_PATHNAME]; /* reserved: empty = auto */
  unsigned long spoof_f_type;       /* reserved: 0 = use d_real_inode */
  int err;
};

// ioctl definitions (for fd-based mode)
// Must be after struct definitions
#define SUMH_IOC_MAGIC 'S'
#define SUMH_IOC_GET_ENABLED _IOR(SUMH_IOC_MAGIC, 39, int)
#define SUMH_IOC_ADD_RULE _IOW(SUMH_IOC_MAGIC, 1, struct sumh_syscall_arg)
#define SUMH_IOC_DEL_RULE _IOW(SUMH_IOC_MAGIC, 2, struct sumh_syscall_arg)
#define SUMH_IOC_HIDE_RULE _IOW(SUMH_IOC_MAGIC, 3, struct sumh_syscall_arg)
#define SUMH_IOC_CLEAR_ALL _IO(SUMH_IOC_MAGIC, 5)
#define SUMH_IOC_GET_VERSION _IOR(SUMH_IOC_MAGIC, 6, int)
#define SUMH_IOC_LIST_RULES                                                    \
  _IOWR(SUMH_IOC_MAGIC, 7, struct sumh_syscall_list_arg)
#define SUMH_IOC_SET_DEBUG _IOW(SUMH_IOC_MAGIC, 8, int)
#define SUMH_IOC_REORDER_MNT_ID _IO(SUMH_IOC_MAGIC, 9)
/* Reserved legacy setter: fixed protections reject changes with -EOPNOTSUPP. */
#define SUMH_IOC_SET_STEALTH _IOW(SUMH_IOC_MAGIC, 10, int)
/* An empty src clears xattr targets when OVERLAY_XATTR_HIDE is advertised. */
#define SUMH_IOC_HIDE_OVERLAY_XATTRS                                           \
  _IOW(SUMH_IOC_MAGIC, 11, struct sumh_syscall_arg)
#define SUMH_IOC_ADD_MERGE_RULE                                                \
  _IOW(SUMH_IOC_MAGIC, 12, struct sumh_syscall_arg)
/* ABI-reserved legacy slot; pure virtual kernels return -EOPNOTSUPP. */
#define SUMH_IOC_SET_MIRROR_PATH                                               \
  _IOW(SUMH_IOC_MAGIC, 14, struct sumh_syscall_arg)
#define SUMH_IOC_ADD_SPOOF_KSTAT                                               \
  _IOW(SUMH_IOC_MAGIC, 15, struct sumh_spoof_kstat)
#define SUMH_IOC_UPDATE_SPOOF_KSTAT                                            \
  _IOW(SUMH_IOC_MAGIC, 16, struct sumh_spoof_kstat)
/* Command 18 remains reserved for the removed cmdline operation. */
#define SUMH_IOC_GET_FEATURES _IOR(SUMH_IOC_MAGIC, 19, int)
/* Legacy ABI: 1 is a no-op; 0 returns -EPERM. SUMH is always enabled. */
#define SUMH_IOC_SET_ENABLED _IOW(SUMH_IOC_MAGIC, 20, int)
#define SUMH_IOC_GET_HOOKS                                                     \
  _IOWR(SUMH_IOC_MAGIC, 22, struct sumh_syscall_list_arg)
/* Upsert an unmanaged rule by (target_ino, target_dev). */
#define SUMH_IOC_ADD_MAPS_RULE _IOW(SUMH_IOC_MAGIC, 23, struct sumh_maps_rule)
#define SUMH_IOC_CLEAR_MAPS_RULES _IO(SUMH_IOC_MAGIC, 24)
/* Reserved legacy setter; mount hiding has no mutable enabled state. */
#define SUMH_IOC_SET_MOUNT_HIDE                                                \
  _IOW(SUMH_IOC_MAGIC, 25, struct sumh_mount_hide_arg)
#define SUMH_IOC_SET_MAPS_SPOOF                                                \
  _IOW(SUMH_IOC_MAGIC, 26, struct sumh_maps_spoof_arg)
/* Reserved legacy setter; statfs uses the backing view whenever available. */
#define SUMH_IOC_SET_STATFS_SPOOF                                              \
  _IOW(SUMH_IOC_MAGIC, 27, struct sumh_statfs_spoof_arg)
/* Commands 17 and 28 remain reserved for removed uname operations. */

/* Reserved legacy setter; native namespace identity is the only behavior. */
#define SUMH_IOC_SET_MOUNT_HIDE_MODE _IOW(SUMH_IOC_MAGIC, 38, int)

#define SUMH_IOC_USER_HIDE_UPSERT                                              \
  _IOWR(SUMH_IOC_MAGIC, 40, struct sumh_user_hide_arg)
#define SUMH_IOC_USER_HIDE_DELETE                                              \
  _IOWR(SUMH_IOC_MAGIC, 41, struct sumh_user_hide_arg)
#define SUMH_IOC_USER_HIDE_QUERY                                               \
  _IOWR(SUMH_IOC_MAGIC, 42, struct sumh_user_hide_arg)
#define SUMH_IOC_USER_HIDE_CLEAR _IO(SUMH_IOC_MAGIC, 43)
#define SUMH_IOC_USER_HIDE_RETRY                                               \
  _IOWR(SUMH_IOC_MAGIC, 44, struct sumh_user_hide_arg)

#define SUMH_IOC_SET_KERNEL_BUILD                                              \
  _IOW(SUMH_IOC_MAGIC, 45, struct sumh_kernel_build_arg)
#define SUMH_IOC_GET_KERNEL_BUILD                                              \
  _IOR(SUMH_IOC_MAGIC, 46, struct sumh_kernel_build_arg)
/* Bypass the spoof for internal KMI/payload selection. Same output format. */
#define SUMH_IOC_GET_ORIGINAL_KERNEL_BUILD                                     \
  _IOR(SUMH_IOC_MAGIC, 47, struct sumh_kernel_build_arg)
#define SUMH_IOC_SET_MANAGED_MAPS                                              \
  _IOW(SUMH_IOC_MAGIC, 48, struct sumh_managed_maps_arg)
#define SUMH_IOC_SET_MANAGED_KSTAT                                             \
  _IOW(SUMH_IOC_MAGIC, 49, struct sumh_managed_kstat_arg)
#define SUMH_IOC_GET_RAW_KSTAT                                                 \
  _IOWR(SUMH_IOC_MAGIC, 50, struct sumh_raw_kstat_arg)
#define SUMH_IOC_GET_MANAGED_KSTAT_STATUS                                      \
  _IOWR(SUMH_IOC_MAGIC, 51, struct sumh_managed_kstat_status_arg)

#endif /* _SUMH_UAPI_H */
