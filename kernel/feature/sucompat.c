#include <asm/current.h>
#include <asm/unistd.h>
#include <linux/compiler_types.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/namei.h>
#include <linux/pgtable.h>
#include <linux/printk.h>
#include <linux/ptrace.h>
#include <linux/sched/task_stack.h>
#include <linux/syscalls.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include "policy/allowlist.h"
#include "policy/app_profile.h"
#include "arch.h"
#include "util.h"
#include "policy/feature.h"
#include "hook/syscall_hook.h"
#include "klog.h" // IWYU pragma: keep
#include "ksu.h"
#include "runtime/ksud.h"
#include "sulog/event.h"
#include "supercall/supercall.h"
#include "feature/sucompat.h"
#include "feature/sucompat_exec.h"
#include "feature/sucompat_vfs.h"

#define SU_PATH "/system/bin/su"

/* SUMH is the only sucompat provider; the legacy route stays disabled. */
const bool ksu_su_compat_enabled = false;
static const char su_path[] = SU_PATH;
static bool sumh_sucompat_feature_registered;
static bool sumh_sucompat_started;

#ifndef fd_file
#define fd_file(fd) ((fd).file)
#endif

static int su_compat_feature_get(u64 *value)
{
	*value = READ_ONCE(ksu_su_compat_enabled) ? 1 : 0;
	return 0;
}

static int su_compat_feature_set(u64 value)
{
	if (value > 1)
		return -EINVAL;
	return value ? -EPERM : 0;
}

static const struct ksu_feature_handler su_compat_handler = {
    .feature_id = KSU_FEATURE_SU_COMPAT,
    .name = "su_compat",
    .get_handler = su_compat_feature_get,
    .set_handler = su_compat_feature_set,
};

static int sumh_sucompat_feature_get(u64 *value)
{
	*value = ksu_sucompat_vfs_enabled() ? 1 : 0;
	return 0;
}

static int sumh_sucompat_feature_set(u64 value)
{
	if (value > 1)
		return -EINVAL;
	if (!value)
		return -EPERM;
	/* Preserve the post-mount rebind request without allowing mode changes.
	 */
	return ksu_sucompat_vfs_refresh();
}

static const struct ksu_feature_handler sumh_sucompat_handler = {
    .feature_id = KSU_FEATURE_SUMH_SUCOMPAT,
    .name = "sumh_sucompat",
    .get_handler = sumh_sucompat_feature_get,
    .set_handler = sumh_sucompat_feature_set,
};

bool ksu_sucompat_exec_enabled(void)
{
	return READ_ONCE(ksu_su_compat_enabled) && !ksu_sucompat_vfs_active();
}

static char __user *ksud_user_path(void)
{
	static const char ksud_path[] = KSUD_PATH;

	return userspace_stack_buffer(ksud_path, sizeof(ksud_path));
}

static char __user *empty_user_path(void)
{
	static const char empty_path[] = "";

	return userspace_stack_buffer(empty_path, sizeof(empty_path));
}

static bool is_su_path(const char __user *filename_user)
{
	char path[sizeof(su_path) + 1];
	const char __user *fn;
	long ret;
	unsigned long addr;

	if (unlikely(!filename_user))
		return false;

	addr = untagged_addr((unsigned long)filename_user);
	fn = (const char __user *)addr;
	memset(path, 0, sizeof(path));
	ret = strncpy_from_user(path, fn, sizeof(path));
	if (ret < 0)
		return false;
	path[sizeof(path) - 1] = '\0';

	return !memcmp(path, su_path, sizeof(su_path));
}

static int resolve_exec_path(const char __user *filename_user, bool execveat,
			     const struct pt_regs *regs, struct path *path)
{
	const char __user *fn;
	unsigned int lookup_flags = LOOKUP_FOLLOW;
	unsigned long addr;
	struct fd fd;
	char first;
	int dfd = AT_FDCWD;
	int flags = 0;

	if (!filename_user || !path)
		return -EFAULT;
	if (execveat) {
		dfd = (int)PT_REGS_SYSCALL_PARM1(regs);
		flags = (int)PT_REGS_PARM5(regs);
		if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
			return -EINVAL;
		if (flags & AT_SYMLINK_NOFOLLOW)
			lookup_flags = 0;
	}

	addr = untagged_addr((unsigned long)filename_user);
	fn = (const char __user *)addr;
	if (get_user(first, fn))
		return -EFAULT;
	if (first != '\0')
		return user_path_at(dfd, fn, lookup_flags, path);
	if (!execveat || !(flags & AT_EMPTY_PATH))
		return -ENOENT;

	fd = fdget_raw(dfd);
	if (!fd_file(fd))
		return -EBADF;
	*path = fd_file(fd)->f_path;
	path_get(path);
	fdput(fd);
	return 0;
}

