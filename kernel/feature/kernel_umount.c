#include "infra/mount_policy.h"
#include <linux/cred.h>
#include <linux/compiler.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/nsproxy.h>
#include <linux/path.h>
#include <linux/printk.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sort.h>
#include <linux/syscalls.h>
#include <linux/task_work.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <uapi/linux/mount.h>

#include "policy/allowlist.h"
#include "policy/feature.h"
#include "feature/kernel_umount.h"
#include "klog.h" // IWYU pragma: keep
#include "ksu.h"
#include "util.h"
#include "sumh_proc_read_hooks.h"
#include "sumh_path_policy.h"
#include "runtime/ksud_boot.h"
#include "runtime/ksud.h"
#include "selinux/selinux.h"

static bool ksu_webview_zygote_umount_enabled;
static bool ksu_unshare_mnt_enabled;
static bool ksu_unshare_mnt_hooks_held;

bool ksu_is_webview_zygote_umount_enabled(void)
{
	return READ_ONCE(ksu_webview_zygote_umount_enabled);
}

static int webview_zygote_umount_feature_get(u64 *value)
{
	*value = ksu_is_webview_zygote_umount_enabled() ? 1 : 0;
	return 0;
}

static int webview_zygote_umount_feature_set(u64 value)
{
	bool enable = value != 0;

	WRITE_ONCE(ksu_webview_zygote_umount_enabled, enable);
	pr_info("webview_zygote_umount: set to %d\n", enable);
	return 0;
}

static const struct ksu_feature_handler webview_zygote_umount_handler = {
    .feature_id = KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT,
    .name = "webview_zygote_umount",
    .get_handler = webview_zygote_umount_feature_get,
    .set_handler = webview_zygote_umount_feature_set,
};

bool ksu_is_unshare_mnt_enabled(void)
{
	return READ_ONCE(ksu_unshare_mnt_enabled);
}

static int unshare_mnt_feature_get(u64 *value)
{
	*value = READ_ONCE(ksu_unshare_mnt_enabled) ? 1 : 0;
	return 0;
}

static int unshare_mnt_feature_set(u64 value)
{
	bool enable = value != 0;
	int ret;

	if (enable && !ksu_unshare_mnt_hooks_held) {
		ret = sumh_proc_proxy_get();
		if (ret)
			return ret;
		ksu_unshare_mnt_hooks_held = true;
	}

	WRITE_ONCE(ksu_unshare_mnt_enabled, enable);
	pr_info("unshare_mnt: set to %d\n", enable);
	return 0;
}

static const struct ksu_feature_handler unshare_mnt_handler = {
    .feature_id = KSU_FEATURE_UNSHARE_MNT,
    .name = "unshare_mnt",
    .get_handler = unshare_mnt_feature_get,
    .set_handler = unshare_mnt_feature_set,
};

extern int path_umount(struct path *path, int flags);

static int ksu_umount_mnt(struct path *path, int flags)
{
	int err = path_umount(path, flags);
	if (err) {
		pr_info("umount %s failed: %d\n", path->dentry->d_iname, err);
	}
	return err;
}

/* ── robust per-app module umount ──────────────────────────────────────────
 * The zygote-child path reads the parent zygote's mountinfo and detaches
 * matching inherited mounts from the child's current namespace. This avoids
 * instantiating the child's own /proc/<pid>/mountinfo inode during specialize.
 */

#define KSU_UMOUNT_MAX_TARGETS 512
#define KSU_MOUNTINFO_BUF (256 * 1024)

struct ksu_umount_target {
	char *path;
	char *root;
	char *fstype;
	dev_t dev;
	unsigned int order;
};

/* Parse one mountinfo line in place (strsep NUL-terminates the fields):
 *   id parent maj:min ROOT TARGET opts [optional…] - fstype SOURCE super */
static bool ksu_parse_mountinfo(char *line, char **dev, char **root,
				char **target, char **fstype, char **source,
				char **super)
{
	char *tok, *f2 = NULL, *f3 = NULL, *f4 = NULL;
	int n = 0;

	while ((tok = strsep(&line, " ")) != NULL) {
		if (n == 2)
			f2 = tok;
		else if (n == 3)
			f3 = tok;
		else if (n == 4)
			f4 = tok;
		else if (n >= 6 && !strcmp(tok, "-")) {
			char *fs = strsep(&line, " "); /* fstype */
			char *src = strsep(&line, " "); /* mount source */
			if (!fs || !src || !f2 || !f3 || !f4)
				return false;
			*dev = f2;
			*root = f3;
			*target = f4;
			*fstype = fs;
			*source = src;
			*super = strsep(&line,
					" "); /* super options (may be NULL) */
			return true;
		}
		n++;
	}
	return false;
}

