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
#include <linux/percpu.h>
#include <linux/smp.h>
#include <linux/mount.h>
#include <linux/xattr.h>
#include <linux/seq_file.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/xarray.h>
#include <uapi/linux/magic.h>
#include <asm/unistd.h>
#include "sumh_runtime.h"
#include "sumh_store.h"
#include "sumh_path_policy.h"
#include "sumh_dirhijack.h"
#include "sumh_hide_rules.h"
#include "sumh_name_index.h"
#include "policy/allowlist.h"

#define SUMH_MEDIA_RW_GID 1023

/* ======================================================================
 * Part 11: Core Logic - Privileged Check / Allowlist
 * ====================================================================== */

bool sumh_is_privileged_process(void)
{
	return uid_eq(current_uid(), GLOBAL_ROOT_UID);
}

static enum sumh_policy_scope sumh_policy_scope_for_uid(uid_t uid)
{
	if (is_isolated_process(uid) ||
	    (uid % PER_USER_RANGE >= 90000 &&
	     uid % PER_USER_RANGE < FIRST_ISOLATED_UID))
		return SUMH_POLICY_SCOPE_SPOOF;
	if (!is_appuid(uid))
		return SUMH_POLICY_SCOPE_VIEW;
	return ksu_uid_should_umount(uid) ? SUMH_POLICY_SCOPE_SPOOF
					  : SUMH_POLICY_SCOPE_VIEW;
}

enum sumh_policy_scope sumh_policy_current_scope(void)
{
	if (sumh_hide_rules_resolving() || !smp_load_acquire(&sumh_enabled))
		return SUMH_POLICY_SCOPE_NONE;
	return sumh_policy_scope_for_uid(__kuid_val(task_uid(current)));
}

bool sumh_policy_current_is_view_target(void)
{
	return sumh_policy_current_scope() == SUMH_POLICY_SCOPE_VIEW;
}

bool sumh_hide_storage_parent(const struct inode *parent)
{
	return parent && (parent->i_sb->s_magic == FUSE_SUPER_MAGIC ||
			  gid_eq(READ_ONCE(parent->i_gid),
				 KGIDT_INIT(SUMH_MEDIA_RW_GID)));
}

static bool sumh_hide_scope_allowed(bool storage_managed)
{
	long ioctl_tgid;
	uid_t uid = __kuid_val(current_uid());

	if (!smp_load_acquire(&sumh_enabled) || sumh_is_privileged_process())
		return false;
	if (is_appuid(uid) && ksu_is_allow_uid(uid))
		return false;
	/* Storage services need the backing view, not a global hide exemption.
	 */
	if (storage_managed && in_group_p(KGIDT_INIT(SUMH_MEDIA_RW_GID)))
		return false;
	ioctl_tgid = atomic_long_read(&sumh_ioctl_tgid);
	return ioctl_tgid <= 0 || ioctl_tgid != (long)task_tgid_vnr(current);
}

bool sumh_policy_current_is_hide_target(const struct inode *parent)
{
	return sumh_hide_scope_allowed(sumh_hide_storage_parent(parent));
}

bool sumh_policy_current_is_spoof_target(void)
{
	return sumh_policy_current_scope() == SUMH_POLICY_SCOPE_SPOOF;
}

bool sumh_policy_uid_is_spoof_target(uid_t uid)
{
	return sumh_policy_scope_for_uid(uid) == SUMH_POLICY_SCOPE_SPOOF;
}

bool sumh_policy_current_is_isolated(void)
{
	uid_t appid = __kuid_val(task_uid(current)) % PER_USER_RANGE;

	return appid >= 90000 && appid <= LAST_ISOLATED_UID;
}

/* Mount observations are independent of the module-content policy. Apps
 * retaining their mounts still need a filtered view when mount_hide is on.
 * Keep VIEW/SPOOF routing intact: changing it would disable their SUMH files.
 */
