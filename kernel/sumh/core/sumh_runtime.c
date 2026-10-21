#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
#include <linux/mnt_idmapping.h>
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0) &&                           \
    !defined(arch_ftrace_get_regs)
#define arch_ftrace_get_regs(fregs) (NULL)
#endif
#include <linux/kprobes.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/jhash.h>
#include <linux/kdev_t.h>
#include <linux/hashtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fdtable.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/sched/task.h>
#include <linux/fs_struct.h>
#include <linux/dirent.h>
#include <linux/stat.h>
#include <linux/time.h>
#include <linux/anon_inodes.h>
#include <linux/fcntl.h>
#include <linux/smp.h>
#include <linux/mount.h>
#include <linux/xattr.h>
#include <linux/seq_file.h>
#include <uapi/linux/magic.h>

#include "sumh_runtime.h"
#include "sumh_store.h"
#include "sumh_dirhijack.h"
#include "sumh_path_policy.h"
#include "sumh_fop_override.h"
#include "sumh_vfs_view.h"
#include "sumh_vnode.h"

bool sumh_enabled;
atomic_t sumh_rule_count = ATOMIC_INIT(0);
atomic_t sumh_hide_count = ATOMIC_INIT(0);
struct sumh_hook_stats sumh_hook_stats;

#define SUMH_INTERNAL_VFS_HASH_BITS 5

struct sumh_internal_vfs_guard {
	struct task_struct *task;
	struct hlist_node node;
};

static DEFINE_HASHTABLE(sumh_internal_vfs_tasks, SUMH_INTERNAL_VFS_HASH_BITS);
static DEFINE_SPINLOCK(sumh_internal_vfs_lock);