static bool ksu_parse_mount_dev(const char *field, dev_t *dev)
{
	unsigned int major, minor;
	dev_t parsed;

	if (sscanf(field, "%u:%u", &major, &minor) != 2)
		return false;
	parsed = MKDEV(major, minor);
	if (MAJOR(parsed) != major || MINOR(parsed) != minor)
		return false;
	*dev = parsed;
	return true;
}

static bool ksu_fstype_matches(const char *actual, const char *expected)
{
	size_t len = strlen(actual);

	return !strcmp(actual, expected) ||
	       (!strncmp(actual, expected, len) && expected[len] == '.');
}

static bool ksu_try_umount_verified(const struct ksu_umount_target *target,
				    char *path_buf)
{
	struct path path;
	struct super_block *sb;
	char *root;
	int err;

	err = kern_path(target->path, 0, &path);
	if (err)
		return false;

	sb = path.mnt->mnt_sb;
	root = dentry_path_raw(path.mnt->mnt_root, path_buf, PATH_MAX);
	if (path.dentry != path.mnt->mnt_root || IS_ERR(root) || !sb ||
	    sb->s_dev != target->dev || !sb->s_type ||
	    !ksu_fstype_matches(sb->s_type->name, target->fstype) ||
	    strcmp(root, target->root)) {
		path_put(&path);
		return false;
	}

	return !ksu_umount_mnt(&path, MNT_DETACH);
}

static int ksu_umount_target_cmp(const void *lhs, const void *rhs)
{
	const struct ksu_umount_target *a = lhs;
	const struct ksu_umount_target *b = rhs;
	size_t a_len = strlen(a->path);
	size_t b_len = strlen(b->path);

	if (a_len != b_len)
		return a_len < b_len ? 1 : -1;
	if (a->order != b->order)
		return a->order < b->order ? 1 : -1;
	return 0;
}

/* Consume an opened mountinfo file, validate the complete snapshot, then
 * detach deeper paths before their parents. Parent-derived candidates are
 * matched against the child's live mount signature before detach. */
static int ksu_umount_scan_mountinfo(struct file *f, bool *signature_mismatch)
{
	struct ksu_umount_target *targets;
	char *buf, *path_buf = NULL, *p, *line;
	char *dev, *root, *target, *fstype, *source, *super;
	loff_t pos = 0;
	size_t total = 0;
	bool complete = false;
	int ret = 0;
	int nt = 0, i;

	if (signature_mismatch)
		*signature_mismatch = false;

	buf = vmalloc(KSU_MOUNTINFO_BUF);
	if (!buf) {
		filp_close(f, NULL);
		return -ENOMEM;
	}
	targets =
	    kmalloc_array(KSU_UMOUNT_MAX_TARGETS, sizeof(*targets), GFP_KERNEL);
	if (!targets) {
		filp_close(f, NULL);
		vfree(buf);
		return -ENOMEM;
	}

	while (total < KSU_MOUNTINFO_BUF - 1) {
		ssize_t n = kernel_read(f, buf + total,
					KSU_MOUNTINFO_BUF - 1 - total, &pos);
		if (n < 0) {
			ret = n;
			break;
		}
		if (!n) {
			complete = true;
			break;
		}
		total += n;
	}
	filp_close(f, NULL);
	if (ret)
		goto out;
	if (!complete) {
		ret = -E2BIG;
		goto out;
	}
	buf[total] = '\0';

	p = buf;
	while ((line = strsep(&p, "\n")) != NULL) {
		if (!*line)
			continue;
		if (!ksu_parse_mountinfo(line, &dev, &root, &target, &fstype,
					 &source, &super)) {
			ret = -EINVAL;
			goto out;
		}
		struct ksu_mount_fields fields = {
		    .dev = {dev, strlen(dev)},
		    .root = {root, strlen(root)},
		    .target = {target, strlen(target)},
		    .fstype = {fstype, strlen(fstype)},
		    .source = {source, strlen(source)},
		    .super = {super, super ? strlen(super) : 0},
		    .escaped = true,
		};
		bool module_mount = ksu_mount_is_module(&fields);
		if (!ksu_mount_unescape(root) || !ksu_mount_unescape(target) ||
		    !ksu_mount_unescape(source) ||
		    (super && !ksu_mount_unescape(super))) {
			ret = -EINVAL;
			goto out;
		}
		if (!module_mount)
			continue;
		if (nt == KSU_UMOUNT_MAX_TARGETS) {
			ret = -E2BIG;
			goto out;
		}
		if (!ksu_parse_mount_dev(dev, &targets[nt].dev)) {
			ret = -EINVAL;
			goto out;
		}
		targets[nt].path = target;
		targets[nt].root = root;
		targets[nt].fstype = fstype;
		targets[nt].order = nt;
		nt++;
	}

	if (nt) {
		path_buf = kmalloc(PATH_MAX, GFP_KERNEL);
		if (!path_buf) {
			ret = -ENOMEM;
			goto out;
		}
	}

	sort(targets, nt, sizeof(*targets), ksu_umount_target_cmp, NULL);
	for (i = 0; i < nt; i++) {
		pr_info("%s: detaching %s\n", __func__, targets[i].path);
		if (!ksu_try_umount_verified(&targets[i], path_buf)) {
			if (signature_mismatch)
				*signature_mismatch = true;
			pr_warn("%s: signature mismatch for %s\n", __func__,
				targets[i].path);
		}
	}
	ret = nt;

out:
	kfree(path_buf);
	kfree(targets);
	vfree(buf);
	return ret;
}

