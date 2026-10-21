#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/path.h>
#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/fcntl.h>
#include <linux/mount.h>
#include <linux/seq_file.h>
#include <linux/srcu.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/ctype.h>
#include <linux/namei.h>
#include <linux/pid.h>
#include <linux/poll.h>
#include <uapi/linux/magic.h>
#include "sumh_runtime.h"
#include "sumh_store.h"
#include "sumh_maps_store.h"
#include "sumh_entrypoints.h"
#include "sumh_path_policy.h"
#include "sumh_proc_read_hooks.h"
#include "sumh_proc_memory.h"
#include "sumh_fake_mountinfo.h"
#include "sumh_vnode.h"
#include "feature/kernel_umount.h"
#include "policy/allowlist.h"

#define SUMH_PROC_FILTER_BUF 65536
#define SUMH_PROC_STREAM_ALLOCATION (SUMH_PROC_FILTER_BUF + 1)
#define SUMH_PROC_STREAM_MAX (1024 * 1024)

static int sumh_filter_maps_lines(const char *src, size_t len, char *dst,
				  size_t dst_size, size_t *written,
				  bool *changed, bool *valid,
				  enum sumh_policy_scope scope,
				  const struct sumh_maps_snapshot *snapshot);

enum sumh_proc_proxy_kind {
	SUMH_PROC_PROXY_NONE = 0,
	SUMH_PROC_PROXY_MOUNTINFO,
	SUMH_PROC_PROXY_MOUNTS,
	SUMH_PROC_PROXY_MAPS,
	SUMH_PROC_PROXY_MOUNTINFO_GROUPS,
};

/*
 * Pinned proxy lifecycle:
 *   - .owner = THIS_MODULE makes the VFS hold the module from fops_get() until
 *     __fput() calls fops_put() after ->release. Therefore module exit cannot
 *     race a dispatch that already selected proxy_fops.
 *   - Every install adds the proxy to sumh_proxy_list under
 *     sumh_proxy_list_lock. Natural close removes it, runs the original
 *     ->release, and transfers the file's module reference to a static closed
 *     fops before freeing the per-file object. __fput() can then safely call
 *     fops_put(file->f_op) after our ->release returns.
 *   - Module exit can run only after all proxy fds close and their fops_put()
 *     calls drop the module references. No callback into module text is
 *     queued and the proxy list is empty by construction.
 */

#define SUMH_PROXY_STATE_OPEN 0
#define SUMH_PROXY_STATE_RELEASED 1

struct sumh_mount_file_proxy {
	const struct file_operations *orig_fops;
	struct file_operations proxy_fops;
	enum sumh_proc_proxy_kind kind;
	enum sumh_policy_scope scope;
	fmode_t orig_f_mode;
	struct list_head node;
	atomic_t state;
	atomic_t filter_invalidated;
	struct mutex stream_lock;
	char *stream_raw;
	size_t stream_raw_capacity;
	char *stream_filtered;
	size_t stream_filtered_capacity;
	size_t stream_raw_len;
	size_t stream_tail_off;
	size_t stream_out_len;
	size_t stream_out_off;
	loff_t stream_orig_pos;
	loff_t stream_user_pos;
	u64 stream_generation;
	bool stream_eof;
	bool stream_failed;
	bool maps_snapshot_acquired;
	struct sumh_maps_snapshot *maps_snapshot;
	bool mountinfo_checked;
	int mountinfo_error;
	struct sumh_mi_snapshot *mountinfo_snapshot;
};

static LIST_HEAD(sumh_proxy_list);
static DEFINE_SPINLOCK(sumh_proxy_list_lock);
DEFINE_STATIC_SRCU(sumh_proxy_srcu);
static atomic_t sumh_proxy_shutdown = ATOMIC_INIT(0);
static atomic_t sumh_proxy_live = ATOMIC_INIT(0);

static struct kprobe sumh_kp_fd_install;
static bool sumh_fd_install_registered;
static DEFINE_MUTEX(sumh_proxy_hooks_lock);
static unsigned int sumh_proxy_hook_users;
static bool sumh_view_proxy_held;
static int sumh_mount_proxy_install_file(struct file *file,
					 enum sumh_proc_proxy_kind kind,
					 enum sumh_policy_scope scope);
static SUMH_NOCFI int sumh_mount_proxy_release(struct inode *inode,
					       struct file *file);

static enum sumh_proc_proxy_kind
sumh_proc_proxy_kind_for_file(struct file *file,
			      enum sumh_policy_scope *scope_out)
{
	const struct qstr *name;
	enum sumh_policy_scope scope;
	bool spoof;

	if (!file || !file->f_path.dentry || !file->f_inode ||
	    !file->f_inode->i_sb ||
	    file->f_inode->i_sb->s_magic != PROC_SUPER_MAGIC)
		return SUMH_PROC_PROXY_NONE;

	scope = sumh_policy_current_scope();
	if (scope_out)
		*scope_out = scope;
	spoof = scope == SUMH_POLICY_SCOPE_SPOOF;
	name = &file->f_path.dentry->d_name;
	if (sumh_policy_current_is_mount_view_target() &&
	    (READ_ONCE(sumh_feature_enabled_mask) & SUMH_FEATURE_MOUNT_HIDE) &&
	    sumh_fake_mi_active() &&
	    ((name->len == 9 && !memcmp(name->name, "mountinfo", 9)) ||
	     (name->len == 6 && !memcmp(name->name, "mounts", 6))))
		return name->len == 9 ? SUMH_PROC_PROXY_MOUNTINFO
				      : SUMH_PROC_PROXY_MOUNTS;
	if (ksu_is_unshare_mnt_enabled() && name->len == 9 &&
	    !memcmp(name->name, "mountinfo", 9) &&
	    is_appuid(__kuid_val(current_uid())) &&
	    ksu_uid_should_umount(__kuid_val(current_uid())))
		return SUMH_PROC_PROXY_MOUNTINFO_GROUPS;
	if ((spoof && (sumh_feature_enabled_mask & SUMH_FEATURE_MAPS_SPOOF)) &&
	    ((name->len == 4 && !memcmp(name->name, "maps", 4)) ||
	     (name->len == 5 && !memcmp(name->name, "smaps", 5)) ||
	     (name->len == 12 && !memcmp(name->name, "smaps_rollup", 12))))
		return SUMH_PROC_PROXY_MAPS;
	return SUMH_PROC_PROXY_NONE;
}

