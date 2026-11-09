#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/version.h>
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
#include "sumh_path_policy.h"
#include "sumh_fop_override.h"
#include "sumh_vfs_view.h"

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
struct sumh_managed_kstat_snapshot __rcu *sumh_managed_kstat;
u64 sumh_managed_kstat_generation;
atomic_t sumh_spoof_kstat_count = ATOMIC_INIT(0);

DEFINE_MUTEX(sumh_config_mutex);
LIST_HEAD(sumh_maps_rules);
DEFINE_MUTEX(sumh_maps_mutex);

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
struct dentry *(*sumh_lookup_one_len)(const char *, struct dentry *, int);
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

int SUMH_NOCFI sumh_entry_capture_source(struct sumh_entry *entry,
					 const char *source_path)
{
	struct path nofollow_path;
	struct inode *nofollow_inode;
	struct kstat visible_kstat;
	struct path visible_path;
	struct inode *visible_inode;
	struct inode *inode;
	struct path path;
	int ret;

	if (!entry || !source_path || !*source_path)
		return -EINVAL;
	if (entry->source_path_valid)
		return -EALREADY;

	ret = kern_path(source_path, LOOKUP_FOLLOW, &path);
	if (ret)
		return ret;
	inode = d_inode(path.dentry);
	if (!inode) {
		path_put(&path);
		return -ENOENT;
	}

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
		path_put(&path);
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
	if (kern_path(source_path, 0, &nofollow_path) == 0) {
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
		path_put(&nofollow_path);
	}
	entry->source_path_valid = true;
	atomic64_inc(&sumh_vnode_allocated_count);
	atomic_inc(&sumh_vnode_live_count);
	path_put(&path);
	return 0;
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
static LLIST_HEAD(sumh_retired_merges);
static void sumh_store_free_workfn(struct work_struct *work);
static DECLARE_WORK(sumh_store_free_work, sumh_store_free_workfn);

static void sumh_entry_free(struct sumh_entry *e)
{
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

	kfree(e->path);
	kfree(e);
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
	if (e->target_dentry)
		dput(e->target_dentry);
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

	(void)work;
	while ((head = llist_del_all(&sumh_retired_entries)) != NULL) {
		llist_for_each_safe(node, next, head)
		{
			struct sumh_entry *e =
			    container_of(node, struct sumh_entry, free_node);

			sumh_entry_free(e);
			cond_resched();
		}
	}
	while ((head = llist_del_all(&sumh_retired_merges)) != NULL) {
		llist_for_each_safe(node, next, head)
		{
			struct sumh_merge_entry *e = container_of(
			    node, struct sumh_merge_entry, free_node);

			sumh_merge_entry_free(e);
			cond_resched();
		}
	}
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

void sumh_managed_kstat_snapshot_free_rcu(struct rcu_head *head)
{
	struct sumh_managed_kstat_snapshot *snapshot =
	    container_of(head, struct sumh_managed_kstat_snapshot, rcu);
	u32 i;

	for (i = 0; i < snapshot->count; i++) {
		kfree(snapshot->entries[i]->target_pathname);
		kfree(snapshot->entries[i]);
	}
	kfree(snapshot);
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
	struct sumh_managed_kstat_snapshot *managed;
	u32 i;

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
	managed = rcu_dereference(sumh_managed_kstat);
	if (managed) {
		for (i = 0; i < managed->count; i++) {
			e = managed->entries[i];
			if (e->target_ino == ino && e->target_dev == dev)
				return e;
		}
	}
	return wildcard;
}

void sumh_mark_inode_hidden(struct inode *inode)
{
	if (inode && inode->i_mapping)
		set_bit(AS_FLAGS_SUMH_HIDE, &inode->i_mapping->flags);
}

SUMH_NOCFI void sumh_mark_dir_has_inject(const char *path_str)
{
	struct path p;

	if (!path_str)
		return;
	if (kern_path(path_str, LOOKUP_FOLLOW, &p) != 0)
		return;
	if (p.dentry && d_inode(p.dentry) && d_inode(p.dentry)->i_mapping) {
		struct inode *inode = d_inode(p.dentry);

		set_bit(AS_FLAGS_SUMH_DIR_HAS_INJECT, &inode->i_mapping->flags);
		(void)sumh_fop_install(inode);
	}
	path_put(&p);
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

bool sumh_store_upsert(struct sumh_entry *replacement)
{
	struct sumh_entry *entry;
	u32 hash = replacement->src_hash;
	bool changed = true, found = false;
	unsigned long h1, h2;

	mutex_lock(&sumh_config_mutex);
	hlist_for_each_entry(entry, &sumh_paths[hash_min(hash, SUMH_HASH_BITS)],
			     node)
	{
		if (entry->src_hash != hash ||
		    strcmp(entry->src, replacement->src))
			continue;
		changed = strcmp(entry->target, replacement->target) != 0;
		hlist_add_head_rcu(&replacement->node,
				   &sumh_paths[hash_min(hash, SUMH_HASH_BITS)]);
		hlist_del_rcu(&entry->node);
		/* Keep the shared flag: an explicit kstat rule or another
		 * redirect may still own this source. getattr checks live
		 * rules. */
		call_rcu(&entry->rcu, sumh_entry_free_rcu);
		found = true;
		break;
	}
	if (!found) {
		hlist_add_head_rcu(&replacement->node,
				   &sumh_paths[hash_min(hash, SUMH_HASH_BITS)]);
		h1 = jhash(replacement->src, strlen(replacement->src), 0) &
		     (SUMH_BLOOM_SIZE - 1);
		h2 = jhash(replacement->src, strlen(replacement->src), 1) &
		     (SUMH_BLOOM_SIZE - 1);
		set_bit(h1, sumh_path_bloom);
		set_bit(h2, sumh_path_bloom);
		atomic_inc(&sumh_rule_count);
	}
	mutex_unlock(&sumh_config_mutex);
	return changed;
}

void sumh_cleanup_locked(void)
{
	struct sumh_entry *entry;
	struct sumh_hide_entry *hide_entry;
	struct sumh_inject_entry *inject_entry;
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
		call_rcu(&entry->rcu, sumh_entry_free_rcu);
	}
	hash_for_each_safe(sumh_hide_paths, bkt, tmp, hide_entry, node)
	{
		sumh_clear_inode_flags_for_path(hide_entry->path,
						AS_FLAGS_SUMH_HIDE);
		hlist_del_rcu(&hide_entry->node);
		call_rcu(&hide_entry->rcu, sumh_hide_entry_free_rcu);
	}
	hash_for_each_safe(sumh_inject_dirs, bkt, tmp, inject_entry, node)
	{
		sumh_clear_inode_flags_for_path(inject_entry->dir,
						AS_FLAGS_SUMH_DIR_HAS_INJECT);
		hlist_del_rcu(&inject_entry->node);
		call_rcu(&inject_entry->rcu, sumh_inject_entry_free_rcu);
	}
	hash_for_each_safe(sumh_xattr_sbs, bkt, tmp, sb_entry, node)
	{
		hlist_del_rcu(&sb_entry->node);
		call_rcu(&sb_entry->rcu, sumh_xattr_sb_entry_free_rcu);
	}
	hash_for_each_safe(sumh_merge_dirs, bkt, tmp, merge_entry, node)
	{
		hlist_del_rcu(&merge_entry->node);
		call_rcu(&merge_entry->rcu, sumh_merge_entry_free_rcu);
	}
	{
		struct sumh_spoof_kstat_entry *sk_entry;
		struct sumh_managed_kstat_snapshot *managed;

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
		managed = rcu_dereference_protected(sumh_managed_kstat, 1);
		RCU_INIT_POINTER(sumh_managed_kstat, NULL);
		if (managed) {
			if (managed->count)
				sumh_managed_kstat_generation++;
			call_rcu(&managed->rcu,
				 sumh_managed_kstat_snapshot_free_rcu);
		}
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