enum ksu_umount_scan_source {
	KSU_UMOUNT_SCAN_PARENT,
	KSU_UMOUNT_SCAN_NONE,
};

struct umount_tw {
	struct callback_head cb;
	uid_t uid;
	enum ksu_umount_scan_source source;
	struct file *mountinfo;
};

static void ksu_umount_mount_list(void)
{
	struct mount_entry *entry;
	struct ksu_umount_target *targets;
	char *path_buf;
	unsigned int count = 0, i;

	targets =
	    kmalloc_array(KSU_UMOUNT_MAX_TARGETS, sizeof(*targets), GFP_KERNEL);
	path_buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!targets || !path_buf)
		goto out;

	down_read(&mount_list_lock);
	list_for_each_entry (entry, &mount_list, list) {
		if (count == KSU_UMOUNT_MAX_TARGETS)
			goto unlock;
		if (!entry->mount_root || !entry->mount_fstype)
			continue;
		targets[count] = (struct ksu_umount_target){
		    .path = entry->umountable,
		    .root = entry->mount_root,
		    .fstype = entry->mount_fstype,
		    .dev = entry->mount_dev,
		    .order = count,
		};
		count++;
	}
	/* SUMHP's registered identity, not the path alone, owns the detach.
	 * Keep entries pinned while checking the child's live mounts. */
	sort(targets, count, sizeof(*targets), ksu_umount_target_cmp, NULL);
	for (i = 0; i < count; i++)
		ksu_try_umount_verified(&targets[i], path_buf);
unlock:
	up_read(&mount_list_lock);
out:
	kfree(path_buf);
	kfree(targets);
}

static void umount_tw_func(struct callback_head *cb)
{
	struct umount_tw *tw = container_of(cb, struct umount_tw, cb);
	const struct cred *saved;
	bool signature_mismatch = false;
	int scanned = -EINVAL;

	/* A profile may change after enqueue. Decide before override_creds(),
	 * using the real application UID rather than the privileged worker UID.
	 */
	if ((current->flags & PF_EXITING) ||
	    __kuid_val(task_uid(current)) != tw->uid ||
	    !sumh_policy_uid_should_umount(tw->uid))
		goto out;
	saved = override_creds(ksu_cred);

	if (tw->source == KSU_UMOUNT_SCAN_PARENT) {
		struct file *f = tw->mountinfo;

		tw->mountinfo = NULL;
		scanned = ksu_umount_scan_mountinfo(f, &signature_mismatch);
	}

	if (tw->source == KSU_UMOUNT_SCAN_NONE || scanned < 0 ||
	    (tw->source == KSU_UMOUNT_SCAN_PARENT && !scanned &&
	     !signature_mismatch))
		ksu_umount_mount_list();

	if (READ_ONCE(ksu_unshare_mnt_enabled) &&
	    !(current->flags & PF_EXITING) && current->fs && current->nsproxy) {
		/* Clone the remaining mounts only after all detach attempts. */
		int ret = ksyscall(unshare, CLONE_NEWNS);

		if (ret)
			pr_warn_ratelimited("unshare_mnt: pid %d failed: %d\n",
					    current->pid, ret);
	}

	revert_creds(saved);

out:
	if (tw->mountinfo)
		filp_close(tw->mountinfo, NULL);
	kfree(tw);
}

