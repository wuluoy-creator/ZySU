#include "infra/mount_policy.h"
#include "sumh_vfs_view.h"
#include "sumh_entrypoints.h"
#include "sumh_fake_mountinfo.h"
#include "sumh_runtime.h"
#include "sumh_store.h"
#include "sumh_xattr_filter.h"

#include <linux/cred.h>
#include <linux/fs_struct.h>
#include <linux/hashtable.h>
#include <linux/kprobes.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/nsproxy.h>
#include <linux/rcupdate.h>
#include <linux/rwsem.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/statfs.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/xattr.h>
#include <uapi/linux/magic.h>
#include <fs/mount.h>

#define SUMH_VIEW_LINE_MAX 65536
#define SUMH_VIEW_MAX_DEPTH 256

struct sumh_view_guard {
	struct task_struct *task;
	struct hlist_node node;
};

static DEFINE_HASHTABLE(sumh_view_guards, 6);
static DEFINE_SPINLOCK(sumh_view_guard_lock);
static atomic_t sumh_view_active = ATOMIC_INIT(0);
static atomic_t sumh_xattr_owned = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(sumh_view_wait);
static bool sumh_view_stopped = true;
static bool sumh_view_drained;
static bool sumh_xattr_ready;
static bool sumh_statfs_ready;
static struct workqueue_struct *sumh_xattr_wq;
static struct rw_semaphore *sumh_namespace_sem;
static int (*sumh_view_follow_up)(struct path *);
static int (*sumh_view_statfs_orig)(const struct path *, struct kstatfs *);
static ssize_t (*sumh_view_listxattr_orig)(struct dentry *, char *, size_t);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static ssize_t (*sumh_view_getxattr_orig)(struct mnt_idmap *, struct dentry *,
					  const char *, void *, size_t);
#else
static ssize_t (*sumh_view_getxattr_orig)(struct user_namespace *,
					  struct dentry *, const char *, void *,
					  size_t);
#endif
static struct kprobe sumh_view_get_probe;
static struct kprobe sumh_view_list_probe;
static struct kprobe sumh_view_statfs_probe;
static bool sumh_view_get_registered;
static bool sumh_view_list_registered;
static bool sumh_view_statfs_registered;