bool sumh_vfs_internal_current(void)
{
	struct sumh_internal_vfs_guard *guard;
	unsigned long flags;
	bool found = false;

	/* Only current publishes its own stack marker. An empty bucket cannot
	 * contain it; avoid taking the global lock for ordinary getattr calls.
	 * Never follow a non-NULL pointer until the lock protects its lifetime.
	 */
	if (!READ_ONCE(
		sumh_internal_vfs_tasks[hash_min((unsigned long)current,
						 SUMH_INTERNAL_VFS_HASH_BITS)]
		    .first))
		return false;

	spin_lock_irqsave(&sumh_internal_vfs_lock, flags);
	hash_for_each_possible(sumh_internal_vfs_tasks, guard, node,
			       (unsigned long)current)
	{
		if (guard->task == current) {
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&sumh_internal_vfs_lock, flags);
	return found;
}

int SUMH_NOCFI sumh_vfs_getattr_unprojected(const struct path *path,
					    struct kstat *stat,
					    u32 request_mask,
					    unsigned int query_flags)
{
	struct sumh_internal_vfs_guard guard = {.task = current};
	unsigned long flags;
	int ret;

	/* Internal source inspection must not be fed back through SUMH's own
	 * legacy kstat projection.  A stack-backed, lock-protected task marker
	 * is safe across sleeping filesystem getattr implementations and nests.
	 */
	spin_lock_irqsave(&sumh_internal_vfs_lock, flags);
	hash_add(sumh_internal_vfs_tasks, &guard.node, (unsigned long)current);
	spin_unlock_irqrestore(&sumh_internal_vfs_lock, flags);
	ret = vfs_getattr(path, stat, request_mask, query_flags);
	spin_lock_irqsave(&sumh_internal_vfs_lock, flags);
	hash_del(&guard.node);
	spin_unlock_irqrestore(&sumh_internal_vfs_lock, flags);
	return ret;
}

/*
 * Parse the source file's on-disk capabilities via the kernel's own reader, so
 * an exec of a redirected setcap binary keeps its file capabilities.  Sleepable
 * (reads security.capability): call from vnode-create context, never from a
 * kprobe handler.  @out is filled and 0 returned only when the source carries
 * caps; any error (incl. -ENODATA) leaves @out untouched.
 */
int SUMH_NOCFI sumh_source_vfs_caps(const struct path *src,
				    struct cpu_vfs_cap_data *out)
{
	if (!src || !src->dentry || !src->mnt || !out ||
	    !sumh_get_vfs_caps_from_disk)
		return -EOPNOTSUPP;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	return sumh_get_vfs_caps_from_disk(mnt_idmap(src->mnt), src->dentry,
					   out);
#else
	return sumh_get_vfs_caps_from_disk(mnt_user_ns(src->mnt), src->dentry,
					   out);
#endif
}

/* Metadata-only device namespace for virtual nodes.  No real superblock is
 * registered: pathname hooks, virtual descriptors and proc projections all
 * publish this identity from the rule snapshot. */
#define SUMH_VNODE_MINOR 0x4b5
static atomic64_t sumh_vnode_allocated_count = ATOMIC64_INIT(0);
static atomic_t sumh_vnode_live_count = ATOMIC_INIT(0);

/* Per-rule synthetic inode allocator.  Each rule source is assigned a stable,
 * plausible-magnitude (u32-range, above typical real inode usage) inode number
 * at install time; every projection surface (getattr, getdents, /proc/maps,
 * syscall stat handlers) resolves the SAME value through
 * sumh_vnode_source_ino() so stat/ls -i/maps never disagree.  Values stay <
 * 2^32 so an f2fs/ext4 sibling comparison sees a realistic inode, not the old
 * 19-digit bit-63 tell. */
#define SUMH_VNODE_INO_BASE 0xC0000000UL
#define SUMH_VNODE_INO_TOP 0xFFFFFF00UL
#define SUMH_VNODE_INO_SPAN (SUMH_VNODE_INO_TOP - SUMH_VNODE_INO_BASE)

struct sumh_ino_map_entry {
	dev_t src_dev;
	u64 src_ino;
	unsigned long alloc_ino;
	struct hlist_node node;
	struct rcu_head rcu;
};
static DEFINE_HASHTABLE(sumh_ino_map, SUMH_HASH_BITS);
static atomic64_t sumh_ino_next = ATOMIC64_INIT(SUMH_VNODE_INO_BASE);
static atomic_t sumh_ino_map_count = ATOMIC_INIT(0);

/*
 * Resolve the synthetic inode for a source (dev, ino).  LOOKUP-ONLY and
 * RCU/atomic-safe (never allocates), so it is callable from the getattr fast
 * path.  A rule source hits the allocator map and returns its stable assigned
 * value; a dynamic non-rule source (./.., non-materialized merge enumeration)
 * misses and gets a deterministic plausible-range hash in the same window, so
 * repeated calls stay stable without growing the map.
 */
unsigned long sumh_vnode_source_ino(dev_t source_dev, u64 source_ino)
{
	struct sumh_ino_map_entry *e;
	u64 dev = (u64)source_dev;
	u32 words[4] = {
	    (u32)dev,
	    (u32)(dev >> 32),
	    (u32)source_ino,
	    (u32)(source_ino >> 32),
	};
	u64 h;

	if (source_ino) {
		rcu_read_lock();
		hlist_for_each_entry_rcu(
		    e, &sumh_ino_map[hash_min(source_ino, SUMH_HASH_BITS)],
		    node)
		{
			if (e->src_ino == source_ino &&
			    e->src_dev == source_dev) {
				unsigned long ino = e->alloc_ino;

				rcu_read_unlock();
				return ino;
			}
		}
		rcu_read_unlock();
	}

	h = ((u64)jhash2(words, ARRAY_SIZE(words), 0x4b617375) << 32) |
	    jhash2(words, ARRAY_SIZE(words), 0x6d695646);
	return SUMH_VNODE_INO_BASE + (unsigned long)(h % SUMH_VNODE_INO_SPAN);
}

/*
 * Synthetic inode for a pure-virtual directory node (F_VIRTUAL_DIR), which has
 * no source (dev,ino) to project.  Derives a stable, plausible-range u32 from
 * the visible path so stat and getdents agree across lookups.  Shares the
 * [BASE, BASE+SPAN) window with sumh_vnode_source_ino; a collision with a
 * real allocated ino is possible but low-probability and non-fatal (same
 * residual as the source-ino scheme).
 */
unsigned long sumh_vnode_vpath_ino(const char *vpath)
{
	u32 h;

	if (!vpath)
		return SUMH_VNODE_INO_BASE;
	h = jhash(vpath, (u32)strlen(vpath), 0x76746F70 /* "vtop" */);
	return SUMH_VNODE_INO_BASE + (unsigned long)(h % SUMH_VNODE_INO_SPAN);
}

/*
 * Allocate (or return the existing) stable synthetic inode for a rule source.
 * SLEEPABLE, install-time only, serialized by sumh_config_mutex (like the
 * spoof_kstat table), so the lookup-then-insert needs no extra lock. Idempotent
 * per (src_dev, src_ino).  On OOM falls back to the deterministic hash so a
 * rule always gets a usable identity.
 */
unsigned long sumh_vnode_ino_alloc(dev_t src_dev, u64 src_ino)
{
	struct sumh_ino_map_entry *e;
	u64 seq;

	if (!src_ino)
		return sumh_vnode_source_ino(src_dev, src_ino);

	rcu_read_lock();
	hlist_for_each_entry_rcu(
	    e, &sumh_ino_map[hash_min(src_ino, SUMH_HASH_BITS)], node)
	{
		if (e->src_ino == src_ino && e->src_dev == src_dev) {
			unsigned long ino = e->alloc_ino;

			rcu_read_unlock();
			return ino;
		}
	}
	rcu_read_unlock();

	e = kmalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return sumh_vnode_source_ino(src_dev, src_ino);
	seq =
	    (u64)atomic64_inc_return(&sumh_ino_next) - SUMH_VNODE_INO_BASE - 1;
	e->src_dev = src_dev;
	e->src_ino = src_ino;
	e->alloc_ino =
	    SUMH_VNODE_INO_BASE + (unsigned long)(seq % SUMH_VNODE_INO_SPAN);
	hlist_add_head_rcu(&e->node,
			   &sumh_ino_map[hash_min(src_ino, SUMH_HASH_BITS)]);
	atomic_inc(&sumh_ino_map_count);
	return e->alloc_ino;
}

dev_t sumh_vnode_device(void)
{
	return MKDEV(0, SUMH_VNODE_MINOR);
}

/*
 * Scheme A: publish the device a real file at the visible path would report,
 * so a virtual node's st_dev matches its sibling real files instead of an
 * anonymous vnode minor (which is itself an odd-dev-out tell).  Resolves the
 * visible path, or its parent directory when nothing is backed there yet, and
 * falls back to the captured /system dev, then to the vnode minor, so a valid
 * non-zero device is always published.  Sleepable context only (path lookup).
 */
SUMH_NOCFI dev_t sumh_vnode_visible_dev(const char *visible_path)
{
	struct path p;
	dev_t dev = READ_ONCE(sumh_system_dev);
	char *parent;
	char *slash;

	if (!visible_path || !*visible_path)
		goto out;
	if (kern_path(visible_path, LOOKUP_FOLLOW, &p) == 0) {
		if (p.dentry && p.dentry->d_sb)
			dev = p.dentry->d_sb->s_dev;
		path_put(&p);
		goto out;
	}
	parent = kstrdup(visible_path, GFP_KERNEL);
	if (parent) {
		slash = strrchr(parent, '/');
		if (slash && slash != parent) {
			*slash = '\0';
			if (kern_path(parent, LOOKUP_FOLLOW, &p) == 0) {
				if (p.dentry && p.dentry->d_sb)
					dev = p.dentry->d_sb->s_dev;
				path_put(&p);
			}
		}
		kfree(parent);
	}
out:
	if (!dev)
		dev = sumh_vnode_device();
	return dev;
}

u64 sumh_vnode_allocated(void)
{
	return (u64)atomic64_read(&sumh_vnode_allocated_count);
}

unsigned int sumh_vnode_live(void)
{
	return (unsigned int)atomic_read(&sumh_vnode_live_count);
}

atomic_long_t sumh_ioctl_tgid = ATOMIC_LONG_INIT(0);

struct kmem_cache *sumh_filldir_cache;

DEFINE_HASHTABLE(sumh_paths, SUMH_HASH_BITS);
DEFINE_HASHTABLE(sumh_hide_paths, SUMH_HASH_BITS);
DEFINE_HASHTABLE(sumh_inject_dirs, SUMH_HASH_BITS);
DEFINE_HASHTABLE(sumh_xattr_sbs, SUMH_HASH_BITS);
DEFINE_HASHTABLE(sumh_merge_dirs, SUMH_HASH_BITS);

DEFINE_HASHTABLE(sumh_spoof_kstat_path, SUMH_HASH_BITS);
DEFINE_HASHTABLE(sumh_spoof_kstat_ino, SUMH_HASH_BITS);
atomic_t sumh_spoof_kstat_count = ATOMIC_INIT(0);

DEFINE_MUTEX(sumh_config_mutex);

bool sumh_debug_enabled;
bool sumh_stealth_enabled;

int sumh_mount_hide_vfsmnt_registered;
int sumh_mount_hide_mountinfo_registered;
int sumh_proc_proxy_registered;
int sumh_feature_enabled_mask;
int sumh_mount_hide_mode = SUMH_MOUNT_HIDE_MODE_NORMAL;
int sumh_fscap_kretprobe_registered;
int sumh_fscaps_enabled = 1;
int sumh_device_sources_enabled = 1;

DECLARE_BITMAP(sumh_path_bloom, SUMH_BLOOM_SIZE);
DECLARE_BITMAP(sumh_hide_bloom, SUMH_BLOOM_SIZE);

dev_t sumh_system_dev;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
int (*sumh_notify_change)(struct mnt_idmap *, struct dentry *, struct iattr *,
			  struct inode **);
#else
int (*sumh_notify_change)(struct user_namespace *, struct dentry *,
			  struct iattr *, struct inode **);
#endif
ssize_t (*sumh_vfs_read)(struct file *, char __user *, size_t, loff_t *);
ssize_t (*sumh_vfs_write)(struct file *, const char __user *, size_t, loff_t *);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
int (*sumh_get_vfs_caps_from_disk)(struct mnt_idmap *, const struct dentry *,
				   struct cpu_vfs_cap_data *);
#else
int (*sumh_get_vfs_caps_from_disk)(struct user_namespace *,
				   const struct dentry *,
				   struct cpu_vfs_cap_data *);
#endif
typeof(security_inode_getsecctx) *sumh_security_inode_getsecctx;
typeof(security_inode_notifysecctx) *sumh_security_inode_notifysecctx;
typeof(security_release_secctx) *sumh_security_release_secctx;
char *(*sumh_d_absolute_path)(const struct path *, char *, int);
struct dentry *(*sumh_d_hash_and_lookup)(struct dentry *, const struct qstr *);
void *sumh_vfs_getxattr_addr;
void *sumh_vfs_listxattr_addr;
void *sumh_vfs_setxattr_addr;
void *sumh_vfs_removexattr_addr;
void *sumh_mnt_want_write_addr;
void *sumh_mnt_drop_write_addr;

/* SUMH_LOOKUP_COMPAT_BEGIN */
struct dentry *(*sumh_lookup_one_len)(const char *, struct dentry *, int);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
typedef struct dentry *(*sumh_lookup_one_qstr_fn)(struct mnt_idmap *,
						  struct qstr *,
						  struct dentry *);

_Static_assert(__builtin_types_compatible_p(typeof(&lookup_one),
					    sumh_lookup_one_qstr_fn),
	       "SUMH requires the lookup_one(idmap, qstr, parent) prototype");
_Static_assert(__builtin_types_compatible_p(typeof(nop_mnt_idmap),
					    struct mnt_idmap),
	       "SUMH requires nop_mnt_idmap to be an mnt_idmap object");

static typeof(lookup_one) *sumh_lookup_one_qstr;
static typeof(&nop_mnt_idmap) sumh_lookup_identity_idmap;

/* Preserve lookup_one_len's identity idmap and caller-owned parent lock. */
static struct dentry *sumh_lookup_one_len_compat(const char *name,
						 struct dentry *parent, int len)
{
	struct qstr component = {};

	if (len < 0)
		return ERR_PTR(-EINVAL);
	if (!sumh_lookup_one_qstr || !sumh_lookup_identity_idmap)
		return ERR_PTR(-EOPNOTSUPP);
	component.name = (const unsigned char *)name;
	component.len = len;
	return sumh_lookup_one_qstr(sumh_lookup_identity_idmap, &component,
				    parent);
}
#else
_Static_assert(__builtin_types_compatible_p(typeof(&lookup_one_len),
					    typeof(sumh_lookup_one_len)),
	       "SUMH requires the lookup_one_len(name, parent, int) prototype");
#endif

/* Startup only, before view publication; never refresh live dependencies. */
void sumh_resolve_lookup_helpers(void)
{
	sumh_lookup_one_len = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
	sumh_lookup_one_qstr = NULL;
	sumh_lookup_identity_idmap = NULL;
	sumh_lookup_one_qstr =
	    (typeof(&lookup_one))find_kernel_symbol_exact("lookup_one");
	sumh_lookup_identity_idmap =
	    (typeof(&nop_mnt_idmap))find_kernel_symbol_exact("nop_mnt_idmap");
	if (!sumh_lookup_one_qstr || !sumh_lookup_identity_idmap) {
		pr_warn("sumh: directory lookup unavailable (lookup_one=%s, "
			"nop_mnt_idmap=%s)\n",
			sumh_lookup_one_qstr ? "present" : "missing",
			sumh_lookup_identity_idmap ? "present" : "missing");
		return;
	}
	sumh_lookup_one_len = sumh_lookup_one_len_compat;
#else
	sumh_lookup_one_len =
	    (typeof(&lookup_one_len))find_kernel_symbol_exact("lookup_one_len");
	if (!sumh_lookup_one_len)
		pr_warn("sumh: directory lookup unavailable (lookup_one_len "
			"missing)\n");
#endif
}
/* SUMH_LOOKUP_COMPAT_END */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
int (*sumh_vfs_create)(struct mnt_idmap *, struct inode *, struct dentry *,
		       umode_t, bool);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
struct dentry *(*sumh_vfs_mkdir)(struct mnt_idmap *, struct inode *,
				 struct dentry *, umode_t);
#else
int (*sumh_vfs_mkdir)(struct mnt_idmap *, struct inode *, struct dentry *,
		      umode_t);
#endif
int (*sumh_vfs_mknod)(struct mnt_idmap *, struct inode *, struct dentry *,
		      umode_t, dev_t);
int (*sumh_vfs_symlink)(struct mnt_idmap *, struct inode *, struct dentry *,
			const char *);
int (*sumh_vfs_unlink)(struct mnt_idmap *, struct inode *, struct dentry *,
		       struct inode **);
int (*sumh_vfs_rmdir)(struct mnt_idmap *, struct inode *, struct dentry *);
int (*sumh_vfs_link)(struct dentry *, struct mnt_idmap *, struct inode *,
		     struct dentry *, struct inode **);
#else
int (*sumh_vfs_create)(struct user_namespace *, struct inode *, struct dentry *,
		       umode_t, bool);
int (*sumh_vfs_mkdir)(struct user_namespace *, struct inode *, struct dentry *,
		      umode_t);
int (*sumh_vfs_mknod)(struct user_namespace *, struct inode *, struct dentry *,
		      umode_t, dev_t);
int (*sumh_vfs_symlink)(struct user_namespace *, struct inode *,
			struct dentry *, const char *);
int (*sumh_vfs_unlink)(struct user_namespace *, struct inode *, struct dentry *,
		       struct inode **);
int (*sumh_vfs_rmdir)(struct user_namespace *, struct inode *, struct dentry *);
int (*sumh_vfs_link)(struct dentry *, struct user_namespace *, struct inode *,
		     struct dentry *, struct inode **);
#endif
int (*sumh_vfs_rename)(struct renamedata *);
void (*sumh_free_inode_nonrcu_ptr)(struct inode *);

bool sumh_valid_kernel_addr(unsigned long addr)
{
	if (!addr)
		return false;
	if (IS_ERR_VALUE(addr))
		return false;
#if defined(CONFIG_64BIT)
	return (addr & (1UL << 63)) != 0;
#else
	return addr >= PAGE_OFFSET;
#endif
}

/* Borrowed source objects: callers resolve first, then retain their own
 * references until this helper has acquired the entry's references. No source
 * pathname lookup is permitted here. The visible-path metadata probe is an
 * independent template observation, not part of source identity. */
static int SUMH_NOCFI sumh_entry_capture_paths(
    struct sumh_entry *entry, const struct path *source,
    const struct path *nofollow_source, const char *source_path)
{
	struct path nofollow_path;
	struct inode *nofollow_inode;
	struct kstat visible_kstat;
	struct path visible_path;
	struct inode *visible_inode;
	struct inode *inode;
	struct path path;

	if (!entry || !source || !source->dentry || !source->mnt)
		return -EINVAL;
	if (entry->source_path_valid)
		return -EALREADY;

	path = *source;
	inode = d_inode(path.dentry);
	if (!inode)
		return -ENOENT;
	if (S_ISLNK(inode->i_mode))
		return -ELOOP;

	/* char/blk/fifo sources are served by a SUMH_VNODE_F_SPECIAL wrapper:
	 * the vnode is minted S_IFREG (so it passes may_open_dev on the visible
	 * nodev mount) while its .open delegates to the pinned source via
	 * dentry_open (which bypasses may_open), and getattr projects the real
	 * type+rdev.  A socket has no openable form (sock_no_open -> -ENXIO on
	 * the real source too), so it is always rejected.  When device-source
	 * support is disabled, reject all four rather than admit a rule with no
	 * working transport. */
	if (S_ISSOCK(inode->i_mode) ||
	    (!sumh_device_sources_enabled &&
	     (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode) ||
	      S_ISFIFO(inode->i_mode)))) {
		const char *kind = S_ISCHR(inode->i_mode)   ? "character-device"
				   : S_ISBLK(inode->i_mode) ? "block-device"
				   : S_ISFIFO(inode->i_mode) ? "fifo"
							     : "socket";
		pr_warn("sumh: rejecting %s source '%s': %s\n", kind,
			source_path,
			S_ISSOCK(inode->i_mode)
			    ? "a socket has no openable vnode form"
			    : "char/blk/fifo source support is disabled");
		return -EOPNOTSUPP;
	}

	entry->source_path = path;
	path_get(&entry->source_path);
	if (sumh_d_absolute_path) {
		char *buffer = kmalloc(SUMH_MAX_LEN_PATHNAME, GFP_KERNEL);

		if (buffer) {
			char *canonical = sumh_d_absolute_path(
			    &path, buffer, SUMH_MAX_LEN_PATHNAME);

			if (!IS_ERR_OR_NULL(canonical) && canonical[0] == '/')
				entry->source_canonical =
				    kstrdup(canonical, GFP_KERNEL);
			kfree(buffer);
		}
	}
	entry->source_inode = inode;
	entry->source_ino = inode->i_ino;
	entry->source_dev = inode->i_sb ? inode->i_sb->s_dev : 0;
	entry->source_mode = inode->i_mode;
	entry->source_uid = inode->i_uid;
	entry->source_gid = inode->i_gid;
	entry->source_size = i_size_read(inode);
	memset(&entry->source_stat, 0, sizeof(entry->source_stat));
	if (sumh_vfs_getattr_unprojected(&path, &entry->source_stat,
					 STATX_BASIC_STATS | STATX_BTIME,
					 AT_STATX_SYNC_AS_STAT) == 0) {
		entry->source_stat_valid = true;
	} else {
		entry->source_stat.result_mask =
		    STATX_TYPE | STATX_MODE | STATX_NLINK | STATX_UID |
		    STATX_GID | STATX_INO | STATX_SIZE | STATX_BLOCKS;
		entry->source_stat.dev = entry->source_dev;
		entry->source_stat.ino = entry->source_ino;
		entry->source_stat.mode = entry->source_mode;
		entry->source_stat.nlink = inode->i_nlink;
		entry->source_stat.uid = entry->source_uid;
		entry->source_stat.gid = entry->source_gid;
		entry->source_stat.rdev = inode->i_rdev;
		entry->source_stat.size = entry->source_size;
		entry->source_stat.blocks = inode->i_blocks;
		entry->source_stat.blksize = 1U << inode->i_blkbits;
		entry->source_stat_valid = true;
	}
	entry->visible_stat = entry->source_stat;
	entry->visible_stat_valid = entry->source_stat_valid;

	/* Preserve inode identity across rename and hard links without
	 * publishing the captured filesystem's raw dev/ino pair. */
	entry->visible_ino =
	    sumh_vnode_ino_alloc(entry->source_dev, entry->source_ino);
	entry->visible_dev = sumh_vnode_visible_dev(entry->src);
	if (entry->src &&
	    kern_path(entry->src, LOOKUP_FOLLOW, &visible_path) == 0) {
		visible_inode = d_inode(visible_path.dentry);
		if (visible_inode) {
			entry->preserve_visible_metadata = true;
			memset(&visible_kstat, 0, sizeof(visible_kstat));
			if (sumh_vfs_getattr_unprojected(
				&visible_path, &visible_kstat,
				STATX_BASIC_STATS | STATX_BTIME,
				AT_STATX_SYNC_AS_STAT) == 0) {
				entry->visible_stat = visible_kstat;
				/* The virtual node lives at the visible path,
				 * but its data and file kind come from the
				 * captured source. Keep visible ownership,
				 * permissions and timestamps while publishing
				 * the source type, size and backing allocation
				 * semantics. */
				entry->visible_stat.mode =
				    (entry->source_stat.mode & S_IFMT) |
				    (visible_kstat.mode & ~S_IFMT);
				entry->visible_stat.rdev =
				    entry->source_stat.rdev;
				entry->visible_stat.size =
				    entry->source_stat.size;
				entry->visible_stat.blocks =
				    entry->source_stat.blocks;
				entry->visible_stat.blksize =
				    entry->source_stat.blksize;
				entry->visible_stat.result_mask |=
				    entry->source_stat.result_mask &
				    (STATX_TYPE | STATX_MODE | STATX_SIZE |
				     STATX_BLOCKS);
				entry->visible_stat_valid = true;
			} else {
				entry->visible_stat.mode =
				    (entry->source_stat.mode & S_IFMT) |
				    (visible_inode->i_mode & ~S_IFMT);
				entry->visible_stat.nlink =
				    visible_inode->i_nlink;
				entry->visible_stat.uid = visible_inode->i_uid;
				entry->visible_stat.gid = visible_inode->i_gid;
			}
		}
		path_put(&visible_path);
	}
	entry->visible_stat.ino = entry->visible_ino;
	entry->visible_stat.dev = entry->visible_dev;
	if (nofollow_source) {
		nofollow_path = *nofollow_source;
		nofollow_inode = d_inode(nofollow_path.dentry);
		if (nofollow_inode && S_ISLNK(nofollow_inode->i_mode)) {
			entry->source_nofollow_path = nofollow_path;
			path_get(&entry->source_nofollow_path);
			entry->source_nofollow_mode = nofollow_inode->i_mode;
			memset(&entry->source_nofollow_stat, 0,
			       sizeof(entry->source_nofollow_stat));
			if (sumh_vfs_getattr_unprojected(
				&nofollow_path, &entry->source_nofollow_stat,
				STATX_BASIC_STATS | STATX_BTIME,
				AT_STATX_SYNC_AS_STAT) == 0) {
				entry->source_nofollow_stat_valid = true;
			} else {
				entry->source_nofollow_stat.result_mask =
				    STATX_TYPE | STATX_MODE | STATX_NLINK |
				    STATX_UID | STATX_GID | STATX_INO |
				    STATX_SIZE | STATX_BLOCKS;
				entry->source_nofollow_stat.dev =
				    nofollow_inode->i_sb
					? nofollow_inode->i_sb->s_dev
					: 0;
				entry->source_nofollow_stat.ino =
				    nofollow_inode->i_ino;
				entry->source_nofollow_stat.mode =
				    nofollow_inode->i_mode;
				entry->source_nofollow_stat.nlink =
				    nofollow_inode->i_nlink;
				entry->source_nofollow_stat.uid =
				    nofollow_inode->i_uid;
				entry->source_nofollow_stat.gid =
				    nofollow_inode->i_gid;
				entry->source_nofollow_stat.size =
				    i_size_read(nofollow_inode);
				entry->source_nofollow_stat.blocks =
				    nofollow_inode->i_blocks;
				entry->source_nofollow_stat.blksize =
				    1U << nofollow_inode->i_blkbits;
				entry->source_nofollow_stat_valid = true;
			}
			entry->nofollow_visible_ino = sumh_vnode_ino_alloc(
			    nofollow_inode->i_sb ? nofollow_inode->i_sb->s_dev
						 : 0,
			    nofollow_inode->i_ino);
			entry->nofollow_visible_dev =
			    sumh_vnode_visible_dev(entry->src);
			entry->source_nofollow_stat.ino =
			    entry->nofollow_visible_ino;
			entry->source_nofollow_stat.dev =
			    entry->nofollow_visible_dev;
			entry->source_nofollow_path_valid = true;
		}
	}
	entry->source_path_valid = true;
	atomic64_inc(&sumh_vnode_allocated_count);
	atomic_inc(&sumh_vnode_live_count);
	return 0;
}

