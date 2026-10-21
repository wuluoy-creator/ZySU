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
	struct list_head *head;
	struct sumh_name_list *item;
	struct inode *parent;
	loff_t pos = SUMH_MAGIC_POS;

	if (!w->view_allowed || !w->dir_has_inject || w->inject_done ||
	    !w->merge_snapshot || !w->parent_dentry)
		return;

	w->inject_done = true;
	w->inject_pending = true;
	parent = d_inode(w->parent_dentry);
	head = sumh_merge_snapshot_names(w->merge_snapshot);
	list_for_each_entry (item, head, list) {
		int nlen = strlen(item->name);

		if (parent && sumh_dirhijack_hidden(parent, item->name, nlen))
			continue;
		/* Exact rules already owned by the specialized iterator appear
		 * after the native stream. Merge-only children stay in this
		 * list. */
		if (w->has_iterate_client && parent &&
		    sumh_dirhijack_emits(parent, item->name, nlen))
			continue;
		if (pos++ < w->inject_pos || w->stopped)
			continue;
		w->orig_ctx->pos = w->inject_pos;
		if (!w->orig_ctx->actor(w->orig_ctx, item->name, nlen,
					w->inject_pos, item->ino ?: 1,
					item->type)) {
			w->stopped = true;
			continue;
		}
		w->inject_pos++;
		w->emitted++;
		atomic64_inc(&sumh_hook_stats.filldir_injected);
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

	/* Coverage and injected entries come from the same complete snapshot,
	 * including targets beyond the former fixed array and cold dentries. */
	if (w->view_allowed &&
	    sumh_merge_snapshot_covers(w->merge_snapshot, name, namlen)) {
		atomic64_inc(&sumh_hook_stats.filldir_hidden);
		return true;
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
	bool explicit_rule = false, redirect_source;

	if (!stat)
		return;
	if (sumh_vfs_internal_current())
		return;
	if (!sumh_policy_current_is_view_target())
		return;

	/* Explicit per-inode spoof rule (api15) takes precedence. */
	rcu_read_lock();
	if (inode && inode->i_sb && atomic_read(&sumh_spoof_kstat_count) > 0) {
		e = sumh_spoof_kstat_lookup_by_ino(
		    (unsigned long)inode->i_ino,
		    (unsigned long)inode->i_sb->s_dev);
		if (e) {
			explicit_rule = true;
			if (e->spoofed_dev)
				stat->dev = e->spoofed_dev;
			if (e->spoofed_ino)
				stat->ino = e->spoofed_ino;
			if (e->spoofed_nlink)
				stat->nlink = e->spoofed_nlink;
			if (e->spoofed_size)
				stat->size = e->spoofed_size;
			if (e->spoofed_blksize)
				stat->blksize = e->spoofed_blksize;
			if (e->spoofed_blocks)
				stat->blocks = e->spoofed_blocks;
			if (e->spoofed_atime_sec || e->spoofed_atime_nsec) {
				stat->atime.tv_sec = e->spoofed_atime_sec;
				stat->atime.tv_nsec = e->spoofed_atime_nsec;
			}
			if (e->spoofed_mtime_sec || e->spoofed_mtime_nsec) {
				stat->mtime.tv_sec = e->spoofed_mtime_sec;
				stat->mtime.tv_nsec = e->spoofed_mtime_nsec;
			}
			if (e->spoofed_ctime_sec || e->spoofed_ctime_nsec) {
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

	stat->uid = GLOBAL_ROOT_UID;
	stat->gid = GLOBAL_ROOT_GID;

	sumh_log("kstat: spoofed ino %lu (explicit=%d)\n",
		 (unsigned long)stat->ino, explicit_rule);

	if (inode && inode->i_mapping)
		set_bit(AS_FLAGS_SUMH_SPOOF_KSTAT, &inode->i_mapping->flags);
}

/* Preparation failures must release storage without publishing a cursor. */
static void sumh_iterate_free_wrapper(struct sumh_filldir_wrapper *wrapper)
{
	sumh_merge_snapshot_destroy(wrapper->merge_snapshot);
	kfree(wrapper->dir_path);
	kmem_cache_free(sumh_filldir_cache, wrapper);
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
	int ret;

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
	/* Most directories need no filtering. Decide before allocating the
	 * wrapper; only injection needs a separately allocated path buffer. */
	if (!dir_has_hidden && !dir_has_inject &&
	    (scope != SUMH_POLICY_SCOPE_SPOOF || !sumh_stealth_enabled ||
	     dir_path_len != 4))
		return NULL;

	/* This is a sleepable fop callback, outside the caller's RCU lookup. */
	w = kmem_cache_zalloc(sumh_filldir_cache, GFP_KERNEL);
	if (!w)
		return ERR_PTR(-ENOMEM);

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
		 * Only marked VIEW directories with live rules need the path
		 * and complete per-call snapshot, including native-cookie
		 * resumes.
		 */
		if (w->view_allowed && atomic_read(&sumh_rule_count) > 0 &&
		    w->dir_has_inject) {
			char *dp;
			size_t offset, plen;

			w->dir_path =
			    kmalloc(SUMH_ITERATE_PATH_BUF, GFP_KERNEL);
			if (!w->dir_path) {
				ret = -ENOMEM;
				goto fail;
			}

			if (sumh_d_absolute_path)
				dp = sumh_d_absolute_path(
				    &file->f_path, w->dir_path,
				    SUMH_ITERATE_PATH_BUF);
			else
				dp = d_path(&file->f_path, w->dir_path,
					    SUMH_ITERATE_PATH_BUF);
			/* A primary resolver error must not become a shorter
			 * raw dentry path with a different mount-relative
			 * identity. */
			if (IS_ERR(dp)) {
				ret = PTR_ERR(dp);
				goto fail;
			}
			/* Validate the returned span before reading it.
			 * Unsigned subtraction also rejects pointers preceding
			 * the buffer. */
			offset = (unsigned long)dp - (unsigned long)w->dir_path;
			if (!dp || offset >= SUMH_ITERATE_PATH_BUF) {
				ret = -EIO;
				goto fail;
			}
			plen = strnlen(dp, SUMH_ITERATE_PATH_BUF - offset);
			if (plen == SUMH_ITERATE_PATH_BUF - offset) {
				ret = -ENAMETOOLONG;
				goto fail;
			}
			if (*dp != '/') {
				ret = -EIO;
				goto fail;
			}
			memmove(w->dir_path, dp, plen + 1);
			dp = w->dir_path;
			ret = sumh_merge_snapshot_create(file, dp,
							 &w->merge_snapshot);
			if (ret)
				goto fail;
		}
	}

	return w;
fail:
	sumh_iterate_free_wrapper(w);
	return ERR_PTR(ret);
}

void sumh_iterate_finish_wrapper(struct sumh_filldir_wrapper *wrapper)
{
	if (!wrapper)
		return;
	if (wrapper->orig_ctx) {
		wrapper->orig_ctx->pos =
		    wrapper->inject_pending && !wrapper->wrap_ctx.pos
			? wrapper->inject_pos
			: wrapper->wrap_ctx.pos;
	}
	sumh_iterate_free_wrapper(wrapper);
}