static bool ksu_parent_scan_context(struct task_struct *parent)
{
	const struct cred *cred;
	bool valid;

	rcu_read_lock();
	valid = pid_alive(parent) &&
		rcu_dereference(current->real_parent) == parent;
	rcu_read_unlock();
	if (!valid)
		return false;

	cred = get_task_cred(parent);
	valid = ksu_is_trusted_zygote(cred);
	put_cred(cred);
	if (!valid)
		return false;

	task_lock(parent);
	valid = parent->nsproxy && parent->nsproxy->mnt_ns &&
		current->nsproxy && current->nsproxy->mnt_ns &&
		current->nsproxy->mnt_ns != parent->nsproxy->mnt_ns;
	task_unlock(parent);
	return valid;
}

static struct file *ksu_open_parent_mountinfo(bool *fallback_safe)
{
	struct task_struct *parent;
	const struct cred *saved;
	struct file *f;
	char path[48];
	pid_t pid;

	*fallback_safe = false;
	rcu_read_lock();
	parent = rcu_dereference(current->real_parent);
	if (parent)
		get_task_struct(parent);
	rcu_read_unlock();
	if (!parent)
		return ERR_PTR(-ESRCH);

	if (!ksu_parent_scan_context(parent)) {
		f = ERR_PTR(-EPERM);
		goto out;
	}

	pid = task_tgid_vnr(parent);
	if (pid <= 0) {
		f = ERR_PTR(-ESRCH);
		goto out;
	}

	*fallback_safe = true;
	scnprintf(path, sizeof(path), "/proc/%d/mountinfo", pid);
	saved = override_creds(ksu_cred);
	f = filp_open(path, O_RDONLY, 0);
	revert_creds(saved);
	if (IS_ERR(f))
		goto out;

	if (!ksu_parent_scan_context(parent)) {
		filp_close(f, NULL);
		f = ERR_PTR(-ESRCH);
		*fallback_safe = false;
	}

out:
	put_task_struct(parent);
	return f;
}

int ksu_handle_umount(uid_t old_uid, uid_t new_uid)
{
	struct file *mountinfo;
	struct umount_tw *tw;
	bool fallback_safe;

	// if there isn't any module mounted, just ignore it!
	if (!ksu_module_mounted) {
		return 0;
	}

	if (!ksu_cred) {
		return 0;
	}

	/* Authorization, SUMH content routing and physical detach share one
	 * decision, including both Android isolated UID ranges and WebView. */
	if (!sumh_policy_uid_should_umount(new_uid)) {
		return 0;
	}

	/* Specialization still runs in the trusted factory's SELinux domain.
	 * The parent check below separately rejects a shared mount namespace.
	 */
	bool is_zygote_child = ksu_is_trusted_zygote(current_cred());
	if (!is_zygote_child) {
		pr_info("handle umount ignore non zygote child: %d\n",
			current->pid);
		return 0;
	}
	// umount the target mnt
	pr_info("handle umount for uid: %d, pid: %d\n", new_uid, current->pid);

	mountinfo = ksu_open_parent_mountinfo(&fallback_safe);
	if (IS_ERR(mountinfo) && !fallback_safe) {
		pr_warn("handle umount rejected unsafe parent mountinfo: %ld\n",
			PTR_ERR(mountinfo));
		return 0;
	}

	tw = kzalloc(sizeof(*tw), GFP_KERNEL);
	if (!tw)
		goto close_mountinfo;

	tw->cb.func = umount_tw_func;
	tw->uid = new_uid;
	if (IS_ERR(mountinfo)) {
		tw->source = KSU_UMOUNT_SCAN_NONE;
		tw->mountinfo = NULL;
	} else {
		tw->source = KSU_UMOUNT_SCAN_PARENT;
		tw->mountinfo = mountinfo;
	}

	int err = task_work_add(current, &tw->cb, TWA_RESUME);
	if (!err)
		return 0;

	if (tw->mountinfo)
		filp_close(tw->mountinfo, NULL);
	kfree(tw);
	pr_warn("unmount add task_work failed: %d\n", err);
	return 0;

close_mountinfo:
	if (!IS_ERR(mountinfo))
		filp_close(mountinfo, NULL);
	return 0;
}

void ksu_kernel_umount_init(void)
{
	if (ksu_register_feature_handler(&webview_zygote_umount_handler)) {
		pr_err("Failed to register webview_zygote_umount feature "
		       "handler\n");
	}
	if (ksu_register_feature_handler(&unshare_mnt_handler)) {
		pr_err("Failed to register unshare_mnt feature handler\n");
	}
}

void ksu_kernel_umount_exit(void)
{
	ksu_unregister_feature_handler(KSU_FEATURE_UNSHARE_MNT);
	WRITE_ONCE(ksu_unshare_mnt_enabled, false);
	if (ksu_unshare_mnt_hooks_held) {
		sumh_proc_proxy_put();
		ksu_unshare_mnt_hooks_held = false;
	}
	ksu_unregister_feature_handler(KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT);
}