int SUMH_NOCFI sumh_entry_capture_resolved(struct sumh_entry *entry,
					   const struct path *source)
{
	return sumh_entry_capture_paths(
	    entry, source, NULL,
	    entry && entry->target ? entry->target : "<held source>");
}

int SUMH_NOCFI sumh_entry_capture_source_checked(
    struct sumh_entry *entry, const char *source_path,
    const struct path *expected_leaf)
{
	struct path leaf, followed, rechecked_leaf;
	struct inode *inode;
	int ret;

	if (!entry || !source_path || !*source_path ||
	    (expected_leaf && (!expected_leaf->dentry || !expected_leaf->mnt)))
		return -EINVAL;
	if (entry->source_path_valid)
		return -EALREADY;

	/* Resolve a non-link exactly once. In particular, replacement of the
	 * pathname after this lookup cannot pair a new inode with old metadata.
	 */
	ret = kern_path(source_path, 0, &leaf);
	if (ret)
		return ret;
	if (expected_leaf && !path_equal(&leaf, expected_leaf)) {
		ret = -ESTALE;
		goto out_leaf;
	}
	inode = d_inode(leaf.dentry);
	if (!inode) {
		ret = -ENOENT;
		goto out_leaf;
	}
	if (!S_ISLNK(inode->i_mode)) {
		ret = sumh_entry_capture_resolved(entry, &leaf);
		goto out_leaf;
	}

	/* Keep the native pathwalk for links: root, .., mount, magic-link and
	 * security semantics must not become a lookup rooted at the held
	 * parent. Recheck before capture to reject a changed leaf still
	 * observable after following. These lookups do not form an atomic or
	 * ABA-safe pair; they do not freeze dynamic links, their target chain
	 * or later changes. */
	ret = kern_path(source_path, LOOKUP_FOLLOW, &followed);
	if (ret)
		goto out_leaf;
	ret = kern_path(source_path, 0, &rechecked_leaf);
	if (ret)
		goto out_followed;
	if (!path_equal(&leaf, &rechecked_leaf))
		ret = -ESTALE;
	path_put(&rechecked_leaf);
	if (!ret)
		ret = sumh_entry_capture_paths(entry, &followed, &leaf,
					       source_path);
out_followed:
	path_put(&followed);
out_leaf:
	path_put(&leaf);
	return ret;
}

