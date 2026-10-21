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
	struct hlist_node path_node;
	struct hlist_node ino_node;
	struct rcu_head rcu;
};

extern struct hlist_head sumh_paths[1 << SUMH_HASH_BITS];
extern struct hlist_head sumh_hide_paths[1 << SUMH_HASH_BITS];
extern struct hlist_head sumh_inject_dirs[1 << SUMH_HASH_BITS];
extern struct hlist_head sumh_xattr_sbs[1 << SUMH_HASH_BITS];
extern struct hlist_head sumh_merge_dirs[1 << SUMH_HASH_BITS];
extern struct hlist_head sumh_spoof_kstat_path[1 << SUMH_HASH_BITS];
extern struct hlist_head sumh_spoof_kstat_ino[1 << SUMH_HASH_BITS];

extern struct mutex sumh_config_mutex;

extern unsigned long sumh_path_bloom[BITS_TO_LONGS(SUMH_BLOOM_SIZE)];
extern unsigned long sumh_hide_bloom[BITS_TO_LONGS(SUMH_BLOOM_SIZE)];

struct sumh_spoof_kstat_entry *
sumh_spoof_kstat_lookup_by_path(const char *path_str);
struct sumh_spoof_kstat_entry *
sumh_spoof_kstat_lookup_by_ino(unsigned long ino, unsigned long dev);

void sumh_mark_inode_hidden(struct inode *inode);
void sumh_clear_inode_flags_for_path(const char *path_str, unsigned int bit);
enum sumh_inject_missing {
	SUMH_INJECT_MISSING_NONE,
	SUMH_INJECT_MISSING_UNBOUND,
};
/* Prepare under mutation serialization, outside config. Only ENOENT uses
 * the missing policy; bound claims pin exact native inode and active sb. */
int sumh_inject_prepare_directory(const char *key,
				  enum sumh_inject_missing missing,
				  struct sumh_inject_claim **out);
int sumh_inject_prepare_observed(const char *key, const struct path *parent,
				 enum sumh_inject_missing missing,
				 struct sumh_inject_claim **out);
int sumh_inject_prepare_merge(const char *key, char **resolved_src,
			      struct sumh_inject_claim *claims[2]);
/* Attach/detach/key lookup require config; max two claims attach as a group. */
struct sumh_inject_inode;
struct sumh_inject_attach_plan {
	struct sumh_inject_claim *claims[2];
	struct sumh_inject_entry *keys[2];
	struct sumh_inject_inode *bindings[2];
	unsigned int count;
};
int sumh_inject_preflight_locked(struct sumh_inject_claim *claims[],
				 unsigned int count, unsigned int extra_rows,
				 struct sumh_inject_attach_plan *plan);
void sumh_inject_commit_locked(struct sumh_inject_attach_plan *plan);
int sumh_inject_attach_locked(struct sumh_inject_claim *claims[],
			      unsigned int count);
void sumh_inject_detach_locked(struct sumh_inject_claim *claim);
bool sumh_inject_key_present_locked(const char *key);
/* Sleepable disposal outside config. Destroy never detaches a live claim. */
void sumh_inject_finish_prepare(struct sumh_inject_claim *claim);
void sumh_inject_destroy(struct sumh_inject_claim *claim);
/* On success the store owns entry; on error caller retains it. Optional
 * changed reports target spelling/captured follow/nofollow identity changes,
 * independently of the replacement's unconditional injection ownership. */
int sumh_store_upsert(struct sumh_entry *entry, bool *changed);
struct sumh_dh_retire;
void sumh_cleanup_locked(struct sumh_dh_retire *retired);
void sumh_store_drain(void);

int sumh_entry_capture_source(struct sumh_entry *entry,
			      const char *source_path);
/* Borrow a held non-link source and acquire the entry's own reference.
 * Symlink inputs return ELOOP; no source pathname is resolved here. */
int sumh_entry_capture_resolved(struct sumh_entry *entry,
				const struct path *source);
/* Native nofollow lookup must match expected_leaf when supplied (ESTALE on
 * mismatch). Non-links are captured once; links retain native following and
 * one native nofollow recheck before capture. A changed mount+dentry returns
 * ESTALE; a failed recheck propagates its errno, without a SUMH retry. This
 * detects only changes still visible at the recheck, not ABA, dynamic links,
 * target-chain or later changes; the pair is not atomic. Borrowed
 * expected_leaf is never consumed. On error no new source references are
 * acquired; an already initialized entry remains unchanged. */
int sumh_entry_capture_source_checked(struct sumh_entry *entry,
				      const char *source_path,
				      const struct path *expected_leaf);
void sumh_entry_release_source(struct sumh_entry *entry);
void sumh_entry_free_rcu(struct rcu_head *head);
void sumh_hide_entry_free_rcu(struct rcu_head *head);
void sumh_inject_entry_free_rcu(struct rcu_head *head);
void sumh_xattr_sb_entry_free_rcu(struct rcu_head *head);
void sumh_merge_entry_free_rcu(struct rcu_head *head);
void sumh_spoof_kstat_entry_free_rcu(struct rcu_head *head);

#endif /* _SUMH_STORE_H */