bool sumh_policy_current_is_mount_view_target(void)
{
	uid_t uid = __kuid_val(task_uid(current));

	if (sumh_hide_rules_resolving() || !smp_load_acquire(&sumh_enabled))
		return false;
	if (sumh_policy_current_is_isolated())
		return true;
	/* The allowlist includes the manager. Privileged tools and system
	 * services continue to inspect and manage the real mount topology.
	 */
	return is_appuid(uid) && !ksu_is_allow_uid(uid);
}

#define SUMH_SYMLINK_LIMIT 40

static bool sumh_relative_path_safe(const char *path)
{
	const char *component = path;

	while (component && *component) {
		const char *slash = strchr(component, '/');
		size_t length =
		    slash ? (size_t)(slash - component) : strlen(component);

		if ((length == 1 && component[0] == '.') ||
		    (length == 2 && component[0] == '.' && component[1] == '.'))
			return false;
		component = slash ? slash + 1 : NULL;
	}
	return true;
}

static bool SUMH_NOCFI sumh_rule_get_source_flags_depth(
    const char *pathname, unsigned int lookup_flags,
    struct sumh_rule_source *source, unsigned int symlink_depth);

void sumh_project_visible_stat(struct kstat *result,
			       const struct kstat *source_stat,
			       const struct kstat *visible_template,
			       bool preserve_visible_metadata,
			       unsigned long visible_ino,
			       unsigned long visible_dev)
{
	if (!result || !source_stat)
		return;
	if (preserve_visible_metadata && visible_template) {
		*result = *visible_template;
		result->mode = (source_stat->mode & S_IFMT) |
			       (visible_template->mode & ~S_IFMT);
		result->rdev = source_stat->rdev;
		result->size = source_stat->size;
		result->blocks = source_stat->blocks;
		result->blksize = source_stat->blksize;
		result->result_mask |=
		    source_stat->result_mask &
		    (STATX_TYPE | STATX_MODE | STATX_SIZE | STATX_BLOCKS);
	} else {
		*result = *source_stat;
	}
	result->ino = visible_ino;
	result->dev = visible_dev;
}

static bool sumh_rule_refresh_exact_source(struct sumh_rule_source *source)
{
	struct kstat source_stat;
	int ret;

	if (!source || !source->path.dentry || !source->path.mnt)
		return false;
	memset(&source_stat, 0, sizeof(source_stat));
	ret = sumh_vfs_getattr_unprojected(&source->path, &source_stat,
					   STATX_BASIC_STATS | STATX_BTIME,
					   AT_STATX_SYNC_AS_STAT);
	if (ret) {
		path_put(&source->path);
		source->stat_valid = false;
		source->error = ret;
		return false;
	}
	/* The pinned path fixes object identity across rename/unlink, but
	 * mutable inode metadata must retain ordinary VFS behaviour after rule
	 * install. */
	source->source_mode = source_stat.mode;
	sumh_project_visible_stat(&source->stat, &source_stat, &source->stat,
				  source->preserve_visible_metadata,
				  source->visible_ino, source->visible_dev);
	source->stat_valid = true;
	return true;
}

static bool sumh_rule_source_from_path(struct path *resolved,
				       struct sumh_rule_source *source)
{
	struct inode *inode;
	struct kstat stat;
	int ret;