static void sumh_fd_install_file(struct file *file)
{
	enum sumh_proc_proxy_kind kind;
	enum sumh_policy_scope scope;
	const struct file_operations *fops;

	if (!READ_ONCE(sumh_enabled) && !ksu_is_unshare_mnt_enabled())
		return;
	fops = file ? READ_ONCE(file->f_op) : NULL;
	if (!fops || READ_ONCE(fops->release) == sumh_mount_proxy_release)
		return;
	/* The fd-install ingress normally receives a private, freshly opened
	 * file. Refuse an already-shared object rather than replacing f_op for
	 * other holders.
	 */
	if (file_count(file) != 1)
		return;
	kind = sumh_proc_proxy_kind_for_file(file, &scope);
	if (kind == SUMH_PROC_PROXY_NONE)
		return;
	/* The freshly opened file has not been published yet. Installing
	 * its proxy here avoids an fd-reuse window and needs no sleeping path
	 * lookup; procfs magic plus the final dentry name already identify
	 * every supported view.
	 */
	(void)sumh_mount_proxy_install_file(file, kind, scope);
}

static int sumh_fd_install_pre(struct kprobe *kp, struct pt_regs *regs)
{
	struct file *file;

	(void)kp;
#if defined(__aarch64__)
	file = (struct file *)regs->regs[1];
#elif defined(__x86_64__)
	file = (struct file *)regs->si;
#else
	return 0;
#endif
	sumh_fd_install_file(file);
	return 0;
}

/* __fput() dereferences file->f_op after ->release returns. The live proxy's
 * owner reference is transferred to this permanent fops object while the
 * per-file proxy is reclaimed.
 */
static const struct file_operations sumh_closed_proxy_fops = {
    .owner = THIS_MODULE,
};

static bool sumh_mount_proxy_filter_active(struct sumh_mount_file_proxy *proxy)
{
	enum sumh_policy_scope scope = sumh_policy_current_scope();
	bool active = false;

	if (atomic_read(&proxy->filter_invalidated))
		return false;
	if (!READ_ONCE(sumh_enabled) || scope != proxy->scope)
		goto invalidate;
	if (proxy->kind == SUMH_PROC_PROXY_MAPS)
		active = scope == SUMH_POLICY_SCOPE_SPOOF &&
			 (sumh_feature_enabled_mask & SUMH_FEATURE_MAPS_SPOOF);
	if (active)
		return true;

invalidate:
	atomic_set(&proxy->filter_invalidated, 1);
	return false;
}

static SUMH_NOCFI ssize_t
sumh_mount_proxy_orig_read_iter(struct sumh_mount_file_proxy *proxy,
				struct kiocb *iocb, struct iov_iter *to)
{
	if (proxy->orig_fops->read_iter)
		return proxy->orig_fops->read_iter(iocb, to);

	/* maps/smaps use ->read = seq_read on Linux 6.1. All proc views
	 * proxied here are seq_files, so seq_read_iter is the safe
	 * kernel-buffer equivalent and avoids ever placing unfiltered bytes in
	 * userspace.
	 */
	return seq_read_iter(iocb, to);
}

static bool sumh_mount_proxy_stream_reserve(size_t bytes)
{
	return sumh_proc_memory_reserve(SUMH_PROC_MEMORY_STREAM, bytes);
}

static void sumh_mount_proxy_stream_free(struct sumh_mount_file_proxy *proxy)
{
	/* Pins are released in sleepable read/close/drain paths. An acquired
	 * empty set is also final for this stream; never re-sample after
	 * terminal failure. */
	sumh_maps_snapshot_put(proxy->maps_snapshot);
	proxy->maps_snapshot = NULL;

	if (proxy->stream_raw) {
		kvfree(proxy->stream_raw);
		sumh_proc_memory_release(SUMH_PROC_MEMORY_STREAM,
					 proxy->stream_raw_capacity);
		proxy->stream_raw = NULL;
		proxy->stream_raw_capacity = 0;
	}
	if (proxy->stream_filtered) {
		kvfree(proxy->stream_filtered);
		sumh_proc_memory_release(SUMH_PROC_MEMORY_STREAM,
					 proxy->stream_filtered_capacity);
		proxy->stream_filtered = NULL;
		proxy->stream_filtered_capacity = 0;
	}
	proxy->stream_raw_len = 0;
	proxy->stream_tail_off = 0;
	proxy->stream_out_len = 0;
	proxy->stream_out_off = 0;
}

static int sumh_mount_proxy_stream_alloc(struct sumh_mount_file_proxy *proxy)
{
	char *raw;

	if (proxy->stream_raw)
		return 0;
	if (!sumh_mount_proxy_stream_reserve(SUMH_PROC_STREAM_ALLOCATION))
		return -ENOMEM;
	raw = kvmalloc(SUMH_PROC_STREAM_ALLOCATION, GFP_KERNEL);
	if (!raw)
		goto out_unreserve;
	proxy->stream_raw = raw;
	proxy->stream_raw_capacity = SUMH_PROC_STREAM_ALLOCATION;
	return 0;

out_unreserve:
	sumh_proc_memory_alloc_failed(SUMH_PROC_MEMORY_STREAM);
	sumh_proc_memory_release(SUMH_PROC_MEMORY_STREAM,
				 SUMH_PROC_STREAM_ALLOCATION);
	return -ENOMEM;
}

static int sumh_mount_proxy_stream_grow_raw(struct sumh_mount_file_proxy *proxy)
{
	char *raw;
	size_t old_capacity = proxy->stream_raw_capacity;
	size_t new_capacity;

	if (old_capacity >= SUMH_PROC_STREAM_MAX)
		return -EFBIG;
	new_capacity = min_t(size_t, old_capacity * 2, SUMH_PROC_STREAM_MAX);
	/* The old buffer stays live until the replacement has been copied. */
	if (!sumh_mount_proxy_stream_reserve(new_capacity))
		return -ENOMEM;
	raw = kvmalloc(new_capacity, GFP_KERNEL);
	if (!raw) {
		sumh_proc_memory_alloc_failed(SUMH_PROC_MEMORY_STREAM);
		sumh_proc_memory_release(SUMH_PROC_MEMORY_STREAM, new_capacity);
		return -ENOMEM;
	}
	memcpy(raw, proxy->stream_raw, proxy->stream_raw_len);
	kvfree(proxy->stream_raw);
	sumh_proc_memory_release(SUMH_PROC_MEMORY_STREAM, old_capacity);
	proxy->stream_raw = raw;
	proxy->stream_raw_capacity = new_capacity;
	return 0;
}

static int
sumh_mount_proxy_stream_alloc_filtered(struct sumh_mount_file_proxy *proxy)
{
	char *filtered;

	if (proxy->stream_filtered)
		return 0;
	if (!sumh_mount_proxy_stream_reserve(SUMH_PROC_STREAM_ALLOCATION))
		return -ENOMEM;
	filtered = kvmalloc(SUMH_PROC_STREAM_ALLOCATION, GFP_KERNEL);
	if (!filtered) {
		sumh_proc_memory_alloc_failed(SUMH_PROC_MEMORY_STREAM);
		sumh_proc_memory_release(SUMH_PROC_MEMORY_STREAM,
					 SUMH_PROC_STREAM_ALLOCATION);
		return -ENOMEM;
	}
	proxy->stream_filtered = filtered;
	proxy->stream_filtered_capacity = SUMH_PROC_STREAM_ALLOCATION;
	return 0;
}

