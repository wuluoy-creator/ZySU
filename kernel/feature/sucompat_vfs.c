#include <linux/compat.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/path.h>
#include <linux/security.h>
#include <linux/string.h>
#include <linux/version.h>

#include "feature/sucompat_exec.h"
#include "feature/sucompat_vfs.h"
#include "sumh_bootstrap.h"
#include "sumh_dirhijack.h"
#include "sumh_vnode.h"
#include "ksu.h"
#include "policy/allowlist.h"
#include "selinux/selinux.h"

static DEFINE_MUTEX(su_view_lock);
static bool su_ready;
static bool su_enabled;
static char su_path[KSU_SU_PATH_MAX] = KSU_SU_PATH_DEFAULT;
static struct sumh_dh_claim *su_binding;
static unsigned long su_ino;
static unsigned long su_next_ino = (1UL << 63) | 0x53550000UL;
static const struct inode_operations su_inode_ops;

bool ksu_sucompat_vfs_enabled(void)
{
	return sumh_is_ready() && READ_ONCE(su_enabled);
}
bool ksu_sucompat_vfs_active(void)
{
	return ksu_sucompat_vfs_enabled();
}

bool ksu_sucompat_vfs_current_ino(unsigned long ino)
{
	return ksu_sucompat_vfs_enabled() && ino && ino == READ_ONCE(su_ino);
}

bool ksu_sucompat_vfs_visible(void)
{
#ifdef CONFIG_COMPAT
	if (is_compat_task())
		return false;
#endif
	return ksu_sucompat_vfs_enabled() &&
	       ksu_is_allow_uid_for_current(current_uid().val);
}

bool ksu_sucompat_vfs_is_inode(const struct inode *inode)
{
	return inode && inode->i_op == &su_inode_ops;
}

bool ksu_sucompat_vfs_is_path(const struct path *path)
{
	return path && path->dentry &&
	       ksu_sucompat_vfs_is_inode(d_inode(path->dentry));
}

