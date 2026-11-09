#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/version.h>

#include "core/imgpatch_config.h"
#include "klog.h" // IWYU pragma: keep
#include "ksu.h"
#include "policy/allowlist.h"
#include "preinit/preinit.h"
#include "policy/feature.h"
#include "feature/module_blacklist.h"
#include "uapi/imgpatch_config.h"

static_assert(sizeof(struct ksu_imgpatch_config) == 512,
	      "ImgPatch config ABI drift");
static_assert(offsetof(struct ksu_imgpatch_config, force_features) == 24,
	      "ImgPatch reserved config ABI drift");

static volatile struct ksu_imgpatch_config
    __attribute__((used, section(".data"))) imgpatch_config_store = {
	.magic = KSU_IMGPATCH_CONFIG_MAGIC,
	.version = KSU_IMGPATCH_CONFIG_VERSION,
	.size = sizeof(struct ksu_imgpatch_config),
};

static int write_initramfs_file(const char *path, const char *content,
				size_t content_size)
{
	struct file *file;
	loff_t offset = 0;
	ssize_t written;
	int ret = 0;

	file = filp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(file))
		return PTR_ERR(file);

	if (content_size) {
		written = kernel_write(file, content, content_size, &offset);
		if (written < 0)
			ret = written;
		else if ((size_t)written != content_size)
			ret = -EIO;
	}
	filp_close(file, NULL);
	return ret;
}

static int remove_initramfs_file(const char *name)
{
	struct dentry *dentry;
	struct file *root;
	struct inode *dir;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
	struct qstr qname = QSTR_LEN((const unsigned char *)name, strlen(name));
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
	int ret;

	root = filp_open("/", O_RDONLY | O_DIRECTORY, 0);
	if (IS_ERR(root))
		return PTR_ERR(root);

	ret = mnt_want_write(root->f_path.mnt);
	if (ret)
		goto out_close;

	dir = file_inode(root);
	inode_lock_nested(dir, I_MUTEX_PARENT);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
	dentry = lookup_one(mnt_idmap(root->f_path.mnt), &qname,
			    root->f_path.dentry);
#else
	dentry = lookup_one_len(name, root->f_path.dentry, strlen(name));
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
	if (IS_ERR(dentry)) {
		ret = PTR_ERR(dentry);
		goto out_unlock;
	}

	if (!d_inode(dentry)) {
		ret = 0;
	} else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
		ret =
		    vfs_unlink(mnt_idmap(root->f_path.mnt), dir, dentry, NULL);
#else
		ret = vfs_unlink(mnt_user_ns(root->f_path.mnt), dir, dentry,
				 NULL);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
	}
	dput(dentry);

out_unlock:
	inode_unlock(dir);
	mnt_drop_write(root->f_path.mnt);
out_close:
	filp_close(root, NULL);
	return ret;
}

static int configure_adbd_from_imgpatch(bool enable)
{
	static const char adb_debug_props[] = "ro.debuggable=1\n"
					      "ro.force.debuggable=1\n"
					      "ro.adb.secure=0\n";
	int ret;

	if (enable) {
		/* Android init only consumes adb_debug.prop with this marker.
		 */
		ret = write_initramfs_file("/adb_debug.prop", adb_debug_props,
					   sizeof(adb_debug_props) - 1);
		if (ret)
			return ret;
		return write_initramfs_file("/force_debuggable", NULL, 0);
	}

	/*
	 * A direct-patched LKM may accidentally be loaded by a ramdisk-patched
	 * image. Empty the external properties before removing their marker so
	 * an internal "disabled" value wins as decisively as an enabled value.
	 */
	ret = write_initramfs_file("/adb_debug.prop", NULL, 0);
	if (ret)
		return ret;
	ret = remove_initramfs_file("force_debuggable");
	if (ret)
		return ret;
	return remove_initramfs_file("adb_debug.prop");
}

int ksu_imgpatch_config_apply(void)
{
	struct ksu_imgpatch_config config;
	int ret;

	config = imgpatch_config_store;
	if (config.magic != KSU_IMGPATCH_CONFIG_MAGIC ||
	    (config.version != 2 && config.version != 3 &&
	     config.version != KSU_IMGPATCH_CONFIG_VERSION) ||
	    config.size != sizeof(config)) {
		pr_err("imgpatch config header is invalid\n");
		return -EINVAL;
	}
	if (config.flags & ~KSU_IMGPATCH_CONFIG_VALID_FLAGS) {
		pr_err("imgpatch config flags are invalid: 0x%llx\n",
		       config.flags);
		return -EINVAL;
	}

	if (config.version >= 3) {
		if (!memchr(config.force_features, '\0',
			    sizeof(config.force_features)) ||
		    !memchr(config.block_modules, '\0',
			    sizeof(config.block_modules)))
			return -EINVAL;
		ksu_feature_parse_forced(config.force_features);
		ksu_module_blacklist_parse(config.block_modules);
	}
	if (config.version >= 4)
		ksu_preinit_enabled =
		    !(config.flags & KSU_IMGPATCH_CONFIG_NO_PREINIT);
	ksu_no_custom_rc = !!(config.flags & KSU_IMGPATCH_CONFIG_NO_CUSTOM_RC);
	ksu_bundled = !!(config.flags & KSU_IMGPATCH_CONFIG_BUNDLED);
	allow_shell = !!(config.flags & KSU_IMGPATCH_CONFIG_ALLOW_SHELL);
	ret = configure_adbd_from_imgpatch(
	    !!(config.flags & KSU_IMGPATCH_CONFIG_ENABLE_ADBD));
	if (ret) {
		pr_err("imgpatch ADB debug setup failed: %d\n", ret);
		return ret;
	}

	pr_info("imgpatch config applied: allow_shell=%d enable_adbd=%d\n",
		allow_shell,
		!!(config.flags & KSU_IMGPATCH_CONFIG_ENABLE_ADBD));
	memzero_explicit(&config, sizeof(config));
	return 0;
}
