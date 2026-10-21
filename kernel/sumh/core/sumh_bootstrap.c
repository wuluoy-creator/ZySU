#include "feature/sucompat_module_guard.h"
#include "feature/sucompat.h"
#include "feature/sucompat_vfs.h"
#include "policy/feature.h"
#include <linux/rwsem.h>
#include "sumh_proc_read_hooks.h"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/version.h>

#include "sumh_bootstrap.h"
#include "sumh_config_guard.h"
#include "sumh_runtime.h"
#include "sumh_path_policy.h"
#include "sumh_store.h"
#include "sumh_fop_bridge.h"
#include "sumh_entrypoints.h"
#include "sumh_vfs_hooks.h"
#include "sumh_vfs_view.h"
#include "sumh_iop_override.h"
#include "sumh_dirhijack.h"
#include "sumh_hide_rules.h"
#include "sumh_sop_shadow.h"
#include "sumh_fop_override.h"
#include "sumh_fake_mountinfo.h"
#include "sumh_kernel_build.h"
#include "sumh_unicode_guard.h"

/* VFS callbacks outlive individual control requests; pin their host module. */
static bool sumh_unload_pin_held;

static void sumh_bootstrap_release_unload_pin(void)
{
	if (WARN_ON_ONCE(!READ_ONCE(sumh_unload_pin_held)))
		return;
	WRITE_ONCE(sumh_unload_pin_held, false);
	module_put(THIS_MODULE);
}

static noinline SUMH_NOCFI void sumh_resolve_system_dev(void)
{
	struct path sys_path = {};
	struct dentry *dentry;
	struct vfsmount *mnt;
	struct super_block *sb;
	int ret;

	ret = kern_path("/system", LOOKUP_FOLLOW, &sys_path);
	if (ret) {
		pr_warn(
		    "sumh: could not resolve /system for stat spoofing: %d\n",
		    ret);
		return;
	}

	dentry = READ_ONCE(sys_path.dentry);
	mnt = READ_ONCE(sys_path.mnt);
	sb = dentry ? READ_ONCE(dentry->d_sb) : NULL;
	if (!dentry || !mnt || !sb) {
		pr_warn("sumh: /system resolved to incomplete path (mnt=%p "
			"dentry=%p sb=%p), stat spoofing dev disabled\n",
			mnt, dentry, sb);
		if (dentry && mnt)
			path_put(&sys_path);
		return;
	}

	WRITE_ONCE(sumh_system_dev, sb->s_dev);
	pr_info("sumh: /system dev=%u:%u\n", MAJOR(sumh_system_dev),
		MINOR(sumh_system_dev));
	path_put(&sys_path);
}

static void sumh_resolve_runtime_symbols(void)
{
	sumh_notify_change = (void *)ksu_lookup_symbol("notify_change");
	sumh_vfs_getxattr_addr = (void *)ksu_lookup_symbol("vfs_getxattr");
	sumh_vfs_listxattr_addr = (void *)ksu_lookup_symbol("vfs_listxattr");
	sumh_vfs_setxattr_addr = (void *)ksu_lookup_symbol("vfs_setxattr");
	sumh_vfs_removexattr_addr =
	    (void *)ksu_lookup_symbol("vfs_removexattr");
	sumh_mnt_want_write_addr = (void *)ksu_lookup_symbol("mnt_want_write");
	sumh_mnt_drop_write_addr = (void *)ksu_lookup_symbol("mnt_drop_write");
	sumh_resolve_lookup_helpers();
	sumh_vfs_create = (void *)ksu_lookup_symbol("vfs_create");
	sumh_vfs_mkdir = (void *)ksu_lookup_symbol("vfs_mkdir");
	sumh_vfs_mknod = (void *)ksu_lookup_symbol("vfs_mknod");
	sumh_vfs_symlink = (void *)ksu_lookup_symbol("vfs_symlink");
	sumh_vfs_unlink = (void *)ksu_lookup_symbol("vfs_unlink");
	sumh_vfs_rmdir = (void *)ksu_lookup_symbol("vfs_rmdir");
	sumh_vfs_link = (void *)ksu_lookup_symbol("vfs_link");
	sumh_vfs_rename = (void *)ksu_lookup_symbol("vfs_rename");
	sumh_vfs_read = (void *)ksu_lookup_symbol("vfs_read");
	sumh_vfs_write = (void *)ksu_lookup_symbol("vfs_write");
	sumh_get_vfs_caps_from_disk =
	    (void *)ksu_lookup_symbol("get_vfs_caps_from_disk");
	sumh_security_inode_getsecctx =
	    (void *)ksu_lookup_symbol("security_inode_getsecctx");
	sumh_security_inode_notifysecctx =
	    (void *)ksu_lookup_symbol("security_inode_notifysecctx");
	sumh_security_release_secctx =
	    (void *)ksu_lookup_symbol("security_release_secctx");
	sumh_d_absolute_path = (void *)ksu_lookup_symbol("d_absolute_path");
	sumh_d_hash_and_lookup = (void *)ksu_lookup_symbol("d_hash_and_lookup");
	sumh_free_inode_nonrcu_ptr =
	    (void *)ksu_lookup_symbol("free_inode_nonrcu");
}

