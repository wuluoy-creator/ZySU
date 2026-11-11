#ifndef KSU_MOUNT_POLICY_H
#define KSU_MOUNT_POLICY_H

#include <linux/types.h>

struct file;
struct poll_table_struct;
struct mount_entry;

struct ksu_mount_field {
	const char *data;
	size_t len;
};

struct ksu_mount_fields {
	struct ksu_mount_field dev, root, target, fstype, source, super;
	bool escaped;
};

bool ksu_mount_is_module(const struct ksu_mount_fields *fields);
bool ksu_mount_unescape(char *value);
void ksu_mount_policy_reset(void);
void ksu_free_mount_entry(struct mount_entry *entry);
void ksu_retire_mount_entry(struct mount_entry *entry);
void ksu_mount_policy_begin_update(void);
int ksu_register_managed_mount(const char *mountpoint);
int ksu_unregister_managed_mount(const char *mountpoint);
u64 ksu_mount_policy_generation(void);
void ksu_mount_policy_changed(void);
void ksu_mount_policy_poll(struct file *file, struct poll_table_struct *wait);

#endif
