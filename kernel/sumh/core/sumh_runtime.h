#ifndef _SUMH_RUNTIME_H
#define _SUMH_RUNTIME_H

#include "sumh_base.h"
#include "infra/symbol_resolver.h"
#include <linux/namei.h>
#include <linux/file.h>
#include <linux/string.h>
#include <linux/security.h>

/* GKI exports this without a public namei.h declaration. */
int vfs_path_lookup(struct dentry *dentry, struct vfsmount *mnt,
		    const char *name, unsigned int flags, struct path *path);

#include <linux/anon_inodes.h>
#include <linux/bitmap.h>
#include <linux/capability.h>
#include <linux/fcntl.h>
#include <linux/kprobes.h>
#include <linux/limits.h>
#include <linux/llist.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/seq_file.h>
#include <linux/smp.h>
#include <linux/srcu.h>
#include <linux/stat.h>
#include <linux/task_work.h>
#include <linux/vmalloc.h>

#include "sumh_base.h"
#include "sumh_types.h"

extern bool sumh_enabled;
extern atomic_t sumh_rule_count;
extern atomic_t sumh_hide_count;
extern atomic_t sumh_spoof_kstat_count;

struct sumh_hook_stats {
	atomic64_t iop_getattr_entries;
	atomic64_t iop_getattr_spoofs;
	atomic64_t statfs_entries;
	atomic64_t statfs_spoofs;
	atomic64_t iterate_fop_entries;
	atomic64_t iterate_fop_wrapped;
	atomic64_t filldir_hidden;
	atomic64_t filldir_injected;
};

extern struct sumh_hook_stats sumh_hook_stats;

extern atomic_long_t sumh_ioctl_tgid;
extern struct kmem_cache *sumh_filldir_cache;

bool sumh_valid_kernel_addr(unsigned long addr);
#define sumh_lookup_name find_kernel_symbol_exact
#define sumh_lookup_name_quiet find_kernel_symbol_exact
static inline unsigned long sumh_lookup_callable(const char *name)
{
	return (unsigned long)ksu_lookup_symbol(name);
}
#define sumh_lookup_callable_quiet sumh_lookup_callable
dev_t sumh_vnode_device(void);
unsigned long sumh_vnode_source_ino(dev_t source_dev, u64 source_ino);
unsigned long sumh_vnode_vpath_ino(const char *vpath);
unsigned long sumh_vnode_ino_alloc(dev_t src_dev, u64 src_ino);
dev_t sumh_vnode_visible_dev(const char *visible_path);
u64 sumh_vnode_allocated(void);
unsigned int sumh_vnode_live(void);

extern bool sumh_stealth_enabled;

extern int sumh_mount_hide_vfsmnt_registered;
extern int sumh_mount_hide_mountinfo_registered;
extern int sumh_proc_proxy_registered;
extern int sumh_feature_enabled_mask;
extern int sumh_mount_hide_mode;
/* Lifecycle write lock at init; mutation mutex at shutdown. GET never init. */
void sumh_runtime_state_init(void);
void sumh_runtime_state_shutdown(void);
extern int sumh_fscap_kretprobe_registered;
extern int sumh_fscaps_enabled;
extern int sumh_device_sources_enabled;
extern dev_t sumh_system_dev;

/* notify_change first arg (idmap/userns) varies across kernel versions. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
extern int (*sumh_notify_change)(struct mnt_idmap *, struct dentry *,
				 struct iattr *, struct inode **);
#else
extern int (*sumh_notify_change)(struct user_namespace *, struct dentry *,
				 struct iattr *, struct inode **);
#endif
int sumh_vfs_getattr_unprojected(const struct path *path, struct kstat *stat,
				 u32 request_mask, unsigned int query_flags);
bool sumh_vfs_internal_current(void);
/*
 * Resolved get_vfs_caps_from_disk (security/commoncap.c).  Its leading
 * idmap/user_namespace argument varies by version exactly like notify_change;
 * absent (NULL) simply disables source-file-capability forwarding.  Use the
 * sumh_source_vfs_caps() wrapper, which supplies the source mount's idmap.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
extern int (*sumh_get_vfs_caps_from_disk)(struct mnt_idmap *,
					  const struct dentry *,
					  struct cpu_vfs_cap_data *);
#else
extern int (*sumh_get_vfs_caps_from_disk)(struct user_namespace *,
					  const struct dentry *,
					  struct cpu_vfs_cap_data *);
#endif
int sumh_source_vfs_caps(const struct path *src, struct cpu_vfs_cap_data *out);
/* Data-plane delegates for a char/blk/fifo source wrapper's special fops.
 * Stable signatures across all supported KMIs; NULL disables special read/write
 * (the op returns -EINVAL). */