	inode = resolved && resolved->dentry ? d_inode(resolved->dentry) : NULL;
	if (!inode) {
		path_put(resolved);
		source->error = -ENOENT;
		return false;
	}
	memset(&stat, 0, sizeof(stat));
	ret = sumh_vfs_getattr_unprojected(resolved, &stat,
					   STATX_BASIC_STATS | STATX_BTIME,
					   AT_STATX_SYNC_AS_STAT);
	if (ret) {
		path_put(resolved);
		source->error = ret;
		return false;
	}
	source->path = *resolved;
	source->stat = stat;
	source->visible_ino = sumh_vnode_source_ino(
	    inode->i_sb ? inode->i_sb->s_dev : 0, inode->i_ino);
	/* Scheme A: dynamic (directory/merge) sources have no cached visible
	 * dev. Publish the captured /system dev — a real device and the correct
	 * one for the common /system masquerade — rather than the anonymous
	 * vnode minor. */
	source->visible_dev = READ_ONCE(sumh_system_dev);
	if (!source->visible_dev)
		source->visible_dev = sumh_vnode_device();
	source->stat.ino = source->visible_ino;
	source->stat.dev = source->visible_dev;
	source->source_mode = stat.mode;
	source->stat_valid = true;
	source->preserve_visible_metadata = false;
	return true;
}

static SUMH_NOCFI int sumh_read_link_target(const struct path *link,
					    char *target, size_t target_size)
{
	struct delayed_call done = {};
	const char *value;
	size_t length;
	int ret = 0;

	if (!link || !link->dentry || !target || target_size < 2)
		return -EOPNOTSUPP;
	value = vfs_get_link(link->dentry, &done);
	if (IS_ERR(value)) {
		ret = PTR_ERR(value);
		goto out;
	}
	if (!value) {
		ret = -EIO;
		goto out;
	}
	length = strnlen(value, target_size);
	if (length >= target_size) {
		ret = -ENAMETOOLONG;
		goto out;
	}
	memcpy(target, value, length + 1);
out:
	do_delayed_call(&done);
	return ret;
}

static bool SUMH_NOCFI sumh_follow_absolute_link(
    const char *target, const char *remaining, unsigned int lookup_flags,
    struct sumh_rule_source *source, unsigned int symlink_depth)
{
	struct path resolved;
	char *next;
	size_t target_len;
	size_t remaining_len;
	int ret;

	if (lookup_flags & LOOKUP_BENEATH) {
		source->error = -EXDEV;
		return false;
	}
	if (symlink_depth >= SUMH_SYMLINK_LIMIT) {
		source->error = -ELOOP;
		return false;
	}
	target_len = strlen(target);
	remaining_len = remaining ? strlen(remaining) : 0;
	if (target_len + remaining_len + 1 > SUMH_MAX_LEN_PATHNAME) {
		source->error = -ENAMETOOLONG;
		return false;
	}
	next = kmalloc(target_len + remaining_len + 1, GFP_KERNEL);
	if (!next) {
		source->error = -ENOMEM;
		return false;
	}
	memcpy(next, target, target_len);
	if (remaining_len) {
		if (next[target_len - 1] == '/' && remaining[0] == '/') {
			memcpy(next + target_len, remaining + 1, remaining_len);
		} else {
			memcpy(next + target_len, remaining, remaining_len + 1);
		}
	} else {
		next[target_len] = '\0';
	}
	if (sumh_rule_get_source_flags_depth(next, lookup_flags, source,
					     symlink_depth + 1)) {
		kfree(next);
		return true;
	}
	if (source->error) {
		kfree(next);
		return false;
	}
	if (sumh_should_hide(next)) {
		source->error = -ENOENT;
		kfree(next);
		return false;
	}
	ret = kern_path(next, lookup_flags, &resolved);
	kfree(next);
	if (ret) {
		source->error = ret;
		return false;
	}
	return sumh_rule_source_from_path(&resolved, source);
}

static bool SUMH_NOCFI sumh_directory_rule_source(
    const char *pathname, size_t path_len, unsigned int lookup_flags,
    struct sumh_rule_source *source, unsigned int symlink_depth)
{
	struct sumh_entry *entry;
	struct path directory = {};
	struct path resolved;
	char *relative;
	char *cursor;
	size_t prefix_len = path_len;
	bool pinned = false;
	int ret;

