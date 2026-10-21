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
#include "sumh_store.h"
#include "sumh_path_policy.h"
#include "sumh_overlay.h"
#include "sumh_iop_override.h"
#include "sumh_dirhijack.h"
#include "sumh_vnode.h"
#include "sumh_name_index.h"
/* ======================================================================
 * Part 10b: Inject - populate list for merge/add rule dirs
 * ====================================================================== */

struct sumh_merge_target {
	struct list_head list;
	struct path path;
	struct file *file;
};

struct sumh_merge_snapshot {
	struct list_head targets, emitted_names, covered_names;
	struct rb_root emitted_index, covered_index;
	char *secondary_path;
	size_t requested_bytes;
	unsigned int target_count, unique_names, scanned_records, passes;
};

struct sumh_merge_raw_name {
	struct list_head list;
	u64 ino;
	unsigned int len, type;
	bool projected;
	char name[];
};

struct sumh_merge_ctx {
	struct dir_context ctx;
	struct sumh_merge_snapshot *snapshot;
	struct list_head *raw_names;
	unsigned int pass_records;
	bool projected;
	int error;
};

static void *sumh_merge_alloc(struct sumh_merge_snapshot *s, size_t size,
			      gfp_t gfp)
{
	void *p;

	if (s->requested_bytes > SUMH_MERGE_MEMORY_BUDGET ||
	    size > SUMH_MERGE_MEMORY_BUDGET - s->requested_bytes)
		return ERR_PTR(-E2BIG);
	p = kmalloc(size, gfp);
	if (!p)
		return ERR_PTR(-ENOMEM);
	s->requested_bytes += size;
	return p;
}

static void sumh_merge_free(struct sumh_merge_snapshot *s, void *p, size_t size)
{
	if (!p)
		return;
	s->requested_bytes -= size;
	kfree(p);
}

static bool sumh_merge_valid_name(const char *name, int len)
{
	return name && len > 0 && len <= NAME_MAX && !memchr(name, '\0', len) &&
	       !memchr(name, '/', len);
}

static int sumh_merge_add_name(struct sumh_merge_snapshot *s,
			       struct list_head *head, struct rb_root *index,
			       const char *name, unsigned int len, u64 ino,
			       unsigned int type, gfp_t gfp)
{
	struct sumh_name_list *item;
	char *copy;

	if (!sumh_merge_valid_name(name, len))
		return -EIO;
	if (sumh_name_index_contains(index, name, len))
		return 0;
	if (s->unique_names >= SUMH_MERGE_MAX_NAMES)
		return -E2BIG;
	item = sumh_merge_alloc(s, sizeof(*item), gfp);
	if (IS_ERR(item))
		return PTR_ERR(item);
	copy = sumh_merge_alloc(s, len + 1, gfp);
	if (IS_ERR(copy)) {
		sumh_merge_free(s, item, sizeof(*item));
		return PTR_ERR(copy);
	}
	memcpy(copy, name, len);
	copy[len] = '\0';
	item->name = copy;
	item->ino = ino;
	item->type = (unsigned char)type;
	sumh_name_index_add(index, item);
	list_add(&item->list, head);
	s->unique_names++;
	return 0;
}

static const char *sumh_direct_child_name(const char *path, const char *dir,
					  size_t dir_len)
{
	const char *name;

	if (!path || !dir || !dir_len)
		return NULL;
	if (dir_len == 1 && dir[0] == '/') {
		if (path[0] != '/' || !path[1])
			return NULL;
		name = path + 1;
	} else {
		if (strncmp(path, dir, dir_len) != 0 || path[dir_len] != '/' ||
		    !path[dir_len + 1])
			return NULL;
		name = path + dir_len + 1;
	}
	return strchr(name, '/') ? NULL : name;
}