int SUMH_NOCFI sumh_entry_capture_source(struct sumh_entry *entry,
					 const char *source_path)
{
	return sumh_entry_capture_source_checked(entry, source_path, NULL);
}

void SUMH_NOCFI sumh_entry_release_source(struct sumh_entry *entry)
{
	if (!entry || !entry->source_path_valid)
		return;

	entry->source_path_valid = false;
	atomic_dec(&sumh_vnode_live_count);
	path_put(&entry->source_path);
	memset(&entry->source_path, 0, sizeof(entry->source_path));
	if (entry->source_nofollow_path_valid) {
		entry->source_nofollow_path_valid = false;
		path_put(&entry->source_nofollow_path);
		memset(&entry->source_nofollow_path, 0,
		       sizeof(entry->source_nofollow_path));
	}
	entry->source_inode = NULL;
}

static LLIST_HEAD(sumh_retired_entries);
static LLIST_HEAD(sumh_retired_hides);
static LLIST_HEAD(sumh_retired_merges);
static LLIST_HEAD(sumh_retired_inject_inodes);
static void sumh_store_free_workfn(struct work_struct *work);
static DECLARE_WORK(sumh_store_free_work, sumh_store_free_workfn);

/* SUMH_INJECT_CLAIMS_BEGIN */
/* Only config-serialized writers dereference these aggregates. Unlike the
 * RCU string index, their final release needs process context, not a grace
 * period. Each aggregate owns references independent of the FOP cache. */
