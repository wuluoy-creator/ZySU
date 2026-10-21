#ifndef _SUMH_OVERLAY_H
#define _SUMH_OVERLAY_H

#include <linux/fs.h>
#include <linux/list.h>

/* Success returns one held directory path; failure leaves out zeroed. */
int sumh_capture_merge_target(const char *target, struct path *out);
/* Bounded enumeration precedes child work. Non-link children are relative to
 * held parents. Final links retain native pathname following and its
 * non-atomic pair; this is not an atomic source-tree view. The caller must
 * hold mutation serialization and have published the prefix MERGE owner
 * before materializing its children (also true for recursive callers). */
int sumh_materialize_merge(const char *src_prefix, const char *target_dir,
			   const struct path *target_path, int depth);
bool sumh_is_merge_context(const struct dir_context *ctx);
bool sumh_emit_projected_dirent(struct dir_context *ctx, const char *name,
				int namlen, u64 ino, unsigned int type);
struct sumh_merge_snapshot;
int sumh_merge_snapshot_create(struct file *source, const char *primary,
			       struct sumh_merge_snapshot **out);
/* Borrowed names remain owned by the snapshot until destroy. */
struct list_head *sumh_merge_snapshot_names(struct sumh_merge_snapshot *s);
bool sumh_merge_snapshot_covers(const struct sumh_merge_snapshot *s,
				const char *name, unsigned int len);
void sumh_merge_snapshot_destroy(struct sumh_merge_snapshot *s);

#endif /* _SUMH_OVERLAY_H */