static SUMH_NOCFI bool sumh_merge_filldir(struct dir_context *ctx,
					  const char *name, int namlen,
					  loff_t offset, u64 ino,
					  unsigned int d_type)
{
	struct sumh_merge_ctx *mctx =
	    container_of(ctx, struct sumh_merge_ctx, ctx);
	struct sumh_merge_snapshot *s = mctx->snapshot;
	struct sumh_merge_raw_name *raw;

	(void)offset;
	if (mctx->error)
		return false;
	if (s->scanned_records >= SUMH_MERGE_MAX_RECORDS) {
		mctx->error = -E2BIG;
		return false;
	}
	s->scanned_records++;
	mctx->pass_records++;
	if (!sumh_merge_valid_name(name, namlen)) {
		mctx->error = -EIO;
		return false;
	}
	if ((namlen == 1 && name[0] == '.') ||
	    (namlen == 2 && name[0] == '.' && name[1] == '.'))
		return true;
	raw = sumh_merge_alloc(s, sizeof(*raw) + namlen + 1, GFP_KERNEL);
	if (IS_ERR(raw)) {
		mctx->error = PTR_ERR(raw);
		return false;
	}
	raw->ino = ino;
	raw->len = namlen;
	raw->type = d_type;
	raw->projected = mctx->projected;
	memcpy(raw->name, name, namlen);
	raw->name[namlen] = '\0';
	list_add_tail(&raw->list, mctx->raw_names);
	return true;
}

bool sumh_is_merge_context(const struct dir_context *ctx)
{
	/* Actor identity survives sleeps and migration. */
	return ctx && ctx->actor == sumh_merge_filldir;
}

bool sumh_emit_projected_dirent(struct dir_context *ctx, const char *name,
				int namlen, u64 ino, unsigned int type)
{
	struct sumh_merge_ctx *mctx;
	bool before, ret;

	if (!sumh_is_merge_context(ctx))
		return dir_emit(ctx, name, namlen, ino, type);
	mctx = container_of(ctx, struct sumh_merge_ctx, ctx);
	before = mctx->projected;
	mctx->projected = true;
	ret = dir_emit(ctx, name, namlen, ino, type);
	mctx->projected = before;
	return ret;
}

static void sumh_merge_free_names(struct sumh_merge_snapshot *s,
				  struct list_head *head)
{
	struct sumh_name_list *item, *next;

	list_for_each_entry_safe (item, next, head, list) {
		list_del(&item->list);
		sumh_merge_free(s, item->name, item->name_len + 1);
		sumh_merge_free(s, item, sizeof(*item));
	}
}

void sumh_merge_snapshot_destroy(struct sumh_merge_snapshot *s)
{
	struct sumh_merge_target *target, *next;

	if (!s)
		return;
	/* No tree is queried while its owned list nodes are freed. */
	sumh_merge_free_names(s, &s->emitted_names);
	sumh_merge_free_names(s, &s->covered_names);
	list_for_each_entry_safe (target, next, &s->targets, list) {
		list_del(&target->list);
		if (target->file)
			fput(target->file);
		path_put(&target->path);
		sumh_merge_free(s, target, sizeof(*target));
	}
	sumh_merge_free(s, s->secondary_path, PATH_MAX);
	kfree(s);
}

struct list_head *sumh_merge_snapshot_names(struct sumh_merge_snapshot *s)
{
	return &s->emitted_names;
}

bool sumh_merge_snapshot_covers(const struct sumh_merge_snapshot *s,
				const char *name, unsigned int len)
{
	return s && sumh_name_index_contains(&s->covered_index, name, len);
}