extern ssize_t (*sumh_vfs_read)(struct file *, char __user *, size_t, loff_t *);
extern ssize_t (*sumh_vfs_write)(struct file *, const char __user *, size_t,
				 loff_t *);
extern char *(*sumh_d_absolute_path)(const struct path *, char *, int);
extern struct dentry *(*sumh_d_hash_and_lookup)(struct dentry *,
						const struct qstr *);
extern void *sumh_vfs_getxattr_addr;
extern void *sumh_vfs_listxattr_addr;
extern void *sumh_vfs_setxattr_addr;
extern void *sumh_vfs_removexattr_addr;
extern void *sumh_mnt_want_write_addr;
extern void *sumh_mnt_drop_write_addr;
/*
 * Directory-mutation delegates (Final Phase 1b): a directory-source vnode's
 * i_op create family forwards to these against the pinned source dir.  Their
 * leading idmap/user_namespace argument varies by version exactly like
 * notify_change; vfs_link takes the idmap as its SECOND argument.  Any may be
 * NULL (resolved quietly) — the vnode op returns -EOPNOTSUPP then.
 * sumh_lookup_one_len manufactures the source-side child dentry (caller holds
 * the source dir i_rwsem). Its internal signature is stable; on 6.16+ a typed
 * lookup_one adapter preserves the legacy identity-idmap permission checks.
 * Missing exact helper dependencies leave this capability NULL.
 */
extern struct dentry *(*sumh_lookup_one_len)(const char *, struct dentry *,
					     int);
/* Startup only, before views become callable; not a live refresh API. */
void sumh_resolve_lookup_helpers(void);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
extern int (*sumh_vfs_create)(struct mnt_idmap *, struct inode *,
			      struct dentry *, umode_t, bool);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
extern struct dentry *(*sumh_vfs_mkdir)(struct mnt_idmap *, struct inode *,
					struct dentry *, umode_t);
#else
extern int (*sumh_vfs_mkdir)(struct mnt_idmap *, struct inode *,
			     struct dentry *, umode_t);
#endif
extern int (*sumh_vfs_mknod)(struct mnt_idmap *, struct inode *,
			     struct dentry *, umode_t, dev_t);
extern int (*sumh_vfs_symlink)(struct mnt_idmap *, struct inode *,
			       struct dentry *, const char *);
extern int (*sumh_vfs_unlink)(struct mnt_idmap *, struct inode *,
			      struct dentry *, struct inode **);
extern int (*sumh_vfs_rmdir)(struct mnt_idmap *, struct inode *,
			     struct dentry *);
extern int (*sumh_vfs_link)(struct dentry *, struct mnt_idmap *, struct inode *,
			    struct dentry *, struct inode **);
#else
extern int (*sumh_vfs_create)(struct user_namespace *, struct inode *,
			      struct dentry *, umode_t, bool);
extern int (*sumh_vfs_mkdir)(struct user_namespace *, struct inode *,
			     struct dentry *, umode_t);
extern int (*sumh_vfs_mknod)(struct user_namespace *, struct inode *,
			     struct dentry *, umode_t, dev_t);
extern int (*sumh_vfs_symlink)(struct user_namespace *, struct inode *,
			       struct dentry *, const char *);
extern int (*sumh_vfs_unlink)(struct user_namespace *, struct inode *,
			      struct dentry *, struct inode **);
extern int (*sumh_vfs_rmdir)(struct user_namespace *, struct inode *,
			     struct dentry *);
extern int (*sumh_vfs_link)(struct dentry *, struct user_namespace *,
			    struct inode *, struct dentry *, struct inode **);
#endif
/*
 * vfs_rename uses struct renamedata from the kernel's fs.h. Its idmap/userns
 * members vary across supported kernels and are set by name at the call site.
 */
extern int (*sumh_vfs_rename)(struct renamedata *);
/* Public LSM secctx round-trip: copy a source inode's security context onto a
 * synthetic vnode's in-core SID without touching SELinux blob internals.  Both
 * may be NULL when the LSM/symbols are unavailable — callers must check. */
extern typeof(security_inode_getsecctx) *sumh_security_inode_getsecctx;
extern typeof(security_inode_notifysecctx) *sumh_security_inode_notifysecctx;
extern typeof(security_release_secctx) *sumh_security_release_secctx;
extern void (*sumh_free_inode_nonrcu_ptr)(struct inode *);

#endif /* _SUMH_RUNTIME_H */
