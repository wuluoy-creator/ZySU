#ifndef _SUMH_DIRHIJACK_H
#define _SUMH_DIRHIJACK_H

#include <linux/path.h>
#include <linux/list.h>
#include <linux/limits.h>
#include <linux/types.h>

struct inode;
struct dir_context;
struct inode_operations;
struct super_operations;

int sumh_dirhijack_init(void);
void sumh_dirhijack_stop_new(void);
void sumh_dirhijack_exit(void);

bool sumh_dirhijack_enabled(void);
/* Shared cached-FUSE lookup ingress required by the Unicode path guard. */
bool sumh_dirhijack_fuse_guard_available(void);
const struct inode_operations *
sumh_dirhijack_original_iops(const struct inode *inode);

/*
 * Virtual-inode reclaim leaf functions, invoked by the unified super_operations
 * owner (sumh_sop_shadow) from its destroy/evict/drop_inode trampolines with
 * @orig already resolved under RCU.  The trampoline owns the active-counter
 * drain, so these bodies may early-return freely.  Kept here so the
 * load-bearing sumh_vnode reclaim branches stay co-located with the rest of
 * dirhijack.
 */
void sumh_dh_reclaim_destroy_inode(struct inode *inode,
				   const struct super_operations *orig);
void sumh_dh_reclaim_evict_inode(struct inode *inode,
				 const struct super_operations *orig);
int sumh_dh_reclaim_drop_inode(struct inode *inode,
			       const struct super_operations *orig);

enum sumh_dh_owner_kind {
	SUMH_DH_OWNER_MAPPING,
	SUMH_DH_OWNER_SHADOW,
	SUMH_DH_OWNER_LEGACY_HIDE,
	SUMH_DH_OWNER_MANAGED_HIDE,
	SUMH_DH_OWNER_SU,
	SUMH_DH_OWNER_VTOPO,
};
struct sumh_dh_claim;
struct sumh_dh_spec {
	enum sumh_dh_owner_kind kind;
	const struct path *source;
	unsigned long v_ino;
	u8 flags;
};
struct sumh_dh_change {
	struct sumh_dh_claim *add, *remove;
};
struct sumh_dh_invalidation {
	struct dentry *parent;
	char name[NAME_MAX + 1];
};
struct sumh_dh_retire {
	struct list_head slots, dirs, dops, invalidations;
	struct sumh_dh_invalidation single[2];
	unsigned int count;
	bool bulk;
};
void sumh_dh_retire_init(struct sumh_dh_retire *retired, bool bulk);
/* Preparation acquires the observed parent inode lock before DH. Callers
 * must hold neither that inode lock nor config/DH; producer mutation/SU
 * locks are allowed. No inode lock survives the preparation call, and all
 * callback drains/final reference drops occur after inode/DH unlock.
 * Returned parent has an independent path ref. Only MAPPING permits an
 * absent immediate parent / virtual-top dependency. */
int sumh_dh_prepare_path(const char *visible, const struct sumh_dh_spec *spec,
			 struct sumh_dh_claim **out,
			 struct path *native_parent);
int sumh_dh_prepare_parent(const struct path *parent, const char *name,
			   const struct sumh_dh_spec *spec,
			   struct sumh_dh_claim **out);
/* Abort consumes an unpublished PREPARED or INVALIDATED claim. Put consumes
 * a previously detached published claim; neither frees an attached owner. */
void sumh_dh_abort(struct sumh_dh_claim *claim);
void sumh_dh_claim_put(struct sumh_dh_claim *claim);
bool sumh_dh_claim_matches_parent(const struct sumh_dh_claim *claim,
				  const struct path *parent);
/* Begin holds DH on success. All ordinary failures precede commit. */
int sumh_dh_change_begin(struct sumh_dh_claim *add,
			 struct sumh_dh_claim *remove,
			 struct sumh_dh_change *change);
void sumh_dh_change_commit(struct sumh_dh_change *change,
			   struct sumh_dh_retire *retired);
void sumh_dh_change_end(void);
/* Detach acquires DH internally, may be called under config, never waits for
 * callbacks. Finish and all physical release must run outside config/DH. */
void sumh_dh_detach(struct sumh_dh_claim *claim,
		    struct sumh_dh_retire *retired);
void sumh_dh_retire_finish(struct sumh_dh_retire *retired);
void sumh_dh_drain_releases(void);
int sumh_dirhijack_match_su(const char *visible,
			    const struct sumh_dh_claim *claim,
			    unsigned long v_ino);

struct sumh_hide_binding;
int sumh_dirhijack_hide_get(const struct path *parent, const char *name,
			    struct sumh_hide_binding **result);
void sumh_dirhijack_hide_put(struct sumh_hide_binding *binding);
bool sumh_dirhijack_hide_matches(const struct sumh_hide_binding *binding,
				 const struct path *parent);
bool sumh_dirhijack_hidden(struct inode *parent, const char *name, int len);
/* Whether the specialized iterator emits this name for the current task. */
bool sumh_dirhijack_emits(struct inode *parent, const char *name, int len);
bool sumh_dirhijack_injected_context(const struct dir_context *ctx);

/*
 * Withdraw non-SU owners and invalidate unpublished reservations. Runtime
 * CLEAR preserves active SU; stop withdraws everything. Caller-owned
 * invalidated reservations remain safely abortable. May sleep; no global
 * producer drain.
 */
void sumh_dirhijack_clear(void);

#endif /* _SUMH_DIRHIJACK_H */
