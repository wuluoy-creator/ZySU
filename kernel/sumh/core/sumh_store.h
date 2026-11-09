#ifndef _SUMH_STORE_H
#define _SUMH_STORE_H

#include <linux/bitmap.h>
#include <linux/limits.h>

#include "sumh_runtime.h"

/* Immutable after publication; both indices retire through one RCU callback. */
struct sumh_spoof_kstat_entry {
	char *target_pathname;
	u32 path_hash;
	unsigned long target_ino;
	unsigned long
	    target_dev; /* Exact for path rules, including device zero. */
	unsigned long spoofed_ino;
	unsigned long spoofed_dev;
	unsigned int spoofed_nlink;
	long long spoofed_size;
	long spoofed_atime_sec;
	long spoofed_atime_nsec;
	long spoofed_mtime_sec;
	long spoofed_mtime_nsec;
	long spoofed_ctime_sec;
	long spoofed_ctime_nsec;
	unsigned long spoofed_blksize;
	unsigned long long spoofed_blocks;
	int is_static;
	u32 field_mask; /* Zero retains the legacy zero-as-unchanged contract.
			 */
	struct hlist_node path_node;
	struct hlist_node ino_node;
	struct rcu_head rcu;
};

struct sumh_managed_kstat_snapshot {
	struct rcu_head rcu;
	u32 count;
	struct sumh_spoof_kstat_entry *entries[SUMH_MANAGED_KSTAT_MAX_RULES];
};

struct sumh_maps_rule_entry {
	struct list_head list;
	bool managed;
	unsigned long target_ino;
	unsigned long target_dev;
	unsigned long spoofed_ino;
	unsigned long spoofed_dev;
	char spoofed_pathname[SUMH_MAX_LEN_PATHNAME];
};

extern struct hlist_head sumh_paths[1 << SUMH_HASH_BITS];
extern struct hlist_head sumh_hide_paths[1 << SUMH_HASH_BITS];
extern struct hlist_head sumh_inject_dirs[1 << SUMH_HASH_BITS];
extern struct hlist_head sumh_xattr_sbs[1 << SUMH_HASH_BITS];
extern struct hlist_head sumh_merge_dirs[1 << SUMH_HASH_BITS];
extern struct hlist_head sumh_spoof_kstat_path[1 << SUMH_HASH_BITS];
extern struct hlist_head sumh_spoof_kstat_ino[1 << SUMH_HASH_BITS];
extern struct sumh_managed_kstat_snapshot __rcu *sumh_managed_kstat;
extern u64 sumh_managed_kstat_generation;
extern struct list_head sumh_maps_rules;

extern struct mutex sumh_config_mutex;
extern struct mutex sumh_maps_mutex;

extern unsigned long sumh_path_bloom[BITS_TO_LONGS(SUMH_BLOOM_SIZE)];
extern unsigned long sumh_hide_bloom[BITS_TO_LONGS(SUMH_BLOOM_SIZE)];

struct sumh_spoof_kstat_entry *
sumh_spoof_kstat_lookup_by_path(const char *path_str);
struct sumh_spoof_kstat_entry *
sumh_spoof_kstat_lookup_by_ino(unsigned long ino, unsigned long dev);

void sumh_mark_inode_hidden(struct inode *inode);
void sumh_mark_dir_has_inject(const char *path_str);
void sumh_clear_inode_flags_for_path(const char *path_str, unsigned int bit);
// Publishes a captured entry and retires the previous path owner.
bool sumh_store_upsert(struct sumh_entry *entry);
void sumh_cleanup_locked(void);
void sumh_store_drain(void);

int sumh_entry_capture_source(struct sumh_entry *entry,
			      const char *source_path);
void sumh_entry_release_source(struct sumh_entry *entry);
void sumh_entry_free_rcu(struct rcu_head *head);
void sumh_hide_entry_free_rcu(struct rcu_head *head);
void sumh_inject_entry_free_rcu(struct rcu_head *head);
void sumh_xattr_sb_entry_free_rcu(struct rcu_head *head);
void sumh_merge_entry_free_rcu(struct rcu_head *head);
void sumh_spoof_kstat_entry_free_rcu(struct rcu_head *head);
void sumh_managed_kstat_snapshot_free_rcu(struct rcu_head *head);

#endif /* _SUMH_STORE_H */
