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
#include <uapi/linux/magic.h>
#include <asm/unistd.h>
#include "sumh_runtime.h"
#include "sumh_dirhijack.h"
#include "sumh_store.h"
#include "sumh_entrypoints.h"
#include "sumh_path_policy.h"
#include "sumh_overlay.h"
#include "sumh_vfs_hooks.h"
#include "sumh_proc_read_hooks.h"
#include "sumh_fake_mountinfo.h"
#include "sumh_iop_override.h"
#include "sumh_fop_override.h"

#define SUMH_MAGIC_POS 0x1000000000000000ULL

static bool sumh_iterate_injected_pos(loff_t pos)
{
	return (pos & 0xFFFFFFFF00000000ULL) == SUMH_MAGIC_POS;
}

void sumh_iterate_rewind_injected(struct dir_context *ctx)
{
	/* Rules can be removed while a directory fd retains an injection
	 * cookie. Never forward that private cookie to the native filesystem.
	 */
	if (ctx && sumh_iterate_injected_pos(ctx->pos))
		ctx->pos = 0;
}

SUMH_NOCFI void sumh_iterate_emit_injected(struct sumh_filldir_wrapper *w)
{
	struct list_head head;
	struct sumh_name_list *item, *tmp;
	struct inode *parent;
	loff_t pos = SUMH_MAGIC_POS;

	if (!w->view_allowed || !w->dir_has_inject || w->inject_done ||
	    !w->dir_path || !w->parent_dentry)
		return;

	w->inject_done = true;
	w->inject_pending = true;
	parent = d_inode(w->parent_dentry);
	INIT_LIST_HEAD(&head);
	sumh_populate_injected_list(w->dir_path, w->parent_dentry, &head);
	list_for_each_entry_safe (item, tmp, &head, list) {
		int nlen = strlen(item->name);

		if (parent && sumh_dirhijack_hidden(parent, item->name, nlen))
			goto free_item;
		/* Exact rules already owned by the specialized iterator appear
		 * after the native stream. Merge-only children stay in this
		 * list. */
		if (w->has_iterate_client && parent &&
		    sumh_dirhijack_emits(parent, item->name, nlen))
			goto free_item;
		if (pos++ < w->inject_pos || w->stopped)
			goto free_item;
		w->orig_ctx->pos = w->inject_pos;
		if (!w->orig_ctx->actor(w->orig_ctx, item->name, nlen,
					w->inject_pos, item->ino ?: 1,
					item->type)) {
			w->stopped = true;
			goto free_item;
		}
		w->inject_pos++;
		w->emitted++;
		atomic64_inc(&sumh_hook_stats.filldir_injected);
	free_item:
		list_del(&item->list);
		kfree(item->name);
		kfree(item);
	}
}

SUMH_NOCFI bool sumh_filldir_filter(struct dir_context *ctx, const char *name,
				    int namlen, loff_t offset, u64 ino,
				    unsigned int d_type)
{
	struct sumh_filldir_wrapper *w =
	    container_of(ctx, struct sumh_filldir_wrapper, wrap_ctx);
	bool ret;
	struct inode *parent =
	    w->parent_dentry ? d_inode(w->parent_dentry) : NULL;

	if (parent && sumh_dirhijack_hidden(parent, name, namlen))
		return true;
	/* The specialized client suppresses the native copy before reaching
	 * us. Its own emitted entry must not be removed as a merge duplicate.
	 */
	if (w->has_iterate_client && parent &&
	    sumh_dirhijack_emits(parent, name, namlen))
		goto passthrough;

	if (unlikely(namlen <= 2 && name[0] == '.')) {
		if (namlen == 1 || (namlen == 2 && name[1] == '.'))
			goto passthrough;
	}

	/* Hide real entries that also exist in merge targets. This prevents
	 * duplicates: the injected version (from populate_injected_list)
	 * replaces the original, just like original sumh.c does.
	 * Skip when merge target IS the dir we're listing (e.g. target path
	 * resolved to same inode via symlink) - otherwise we'd hide everything.
	 * This is independent of app-hide policy because explicit merge rules
	 * must not emit duplicate names even during root/manual validation. */
	if (w->view_allowed && sumh_d_hash_and_lookup &&
	    w->merge_target_count > 0 && w->parent_dentry) {
		int i;
		for (i = 0; i < w->merge_target_count; i++) {
			struct dentry *tgt = w->merge_target_dentries[i];
			if (!tgt || tgt == w->parent_dentry)
				continue;
			if (d_inode(tgt) &&
			    d_inode(tgt) == d_inode(w->parent_dentry))
				continue;
			{
				struct dentry *child = sumh_d_hash_and_lookup(
				    tgt, &(struct qstr)QSTR_INIT(name, namlen));
				if (child) {
					bool positive = d_inode(child) != NULL;

					dput(child);
					if (!positive)
						continue;
					atomic64_inc(
					    &sumh_hook_stats.filldir_hidden);
					return true;
				}
			}
		}
	}

	if (sumh_d_hash_and_lookup && w->dir_has_hidden && w->parent_dentry) {
		struct dentry *child;

		child = sumh_d_hash_and_lookup(
		    w->parent_dentry, &(struct qstr)QSTR_INIT(name, namlen));
		if (child) {
			struct inode *cinode = d_inode(child);
			if (cinode && cinode->i_mapping &&
			    test_bit(AS_FLAGS_SUMH_HIDE,
				     &cinode->i_mapping->flags)) {
				dput(child);
				atomic64_inc(&sumh_hook_stats.filldir_hidden);
				return true;
			}
			dput(child);
		}
	}

passthrough:
	if (unlikely(!w->orig_ctx || !w->orig_ctx->actor))
		return true;
	/* filldir writes this offset into the previous dirent. Preserve the
	 * injection/native boundary even when the first native cookie is zero,
	 * so seeking to the last injected dirent does not restart injection. */
	if (w->inject_pending && offset == 0)
		offset = w->inject_pos;
	w->orig_ctx->pos = ctx->pos;
	ret =
	    w->orig_ctx->actor(w->orig_ctx, name, namlen, offset, ino, d_type);
	ctx->pos = w->orig_ctx->pos;
	if (ret) {
		w->inject_pending = false;
		w->emitted++;
	} else {
		w->stopped = true;
	}
	return ret;
}