	while (prefix_len > 1) {
		size_t slash = prefix_len;
		u32 hash;

		while (slash > 0 && pathname[slash - 1] != '/')
			slash--;
		if (slash <= 1)
			break;
		prefix_len = slash - 1;
		if (!sumh_relative_path_safe(pathname + prefix_len + 1))
			return false;
		if (!test_bit(jhash(pathname, (u32)prefix_len, 0) &
				  (SUMH_BLOOM_SIZE - 1),
			      sumh_path_bloom) ||
		    !test_bit(jhash(pathname, (u32)prefix_len, 1) &
				  (SUMH_BLOOM_SIZE - 1),
			      sumh_path_bloom))
			continue;
		hash = full_name_hash(NULL, pathname, prefix_len);
		rcu_read_lock();
		hlist_for_each_entry_rcu(
		    entry, &sumh_paths[hash_min(hash, SUMH_HASH_BITS)], node)
		{
			if (entry->src_hash != hash ||
			    strlen(entry->src) != prefix_len ||
			    memcmp(entry->src, pathname, prefix_len) != 0 ||
			    !entry->source_path_valid ||
			    !S_ISDIR(entry->source_mode))
				continue;
			directory = entry->source_path;
			path_get(&directory);
			pinned = true;
			break;
		}
		rcu_read_unlock();
		if (pinned)
			break;
	}
	if (!pinned)
		return false;
	relative = kstrdup(pathname + prefix_len + 1, GFP_KERNEL);
	if (!relative) {
		path_put(&directory);
		source->error = -ENOMEM;
		return false;
	}
	ret = vfs_path_lookup(directory.dentry, directory.mnt, relative,
			      lookup_flags | LOOKUP_NO_SYMLINKS, &resolved);
	if (!ret) {
		path_put(&directory);
		kfree(relative);
		return sumh_rule_source_from_path(&resolved, source);
	}
	if (ret != -ELOOP || (lookup_flags & LOOKUP_NO_SYMLINKS)) {
		source->error = ret;
		goto out_relative;
	}
	{
		cursor = relative;
		while (*cursor) {
			struct path probe;
			char target[SUMH_MAX_LEN_PATHNAME];
			char *slash = strchr(cursor, '/');
			bool final = !slash;
			char saved = '\0';

			if (slash) {
				saved = *slash;
				*slash = '\0';
			}
			ret = vfs_path_lookup(
			    directory.dentry, directory.mnt, relative,
			    lookup_flags &
				~(LOOKUP_FOLLOW | LOOKUP_NO_SYMLINKS),
			    &probe);
			if (slash)
				*slash = saved;
			if (ret) {
				source->error = ret;
				goto out_relative;
			}
			if (d_is_symlink(probe.dentry)) {
				if (lookup_flags & LOOKUP_NO_SYMLINKS) {
					path_put(&probe);
					source->error = -ELOOP;
					goto out_relative;
				}
				if (!(final &&
				      !(lookup_flags & LOOKUP_FOLLOW))) {
					if (lookup_flags & LOOKUP_CACHED) {
						path_put(&probe);
						source->error = -EAGAIN;
						goto out_relative;
					}
					ret = sumh_read_link_target(
					    &probe, target, sizeof(target));
					if (ret) {
						path_put(&probe);
						if (lookup_flags &
						    LOOKUP_NO_MAGICLINKS) {
							source->error = -ELOOP;
							goto out_relative;
						}
						goto lookup_full;
					}
					if (target[0] == '/' &&
					    !(lookup_flags & LOOKUP_IN_ROOT)) {
						const char *remaining =
						    slash ? slash : "";

						bool found;

						path_put(&probe);
						path_put(&directory);
						found =
						    sumh_follow_absolute_link(
							target, remaining,
							lookup_flags, source,
							symlink_depth);
						kfree(relative);
						return found;
					}
				}
			}
			path_put(&probe);
			if (!slash)
				break;
			cursor = slash + 1;
		}
	}
lookup_full:
	ret = vfs_path_lookup(directory.dentry, directory.mnt, relative,
			      lookup_flags, &resolved);
	if (ret) {
		source->error = ret;
		goto out_relative;
	}
	path_put(&directory);
	kfree(relative);
	return sumh_rule_source_from_path(&resolved, source);

out_relative:
	path_put(&directory);
	kfree(relative);
	return false;
}