static int sumh_start_views(void)
{
	int ret;

	pr_info("sumh: initializing embedded VFS views\n");

	sumh_filldir_cache = kmem_cache_create(
	    "sumh_filldir", sizeof(struct sumh_filldir_wrapper), 0,
	    SLAB_HWCACHE_ALIGN, NULL);
	if (!sumh_filldir_cache) {
		pr_alert("sumh: failed to create filldir slab cache\n");
		return -ENOMEM;
	}

	sumh_resolve_runtime_symbols();

	hash_init(sumh_paths);
	hash_init(sumh_hide_paths);
	hash_init(sumh_inject_dirs);
	hash_init(sumh_xattr_sbs);
	hash_init(sumh_merge_dirs);

	sumh_resolve_system_dev();

	ret = sumh_fake_mi_init();
	if (ret)
		pr_warn("sumh: fake mountinfo unavailable: %d\n", ret);

	sumh_proc_read_hooks_init();

	(void)sumh_iop_override_init();
	ret = sumh_fop_bridge_init();
	if (ret) {
		pr_err("sumh: FATAL - old-KMI fops bridge unavailable: %d\n",
		       ret);
		goto err_fop_bridge;
	}
	(void)sumh_fop_override_init();
	(void)sumh_sop_shadow_init();
	(void)sumh_dirhijack_init();

	/* On old KMI, the first ingress table published below is the
	 * module-init commit point: bridge-open files may already pin
	 * THIS_MODULE. Keep every later initialization step non-fatal.
	 */
	__module_get(THIS_MODULE);
	WRITE_ONCE(sumh_unload_pin_held, true);
	(void)sumh_vfs_view_init();
	sumh_kernel_build_init();
	pr_info("sumh: ready; control via KSU fd\n");
	return 0;

err_fop_bridge:
	sumh_fop_bridge_exit();
	sumh_iop_override_exit();
	sumh_proc_read_hooks_exit();
	sumh_fake_mi_exit();
	if (sumh_filldir_cache) {
		kmem_cache_destroy(sumh_filldir_cache);
		sumh_filldir_cache = NULL;
	}
	return ret;
}

static void sumh_stop_views(void)
{
	struct sumh_dh_retire retired;

	pr_info("sumh: shutting down\n");
	/* Stop policy readers before withdrawing the hooks and freeing their
	 * backing state. Runtime CLEAR_ALL intentionally leaves this enabled.
	 */
	smp_store_release(&sumh_enabled, false);
	/* Direct SUMH stop must join the independent public SU setter too.
	 * The outer hook-manager exit may already have done this. */
	ksu_sucompat_sumh_exit();
	mutex_lock(&sumh_mutation_mutex);
	sumh_runtime_state_shutdown();
	sumh_hide_rules_stop();
	sumh_kernel_build_exit();
	mutex_unlock(&sumh_mutation_mutex);
	sumh_vfs_view_stop();
	sumh_vfs_view_drain();

	/*
	 * The path view is served entirely through the VFS lookup/vnode layer
	 * and the proc/mountinfo kprobes; there is no syscall dispatcher or
	 * sys_enter tracepoint to sever.  Tear down handler-reachable state
	 * directly: free the resources those VFS/proc hooks depend on (proc fd
	 * proxies, fake mountinfo, fop/iop shadows, vfs hooks).
	 */
	sumh_proc_read_hooks_exit();
	sumh_dirhijack_stop_new();
	sumh_sop_shadow_stop_new();
	sumh_dh_retire_init(&retired, true);
	mutex_lock(&sumh_config_mutex);
	sumh_cleanup_locked(&retired);
	mutex_unlock(&sumh_config_mutex);
	sumh_dh_retire_finish(&retired);
	sumh_dirhijack_exit();
	/* Row callbacks enqueue store work, which can enqueue claim callbacks.
	 * Close both stages before any physical FOP/SOP teardown or unload. */
	sumh_store_drain();
	sumh_dh_drain_releases();
	sumh_fop_override_stop_new();
	sumh_fop_bridge_stop_new();
	sumh_fop_override_exit();
	sumh_fop_bridge_exit();
	sumh_iop_override_exit();
	sumh_sop_shadow_exit();
	sumh_fake_mi_exit();
	sumh_vfs_view_exit();
	if (sumh_filldir_cache)
		kmem_cache_destroy(sumh_filldir_cache);
	pr_info("sumh: stopped\n");
	if (READ_ONCE(sumh_unload_pin_held))
		sumh_bootstrap_release_unload_pin();
}

