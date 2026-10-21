#include <linux/fs.h>
#include <linux/file.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uio.h>
#include <linux/version.h>
#include <linux/xattr.h>

#include "sumh_base.h"
#include "sumh_path_policy.h"
#include "sumh_runtime.h"
#include "sumh_sop_shadow.h"
#include "sumh_types.h"
#include "sumh_vnode.h"
#include "sumh_dirhijack.h"
#include "feature/sucompat_vfs.h"

/* i_op->getattr / *attr first argument varies across kernel versions. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
#define KVN_IDMAP_ARG struct mnt_idmap *idmap,
#define KVN_IDMAP_CALL idmap,
#else
#define KVN_IDMAP_ARG struct user_namespace *userns,
#define KVN_IDMAP_CALL userns,
#endif

/*
 * Idmap of the *source* mount for a delegated create/mkdir/... — the redirect
 * writes through the source's own mount, so the source idmap governs uid/gid
 * mapping, not the visible idmap the VFS handed our op.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
#define KVN_SRC_IDMAP(mnt) mnt_idmap(mnt),
#else
#define KVN_SRC_IDMAP(mnt) mnt_user_ns(mnt),
#endif

static int sumh_vnode_src_want_write(const struct path *src);
static void sumh_vnode_src_drop_write(const struct path *src);

static const struct inode_operations sumh_vnode_file_iops;
static const struct inode_operations sumh_vnode_dir_iops;
static const struct file_operations sumh_vnode_file_fops;
static const struct file_operations sumh_vnode_dir_fops;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
static const struct file_operations sumh_vnode_file_fops_mmap_prepare;
#endif

/*
 * A dedicated i_rwsem lockdep class for every SUMH vnode.  The vnode lives on
 * the *visible* (real) superblock — the same fs type as its source — so without
 * this a delegated vfs_unlink/vfs_rmdir on the source, run from inside our own
 * ->unlink/->rmdir while the VFS holds the visible victim's i_rwsem, would look
 * to LOCKDEP like same-class recursive locking (a false positive: one task,
 * strict visible->source order, no cycle).  Giving vnodes their own class is
 * what a stacking filesystem gets implicitly from being its own fs type.
 * Compiles out entirely without CONFIG_LOCKDEP.
 */
static struct lock_class_key sumh_vnode_i_mutex_key;

bool sumh_vnode_is_ours(const struct inode *inode)
{
	const struct inode_operations *iop;
	if (!inode)
		return false;
	iop = sumh_dirhijack_original_iops(inode);
	return iop == &sumh_vnode_file_iops || iop == &sumh_vnode_dir_iops ||
	       ksu_sucompat_vfs_is_inode(inode);
}

bool sumh_vnode_peek_caps(const struct dentry *dentry,
			  struct cpu_vfs_cap_data *out)
{
	struct inode *inode = dentry ? d_inode(dentry) : NULL;
	struct sumh_vnode_info *info;

	if (!inode || !out || !sumh_vnode_is_ours(inode))
		return false;
	info = inode->i_private;
	if (!info || !info->has_caps)
		return false;
	*out = info->caps;
	return true;
}

void sumh_vnode_free_info(struct inode *inode)
{
	struct sumh_vnode_info *info;
	bool sop_vnode_ref;

	if (!inode || !sumh_vnode_is_ours(inode))
		return;
	info = inode->i_private;
	if (!info)
		return;
	sop_vnode_ref = info->sop_vnode_ref;
	if (info->source.dentry)
		path_put(&info->source);
	kfree(info->visible_path);
	inode->i_private = NULL;
	kfree(info);
	if (sop_vnode_ref)
		sumh_sop_vnode_put(inode->i_sb);
}

/* ---- identity ---------------------------------------------------------- */

static int SUMH_NOCFI sumh_vnode_getattr(KVN_IDMAP_ARG const struct path *path,
					 struct kstat *stat, u32 request_mask,
					 unsigned int query_flags)
{
	struct inode *vi = d_inode(path->dentry);
	struct sumh_vnode_info *info = vi ? vi->i_private : NULL;
	int ret;

	if (!info)
		return -EIO;

	/* Live attributes come from the source; identity is projected so that
	 * stat/fstat/maps/ls all report the same (dev, ino) pair — dev is this
	 * inode's real superblock (the visible path's filesystem), ino is the
	 * scheme-A stable value captured in the rule. */
	if (!info->source.dentry) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
		generic_fillattr(KVN_IDMAP_CALL request_mask, vi, stat);
#else
		generic_fillattr(KVN_IDMAP_CALL vi, stat);
#endif
		stat->ino = info->v_ino;
		stat->dev = vi->i_sb->s_dev;
		/* A special (char/blk/fifo) wrapper is minted S_IFREG; present
		 * the source's real type and rdev even on this source-less
		 * fallback. */
		if (info->special_mode) {
			stat->mode = (stat->mode & 07777) |
				     (info->special_mode & S_IFMT);
			stat->rdev = info->special_rdev;
		}
		return 0;
	}

	/* The VFS invokes i_op->getattr from vfs_getattr_nosec() with the
	 * internal AT_GETATTR_NOSEC bit set in query_flags.  Forwarding that to
	 * a security- level getattr trips WARN_ON_ONCE in vfs_getattr() and
	 * returns -EPERM, so request a plain stat-consistent sync of the source
	 * instead. */
	ret = sumh_vfs_getattr_unprojected(&info->source, stat, request_mask,
					   AT_STATX_SYNC_AS_STAT);
	if (ret == 0) {
		stat->ino = info->v_ino;
		stat->dev = vi->i_sb->s_dev;
	}
	return ret;
}

/*
 * A SUMH vnode is a synthetic inode on the visible superblock with no xattr
 * backing.  security.selinux is served by the cloned SID through the LSM, but
 * arbitrary names (user.*, security.capability, ...) cannot be delegated to the
 * source without a system-wide hot-path getxattr callback that is not cleanly
 * unload-safe (getxattr has no quiesce point).  So present NO listed xattrs:
 * this keeps listxattr consistent with getxattr, which returns -ENODATA for
 * non-security names on the synthetic inode, and removes the
 * listxattr<->getxattr mismatch a detector could otherwise use.
 * security.selinux stays gettable via the LSM, exactly as on any file whose
 * listxattr does not enumerate the permission-gated security namespace.
 */
static ssize_t SUMH_NOCFI sumh_vnode_listxattr(struct dentry *dentry,
					       char *buffer, size_t size)
{
	(void)dentry;
	(void)buffer;
	(void)size;
	return 0;
}

static const char *SUMH_NOCFI sumh_vnode_get_link(struct dentry *dentry,
						  struct inode *inode,
						  struct delayed_call *done)
{
	struct sumh_vnode_info *info = inode ? inode->i_private : NULL;
	struct inode *r_inode;

	if (!info || !info->source.dentry)
		return ERR_PTR(-ECHILD);
	r_inode = d_inode(info->source.dentry);
	if (!r_inode || !r_inode->i_op || !r_inode->i_op->get_link)
		return ERR_PTR(-EINVAL);
	return r_inode->i_op->get_link(dentry ? info->source.dentry : NULL,
				       r_inode, done);
}

/*
 * Delegate an attribute change (chmod/chown/utimes/truncate) to the pinned
 * source, so a redirected path is writable-through like a bind mount: the
 * source's DAC/SELinux govern, evaluated against the caller's creds inside
 * notify_change.  A pure-virtual directory has no backing store and is
 * read-only.  Mutation sink (Final Phase 1a).
 */
