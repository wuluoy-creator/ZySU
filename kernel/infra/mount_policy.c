#include <linux/rculist.h>
#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/limits.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <uapi/linux/mount.h>
#include "feature/kernel_umount.h"
#include "infra/mount_policy.h"

struct list_head mount_list = LIST_HEAD_INIT(mount_list);
DECLARE_RWSEM(mount_list_lock);

static atomic64_t mount_policy_generation = ATOMIC64_INIT(2);
static DECLARE_WAIT_QUEUE_HEAD(mount_policy_wait);

static unsigned char field_char(struct ksu_mount_field field, size_t *pos,
				bool escaped)
{
	unsigned char c = field.data[(*pos)++];

	if (escaped && c == '\\' && *pos + 2 < field.len &&
	    field.data[*pos] >= '0' && field.data[*pos] <= '3' &&
	    field.data[*pos + 1] >= '0' && field.data[*pos + 1] <= '7' &&
	    field.data[*pos + 2] >= '0' && field.data[*pos + 2] <= '7') {
		c = ((field.data[*pos] - '0') << 6) |
		    ((field.data[*pos + 1] - '0') << 3) |
		    (field.data[*pos + 2] - '0');
		*pos += 3;
	}
	return c;
}

static bool field_matches(struct ksu_mount_field field, const char *text,
			  bool escaped, bool prefix)
{
	size_t pos = 0;

	if (!field.data || !text)
		return false;
	while (*text) {
		if (pos >= field.len ||
		    field_char(field, &pos, escaped) != (unsigned char)*text++)
			return false;
	}
	return pos == field.len ||
	       (prefix && field_char(field, &pos, escaped) == '/');
}

static bool private_path(struct ksu_mount_field field, bool escaped)
{
	return field_matches(field, "/adb", escaped, true) ||
	       field_matches(field, "/data/adb", escaped, true);
}

static bool private_options(struct ksu_mount_field field, bool escaped)
{
	size_t pos = 0;
	bool boundary = true;

	while (field.data && pos < field.len) {
		size_t start = pos;
		unsigned char c = field_char(field, &pos, escaped);

		if (c == '/' && boundary) {
			size_t end = pos;
			while (end < field.len && field.data[end] != ',' &&
			       field.data[end] != ':')
				end++;
			if (private_path(
				(struct ksu_mount_field){field.data + start,
							 end - start},
				escaped))
				return true;
		}
		boundary = !(escaped && pos - start == 4) &&
			   (c == '=' || c == ':' || c == ',');
	}
	return false;
}

bool ksu_mount_is_module(const struct ksu_mount_fields *fields)
{
	struct mount_entry *entry;
	bool found = false;
	char dev[32];

	if (private_path(fields->root, fields->escaped) ||
	    private_path(fields->target, fields->escaped) ||
	    private_path(fields->source, fields->escaped) ||
	    field_matches(fields->source, "KSU", fields->escaped, false) ||
	    field_matches(fields->source, "magisk", fields->escaped, false) ||
	    field_matches(fields->source, "APatch", fields->escaped, false) ||
	    private_options(fields->super, fields->escaped))
		return true;

	rcu_read_lock();
	list_for_each_entry_rcu(entry, &mount_list, list)
	{
		if (!entry->mount_root || !entry->mount_fstype)
			continue;
		scnprintf(dev, sizeof(dev), "%u:%u", MAJOR(entry->mount_dev),
			  MINOR(entry->mount_dev));
		if (field_matches(fields->dev, dev, false, false) &&
		    field_matches(fields->target, entry->umountable,
				  fields->escaped, false) &&
		    field_matches(fields->root, entry->mount_root,
				  fields->escaped, false) &&
		    field_matches(fields->fstype, entry->mount_fstype,
				  fields->escaped, false)) {
			found = true;
			break;
		}
	}
	rcu_read_unlock();
	return found;
}

static int ksu_capture_mount_identity(struct mount_entry *entry)
{
	struct path path;
	struct super_block *sb;
	char *buffer, *root;
	int ret;

	ret = kern_path(entry->umountable, LOOKUP_FOLLOW, &path);
	if (ret)
		return ret;
	sb = path.mnt->mnt_sb;
	ret = -EINVAL;
	if (path.dentry != path.mnt->mnt_root || !sb || !sb->s_type ||
	    !sb->s_type->name)
		goto out;
	buffer = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!buffer) {
		ret = -ENOMEM;
		goto out;
	}
	root = dentry_path_raw(path.mnt->mnt_root, buffer, PATH_MAX);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
	} else {
		entry->mount_root = kstrdup(root, GFP_KERNEL);
		entry->mount_fstype = kstrdup(sb->s_type->name, GFP_KERNEL);
		entry->mount_dev = sb->s_dev;
		ret = entry->mount_root && entry->mount_fstype ? 0 : -ENOMEM;
	}
	kfree(buffer);
