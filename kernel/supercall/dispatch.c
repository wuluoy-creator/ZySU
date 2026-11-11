#include "sumh_entrypoints.h"
#include "sumh_path_policy.h"
#include <asm/unistd.h>
#include <linux/anon_inodes.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/kallsyms.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/ratelimit.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/seccomp.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "policy/allowlist.h"
#include "arch.h"
#include "policy/feature.h"
#include "feature/selinux_hide.h"
#include "feature/sucompat.h"
#include "feature/sucompat_vfs.h"
#include "infra/file_wrapper.h"
#include "infra/symbol_resolver.h"
#include "preinit/preinit.h"
#include "klog.h" // IWYU pragma: keep
#include "ksu.h"
#include "manager/apk_sign.h"
#include "runtime/ksud_boot.h"
#include "runtime/ksud.h"
#include "manager/manager_identity.h"
#include "infra/seccomp_cache.h"
#include "selinux/selinux.h"
#include "sulog/event.h"
#include "sulog/fd.h"
#include "supercall/supercall.h"
#include "supercall/internal.h"
#include "hook/syscall_hook_manager.h"

#ifdef CONFIG_KSU_SUPERKEY
#include "manager/superkey.h"
#endif // #ifdef CONFIG_KSU_SUPERKEY

static int do_grant_root(void __user *arg)
{
	int ret;
	__u32 audit_uid = current_uid().val;
	__u32 audit_euid = current_euid().val;

	// we already check uid above on allowed_for_su()

	pr_info("allow root for: %d\n", audit_uid);
	ret = escape_with_root_profile();
	ksu_sulog_emit_grant_root(ret, audit_uid, audit_euid, GFP_KERNEL);

	return ret;
}