struct sumh_inject_inode {
	struct inode *inode;
	struct super_block *sb;
	unsigned int users;
	struct hlist_node node;
	struct llist_node free_node;
};

struct sumh_inject_claim {
	struct sumh_inject_entry *key;
	struct sumh_inject_inode *binding;
	struct sumh_inject_entry *spare_key;
	struct sumh_inject_inode *spare_binding;
	bool attached;
};

static DEFINE_HASHTABLE(sumh_inject_inodes, SUMH_HASH_BITS);

static void sumh_inject_inode_free(struct sumh_inject_inode *binding)
{
	if (!binding)
		return;
	/* The inode's filesystem must stay active throughout its final put. */
	iput(binding->inode);
	deactivate_super(binding->sb);
	kfree(binding);
}

void sumh_inject_finish_prepare(struct sumh_inject_claim *claim)
{
	if (!claim)
		return;
	if (claim->spare_key) {
		kfree(claim->spare_key->dir);
		kfree(claim->spare_key);
		claim->spare_key = NULL;
	}
	sumh_inject_inode_free(claim->spare_binding);
	claim->spare_binding = NULL;
}

void sumh_inject_destroy(struct sumh_inject_claim *claim)
{
	if (!claim || WARN_ON_ONCE(claim->attached))
		return;
	/* Logical withdrawal belongs to the mutation, never the RCU worker. */
	sumh_inject_finish_prepare(claim);
	kfree(claim);
}

static int SUMH_NOCFI sumh_inject_prepare_held(const char *key,
					       const struct path *path,
					       struct sumh_inject_claim **out)
{
	struct sumh_inject_claim *claim;
	struct sumh_inject_inode *binding;
	struct inode *inode;
	struct super_block *sb;
	int ret = -ENOMEM;

	claim = kzalloc(sizeof(*claim), GFP_KERNEL);
	if (!claim)
		return -ENOMEM;
	claim->spare_key = kzalloc(sizeof(*claim->spare_key), GFP_KERNEL);
	if (!claim->spare_key)
		goto fail;
	claim->spare_key->dir = kstrdup(key, GFP_KERNEL);
	if (!claim->spare_key->dir)
		goto fail;
	if (!path)
		goto ready;
	if (!path->mnt || !path->dentry) {
		ret = -EINVAL;
		goto fail;
	}
	inode = d_inode(path->dentry);
	if (!inode || !S_ISDIR(inode->i_mode)) {
		ret = -ENOTDIR;
		goto fail;
	}
	if (sumh_vnode_is_ours(inode)) {
		ret = -EOPNOTSUPP;
		goto fail;
	}
	if (!inode->i_mapping || !inode->i_sb) {
		ret = -EINVAL;
		goto fail;
	}
	binding = kzalloc(sizeof(*binding), GFP_KERNEL);
	if (!binding)
		goto fail;
	sb = inode->i_sb;
	/* Native caller path protects this acquisition. Never revive a dead or
	 * not-yet-born filesystem; retain no mount reference in the claim. */
	down_read(&sb->s_umount);
	if (!(sb->s_flags & SB_BORN) || !atomic_inc_not_zero(&sb->s_active)) {
		up_read(&sb->s_umount);
		kfree(binding);
		ret = -ESHUTDOWN;
		goto fail;
	}
	up_read(&sb->s_umount);
	ihold(inode);
	binding->inode = inode;
	binding->sb = sb;
	claim->spare_binding = binding;
	/* FOP may take s_umount itself. Its independent cache can safely
	 * outlive a later preparation failure or the last logical injection
	 * owner. */
	ret = sumh_fop_install(inode);
	if (ret)
		goto fail;
ready:
	*out = claim;
	return 0;
fail:
	sumh_inject_destroy(claim);
	return ret;
}

int sumh_inject_prepare_observed(const char *key, const struct path *parent,
				 enum sumh_inject_missing missing,
				 struct sumh_inject_claim **out)
{
	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!key || key[0] != '/' ||
	    (missing != SUMH_INJECT_MISSING_NONE &&
	     missing != SUMH_INJECT_MISSING_UNBOUND))
		return -EINVAL;
	if (!parent && missing != SUMH_INJECT_MISSING_UNBOUND)
		return 0; /* Direct missing-parent compatibility: no injection
			     key. */
	return sumh_inject_prepare_held(key, parent, out);
}