/* Caller holds RCU. Retired explicit rules can leave an installed wrapper
 * and an inode flag; only live redirects justify the generic projection. */
static bool sumh_kstat_has_redirect_source(struct inode *inode)
{
	struct sumh_entry *entry;
	int bucket;

	if (!inode)
		return false;
	hash_for_each_rcu(sumh_paths, bucket, entry, node)
	{
		if ((entry->source_path_valid &&
		     entry->source_inode == inode) ||
		    (entry->source_nofollow_path_valid &&
		     d_inode(entry->source_nofollow_path.dentry) == inode))
			return true;
	}
	return false;
}

/*
 * Apply explicit attributes or the existing redirect projection from shadow
 * getattr. A real inode is needed to establish ownership; NULL is a no-op.
 */
void sumh_apply_kstat_spoof(struct inode *inode, struct kstat *stat)
{
	struct sumh_spoof_kstat_entry *e = NULL;
	bool explicit_rule = false, managed_rule = false, redirect_source;

	if (!stat)
		return;
	if (sumh_vfs_internal_current())
		return;
	if (!sumh_policy_current_is_view_target())
		return;

	/* Explicit per-inode spoof rule (api15) takes precedence. */
	rcu_read_lock();
	if (inode && inode->i_sb &&
	    (atomic_read(&sumh_spoof_kstat_count) > 0 ||
	     rcu_access_pointer(sumh_managed_kstat))) {
		e = sumh_spoof_kstat_lookup_by_ino(
		    (unsigned long)inode->i_ino,
		    (unsigned long)inode->i_sb->s_dev);
		if (e) {
			explicit_rule = true;
			managed_rule = e->field_mask != 0;
			if (managed_rule
				? (e->field_mask & SUMH_KSTAT_FIELD_DEV)
				: e->spoofed_dev)
				stat->dev = e->spoofed_dev;
			if (managed_rule
				? (e->field_mask & SUMH_KSTAT_FIELD_INO)
				: e->spoofed_ino)
				stat->ino = e->spoofed_ino;
			if (managed_rule
				? (e->field_mask & SUMH_KSTAT_FIELD_NLINK)
				: e->spoofed_nlink)
				stat->nlink = e->spoofed_nlink;
			if (managed_rule
				? (e->field_mask & SUMH_KSTAT_FIELD_SIZE)
				: e->spoofed_size)
				stat->size = e->spoofed_size;
			if (managed_rule
				? (e->field_mask & SUMH_KSTAT_FIELD_BLKSIZE)
				: e->spoofed_blksize)
				stat->blksize = e->spoofed_blksize;
			if (managed_rule
				? (e->field_mask & SUMH_KSTAT_FIELD_BLOCKS)
				: e->spoofed_blocks)
				stat->blocks = e->spoofed_blocks;
			if (managed_rule
				? (e->field_mask & SUMH_KSTAT_FIELD_ATIME)
				: (e->spoofed_atime_sec ||
				   e->spoofed_atime_nsec)) {
				stat->atime.tv_sec = e->spoofed_atime_sec;
				stat->atime.tv_nsec = e->spoofed_atime_nsec;
			}
			if (managed_rule
				? (e->field_mask & SUMH_KSTAT_FIELD_MTIME)
				: (e->spoofed_mtime_sec ||
				   e->spoofed_mtime_nsec)) {
				stat->mtime.tv_sec = e->spoofed_mtime_sec;
				stat->mtime.tv_nsec = e->spoofed_mtime_nsec;
			}
			if (managed_rule
				? (e->field_mask & SUMH_KSTAT_FIELD_CTIME)
				: (e->spoofed_ctime_sec ||
				   e->spoofed_ctime_nsec)) {
				stat->ctime.tv_sec = e->spoofed_ctime_sec;
				stat->ctime.tv_nsec = e->spoofed_ctime_nsec;
			}
		}
	}
	redirect_source =
	    !explicit_rule && sumh_kstat_has_redirect_source(inode);
	rcu_read_unlock();
	if (!explicit_rule && !redirect_source)
		return;

	if (!explicit_rule) {
		dev_t system_dev = READ_ONCE(sumh_system_dev);

		/* Generic fallback: add_rule redirect targets reaching the
		 * getattr path.  Publish the SAME synthetic ino every other
		 * surface emits: sumh_vnode_source_ino() resolves the rule's
		 * allocated inode from the resolved source (dev,ino) the kstat
		 * already carries, so stat here == syscall stat == getdents ==
		 * /proc/maps (and no 19-digit bit-63 tell).  Compute it BEFORE
		 * overwriting stat->dev.
		 *
		 * dev stays the captured /system dev: this callback has no
		 * trustworthy visible path (its @path may be the rewritten
		 * target, whose sb dev would leak), and this holdout only fires
		 * for /system redirect targets where the visible dev already IS
		 * the /system dev. */
		stat->ino = sumh_vnode_source_ino(stat->dev, (u64)stat->ino);
		if (system_dev)
			stat->dev = system_dev;
		if (S_ISREG(stat->mode))
			stat->nlink = 1;
	}

	if (!managed_rule) {
		stat->uid = GLOBAL_ROOT_UID;
		stat->gid = GLOBAL_ROOT_GID;
	}

	sumh_log("kstat: spoofed ino %lu (explicit=%d)\n",
		 (unsigned long)stat->ino, explicit_rule);

	if (inode && inode->i_mapping)
		set_bit(AS_FLAGS_SUMH_SPOOF_KSTAT, &inode->i_mapping->flags);
}