static int do_get_info(void __user *arg)
{
	struct ksu_get_info_cmd cmd = {.version = KERNEL_SU_VERSION,
				       .flags = 0};

	cmd.flags |= KSU_GET_INFO_FLAG_LKM;
	if (ksu_bundled)
		cmd.flags |= KSU_GET_INFO_FLAG_BUNDLED;
	if (is_manager()) {
		cmd.flags |= KSU_GET_INFO_FLAG_MANAGER;
	}
	if (ksu_late_loaded) {
		cmd.flags |= KSU_GET_INFO_FLAG_LATE_LOAD;
	}
	if (ksu_preinit_enabled_param() && !ksu_late_loaded)
		cmd.flags |= KSU_GET_INFO_FLAG_PREINIT;
	cmd.features = KSU_FEATURE_MAX;

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_version: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_get_load_mode(void __user *arg)
{
	struct ksu_get_load_mode_cmd cmd = {.mode = KSU_LOAD_MODE_RAMDISK,
					    .flags = 0};

	if (ksu_late_loaded)
		cmd.mode = KSU_LOAD_MODE_LATE;
	else if (ksu_imgpatch_loaded)
		cmd.mode = KSU_LOAD_MODE_IMAGE_PATCH;

	if (copy_to_user(arg, &cmd, sizeof(cmd)))
		return -EFAULT;
	return 0;
}

static int do_report_event(void __user *arg)
{
	static bool services_started = false;
	struct ksu_report_event_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	switch (cmd.event) {
	case EVENT_POST_FS_DATA: {
		static bool post_fs_data_lock = false;
		if (!post_fs_data_lock) {
			post_fs_data_lock = true;
			if (ksu_late_loaded) {
				pr_info("post-fs-data skipped (late load)\n");
			} else {
				pr_info("post-fs-data triggered\n");
				on_post_fs_data();
			}
		}
		break;
	}
	case EVENT_BOOT_COMPLETED: {
		static bool boot_complete_lock = false;
		if (!boot_complete_lock) {
			boot_complete_lock = true;
			if (ksu_late_loaded) {
				pr_info("boot_complete skipped (late load)\n");
				ksu_selinux_hide_drop_backup_if_unused();
			} else {
				pr_info("boot_complete triggered\n");
				on_boot_completed();
			}
		}
		break;
	}
	case EVENT_MODULE_MOUNTED: {
		pr_info("module mounted!\n");
		on_module_mounted();
		break;
	}
	case EVENT_SERVICES: {
		if (services_started) {
			pr_info("services already started, skipping\n");
			return 0;
		}
		services_started = true;
		pr_info("services triggered\n");
		return 1;
	}
	default:
		break;
	}

	return 0;
}

static int do_set_sepolicy(void __user *arg)
{
	struct ksu_set_sepolicy_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	return handle_sepolicy((void __user *)cmd.data, cmd.data_len);
}

static int do_check_safemode(void __user *arg)
{
	struct ksu_check_safemode_cmd cmd;

	cmd.in_safe_mode = ksu_is_safe_mode();

	if (cmd.in_safe_mode) {
		pr_warn("safemode enabled!\n");
	}

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("check_safemode: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_get_allow_list_common(void __user *arg, bool allow)
{
	int *arr = NULL;
	int err = 0;
	u16 count;
	u32 out_count;
	static const u16 kSize = 128;

	arr = kmalloc(sizeof(int) * kSize, GFP_KERNEL);
	if (!arr)
		return -ENOMEM;

	if (!ksu_get_allow_list(arr, kSize, &count, NULL, allow)) {
		err = -EFAULT;
		goto out;
	}

	out_count = count;

	if (copy_to_user(arg + offsetof(struct ksu_get_allow_list_cmd, count),
			 &out_count, sizeof(u32))) {
		pr_err("get_allow_list: copy_to_user count failed\n");
		err = -EFAULT;
		goto out;
	}

	if (copy_to_user(arg, arr, sizeof(u32) * count)) {
		pr_err("get_allow_list: copy_to_user uids failed\n");
		err = -EFAULT;
	}

out:
	kfree(arr);
	return err;
}

static int do_get_allow_list(void __user *arg)
{
	return do_get_allow_list_common(arg, true);
}

static int do_get_deny_list(void __user *arg)
{
	return do_get_allow_list_common(arg, false);
}

static int do_new_get_allow_list_common(void __user *arg, bool allow)
{
	struct ksu_new_get_allow_list_cmd cmd;
	int *arr = NULL;
	int err = 0;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	if (cmd.count) {
		arr = kmalloc(sizeof(int) * cmd.count, GFP_KERNEL);
		if (!arr)
			return -ENOMEM;
	}

	if (!ksu_get_allow_list(arr, cmd.count, &cmd.count, &cmd.total_count,
				allow)) {
		err = -EFAULT;
		goto out;
	}

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("new_get_allow_list: copy_to_user count failed\n");
		err = -EFAULT;
		goto out;
	}

	if (cmd.count && arr &&
	    copy_to_user(
		((struct ksu_new_get_allow_list_cmd __user *)arg)->uids, arr,
		sizeof(int) * cmd.count)) {
		pr_err("new_get_allow_list: copy_to_user uids failed\n");
		err = -EFAULT;
	}

out:
	kfree(arr);
	return err;
}

static int do_new_get_allow_list(void __user *arg)
{
	return do_new_get_allow_list_common(arg, true);
}

static int do_new_get_deny_list(void __user *arg)
{
	return do_new_get_allow_list_common(arg, false);
}

static int do_uid_granted_root(void __user *arg)
{
	struct ksu_uid_granted_root_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	cmd.granted = ksu_is_uid_granted_root(cmd.uid);

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("uid_granted_root: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_uid_should_umount(void __user *arg)
{
	struct ksu_uid_should_umount_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	cmd.should_umount = sumh_policy_uid_should_umount(cmd.uid);

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("uid_should_umount: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_get_manager_appid(void __user *arg)
{
	struct ksu_get_manager_appid_cmd cmd;

	cmd.appid = ksu_get_manager_appid();

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_manager_appid: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_get_manager_uid(void __user *arg)
{
	struct ksu_get_manager_uid_cmd cmd;

	cmd.uid = ksu_get_manager_uid();

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_manager_uid: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_get_app_profile(void __user *arg)
{
#ifdef CONFIG_KSU_DISABLE_POLICY
	return -EOPNOTSUPP;
#else
	uid_t uid;
	struct app_profile *profile;
	int ret = 0;

	if (copy_from_user(
		&uid,
		(char __user *)arg +
		    offsetof(struct ksu_get_app_profile_cmd, profile.curr_uid),
		sizeof(uid))) {
		pr_err("get_app_profile: copy_from_user failed\n");
		return -EFAULT;
	}

	profile = ksu_get_app_profile(uid);
	if (!profile) {
		ret = -ENOENT;
	} else {
		if (copy_to_user(
			(char __user *)arg +
			    offsetof(struct ksu_get_app_profile_cmd, profile),
			profile, sizeof(*profile))) {
			pr_err("get_app_profile: copy_to_user failed\n");
			ret = -EFAULT;
		}
		ksu_put_app_profile(profile);
	}

	return ret;
#endif // #ifdef CONFIG_KSU_DISABLE_POLICY
}

static int do_set_app_profile(void __user *arg)
{
#ifdef CONFIG_KSU_DISABLE_POLICY
	return -EOPNOTSUPP;
#else
	struct ksu_set_app_profile_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("set_app_profile: copy_from_user failed\n");
		return -EFAULT;
	}

	if (!ksu_set_app_profile(&cmd.profile, true)) {
		return -EFAULT;
	}

	return 0;
#endif // #ifdef CONFIG_KSU_DISABLE_POLICY
}

static int do_get_sulog_fd(void __user *arg)
{
	struct ksu_get_sulog_fd_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("get_sulog_fd: copy_from_user failed\n");
		return -EFAULT;
	}

	if (cmd.flags) {
		pr_err("get_sulog_fd: unsupported flags 0x%x\n", cmd.flags);
		return -EINVAL;
	}

	return ksu_install_sulog_fd();
}

static int do_set_init_pgrp(void __user *arg)
{
	int err;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	struct pid *pids[PIDTYPE_MAX] = {0};
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
	struct task_struct *p = current->group_leader;
	struct pid *init_group = task_pgrp(&init_task);

	write_lock_irq(&tasklist_lock);
	err = -EPERM;
	if (task_session(p) != task_session(&init_task))
		goto out;

	err = 0;
	if (task_pgrp(p) != init_group) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
		change_pid(pids, p, PIDTYPE_PGID, init_group);
#else
		change_pid(p, PIDTYPE_PGID, init_group);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
	}

out:
	write_unlock_irq(&tasklist_lock);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	free_pids(pids);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
	return err;
}

static int do_get_feature(void __user *arg)
{
	struct ksu_get_feature_cmd cmd;
	bool supported = false;
	int ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("get_feature: copy_from_user failed\n");
		return -EFAULT;
	}

	ret = ksu_get_feature(cmd.feature_id, &cmd.value, &supported);
	cmd.supported = supported ? KSU_FEATURE_FLAG_SUPPORTED : 0;
	if (supported && ksu_feature_is_forced(cmd.feature_id))
		cmd.supported |= KSU_FEATURE_FLAG_FORCED;

	if (ret && supported) {
		pr_err("get_feature: failed for feature %u: %d\n",
		       cmd.feature_id, ret);
		return ret;
	}

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_feature: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_set_feature(void __user *arg)
{
	struct ksu_set_feature_cmd cmd;
	int ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("set_feature: copy_from_user failed\n");
		return -EFAULT;
	}

	ret = ksu_set_feature(cmd.feature_id, cmd.value);
	if (ret) {
		pr_err("set_feature: failed for feature %u: %d\n",
		       cmd.feature_id, ret);
		return ret;
	}

	return 0;
}

static int do_get_su_path(void __user *arg)
{
	struct ksu_su_path_config *config;
	int ret;

	config = kzalloc(sizeof(*config), GFP_KERNEL);
	if (!config)
		return -ENOMEM;
	ret = ksu_sucompat_vfs_get_config(config);
	if (!ret && copy_to_user(arg, config, sizeof(*config)))
		ret = -EFAULT;
	kfree(config);
	return ret;
}

static int do_set_su_path(void __user *arg)
{
	struct ksu_su_path_config *config;
	int ret;

	config = memdup_user(arg, sizeof(*config));
	if (IS_ERR(config))
		return PTR_ERR(config);
	ret = ksu_sucompat_vfs_set_config(config);
	kfree(config);
	return ret;
}

static int do_get_wrapper_fd(void __user *arg)
{
	if (!ksu_file_sid) {
		return -EINVAL;
	}

	struct ksu_get_wrapper_fd_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("get_wrapper_fd: copy_from_user failed\n");
		return -EFAULT;
	}

	return ksu_install_file_wrapper(cmd.fd);
}

static int do_manage_mark(void __user *arg)
{
	struct ksu_manage_mark_cmd cmd;
	int ret = 0;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("manage_mark: copy_from_user failed\n");
		return -EFAULT;
	}

	switch (cmd.operation) {
	case KSU_MARK_GET: {
		// Get task mark status
		ret = ksu_get_task_mark(cmd.pid);
		if (ret < 0) {
			pr_err("manage_mark: get failed for pid %d: %d\n",
			       cmd.pid, ret);
			return ret;
		}
		cmd.result = (u32)ret;
		break;
	}
	case KSU_MARK_MARK: {
		if (cmd.pid == 0) {
			ksu_mark_all_process();
		} else {
			ret = ksu_set_task_mark(cmd.pid, true);
			if (ret < 0) {
				pr_err("manage_mark: set_mark failed for pid "
				       "%d: %d\n",
				       cmd.pid, ret);
				return ret;
			}
		}
		break;
	}
	case KSU_MARK_UNMARK: {
		if (cmd.pid == 0) {
			ksu_unmark_all_process();
		} else {
			ret = ksu_set_task_mark(cmd.pid, false);
			if (ret < 0) {
				pr_err("manage_mark: set_unmark failed for pid "
				       "%d: %d\n",
				       cmd.pid, ret);
				return ret;
			}
		}
		break;
	}
	case KSU_MARK_REFRESH: {
		ksu_mark_running_process();
		pr_info("manage_mark: refreshed running processes\n");
		break;
	}
	default: {
		pr_err("manage_mark: invalid operation %u\n", cmd.operation);
		return -EINVAL;
	}
	}
	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("manage_mark: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_nuke_ext4_sysfs(void __user *arg)
{
	struct ksu_nuke_ext4_sysfs_cmd cmd;
	char mnt[256];
	long ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;

	if (!cmd.arg)
		return -EINVAL;

	memset(mnt, 0, sizeof(mnt));

	/*
	 * Hardening: validate user pointer before copying.
	 * This is adapted from upstream KernelSU's nuke_ext4_sysfs safety
	 * fixes to avoid panics on bad userspace pointers.
	 */
	if (!access_ok((const void __user *)(uintptr_t)cmd.arg, sizeof(mnt))) {
		pr_err("nuke ext4: invalid userspace pointer\n");
		return -EFAULT;
	}

	ret = strncpy_from_user(mnt, (const char __user *)(uintptr_t)cmd.arg,
				sizeof(mnt));
	if (ret < 0) {
		pr_err("nuke ext4 copy mnt failed: %ld\\n", ret);
		return -EFAULT;
	}

	if (ret == sizeof(mnt)) {
		pr_err("nuke ext4 mnt path too long\\n");
		return -ENAMETOOLONG;
	}

	pr_info("do_nuke_ext4_sysfs: %s\n", mnt);

	return nuke_ext4_sysfs(mnt);
}

// 100. GET_FULL_VERSION - Get full version string
static int do_get_full_version(void __user *arg)
{
	struct ksu_get_full_version_cmd cmd = {0};
	const char *version_full = KSU_VERSION_FULL;

	strscpy(cmd.version_full, version_full, sizeof(cmd.version_full));

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_full_version: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

// 101. HOOK_TYPE - Get hook type
static int do_get_hook_type(void __user *arg)
{
	struct ksu_hook_type_cmd cmd = {0};
	const char *type = "TSR Hook";

	strscpy(cmd.hook_type, type, sizeof(cmd.hook_type));

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_hook_type: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

// 107. SUPERKEY_AUTH - Authenticate with superkey (APatch-style)
#ifdef CONFIG_KSU_SUPERKEY

static int do_superkey_auth(void __user *arg)
{
	struct ksu_superkey_auth_cmd cmd;
	int ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("superkey_auth: copy_from_user failed\n");
		return -EFAULT;
	}

	cmd.superkey[sizeof(cmd.superkey) - 1] = '\0';

	ret = superkey_authenticate(cmd.superkey);
	cmd.result = ret;

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("superkey_auth: copy_to_user failed\n");
		return -EFAULT;
	}

	return ret;
}

// 108. SUPERKEY_STATUS - Get superkey status
static int do_superkey_status(void __user *arg)
{
	struct ksu_superkey_status_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.enabled = superkey_is_set();
	cmd.authenticated = superkey_is_manager();
	/*
	 * signature_ok: whether manager APK's signature verification
	 * has passed. In SuperKey-only mode (signature bypass), this
	 * will remain 0.
	 */
	cmd.signature_ok = is_manager_apk(NULL) ? 1 : 0;
	cmd.manager_uid = superkey_get_manager_uid();

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("superkey_status: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}
#endif // #ifdef CONFIG_KSU_SUPERKEY

// Lock the calling (already-root) thread and its children out of any further
// KernelSU escalation. Backs `su --ksu-no-new-privs`.
static int do_disable_escape_to_root(void __user *arg)
{
	set_thread_flag(TIF_KSU_DISABLE_ESCAPE_WITH_ROOT);
	return 0;
}

// Report the UAPI contract version via its own ioctl, keeping GET_INFO's number
// and struct stable across kernel/userspace version skew.
static int do_get_uapi_version(void __user *arg)
{
	__u32 v = KERNEL_SU_UAPI_VERSION;

	if (copy_to_user(arg, &v, sizeof(v))) {
		pr_err("get_uapi_version: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

/*
 * Resolve symbols of the running kernel image, exported or not, matching
 * them like ksuinit's /proc/kallsyms walk. Userspace uses this to bind a
 * module's undefined symbols before init_module, so the kernel never parses
 * the module ELF itself.
 */
static int do_lookup_symbols(void __user *arg)
{
	struct ksu_lookup_symbols_cmd cmd;
	const u64 __user *unames;
	u64 __user *uaddrs;
	char **names;
	unsigned long *addrs;
	int ret = 0;
	u32 i;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;

	if (cmd.flags) {
		pr_err_ratelimited("lookup_symbols: unsupported flags 0x%x\n",
				   cmd.flags);
		return -EINVAL;
	}
	if (!cmd.count || cmd.count > KSU_LOOKUP_SYMBOLS_MAX)
		return -EINVAL;

	names = kvcalloc(cmd.count, sizeof(*names), GFP_KERNEL);
	addrs = kvcalloc(cmd.count, sizeof(*addrs), GFP_KERNEL);
	if (!names || !addrs) {
		ret = -ENOMEM;
		goto out;
	}

	unames = (const u64 __user *)(uintptr_t)cmd.names;
	uaddrs = (u64 __user *)(uintptr_t)cmd.addrs;
	for (i = 0; i < cmd.count; i++) {
		u64 uname;
		char *name;

		if (get_user(uname, unames + i)) {
			ret = -EFAULT;
			goto out;
		}
		name = strndup_user((const char __user *)(uintptr_t)uname,
				    KSYM_NAME_LEN);
		if (IS_ERR(name)) {
			// An overlong name cannot be a kernel symbol: report it
			// missing.
			if (PTR_ERR(name) == -EINVAL)
				continue;
			ret = PTR_ERR(name);
			goto out;
		}
		if (!*name) {
			kfree(name);
			continue;
		}
		names[i] = name;
	}

	ret = ksu_lookup_kernel_symbols((const char *const *)names, addrs,
					cmd.count);
	if (ret < 0)
		goto out;

	for (i = 0; i < cmd.count; i++) {
		if (put_user((u64)addrs[i], uaddrs + i)) {
			ret = -EFAULT;
			goto out;
		}
	}

out:
	if (names) {
		for (i = 0; i < cmd.count; i++)
			kfree(names[i]);
	}
	kvfree(names);
	kvfree(addrs);
	return ret;
}

static const struct ksu_ioctl_cmd_map ksu_ioctl_handlers[] = {
    {.cmd = KSU_IOCTL_LOOKUP_SYMBOLS,
     .name = "LOOKUP_SYMBOLS",
     .handler = do_lookup_symbols,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_GRANT_ROOT,
     .name = "GRANT_ROOT",
     .handler = do_grant_root,
     .perm_check = allowed_for_su},
    {.cmd = KSU_IOCTL_GET_INFO,
     .name = "GET_INFO",
     .handler = do_get_info,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_GET_LOAD_MODE,
     .name = "GET_LOAD_MODE",
     .handler = do_get_load_mode,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_REPORT_EVENT,
     .name = "REPORT_EVENT",
     .handler = do_report_event,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_SET_SEPOLICY,
     .name = "SET_SEPOLICY",
     .handler = do_set_sepolicy,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_CHECK_SAFEMODE,
     .name = "CHECK_SAFEMODE",
     .handler = do_check_safemode,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_GET_ALLOW_LIST,
     .name = "GET_ALLOW_LIST",
     .handler = do_get_allow_list,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_DENY_LIST,
     .name = "GET_DENY_LIST",
     .handler = do_get_deny_list,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_NEW_GET_ALLOW_LIST,
     .name = "NEW_GET_ALLOW_LIST",
     .handler = do_new_get_allow_list,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_NEW_GET_DENY_LIST,
     .name = "NEW_GET_DENY_LIST",
     .handler = do_new_get_deny_list,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_UID_GRANTED_ROOT,
     .name = "UID_GRANTED_ROOT",
     .handler = do_uid_granted_root,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_UID_SHOULD_UMOUNT,
     .name = "UID_SHOULD_UMOUNT",
     .handler = do_uid_should_umount,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_MANAGER_APPID,
     .name = "GET_MANAGER_APPID",
     .handler = do_get_manager_appid,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_MANAGER_UID,
     .name = "GET_MANAGER_UID",
     .handler = do_get_manager_uid,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_APP_PROFILE,
     .name = "GET_APP_PROFILE",
     .handler = do_get_app_profile,
     .perm_check = only_manager},
    {.cmd = KSU_IOCTL_SET_APP_PROFILE,
     .name = "SET_APP_PROFILE",
     .handler = do_set_app_profile,
     .perm_check = only_manager},
    {.cmd = KSU_IOCTL_GET_FEATURE,
     .name = "GET_FEATURE",
     .handler = do_get_feature,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_SET_FEATURE,
     .name = "SET_FEATURE",
     .handler = do_set_feature,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_WRAPPER_FD,
     .name = "GET_WRAPPER_FD",
     .handler = do_get_wrapper_fd,
     .perm_check = manager_or_root,
     .allow_su_session = true},
    {.cmd = KSU_IOCTL_MANAGE_MARK,
     .name = "MANAGE_MARK",
     .handler = do_manage_mark,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_NUKE_EXT4_SYSFS,
     .name = "NUKE_EXT4_SYSFS",
     .handler = do_nuke_ext4_sysfs,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_SET_INIT_PGRP,
     .name = "SET_INIT_PGRP",
     .handler = do_set_init_pgrp,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_GET_SULOG_FD,
     .name = "GET_SULOG_FD",
     .handler = do_get_sulog_fd,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_DISABLE_ESCAPE_TO_ROOT,
     .name = "DISABLE_ESCAPE_TO_ROOT",
     .handler = do_disable_escape_to_root,
     .perm_check = only_root,
     .allow_su_session = true},
    {.cmd = KSU_IOCTL_GET_UAPI_VERSION,
     .name = "GET_UAPI_VERSION",
     .handler = do_get_uapi_version,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_GET_SU_PATH,
     .name = "GET_SU_PATH",
     .handler = do_get_su_path,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_SET_SU_PATH,
     .name = "SET_SU_PATH",
     .handler = do_set_su_path,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_FULL_VERSION,
     .name = "GET_FULL_VERSION",
     .handler = do_get_full_version,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_HOOK_TYPE,
     .name = "GET_HOOK_TYPE",
     .handler = do_get_hook_type,
     .perm_check = manager_or_root},
#ifdef CONFIG_KSU_SUPERKEY
    {.cmd = KSU_IOCTL_SUPERKEY_AUTH,
     .name = "SUPERKEY_AUTH",
     .handler = do_superkey_auth,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_SUPERKEY_STATUS,
     .name = "SUPERKEY_STATUS",
     .handler = do_superkey_status,
     .perm_check = always_allow},
#endif // #ifdef CONFIG_KSU_SUPERKEY
    {.cmd = 0, .name = NULL, .handler = NULL, .perm_check = NULL} // Sentinel
};

void ksu_supercall_dump_commands(void)
{
	int i;

	pr_info("KernelSU IOCTL Commands:\n");
	for (i = 0; ksu_ioctl_handlers[i].handler; i++) {
		pr_info("  %-18s = 0x%08x\n", ksu_ioctl_handlers[i].name,
			ksu_ioctl_handlers[i].cmd);
	}
}

long ksu_supercall_handle_ioctl(const struct file *filp, unsigned int cmd,
				void __user *argp)
{
	int i;

#ifdef CONFIG_KSU_DEBUG
	pr_info_ratelimited("ksu ioctl: cmd=0x%x from uid=%d\n", cmd,
			    current_uid().val);
#endif // #ifdef CONFIG_KSU_DEBUG

	if (_IOC_TYPE(cmd) == SUMH_IOC_MAGIC) {
		if (!manager_or_root())
			return -EPERM;
#ifdef CONFIG_COMPAT
		if (in_compat_syscall())
			return -EOPNOTSUPP;
#endif
		return sumh_handle_ioctl(cmd, argp);
	}

	for (i = 0; ksu_ioctl_handlers[i].handler; i++) {
		if (cmd == ksu_ioctl_handlers[i].cmd) {
			if (ksu_ioctl_handlers[i].perm_check &&
			    !ksu_ioctl_handlers[i].perm_check() &&
			    !(ksu_ioctl_handlers[i].allow_su_session &&
			      ksu_is_su_session_fd(filp))) {
				pr_warn_ratelimited(
				    "ksu ioctl: permission denied for "
				    "cmd=0x%x uid=%d\n",
				    cmd, current_uid().val);
				return -EPERM;
			}

			return ksu_ioctl_handlers[i].handler(argp);
		}
	}

	pr_warn_ratelimited("ksu ioctl: unsupported command 0x%x\n", cmd);
	return -ENOTTY;
}