static DECLARE_RWSEM(sumh_lifecycle_lock);
static int sumh_init_error = -EOPNOTSUPP;
static bool sumh_started;

bool sumh_is_ready(void)
{
	return smp_load_acquire(&sumh_init_error) == 0;
}

int sumh_control_begin(void)
{
	int ret;

	down_read(&sumh_lifecycle_lock);
	ret = READ_ONCE(sumh_init_error);
	if (ret)
		up_read(&sumh_lifecycle_lock);
	return ret;
}

void sumh_control_end(void)
{
	up_read(&sumh_lifecycle_lock);
}

static int sumh_bootstrap_init(void)
{
	int ret;

	down_write(&sumh_lifecycle_lock);
	if (sumh_started) {
		ret = 0;
		goto out;
	}
	ret = sumh_check_builtin_conflicts();
	if (ret)
		goto out;
	ret = ksu_sucompat_module_guard_init();
	if (ret)
		goto out;
	ret = ksu_sucompat_module_guard_acquire();
	if (ret)
		goto unguard;
	ret = ksu_sucompat_sumh_init();
	if (ret)
		goto release_guard;
	ret = sumh_start_views();
	if (!ret) {
		sumh_runtime_state_init();
		smp_store_release(&sumh_enabled, true);
		sumh_started = true;
		goto out;
	}
	ksu_sucompat_sumh_exit();
release_guard:
	ksu_sucompat_module_guard_release();
unguard:
	ksu_sucompat_module_guard_exit();
out:
	smp_store_release(&sumh_init_error, ret);
	up_write(&sumh_lifecycle_lock);
	return ret;
}

static int sumh_feature_get(u64 *value)
{
	*value = smp_load_acquire(&sumh_enabled) ? 1 : 0;
	return 0;
}

static int sumh_feature_set(u64 value)
{
	if (value > 1)
		return -EINVAL;
	if (!value)
		return -EPERM;

	/* Preserve the ABI for old callers without exposing lifecycle control.
	 */
	return smp_load_acquire(&sumh_init_error);
}

static const struct ksu_feature_handler sumh_feature = {
    .feature_id = KSU_FEATURE_SUMH,
    .name = "sumh",
    .get_handler = sumh_feature_get,
    .set_handler = sumh_feature_set,
};

void ksu_sumh_init(void)
{
	int ret;

	/* Start the provider independently of userspace feature configuration.
	 */
	ret = sumh_bootstrap_init();
	if (ret)
		pr_err("sumh: initialization failed: %d\n", ret);
	else {
		ret = ksu_sucompat_vfs_refresh();
		if (ret)
			pr_info("sumh: sucompat binding deferred until "
				"post-fs-data: %d\n",
				ret);
	}

	ret = ksu_register_feature_handler(&sumh_feature);

	if (ret)
		pr_err("sumh: feature registration failed: %d\n", ret);
	/* Independent security control; reports unsupported if view startup
	 * could not provide the shared FUSE revalidation hook.
	 */
	sumh_unicode_guard_init();
}

void ksu_sumh_post_fs_data(void)
{
	int ret;

	/* Early LKM loading may precede /system's final mount. Refresh its
	 * metadata before userspace installs any module mappings. */
	if (sumh_control_begin())
		return;
	sumh_resolve_system_dev();
	ret = ksu_sucompat_vfs_refresh();
	if (ret)
		pr_err("sumh: sucompat binding failed: %d\n", ret);
	sumh_control_end();
}

void ksu_sumh_exit(void)
{
	sumh_unicode_guard_exit();
	ksu_unregister_feature_handler(KSU_FEATURE_SUMH);
	down_write(&sumh_lifecycle_lock);
	if (!sumh_started) {
		smp_store_release(&sumh_init_error, -ESHUTDOWN);
		up_write(&sumh_lifecycle_lock);
		return;
	}
	smp_store_release(&sumh_init_error, -ESHUTDOWN);
	sumh_stop_views();
	sumh_started = false;
	ksu_sucompat_module_guard_release();
	ksu_sucompat_module_guard_exit();
	up_write(&sumh_lifecycle_lock);
}