static int
sumh_mount_proxy_stream_grow_filtered(struct sumh_mount_file_proxy *proxy)
{
	char *filtered;
	size_t old_capacity = proxy->stream_filtered_capacity;
	size_t new_capacity;

	if (old_capacity >= SUMH_PROC_STREAM_MAX)
		return -ENOSPC;
	new_capacity = min_t(size_t, old_capacity * 2, SUMH_PROC_STREAM_MAX);
	/* Charge both buffers until the old allocation has been freed. */
	if (!sumh_mount_proxy_stream_reserve(new_capacity))
		return -ENOMEM;
	filtered = kvmalloc(new_capacity, GFP_KERNEL);
	if (!filtered) {
		sumh_proc_memory_alloc_failed(SUMH_PROC_MEMORY_STREAM);
		sumh_proc_memory_release(SUMH_PROC_MEMORY_STREAM, new_capacity);
		return -ENOMEM;
	}
	kvfree(proxy->stream_filtered);
	sumh_proc_memory_release(SUMH_PROC_MEMORY_STREAM, old_capacity);
	proxy->stream_filtered = filtered;
	proxy->stream_filtered_capacity = new_capacity;
	return 0;
}

static void sumh_mount_proxy_stream_compact(struct sumh_mount_file_proxy *proxy)
{
	size_t tail_len;

	if (!proxy->stream_out_len)
		return;
	tail_len = proxy->stream_raw_len - proxy->stream_tail_off;
	if (tail_len)
		memmove(proxy->stream_raw,
			proxy->stream_raw + proxy->stream_tail_off, tail_len);
	proxy->stream_raw_len = tail_len;
	proxy->stream_tail_off = 0;
	proxy->stream_out_len = 0;
	proxy->stream_out_off = 0;
}

static ssize_t sumh_mount_proxy_stream_fill(struct sumh_mount_file_proxy *proxy,
					    struct file *file)
{
	struct kiocb shadow_iocb;
	struct iov_iter kernel_iter;
	struct kvec kvec;
	size_t complete_len;
	size_t tail_len;
	size_t new_len;
	bool maps_changed;
	bool maps_valid;
	ssize_t ret;

	for (;;) {
		sumh_mount_proxy_stream_compact(proxy);
		complete_len = proxy->stream_raw_len;
		while (complete_len > 0 &&
		       proxy->stream_raw[complete_len - 1] != '\n')
			complete_len--;

		if (complete_len > 0) {
			if (proxy->kind == SUMH_PROC_PROXY_MAPS) {
				ret = sumh_mount_proxy_stream_alloc_filtered(
				    proxy);
				if (ret)
					return ret;
				for (;;) {
					maps_changed = false;
					maps_valid = true;
					ret = sumh_filter_maps_lines(
					    proxy->stream_raw, complete_len,
					    proxy->stream_filtered,
					    proxy->stream_filtered_capacity,
					    &new_len, &maps_changed,
					    &maps_valid, proxy->scope,
					    proxy->maps_snapshot);
					if (ret != -ENOSPC)
						break;
					ret =
					    sumh_mount_proxy_stream_grow_filtered(
						proxy);
					if (ret)
						return ret;
				}
				if (ret < 0 || !maps_valid)
					return ret < 0 ? ret : -EIO;
			} else {
				/* Only maps use the buffered filtering path. */
				return -EIO;
			}

			tail_len = proxy->stream_raw_len - complete_len;
			proxy->stream_tail_off = complete_len;
			proxy->stream_out_off = 0;
			proxy->stream_out_len = new_len;
			if (new_len)
				return 1;
			if (tail_len)
				memmove(proxy->stream_raw,
					proxy->stream_raw + complete_len,
					tail_len);
			proxy->stream_raw_len = tail_len;
			proxy->stream_tail_off = 0;
			continue;
		}

		if (proxy->stream_eof)
			return proxy->stream_raw_len ? -EIO : 0;
		if (proxy->stream_raw_len + 1 >= proxy->stream_raw_capacity) {
			ret = sumh_mount_proxy_stream_grow_raw(proxy);
			if (ret)
				return ret;
		}

		kvec.iov_base = proxy->stream_raw + proxy->stream_raw_len;
		kvec.iov_len =
		    proxy->stream_raw_capacity - 1 - proxy->stream_raw_len;
		iov_iter_kvec(&kernel_iter, READ, &kvec, 1, kvec.iov_len);
		init_sync_kiocb(&shadow_iocb, file);
		shadow_iocb.ki_pos = proxy->stream_orig_pos;
		ret = sumh_mount_proxy_orig_read_iter(proxy, &shadow_iocb,
						      &kernel_iter);
		if (ret < 0)
			return ret;
		if (!ret) {
			proxy->stream_eof = true;
			continue;
		}
		if ((size_t)ret > kvec.iov_len)
			return -EIO;
		proxy->stream_raw_len += (size_t)ret;
		proxy->stream_orig_pos = shadow_iocb.ki_pos;
		proxy->stream_raw[proxy->stream_raw_len] = '\0';
	}
}

static int
sumh_mount_proxy_prepare_mountinfo(struct sumh_mount_file_proxy *proxy,
				   struct file *file)
{
	if (!proxy->mountinfo_checked) {
		proxy->mountinfo_error = sumh_fake_mi_get_snapshot(
		    file, proxy->orig_fops, &proxy->mountinfo_snapshot);
		/* Never let a missing view turn into an unfiltered proc read.
		 */
		if (!proxy->mountinfo_error && !proxy->mountinfo_snapshot)
			proxy->mountinfo_error = -EIO;
		proxy->mountinfo_checked = true;
	}
	return proxy->mountinfo_error;
}

/* Render the already-open native view lazily; its namespace and poll stay
 * intact. */
static SUMH_NOCFI int
sumh_mount_proxy_prepare_groups(struct sumh_mount_file_proxy *proxy,
				struct file *file)
{
	loff_t saved_pos = file->f_pos, native_pos = 0;
	int ret;