static int sumh_merge_capture(struct sumh_merge_snapshot *s,
			      const char *primary, const char *secondary)
{
	struct sumh_inject_entry *inject;
	struct sumh_merge_entry *merge;
	struct sumh_entry *entry;
	bool should_inject = false;
	size_t primary_len = strlen(primary), secondary_len = strlen(secondary);
	int bkt, ret = 0;
	u32 hash = full_name_hash(NULL, primary, primary_len);
	u32 secondary_hash = full_name_hash(NULL, secondary, secondary_len);

	rcu_read_lock();
	hlist_for_each_entry_rcu(
	    inject, &sumh_inject_dirs[hash_min(hash, SUMH_HASH_BITS)], node)
	{
		if (!strcmp(inject->dir, primary)) {
			should_inject = true;
			break;
		}
	}
	if (!should_inject) {
		hlist_for_each_entry_rcu(
		    inject,
		    &sumh_inject_dirs[hash_min(secondary_hash, SUMH_HASH_BITS)],
		    node)
		{
			if (!strcmp(inject->dir, secondary)) {
				should_inject = true;
				break;
			}
		}
	}
	hash_for_each_rcu(sumh_merge_dirs, bkt, merge, node)
	{
		struct sumh_merge_target *target;

		if (strcmp(merge->src, primary) &&
		    strcmp(merge->src, secondary) &&
		    (!merge->resolved_src ||
		     (strcmp(merge->resolved_src, primary) &&
		      strcmp(merge->resolved_src, secondary))))
			continue;
		should_inject = true;
		if (s->target_count >= SUMH_MERGE_MAX_TARGETS) {
			ret = -E2BIG;
			goto out;
		}
		if (!merge->target_path.dentry || !merge->target_path.mnt) {
			ret = -ESTALE;
			goto out;
		}
		target = sumh_merge_alloc(s, sizeof(*target), GFP_ATOMIC);
		if (IS_ERR(target)) {
			ret = PTR_ERR(target);
			goto out;
		}
		/* RCU protects the record's owned path while acquiring our own
		 * mount+dentry reference. Open after leaving the read section.
		 */
		target->path = merge->target_path;
		path_get(&target->path);
		target->file = NULL;
		list_add_tail(&target->list, &s->targets);
		s->target_count++;
	}
	if (!should_inject)
		goto out;
	hash_for_each_rcu(sumh_paths, bkt, entry, node)
	{
		const char *name;
		size_t len;
		unsigned int type;

		name = sumh_direct_child_name(entry->src, primary, primary_len);
		if (!name)
			name = sumh_direct_child_name(entry->src, secondary,
						      secondary_len);
		if (!name)
			continue;
		len = strnlen(name, NAME_MAX + 1);
		if ((len == 1 && name[0] == '.') ||
		    (len == 2 && name[0] == '.' && name[1] == '.'))
			continue;
		type = entry->source_stat_valid
			   ? (unsigned int)((entry->source_mode & S_IFMT) >> 12)
			   : entry->type;
		ret = sumh_merge_add_name(s, &s->emitted_names,
					  &s->emitted_index, name, len,
					  entry->visible_ino, type, GFP_ATOMIC);
		if (ret)
			goto out;
	}
out:
	rcu_read_unlock();
	return ret;
}

static int sumh_merge_classify_char(struct file *file,
				    const struct sumh_merge_raw_name *raw,
				    bool *whiteout)
{
	struct inode *dir = file_inode(file), *inode;
	struct dentry *child;
	int ret = 0;

	/* A projected identity is not a raw inode, even on numeric coincidence.
	 */
	if (raw->projected || !sumh_lookup_one_len)
		return -EOPNOTSUPP;
	/* Do not add a blocking write-lock wait under the outer source lock. */
	if (!inode_trylock(dir))
		return -EAGAIN;
	child = sumh_lookup_one_len(raw->name, file->f_path.dentry, raw->len);
	if (IS_ERR(child)) {
		ret = PTR_ERR(child);
		goto unlock;
	}
	inode = d_inode(child);
	if (!inode || !S_ISCHR(inode->i_mode) || (u64)inode->i_ino != raw->ino)
		ret = -ESTALE;
	else
		*whiteout = inode->i_rdev == 0;
	dput(child);
unlock:
	inode_unlock(dir);
	return ret;
}

static int sumh_merge_collect_target(struct sumh_merge_snapshot *s,
				     struct sumh_merge_target *target)
{
	LIST_HEAD(raw_names);
	struct sumh_merge_ctx mctx = {
	    .ctx.actor = sumh_merge_filldir,
	    .snapshot = s,
	    .raw_names = &raw_names,
	};
	struct sumh_merge_raw_name *raw, *next;
	struct inode *inode = file_inode(target->file);
	dev_t dev = inode && inode->i_sb ? inode->i_sb->s_dev : 0;
	int ret = 0;

	for (;;) {
		loff_t before = target->file->f_pos;

		if (s->passes >= SUMH_MERGE_MAX_PASSES) {
			ret = -E2BIG;
			goto out;
		}
		s->passes++;
		mctx.pass_records = 0;
		ret = iterate_dir(target->file, &mctx.ctx);
		if (mctx.error) {
			ret = mctx.error;
			goto out;
		}
		if (ret < 0)
			goto out;
		if (target->file->f_pos == before) {
			if (mctx.pass_records)
				ret = -EIO;
			break;
		}
		cond_resched();
	}
	if (ret < 0)
		goto out;
	list_for_each_entry (raw, &raw_names, list) {
		bool whiteout = false;
		bool marker =
		    raw->len == 8 && !memcmp(raw->name, ".replace", 8);
		u64 ino;

		/* Marker semantics do not depend on inode type or provenance.
		 */
		if (!marker && raw->type == DT_CHR) {
			ret = sumh_merge_classify_char(target->file, raw,
						       &whiteout);
			if (ret)
				goto out;
		}
		ret = sumh_merge_add_name(s, &s->covered_names,
					  &s->covered_index, raw->name,
					  raw->len, 0, raw->type, GFP_KERNEL);
		if (ret)
			goto out;
		if (whiteout || marker)
			continue;
		ino = raw->projected || !dev
			  ? raw->ino
			  : sumh_vnode_source_ino(dev, raw->ino);
		ret = sumh_merge_add_name(s, &s->emitted_names,
					  &s->emitted_index, raw->name,
					  raw->len, ino, raw->type, GFP_KERNEL);
		if (ret)
			goto out;
	}
	ret = 0;
out:
	list_for_each_entry_safe (raw, next, &raw_names, list) {
		list_del(&raw->list);
		sumh_merge_free(s, raw, sizeof(*raw) + raw->len + 1);
	}
	return ret;
}

