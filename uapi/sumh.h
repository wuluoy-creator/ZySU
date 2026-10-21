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

#define SUMH_PROTOCOL_VERSION 1

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
#define SUMH_FEATURE_RUNTIME_STATE (1 << 17)
#define SUMH_FEATURE_RUNTIME_SET (1 << 18)
#define SUMH_FEATURE_KERNEL_BUILD_CAS (1 << 19)
#define SUMH_FEATURE_RULE_INVENTORY (1 << 20)
#define SUMH_FEATURE_TEMPORARY_MAPS_DELETE (1 << 21)

#define SUMH_RULE_INVENTORY_VERSION 1
#define SUMH_RULE_INVENTORY_PATH_MAX 4096
#define SUMH_INVENTORY_MAPPING 1
#define SUMH_INVENTORY_LEGACY_HIDE 2
#define SUMH_INVENTORY_MERGE 3
#define SUMH_INVENTORY_MAPS_MANAGED 4
#define SUMH_INVENTORY_MAPS_TEMPORARY 5
#define SUMH_INVENTORY_INJECT 6
#define SUMH_INVENTORY_USER_HIDE 7
/* Reserved categories: unsupported bits must not be interpreted as empty. */
#define SUMH_INVENTORY_OVERLAY_XATTR 8
#define SUMH_INVENTORY_KSTAT 9
#define SUMH_INVENTORY_VNODE 10
#define SUMH_INVENTORY_SUPPORTED_CATEGORIES 0x7fU
#define SUMH_INVENTORY_DEFINITIONS_COHERENT 1U
#define SUMH_INVENTORY_OWNER_UNATTRIBUTED 0
#define SUMH_INVENTORY_OWNER_USER_HIDE 1
#define SUMH_INVENTORY_OWNER_MANAGED_MAPS 2
#define SUMH_INVENTORY_ID_GENERATION_ORDINAL 1
#define SUMH_INVENTORY_ID_MANAGED_DEFINITION 2
#define SUMH_INVENTORY_LEGACY_STORAGE_MANAGED 1U

/* Native rule definitions, not filesystem/binding/observer health. path1 is
 * the visible path and path2 the backing path for mapping/merge; hide/inject
 * only use path1. Maps only use numeric fields and path2 (spoofed pathname).
 * Byte lengths exclude the terminating NUL; unused bytes are zero. No text
 * tokenization, UTF-8 normalization or pointer values. storage_managed is
 * legacy bookkeeping, not ownership authority. Unattributed is explicit.
 * Ordinal IDs are only category+instance+generation scoped, never mutation
 * handles. Managed-hide IDs/generation describe runtime definitions only;
 * binding jobs change neither the exported definition nor these IDs.
 */
struct sumh_rule_inventory_entry {
  __u32 category;
  __u32 owner;
  __u32 id_kind;
  __u32 type;
  __aligned_u64 id;
  __aligned_u64 definition_generation;
  __aligned_u64 target_ino;
  __aligned_u64 target_dev;
  __aligned_u64 spoofed_ino;
  __aligned_u64 spoofed_dev;
  __u32 path1_len;
  __u32 path2_len;
  __u32 flags;
  __u32 reserved;
  char path1[SUMH_RULE_INVENTORY_PATH_MAX];
  char path2[SUMH_RULE_INVENTORY_PATH_MAX];
};

/* GET: exact size/version, category, expected tokens and ordinal cursor;
 * everything else zero. First request: both tokens/cursor zero. Continuation
 * (including another category at cursor zero): both tokens nonzero/matching.
 * Category zero queries supported bits only, cursor zero. The returned
 * expected_* fields are zero; instance_id/generation identify the collection.
 * One bounded record per page, count 0/1, next_cursor=cursor+count, complete
 * 0/1 (including the final record). End cursor yields empty complete page;
 * past end is EINVAL. Stale token ESTALE, unsupported category EOPNOTSUPP;
 * oversized stored path EOVERFLOW, never truncation. Failure never mutates.
 * All covered definitions are serialized control mutations; no async binding
 * telemetry or current directory contents are included. Across categories,
 * reuse the same token. These observational IDs never authorize a clear.
 */
struct sumh_rule_inventory_arg {
  __u32 size;
  __u32 version;
  __u32 category;
  __u32 count;
  __aligned_u64 expected_instance_id;
  __aligned_u64 expected_generation;
  __aligned_u64 cursor;
  __aligned_u64 instance_id;
  __aligned_u64 generation;
  __aligned_u64 next_cursor;
  __u32 supported_categories;
  __u32 flags;
  __u32 complete;
  __u32 reserved;
  struct sumh_rule_inventory_entry entry;
};

#define SUMH_RUNTIME_STATE_VERSION 1
#define SUMH_RUNTIME_DEBUG 1
#define SUMH_RUNTIME_STEALTH 2
#define SUMH_RUNTIME_MOUNT_HIDE 3
#define SUMH_RUNTIME_MAPS_SPOOF 4
#define SUMH_RUNTIME_STATFS_SPOOF 5
#define SUMH_RUNTIME_KERNEL_BUILD 6
#define SUMH_RUNTIME_STATE_COUNT 6
#define SUMH_RUNTIME_CONTROL_SERIALIZED (1U << 0)
#define SUMH_RUNTIME_READINESS_SAMPLED (1U << 1)

/* Current kernel toggles, not persisted configuration or observer tests.
 * active requires configured and available machinery/dependencies; statfs
 * also needs mount hide configured and available. Debug logging and Kernel
 * Build's initial-UTS override do not depend on the global view switch.
 * degraded is configured but
 * not active. last_error is the latest targeted setter result, including a
 * failed user copy; success clears it. It is not a readiness error. Entry
 * flags and reserved are zero in version 1.
 */