	if (proxy->mountinfo_checked)
		return proxy->mountinfo_error;
	proxy->mountinfo_checked = true;
	ret = sumh_mount_proxy_stream_alloc(proxy);
	if (ret)
		goto out;
	if (!proxy->orig_fops->llseek) {
		ret = -ESPIPE;
		goto out;
	}
	ret = proxy->orig_fops->llseek(file, 0, SEEK_SET);
	if (ret < 0)
		goto out;
	proxy->stream_raw_len = 0;
	for (;;) {
		struct kiocb iocb;
		struct iov_iter iter;
		struct kvec vec;
		char overflow;
		ssize_t bytes;
		bool full;

		if (proxy->stream_raw_len + 1 == proxy->stream_raw_capacity &&
		    proxy->stream_raw_capacity < SUMH_PROC_STREAM_MAX) {
			ret = sumh_mount_proxy_stream_grow_raw(proxy);
			if (ret)
				goto out;
		}
		full = proxy->stream_raw_len + 1 == proxy->stream_raw_capacity;
		vec.iov_base = full ? &overflow
				    : proxy->stream_raw + proxy->stream_raw_len;
		vec.iov_len = full ? 1
				   : proxy->stream_raw_capacity - 1 -
					 proxy->stream_raw_len;
		iov_iter_kvec(&iter, READ, &vec, 1, vec.iov_len);
		init_sync_kiocb(&iocb, file);
		iocb.ki_pos = native_pos;
		bytes = sumh_mount_proxy_orig_read_iter(proxy, &iocb, &iter);
		if (bytes < 0) {
			ret = bytes;
			goto out;
		}
		if (!bytes)
			break;
		if (full) {
			ret = -EFBIG;
			goto out;
		}
		if (bytes > vec.iov_len || iocb.ki_pos <= native_pos) {
			ret = -EIO;
			goto out;
		}
		proxy->stream_raw_len += bytes;
		native_pos = iocb.ki_pos;
	}
	ret =
	    sumh_mi_normalize_groups(proxy->stream_raw, &proxy->stream_raw_len);
	proxy->stream_raw[proxy->stream_raw_len] = '\0';
out:
	file->f_pos = saved_pos;
	if (ret)
		sumh_mount_proxy_stream_free(proxy);
	proxy->mountinfo_error = ret;
	return ret;
}

static ssize_t sumh_mount_proxy_read_buffer(const char *data, size_t len,
					    char __user *buffer,
					    struct iov_iter *to, size_t count,
					    loff_t *pos)
{
	size_t copied;

	if (!pos || *pos < 0)
		return -EINVAL;
	if (*pos >= len || !count)
		return 0;
	count = min_t(size_t, count, len - (size_t)*pos);
	if (to)
		copied = copy_to_iter(data + *pos, count, to);
	else
		copied = count - copy_to_user(buffer, data + *pos, count);
	if (!copied)
		return -EFAULT;
	*pos += copied;
	return copied;
}

static ssize_t
sumh_mount_proxy_read_snapshot(const struct sumh_mi_snapshot *snapshot,
			       bool mounts, char __user *buffer,
			       struct iov_iter *to, size_t count, loff_t *pos)
{
	return sumh_mount_proxy_read_buffer(
	    mounts ? snapshot->mounts : snapshot->data,
	    mounts ? snapshot->mounts_len : snapshot->len, buffer, to, count,
	    pos);
}

static ssize_t sumh_mount_proxy_read_groups(struct sumh_mount_file_proxy *proxy,
					    struct file *file,
					    char __user *buffer,
					    struct iov_iter *to, size_t count,
					    loff_t *pos)
{
	ssize_t ret = 0;

	mutex_lock(&proxy->stream_lock);
	if (count) {
		ret = sumh_mount_proxy_prepare_groups(proxy, file);
		if (!ret)
			ret = sumh_mount_proxy_read_buffer(
			    proxy->stream_raw, proxy->stream_raw_len, buffer,
			    to, count, pos);
	}
	mutex_unlock(&proxy->stream_lock);
	return ret;
}

static ssize_t
sumh_mount_proxy_filtered_read(struct sumh_mount_file_proxy *proxy,
			       struct file *file, char __user *userbuf,
			       struct iov_iter *to, size_t count, loff_t *ppos)
{
	const char *outbuf;
	size_t available;
	size_t copied;
	ssize_t ret;

	if (!ppos || *ppos < 0)
		return -EINVAL;
	if (!count)
		return 0;
	mutex_lock(&proxy->stream_lock);
	if (proxy->stream_failed || *ppos != proxy->stream_user_pos) {
		ret = -EIO;
		goto out_unlock;
	}
	/* One immutable rule set for the entire nonseekable Maps traversal,
	 * including empty sets, native-read chunks and output growth retries.
	 */
	if (proxy->kind == SUMH_PROC_PROXY_MAPS &&
	    !proxy->maps_snapshot_acquired) {
		proxy->maps_snapshot = sumh_maps_snapshot_get();
		proxy->maps_snapshot_acquired = true;
	}
	ret = sumh_mount_proxy_stream_alloc(proxy);
	if (ret)
		goto out_fail;

	while (proxy->stream_out_off == proxy->stream_out_len) {
		ret = sumh_mount_proxy_stream_fill(proxy, file);
		if (ret <= 0)
			goto out_fail;
	}
	outbuf = proxy->kind == SUMH_PROC_PROXY_MAPS ? proxy->stream_filtered
						     : proxy->stream_raw;
	if (!outbuf) {
		ret = -EIO;
		goto out_fail;
	}
	available =
	    min_t(size_t, count, proxy->stream_out_len - proxy->stream_out_off);
	if (to)
		copied =
		    copy_to_iter(outbuf + proxy->stream_out_off, available, to);
	else
		copied = available -
			 copy_to_user(userbuf, outbuf + proxy->stream_out_off,
				      available);
	if (!copied) {
		ret = -EFAULT;
		goto out_unlock;
	}
	proxy->stream_out_off += copied;
	proxy->stream_user_pos += copied;
	*ppos = proxy->stream_user_pos;
	ret = (ssize_t)copied;
	goto out_unlock;

out_fail:
	if (ret < 0) {
		proxy->stream_failed = true;
		sumh_mount_proxy_stream_free(proxy);
	}

out_unlock:
	mutex_unlock(&proxy->stream_lock);
	return ret;
}

static SUMH_NOCFI ssize_t sumh_mount_proxy_read(struct file *file,
						char __user *buf, size_t count,
						loff_t *ppos)
{
	struct sumh_mount_file_proxy *proxy =
	    container_of(file->f_op, struct sumh_mount_file_proxy, proxy_fops);
	ssize_t ret = 0;
	int srcu_idx = srcu_read_lock(&sumh_proxy_srcu);

	if (proxy->kind == SUMH_PROC_PROXY_MOUNTINFO ||
	    proxy->kind == SUMH_PROC_PROXY_MOUNTS) {
		mutex_lock(&proxy->stream_lock);
		if (count) {
			ret = sumh_mount_proxy_prepare_mountinfo(proxy, file);
			if (!ret)
				ret = sumh_mount_proxy_read_snapshot(
				    proxy->mountinfo_snapshot,
				    proxy->kind == SUMH_PROC_PROXY_MOUNTS, buf,
				    NULL, count, ppos);
		}
		mutex_unlock(&proxy->stream_lock);
	} else if (proxy->kind == SUMH_PROC_PROXY_MOUNTINFO_GROUPS) {
		ret = sumh_mount_proxy_read_groups(proxy, file, buf, NULL,
						   count, ppos);
	} else if (sumh_mount_proxy_filter_active(proxy)) {
		ret = sumh_mount_proxy_filtered_read(
		    proxy, file, buf, NULL, count, ppos ? ppos : &file->f_pos);
	} else {
		ret = -EIO;
	}
	srcu_read_unlock(&sumh_proxy_srcu, srcu_idx);
	return ret;
}