static int SUMH_NOCFI sumh_vnode_setattr(KVN_IDMAP_ARG struct dentry *dentry,
					 struct iattr *attr)
{
	struct inode *vi = d_inode(dentry);
	struct sumh_vnode_info *info = vi ? vi->i_private : NULL;
	struct dentry *src_dentry;
	struct inode *src_inode;
	struct iattr sattr;
	int ret;

	if (!info || !info->source.dentry)
		return -EROFS;
	if (!sumh_notify_change)
		return -EOPNOTSUPP;
	/* Sleeping callback on our static i_op table — keep the module-refcount
	 * gate up against a cooperative unload, like the create family. */
	if (!try_module_get(THIS_MODULE))
		return -ENOENT;
	src_dentry = info->source.dentry;
	src_inode = d_inode(src_dentry);
	if (!src_inode) {
		module_put(THIS_MODULE);
		return -ENOENT;
	}
	ret = sumh_vnode_src_want_write(&info->source);
	if (ret) {
		module_put(THIS_MODULE);
		return ret;
	}

	/* ATTR_FILE points at the virtual file; swap it for the opened source
	 * file so a truncate reaches the source's fs/driver, not the vnode. */
	sattr = *attr;
	if (sattr.ia_valid & ATTR_FILE) {
		struct file *rf = (sattr.ia_file && sattr.ia_file->private_data)
				      ? sattr.ia_file->private_data
				      : NULL;
		if (rf)
			sattr.ia_file = rf;
		else
			sattr.ia_valid &= ~ATTR_FILE;
	}

	inode_lock(src_inode);
	ret = sumh_notify_change(KVN_SRC_IDMAP(info->source.mnt) src_dentry,
				 &sattr, NULL);
	inode_unlock(src_inode);
	sumh_vnode_src_drop_write(&info->source);
	module_put(THIS_MODULE);
	return ret;
}

/* ---- data plane: delegate to the pinned source ------------------------- */

static int SUMH_NOCFI sumh_vnode_open(struct inode *inode, struct file *file)
{
	struct sumh_vnode_info *info = inode->i_private;
	struct file *real_file;

	if (!info || !info->source.dentry)
		return -ENODEV;
	real_file = dentry_open(&info->source, file->f_flags, file->f_cred);
	if (IS_ERR(real_file))
		return PTR_ERR(real_file);
	file->private_data = real_file;
	return 0;
}

static int sumh_vnode_release(struct inode *inode, struct file *file)
{
	struct file *real_file = file->private_data;

	if (real_file) {
		fput(real_file);
		file->private_data = NULL;
	}
	return 0;
}

static loff_t sumh_vnode_llseek(struct file *file, loff_t offset, int whence)
{
	struct file *real_file = file->private_data;
	loff_t ret;

	if (!real_file)
		return -EINVAL;
	real_file->f_pos = file->f_pos;
	ret = vfs_llseek(real_file, offset, whence);
	file->f_pos = real_file->f_pos;
	return ret;
}

static ssize_t SUMH_NOCFI sumh_vnode_read_iter(struct kiocb *iocb,
					       struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct file *real_file = file->private_data;
	ssize_t ret;

	if (!real_file || !real_file->f_op || !real_file->f_op->read_iter)
		return -EINVAL;
	iocb->ki_filp = real_file;
	ret = real_file->f_op->read_iter(iocb, to);
	iocb->ki_filp = file;
	return ret;
}

static ssize_t SUMH_NOCFI sumh_vnode_write_iter(struct kiocb *iocb,
						struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct file *real_file = file->private_data;
	ssize_t ret;

	if (!real_file || !real_file->f_op || !real_file->f_op->write_iter)
		return -EINVAL;
	iocb->ki_filp = real_file;
	ret = real_file->f_op->write_iter(iocb, from);
	iocb->ki_filp = file;
	return ret;
}