SUMH_NOCFI struct sumh_filldir_wrapper *
sumh_iterate_prepare_wrapper(struct file *file, struct dir_context *orig_ctx)
{
	struct sumh_filldir_wrapper *w;
	struct dentry *parent;
	struct inode *dir_inode;
	const char *dname;
	enum sumh_policy_scope scope;
	bool dir_has_hidden = false, dir_has_inject = false;
	int dir_path_len = 0;

	if (atomic_long_read(&sumh_ioctl_tgid) == (long)task_tgid_vnr(current))
		return NULL;
	if (!READ_ONCE(sumh_enabled))
		return NULL;
	if (atomic_long_read(&sumh_ioctl_tgid) > 0 &&
	    task_tgid_vnr(current) == atomic_long_read(&sumh_ioctl_tgid))
		return NULL;
	if (!orig_ctx || !orig_ctx->actor)
		return NULL;
	if (orig_ctx->actor == sumh_filldir_filter ||
	    sumh_is_merge_context(orig_ctx))
		return NULL;
	scope = sumh_policy_current_scope();
	if (scope == SUMH_POLICY_SCOPE_NONE)
		return NULL;

	parent = file ? file->f_path.dentry : NULL;
	if (parent) {
		dir_inode = d_inode(parent);
		if (dir_inode && dir_inode->i_mapping) {
			dir_has_hidden =
			    test_bit(AS_FLAGS_SUMH_DIR_HAS_HIDDEN,
				     &dir_inode->i_mapping->flags) &&
			    sumh_policy_current_is_hide_target(dir_inode);
			dir_has_inject = scope == SUMH_POLICY_SCOPE_VIEW &&
					 test_bit(AS_FLAGS_SUMH_DIR_HAS_INJECT,
						  &dir_inode->i_mapping->flags);
		}
		dname = parent->d_name.name;
		if (dname[0] == 'd' && dname[1] == 'e' && dname[2] == 'v' &&
		    dname[3] == '\0')
			dir_path_len = 4;
	}
	/* Most directories need no filtering. Decide before allocating and
	 * zeroing the wrapper (including its path buffer and merge targets). */
	if (!dir_has_hidden && !dir_has_inject &&
	    (scope != SUMH_POLICY_SCOPE_SPOOF || dir_path_len != 4))
		return NULL;

	w = kmem_cache_zalloc(sumh_filldir_cache, GFP_ATOMIC);
	if (!w)
		return NULL;

	w->orig_ctx = orig_ctx;
	w->wrap_ctx.actor = sumh_filldir_filter;
	w->wrap_ctx.pos = orig_ctx->pos;
	/* The native iterator always starts at its own zero cookie while the
	 * caller is still draining the separately indexed injected entries. */
	if (sumh_iterate_injected_pos(orig_ctx->pos))
		w->wrap_ctx.pos = 0;
	w->view_allowed = scope == SUMH_POLICY_SCOPE_VIEW;
	w->spoof_allowed = scope == SUMH_POLICY_SCOPE_SPOOF;
	w->parent_dentry = parent;
	w->inject_done = w->wrap_ctx.pos != 0;
	w->inject_pos = sumh_iterate_injected_pos(orig_ctx->pos)
			    ? orig_ctx->pos
			    : SUMH_MAGIC_POS;
	w->dir_has_hidden = dir_has_hidden;
	w->dir_has_inject = dir_has_inject;
	w->dir_path_len = dir_path_len;

	if (w->parent_dentry) {
		/*
		 * Only when dir_has_inject (from flag) is true: build full path
		 * and traverse hash to get merge_target_dentries. Most dirs
		 * skip this.
		 */
		if (w->view_allowed && atomic_read(&sumh_rule_count) > 0 &&
		    w->dir_has_inject) {
			char *buf = w->dir_path_buf;
			char *dp = ERR_PTR(-ENOENT);

			if (sumh_d_absolute_path)
				dp = sumh_d_absolute_path(
				    &file->f_path, buf, SUMH_ITERATE_PATH_BUF);
			if (IS_ERR(dp))
				dp = dentry_path_raw(w->parent_dentry, buf,
						     SUMH_ITERATE_PATH_BUF);

			if (!IS_ERR_OR_NULL(dp) && *dp == '/') {
				struct sumh_inject_entry *ie;
				struct sumh_merge_entry *me;
				u32 h;
				int mbkt;
				size_t plen = strlen(dp);

				if (plen < SUMH_ITERATE_PATH_BUF) {
					memmove(w->dir_path_buf, dp, plen + 1);
					w->dir_path = w->dir_path_buf;
					dp = w->dir_path_buf;
				}
				h = full_name_hash(NULL, dp, strlen(dp));

				rcu_read_lock();
				hlist_for_each_entry_rcu(
				    ie,
				    &sumh_inject_dirs[hash_min(h,
							       SUMH_HASH_BITS)],
				    node)
				{
					if (strcmp(ie->dir, dp) == 0) {
						w->dir_has_inject = true;
						break;
					}
				}
				/* Scan all merge entries (few) to match both
				 * src and resolved_src; cache target dentries.
				 */
				hash_for_each_rcu(sumh_merge_dirs, mbkt, me,
						  node)
				{
					if (strcmp(me->src, dp) == 0 ||
					    (me->resolved_src &&
					     strcmp(me->resolved_src, dp) ==
						 0)) {
						w->dir_has_inject = true;
						/* The iterator can sleep after
						 * RCU unlock; retain the target
						 * across a concurrent CLEAR.
						 */
						if (me->target_dentry &&
						    w->merge_target_count <
							SUMH_MAX_MERGE_TARGETS)
							w->merge_target_dentries
							    [w->merge_target_count++] =
							    dget(
								me->target_dentry);
					}
				}
				rcu_read_unlock();
			}
		}
	}

	return w;
}

void sumh_iterate_finish_wrapper(struct sumh_filldir_wrapper *wrapper)
{
	int i;

	if (!wrapper)
		return;
	if (wrapper->orig_ctx) {
		wrapper->orig_ctx->pos =
		    wrapper->inject_pending && !wrapper->wrap_ctx.pos
			? wrapper->inject_pos
			: wrapper->wrap_ctx.pos;
	}
	for (i = 0; i < wrapper->merge_target_count; i++)
		dput(wrapper->merge_target_dentries[i]);
	kmem_cache_free(sumh_filldir_cache, wrapper);
}