static bool SUMH_NOCFI sumh_rule_get_source_flags_depth(
    const char *pathname, unsigned int lookup_flags,
    struct sumh_rule_source *source, unsigned int symlink_depth)
{
	struct sumh_entry *entry;
	struct path exact_link = {};
	size_t path_len;
	u32 hash;
	pid_t pid;
	bool found = false;
	bool followed_exact_link = false;

	if (unlikely(!sumh_enabled || !pathname || !source ||
		     !sumh_policy_current_is_view_target()))
		return false;
	pid = task_tgid_vnr(current);
	if (atomic_long_read(&sumh_ioctl_tgid) > 0 &&
	    pid == atomic_long_read(&sumh_ioctl_tgid))
		return false;
	if (atomic_read(&sumh_rule_count) == 0)
		return false;
	if (symlink_depth >= SUMH_SYMLINK_LIMIT) {
		source->error = -ELOOP;
		return false;
	}

	path_len = strlen(pathname);
	if (test_bit(jhash(pathname, (u32)path_len, 0) & (SUMH_BLOOM_SIZE - 1),
		     sumh_path_bloom) &&
	    test_bit(jhash(pathname, (u32)path_len, 1) & (SUMH_BLOOM_SIZE - 1),
		     sumh_path_bloom)) {
		hash = full_name_hash(NULL, pathname, path_len);
		rcu_read_lock();
		hlist_for_each_entry_rcu(
		    entry, &sumh_paths[hash_min(hash, SUMH_HASH_BITS)], node)
		{
			if (entry->src_hash != hash ||
			    strcmp(entry->src, pathname) != 0)
				continue;
			if (entry->source_path_valid &&
			    entry->source_path.dentry &&
			    entry->source_path.mnt) {
				if (!(lookup_flags & LOOKUP_FOLLOW) &&
				    entry->source_nofollow_path_valid) {
					source->path =
					    entry->source_nofollow_path;
					source->stat =
					    entry->source_nofollow_stat;
					source->source_mode =
					    entry->source_nofollow_mode;
					source->visible_ino =
					    entry->nofollow_visible_ino;
					source->visible_dev =
					    entry->nofollow_visible_dev;
					source->stat_valid =
					    entry->source_nofollow_stat_valid;
					source->preserve_visible_metadata =
					    false;
				} else {
					if ((lookup_flags &
					     LOOKUP_NO_SYMLINKS) &&
					    entry->source_nofollow_path_valid) {
						source->error = -ELOOP;
						break;
					}
					source->path = entry->source_path;
					source->stat = entry->visible_stat;
					source->source_mode =
					    entry->source_mode;
					source->visible_ino =
					    entry->visible_ino;
					source->visible_dev =
					    entry->visible_dev;
					source->stat_valid =
					    entry->visible_stat_valid;
					source->preserve_visible_metadata =
					    entry->preserve_visible_metadata;
					if (entry->source_nofollow_path_valid) {
						exact_link =
						    entry->source_nofollow_path;
						path_get(&exact_link);
						followed_exact_link = true;
					}
				}
				path_get(&source->path);
				found = true;
			}
			break;
		}
		rcu_read_unlock();
	}
	if (found && !sumh_rule_refresh_exact_source(source)) {
		if (followed_exact_link)
			path_put(&exact_link);
		return false;
	}
	if (found && followed_exact_link) {
		char target[SUMH_MAX_LEN_PATHNAME];
		int ret =
		    sumh_read_link_target(&exact_link, target, sizeof(target));

		path_put(&exact_link);
		if (ret) {
			if (lookup_flags & LOOKUP_NO_MAGICLINKS) {
				path_put(&source->path);
				source->error = -ELOOP;
				return false;
			}
			return true;
		}
		if (target[0] == '/' && !(lookup_flags & LOOKUP_IN_ROOT)) {
			struct sumh_rule_source redirected = {};

			if (sumh_follow_absolute_link(target, "", lookup_flags,
						      &redirected,
						      symlink_depth)) {
				path_put(&source->path);
				*source = redirected;
				return true;
			}
			if (redirected.error) {
				path_put(&source->path);
				source->error = redirected.error;
				return false;
			}
		}
	}
	if (found || source->error)
		return found;
	return sumh_directory_rule_source(pathname, path_len, lookup_flags,
					  source, symlink_depth);
}