static ssize_t sumh_mount_proxy_read_iter(struct kiocb *iocb,
					  struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct sumh_mount_file_proxy *proxy =
	    container_of(file->f_op, struct sumh_mount_file_proxy, proxy_fops);
	ssize_t ret = 0;
	int srcu_idx = srcu_read_lock(&sumh_proxy_srcu);

	if (proxy->kind == SUMH_PROC_PROXY_MOUNTINFO ||
	    proxy->kind == SUMH_PROC_PROXY_MOUNTS) {
		mutex_lock(&proxy->stream_lock);
		if (iov_iter_count(to)) {
			ret = sumh_mount_proxy_prepare_mountinfo(proxy, file);
			if (!ret)
				ret = sumh_mount_proxy_read_snapshot(
				    proxy->mountinfo_snapshot,
				    proxy->kind == SUMH_PROC_PROXY_MOUNTS, NULL,
				    to, iov_iter_count(to), &iocb->ki_pos);
		}
		mutex_unlock(&proxy->stream_lock);
	} else if (proxy->kind == SUMH_PROC_PROXY_MOUNTINFO_GROUPS) {
		ret = sumh_mount_proxy_read_groups(
		    proxy, file, NULL, to, iov_iter_count(to), &iocb->ki_pos);
	} else if (sumh_mount_proxy_filter_active(proxy)) {
		ret = sumh_mount_proxy_filtered_read(
		    proxy, file, NULL, to, iov_iter_count(to), &iocb->ki_pos);
	} else {
		ret = -EIO;
	}
	srcu_read_unlock(&sumh_proxy_srcu, srcu_idx);
	return ret;
}

static SUMH_NOCFI ssize_t sumh_mount_proxy_splice_read(
    struct file *file, loff_t *ppos, struct pipe_inode_info *pipe, size_t len,
    unsigned int flags)
{
	struct sumh_mount_file_proxy *proxy =
	    container_of(file->f_op, struct sumh_mount_file_proxy, proxy_fops);
	int ret = 0;

	mutex_lock(&proxy->stream_lock);
	if (len) {
		if (proxy->kind == SUMH_PROC_PROXY_MOUNTINFO_GROUPS)
			ret = sumh_mount_proxy_prepare_groups(proxy, file);
		else
			ret = sumh_mount_proxy_prepare_mountinfo(proxy, file);
	}
	mutex_unlock(&proxy->stream_lock);
	if (ret)
		return ret;
	/* The native proc splice helper consumes our read_iter callback. */
	return proxy->orig_fops->splice_read(file, ppos, pipe, len, flags);
}

static bool
sumh_mount_proxy_reject_ioctl(const struct sumh_mount_file_proxy *proxy,
			      unsigned int cmd)
{
#ifdef PROCMAP_QUERY
	return proxy->kind == SUMH_PROC_PROXY_MAPS && cmd == PROCMAP_QUERY;
#else
	(void)proxy;
	(void)cmd;
	return false;
#endif
}

static SUMH_NOCFI long
sumh_mount_proxy_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct sumh_mount_file_proxy *proxy =
	    container_of(file->f_op, struct sumh_mount_file_proxy, proxy_fops);
	long ret;
	int srcu_idx;

	srcu_idx = srcu_read_lock(&sumh_proxy_srcu);
	if (!sumh_mount_proxy_filter_active(proxy))
		ret = -EIO;
	else if (sumh_mount_proxy_reject_ioctl(proxy, cmd))
		ret = -EOPNOTSUPP;
	else if (proxy->orig_fops->unlocked_ioctl)
		ret = proxy->orig_fops->unlocked_ioctl(file, cmd, arg);
	else
		ret = -ENOTTY;
	srcu_read_unlock(&sumh_proxy_srcu, srcu_idx);
	return ret;
}

#ifdef CONFIG_COMPAT
static SUMH_NOCFI long sumh_mount_proxy_compat_ioctl(struct file *file,
						     unsigned int cmd,
						     unsigned long arg)
{
	struct sumh_mount_file_proxy *proxy =
	    container_of(file->f_op, struct sumh_mount_file_proxy, proxy_fops);
	long ret;
	int srcu_idx;

	srcu_idx = srcu_read_lock(&sumh_proxy_srcu);
	if (!sumh_mount_proxy_filter_active(proxy))
		ret = -EIO;
	else if (sumh_mount_proxy_reject_ioctl(proxy, cmd))
		ret = -EOPNOTSUPP;
	else if (proxy->orig_fops->compat_ioctl)
		ret = proxy->orig_fops->compat_ioctl(file, cmd, arg);
	else
		ret = -ENOTTY;
	srcu_read_unlock(&sumh_proxy_srcu, srcu_idx);
	return ret;
}
#endif

static SUMH_NOCFI loff_t sumh_mount_proxy_llseek(struct file *file,
						 loff_t offset, int whence)
{
	struct sumh_mount_file_proxy *proxy =
	    container_of(file->f_op, struct sumh_mount_file_proxy, proxy_fops);
	loff_t ret;

	if (proxy->kind == SUMH_PROC_PROXY_MAPS)
		return -ESPIPE;
	if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
		return -EINVAL;
	if (whence == SEEK_SET && offset < 0)
		return -EINVAL;
	mutex_lock(&proxy->stream_lock);
	if ((proxy->kind == SUMH_PROC_PROXY_MOUNTINFO ||
	     proxy->kind == SUMH_PROC_PROXY_MOUNTS) &&
	    !offset && whence == SEEK_SET) {
		ret = proxy->orig_fops->llseek
			  ? proxy->orig_fops->llseek(file, 0, SEEK_SET)
			  : -ESPIPE;
		if (!ret) {
			sumh_fake_mi_put_snapshot(proxy->mountinfo_snapshot);
			proxy->mountinfo_snapshot = NULL;
			proxy->mountinfo_checked = false;
			proxy->mountinfo_error = 0;
		}
		goto unlock;
	}
	if (proxy->kind == SUMH_PROC_PROXY_MOUNTINFO_GROUPS) {
		if (whence == SEEK_END) {
			ret = -EINVAL;
		} else if (!offset && whence == SEEK_SET) {
			ret = proxy->orig_fops->llseek(file, 0, SEEK_SET);
			if (!ret) {
				proxy->mountinfo_checked = false;
				proxy->mountinfo_error = 0;
				proxy->stream_raw_len = 0;
			}
		} else {
			ret = generic_file_llseek_size(file, offset, whence,
						       MAX_LFS_FILESIZE, 0);
		}
		goto unlock;
	}
	if (offset || whence == SEEK_END) {
		ret = sumh_mount_proxy_prepare_mountinfo(proxy, file);
		if (ret)
			goto unlock;
	}
	if (proxy->mountinfo_snapshot)
		ret = generic_file_llseek_size(
		    file, offset, whence, MAX_LFS_FILESIZE,
		    (proxy->kind == SUMH_PROC_PROXY_MOUNTS
			 ? proxy->mountinfo_snapshot->mounts_len
			 : proxy->mountinfo_snapshot->len));
	else
		ret = proxy->orig_fops->llseek
			  ? proxy->orig_fops->llseek(file, offset, whence)
			  : -ESPIPE;
unlock:
	mutex_unlock(&proxy->stream_lock);
	return ret;
}