int sumh_merge_snapshot_create(struct file *source, const char *primary,
			       struct sumh_merge_snapshot **out)
{
	struct sumh_merge_snapshot *s;
	struct sumh_merge_target *target;
	char *secondary;
	size_t offset, len;
	int ret;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!source || !file_inode(source) || !primary)
		return -EINVAL;
	if (sizeof(*s) + PATH_MAX > SUMH_MERGE_MEMORY_BUDGET)
		return -E2BIG;
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	INIT_LIST_HEAD(&s->targets);
	INIT_LIST_HEAD(&s->emitted_names);
	INIT_LIST_HEAD(&s->covered_names);
	s->emitted_index = RB_ROOT;
	s->covered_index = RB_ROOT;
	/* Charge the caller-owned primary path for its overlapping lifetime. */
	s->requested_bytes = sizeof(*s) + PATH_MAX;
	secondary = sumh_merge_alloc(s, PATH_MAX, GFP_KERNEL);
	if (IS_ERR(secondary)) {
		ret = PTR_ERR(secondary);
		goto fail;
	}
	s->secondary_path = secondary;
	secondary = d_path(&source->f_path, s->secondary_path, PATH_MAX);
	if (IS_ERR(secondary)) {
		ret = PTR_ERR(secondary);
		goto fail;
	}
	offset = (unsigned long)secondary - (unsigned long)s->secondary_path;
	if (!secondary || offset >= PATH_MAX) {
		ret = -EIO;
		goto fail;
	}
	len = strnlen(secondary, PATH_MAX - offset);
	if (len == PATH_MAX - offset) {
		ret = -ENAMETOOLONG;
		goto fail;
	}
	if (*secondary != '/') {
		ret = -EIO;
		goto fail;
	}
	ret = sumh_merge_capture(s, primary, secondary);
	if (ret)
		goto fail;
	/* Open the rule's captured identity, even if its old pathname now names
	 * a replacement directory. Never recheck enable/rule_count halfway
	 * through the acquired view. */
	list_for_each_entry (target, &s->targets, list) {
		struct inode *inode;
		const struct cred *cred;
		struct file *file;

		inode = d_inode(target->path.dentry);
		if (!inode || !S_ISDIR(inode->i_mode))
			ret = -ENOTDIR;
		else if (sumh_vnode_is_ours(inode))
			ret = -EOPNOTSUPP;
		else if (inode == file_inode(source))
			continue;
		if (ret)
			goto fail;
		cred = get_task_cred(&init_task);
		file = dentry_open(&target->path, O_RDONLY | O_DIRECTORY, cred);
		put_cred(cred);
		if (IS_ERR(file)) {
			ret = PTR_ERR(file);
			goto fail;
		}
		target->file = file;
		ret = sumh_merge_collect_target(s, target);
		if (ret)
			goto fail;
	}
	*out = s;
	return 0;
fail:
	sumh_merge_snapshot_destroy(s);
	return ret;
}

/* Materialize merge targets into native path rules in sleepable context. */