bool SUMH_NOCFI sumh_rule_get_source_flags(const char *pathname,
					   unsigned int lookup_flags,
					   struct sumh_rule_source *source)
{
	if (source) {
		source->error = 0;
		source->preserve_visible_metadata = false;
	}
	return sumh_rule_get_source_flags_depth(pathname, lookup_flags, source,
						0);
}

/* ---- Slice 4c-v2: pure-virtual directory topology rule-table queries ----- */

int SUMH_NOCFI sumh_rule_vpath_child(const char *dir, const char *child,
				     struct path *leaf_src, umode_t *leaf_mode,
				     unsigned long *leaf_ino)
{
	struct sumh_entry *entry;
	char *full;
	size_t dlen, clen, full_len;
	u32 hash;
	int kind = SUMH_VPATH_NONE;
	int bkt;

	if (!dir || !child || !*child)
		return SUMH_VPATH_NONE;
	dlen = strlen(dir);
	clen = strlen(child);
	full = kmalloc(dlen + 1 + clen + 1, GFP_KERNEL);
	if (!full)
		return -ENOMEM;
	memcpy(full, dir, dlen);
	full[dlen] = '/';
	memcpy(full + dlen + 1, child, clen);
	full[dlen + 1 + clen] = '\0';
	full_len = dlen + 1 + clen;
	hash = full_name_hash(NULL, full, full_len);

	rcu_read_lock();
	/* Exact rule at dir/child -> a leaf redirect. */
	hlist_for_each_entry_rcu(
	    entry, &sumh_paths[hash_min(hash, SUMH_HASH_BITS)], node)
	{
		if (entry->src_hash != hash || !entry->src ||
		    !entry->source_path_valid || strcmp(entry->src, full) != 0)
			continue;
		if (entry->source_nofollow_path_valid) {
			if (leaf_src) {
				*leaf_src = entry->source_nofollow_path;
				path_get(leaf_src);
			}
			if (leaf_mode)
				*leaf_mode = entry->source_nofollow_mode;
			if (leaf_ino)
				*leaf_ino = entry->nofollow_visible_ino;
		} else {
			if (leaf_src) {
				*leaf_src = entry->source_path;
				path_get(leaf_src);
			}
			if (leaf_mode)
				*leaf_mode = entry->source_mode;
			if (leaf_ino)
				*leaf_ino = entry->visible_ino;
		}
		kind = SUMH_VPATH_LEAF;
		break;
	}
	/* Otherwise, a prefix of some rule -> a deeper virtual directory. */
	if (kind == SUMH_VPATH_NONE) {
		hash_for_each_rcu(sumh_paths, bkt, entry, node)
		{
			if (!entry->src)
				continue;
			if (strncmp(entry->src, full, full_len) == 0 &&
			    entry->src[full_len] == '/') {
				kind = SUMH_VPATH_VDIR;
				break;
			}
		}
	}
	rcu_read_unlock();
	kfree(full);
	return kind;
}