static SUMH_NOCFI __poll_t sumh_mount_proxy_poll(struct file *file,
						 struct poll_table_struct *wait)
{
	struct sumh_mount_file_proxy *proxy =
	    container_of(file->f_op, struct sumh_mount_file_proxy, proxy_fops);
	__poll_t mask = EPOLLIN | EPOLLRDNORM;
	u64 generation;

	mutex_lock(&proxy->stream_lock);
	sumh_fake_mi_poll_wait(file, wait);
	if (proxy->orig_fops->poll)
		mask = proxy->orig_fops->poll(file, wait);
	if (proxy->mountinfo_error)
		mask |= EPOLLERR;
	generation = sumh_fake_mi_generation();
	if (proxy->stream_generation != generation) {
		proxy->stream_generation = generation;
		mask |= EPOLLERR | EPOLLPRI;
	}
	mutex_unlock(&proxy->stream_lock);
	return mask;
}

static SUMH_NOCFI int sumh_mount_proxy_release(struct inode *inode,
					       struct file *file)
{
	struct sumh_mount_file_proxy *proxy =
	    container_of(file->f_op, struct sumh_mount_file_proxy, proxy_fops);
	const struct file_operations *orig_fops = proxy->orig_fops;
	int ret = 0;
	int srcu_idx;

	srcu_idx = srcu_read_lock(&sumh_proxy_srcu);
	atomic_set(&proxy->state, SUMH_PROXY_STATE_RELEASED);
	spin_lock(&sumh_proxy_list_lock);
	list_del_init(&proxy->node);
	spin_unlock(&sumh_proxy_list_lock);
	atomic_dec(&sumh_proxy_live);

	WRITE_ONCE(file->f_mode, proxy->orig_f_mode);
	if (orig_fops->release)
		ret = orig_fops->release(inode, file);
	sumh_fake_mi_put_snapshot(proxy->mountinfo_snapshot);

	/* Keep the live proxy's THIS_MODULE reference for __fput(), but stop
	 * __fput() from touching proxy storage after this callback returns.
	 */
	WRITE_ONCE(file->f_op, &sumh_closed_proxy_fops);
	fops_put(orig_fops);
	srcu_read_unlock(&sumh_proxy_srcu, srcu_idx);
	sumh_mount_proxy_stream_free(proxy);
	kfree(proxy);
	return ret;
}

static int sumh_mount_proxy_install_file(struct file *file,
					 enum sumh_proc_proxy_kind kind,
					 enum sumh_policy_scope scope)
{
	struct sumh_mount_file_proxy *proxy;
	const struct file_operations *orig_fops;
	const struct file_operations *new_fops;
	if (!file || kind == SUMH_PROC_PROXY_NONE ||
	    atomic_read(&sumh_proxy_shutdown))
		return -ESHUTDOWN;
	if ((kind == SUMH_PROC_PROXY_MOUNTINFO ||
	     kind == SUMH_PROC_PROXY_MOUNTS) &&
	    !sumh_policy_current_is_mount_view_target())
		return -EINVAL;
	if (kind == SUMH_PROC_PROXY_MAPS && scope != SUMH_POLICY_SCOPE_VIEW &&
	    scope != SUMH_POLICY_SCOPE_SPOOF)
		return -EINVAL;
	orig_fops = READ_ONCE(file->f_op);
	if (!orig_fops || orig_fops->release == sumh_mount_proxy_release)
		return -EALREADY;
	proxy = kzalloc(sizeof(*proxy), GFP_ATOMIC);
	if (!proxy)
		return -ENOMEM;

	proxy->orig_fops = orig_fops;
	proxy->kind = kind;
	/* Use the opened proc namespace for every app, including isolated
	 * processes. Borrowing another task's table breaks mount-ID lookups.
	 */
	proxy->scope = scope;
	proxy->orig_f_mode = READ_ONCE(file->f_mode);
	proxy->proxy_fops = *orig_fops;
	/* Pin module text and proxy storage through VFS dispatch and ->release.
	 * An open proc proxy fd intentionally makes delete_module() return
	 * -EWOULDBLOCK until the fd closes.
	 */
	proxy->proxy_fops.owner = THIS_MODULE;
	if (proxy->orig_fops->read)
		proxy->proxy_fops.read = sumh_mount_proxy_read;
	proxy->proxy_fops.read_iter = sumh_mount_proxy_read_iter;
	proxy->proxy_fops.splice_read =
	    (kind == SUMH_PROC_PROXY_MOUNTINFO ||
	     kind == SUMH_PROC_PROXY_MOUNTS ||
	     kind == SUMH_PROC_PROXY_MOUNTINFO_GROUPS) &&
		    orig_fops->splice_read
		? sumh_mount_proxy_splice_read
		: NULL;
	proxy->proxy_fops.llseek = sumh_mount_proxy_llseek;
	if (kind == SUMH_PROC_PROXY_MOUNTINFO || kind == SUMH_PROC_PROXY_MOUNTS)
		proxy->proxy_fops.poll = sumh_mount_proxy_poll;
	if (kind == SUMH_PROC_PROXY_MAPS) {
		proxy->proxy_fops.unlocked_ioctl = sumh_mount_proxy_ioctl;
#ifdef CONFIG_COMPAT
		proxy->proxy_fops.compat_ioctl = sumh_mount_proxy_compat_ioctl;
#endif
	}
	proxy->proxy_fops.release = sumh_mount_proxy_release;
	INIT_LIST_HEAD(&proxy->node);
	atomic_set(&proxy->state, SUMH_PROXY_STATE_OPEN);
	atomic_set(&proxy->filter_invalidated, 0);
	mutex_init(&proxy->stream_lock);
	proxy->stream_orig_pos = 0;
	proxy->stream_user_pos = 0;
	proxy->stream_generation = sumh_fake_mi_generation();
	proxy->stream_failed = false;

	spin_lock(&sumh_proxy_list_lock);
	/* Fd installation has not published file yet, but keep this comparison
	 * under the same lock as list publication so duplicate installers
	 * cannot overwrite and leak one another.
	 */
	if (atomic_read(&sumh_proxy_shutdown) ||
	    READ_ONCE(file->f_op) != orig_fops) {
		spin_unlock(&sumh_proxy_list_lock);
		kfree(proxy);
		return -EAGAIN;
	}
	new_fops = fops_get(&proxy->proxy_fops);
	if (!new_fops) {
		spin_unlock(&sumh_proxy_list_lock);
		kfree(proxy);
		return -ENOENT;
	}
	list_add(&proxy->node, &sumh_proxy_list);
	atomic_inc(&sumh_proxy_live);
	if (kind == SUMH_PROC_PROXY_MAPS)
		WRITE_ONCE(file->f_mode,
			   proxy->orig_f_mode & ~(FMODE_LSEEK | FMODE_PREAD));
	WRITE_ONCE(file->f_op, new_fops);
	spin_unlock(&sumh_proxy_list_lock);