static SUMH_NOCFI int sumh_add_path_entry(const char *src_prefix,
					  const char *src, const char *tgt,
					  unsigned char type,
					  const struct path *leaf)
{
	struct sumh_entry *entry;
	struct path source, native_parent = {};
	struct sumh_dh_spec spec = {.kind = SUMH_DH_OWNER_SHADOW};
	unsigned long ino;
	u8 flags = 0;
	int ret = -ENOMEM;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	entry->src = kstrdup(src, GFP_KERNEL);
	entry->target = kstrdup(tgt, GFP_KERNEL);
	entry->src_hash = full_name_hash(NULL, src, strlen(src));
	entry->type = type;
	if (!entry->src || !entry->target)
		goto fail;
	/* Non-links come directly from the held source directory. Native final
	 * symlink following retains its process-root and security semantics;
	 * checks before and after following reject observable lexical-leaf
	 * replacements, without an atomic or ABA-safe follow/nofollow pair. */
	if (S_ISLNK(d_inode(leaf->dentry)->i_mode))
		ret = sumh_entry_capture_source_checked(entry, tgt, leaf);
	else
		ret = sumh_entry_capture_resolved(entry, leaf);
	if (ret)
		goto fail;
	if (entry->source_nofollow_path_valid) {
		source = entry->source_nofollow_path;
		ino = entry->nofollow_visible_ino;
		flags = SUMH_VNODE_F_LNK;
	} else {
		source = entry->source_path;
		ino = entry->visible_ino;
		if (S_ISDIR(entry->source_mode))
			flags = SUMH_VNODE_F_DIR;
		else if (S_ISCHR(entry->source_mode) &&
			 !d_inode(source.dentry)->i_rdev) {
			ret = -EOPNOTSUPP;
			goto fail;
		} else if (S_ISCHR(entry->source_mode) ||
			   S_ISBLK(entry->source_mode) ||
			   S_ISFIFO(entry->source_mode))
			flags = SUMH_VNODE_F_SPECIAL;
	}
	spec.source = &source;
	spec.v_ino = ino;
	spec.flags = flags;
	ret =
	    sumh_dh_prepare_path(src, &spec, &entry->dh_claim, &native_parent);
	if (ret)
		goto fail;
	/* src is join(src_prefix, one component): both ownership services use
	 * this exact visible parent, never the source-side iteration parent. */
	ret = sumh_inject_prepare_observed(src_prefix, &native_parent,
					   SUMH_INJECT_MISSING_NONE,
					   &entry->inject_claim);
	path_put(&native_parent);
	if (ret)
		goto fail;
	path_get(&source);
	ret = sumh_store_upsert(entry, NULL);
	if (ret) {
		path_put(&source);
		goto fail;
	}
	sumh_inject_finish_prepare(entry->inject_claim);
	/* Each child is complete; previous successful MERGE children remain
	 * published if a later child's preparation fails. */
	(void)sumh_iop_mark_spoof(d_inode(source.dentry));
	path_put(&source);
	return ret;
fail:
	sumh_dh_abort(entry->dh_claim);
	sumh_inject_destroy(entry->inject_claim);
	sumh_entry_release_source(entry);
	kfree(entry->src);
	kfree(entry->target);
	kfree(entry->source_canonical);
	kfree(entry);
	return ret;
}

static SUMH_NOCFI int sumh_validate_merge_target(const struct path *path)
{
	struct inode *inode;
	struct dentry *marker;
	int ret;

	if (!path || !path->dentry || !path->mnt)
		return -EINVAL;
	if (!sumh_lookup_one_len)
		return -EOPNOTSUPP;
	inode = d_inode(path->dentry);
	if (!inode || !S_ISDIR(inode->i_mode))
		return -ENOTDIR;
	if (sumh_vnode_is_ours(inode))
		return -EOPNOTSUPP;
	/* Validation never waits behind a concurrent directory mutation. */
	if (!inode_trylock(inode))
		return -EAGAIN;
	marker = sumh_lookup_one_len(".replace", path->dentry, 8);
	if (IS_ERR(marker)) {
		ret = PTR_ERR(marker);
	} else {
		ret = d_inode(marker) ? -EOPNOTSUPP : 0;
		dput(marker);
	}
	inode_unlock(inode);
	return ret;
}

int SUMH_NOCFI sumh_capture_merge_target(const char *target, struct path *out)
{
	struct path path;
	int ret;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (!target || !*target)
		return -EINVAL;
	ret = kern_path(target, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &path);
	if (ret)
		return ret;
	ret = sumh_validate_merge_target(&path);
	if (ret) {
		path_put(&path);
		return ret;
	}
	*out = path;
	return 0;
}