out:
	path_put(&path);
	return ret;
}

u64 ksu_mount_policy_generation(void)
{
	u64 generation;

	smp_rmb();
	generation = atomic64_read(&mount_policy_generation);
	smp_rmb();
	return generation;
}

void ksu_mount_policy_begin_update(void)
{
	atomic64_inc(&mount_policy_generation);
	smp_wmb();
}

void ksu_mount_policy_changed(void)
{
	smp_wmb();
	atomic64_inc(&mount_policy_generation);
	wake_up_all(&mount_policy_wait);
}

void ksu_mount_policy_poll(struct file *file, struct poll_table_struct *wait)
{
	poll_wait(file, &mount_policy_wait, wait);
}

bool ksu_mount_unescape(char *value)
{
	struct ksu_mount_field field = {value, strlen(value)};
	size_t pos = 0, out = 0;

	while (pos < field.len) {
		size_t start = pos;
		unsigned char c = field_char(field, &pos, true);

		if (!c)
			return false;
		if (c == '\\' && pos - start == 1 && start + 3 < field.len &&
		    field.data[start + 1] >= '0' &&
		    field.data[start + 1] <= '7' &&
		    field.data[start + 2] >= '0' &&
		    field.data[start + 2] <= '7' &&
		    field.data[start + 3] >= '0' &&
		    field.data[start + 3] <= '7')
			return false;
		value[out++] = c;
	}
	value[out] = '\0';
	return true;
}

void ksu_mount_policy_reset(void)
{
	struct mount_entry *entry, *tmp;

	down_write(&mount_list_lock);
	ksu_mount_policy_begin_update();
	list_for_each_entry_safe (entry, tmp, &mount_list, list) {
		list_del_rcu(&entry->list);
		ksu_retire_mount_entry(entry);
	}
	ksu_mount_policy_changed();
	up_write(&mount_list_lock);
}

void ksu_free_mount_entry(struct mount_entry *entry)
{
	kfree(entry->mount_root);
	kfree(entry->mount_fstype);
	kfree(entry->umountable);
	kfree(entry);
}

static void ksu_mount_entry_release(struct rcu_head *rcu)
{
	ksu_free_mount_entry(container_of(rcu, struct mount_entry, rcu));
}

void ksu_retire_mount_entry(struct mount_entry *entry)
{
	call_rcu(&entry->rcu, ksu_mount_entry_release);
}

int ksu_register_managed_mount(const char *mountpoint)
{
	struct mount_entry *entry, *new_entry;
	int ret;

	new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
	if (!new_entry)
		return -ENOMEM;
	new_entry->umountable = kstrdup(mountpoint, GFP_KERNEL);
	if (!new_entry->umountable) {
		ksu_free_mount_entry(new_entry);
		return -ENOMEM;
	}
	new_entry->flags = MNT_DETACH;
	/* Publish only complete live mount identities. SUMHP rolls the mount
	 * back if capture fails; a failed refresh preserves the previous entry.
	 */
	ret = ksu_capture_mount_identity(new_entry);
	if (ret) {
		ksu_free_mount_entry(new_entry);
		return ret;
	}

	down_write(&mount_list_lock);
	ksu_mount_policy_begin_update();
	list_for_each_entry (entry, &mount_list, list) {
		if (!strcmp(entry->umountable, mountpoint)) {
			list_replace_rcu(&entry->list, &new_entry->list);
			ksu_retire_mount_entry(entry);
			goto changed;
		}
	}
	list_add_rcu(&new_entry->list, &mount_list);
changed:
	ksu_mount_policy_changed();
	up_write(&mount_list_lock);
	return 0;
}

int ksu_unregister_managed_mount(const char *mountpoint)
{
	struct mount_entry *entry;

	down_write(&mount_list_lock);
	list_for_each_entry (entry, &mount_list, list) {
		if (!strcmp(entry->umountable, mountpoint)) {
			ksu_mount_policy_begin_update();
			list_del_rcu(&entry->list);
			ksu_retire_mount_entry(entry);
			ksu_mount_policy_changed();
			break;
		}
	}
	up_write(&mount_list_lock);
	return 0;
}
