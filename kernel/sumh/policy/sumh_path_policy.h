#ifndef _SUMH_PATH_POLICY_H
#define _SUMH_PATH_POLICY_H

#include <linux/list.h>
#include <linux/path.h>
#include <linux/stat.h>
#include <linux/types.h>
#include <uapi/linux/magic.h>

struct sumh_entry;
struct inode;

struct sumh_rule_source {
	struct path path;
	struct kstat stat;
	umode_t source_mode;
	unsigned long visible_ino;
	unsigned long visible_dev;
	bool stat_valid;
	bool preserve_visible_metadata;
	int error;
};

void sumh_project_visible_stat(struct kstat *result,
			       const struct kstat *source_stat,
			       const struct kstat *visible_template,
			       bool preserve_visible_metadata,
			       unsigned long visible_ino,
			       unsigned long visible_dev);

enum sumh_policy_scope {
	SUMH_POLICY_SCOPE_NONE = 0,
	SUMH_POLICY_SCOPE_VIEW,
	SUMH_POLICY_SCOPE_SPOOF,
};

bool sumh_is_privileged_process(void);
bool sumh_policy_current_is_isolated(void);
enum sumh_policy_scope sumh_policy_current_scope(void);
bool sumh_policy_current_is_view_target(void);
bool sumh_hide_storage_parent(const struct inode *parent);
bool sumh_policy_current_is_hide_target(const struct inode *parent);
bool sumh_policy_current_is_spoof_target(void);
bool sumh_policy_current_is_mount_view_target(void);
bool sumh_policy_uid_is_spoof_target(uid_t uid);
bool sumh_rule_get_source_flags(const char *pathname, unsigned int lookup_flags,
				struct sumh_rule_source *source);
bool sumh_should_hide(const char *pathname);

/*
 * Slice 4c-v2 pure-virtual directory topology: resolve names under a synthetic
 * (source-less) directory against the rule table.  A F_VIRTUAL_DIR vnode uses
 * these to decide whether a child name is an exact redirect (a leaf) or the
 * prefix of one (a deeper virtual dir), and to enumerate its children.
 */
enum sumh_vpath_kind {
	SUMH_VPATH_NONE = 0, /* no rule under dir/child */
	SUMH_VPATH_LEAF, /* dir/child is an exact rule -> a real redirect */
	SUMH_VPATH_VDIR, /* dir/child is a prefix of some rule -> virtual dir
			  */
};

/*
 * Resolve @child within virtual directory @dir.  On SUMH_VPATH_LEAF pins
 * *leaf_src (caller path_put) and fills *leaf_mode / *leaf_ino with the
 * leaf's source identity (the nofollow link for a symlink target). Non-sleeping
 * apart from a GFP_KERNEL scratch alloc; the rule-table scan runs under RCU.
 * Returns a sumh_vpath_kind or a negative errno; allocation failure must not
 * be cached as a negative dentry.
 */
int sumh_rule_vpath_child(const char *dir, const char *child,
			  struct path *leaf_src, umode_t *leaf_mode,
			  unsigned long *leaf_ino);

/*
 * Collect the direct children of virtual directory @dir into @out as
 * struct sumh_name_list nodes (name/ino/type), deduplicated.  Caller emits
 * and frees each node (kfree(name) then kfree(node)). Returns the child count
 * or a negative errno. On error @out can contain a partial list: it must be
 * freed without publishing any entries or advancing a directory cursor.
 */
int sumh_rule_vpath_emit(const char *dir, struct list_head *out);

#endif /* _SUMH_PATH_POLICY_H */