/* Register a nested merge_entry from within materialize (process context).
 * On success out owns the rule's pinned root for recursive materialization,
 * including when an identical rule already exists. Strings are borrowed.
 *
 * Used so that DT_DIR children discovered while materializing a parent merge
 * become their own merge rules, instead of being registered as DT_DIR
 * entries in sumh_paths. The latter would cause exact path redirect to
 * wholesale-redirect any lookup of that subdir to the module's (typically
 * incomplete) copy, destroying real subdir content. A nested merge rule, by
 * contrast, keeps the real subdir intact and only performs iterate_dir-time
 * injection of the module's contents on top.
 */
static SUMH_NOCFI int sumh_register_nested_merge(const char *src_str,
						 const char *target_str,
						 const struct path *captured,
						 struct path *out)
{
	struct sumh_merge_entry *me = NULL, *existing;
	struct path target_path = {};
	u32 hash;
	int ret;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (!src_str || !target_str)
		return -EINVAL;
	/* Reuse an existing record before consulting a pathname that may now
	 * be missing or describe a different directory. The mutex protects its
	 * owned reference while we acquire the recursive caller's reference. */
	hash = full_name_hash(NULL, src_str, strlen(src_str));
	mutex_lock(&sumh_config_mutex);
	hlist_for_each_entry(
	    existing, &sumh_merge_dirs[hash_min(hash, SUMH_HASH_BITS)], node)
	{
		if (strcmp(existing->src, src_str) == 0 &&
		    strcmp(existing->target, target_str) == 0) {
			*out = existing->target_path;
			path_get(out);
			mutex_unlock(&sumh_config_mutex);
			return 0;
		}
	}
	mutex_unlock(&sumh_config_mutex);
	ret = sumh_validate_merge_target(captured);
	if (ret)
		return ret;
	target_path = *captured;
	path_get(&target_path);

	me = kzalloc(sizeof(*me), GFP_KERNEL);
	if (!me) {
		ret = -ENOMEM;
		goto done;
	}
	me->src = kstrdup(src_str, GFP_KERNEL);
	me->target = kstrdup(target_str, GFP_KERNEL);
	if (!me->src || !me->target) {
		ret = -ENOMEM;
		goto done;
	}
	ret = sumh_inject_prepare_merge(src_str, &me->resolved_src,
					me->inject_claims);
	if (ret)
		goto done;

	mutex_lock(&sumh_config_mutex);
	hlist_for_each_entry(
	    existing, &sumh_merge_dirs[hash_min(hash, SUMH_HASH_BITS)], node)
	{
		if (strcmp(existing->src, src_str) == 0 &&
		    strcmp(existing->target, target_str) == 0) {
			/* Do not mix this record's old root with a newly
			 * resolved directory at the same target spelling. */
			*out = existing->target_path;
			path_get(out);
			mutex_unlock(&sumh_config_mutex);
			ret = 0;
			goto done;
		}
	}
	ret = sumh_inject_attach_locked(me->inject_claims, 2);
	if (ret) {
		mutex_unlock(&sumh_config_mutex);
		goto done;
	}
	me->target_path = target_path;
	*out = target_path;
	path_get(out);
	memset(&target_path, 0, sizeof(target_path));
	hlist_add_head_rcu(&me->node,
			   &sumh_merge_dirs[hash_min(hash, SUMH_HASH_BITS)]);
	mutex_unlock(&sumh_config_mutex);

	sumh_inject_finish_prepare(me->inject_claims[0]);
	sumh_inject_finish_prepare(me->inject_claims[1]);
	me = NULL; /* The published store owns the record and root path. */
	ret = 0;
done:
	if (me) {
		sumh_inject_destroy(me->inject_claims[0]);
		sumh_inject_destroy(me->inject_claims[1]);
		kfree(me->src);
		kfree(me->target);
		kfree(me->resolved_src);
		kfree(me);
	}
	if (target_path.dentry)
		path_put(&target_path);
	return ret;
}

/* One budget spans recursive queues and joined path strings. Persistent
 * rules and VFS allocations are separate. Contents are not frozen. */
struct sumh_mat_budget {
	size_t bytes;
	unsigned int records, names, passes, directories;
	struct path ancestors[9]; /* Borrowed from active recursive callers. */
};

struct sumh_mat_name {
	struct list_head list;
	unsigned int len;
	char name[];
};

struct sumh_mat_ctx {
	struct dir_context ctx;
	struct sumh_mat_budget *budget;
	struct list_head names;
	unsigned int pass_records;
	int error;
};