static bool sumh_view_guarded(void)
{
	struct sumh_view_guard *guard;
	unsigned long flags;
	bool found = false;

	/* The current task alone installs its markers. Empty buckets therefore
	 * need no lock; occupied buckets still require it before dereferencing
	 * stack-backed guards belonging to this or another task.
	 */
	if (!READ_ONCE(sumh_view_guards[hash_min((unsigned long)current,
						 HASH_BITS(sumh_view_guards))]
			   .first))
		return false;

	spin_lock_irqsave(&sumh_view_guard_lock, flags);
	hash_for_each_possible(sumh_view_guards, guard, node,
			       (unsigned long)current)
	{
		if (guard->task == current) {
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&sumh_view_guard_lock, flags);
	return found;
}

static void sumh_view_enter(struct sumh_view_guard *guard)
{
	unsigned long flags;

	guard->task = current;
	spin_lock_irqsave(&sumh_view_guard_lock, flags);
	hash_add(sumh_view_guards, &guard->node, (unsigned long)current);
	spin_unlock_irqrestore(&sumh_view_guard_lock, flags);
}

static void sumh_view_leave(struct sumh_view_guard *guard)
{
	unsigned long flags;

	spin_lock_irqsave(&sumh_view_guard_lock, flags);
	hash_del(&guard->node);
	spin_unlock_irqrestore(&sumh_view_guard_lock, flags);
	if (atomic_dec_and_test(&sumh_view_active))
		wake_up_all(&sumh_view_wait);
}

static bool sumh_xattr_target(struct dentry *dentry)
{
	struct sumh_xattr_sb_entry *entry;
	bool found = false;

	if (!READ_ONCE(sumh_xattr_ready) || !dentry || !dentry->d_sb ||
	    !sumh_policy_current_is_mount_view_target())
		return false;
	rcu_read_lock();
	hash_for_each_possible_rcu(sumh_xattr_sbs, entry, node,
				   (unsigned long)dentry->d_sb)
	{
		if (entry->sb == dentry->d_sb) {
			found = true;
			break;
		}
	}
	rcu_read_unlock();
	return found;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static SUMH_NOCFI ssize_t sumh_view_getxattr(struct mnt_idmap *idmap,
					     struct dentry *dentry,
					     const char *name, void *value,
					     size_t size)
#else
static SUMH_NOCFI ssize_t sumh_view_getxattr(struct user_namespace *idmap,
					     struct dentry *dentry,
					     const char *name, void *value,
					     size_t size)
#endif
{
	struct sumh_view_guard guard;
	ssize_t ret;

	sumh_view_enter(&guard);
	ret = sumh_view_getxattr_orig(idmap, dentry, name, value, size);
	if ((ret >= 0 || ret == -ERANGE) && sumh_overlay_name(name) &&
	    sumh_xattr_target(dentry))
		ret = -ENODATA;
	sumh_view_leave(&guard);
	return ret;
}

static SUMH_NOCFI ssize_t sumh_view_listxattr(struct dentry *dentry, char *list,
					      size_t size)
{
	struct sumh_view_guard guard;
	char small[256];
	char *buffer = small;
	size_t capacity = sizeof(small);
	size_t output;
	ssize_t ret;

	sumh_view_enter(&guard);
	if (in_atomic() || irqs_disabled()) {
		ret = sumh_view_listxattr_orig(dentry, list, size);
		goto out;
	}
	/* Most files only carry a few short attribute names. Avoid a 64 KiB
	 * allocation for those reads, including the size-only query. A single
	 * full-size retry also handles a list growing between the two calls.
	 */
	ret = sumh_view_listxattr_orig(dentry, buffer, capacity);
	if (ret == -ERANGE) {
		capacity = XATTR_LIST_MAX;
		buffer = kvmalloc(capacity, GFP_KERNEL);
		if (!buffer) {
			ret = -ENOMEM;
			goto out;
		}
		ret = sumh_view_listxattr_orig(dentry, buffer, capacity);
	}
	if (ret < 0)
		goto out_free;
	if (ret > capacity) {
		ret = -EOVERFLOW;
		goto out_free;
	}
	ret = sumh_xattr_filter_list(buffer, ret);
	if (ret < 0)
		goto out_free;
	output = ret;
	if (size) {
		if (output > size)
			ret = -ERANGE;
		else if (output && list)
			memcpy(list, buffer, output);
		else if (output)
			ret = -EFAULT;
	}
out_free:
	if (buffer != small)
		kvfree(buffer);
out:
	sumh_view_leave(&guard);
	return ret;
}

static inline bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

#define FAKE_MI_MAX_PROP_FIELDS 8

enum fake_mi_prop_kind {
	FAKE_MI_PROP_SHARED = 0,
	FAKE_MI_PROP_MASTER,
	FAKE_MI_PROP_PROPAGATE_FROM,
};

struct fake_mi_prop_ref {
	size_t value_start;
	size_t value_end;
	int old_id;
	enum fake_mi_prop_kind kind;
};

static bool parse_decimal_token(const char *line, size_t start, size_t end,
				int *out)
{
	long v;
	char tmp[32];
	size_t len;

	if (!out || start >= end)
		return false;

	len = end - start;
	if (len >= sizeof(tmp))
		return false;

	memcpy(tmp, line + start, len);
	tmp[len] = 0;
	if (kstrtol(tmp, 10, &v))
		return false;

	*out = (int)v;
	return true;
}

static bool token_has_prefix(const char *line, size_t start, size_t end,
			     const char *prefix)
{
	size_t plen = strlen(prefix);

	return end >= start + plen && memcmp(line + start, prefix, plen) == 0;
}

/* Parse the identity fields rendered by the statfs view. */
static bool parse_line(const char *line, size_t len, int *mnt_id,
		       int *parent_id, size_t *mi_start, size_t *mi_end,
		       size_t *pi_start, size_t *pi_end,
		       struct fake_mi_prop_ref *prop_refs, size_t *prop_count,
		       bool *is_hidden, bool *is_namespace_root,
		       size_t *mountpoint_start_out, size_t *mountpoint_end_out)
{
	size_t i = 0, token_start, token_end;
	size_t mount_root_start = 0, mount_root_end = 0;
	size_t mountpoint_start = 0, mountpoint_end = 0;
	size_t j;
	struct ksu_mount_fields fields = {.escaped = true};

	*is_hidden = false;
	*is_namespace_root = false;
	if (prop_count)
		*prop_count = 0;

	/* mnt_id */
	*mi_start = i;
	while (i < len && is_digit(line[i]))
		i++;
	if (i == *mi_start || i >= len || line[i] != ' ')
		return false;
	*mi_end = i;
	if (!parse_decimal_token(line, *mi_start, *mi_end, mnt_id))
		return false;
	i++;

	/* parent_id */
	*pi_start = i;
	while (i < len && is_digit(line[i]))
		i++;
	if (i == *pi_start || i >= len || line[i] != ' ')
		return false;
	*pi_end = i;
	if (!parse_decimal_token(line, *pi_start, *pi_end, parent_id))
		return false;
	i++;

	/* Parse major:minor, root, mountpoint, mount opts. A self-parent entry
	 * is only a legitimate graph terminator when it is mounted at namespace
	 * /.
	 */
	for (j = 0; j < 4; j++) {
		token_start = i;
		while (i < len && line[i] != ' ')
			i++;
		token_end = i;
		if (token_start == token_end || i >= len || line[i] != ' ')
			return false;
		i++;
		if (j == 0)
			fields.dev = (struct ksu_mount_field){
			    line + token_start, token_end - token_start};
		if (j == 1) {
			mount_root_start = token_start;
			mount_root_end = token_end;
		} else if (j == 2) {
			mountpoint_start = token_start;
			mountpoint_end = token_end;
		}
		if (j == 2 && token_end == token_start + 1 &&
		    line[token_start] == '/')
			*is_namespace_root = true;
	}

	fields.root = (struct ksu_mount_field){
	    line + mount_root_start, mount_root_end - mount_root_start};
	fields.target = (struct ksu_mount_field){
	    line + mountpoint_start, mountpoint_end - mountpoint_start};
	if (mountpoint_start_out)
		*mountpoint_start_out = mountpoint_start;
	if (mountpoint_end_out)
		*mountpoint_end_out = mountpoint_end;

	while (i < len) {
		int value;

		token_start = i;
		while (i < len && line[i] != ' ')
			i++;
		token_end = i;
		if (token_start == token_end)
			return false;

		if (token_end == token_start + 1 && line[token_start] == '-') {
			if (i < len && line[i] == ' ')
				i++;
			break;
		}

		if (prop_refs && prop_count &&
		    *prop_count < FAKE_MI_MAX_PROP_FIELDS) {
			size_t value_start = 0;
			const char *prefix = NULL;
			enum fake_mi_prop_kind kind = FAKE_MI_PROP_SHARED;

			if (token_has_prefix(line, token_start, token_end,
					     "shared:")) {
				prefix = "shared:";
			} else if (token_has_prefix(line, token_start,
						    token_end, "master:")) {
				prefix = "master:";
				kind = FAKE_MI_PROP_MASTER;
			} else if (token_has_prefix(line, token_start,
						    token_end,
						    "propagate_from:")) {
				prefix = "propagate_from:";
				kind = FAKE_MI_PROP_PROPAGATE_FROM;
			}

			if (prefix) {
				value_start = token_start + strlen(prefix);
				if (value_start < token_end &&
				    parse_decimal_token(line, value_start,
							token_end, &value)) {
					prop_refs[*prop_count].value_start =
					    value_start;
					prop_refs[*prop_count].value_end =
					    token_end;
					prop_refs[*prop_count].old_id = value;
					prop_refs[*prop_count].kind = kind;
					(*prop_count)++;
				}
			}
		}

		if (i < len && line[i] == ' ')
			i++;
	}

	/* Read filesystem identity after the optional fields. */
	token_start = i;
	while (i < len && line[i] != ' ')
		i++;
	if (i == token_start || i == len)
		return false;
	fields.fstype =
	    (struct ksu_mount_field){line + token_start, i - token_start};
	i++;

	/* source */
	token_start = i;
	while (i < len && line[i] != ' ')
		i++;
	token_end = i;
	if (token_start == token_end)
		return false;
	fields.source = (struct ksu_mount_field){line + token_start,
						 token_end - token_start};
	while (i < len && line[i] == ' ')
		i++;
	fields.super = (struct ksu_mount_field){line + i, len - i};
	*is_hidden = ksu_mount_is_module(&fields);

	return true;
}

static int sumh_view_classify_mount(const char *line, size_t len, bool *hidden)
{
	int mount_id, parent_id;
	u64 policy_generation = ksu_mount_policy_generation();

	if (policy_generation & 1)
		return -EAGAIN;
	size_t mi_start, mi_end, pi_start, pi_end;
	bool namespace_root;

	if (!line || !hidden ||
	    !parse_line(line, len, &mount_id, &parent_id, &mi_start, &mi_end,
			&pi_start, &pi_end, NULL, NULL, hidden, &namespace_root,
			NULL, NULL))
		return -EINVAL;
	return policy_generation == ksu_mount_policy_generation() ? 0 : -EAGAIN;
}

/* Render just the identity fields used by the mountinfo classifier. No proc
 * file is opened, and filesystem option printers are deliberately not called.
 * namespace_sem pins the topology while follow_up selects a visible ancestor.
 */
static SUMH_NOCFI int sumh_view_mount_hidden(struct seq_file *seq,
					     const struct path *path,
					     const struct path *root,
					     bool *hidden)
{
	struct mount *mnt = real_mount(path->mnt);
	struct super_block *sb = path->mnt->mnt_sb;
	struct path mount_root = {.mnt = path->mnt,
				  .dentry = path->mnt->mnt_root};
	int ret;

	seq->count = 0;
	seq_printf(seq, "%i %i %u:%u ", mnt->mnt_id, mnt->mnt_parent->mnt_id,
		   MAJOR(sb->s_dev), MINOR(sb->s_dev));
	if (sb->s_op->show_path) {
		ret = sb->s_op->show_path(seq, mount_root.dentry);
		if (ret)
			return ret;
	} else if (seq_dentry(seq, mount_root.dentry, " \t\n\\") < 0) {
		return -EOVERFLOW;
	}
	seq_putc(seq, ' ');
	ret = seq_path_root(seq, &mount_root, root, " \t\n\\");
	if (ret)
		return ret;
	seq_puts(seq, " rw - ");
	seq_puts(seq, sb->s_type->name);
	seq_putc(seq, ' ');
	if (sb->s_op->show_devname) {
		ret = sb->s_op->show_devname(seq, mount_root.dentry);
		if (ret)
			return ret;
	} else {
		seq_escape(seq, mnt->mnt_devname ? mnt->mnt_devname : "none",
			   " \t\n\\");
	}
	seq_puts(seq, " rw\n");
	if (seq_has_overflowed(seq))
		return -EOVERFLOW;
	return sumh_view_classify_mount(seq->buf, seq->count, hidden);
}

static SUMH_NOCFI int sumh_view_statfs(const struct path *path,
				       struct kstatfs *buf)
{
	struct sumh_view_guard guard;
	struct path root, projected;
	struct kstatfs backing;
	struct file file = {.f_cred = current_cred()};
	struct seq_file seq = {.file = &file, .size = SUMH_VIEW_LINE_MAX};
	struct proc_mounts view = {};
	bool hidden = false, changed = false;
	int ret, depth;

	sumh_view_enter(&guard);
	ret = sumh_view_statfs_orig(path, buf);
	if (ret || in_atomic() || irqs_disabled() || !current->fs ||
	    !current->nsproxy)
		goto out;
	seq.buf = kvmalloc(seq.size, GFP_KERNEL);
	if (!seq.buf) {
		ret = -ENOMEM;
		goto out;
	}
	get_fs_root(current->fs, &root);
	projected = *path;
	path_get(&projected);
	view.ns = current->nsproxy->mnt_ns;
	view.root = root;
	seq.private = &view;
	/* Try-lock also makes nested callers holding namespace_sem safe when a
	 * writer is pending: never recursively block behind that writer.
	 */
	if (!down_read_trylock(sumh_namespace_sem)) {
		ret = -EAGAIN;
		goto out_paths;
	}
	for (depth = 0; depth < SUMH_VIEW_MAX_DEPTH; depth++) {
		if (real_mount(projected.mnt)->mnt_ns != view.ns)
			break;
		ret = sumh_view_mount_hidden(&seq, &projected, &root, &hidden);
		if (ret == SEQ_SKIP) {
			ret = 0;
			break;
		}
		if (ret || !hidden)
			break;
		if (!sumh_view_follow_up(&projected))
			break;
		changed = true;
	}
	up_read(sumh_namespace_sem);
	if (depth == SUMH_VIEW_MAX_DEPTH)
		ret = -ELOOP;
	if (!ret && changed && !hidden) {
		ret = sumh_view_statfs_orig(&projected, &backing);
		if (!ret) {
			/* Keep one coherent filesystem view. Replacing only
			 * f_type still exposes the hidden mount through its
			 * fsid, capacity, inode counts and mount flags (notably
			 * a writable tmpfs skeleton above a read-only system
			 * partition).
			 */
			*buf = backing;
			atomic64_inc(&sumh_hook_stats.statfs_spoofs);
		}
	}
out_paths:
	path_put(&projected);
	path_put(&root);
	kvfree(seq.buf);
out:
	sumh_view_leave(&guard);
	return ret;
}

/* Only redirect here. Allocation, native VFS calls and topology inspection
 * run after the exception has returned to the original sleepable context.
 */
static int sumh_view_pre(struct kprobe *probe, struct pt_regs *regs)
{
	unsigned long target;
	struct dentry *dentry;
	const char *name;

	if (READ_ONCE(sumh_view_stopped) || !current->mm || sumh_view_guarded())
		return 0;
#if defined(CONFIG_ARM64)
	if (regs->pstate & PSR_I_BIT)
		return 0;
	if (probe == &sumh_view_statfs_probe) {
		if (!READ_ONCE(sumh_statfs_ready) ||
		    !sumh_policy_current_is_mount_view_target())
			return 0;
		atomic64_inc(&sumh_hook_stats.statfs_entries);
		target = (unsigned long)sumh_view_statfs;
	} else if (probe == &sumh_view_list_probe) {
		dentry = (void *)regs->regs[0];
		if (!sumh_xattr_target(dentry))
			return 0;
		target = (unsigned long)sumh_view_listxattr;
	} else {
		dentry = (void *)regs->regs[1];
		name = (void *)regs->regs[2];
		if (!sumh_overlay_name(name) || !sumh_xattr_target(dentry))
			return 0;
		target = (unsigned long)sumh_view_getxattr;
	}
	atomic_inc(&sumh_view_active);
	instruction_pointer_set(regs, target);
	return 1;
#else
	return 0;
#endif
}

static void sumh_xattr_free_work(struct work_struct *work)
{
	struct sumh_xattr_sb_entry *entry =
	    container_of(work, struct sumh_xattr_sb_entry, free_work);

	deactivate_super(entry->sb);
	kfree(entry);
	if (atomic_dec_and_test(&sumh_xattr_owned))
		wake_up_all(&sumh_view_wait);
}

void sumh_overlay_xattr_retire(struct sumh_xattr_sb_entry *entry)
{
	queue_work(sumh_xattr_wq, &entry->free_work);
}

SUMH_NOCFI int sumh_overlay_xattr_mark(const char *name)
{
	struct sumh_xattr_sb_entry *entry, *existing;
	struct hlist_node *tmp;
	struct path path;
	int ret, bucket;

	if (!name)
		return -EINVAL;
	if (!*name) {
		mutex_lock(&sumh_config_mutex);
		hash_for_each_safe(sumh_xattr_sbs, bucket, tmp, entry, node)
		{
			hash_del_rcu(&entry->node);
			call_rcu(&entry->rcu, sumh_xattr_sb_entry_free_rcu);
		}
		mutex_unlock(&sumh_config_mutex);
		return 0;
	}
	if (!sumh_overlay_xattr_available())
		return -EOPNOTSUPP;
	ret = kern_path(name, LOOKUP_FOLLOW, &path);
	if (ret)
		return ret;
	if (path.dentry->d_sb->s_magic != OVERLAYFS_SUPER_MAGIC) {
		ret = -EINVAL;
		goto out_path;
	}
	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		ret = -ENOMEM;
		goto out_path;
	}
	entry->sb = path.dentry->d_sb;
	INIT_WORK(&entry->free_work, sumh_xattr_free_work);
	mutex_lock(&sumh_config_mutex);
	if (!sumh_overlay_xattr_available()) {
		ret = -ESHUTDOWN;
		goto out_unlock;
	}
	hash_for_each_possible(sumh_xattr_sbs, existing, node,
			       (unsigned long)entry->sb)
	{
		if (existing->sb == entry->sb)
			goto out_unlock;
	}
	if (!atomic_inc_not_zero(&entry->sb->s_active)) {
		ret = -ESHUTDOWN;
		goto out_unlock;
	}
	atomic_inc(&sumh_xattr_owned);
	hash_add_rcu(sumh_xattr_sbs, &entry->node, (unsigned long)entry->sb);
	mutex_unlock(&sumh_config_mutex);
	path_put(&path);
	return 0;
out_unlock:
	mutex_unlock(&sumh_config_mutex);
	kfree(entry);
out_path:
	path_put(&path);
	return ret;
}

bool sumh_overlay_xattr_available(void)
{
	return READ_ONCE(sumh_xattr_ready);
}

bool sumh_statfs_view_available(void)
{
	return READ_ONCE(sumh_statfs_ready);
}

static int sumh_view_register(struct kprobe *probe, const char *name)
{
	probe->symbol_name = name;
	probe->pre_handler = sumh_view_pre;
	return register_kprobe(probe);
}

static SUMH_NOCFI bool sumh_view_mount_layout_valid(void)
{
	long (*read_kernel)(void *dst, const void *src, size_t size);
	int (*show_mount)(struct seq_file *seq, struct vfsmount *mnt);
	struct path root;
	struct mnt_namespace *ns;
	struct mount *mnt;
	struct file file = {.f_cred = current_cred()};
	struct seq_file seq = {.file = &file, .size = SUMH_VIEW_LINE_MAX};
	struct proc_mounts view = {};
	char token[16];
	size_t len = 0;
	int mount_id, expected_id, ret;
	bool valid = false;

	if (!current->fs || !current->nsproxy)
		return false;
	read_kernel =
	    (void *)sumh_lookup_callable_quiet("copy_from_kernel_nofault");
	show_mount = (void *)sumh_lookup_callable_quiet("show_mountinfo");
	if (!read_kernel || !show_mount)
		return false;
	seq.buf = kvmalloc(seq.size, GFP_KERNEL);
	if (!seq.buf)
		return false;
	get_fs_root(current->fs, &root);
	mnt = real_mount(root.mnt);
	view.ns = current->nsproxy->mnt_ns;
	view.root = root;
	seq.private = &view;
	if (!down_read_trylock(sumh_namespace_sem))
		goto out;
	/* The kernel formatter validates the private mount layout without
	 * opening a proc file. vfs_getattr() does not populate statx mount IDs.
	 */
	ret = show_mount(&seq, root.mnt);
	if (ret || seq_has_overflowed(&seq))
		goto out_unlock;
	while (len < seq.count && len + 1 < sizeof(token) &&
	       seq.buf[len] >= '0' && seq.buf[len] <= '9')
		len++;
	if (!len || len >= seq.count || seq.buf[len] != ' ')
		goto out_unlock;
	memcpy(token, seq.buf, len);
	token[len] = '\0';
	if (!kstrtoint(token, 10, &expected_id) &&
	    !read_kernel(&mount_id, &mnt->mnt_id, sizeof(mount_id)) &&
	    !read_kernel(&ns, &mnt->mnt_ns, sizeof(ns)))
		valid = expected_id == mount_id && ns == view.ns;
out_unlock:
	up_read(sumh_namespace_sem);
out:
	path_put(&root);
	kvfree(seq.buf);
	return valid;
}

int sumh_vfs_view_init(void)
{
	int ret;

#ifndef CONFIG_ARM64
	return -EOPNOTSUPP;
#endif
	sumh_xattr_wq =
	    alloc_workqueue("sumh_xattr_free", WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!sumh_xattr_wq)
		return -ENOMEM;
	sumh_view_getxattr_orig = sumh_vfs_getxattr_addr;
	sumh_view_listxattr_orig = sumh_vfs_listxattr_addr;
	sumh_view_statfs_orig =
	    (void *)sumh_lookup_callable_quiet("vfs_statfs");
	sumh_view_follow_up = (void *)sumh_lookup_callable_quiet("follow_up");
	sumh_namespace_sem = (void *)sumh_lookup_name("namespace_sem");
	WRITE_ONCE(sumh_view_stopped, false);
	if (sumh_view_getxattr_orig && sumh_view_listxattr_orig) {
		ret = sumh_view_register(&sumh_view_get_probe, "vfs_getxattr");
		sumh_view_get_registered = !ret;
		if (!ret) {
			ret = sumh_view_register(&sumh_view_list_probe,
						 "vfs_listxattr");
			sumh_view_list_registered = !ret;
		}
		WRITE_ONCE(sumh_xattr_ready, !ret);
	}
	if (sumh_view_statfs_orig && sumh_view_follow_up &&
	    sumh_namespace_sem && sumh_proc_proxy_registered &&
	    sumh_fake_mi_active() && sumh_view_mount_layout_valid()) {
		ret = sumh_view_register(&sumh_view_statfs_probe, "vfs_statfs");
		sumh_view_statfs_registered = !ret;
		WRITE_ONCE(sumh_statfs_ready, !ret);
	}
	pr_info("sumh: VFS views: overlay xattrs=%d statfs=%d\n",
		sumh_xattr_ready, sumh_statfs_ready);
	return 0;
}

void sumh_vfs_view_stop(void)
{
	WRITE_ONCE(sumh_view_stopped, true);
	WRITE_ONCE(sumh_xattr_ready, false);
	WRITE_ONCE(sumh_statfs_ready, false);
	if (sumh_view_get_registered) {
		unregister_kprobe(&sumh_view_get_probe);
		sumh_view_get_registered = false;
	}
	if (sumh_view_list_registered) {
		unregister_kprobe(&sumh_view_list_probe);
		sumh_view_list_registered = false;
	}
	if (sumh_view_statfs_registered) {
		unregister_kprobe(&sumh_view_statfs_probe);
		sumh_view_statfs_registered = false;
	}
}

void sumh_vfs_view_drain(void)
{
	wait_event(sumh_view_wait, !atomic_read(&sumh_view_active));
	synchronize_rcu_tasks();
}

void sumh_vfs_view_exit(void)
{
	sumh_vfs_view_stop();
	sumh_vfs_view_drain();
	if (sumh_xattr_wq) {
		rcu_barrier();
		flush_workqueue(sumh_xattr_wq);
		destroy_workqueue(sumh_xattr_wq);
		sumh_xattr_wq = NULL;
	}
}
