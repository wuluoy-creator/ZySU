#ifndef _SUMH_FAKE_MOUNTINFO_H
#define _SUMH_FAKE_MOUNTINFO_H

#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/refcount.h>

struct sumh_mi_snapshot {
	refcount_t refs;
	char *data;
	size_t data_capacity;
	size_t len;
	char *mounts;
	size_t mounts_capacity;
	size_t mounts_len;
};

int sumh_fake_mi_init(void);
void sumh_fake_mi_exit(void);
bool sumh_fake_mi_active(void);
bool sumh_fake_mi_native_view(struct file *file);
int sumh_mi_normalize_groups(char *data, size_t *len);
/* Each snapshot belongs to the opened proc file's namespace and root. */
int sumh_fake_mi_get_snapshot(struct file *file,
			      const struct file_operations *ops,
			      struct sumh_mi_snapshot **out);
void sumh_fake_mi_put_snapshot(struct sumh_mi_snapshot *snapshot);
void sumh_fake_mi_invalidate_all(void);
u64 sumh_fake_mi_generation(void);
void sumh_fake_mi_poll_wait(struct file *file, struct poll_table_struct *wait);

#endif