	sumh_log("proc_proxy: installed kind=%d pid=%d comm=%s\n", kind,
		 task_pid_nr(current), current->comm);
	return 0;
}

static SUMH_NOCFI void sumh_mount_proxy_drain(void)
{
	struct sumh_mount_file_proxy *p, *tmp;
	LIST_HEAD(stale_proxies);

	atomic_set(&sumh_proxy_shutdown, 1);

	/* Every installed proxy holds THIS_MODULE through its fops owner, so
	 * module exit cannot begin until its release path removes it here.
	 */
	synchronize_srcu(&sumh_proxy_srcu);
	spin_lock(&sumh_proxy_list_lock);
	list_splice_init(&sumh_proxy_list, &stale_proxies);
	spin_unlock(&sumh_proxy_list_lock);
	list_for_each_entry_safe (p, tmp, &stale_proxies, node) {
		WARN_ON_ONCE(atomic_read(&p->state) == SUMH_PROXY_STATE_OPEN);
		list_del_init(&p->node);
		sumh_mount_proxy_stream_free(p);
		kfree(p);
	}
}

static int sumh_parse_maps_line(const char *line, size_t line_len,
				unsigned long *start, unsigned long *end,
				char *flags, unsigned long *pgoff,
				unsigned long *dev, unsigned long *ino,
				const char **pathname)
{
	unsigned int ma, mi;
	const char *p = line;
	char *endptr;

	if (line_len < 40)
		return -1;
	*start = simple_strtoul(p, &endptr, 16);
	if (endptr == p || *endptr != '-')
		return -1;
	p = endptr + 1;
	*end = simple_strtoul(p, &endptr, 16);
	if (endptr == p || *endptr != ' ')
		return -1;
	p = endptr + 1;
	flags[0] = p[0];
	flags[1] = p[1];
	flags[2] = p[2];
	flags[3] = p[3];
	flags[4] = '\0';
	p += 4;
	if (*p != ' ')
		return -1;
	*pgoff = simple_strtoul(p + 1, &endptr, 16);
	p = endptr;
	if (*p != ' ')
		return -1;
	ma = (unsigned int)simple_strtoul(p + 1, &endptr, 16);
	if (*endptr != ':')
		return -1;
	mi = (unsigned int)simple_strtoul(endptr + 1, &endptr, 16);
	*dev = (unsigned long)MKDEV(ma, mi);
	p = endptr;
	if (*p != ' ')
		return -1;
	*ino = simple_strtoul(p + 1, &endptr, 10);
	p = endptr;
	while (*p == ' ')
		p++;
	*pathname = p;
	return 0;
}

static bool sumh_maps_line_looks_like_header(const char *line, size_t line_len)
{
	size_t i = 0;

	while (i < line_len && isxdigit(line[i]))
		i++;
	return i > 0 && i < line_len && line[i] == '-';
}

static int sumh_filter_maps_lines(const char *src, size_t len, char *dst,
				  size_t dst_size, size_t *written,
				  bool *changed, bool *valid,
				  enum sumh_policy_scope scope,
				  const struct sumh_maps_snapshot *snapshot)
{
	size_t in = 0, out = 0;
	const struct sumh_maps_rule_entry *r;
	const char *pathname;
	char replacement_path[SUMH_MAX_LEN_PATHNAME];
	char header[128];
	char flags[5];
	unsigned long start, end, pgoff, dev, ino;
	unsigned long spoof_ino, spoof_dev;
	size_t path_len, original_path_len, pathname_offset;
	int header_len;
	bool have_replacement_path;
	bool line_changed;
	bool spoof = scope == SUMH_POLICY_SCOPE_SPOOF;

	if (written)
		*written = 0;
	if (changed)
		*changed = false;
	if (valid)
		*valid = true;
	if (!src || !dst || !written)
		return -EINVAL;

	while (in < len) {
		size_t line_start = in;
		size_t line_len;
		bool complete_line;

		while (in < len && src[in] != '\n')
			in++;
		complete_line = in < len && src[in] == '\n';
		line_len = in - line_start;
		if (complete_line)
			line_len++;
		if (!complete_line) {
			if (valid)
				*valid = false;
			return -EINVAL;
		}

		if (sumh_parse_maps_line(src + line_start, line_len, &start,
					 &end, flags, &pgoff, &dev, &ino,
					 &pathname) != 0) {
			/* smaps metadata is intentionally passed through. A
			 * line that starts like a VMA header but cannot be
			 * parsed is unsafe to expose.
			 */
			if (sumh_maps_line_looks_like_header(src + line_start,
							     line_len)) {
				if (valid)
					*valid = false;
				return -EINVAL;
			}
			if (out > dst_size || line_len > dst_size - out)
				return -ENOSPC;
			memcpy(dst + out, src + line_start, line_len);
			out += line_len;
			in++;
			continue;
		}

		pathname_offset = (size_t)(pathname - src);
		if (pathname_offset > line_start + line_len - 1)
			return -EINVAL;
		original_path_len = line_start + line_len - 1 - pathname_offset;
		spoof_ino = ino;
		spoof_dev = dev;
		have_replacement_path = false;
		replacement_path[0] = '\0';

		if (spoof) {
			r = sumh_maps_snapshot_lookup(snapshot, ino, dev);
			if (r) {
				spoof_ino = r->spoofed_ino;
				spoof_dev = r->spoofed_dev;
				/* The stream retains the immutable collection.
				 */
				strscpy(replacement_path, r->spoofed_pathname,
					sizeof(replacement_path));
				have_replacement_path = true;
			}
		}

		if (have_replacement_path) {
			path_len =
			    strnlen(replacement_path, sizeof(replacement_path));
			line_changed =
			    spoof_ino != ino || spoof_dev != dev ||
			    path_len != original_path_len ||
			    memcmp(replacement_path, src + pathname_offset,
				   min(path_len, original_path_len)) != 0;
		} else {
			path_len = original_path_len;
			line_changed = spoof_ino != ino || spoof_dev != dev;
		}

		if (!line_changed) {
			if (out > dst_size || line_len > dst_size - out)
				return -ENOSPC;
			memcpy(dst + out, src + line_start, line_len);
			out += line_len;
			in++;
			continue;
		}

		header_len =
		    scnprintf(header, sizeof(header),
			      "%08lx-%08lx %s %08lx %02x:%02x %lu ", start, end,
			      flags, pgoff, (unsigned int)MAJOR(spoof_dev),
			      (unsigned int)MINOR(spoof_dev), spoof_ino);
		if (header_len <= 0 || header_len >= sizeof(header)) {
			if (valid)
				*valid = false;
			return -EINVAL;
		}
		if (out > dst_size || (size_t)header_len + 1 > dst_size - out ||
		    path_len > dst_size - out - (size_t)header_len - 1)
			return -ENOSPC;
		memcpy(dst + out, header, header_len);
		out += (size_t)header_len;
		if (path_len > 0) {
			if (have_replacement_path)
				memcpy(dst + out, replacement_path, path_len);
			else
				memcpy(dst + out, src + pathname_offset,
				       path_len);
			out += path_len;
		}
		dst[out++] = '\n';
		if (changed)
			*changed = true;
		in++;
	}

	*written = out;
	return 0;
}