struct sumh_runtime_feature_state {
  __u32 id;
  __u32 supported;
  __u32 configured;
  __u32 active;
  __u32 degraded;
  __s32 last_error;
  __u32 flags;
  __u32 reserved;
};

/* GET input: exact size, version 1, everything else zero. Output settings
 * and error records are serialized against control mutations; independently
 * changing readiness providers are sampled, never globally atomic. Header
 * flags always declare these two properties. generation is a nonzero control
 * sequence for attempted mutations/results, not an external readiness or
 * rule revision. Queries do not change it. instance_id is initialized at
 * bootstrap (randomized per module lifetime, incremented on reinitialization)
 * and distinguishes sequences; it is not persistent identity. CLEAR_ALL
 * retains debug and its error, resets the five other toggles/errors. Errors
 * start at zero in each instance; stopped providers return -ESHUTDOWN.
 */
struct sumh_runtime_state_arg {
  __u32 size;
  __u32 version;
  __u32 count;
  __u32 flags;
  __aligned_u64 generation;
  __aligned_u64 instance_id;
  __u32 mount_hide_mode;
  __u32 reserved[3];
  struct sumh_runtime_feature_state entries[SUMH_RUNTIME_STATE_COUNT];
};

/* Conditional update for the five simple toggles only (IDs 1..5). Kernel
 * Build requires its string ABI. Input size/version exact, enable 0/1,
 * reserved zero, mount_hide_mode NORMAL (zero) for all supported IDs.
 * expected_* must match the last GET. -ESTALE leaves settings, errors and
 * generation untouched. Malformed input/unrecognized IDs also leave them
 * untouched. A valid matched attempt records its result and advances once,
 * including unsupported enable and successful no-op. No global transaction
 * across other feature setters, rule sets or persisted config is implied.
 */
struct sumh_runtime_set_arg {
  __u32 size;
  __u32 version;
  __u32 id;
  __u32 enable;
  __aligned_u64 expected_generation;
  __aligned_u64 expected_instance_id;
  __u32 mount_hide_mode;
  __u32 reserved[3];
};

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

/* Conditional form of Kernel Build SET. Schema version1, exact size, enable
 * 0/1 and all reserved bytes zero. Strings use the legacy canonical contract.
 * Invalid input or stale expected tokens leaves settings/errors/sequence
 * untouched. A matched attempt records its result and advances generation
 * once, even for unavailable enable or successful no-op. This sequence only
 * tracks SUMH control writers, not third-party writes to init_uts_ns.
 */
struct sumh_kernel_build_cas_arg {
  __u32 size;
  __u32 version;
  __u32 enable;
  __u32 reserved;
  __aligned_u64 expected_generation;
  __aligned_u64 expected_instance_id;
  char release[SUMH_KERNEL_BUILD_MAX];
  char version_string[SUMH_KERNEL_BUILD_MAX];
  char reserved_tail[6];
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

#define SUMH_MAPS_DELETE_VERSION 1

/* Delete one temporary rule by its exact stored key; target_dev 0 identifies
 * the wildcard rule itself, never a request to delete another device's rule.
 * Both expected tokens must be nonzero and match the current inventory.
 * size/version must match; target_ino must fit the kernel's unsigned long and
 * target_dev uses the 32-bit MKDEV encoding. Zero ino/dev are valid keys.
 * Absent or managed-only keys return ENOENT. Stale tokens return ESTALE.
 * Failure leaves both sets and the generation unchanged; success publishes
 * once and advances the generation once. No output copy follows publication.
 * Cleanup is available independently of Maps filtering readiness.
 */
struct sumh_maps_delete_arg {
  __u32 size;
  __u32 version;
  __aligned_u64 expected_instance_id;
  __aligned_u64 expected_generation;
  __aligned_u64 target_ino;
  __aligned_u64 target_dev;
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
#define SUMH_IOC_SET_MOUNT_HIDE                                                \
  _IOW(SUMH_IOC_MAGIC, 25, struct sumh_mount_hide_arg)
#define SUMH_IOC_SET_MAPS_SPOOF                                                \
  _IOW(SUMH_IOC_MAGIC, 26, struct sumh_maps_spoof_arg)
#define SUMH_IOC_SET_STATFS_SPOOF                                              \
  _IOW(SUMH_IOC_MAGIC, 27, struct sumh_statfs_spoof_arg)
/* Commands 17 and 28 remain reserved for removed uname operations. */

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
#define SUMH_IOC_GET_RUNTIME_STATE                                             \
  _IOWR(SUMH_IOC_MAGIC, 49, struct sumh_runtime_state_arg)
#define SUMH_IOC_SET_RUNTIME_FEATURE                                           \
  _IOW(SUMH_IOC_MAGIC, 50, struct sumh_runtime_set_arg)
#define SUMH_IOC_SET_KERNEL_BUILD_CAS                                          \
  _IOW(SUMH_IOC_MAGIC, 51, struct sumh_kernel_build_cas_arg)
#define SUMH_IOC_GET_RULE_INVENTORY                                            \
  _IOWR(SUMH_IOC_MAGIC, 52, struct sumh_rule_inventory_arg)
#define SUMH_IOC_DELETE_TEMPORARY_MAPS                                         \
  _IOW(SUMH_IOC_MAGIC, 53, struct sumh_maps_delete_arg)

#endif /* _SUMH_UAPI_H */