static void *sumh_mat_alloc(struct sumh_mat_budget *budget, size_t size)
{
	void *p;

	if (budget->bytes > SUMH_MERGE_MEMORY_BUDGET ||
	    size > SUMH_MERGE_MEMORY_BUDGET - budget->bytes)
		return ERR_PTR(-E2BIG);
	p = kmalloc(size, GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);
	budget->bytes += size;
	return p;
}

static void sumh_mat_free(struct sumh_mat_budget *budget, void *p, size_t size)
{
	if (!p)
		return;
	budget->bytes -= size;
	kfree(p);
}

static void sumh_mat_clear_names(struct sumh_mat_ctx *mc)
{
	struct sumh_mat_name *name, *next;

	list_for_each_entry_safe (name, next, &mc->names, list) {
		list_del(&name->list);
		sumh_mat_free(mc->budget, name, sizeof(*name) + name->len + 1);
	}
}

static SUMH_NOCFI bool sumh_mat_filldir(struct dir_context *ctx,
					const char *name, int namlen,
					loff_t offset, u64 ino,
					unsigned int d_type)
{
	struct sumh_mat_ctx *mc = container_of(ctx, struct sumh_mat_ctx, ctx);
	struct sumh_mat_name *item;

	(void)offset;
	(void)ino;
	(void)d_type; /* A held child lookup determines the actual type. */
	if (mc->error)
		return false;
	if (mc->budget->records >= SUMH_MERGE_MAX_RECORDS) {
		mc->error = -E2BIG;
		return false;
	}
	mc->budget->records++;
	mc->pass_records++;
	if (!sumh_merge_valid_name(name, namlen)) {
		mc->error = -EIO;
		return false;
	}
	if ((namlen == 1 && name[0] == '.') ||
	    (namlen == 2 && name[0] == '.' && name[1] == '.'))
		return true;
	if (namlen == 8 && !memcmp(name, ".replace", 8)) {
		mc->error = -EOPNOTSUPP;
		return false;
	}
	if (mc->budget->names >= SUMH_MERGE_MAX_NAMES) {
		mc->error = -E2BIG;
		return false;
	}
	item = sumh_mat_alloc(mc->budget, sizeof(*item) + namlen + 1);
	if (IS_ERR(item)) {
		mc->error = PTR_ERR(item);
		return false;
	}
	item->len = namlen;
	memcpy(item->name, name, namlen);
	item->name[namlen] = '\0';
	list_add_tail(&item->list, &mc->names);
	mc->budget->names++;
	return true;
}

static SUMH_NOCFI int sumh_mat_collect(struct file *file,
				       struct sumh_mat_ctx *mc)
{
	int ret;

	for (;;) {
		loff_t before = file->f_pos;

		if (mc->budget->passes >= SUMH_MERGE_MAX_PASSES)
			return -E2BIG;
		mc->budget->passes++;
		mc->pass_records = 0;
		ret = iterate_dir(file, &mc->ctx);
		if (mc->error)
			return mc->error;
		if (ret < 0)
			return ret;
		if (file->f_pos == before)
			return mc->pass_records ? -EIO : 0;
		cond_resched();
	}
}

static char *sumh_mat_join(struct sumh_mat_budget *budget, const char *parent,
			   const struct sumh_mat_name *name)
{
	size_t len = strnlen(parent, SUMH_MAX_LEN_PATHNAME);
	size_t separator;
	char *path;

	if (!len)
		return ERR_PTR(-EINVAL);
	separator = parent[len - 1] == '/' ? 0 : 1;
	if (len >= SUMH_MAX_LEN_PATHNAME ||
	    name->len + separator >= SUMH_MAX_LEN_PATHNAME - len)
		return ERR_PTR(-ENAMETOOLONG);
	path = sumh_mat_alloc(budget, len + separator + name->len + 1);
	if (IS_ERR(path))
		return path;
	memcpy(path, parent, len);
	if (separator)
		path[len++] = '/';
	memcpy(path + len, name->name, name->len + 1);
	return path;
}

static SUMH_NOCFI int
sumh_materialize_merge_depth(const char *src_prefix, const char *target_dir,
			     const struct path *target_path, int depth,
			     struct sumh_mat_budget *budget);