/*
 * get_vfs_caps_from_disk kretprobe (cold, exec path only).  When a redirected
 * setcap binary is exec'd, the kernel reads its file capabilities off the
 * *visible* dentry, which is our synthetic vnode with no on-disk
 * security.capability, so the read misses and the binary would lose its caps.
 * The source's caps were parsed and stashed on the vnode at create time; here
 * we simply replay them (atomic-safe: pure copy) and report success.
 *
 * get_vfs_caps_from_disk(idmap/userns, dentry, cpu_caps) places the
 * dentry and output buffer in the second and third argument registers.
 */
static int sumh_get_vfs_caps_entry(struct kretprobe_instance *ri,
				   struct pt_regs *regs)
{
	struct sumh_fscap_ri_data *d = (void *)ri->data;
	const struct dentry *dentry;

	d->have = false;
	d->out = NULL;
	if (!READ_ONCE(sumh_fscaps_enabled))
		return 0;
#if defined(__aarch64__)
	dentry = (const struct dentry *)regs->regs[1];
	d->out = (struct cpu_vfs_cap_data *)regs->regs[2];
#elif defined(__x86_64__)
	dentry = (const struct dentry *)regs->si;
	d->out = (struct cpu_vfs_cap_data *)regs->dx;
#else
	dentry = NULL;
	d->out = NULL;
#endif
	if (!dentry || !d->out)
		return 0;
	if (sumh_vnode_peek_caps(dentry, &d->caps))
		d->have = true;
	return 0;
}

static int sumh_get_vfs_caps_ret(struct kretprobe_instance *ri,
				 struct pt_regs *regs)
{
	struct sumh_fscap_ri_data *d = (void *)ri->data;

	if (!d->have || !d->out)
		return 0;
	*d->out = d->caps;
	/* Report success (0) so the caller applies the replayed caps regardless
	 * of the miss the real read returned on the synthetic inode. */
#if defined(__aarch64__)
	regs->regs[0] = 0;
#elif defined(__x86_64__)
	regs->ax = 0;
#endif
	return 0;
}

static struct kretprobe sumh_krp_get_vfs_caps = {
    .entry_handler = sumh_get_vfs_caps_entry,
    .handler = sumh_get_vfs_caps_ret,
    .data_size = sizeof(struct sumh_fscap_ri_data),
    .maxactive = 64,
};

/* Hook readiness is a capability, not proof that a particular reader's
 * policy, file or allocation permitted filtering. */
bool sumh_proc_proxy_available(void)
{
	return READ_ONCE(sumh_proc_proxy_registered) &&
	       !atomic_read(&sumh_proxy_shutdown);
}

int sumh_proc_proxy_get(void)
{
	unsigned long fd_install_addr;
	bool use_proxy_filter = false;
	int ret = 0;

	mutex_lock(&sumh_proxy_hooks_lock);
	if (sumh_proxy_hook_users)
		goto acquired;
	fd_install_addr = sumh_lookup_name_quiet("fd_install");
	atomic_set(&sumh_proxy_shutdown, 0);
	if (fd_install_addr) {
		sumh_kp_fd_install.addr = (kprobe_opcode_t *)fd_install_addr;
		sumh_kp_fd_install.pre_handler = sumh_fd_install_pre;
		if (!register_kprobe(&sumh_kp_fd_install)) {
			sumh_fd_install_registered = true;
			use_proxy_filter = true;
		} else {
			pr_warn("sumh: register_kprobe(fd_install) "
				"failed\n");
		}
	}
	if (!fd_install_addr)
		pr_warn("sumh: no fd-install ingress found\n");

	if (use_proxy_filter) {
		WRITE_ONCE(sumh_proc_proxy_registered, 1);
		pr_info("sumh: proc views filtered by fd_install fop proxy\n");
	}
	if (!use_proxy_filter) {
		ret = -ENOSYS;
		goto unlock;
	}
acquired:
	sumh_proxy_hook_users++;
unlock:
	mutex_unlock(&sumh_proxy_hooks_lock);
	return ret;
}

void sumh_proc_proxy_put(void)
{
	mutex_lock(&sumh_proxy_hooks_lock);
	if (WARN_ON_ONCE(!sumh_proxy_hook_users))
		goto unlock;
	if (--sumh_proxy_hook_users)
		goto unlock;
	atomic_set(&sumh_proxy_shutdown, 1);
	WRITE_ONCE(sumh_proc_proxy_registered, 0);
	if (sumh_fd_install_registered) {
		unregister_kprobe(&sumh_kp_fd_install);
		sumh_fd_install_registered = false;
	}
	sumh_mount_proxy_drain();
	WARN_ON_ONCE(atomic_read(&sumh_proxy_live));
unlock:
	mutex_unlock(&sumh_proxy_hooks_lock);
}

void sumh_proc_read_hooks_init(void)
{
	unsigned long caps_addr = sumh_lookup_name("get_vfs_caps_from_disk");

	if (caps_addr) {
		sumh_krp_get_vfs_caps.kp.addr = (kprobe_opcode_t *)caps_addr;
		if (register_kretprobe(&sumh_krp_get_vfs_caps) == 0) {
			sumh_fscap_kretprobe_registered = 1;
			pr_info("sumh: source file-capability replay via "
				"get_vfs_caps_from_disk\n");
		} else {
			pr_warn("sumh: "
				"register_kretprobe(get_vfs_caps_from_disk) "
				"failed\n");
		}
	} else {
		pr_warn("sumh: get_vfs_caps_from_disk not found, redirected "
			"fscaps disabled\n");
	}

	sumh_view_proxy_held = sumh_proc_proxy_get() == 0;
}

void sumh_proc_read_hooks_stop_new(void)
{
	if (sumh_view_proxy_held) {
		sumh_proc_proxy_put();
		sumh_view_proxy_held = false;
	}
	if (sumh_fscap_kretprobe_registered) {
		unregister_kretprobe(&sumh_krp_get_vfs_caps);
		sumh_fscap_kretprobe_registered = 0;
	}
}

void sumh_proc_read_hooks_exit(void)
{
	sumh_proc_read_hooks_stop_new();
	/* Exclusive lifecycle shutdown excludes ioctl writers; existing proxy
	 * references (if any during unwind) retain their own collection. */
	sumh_maps_store_clear();
}