bool ksu_sucompat_vfs_is_file(const struct file *file)
{
	return file && ksu_sucompat_vfs_is_inode(file_inode(file));
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int su_permission(struct mnt_idmap *idmap, struct inode *inode, int mask)
#else
static int su_permission(struct user_namespace *userns, struct inode *inode,
			 int mask)
#endif
{
	if (!ksu_sucompat_vfs_visible() ||
	    !ksu_sucompat_vfs_current_ino(inode->i_ino))
		return -EACCES;
	return mask & MAY_WRITE ? -EROFS : 0;
}

static int su_open(struct inode *inode, struct file *file)
{
	return ksu_sucompat_exec_file_open(file);
}

static const struct inode_operations su_inode_ops = {
    .permission = su_permission,
};
static const struct file_operations su_file_ops = {
    .owner = THIS_MODULE,
    .open = su_open,
    .release = ksu_sucompat_exec_file_release,
};

int ksu_sucompat_vfs_setup_inode(struct inode *inode)
{
	int ret;

	inode->i_op = &su_inode_ops;
	inode->i_fop = &su_file_ops;
	inode->i_mode = S_IFREG | 0555;
	inode_lock(inode);
	ret = security_inode_notifysecctx(inode, (void *)KSU_FILE_CONTEXT,
					  sizeof(KSU_FILE_CONTEXT) - 1);
	inode_unlock(inode);
	return ret;
}

static void su_release_binding_locked(void)
{
	struct sumh_dh_claim *old = su_binding;
	struct sumh_dh_retire retired;

	su_binding = NULL;
	WRITE_ONCE(su_ino, 0);
	if (!old)
		return;
	sumh_dh_retire_init(&retired, false);
	sumh_dh_detach(old, &retired);
	sumh_dh_retire_finish(&retired);
	sumh_dh_claim_put(old);
}

static int su_bind_locked(const char *path)
{
	const struct cred *old;
	struct sumh_dh_claim *candidate = NULL, *previous;
	struct sumh_dh_spec spec = {.kind = SUMH_DH_OWNER_SU,
				    .flags = SUMH_VNODE_F_SU};
	struct sumh_dh_change change;
	struct sumh_dh_retire retired;
	int ret;

	lockdep_assert_held(&su_view_lock);
	if (!su_ready || !sumh_is_ready())
		return -ESHUTDOWN;
	if (!ksu_cred)
		return -EAGAIN;
	old = override_creds(ksu_cred);
	if (su_enabled && !strcmp(path, su_path)) {
		ret = sumh_dirhijack_match_su(path, su_binding,
					      READ_ONCE(su_ino));
		if (ret) {
			if (ret > 0)
				ret = 0;
			goto out;
		}
	}
	if (su_next_ino == ULONG_MAX) {
		ret = -EOVERFLOW;
		goto out;
	}
	spec.v_ino = ++su_next_ino;
	ret = sumh_dh_prepare_path(path, &spec, &candidate, NULL);
	if (ret)
		goto out;
	/* CLEAR can invalidate this reservation without taking su_view_lock. */
	if (!su_ready || !sumh_is_ready()) {
		ret = -ESHUTDOWN;
		goto abort;
	}
	previous = su_binding;
	ret = sumh_dh_change_begin(candidate, previous, &change);
	if (ret)
		goto abort;
	sumh_dh_retire_init(&retired, false);
	sumh_dh_change_commit(&change, &retired);
	WRITE_ONCE(su_ino, spec.v_ino);
	su_binding = candidate;
	sumh_dh_change_end();
	sumh_dh_retire_finish(&retired);
	sumh_dh_claim_put(previous);
	goto out;
abort:
	sumh_dh_abort(candidate);
out:
	revert_creds(old);
	return ret;
}

static int su_validate_path(const char *path)
{
	const char *component;
	size_t len = strnlen(path, KSU_SU_PATH_MAX);
	size_t i;

	if (len == KSU_SU_PATH_MAX)
		return -ENAMETOOLONG;
	if (len < 2 || path[0] != '/')
		return -EINVAL;
	component = path + 1;
	for (i = 1; i <= len; i++) {
		size_t size;

		if (path[i] && (unsigned char)path[i] < 0x20)
			return -EINVAL;
		if (path[i] && path[i] != '/')
			continue;
		size = path + i - component;
		if (!size || (size == 1 && component[0] == '.') ||
		    (size == 2 && component[0] == '.' && component[1] == '.'))
			return -EINVAL;
		if (size > NAME_MAX)
			return -ENAMETOOLONG;
		component = path + i + 1;
	}
	return 0;
}

int ksu_sucompat_vfs_get_config(struct ksu_su_path_config *config)
{
	memset(config, 0, sizeof(*config));
	config->version = KSU_SU_PATH_VERSION;
	config->size = sizeof(*config);
	mutex_lock(&su_view_lock);
	config->flags = ksu_sucompat_vfs_enabled() ? KSU_SU_PATH_ENABLED : 0;
	strscpy(config->path, su_path, sizeof(config->path));
	mutex_unlock(&su_view_lock);
	return 0;
}

bool ksu_sucompat_vfs_reserved_path(const char *path)
{
	bool reserved;

	mutex_lock(&su_view_lock);
	reserved = !strcmp(path, su_path);
	mutex_unlock(&su_view_lock);
	return reserved;
}

int ksu_sucompat_vfs_set_config(const struct ksu_su_path_config *config)
{
	int ret;

	if (config->version != KSU_SU_PATH_VERSION ||
	    config->size != sizeof(*config) || config->flags ||
	    config->reserved)
		return -EINVAL;
	ret = su_validate_path(config->path);
	if (ret)
		return ret;
	mutex_lock(&su_view_lock);
	if (su_ready && sumh_is_ready()) {
		/* Also retry a deferred or failed initial binding, even when
		 * the configured path has not changed. */
		ret = su_bind_locked(config->path);
		if (ret)
			goto out;
		WRITE_ONCE(su_enabled, true);
	} else if (strcmp(su_path, config->path) && su_binding) {
		su_release_binding_locked();
	}
	strscpy(su_path, config->path, sizeof(su_path));
out:
	mutex_unlock(&su_view_lock);
	return ret;
}

int ksu_sucompat_vfs_refresh(void)
{
	int ret = 0;

	mutex_lock(&su_view_lock);
	if (!su_ready || !sumh_is_ready()) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	/* Bind automatically once credentials and /system are available. */
	ret = su_bind_locked(su_path);
	if (!ret)
		WRITE_ONCE(su_enabled, true);
out:
	mutex_unlock(&su_view_lock);
	return ret;
}

int ksu_sucompat_vfs_init(void)
{
	WRITE_ONCE(su_ready, true);
	return 0;
}

void ksu_sucompat_vfs_exit(void)
{
	mutex_lock(&su_view_lock);
	WRITE_ONCE(su_enabled, false);
	WRITE_ONCE(su_ready, false);
	su_release_binding_locked();
	mutex_unlock(&su_view_lock);
}