static SUMH_NOCFI int sumh_mat_install_child(
    const char *src_prefix, const char *target_dir, const struct path *parent,
    const struct sumh_mat_name *name, int depth, struct sumh_mat_budget *budget)
{
	struct path leaf, visible, nested;
	struct inode *inode;
	char *src, *target;
	unsigned char type;
	bool visible_dir = false;
	int ret;

	src = sumh_mat_join(budget, src_prefix, name);
	if (IS_ERR(src))
		return PTR_ERR(src);
	target = sumh_mat_join(budget, target_dir, name);
	if (IS_ERR(target)) {
		ret = PTR_ERR(target);
		goto out_src;
	}
	/* One validated component, no final follow. FOLLOW with this API's
	 * explicit root would alter absolute and parent-relative links. */
	ret =
	    vfs_path_lookup(parent->dentry, parent->mnt, name->name, 0, &leaf);
	if (ret)
		goto out_target;
	inode = d_inode(leaf.dentry);
	if (!inode) {
		ret = -ENOENT;
		goto out_leaf;
	}
	if (sumh_vnode_is_ours(inode)) {
		ret = -EOPNOTSUPP;
		goto out_leaf;
	}
	type = (inode->i_mode & S_IFMT) >> 12;
	if (S_ISDIR(inode->i_mode)) {
		ret = kern_path(src, LOOKUP_FOLLOW, &visible);
		if (!ret) {
			struct inode *vi = d_inode(visible.dentry);

			visible_dir = vi && S_ISDIR(vi->i_mode);
			path_put(&visible);
		} else if (ret != -ENOENT && ret != -ENOTDIR) {
			/* An inaccessible or failed observation is not absence.
			 */
			goto out_leaf;
		}
	}
	if (!visible_dir) {
		ret = sumh_add_path_entry(src_prefix, src, target, type, &leaf);
	} else if (depth < 8) {
		ret = sumh_register_nested_merge(src, target, &leaf, &nested);
		if (!ret) {
			ret = sumh_materialize_merge_depth(src, target, &nested,
							   depth + 1, budget);
			path_put(&nested);
		}
	} else {
		ret = -ELOOP;
	}
out_leaf:
	path_put(&leaf);
out_target:
	sumh_mat_free(budget, target, strlen(target) + 1);
out_src:
	sumh_mat_free(budget, src, strlen(src) + 1);
	return ret;
}

static SUMH_NOCFI int
sumh_materialize_merge_depth(const char *src_prefix, const char *target_dir,
			     const struct path *target_path, int depth,
			     struct sumh_mat_budget *budget)
{
	struct sumh_mat_ctx mc = {};
	struct sumh_mat_name *name, *next;
	struct file *file;
	int ret, i;

	if (depth < 0 || depth > 8)
		return -ELOOP;
	if (!src_prefix || !target_dir || !target_path ||
	    !target_path->dentry || !target_path->mnt)
		return -EINVAL;
	if (!d_inode(target_path->dentry) ||
	    !S_ISDIR(d_inode(target_path->dentry)->i_mode))
		return -ENOTDIR;
	for (i = 0; i < depth; i++)
		if (path_equal(&budget->ancestors[i], target_path))
			return -ELOOP;
	if (budget->directories >= SUMH_MERGE_MAX_TARGETS)
		return -E2BIG;
	budget->directories++;
	budget->ancestors[depth] = *target_path;
	mc.budget = budget;
	mc.ctx.actor = sumh_mat_filldir;
	INIT_LIST_HEAD(&mc.names);
	file = dentry_open(target_path, O_RDONLY | O_DIRECTORY, current_cred());
	if (IS_ERR(file))
		return PTR_ERR(file);
	ret = sumh_mat_collect(file, &mc);
	fput(file);
	if (ret)
		goto out;
	/* Source iteration and its lock have ended. Only now resolve children,
	 * publish mappings and recurse. Parent queues share the same budget. */
	list_for_each_entry_safe (name, next, &mc.names, list) {
		ret = sumh_mat_install_child(src_prefix, target_dir,
					     target_path, name, depth, budget);
		if (ret)
			break;
		list_del(&name->list);
		sumh_mat_free(budget, name, sizeof(*name) + name->len + 1);
		cond_resched();
	}
out:
	sumh_mat_clear_names(&mc);
	return ret;
}

SUMH_NOCFI int sumh_materialize_merge(const char *src_prefix,
				      const char *target_dir,
				      const struct path *target_path, int depth)
{
	struct sumh_mat_budget budget = {};

	return sumh_materialize_merge_depth(src_prefix, target_dir, target_path,
					    depth, &budget);
}