static int SUMH_NOCFI sumh_vnode_mmap(struct file *file,
				      struct vm_area_struct *vma)
{
	/* The virtual inode shares the source's address_space (set at create),
	 * so the generic mapping path serves source pages directly. */
	return generic_file_mmap(file, vma);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
static int SUMH_NOCFI sumh_vnode_mmap_prepare(struct vm_area_desc *desc)
{
	return generic_file_mmap_prepare(desc);
}
#endif

static long SUMH_NOCFI sumh_vnode_ioctl(struct file *file, unsigned int cmd,
					unsigned long arg)
{
	struct file *real_file = file->private_data;

	if (!real_file || !real_file->f_op || !real_file->f_op->unlocked_ioctl)
		return -ENOTTY;
	return real_file->f_op->unlocked_ioctl(real_file, cmd, arg);
}

#ifdef CONFIG_COMPAT
static long SUMH_NOCFI sumh_vnode_compat_ioctl(struct file *file,
					       unsigned int cmd,
					       unsigned long arg)
{
	struct file *real_file = file->private_data;

	if (!real_file || !real_file->f_op || !real_file->f_op->compat_ioctl)
		return -ENOTTY;
	return real_file->f_op->compat_ioctl(real_file, cmd, arg);
}
#endif

static ssize_t SUMH_NOCFI sumh_vnode_splice_read(struct file *in, loff_t *ppos,
						 struct pipe_inode_info *pipe,
						 size_t len, unsigned int flags)
{
	struct file *real_file = in->private_data;

	if (!real_file || !real_file->f_op || !real_file->f_op->splice_read)
		return -EINVAL;
	return real_file->f_op->splice_read(real_file, ppos, pipe, len, flags);
}

static int SUMH_NOCFI sumh_vnode_fsync(struct file *file, loff_t start,
				       loff_t end, int datasync)
{
	struct file *real_file = file->private_data;

	if (!real_file || !real_file->f_op || !real_file->f_op->fsync)
		return -EINVAL;
	return real_file->f_op->fsync(real_file, start, end, datasync);
}

static const struct inode_operations sumh_vnode_file_iops = {
    .getattr = sumh_vnode_getattr,
    .setattr = sumh_vnode_setattr,
    .listxattr = sumh_vnode_listxattr,
    .get_link = sumh_vnode_get_link,
};

static const struct file_operations sumh_vnode_file_fops = {
    .owner = THIS_MODULE,
    .open = sumh_vnode_open,
    .release = sumh_vnode_release,
    .llseek = sumh_vnode_llseek,
    .read_iter = sumh_vnode_read_iter,
    .write_iter = sumh_vnode_write_iter,
    .mmap = sumh_vnode_mmap,
    .unlocked_ioctl = sumh_vnode_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = sumh_vnode_compat_ioctl,
#endif
    .splice_read = sumh_vnode_splice_read,
    .fsync = sumh_vnode_fsync,
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
static const struct file_operations sumh_vnode_file_fops_mmap_prepare = {
    .owner = THIS_MODULE,
    .open = sumh_vnode_open,
    .release = sumh_vnode_release,
    .llseek = sumh_vnode_llseek,
    .read_iter = sumh_vnode_read_iter,
    .write_iter = sumh_vnode_write_iter,
    .mmap_prepare = sumh_vnode_mmap_prepare,
    .unlocked_ioctl = sumh_vnode_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = sumh_vnode_compat_ioctl,
#endif
    .splice_read = sumh_vnode_splice_read,
    .fsync = sumh_vnode_fsync,
};
#endif

/* ---- special vnode: char/blk/fifo source, full fop delegation --------------
 *
 * The inode is minted S_IFREG (sumh_vnode_new) so path_openat's may_open does
 * not apply the device (may_open_dev) check on the nodev visible mount; .open
 * (sumh_vnode_open) then opens the *real* source via dentry_open, which
 * routes through chrdev_open/blkdev/fifo_open and bypasses may_open entirely.
 * Read, write, poll, mmap, ioctl and fasync forward to that real file.  getattr
 * still projects the source's real type and rdev, so userspace sees a
 * device/fifo.
 */
static ssize_t SUMH_NOCFI sumh_vnode_special_read(struct file *file,
						  char __user *buf, size_t len,
						  loff_t *ppos)
{
	struct file *real = file->private_data;

	if (!real || !sumh_vfs_read)
		return -EINVAL;
	return sumh_vfs_read(real, buf, len, ppos);
}

static ssize_t SUMH_NOCFI sumh_vnode_special_write(struct file *file,
						   const char __user *buf,
						   size_t len, loff_t *ppos)
{
	struct file *real = file->private_data;

	if (!real || !sumh_vfs_write)
		return -EINVAL;
	return sumh_vfs_write(real, buf, len, ppos);
}

static __poll_t SUMH_NOCFI
sumh_vnode_special_poll(struct file *file, struct poll_table_struct *wait)
{
	struct file *real = file->private_data;

	if (!real)
		return EPOLLERR;
	return vfs_poll(real, wait);
}

static int SUMH_NOCFI sumh_vnode_special_mmap(struct file *file,
					      struct vm_area_struct *vma)
{
	struct file *real = file->private_data;

	if (!real || !real->f_op || !real->f_op->mmap)
		return -ENODEV;
	/* Redirect the vma onto the real device file (drops the ref on our
	 * outer file, takes one on the real file), then run the device's own
	 * mmap. */
	vma_set_file(vma, real);
	return real->f_op->mmap(real, vma);
}

static int SUMH_NOCFI sumh_vnode_special_fasync(int fd, struct file *file,
						int on)
{
	struct file *real = file->private_data;

	if (real && real->f_op && real->f_op->fasync)
		return real->f_op->fasync(fd, real, on);
	return 0;
}

static const struct file_operations sumh_vnode_special_fops = {
    .owner = THIS_MODULE,
    .open = sumh_vnode_open,
    .release = sumh_vnode_release,
    .llseek = sumh_vnode_llseek,
    .read = sumh_vnode_special_read,
    .write = sumh_vnode_special_write,
    .poll = sumh_vnode_special_poll,
    .unlocked_ioctl = sumh_vnode_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = sumh_vnode_compat_ioctl,
#endif
    .mmap = sumh_vnode_special_mmap,
    .fasync = sumh_vnode_special_fasync,
    .splice_read = sumh_vnode_splice_read,
    .fsync = sumh_vnode_fsync,
};

/* ---- directory vnode: lookup/iterate delegate to the pinned source dir ----
 */

static int SUMH_NOCFI sumh_vnode_dir_open(struct inode *inode,
					  struct file *file)
{
	struct sumh_vnode_info *info = inode->i_private;
	struct file *real_dir;

	if (!info)
		return -ENOTDIR;
	if (!info->source.dentry) {
		/* Pure-virtual directory: nothing to open; iterate enumerates
		 * the rule table.  release() tolerates a NULL private_data. */
		file->private_data = NULL;
		return 0;
	}
	real_dir =
	    dentry_open(&info->source, O_RDONLY | O_DIRECTORY, file->f_cred);
	if (IS_ERR(real_dir))
		return PTR_ERR(real_dir);
	file->private_data = real_dir;
	return 0;
}

struct sumh_vnode_dir_ctx {
	struct dir_context ctx;
	struct dir_context *orig;
	dev_t source_dev;
	unsigned long self_ino;
};

static bool SUMH_NOCFI sumh_vnode_dir_actor(struct dir_context *ctx,
					    const char *name, int namlen,
					    loff_t offset, u64 ino,
					    unsigned int d_type)
{
	struct sumh_vnode_dir_ctx *dc =
	    container_of(ctx, struct sumh_vnode_dir_ctx, ctx);
	bool ret;
	u64 proj;

	/* Project each child's identity so getdents d_ino matches a later stat.
	 * "." maps to this vnode's own ino; ".." is left as the source value
	 * (the real parent's identity is projected where that dir is itself
	 * virtual); everything else goes through the scheme-A projection. */
	if (namlen == 1 && name[0] == '.')
		proj = dc->self_ino;
	else if (namlen == 2 && name[0] == '.' && name[1] == '.')
		proj = ino;
	else
		proj = sumh_vnode_source_ino(dc->source_dev, ino);

	dc->orig->pos = dc->ctx.pos;
	ret = dc->orig->actor(dc->orig, name, namlen, offset, proj, d_type);
	dc->ctx.pos = dc->orig->pos;
	return ret;
}

/* Enumerate a pure-virtual directory's children from the rule table. */
static int SUMH_NOCFI sumh_vnode_dir_iterate_virtual(
    struct file *file, struct dir_context *ctx, struct sumh_vnode_info *info)
{
	struct list_head children;
	struct sumh_name_list *item, *tmp;
	loff_t idx = 2; /* children follow . and .. */
	int ret;

	if (!info->visible_path)
		return -ENOTDIR;
	INIT_LIST_HEAD(&children);
	/* Build before advancing even the dot cookies. On allocation failure no
	 * names are published and a retry starts at the same position. */
	ret = sumh_rule_vpath_emit(info->visible_path, &children);
	if (ret < 0)
		goto out;
	ret = 0;
	if (!dir_emit_dots(file, ctx))
		goto out;
	list_for_each_entry_safe (item, tmp, &children, list) {
		bool stop = false;

		if (idx >= ctx->pos) {
			if (!dir_emit(ctx, item->name, strlen(item->name),
				      item->ino ? item->ino : (u64)idx,
				      item->type))
				stop = true;
			else
				ctx->pos = idx + 1;
		}
		list_del(&item->list);
		kfree(item->name);
		kfree(item);
		idx++;
		if (stop)
			break;
	}
out:
	list_for_each_entry_safe (item, tmp, &children, list) {
		list_del(&item->list);
		kfree(item->name);
		kfree(item);
	}
	return ret;
}

static int SUMH_NOCFI sumh_vnode_dir_iterate(struct file *file,
					     struct dir_context *ctx)
{
	struct file *real_dir = file->private_data;
	struct inode *vi = file_inode(file);
	struct sumh_vnode_info *info = vi ? vi->i_private : NULL;
	struct sumh_vnode_dir_ctx dc = {.ctx.actor = sumh_vnode_dir_actor};
	struct inode *r_inode;
	int ret;

	if (!info)
		return -ENOTDIR;
	if (!info->source.dentry)
		return sumh_vnode_dir_iterate_virtual(file, ctx, info);
	if (!real_dir)
		return -ENOTDIR;
	r_inode = d_inode(info->source.dentry);
	dc.source_dev = (r_inode && r_inode->i_sb) ? r_inode->i_sb->s_dev : 0;
	dc.self_ino = info->v_ino;
	dc.orig = ctx;
	/* iterate_dir keeps the cursor in the source dir file's f_pos; drive it
	 * from the vnode's requested position so rewinddir/seek work, and copy
	 * the advanced position back out. */
	real_dir->f_pos = ctx->pos;
	dc.ctx.pos = ctx->pos;
	ret = iterate_dir(real_dir, &dc.ctx);
	ctx->pos = real_dir->f_pos;
	return ret;
}

static struct dentry *SUMH_NOCFI sumh_vnode_dir_lookup(struct inode *dir,
						       struct dentry *dentry,
						       unsigned int flags)
{
	struct sumh_vnode_info *info = dir->i_private;
	struct inode *cvi = NULL;
	char *name;

	(void)flags;
	if (!info)
		return ERR_PTR(-ENOENT);
	if (dentry->d_name.len > NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);
	/* A sleeping path-walk callback on a static module op table: pin the
	 * module so a cooperative unload (prepare_unload's module-refcount
	 * gate) cannot free our text mid-lookup. */
	if (!try_module_get(THIS_MODULE))
		return ERR_PTR(-ENOENT);
	name = kstrndup(dentry->d_name.name, dentry->d_name.len, GFP_KERNEL);
	if (!name) {
		module_put(THIS_MODULE);
		return ERR_PTR(-ENOMEM);
	}

	if (info->source.dentry && info->source.mnt) {
		/* Backed directory: resolve the child within the pinned source
		 * dir (single component, nofollow so a symlink child stays a
		 * symlink). */
		struct path child = {};
		struct inode *ci;
		int ret;

		ret = vfs_path_lookup(info->source.dentry, info->source.mnt,
				      name, 0, &child);
		kfree(name);
		module_put(THIS_MODULE);
		if (ret) {
			if (ret == -ENOENT) {
				d_add(dentry, NULL);
				return NULL;
			}
			return ERR_PTR(ret);
		}
		ci = d_inode(child.dentry);
		if (ci) {
			u8 cf = S_ISDIR(ci->i_mode)   ? SUMH_VNODE_F_DIR
				: S_ISLNK(ci->i_mode) ? SUMH_VNODE_F_LNK
						      : 0;

			cvi = sumh_vnode_new(
			    dir->i_sb, &child,
			    sumh_vnode_source_ino(
				ci->i_sb ? ci->i_sb->s_dev : 0, (u64)ci->i_ino),
			    ci->i_mode, cf);
		}
		path_put(&child);
	} else if (info->visible_path) {
		/* Pure-virtual directory: resolve the child against the rule
		 * table — an exact rule is a leaf redirect, a prefix is a
		 * deeper virtual dir. */
		struct path leaf = {};
		umode_t lmode = 0;
		unsigned long lino = 0;
		int kind = sumh_rule_vpath_child(info->visible_path, name,
						 &leaf, &lmode, &lino);

		if (kind == SUMH_VPATH_LEAF) {
			u8 cf = S_ISDIR(lmode)	 ? SUMH_VNODE_F_DIR
				: S_ISLNK(lmode) ? SUMH_VNODE_F_LNK
						 : 0;

			cvi = sumh_vnode_new(dir->i_sb, &leaf, lino, lmode, cf);
			path_put(&leaf);
		} else if (kind == SUMH_VPATH_VDIR) {
			char *cvp = kasprintf(GFP_KERNEL, "%s/%s",
					      info->visible_path, name);

			if (cvp) {
				cvi = sumh_vnode_new_virtual(
				    dir->i_sb, cvp, sumh_vnode_vpath_ino(cvp),
				    dir);
				kfree(cvp);
			}
		}
		kfree(name);
		module_put(THIS_MODULE);
		if (kind < 0)
			return ERR_PTR(kind);
		if (kind == SUMH_VPATH_NONE) {
			d_add(dentry, NULL);
			return NULL;
		}
	} else {
		kfree(name);
		module_put(THIS_MODULE);
		return ERR_PTR(-ENOENT);
	}

	if (!cvi)
		return ERR_PTR(-ENOMEM);
	return d_splice_alias(cvi, dentry);
}

/* ---- directory vnode: create family delegates to the pinned source dir ----
 *
 * When a directory-source redirect's vnode receives a namespace mutation
 * (create/mkdir/mknod/symlink/unlink/rmdir/link), forward it to the pinned
 * source directory via the captured vfs_* helper, so the redirect is
 * writable-through like a bind mount: the source's DAC/SELinux govern,
 * evaluated against the caller's creds inside the vfs_* helper.  A pure-virtual
 * directory (F_VIRTUAL_DIR, no source) is a synthetic container and read-only.
 * Mutation sink (Final Phase 1b).
 */

/*
 * Take freeze / read-only write protection on the source mount for a mutation.
 * Both captured helpers gate as a pair: if either is missing we proceed
 * unprotected (best-effort, mirrors the captured-xattr write path) rather than
 * fail.  Returns 0 to proceed, or a negative errno when the write is denied
 * (a read-only or frozen source) — in which case drop_write must NOT run.
 */
static int SUMH_NOCFI sumh_vnode_src_want_write(const struct path *src)
{
	int (*want)(struct vfsmount *) = sumh_mnt_want_write_addr;

	if (!want || !sumh_mnt_drop_write_addr)
		return 0;
	return want(src->mnt);
}

static void SUMH_NOCFI sumh_vnode_src_drop_write(const struct path *src)
{
	void (*drop)(struct vfsmount *) = sumh_mnt_drop_write_addr;

	if (drop && sumh_mnt_want_write_addr)
		drop(src->mnt);
}

/*
 * Manufacture a child dentry inside the pinned source directory named like the
 * visible target @vis.  On success returns the source child (negative for a
 * create, positive for a remove) with the source dir inode LOCKED — the caller
 * releases with inode_unlock(*dir_out) + dput(child).  I_MUTEX_PARENT2 keeps
 * lockdep from confusing this with the visible parent our VFS caller already
 * holds at I_MUTEX_PARENT.  On error returns ERR_PTR and leaves nothing locked.
 */
static SUMH_NOCFI struct dentry *
sumh_vnode_src_child(struct sumh_vnode_info *info, const struct dentry *vis,
		     struct inode **dir_out)
{
	struct inode *src_dir;
	struct dentry *child;

	if (!info->source.dentry || !info->source.mnt)
		return ERR_PTR(-EROFS);
	if (!sumh_lookup_one_len)
		return ERR_PTR(-EOPNOTSUPP);
	src_dir = d_inode(info->source.dentry);
	if (!src_dir || !S_ISDIR(src_dir->i_mode))
		return ERR_PTR(-ENOTDIR);
	inode_lock_nested(src_dir, I_MUTEX_PARENT2);
	child = sumh_lookup_one_len(vis->d_name.name, info->source.dentry,
				    vis->d_name.len);
	if (IS_ERR(child)) {
		inode_unlock(src_dir);
		return child;
	}
	*dir_out = src_dir;
	return child;
}

/*
 * Begin a delegated directory mutation.  Pin the module first: like
 * sumh_vnode_dir_lookup, these are sleeping callbacks on our static i_op
 * table, so an in-flight op must keep the module-refcount gate raised or a
 * cooperative unload could free our text underneath it.  Then take source-mount
 * write protection and manufacture the locked source child.  On any failure
 * everything acquired here is unwound and an ERR_PTR is returned; on success
 * pair with sumh_vnode_src_end().
 */
static struct dentry *sumh_vnode_src_begin(struct sumh_vnode_info *info,
					   const struct dentry *vis,
					   struct inode **dir_out)
{
	struct dentry *child;
	int ret;

	if (!try_module_get(THIS_MODULE))
		return ERR_PTR(-ENOENT);
	ret = sumh_vnode_src_want_write(&info->source);
	if (ret) {
		module_put(THIS_MODULE);
		return ERR_PTR(ret);
	}
	child = sumh_vnode_src_child(info, vis, dir_out);
	if (IS_ERR(child)) {
		sumh_vnode_src_drop_write(&info->source);
		module_put(THIS_MODULE);
	}
	return child;
}

static void sumh_vnode_src_end(struct sumh_vnode_info *info,
			       struct inode *src_dir, struct dentry *child)
{
	inode_unlock(src_dir);
	sumh_vnode_src_drop_write(&info->source);
	if (child)
		dput(child);
	module_put(THIS_MODULE);
}

/*
 * A create-family op just made @src_child positive in the source dir; publish
 * it on the visible side by splicing a fresh vnode over it onto @vis so a later
 * stat/open/iterate resolves through the vnode.  @src_child stays owned by the
 * caller (the new vnode pins its own reference).
 */
static int SUMH_NOCFI sumh_vnode_publish_child(struct super_block *sb,
					       const struct path *src_parent,
					       struct dentry *src_child,
					       struct dentry *vis)
{
	struct path cpath = {.mnt = src_parent->mnt, .dentry = src_child};
	struct inode *ci = d_inode(src_child);
	struct inode *cvi;
	u8 cf;

	if (!ci)
		return -ENOENT;
	cf = S_ISDIR(ci->i_mode)   ? SUMH_VNODE_F_DIR
	     : S_ISLNK(ci->i_mode) ? SUMH_VNODE_F_LNK
				   : 0;
	cvi =
	    sumh_vnode_new(sb, &cpath,
			   sumh_vnode_source_ino(ci->i_sb ? ci->i_sb->s_dev : 0,
						 (u64)ci->i_ino),
			   ci->i_mode, cf);
	if (!cvi)
		return -ENOMEM;
	d_instantiate(vis, cvi);
	return 0;
}

static int SUMH_NOCFI sumh_vnode_dir_create(KVN_IDMAP_ARG struct inode *dir,
					    struct dentry *dentry, umode_t mode,
					    bool excl)
{
	struct sumh_vnode_info *info = dir->i_private;
	struct inode *src_dir = NULL;
	struct dentry *child;
	int ret;

	if (!info)
		return -ENOTDIR;
	if (!info->source.dentry)
		return -EROFS;
	if (!sumh_vfs_create)
		return -EOPNOTSUPP;
	child = sumh_vnode_src_begin(info, dentry, &src_dir);
	if (IS_ERR(child))
		return PTR_ERR(child);
	ret = sumh_vfs_create(KVN_SRC_IDMAP(info->source.mnt) src_dir, child,
			      mode, excl);
	if (ret == 0)
		ret = sumh_vnode_publish_child(dir->i_sb, &info->source, child,
					       dentry);
	sumh_vnode_src_end(info, src_dir, child);
	return ret;
}

static int SUMH_NOCFI sumh_vnode_dir_mkdir_common(struct inode *dir,
						  struct dentry *dentry,
						  umode_t mode)
{
	struct sumh_vnode_info *info = dir->i_private;
	struct inode *src_dir = NULL;
	struct dentry *child;
	int ret;

	if (!info)
		return -ENOTDIR;
	if (!info->source.dentry)
		return -EROFS;
	if (!sumh_vfs_mkdir)
		return -EOPNOTSUPP;
	child = sumh_vnode_src_begin(info, dentry, &src_dir);
	if (IS_ERR(child))
		return PTR_ERR(child);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
	{
		struct dentry *created;

		/* vfs_mkdir() consumes @child on error or when it substitutes a
		 * different dentry, and returns the dentry the caller must
		 * release. */
		created = sumh_vfs_mkdir(
		    KVN_SRC_IDMAP(info->source.mnt) src_dir, child, mode);
		if (IS_ERR(created)) {
			ret = PTR_ERR(created);
			child = NULL;
		} else {
			child = created;
			ret = sumh_vnode_publish_child(dir->i_sb, &info->source,
						       child, dentry);
		}
	}
#else
	ret = sumh_vfs_mkdir(KVN_SRC_IDMAP(info->source.mnt) src_dir, child,
			     mode);
	if (ret == 0)
		ret = sumh_vnode_publish_child(dir->i_sb, &info->source, child,
					       dentry);
#endif
	sumh_vnode_src_end(info, src_dir, child);
	return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
static struct dentry *
    SUMH_NOCFI sumh_vnode_dir_mkdir(KVN_IDMAP_ARG struct inode *dir,
				    struct dentry *dentry, umode_t mode)
{
	int ret = sumh_vnode_dir_mkdir_common(dir, dentry, mode);

	return ret ? ERR_PTR(ret) : NULL;
}
#else
static int SUMH_NOCFI sumh_vnode_dir_mkdir(KVN_IDMAP_ARG struct inode *dir,
					   struct dentry *dentry, umode_t mode)
{
	return sumh_vnode_dir_mkdir_common(dir, dentry, mode);
}
#endif

static int SUMH_NOCFI sumh_vnode_dir_mknod(KVN_IDMAP_ARG struct inode *dir,
					   struct dentry *dentry, umode_t mode,
					   dev_t dev)
{
	struct sumh_vnode_info *info = dir->i_private;
	struct inode *src_dir = NULL;
	struct dentry *child;
	int ret;

	if (!info)
		return -ENOTDIR;
	if (!info->source.dentry)
		return -EROFS;
	if (!sumh_vfs_mknod)
		return -EOPNOTSUPP;
	child = sumh_vnode_src_begin(info, dentry, &src_dir);
	if (IS_ERR(child))
		return PTR_ERR(child);
	/* A device node created on the (nodev) source is real but unopenable —
	 * consistent with the source itself; pass the result through untouched.
	 */
	ret = sumh_vfs_mknod(KVN_SRC_IDMAP(info->source.mnt) src_dir, child,
			     mode, dev);
	if (ret == 0)
		ret = sumh_vnode_publish_child(dir->i_sb, &info->source, child,
					       dentry);
	sumh_vnode_src_end(info, src_dir, child);
	return ret;
}

static int SUMH_NOCFI sumh_vnode_dir_symlink(KVN_IDMAP_ARG struct inode *dir,
					     struct dentry *dentry,
					     const char *oldname)
{
	struct sumh_vnode_info *info = dir->i_private;
	struct inode *src_dir = NULL;
	struct dentry *child;
	int ret;

	if (!info)
		return -ENOTDIR;
	if (!info->source.dentry)
		return -EROFS;
	if (!sumh_vfs_symlink)
		return -EOPNOTSUPP;
	child = sumh_vnode_src_begin(info, dentry, &src_dir);
	if (IS_ERR(child))
		return PTR_ERR(child);
	ret = sumh_vfs_symlink(KVN_SRC_IDMAP(info->source.mnt) src_dir, child,
			       oldname);
	if (ret == 0)
		ret = sumh_vnode_publish_child(dir->i_sb, &info->source, child,
					       dentry);
	sumh_vnode_src_end(info, src_dir, child);
	return ret;
}

static int SUMH_NOCFI sumh_vnode_dir_unlink(struct inode *dir,
					    struct dentry *dentry)
{
	struct sumh_vnode_info *info = dir->i_private;
	struct inode *src_dir = NULL;
	struct dentry *child;
	int ret;

	if (!info)
		return -ENOTDIR;
	if (!info->source.dentry)
		return -EROFS;
	if (!sumh_vfs_unlink)
		return -EOPNOTSUPP;
	child = sumh_vnode_src_begin(info, dentry, &src_dir);
	if (IS_ERR(child))
		return PTR_ERR(child);
	if (d_really_is_negative(child))
		ret = -ENOENT;
	else
		ret = sumh_vfs_unlink(KVN_SRC_IDMAP(info->source.mnt) src_dir,
				      child, NULL);
	sumh_vnode_src_end(info, src_dir, child);
	/* Reflect the removal on the synthetic child so it evicts with nlink 0;
	 * the VFS caller d_delete()s the visible dentry after we return. */
	if (ret == 0 && d_really_is_positive(dentry))
		drop_nlink(d_inode(dentry));
	return ret;
}

static int SUMH_NOCFI sumh_vnode_dir_rmdir(struct inode *dir,
					   struct dentry *dentry)
{
	struct sumh_vnode_info *info = dir->i_private;
	struct inode *src_dir = NULL;
	struct dentry *child;
	int ret;

	if (!info)
		return -ENOTDIR;
	if (!info->source.dentry)
		return -EROFS;
	if (!sumh_vfs_rmdir)
		return -EOPNOTSUPP;
	child = sumh_vnode_src_begin(info, dentry, &src_dir);
	if (IS_ERR(child))
		return PTR_ERR(child);
	if (d_really_is_negative(child))
		ret = -ENOENT;
	else
		ret = sumh_vfs_rmdir(KVN_SRC_IDMAP(info->source.mnt) src_dir,
				     child);
	sumh_vnode_src_end(info, src_dir, child);
	if (ret == 0 && d_really_is_positive(dentry)) {
		clear_nlink(d_inode(dentry)); /* removed dir: no more links */
		drop_nlink(dir); /* its ".." backref is gone */
	}
	return ret;
}

static int SUMH_NOCFI sumh_vnode_dir_link(struct dentry *old_dentry,
					  struct inode *dir,
					  struct dentry *dentry)
{
	struct sumh_vnode_info *info = dir->i_private;
	struct inode *old_vi = d_inode(old_dentry);
	struct sumh_vnode_info *old_info =
	    (old_vi && sumh_vnode_is_ours(old_vi)) ? old_vi->i_private : NULL;
	struct inode *src_dir = NULL;
	struct dentry *child;
	int ret;

	if (!info)
		return -ENOTDIR;
	if (!info->source.dentry)
		return -EROFS;
	if (!sumh_vfs_link)
		return -EOPNOTSUPP;
	/* The hardlink target is a visible dentry; it must resolve to a real
	 * source (its own redirect's pinned source).  A pure-virtual or foreign
	 * inode has no backing store to link — report cross-device. */
	if (!old_info || !old_info->source.dentry)
		return -EXDEV;
	child = sumh_vnode_src_begin(info, dentry, &src_dir);
	if (IS_ERR(child))
		return PTR_ERR(child);
	ret =
	    sumh_vfs_link(old_info->source.dentry,
			  KVN_SRC_IDMAP(info->source.mnt) src_dir, child, NULL);
	if (ret == 0)
		ret = sumh_vnode_publish_child(dir->i_sb, &info->source, child,
					       dentry);
	sumh_vnode_src_end(info, src_dir, child);
	return ret;
}

/* ---- directory vnode: rename delegates to the pinned source dir ----------
 *
 * Rename is the one mutation that touches two directories at once.  Both the
 * old and new visible dirs are vnodes on the *real* superblock, so their source
 * dirs share that superblock too.  That yields the key simplification for
 * locking: whenever the two source dirs differ, the outer VFS is performing a
 * cross-directory *visible* rename and already holds this sb's
 * s_vfs_rename_mutex; whenever the outer rename is same-directory, the two
 * source dirs are identical (same vnode -> same i_private -> same source).  So
 * the source-side parent locking mirrors lock_rename() but never re-takes
 * s_vfs_rename_mutex.  Mutation sink (Final Phase 1c).
 */

/* d_ancestor() is not exported; reimplement it.  Safe here because the caller
 * runs under the outer rename's s_vfs_rename_mutex (held whenever the two dirs
 * differ), which freezes directory ancestry against concurrent renames.
 * Returns the child of @p1 on the path to @p2 when @p1 is an ancestor of @p2.
 */
static struct dentry *sumh_d_ancestor(struct dentry *p1, struct dentry *p2)
{
	struct dentry *p;

	for (p = p2; !IS_ROOT(p); p = p->d_parent) {
		if (p->d_parent == p1)
			return p;
	}
	return NULL;
}

/*
 * Lock two source parent dirs for a delegated rename, mirroring lock_rename()
 * minus s_vfs_rename_mutex (see the block comment).  @p1 is the new parent, @p2
 * the old, matching lock_rename(new, old).  Returns the subtree trap (common
 * ancestor) or NULL so the caller can reject renaming a directory into its own
 * descendant.
 */
static struct dentry *sumh_lock_src_rename(struct dentry *p1, struct dentry *p2)
{
	struct dentry *p;

	if (p1 == p2) {
		inode_lock_nested(d_inode(p1), I_MUTEX_PARENT);
		return NULL;
	}
	p = sumh_d_ancestor(p2, p1);
	if (p) {
		inode_lock_nested(d_inode(p2), I_MUTEX_PARENT);
		inode_lock_nested(d_inode(p1), I_MUTEX_CHILD);
		return p;
	}
	p = sumh_d_ancestor(p1, p2);
	if (p) {
		inode_lock_nested(d_inode(p1), I_MUTEX_PARENT);
		inode_lock_nested(d_inode(p2), I_MUTEX_CHILD);
		return p;
	}
	inode_lock_nested(d_inode(p1), I_MUTEX_PARENT);
	inode_lock_nested(d_inode(p2), I_MUTEX_PARENT2);
	return NULL;
}

static void sumh_unlock_src_rename(struct dentry *p1, struct dentry *p2)
{
	inode_unlock(d_inode(p1));
	if (p1 != p2)
		inode_unlock(d_inode(p2));
}

/* Issue the delegated rename on the source dentries, building the version-
 * appropriate call shape. Older kernels carry both source-mount idmaps;
 * 6.18 requires a single mount/idmap and parent dentries. */
static int SUMH_NOCFI sumh_vnode_do_src_rename(
    struct sumh_vnode_info *oinfo, struct sumh_vnode_info *ninfo,
    struct inode *sodir, struct dentry *src_old, struct inode *sndir,
    struct dentry *src_new, unsigned int flags)
{
	struct renamedata rd = {};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
	/* 6.18 collapsed renamedata to one mount idmap and parent dentries,
	 * matching renameat2's same-mount requirement. */
	if (oinfo->source.mnt != ninfo->source.mnt)
		return -EXDEV;
	rd.mnt_idmap = mnt_idmap(oinfo->source.mnt);
	rd.old_parent = oinfo->source.dentry;
	rd.old_dentry = src_old;
	rd.new_parent = ninfo->source.dentry;
	rd.new_dentry = src_new;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	rd.old_mnt_idmap = mnt_idmap(oinfo->source.mnt);
	rd.new_mnt_idmap = mnt_idmap(ninfo->source.mnt);
	rd.old_dir = sodir;
	rd.old_dentry = src_old;
	rd.new_dir = sndir;
	rd.new_dentry = src_new;
#else
	rd.old_mnt_userns = mnt_user_ns(oinfo->source.mnt);
	rd.new_mnt_userns = mnt_user_ns(ninfo->source.mnt);
	rd.old_dir = sodir;
	rd.old_dentry = src_old;
	rd.new_dir = sndir;
	rd.new_dentry = src_new;
#endif
	rd.delegated_inode = NULL;
	rd.flags = flags;
	return sumh_vfs_rename(&rd);
}

static int SUMH_NOCFI sumh_vnode_dir_rename(KVN_IDMAP_ARG struct inode *old_dir,
					    struct dentry *old_dentry,
					    struct inode *new_dir,
					    struct dentry *new_dentry,
					    unsigned int flags)
{
	struct sumh_vnode_info *oinfo = old_dir->i_private;
	struct sumh_vnode_info *ninfo = new_dir->i_private;
	struct dentry *src_odir, *src_ndir, *src_old = NULL, *src_new = NULL;
	struct dentry *trap;
	struct inode *sodir, *sndir;
	bool two_mnt;
	int ret;

	if (!oinfo || !ninfo)
		return -ENOTDIR;
	/* Rename needs real backing on both ends; a source-less virtual
	 * container cannot be a rename source or target — report cross-device
	 * so userspace falls back to copy + unlink. */
	if (!oinfo->source.dentry || !ninfo->source.dentry)
		return -EXDEV;
	if (!sumh_vfs_rename || !sumh_lookup_one_len)
		return -EOPNOTSUPP;

	/* Sleeping op on our static i_op table: hold the module-refcount gate.
	 */
	if (!try_module_get(THIS_MODULE))
		return -ENOENT;

	src_odir = oinfo->source.dentry;
	src_ndir = ninfo->source.dentry;
	sodir = d_inode(src_odir);
	sndir = d_inode(src_ndir);
	if (!sodir || !sndir || !S_ISDIR(sodir->i_mode) ||
	    !S_ISDIR(sndir->i_mode)) {
		module_put(THIS_MODULE);
		return -ENOTDIR;
	}

	/* Source-mount write protection on both ends (deduped when identical).
	 */
	two_mnt = oinfo->source.mnt != ninfo->source.mnt;
	ret = sumh_vnode_src_want_write(&oinfo->source);
	if (ret) {
		module_put(THIS_MODULE);
		return ret;
	}
	if (two_mnt) {
		ret = sumh_vnode_src_want_write(&ninfo->source);
		if (ret) {
			sumh_vnode_src_drop_write(&oinfo->source);
			module_put(THIS_MODULE);
			return ret;
		}
	}

	trap = sumh_lock_src_rename(src_ndir, src_odir);

	src_old = sumh_lookup_one_len(old_dentry->d_name.name, src_odir,
				      old_dentry->d_name.len);
	if (IS_ERR(src_old)) {
		ret = PTR_ERR(src_old);
		src_old = NULL;
		goto unlock;
	}
	src_new = sumh_lookup_one_len(new_dentry->d_name.name, src_ndir,
				      new_dentry->d_name.len);
	if (IS_ERR(src_new)) {
		ret = PTR_ERR(src_new);
		src_new = NULL;
		goto unlock;
	}

	if (d_really_is_negative(src_old)) {
		ret = -ENOENT; /* source vanished out-of-band */
		goto unlock;
	}
	/* Subtree-loop guards, as do_renameat2 applies against the trap. */
	if (src_old == trap) {
		ret = -EINVAL; /* old is an ancestor of new */
		goto unlock;
	}
	if (src_new == trap) {
		ret = -ENOTEMPTY; /* new is an ancestor of old */
		goto unlock;
	}

	ret = sumh_vnode_do_src_rename(oinfo, ninfo, sodir, src_old, sndir,
				       src_new, flags);
	/* On success the source dcache is fixed up inside the delegated
	 * vfs_rename (d_move/d_exchange on the source dentries); the visible
	 * dentries are moved by the outer vfs_rename after we return, so the
	 * vnodes (which pin the now-renamed source dentries) stay consistent.
	 */

unlock:
	sumh_unlock_src_rename(src_ndir, src_odir);
	dput(src_old);
	dput(src_new);
	if (two_mnt)
		sumh_vnode_src_drop_write(&ninfo->source);
	sumh_vnode_src_drop_write(&oinfo->source);
	module_put(THIS_MODULE);
	return ret;
}

static const struct inode_operations sumh_vnode_dir_iops = {
    .lookup = sumh_vnode_dir_lookup,
    .getattr = sumh_vnode_getattr,
    .setattr = sumh_vnode_setattr,
    .listxattr = sumh_vnode_listxattr,
    .create = sumh_vnode_dir_create,
    .mkdir = sumh_vnode_dir_mkdir,
    .mknod = sumh_vnode_dir_mknod,
    .symlink = sumh_vnode_dir_symlink,
    .unlink = sumh_vnode_dir_unlink,
    .rmdir = sumh_vnode_dir_rmdir,
    .link = sumh_vnode_dir_link,
    .rename = sumh_vnode_dir_rename,
};

static const struct file_operations sumh_vnode_dir_fops = {
    .owner = THIS_MODULE,
    .open = sumh_vnode_dir_open,
    .release = sumh_vnode_release,
    .read = generic_read_dir,
    .iterate_shared = sumh_vnode_dir_iterate,
    .llseek = generic_file_llseek,
};

/* Clone the source inode's SELinux context onto the vnode's in-core SID, so
 * getxattr(security.selinux) and AVC checks on the vnode report the same label
 * as the source — the last view-consistency axis for a synthetic inode.  Uses
 * the public LSM secctx round-trip (getsecctx on source -> notifysecctx on
 * vnode); no SELinux blob offsets or struct layout assumptions, so it stays
 * stable across KMIs.  Best-effort: on any failure or missing symbol the vnode
 * keeps its default label rather than failing creation. */
static SUMH_NOCFI void sumh_vnode_clone_sid(struct inode *vnode,
					    struct inode *src)
{
	if (!vnode || !src || !sumh_security_inode_getsecctx ||
	    !sumh_security_inode_notifysecctx || !sumh_security_release_secctx)
		return;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
	{
		struct lsm_context ctx = {};
		int ret = sumh_security_inode_getsecctx(src, &ctx);

		if (!ret && ctx.context && ctx.len)
			(void)sumh_security_inode_notifysecctx(
			    vnode, ctx.context, ctx.len);
		if (ctx.context)
			sumh_security_release_secctx(&ctx);
	}
#else
	{
		void *ctx = NULL;
		u32 ctxlen = 0;
		int ret = sumh_security_inode_getsecctx(src, &ctx, &ctxlen);

		if (!ret && ctx && ctxlen)
			(void)sumh_security_inode_notifysecctx(vnode, ctx,
							       ctxlen);
		if (ctx)
			sumh_security_release_secctx(ctx, ctxlen);
	}
#endif
}

struct inode *sumh_vnode_new(struct super_block *sb, const struct path *source,
			     unsigned long v_ino, umode_t mode, u8 flags)
{
	struct inode *inode;
	struct inode *r_inode = NULL;
	struct sumh_vnode_info *info;

	if (!sb)
		return NULL;
	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return NULL;
	info->v_ino = v_ino;
	info->flags = flags;
	if (source && source->dentry && source->mnt) {
		info->source = *source;
		path_get(&info->source);
		r_inode = d_inode(source->dentry);
	}
	if (!sumh_sop_vnode_get(sb)) {
		if (info->source.dentry)
			path_put(&info->source);
		kfree(info);
		return NULL;
	}
	inode = new_inode(sb);
	if (!inode) {
		sumh_sop_vnode_put(sb);
		if (info->source.dentry)
			path_put(&info->source);
		kfree(info);
		return NULL;
	}
	lockdep_set_class(&inode->i_rwsem, &sumh_vnode_i_mutex_key);
	info->sop_vnode_ref = true;
	/* Publish the owner marker before any post-new_inode failure/reclaim
	 * path can drop this inode.  The sop vnode count is released only by
	 * the reclaim trampoline once i_private has been detached. */
	inode->i_private = info;
	inode->i_ino = v_ino;
	inode->i_uid = r_inode ? r_inode->i_uid : GLOBAL_ROOT_UID;
	inode->i_gid = r_inode ? r_inode->i_gid : GLOBAL_ROOT_GID;

	if (r_inode && (flags & SUMH_VNODE_F_SPECIAL) &&
	    (S_ISCHR(r_inode->i_mode) || S_ISBLK(r_inode->i_mode) ||
	     S_ISFIFO(r_inode->i_mode))) {
		/* char/blk/fifo source wrapper: mint S_IFREG so path_openat's
		 * may_open() does not apply may_open_dev() on the nodev visible
		 * mount; the special fops' .open delegates to the real source
		 * via dentry_open (which bypasses may_open) and the data plane
		 * forwards there.  Remember the real type+rdev so getattr still
		 * presents a device/fifo.  No shared page cache: the device
		 * owns its mapping. */
		info->special_mode = r_inode->i_mode;
		info->special_rdev = r_inode->i_rdev;
		inode->i_mode = S_IFREG | (r_inode->i_mode & 07777);
		inode->i_op = &sumh_vnode_file_iops;
		inode->i_fop = &sumh_vnode_special_fops;
	} else {
		inode->i_mode = r_inode ? r_inode->i_mode : mode;
		if (r_inode) {
			inode->i_size = i_size_read(r_inode);
			/* Share the source page cache so the generic mmap/read
			 * path serves source content without a private
			 * snapshot.  Directories don't mmap and iterate through
			 * the pinned source, so keep their own mapping and
			 * carry the source link count. */
			if (!S_ISDIR(r_inode->i_mode))
				inode->i_mapping = r_inode->i_mapping;
			else
				set_nlink(inode, r_inode->i_nlink);
		}

		if ((flags & SUMH_VNODE_F_DIR) ||
		    (r_inode && S_ISDIR(r_inode->i_mode))) {
			inode->i_op = &sumh_vnode_dir_iops;
			inode->i_fop = &sumh_vnode_dir_fops;
		} else {
			inode->i_op = &sumh_vnode_file_iops;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
			inode->i_fop =
			    (r_inode && !S_ISLNK(r_inode->i_mode) &&
			     r_inode->i_fop && r_inode->i_fop->mmap_prepare)
				? &sumh_vnode_file_fops_mmap_prepare
				: &sumh_vnode_file_fops;
#else
			inode->i_fop = &sumh_vnode_file_fops;
#endif
		}
	}
	/* Deliberately NOT S_PRIVATE: the vnode must stay LSM-visible so
	 * SELinux serves security.selinux from the in-core SID we clone below
	 * and applies AVC using the source's label.  S_PRIVATE would make
	 * security_inode_getsecurity() short-circuit to -EOPNOTSUPP, leaking an
	 * empty/ENODATA label through the fs xattr fallback. */
	inode->i_flags |= S_NOATIME | S_NOCMTIME | S_NOSEC;
	inode->i_opflags |= IOP_XATTR;
	if (!S_ISLNK(inode->i_mode))
		inode->i_opflags |= IOP_NOFOLLOW;

	/* Project the source's SELinux label onto the vnode (view-consistency:
	 * security.selinux must match the source).  Done last, after i_security
	 * is allocated by new_inode() and all identity is in place. */
	if ((flags & SUMH_VNODE_F_SU) && ksu_sucompat_vfs_setup_inode(inode)) {
		iput(inode);
		return NULL;
	}
	if (r_inode)
		sumh_vnode_clone_sid(inode, r_inode);

	/* Stash the source's file capabilities so an exec of a redirected
	 * setcap binary keeps them: the exec-path get_vfs_caps_from_disk read
	 * lands on this synthetic inode, which has no on-disk
	 * security.capability.  Only meaningful for executable regular files;
	 * the read is sleepable and safe here (lookup context).
	 * Failure/-ENODATA simply leaves has_caps false. */
	if (r_inode && S_ISREG(r_inode->i_mode) && (r_inode->i_mode & 0111) &&
	    sumh_get_vfs_caps_from_disk &&
	    sumh_source_vfs_caps(&info->source, &info->caps) == 0)
		info->has_caps = true;
	return inode;
}

struct inode *sumh_vnode_new_virtual(struct super_block *sb,
				     const char *visible_path,
				     unsigned long v_ino,
				     struct inode *label_donor)
{
	struct inode *inode;
	struct sumh_vnode_info *info;

	if (!sb || !visible_path)
		return NULL;
	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return NULL;
	info->visible_path = kstrdup(visible_path, GFP_KERNEL);
	if (!info->visible_path) {
		kfree(info);
		return NULL;
	}
	info->v_ino = v_ino;
	info->flags = SUMH_VNODE_F_DIR | SUMH_VNODE_F_VIRTUAL_DIR;
	if (!sumh_sop_vnode_get(sb)) {
		kfree(info->visible_path);
		kfree(info);
		return NULL;
	}
	inode = new_inode(sb);
	if (!inode) {
		sumh_sop_vnode_put(sb);
		kfree(info->visible_path);
		kfree(info);
		return NULL;
	}
	lockdep_set_class(&inode->i_rwsem, &sumh_vnode_i_mutex_key);
	info->sop_vnode_ref = true;

	inode->i_private = info;
	inode->i_ino = v_ino;
	/* A synthesized container directory: world traversable/listable, owned
	 * by root, no data source.  lookup/iterate resolve descendants from the
	 * rule table; getattr fills from this inode with the projected ino/dev.
	 * Mode is kept permissive (0555) rather than cloned from the donor so a
	 * restrictive ancestor cannot make the injected subtree unreachable to
	 * the view app. */
	inode->i_mode = S_IFDIR | 0555;
	inode->i_uid = GLOBAL_ROOT_UID;
	inode->i_gid = GLOBAL_ROOT_GID;
	set_nlink(inode, 2);
	inode->i_op = &sumh_vnode_dir_iops;
	inode->i_fop = &sumh_vnode_dir_fops;
	inode->i_flags |= S_NOATIME | S_NOCMTIME | S_NOSEC;
	inode->i_opflags |= IOP_NOFOLLOW;
	/* Project the deepest real ancestor's SELinux label onto the
	 * synthesized directory so getxattr(security.selinux)/ls -Z report a
	 * plausible context instead of the pseudo default.  The donor is on the
	 * visible path the app already traversed, so its label is
	 * app-searchable and cloning it does not make the virtual dir
	 * unsearchable under AVC.  Best-effort. */
	if (label_donor)
		sumh_vnode_clone_sid(inode, label_donor);
	return inode;
}