int SUMH_NOCFI sumh_inject_prepare_directory(const char *key,
					     enum sumh_inject_missing missing,
					     struct sumh_inject_claim **out)
{
	struct path path;
	int ret;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!key || key[0] != '/' ||
	    (missing != SUMH_INJECT_MISSING_NONE &&
	     missing != SUMH_INJECT_MISSING_UNBOUND))
		return -EINVAL;
	ret = kern_path(key, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &path);
	if (ret == -ENOENT) {
		if (missing == SUMH_INJECT_MISSING_NONE)
			return 0;
		return sumh_inject_prepare_held(key, NULL, out);
	}
	if (ret)
		return ret;
	ret = sumh_inject_prepare_held(key, &path, out);
	path_put(&path);
	return ret;
}

int SUMH_NOCFI sumh_inject_prepare_merge(const char *key, char **resolved_src,
					 struct sumh_inject_claim *claims[2])
{
	struct path path;
	char *buffer, *text;
	int ret;

	if (!resolved_src || !claims)
		return -EINVAL;
	*resolved_src = NULL;
	claims[0] = NULL;
	claims[1] = NULL;
	if (!key || key[0] != '/')
		return -EINVAL;
	ret = kern_path(key, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &path);
	if (ret == -ENOENT)
		return sumh_inject_prepare_held(key, NULL, &claims[0]);
	if (ret)
		return ret;
	buffer = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!buffer) {
		ret = -ENOMEM;
		goto out_path;
	}
	text = d_path(&path, buffer, PATH_MAX);
	if (IS_ERR(text)) {
		ret = PTR_ERR(text);
		goto out;
	}
	if (text[0] != '/') {
		ret = -EINVAL;
		goto out;
	}
	if (strcmp(key, text)) {
		*resolved_src = kstrdup(text, GFP_KERNEL);
		if (!*resolved_src) {
			ret = -ENOMEM;
			goto out;
		}
	}
	ret = sumh_inject_prepare_held(key, &path, &claims[0]);
	if (!ret && *resolved_src)
		ret =
		    sumh_inject_prepare_held(*resolved_src, &path, &claims[1]);
out:
	kfree(buffer);
out_path:
	path_put(&path);
	if (ret) {
		sumh_inject_destroy(claims[0]);
		sumh_inject_destroy(claims[1]);
		claims[0] = NULL;
		claims[1] = NULL;
		kfree(*resolved_src);
		*resolved_src = NULL;
	}
	return ret;
}

static struct sumh_inject_entry *sumh_inject_key_lookup_locked(const char *key)
{
	struct sumh_inject_entry *entry;
	u32 hash = full_name_hash(NULL, key, strlen(key));

	hlist_for_each_entry(entry,
			     &sumh_inject_dirs[hash_min(hash, SUMH_HASH_BITS)],
			     node) if (!strcmp(entry->dir, key)) return entry;
	return NULL;
}

bool sumh_inject_key_present_locked(const char *key)
{
	return key && sumh_inject_key_lookup_locked(key);
}

static struct sumh_inject_inode *
sumh_inject_inode_lookup_locked(struct inode *inode)
{
	struct sumh_inject_inode *binding;

	hash_for_each_possible(sumh_inject_inodes, binding, node,
			       (unsigned long)inode) if (binding->inode ==
							 inode) return binding;
	return NULL;
}

int sumh_inject_preflight_locked(struct sumh_inject_claim *claims[],
				 unsigned int count, unsigned int extra_rows,
				 struct sumh_inject_attach_plan *plan)
{
	struct sumh_inject_entry *keys[2] = {};
	struct sumh_inject_inode *bindings[2] = {};
	unsigned int i, j, additions, new_keys = 0;
	int rows = atomic_read(&sumh_rule_count);

	if (WARN_ON_ONCE(count > 2 || (count && !claims) || extra_rows > 1))
		return -EINVAL;
	/* Resolve the complete group before any mutation. Repeated keys/inodes
	 * share one candidate, including two distinct aliases of one inode. */
	for (i = 0; i < count; i++) {
		struct sumh_inject_claim *claim = claims[i];

		if (!claim)
			continue;
		if (WARN_ON_ONCE(claim->attached || claim->key ||
				 claim->binding || !claim->spare_key ||
				 !claim->spare_key->dir ||
				 claim->spare_key->dir[0] != '/' ||
				 claim->spare_key->users ||
				 (claim->spare_binding &&
				  (!claim->spare_binding->inode ||
				   !claim->spare_binding->sb ||
				   claim->spare_binding->users))))
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (WARN_ON_ONCE(claims[j] == claim))
				return -EINVAL;
		keys[i] = sumh_inject_key_lookup_locked(claim->spare_key->dir);
		for (j = 0; !keys[i] && j < i; j++)
			if (keys[j] &&
			    !strcmp(keys[j]->dir, claim->spare_key->dir))
				keys[i] = keys[j];
		if (!keys[i]) {
			keys[i] = claim->spare_key;
			new_keys++;
		}
		if (!claim->spare_binding)
			continue;
		bindings[i] = sumh_inject_inode_lookup_locked(
		    claim->spare_binding->inode);
		for (j = 0; !bindings[i] && j < i; j++)
			if (bindings[j] &&
			    bindings[j]->inode == claim->spare_binding->inode)
				bindings[i] = bindings[j];
		if (!bindings[i])
			bindings[i] = claim->spare_binding;
	}
	for (i = 0; i < count; i++) {
		if (keys[i]) {
			additions = 0;
			for (j = 0; j < count; j++)
				additions += keys[j] == keys[i];
			if (keys[i]->users > UINT_MAX - additions)
				return -EOVERFLOW;
		}
		if (bindings[i]) {
			additions = 0;
			for (j = 0; j < count; j++)
				additions += bindings[j] == bindings[i];
			if (bindings[i]->users > UINT_MAX - additions)
				return -EOVERFLOW;
		}
	}
	if (WARN_ON_ONCE(rows < 0) ||
	    (unsigned int)rows > INT_MAX - new_keys - extra_rows)
		return -EOVERFLOW;
	plan->count = count;
	for (i = 0; i < count; i++) {
		plan->claims[i] = claims[i];
		plan->keys[i] = keys[i];
		plan->bindings[i] = bindings[i];
	}
	return 0;
}

void sumh_inject_commit_locked(struct sumh_inject_attach_plan *plan)
{
	struct sumh_inject_claim **claims = plan->claims;
	struct sumh_inject_entry **keys = plan->keys;
	struct sumh_inject_inode **bindings = plan->bindings;
	unsigned int count = plan->count, i;
	/* Preflight made the remaining publication allocation-free. */
	for (i = 0; i < count; i++) {
		struct sumh_inject_claim *claim = claims[i];
		u32 hash;

		if (!claim)
			continue;
		if (!keys[i]->users) {
			hash = full_name_hash(NULL, keys[i]->dir,
					      strlen(keys[i]->dir));
			hlist_add_head_rcu(
			    &keys[i]->node,
			    &sumh_inject_dirs[hash_min(hash, SUMH_HASH_BITS)]);
			claim->spare_key = NULL;
			atomic_inc(&sumh_rule_count);
		}
		keys[i]->users++;
		if (bindings[i]) {
			if (!bindings[i]->users) {
				hash_add(sumh_inject_inodes, &bindings[i]->node,
					 (unsigned long)bindings[i]->inode);
				set_bit(AS_FLAGS_SUMH_DIR_HAS_INJECT,
					&bindings[i]->inode->i_mapping->flags);
				claim->spare_binding = NULL;
			}
			bindings[i]->users++;
		}
		claim->key = keys[i];
		claim->binding = bindings[i];
		claim->attached = true;
	}
}