static bool is_vfs_su_exec(const char __user *filename_user, bool execveat,
			   const struct pt_regs *regs)
{
	struct path path;
	bool match;

	if (resolve_exec_path(filename_user, execveat, regs, &path))
		return false;
	match = ksu_sucompat_vfs_is_path(&path);
	path_put(&path);
	return match;
}

static bool is_ksud_visible(void)
{
	struct path path;

	if (kern_path(KSUD_PATH, 0, &path))
		return false;
	path_put(&path);
	return true;
}

static long ksu_handle_path_sucompat(int orig_nr, const struct pt_regs *regs,
				     const char *operation)
{
	const char __user *filename_user;
	const struct cred *old_cred;
	struct pt_regs redirect_regs;
	char __user *redirect_path;
	long ret;

	if (!READ_ONCE(ksu_su_compat_enabled))
		goto do_orig;

	if (!ksu_is_allow_uid_for_current(current_uid().val))
		goto do_orig;

	filename_user = (const char __user *)PT_REGS_PARM2(regs);
	if (!is_su_path(filename_user))
		goto do_orig;

	if (unlikely(!ksu_cred)) {
		pr_err("%s: KernelSU credential is unavailable\n", operation);
		goto do_orig;
	}

	old_cred = override_creds(ksu_cred);
	if (!is_ksud_visible())
		goto revert_orig;

	redirect_path = ksud_user_path();
	if (!redirect_path)
		goto revert_orig;

	pr_info("%s su->ksud\n", operation);
	redirect_regs = *regs;
	PT_REGS_PARM2(&redirect_regs) = (unsigned long)redirect_path;
	ret = ksu_syscall_table[orig_nr](&redirect_regs);
	revert_creds(old_cred);
	return ret;

revert_orig:
	revert_creds(old_cred);
do_orig:
	return ksu_syscall_table[orig_nr](regs);
}

long ksu_handle_faccessat_sucompat(int orig_nr, const struct pt_regs *regs)
{
	return ksu_handle_path_sucompat(orig_nr, regs, "faccessat");
}

long ksu_handle_stat_sucompat(int orig_nr, const struct pt_regs *regs)
{
	return ksu_handle_path_sucompat(orig_nr, regs, "newfstatat");
}

static void close_tmp_fd(unsigned int fd)
{
	close_fd(fd);
}

