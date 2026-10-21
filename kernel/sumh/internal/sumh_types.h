#ifndef _SUMH_TYPES_H
#define _SUMH_TYPES_H

#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/list.h>
#include <linux/llist.h>
#include <linux/mutex.h>
#include <linux/path.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>
#include <linux/workqueue.h>

#include "sumh_base.h"

struct sumh_inject_claim;
struct sumh_dh_claim;

struct sumh_entry {
	char *src;
	char *target;
	char *source_canonical;
	/* Pinned data source resolved from target when the rule is installed.
	 */
	struct path source_path;
	struct path source_nofollow_path;
	struct inode *source_inode;
	struct kstat source_stat;
	struct kstat source_nofollow_stat;
	struct kstat visible_stat;
	unsigned long source_ino;
	unsigned long source_dev;
	unsigned long visible_ino;
	unsigned long visible_dev;
	unsigned long nofollow_visible_ino;
	unsigned long nofollow_visible_dev;
	umode_t source_mode;
	umode_t source_nofollow_mode;
	kuid_t source_uid;
	kgid_t source_gid;
	loff_t source_size;
	bool source_stat_valid;
	bool visible_stat_valid;
	/* A real node existed at src when the rule was installed.  Its DAC and
	 * timestamp metadata remains the visible template while source-backed
	 * type/size/allocation fields are refreshed dynamically. */
	bool preserve_visible_metadata;
	bool source_path_valid;
	bool source_nofollow_stat_valid;
	bool source_nofollow_path_valid;
	unsigned char type;
	u32 src_hash;
	/* Owned injection projection; detached synchronously before retirement.
	 */
	struct sumh_inject_claim *inject_claim;
	struct sumh_dh_claim *dh_claim;
	struct hlist_node node;
	struct rcu_head rcu;
	struct llist_node free_node;
};

struct sumh_hide_entry {
	char *path;
	u32 path_hash;
	bool storage_managed;
	struct sumh_dh_claim *dh_claim;
	struct hlist_node node;
	struct rcu_head rcu;
	struct llist_node free_node;
};

struct sumh_inject_entry {
	char *dir;
	unsigned int users; /* Writers hold sumh_config_mutex. */
	struct hlist_node node;
	struct rcu_head rcu;
};

struct sumh_xattr_sb_entry {
	struct super_block *sb;
	struct work_struct free_work;
	struct hlist_node node;
	struct rcu_head rcu;
};

struct sumh_merge_entry {
	char *src;
	char *target;
	char *resolved_src;
	struct sumh_inject_claim *inject_claims[2];
	/* Captured directory identity; owns both mount and dentry until the
	 * sleepable retirement worker releases it after RCU readers finish. */
	struct path target_path;
	struct hlist_node node;
	struct rcu_head rcu;
	struct llist_node free_node;
};

struct sumh_name_list {
	char *name;
	u64 ino;
	unsigned int name_len;
	unsigned char type;
	struct list_head list;
	struct rb_node name_node;
};

struct sumh_merge_snapshot;

struct sumh_filldir_wrapper {
	struct dir_context wrap_ctx;
	struct dir_context *orig_ctx;
	struct dentry *parent_dentry;
	int dir_path_len;
	bool dir_has_hidden;
	char *dir_path;
	bool dir_has_inject;
	bool inject_done;
	bool inject_pending;
	bool has_iterate_client;
	bool stopped;
	loff_t inject_pos;
	unsigned int emitted;
	bool view_allowed;
	bool spoof_allowed;
	struct sumh_merge_snapshot *merge_snapshot;
};

/*
 * get_vfs_caps_from_disk kretprobe state.  The exec-path file-capability read
 * lands on the visible vnode's dentry, whose synthetic inode carries no on-disk
 * security.capability.  We stash the source's parsed caps at vnode-create time
 * (sleepable) and replay them here (atomic): @have gates the replay, @caps is
 * the pre-parsed value, @out points at the caller's cpu_vfs_cap_data.
 */
struct sumh_fscap_ri_data {
	bool have;
	struct cpu_vfs_cap_data *out;
	struct cpu_vfs_cap_data caps;
};

#endif /* _SUMH_TYPES_H */