int sumh_inject_attach_locked(struct sumh_inject_claim *claims[],
			      unsigned int count)
{
	struct sumh_inject_attach_plan plan;
	int ret = sumh_inject_preflight_locked(claims, count, 0, &plan);

	if (!ret)
		sumh_inject_commit_locked(&plan);
	return ret;
}

void sumh_inject_detach_locked(struct sumh_inject_claim *claim)
{
	struct sumh_inject_entry *key;
	struct sumh_inject_inode *binding;

	if (!claim || !claim->attached)
		return;
	key = claim->key;
	binding = claim->binding;
	if (WARN_ON_ONCE(!key || !key->users || (binding && !binding->users)))
		return;
	if (!--key->users) {
		hlist_del_rcu(&key->node);
		atomic_dec(&sumh_rule_count);
		call_rcu(&key->rcu, sumh_inject_entry_free_rcu);
	}
	if (binding && !--binding->users) {
		clear_bit(AS_FLAGS_SUMH_DIR_HAS_INJECT,
			  &binding->inode->i_mapping->flags);
		hash_del(&binding->node);
		llist_add(&binding->free_node, &sumh_retired_inject_inodes);
		schedule_work(&sumh_store_free_work);
	}
	claim->key = NULL;
	claim->binding = NULL;
	claim->attached = false;
}
/* SUMH_INJECT_CLAIMS_END */

static void sumh_entry_free(struct sumh_entry *e)
{
	sumh_dh_claim_put(e->dh_claim);
	sumh_inject_destroy(e->inject_claim);
	sumh_entry_release_source(e);
	kfree(e->src);
	kfree(e->target);
	kfree(e->source_canonical);
	kfree(e);
}

void sumh_entry_free_rcu(struct rcu_head *head)
{
	struct sumh_entry *e = container_of(head, struct sumh_entry, rcu);

	/* The last path reference can evict an inode and sleep. RCU callbacks
	 * only transfer ownership; release VFS references in process context.
	 */
	llist_add(&e->free_node, &sumh_retired_entries);
	schedule_work(&sumh_store_free_work);
}

void sumh_hide_entry_free_rcu(struct rcu_head *head)
{
	struct sumh_hide_entry *e =
	    container_of(head, struct sumh_hide_entry, rcu);

	llist_add(&e->free_node, &sumh_retired_hides);
	schedule_work(&sumh_store_free_work);
}

void sumh_inject_entry_free_rcu(struct rcu_head *head)
{
	struct sumh_inject_entry *e =
	    container_of(head, struct sumh_inject_entry, rcu);

	kfree(e->dir);
	kfree(e);
}

void sumh_xattr_sb_entry_free_rcu(struct rcu_head *head)
{
	struct sumh_xattr_sb_entry *e =
	    container_of(head, struct sumh_xattr_sb_entry, rcu);

	sumh_overlay_xattr_retire(e);
}

static void sumh_merge_entry_free(struct sumh_merge_entry *e)
{
	sumh_inject_destroy(e->inject_claims[0]);
	sumh_inject_destroy(e->inject_claims[1]);
	if (e->target_path.dentry)
		path_put(&e->target_path);
	kfree(e->src);
	kfree(e->target);
	kfree(e->resolved_src);
	kfree(e);
}

void sumh_merge_entry_free_rcu(struct rcu_head *head)
{
	struct sumh_merge_entry *e =
	    container_of(head, struct sumh_merge_entry, rcu);

	llist_add(&e->free_node, &sumh_retired_merges);
	schedule_work(&sumh_store_free_work);
}

static void sumh_store_free_workfn(struct work_struct *work)
{
	struct llist_node *head, *node, *next;
	bool progress;

	(void)work;
	do {
		progress = false;
		while ((head = llist_del_all(&sumh_retired_entries)) != NULL) {
			progress = true;
			llist_for_each_safe(node, next, head)
			{
				struct sumh_entry *e = container_of(
				    node, struct sumh_entry, free_node);

				sumh_entry_free(e);
				cond_resched();
			}
		}
		while ((head = llist_del_all(&sumh_retired_hides)) != NULL) {
			progress = true;
			llist_for_each_safe(node, next, head)
			{
				struct sumh_hide_entry *e = container_of(
				    node, struct sumh_hide_entry, free_node);
				sumh_dh_claim_put(e->dh_claim);
				kfree(e->path);
				kfree(e);
			}
		}
		while ((head = llist_del_all(&sumh_retired_merges)) != NULL) {
			progress = true;
			llist_for_each_safe(node, next, head)
			{
				struct sumh_merge_entry *e = container_of(
				    node, struct sumh_merge_entry, free_node);

				sumh_merge_entry_free(e);
				cond_resched();
			}
		}
		while ((head = llist_del_all(&sumh_retired_inject_inodes)) !=
		       NULL) {
			progress = true;
			llist_for_each_safe(node, next, head)
			{
				struct sumh_inject_inode *binding =
				    container_of(node, struct sumh_inject_inode,
						 free_node);

				sumh_inject_inode_free(binding);
				cond_resched();
			}
		}
		/* Callbacks may have filled an earlier queue during a release.
		 * Enqueue always schedules work, also covering the final race.
		 */
	} while (progress);
}

void sumh_store_drain(void)
{
	/* Callers serialize rule mutations before waiting. First complete the
	 * RCU ownership transfer, then finish all sleepable teardown before a
	 * control clear returns or the module lifecycle pin is released.
	 */
	rcu_barrier();
	flush_work(&sumh_store_free_work);
}

void sumh_spoof_kstat_entry_free_rcu(struct rcu_head *head)
{
	struct sumh_spoof_kstat_entry *e =
	    container_of(head, struct sumh_spoof_kstat_entry, rcu);

	kfree(e->target_pathname);
	kfree(e);
}

struct sumh_spoof_kstat_entry *
sumh_spoof_kstat_lookup_by_path(const char *path_str)
{
	struct sumh_spoof_kstat_entry *e;
	u32 hash;

	if (!path_str || !*path_str)
		return NULL;
	hash = full_name_hash(NULL, path_str, strlen(path_str));
	hlist_for_each_entry_rcu(
	    e, &sumh_spoof_kstat_path[hash_min(hash, SUMH_HASH_BITS)],
	    path_node)
	{
		if (e->path_hash == hash && e->target_pathname &&
		    strcmp(e->target_pathname, path_str) == 0)
			return e;
	}
	return NULL;
}

struct sumh_spoof_kstat_entry *sumh_spoof_kstat_lookup_by_ino(unsigned long ino,
							      unsigned long dev)
{
	struct sumh_spoof_kstat_entry *e, *wildcard = NULL;

	if (!ino)
		return NULL;
	hlist_for_each_entry_rcu(
	    e, &sumh_spoof_kstat_ino[hash_min(ino, SUMH_HASH_BITS)], ino_node)
	{
		if (e->target_ino != ino)
			continue;
		/* A path on device zero is still exact. Only legacy inode-only
		 * rules are wildcards, and exact matches always win. */
		if (!e->target_pathname)
			wildcard = e;
		else if (e->target_dev == dev)
			return e;
	}
	return wildcard;
}

void sumh_mark_inode_hidden(struct inode *inode)
{
	if (inode && inode->i_mapping)
		set_bit(AS_FLAGS_SUMH_HIDE, &inode->i_mapping->flags);
}