static long
ksu_handle_execve_sucompat_common(const char __user **filename_user,
				  const char __user *const __user *argv_user,
				  unsigned long envp, bool execveat,
				  int orig_nr, const struct pt_regs *regs)
{
	const struct cred *old_cred;
	struct ksu_sulog_pending_event *pending_sucompat = NULL;
	struct pt_regs exec_regs;
	struct file *ksud_file;
	char __user *empty_path;
	int su_fd;
	int tmp_fd;
	long ret;
	bool vfs_match = false;
	bool allowed;

	if (unlikely(!filename_user || !*filename_user))
		goto do_orig_execve;

	allowed = ksu_is_allow_uid_for_current(current_uid().val);

	if (ksu_sucompat_vfs_enabled()) {
		vfs_match = is_vfs_su_exec(*filename_user, execveat, regs);
		if (vfs_match && !ksu_sucompat_vfs_enabled())
			vfs_match = false;
	}
	if (vfs_match && !allowed)
		return -EACCES;
	if (!vfs_match) {
		if (!allowed)
			goto do_orig_execve;
		if (!READ_ONCE(ksu_su_compat_enabled))
			goto do_orig_execve;
		if (execveat && ((int)PT_REGS_SYSCALL_PARM1(regs) != AT_FDCWD ||
				 (int)PT_REGS_PARM5(regs) != 0))
			goto do_orig_execve;
		if (!is_su_path(*filename_user))
			goto do_orig_execve;
	}

	pr_info("exec su found (%s)\n", vfs_match ? "vnode" : "classic");
	if (unlikely(!ksu_cred)) {
		pr_err("exec su: KernelSU credential is unavailable\n");
		goto do_orig_execve;
	}
	if (!ksu_is_allow_uid_for_current(current_uid().val) ||
	    (vfs_match ? !ksu_sucompat_vfs_enabled()
		       : !READ_ONCE(ksu_su_compat_enabled)))
		return -EACCES;

	tmp_fd = get_unused_fd_flags(O_CLOEXEC);
	if (tmp_fd < 0) {
		pr_err("alloc ksud fd failed: %d\n", tmp_fd);
		goto do_orig_execve;
	}

	old_cred = override_creds(ksu_cred);
	ksud_file = filp_open(KSUD_PATH, O_PATH, 0);
	revert_creds(old_cred);
	if (IS_ERR(ksud_file)) {
		pr_info("ksud is not visible for sucompat: %ld\n",
			PTR_ERR(ksud_file));
		put_unused_fd(tmp_fd);
		goto do_orig_execve;
	}
	fd_install(tmp_fd, ksud_file);

	empty_path = empty_user_path();
	if (!empty_path) {
		close_tmp_fd(tmp_fd);
		goto do_orig_execve;
	}

	pending_sucompat =
	    ksu_sulog_capture_sucompat(*filename_user, argv_user, GFP_KERNEL);
	exec_regs = *regs;
	PT_REGS_SYSCALL_PARM1(&exec_regs) = tmp_fd;
	PT_REGS_PARM2(&exec_regs) = (unsigned long)empty_path;
	PT_REGS_PARM3(&exec_regs) = (unsigned long)argv_user;
	PT_REGS_SYSCALL_PARM4(&exec_regs) = envp;
	PT_REGS_PARM5(&exec_regs) = AT_EMPTY_PATH;

	/* Policy and feature state can change while the executable is opened.
	 */
	if (!ksu_is_allow_uid_for_current(current_uid().val) ||
	    (vfs_match ? !ksu_sucompat_vfs_enabled()
		       : !READ_ONCE(ksu_su_compat_enabled))) {
		ret = -EACCES;
		ksu_sulog_emit_pending(pending_sucompat, ret, GFP_KERNEL);
		close_tmp_fd(tmp_fd);
		return ret;
	}

	ret = escape_with_root_profile();
	if (ret) {
		pr_err("escape_with_root_profile failed: %ld\n", ret);
		ksu_sulog_emit_pending(pending_sucompat, ret, GFP_KERNEL);
		close_tmp_fd(tmp_fd);
		return ret;
	}

	ret = ksu_syscall_table[__NR_execveat](&exec_regs);
	if (ret < 0) {
		pr_err("failed to exec ksud as su: %ld\n", ret);
		close_tmp_fd(tmp_fd);
	} else {
		su_fd = ksu_install_su_fd();
		if (su_fd < 0)
			pr_warn("install su session fd failed: %d\n", su_fd);
	}
	ksu_sulog_emit_pending(pending_sucompat, ret, GFP_KERNEL);
	return ret;

do_orig_execve:
	return ksu_syscall_table[orig_nr](regs);
}

long ksu_handle_execve_sucompat(const char __user **filename_user, int orig_nr,
				const struct pt_regs *regs)
{
	return ksu_handle_execve_sucompat_common(
	    filename_user,
	    (const char __user *const __user *)PT_REGS_PARM2(regs),
	    PT_REGS_PARM3(regs), false, orig_nr, regs);
}

long ksu_handle_execveat_sucompat(const char __user **filename_user,
				  int orig_nr, const struct pt_regs *regs)
{
	return ksu_handle_execve_sucompat_common(
	    filename_user,
	    (const char __user *const __user *)PT_REGS_PARM3(regs),
	    PT_REGS_SYSCALL_PARM4(regs), true, orig_nr, regs);
}

// sucompat: permitted process can execute 'su' to gain root access.
void ksu_sucompat_init(void)
{
	if (ksu_register_feature_handler(&su_compat_handler))
		pr_err("Failed to register su_compat feature handler\n");
	if (ksu_register_feature_handler(&sumh_sucompat_handler)) {
		pr_err("sumh: sucompat: failed to register feature handler\n");
	} else {
		sumh_sucompat_feature_registered = true;
	}
}

int ksu_sucompat_sumh_init(void)
{
	int ret = ksu_sucompat_vfs_init();

	if (ret)
		return ret;
	ret = ksu_sucompat_exec_init();
	if (ret) {
		ksu_sucompat_vfs_exit();
		return ret;
	}
	sumh_sucompat_started = true;
	return 0;
}

void ksu_sucompat_sumh_exit(void)
{
	if (sumh_sucompat_started) {
		ksu_sucompat_exec_exit();
		ksu_sucompat_vfs_exit();
		sumh_sucompat_started = false;
	}
}

void ksu_sucompat_exit(void)
{
	ksu_sucompat_sumh_exit();
	if (sumh_sucompat_feature_registered) {
		ksu_unregister_feature_handler(KSU_FEATURE_SUMH_SUCOMPAT);
		sumh_sucompat_feature_registered = false;
	}
	ksu_unregister_feature_handler(KSU_FEATURE_SU_COMPAT);
}