int SUMH_NOCFI sumh_rule_vpath_emit(const char *dir, struct list_head *out)
{
	struct sumh_entry *entry;
	struct rb_root names;
	size_t dlen;
	int count = 0;
	int bkt;

	if (!dir || !out)
		return 0;
	dlen = strlen(dir);
	sumh_name_index_init(&names, out);

	rcu_read_lock();
	hash_for_each_rcu(sumh_paths, bkt, entry, node)
	{
		const char *rest;
		const char *slash;
		size_t seg_len;
		bool is_leaf;
		struct sumh_name_list *item;
		umode_t m;

		if (!entry->src)
			continue;
		if (strncmp(entry->src, dir, dlen) != 0 ||
		    entry->src[dlen] != '/')
			continue;
		rest = entry->src + dlen + 1;
		if (!*rest)
			continue;
		slash = strchr(rest, '/');
		seg_len = slash ? (size_t)(slash - rest) : strlen(rest);
		if (!seg_len)
			continue;
		is_leaf = (slash == NULL);
		if (sumh_name_index_contains(&names, rest, seg_len))
			continue;
		item = kmalloc(sizeof(*item), GFP_ATOMIC);
		if (!item) {
			count = -ENOMEM;
			break;
		}
		item->name = kmalloc(seg_len + 1, GFP_ATOMIC);
		if (!item->name) {
			kfree(item);
			count = -ENOMEM;
			break;
		}
		memcpy(item->name, rest, seg_len);
		item->name[seg_len] = '\0';
		if (is_leaf) {
			m = entry->source_nofollow_path_valid
				? entry->source_nofollow_mode
				: entry->source_mode;
			item->type = (unsigned char)((m & S_IFMT) >> 12);
			item->ino = entry->source_nofollow_path_valid
					? entry->nofollow_visible_ino
					: entry->visible_ino;
		} else {
			char *vpath =
			    kmalloc(dlen + 1 + seg_len + 1, GFP_ATOMIC);

			item->type = DT_DIR;
			if (vpath) {
				memcpy(vpath, dir, dlen);
				vpath[dlen] = '/';
				memcpy(vpath + dlen + 1, rest, seg_len);
				vpath[dlen + 1 + seg_len] = '\0';
				item->ino = sumh_vnode_vpath_ino(vpath);
				kfree(vpath);
			} else {
				kfree(item->name);
				kfree(item);
				count = -ENOMEM;
				break;
			}
		}
		sumh_name_index_add(&names, item);
		list_add_tail(&item->list, out);
		count++;
	}
	rcu_read_unlock();
	return count;
}

/* ======================================================================
 * Part 14: Hide Logic
 * ====================================================================== */

static bool sumh_hide_rule_matches(const char *pathname)
{
	struct sumh_hide_entry *he;
	u32 hash;
	size_t len;

	if (!pathname || !*pathname)
		return false;
	if (atomic_read(&sumh_hide_count) == 0)
		return false;

	len = strlen(pathname);
	{
		unsigned long bh1 =
		    jhash(pathname, (u32)len, 0) & (SUMH_BLOOM_SIZE - 1);
		unsigned long bh2 =
		    jhash(pathname, (u32)len, 1) & (SUMH_BLOOM_SIZE - 1);

		if (!test_bit(bh1, sumh_hide_bloom) ||
		    !test_bit(bh2, sumh_hide_bloom))
			return false;
	}

	hash = full_name_hash(NULL, pathname, len);
	rcu_read_lock();
	hlist_for_each_entry_rcu(
	    he, &sumh_hide_paths[hash_min(hash, SUMH_HASH_BITS)], node)
	{
		if (he->path_hash == hash && strcmp(he->path, pathname) == 0) {
			bool hidden = sumh_hide_scope_allowed(
			    READ_ONCE(he->storage_managed));

			rcu_read_unlock();
			return hidden;
		}
	}
	rcu_read_unlock();
	return false;
}

bool sumh_should_hide(const char *pathname)
{
	if (!sumh_hide_scope_allowed(false))
		return false;
	return sumh_hide_rule_matches(pathname);
}