SUMH_NOCFI void sumh_clear_inode_flags_for_path(const char *path_str,
						unsigned int bit)
{
	struct path p;

	if (!path_str)
		return;
	if (kern_path(path_str, LOOKUP_FOLLOW, &p) != 0)
		return;
	if (p.dentry && d_inode(p.dentry) && d_inode(p.dentry)->i_mapping)
		clear_bit(bit, &d_inode(p.dentry)->i_mapping->flags);
	path_put(&p);
}

int sumh_store_upsert(struct sumh_entry *replacement, bool *changed_out)
{
	struct sumh_entry *entry, *previous = NULL;
	u32 hash = replacement->src_hash;
	bool changed = true;
	unsigned long h1, h2;
	int ret;
	struct sumh_inject_attach_plan injection;
	struct sumh_dh_change change;
	struct sumh_dh_retire retired;

	if (!replacement->dh_claim)
		return -EINVAL;
	sumh_dh_retire_init(&retired, false);
	mutex_lock(&sumh_config_mutex);
	hlist_for_each_entry(entry, &sumh_paths[hash_min(hash, SUMH_HASH_BITS)],
			     node)
	{
		if (entry->src_hash != hash ||
		    strcmp(entry->src, replacement->src))
			continue;
		previous = entry;
		changed = strcmp(entry->target, replacement->target) != 0 ||
			  !path_equal(&entry->source_path,
				      &replacement->source_path) ||
			  entry->source_nofollow_path_valid !=
			      replacement->source_nofollow_path_valid ||
			  (entry->source_nofollow_path_valid &&
			   !path_equal(&entry->source_nofollow_path,
				       &replacement->source_nofollow_path));
		break;
	}
	/* Count the new mapping and all new unique keys together. Even an
	 * identical reissue acquires its own claim before the old one leaves.
	 */
	ret = sumh_inject_preflight_locked(&replacement->inject_claim, 1,
					   previous ? 0 : 1, &injection);
	if (ret)
		goto out;
	ret =
	    sumh_dh_change_begin(replacement->dh_claim,
				 previous ? previous->dh_claim : NULL, &change);
	if (ret)
		goto out;
	sumh_inject_commit_locked(&injection);
	sumh_dh_change_commit(&change, &retired);
	hlist_add_head_rcu(&replacement->node,
			   &sumh_paths[hash_min(hash, SUMH_HASH_BITS)]);
	if (previous) {
		hlist_del_rcu(&previous->node);
		sumh_inject_detach_locked(previous->inject_claim);
		/* Keep the shared flag: an explicit kstat rule or another
		 * redirect may still own this source. getattr checks live
		 * rules. */
		call_rcu(&previous->rcu, sumh_entry_free_rcu);
	} else {
		h1 = jhash(replacement->src, strlen(replacement->src), 0) &
		     (SUMH_BLOOM_SIZE - 1);
		h2 = jhash(replacement->src, strlen(replacement->src), 1) &
		     (SUMH_BLOOM_SIZE - 1);
		set_bit(h1, sumh_path_bloom);
		set_bit(h2, sumh_path_bloom);
		atomic_inc(&sumh_rule_count);
	}
	sumh_dh_change_end();
	if (changed_out)
		*changed_out = changed;
out:
	mutex_unlock(&sumh_config_mutex);
	sumh_dh_retire_finish(&retired);
	return ret;
}

void sumh_cleanup_locked(struct sumh_dh_retire *retired)
{
	struct sumh_entry *entry;
	struct sumh_hide_entry *hide_entry;
	struct sumh_xattr_sb_entry *sb_entry;
	struct sumh_merge_entry *merge_entry;
	struct hlist_node *tmp;
	int bkt;

	/* Rule cleanup is reusable while the always-on provider remains live.
	 * Lifecycle shutdown withdraws sumh_enabled before calling here. */
	sumh_stealth_enabled = false;
	sumh_feature_enabled_mask = 0;
	sumh_mount_hide_mode = SUMH_MOUNT_HIDE_MODE_NORMAL;
	/* Stop provider calls and release any external module reference. */

	hash_for_each_safe(sumh_paths, bkt, tmp, entry, node)
	{
		sumh_clear_inode_flags_for_path(entry->src, AS_FLAGS_SUMH_HIDE);
		sumh_clear_inode_flags_for_path(entry->target,
						AS_FLAGS_SUMH_SPOOF_KSTAT);
		hlist_del_rcu(&entry->node);
		sumh_dh_detach(entry->dh_claim, retired);
		sumh_inject_detach_locked(entry->inject_claim);
		call_rcu(&entry->rcu, sumh_entry_free_rcu);
	}
	hash_for_each_safe(sumh_hide_paths, bkt, tmp, hide_entry, node)
	{
		sumh_clear_inode_flags_for_path(hide_entry->path,
						AS_FLAGS_SUMH_HIDE);
		hlist_del_rcu(&hide_entry->node);
		sumh_dh_detach(hide_entry->dh_claim, retired);
		call_rcu(&hide_entry->rcu, sumh_hide_entry_free_rcu);
	}
	hash_for_each_safe(sumh_xattr_sbs, bkt, tmp, sb_entry, node)
	{
		hlist_del_rcu(&sb_entry->node);
		call_rcu(&sb_entry->rcu, sumh_xattr_sb_entry_free_rcu);
	}
	hash_for_each_safe(sumh_merge_dirs, bkt, tmp, merge_entry, node)
	{
		hlist_del_rcu(&merge_entry->node);
		sumh_inject_detach_locked(merge_entry->inject_claims[0]);
		sumh_inject_detach_locked(merge_entry->inject_claims[1]);
		call_rcu(&merge_entry->rcu, sumh_merge_entry_free_rcu);
	}
	/* A surviving owner is an internal defect; never free its indexed
	 * objects behind it or attempt pathname-based marker repair. */
	WARN_ON_ONCE(!hash_empty(sumh_inject_dirs));
	WARN_ON_ONCE(!hash_empty(sumh_inject_inodes));
	{
		struct sumh_spoof_kstat_entry *sk_entry;

		hash_for_each_safe(sumh_spoof_kstat_path, bkt, tmp, sk_entry,
				   path_node)
		{
			sumh_clear_inode_flags_for_path(
			    sk_entry->target_pathname,
			    AS_FLAGS_SUMH_SPOOF_KSTAT);
			hlist_del_rcu(&sk_entry->path_node);
			if (sk_entry->target_ino)
				hlist_del_rcu(&sk_entry->ino_node);
			call_rcu(&sk_entry->rcu,
				 sumh_spoof_kstat_entry_free_rcu);
		}
		hash_for_each_safe(sumh_spoof_kstat_ino, bkt, tmp, sk_entry,
				   ino_node)
		{
			hlist_del_rcu(&sk_entry->ino_node);
			call_rcu(&sk_entry->rcu,
				 sumh_spoof_kstat_entry_free_rcu);
		}
		atomic_set(&sumh_spoof_kstat_count, 0);
	}
	{
		struct sumh_ino_map_entry *im_entry;

		hash_for_each_safe(sumh_ino_map, bkt, tmp, im_entry, node)
		{
			hlist_del_rcu(&im_entry->node);
			kfree_rcu(im_entry, rcu);
		}
		atomic_set(&sumh_ino_map_count, 0);
		atomic64_set(&sumh_ino_next, SUMH_VNODE_INO_BASE);
	}

	bitmap_zero(sumh_path_bloom, SUMH_BLOOM_SIZE);
	bitmap_zero(sumh_hide_bloom, SUMH_BLOOM_SIZE);
	atomic_set(&sumh_rule_count, 0);
	atomic_set(&sumh_hide_count, 0);
}
